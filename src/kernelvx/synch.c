/* kernelvx - synchronization functions added in Windows 7 and 8. */
#include "../common/vx.h"

/* ---- WaitOnAddress / WakeByAddress (Windows 8) ---------------------------
 * Rust's standard library builds every Mutex, Condvar, Once and thread park
 * on these (Firefox has a lot of Rust), so they must be fast and must wake
 * only whom they are asked to wake.
 *
 * Each waiter puts a small record on its own stack into the list of a hashed
 * bucket and sleeps on the record's own condition variable. A waker takes
 * the bucket's lock, takes matching records off the list (one for
 * WakeByAddressSingle, all for WakeByAddressAll) and wakes exactly those.
 * The waiter compares the value under the same lock, so a wake cannot slip
 * in between the check and the sleep. A waker that finds the bucket empty
 * returns without taking the lock: the waiter counts itself in before it
 * reads the value, and both sides have a full memory barrier in between, so
 * either the waker sees the waiter or the waiter sees the new value. */
typedef struct vx_waiter {
    struct vx_waiter *next, *prev;
    volatile void *addr;
    CONDITION_VARIABLE cv;
    BOOL woken;
} vx_waiter;

#define NBUCKETS 256
static struct DECLSPEC_ALIGN(64) {
    SRWLOCK lock;
    vx_waiter *head, *tail;
    volatile LONG count;
} g_wb[NBUCKETS];                                   /* zero = initialized */

static unsigned bucket(volatile void *a)
{
    ULONG_PTR x = (ULONG_PTR)a >> 2;
    x ^= x >> 9;
    return (unsigned)((x * 0x9E3779B1u) >> 8) % NBUCKETS;
}

static void unlink_waiter(unsigned b, vx_waiter *w)
{
    if (w->prev) w->prev->next = w->next; else g_wb[b].head = w->next;
    if (w->next) w->next->prev = w->prev; else g_wb[b].tail = w->prev;
    InterlockedDecrement(&g_wb[b].count);
}

static BOOL same(volatile const void *a, const void *b, SIZE_T n)
{
    switch (n) {
    case 1: return *(volatile const BYTE *)a == *(const BYTE *)b;
    case 2: return *(volatile const WORD *)a == *(const WORD *)b;
    case 4: return *(volatile const DWORD *)a == *(const DWORD *)b;
    default: return *(volatile const ULONGLONG *)a == *(const ULONGLONG *)b;
    }
}

/* how long to look before sleeping: not at all on one processor (the other
 * thread cannot run meanwhile) */
static unsigned spin_count(void)
{
    static volatile LONG n = -1;
    if (n < 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        InterlockedExchange(&n, si.dwNumberOfProcessors > 1 ? 128 : 0);
    }
    return (unsigned)n;
}

VX_EXPORT BOOL WINAPI vx_WaitOnAddress(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms)
{ VX_NATIVE(K32, WaitOnAddress, BOOL, (volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms), (addr, cmp, size, ms));
    unsigned b;
    vx_waiter w;
    BOOL ok = TRUE;
    if (!addr || !cmp || (size != 1 && size != 2 && size != 4 && size != 8)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!same(addr, cmp, size)) return TRUE;         /* already changed: no lock needed */
    if (!ms) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    /* With more than one processor, the value often changes within a few
     * hundred nanoseconds (a lock handed over, a reply): a short look before
     * going to sleep saves the sleep and the wake-up (ping-pong benchmark:
     * ~700-1000 ns per round trip in the VMs). Returning on a changed value
     * is what WaitOnAddress does anyway. */
    {
        unsigned k;
        for (k = spin_count(); k; k--) {
            YieldProcessor();
            if (!same(addr, cmp, size)) return TRUE;
        }
    }
    b = bucket(addr);
    w.addr = addr;
    w.next = NULL;
    w.woken = FALSE;
    InitializeConditionVariable(&w.cv);
    AcquireSRWLockExclusive(&g_wb[b].lock);
    w.prev = g_wb[b].tail;
    if (w.prev) w.prev->next = &w; else g_wb[b].head = &w;
    g_wb[b].tail = &w;
    InterlockedIncrement(&g_wb[b].count);             /* full barrier before the value is read */
    if (same(addr, cmp, size)) {
        /* one sleep: a spurious wake-up returns TRUE, which callers handle */
        if (!SleepConditionVariableSRW(&w.cv, &g_wb[b].lock, ms, 0) && !w.woken) ok = FALSE;
    }
    if (!w.woken) unlink_waiter(b, &w);              /* a waker takes it off the list itself */
    ReleaseSRWLockExclusive(&g_wb[b].lock);
    if (!ok) SetLastError(ERROR_TIMEOUT);
    return ok;
}

static void wake(PVOID addr, BOOL all)
{
    unsigned b = bucket(addr);
    vx_waiter *w, *next;
    MemoryBarrier();                                  /* the caller's store before the count is read */
    if (!g_wb[b].count) return;
    AcquireSRWLockExclusive(&g_wb[b].lock);
    for (w = g_wb[b].head; w; w = next) {
        next = w->next;
        if (w->addr != addr) continue;
        unlink_waiter(b, w);
        w->woken = TRUE;
        /* under the lock: the waiter cannot return (and its record cannot
         * go away) before the lock is released */
        WakeConditionVariable(&w->cv);
        if (!all) break;
    }
    ReleaseSRWLockExclusive(&g_wb[b].lock);
}

VX_EXPORT void WINAPI vx_WakeByAddressAll(PVOID addr)
{ VX_NATIVE_VOID(K32, WakeByAddressAll, (PVOID addr), (addr));
    wake(addr, TRUE);
}

VX_EXPORT void WINAPI vx_WakeByAddressSingle(PVOID addr)
{ VX_NATIVE_VOID(K32, WakeByAddressSingle, (PVOID addr), (addr));
    wake(addr, FALSE);
}

/* ---- synchronization barrier (Windows 8) --------------------------------
 * The state lives in the caller's SYNCHRONIZATION_BARRIER (it fits on both
 * architectures), so there is nothing to free. A thread's very last access
 * is the decrement of "inside", and DeleteSynchronizationBarrier waits for
 * that to reach 0, so the caller may free the structure as soon as Delete
 * returns, even while woken threads are still on their way out. */
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    LONG total, remaining, generation;
    volatile LONG inside;
} vx_barrier;

typedef struct { DWORD r1, r2; ULONG_PTR r3[2]; DWORD r4, r5; } VX_SYNCHRONIZATION_BARRIER;
typedef char vx_barrier_fits[sizeof(vx_barrier) <= sizeof(VX_SYNCHRONIZATION_BARRIER) ? 1 : -1];

VX_EXPORT BOOL WINAPI vx_InitializeSynchronizationBarrier(VX_SYNCHRONIZATION_BARRIER *b, LONG total, LONG spin)
{ VX_NATIVE(K32, InitializeSynchronizationBarrier, BOOL, (VX_SYNCHRONIZATION_BARRIER *b, LONG total, LONG spin), (b, total, spin));
    vx_barrier *x = (vx_barrier *)b;
    if (!b || total < 1) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    vx_memset(b, 0, sizeof(*b));
    InitializeSRWLock(&x->lock);
    InitializeConditionVariable(&x->cv);
    x->total = x->remaining = total;
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_EnterSynchronizationBarrier(VX_SYNCHRONIZATION_BARRIER *b, DWORD flags)
{ VX_NATIVE(K32, EnterSynchronizationBarrier, BOOL, (VX_SYNCHRONIZATION_BARRIER *b, DWORD flags), (b, flags));
    vx_barrier *x = (vx_barrier *)b;
    BOOL last = FALSE;
    LONG gen;
    InterlockedIncrement(&x->inside);
    AcquireSRWLockExclusive(&x->lock);
    gen = x->generation;
    if (--x->remaining == 0) {
        x->remaining = x->total;
        x->generation++;
        WakeAllConditionVariable(&x->cv);
        last = TRUE;                 /* the last thread in gets TRUE */
    } else {
        while (gen == x->generation)
            SleepConditionVariableSRW(&x->cv, &x->lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&x->lock);
    InterlockedDecrement(&x->inside);  /* last access to the barrier */
    return last;
}

VX_EXPORT BOOL WINAPI vx_DeleteSynchronizationBarrier(VX_SYNCHRONIZATION_BARRIER *b)
{ VX_NATIVE(K32, DeleteSynchronizationBarrier, BOOL, (VX_SYNCHRONIZATION_BARRIER *b), (b));
    vx_barrier *x = (vx_barrier *)b;
    while (InterlockedCompareExchange(&x->inside, 0, 0)) SwitchToThread();
    return TRUE;
}

/* ---- TryAcquireSRWLock* (Windows 7) --------------------------------------
 * Vista and 7 share the SRW lock word layout: bit 0 = owned, bits 1-3 =
 * waiter/wake flags, bits 4+ = shared owner count. To stay safe these only
 * succeed in the uncontended cases and report "busy" otherwise, which callers
 * of a Try function always handle. */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOLEAN WINAPI vx_TryAcquireSRWLockExclusive(PSRWLOCK l)
{ VX_NATIVE(K32, TryAcquireSRWLockExclusive, BOOLEAN, (PSRWLOCK l), (l));
    return InterlockedCompareExchangePointer(&l->Ptr, (PVOID)1, NULL) == NULL;
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOLEAN WINAPI vx_TryAcquireSRWLockShared(PSRWLOCK l)
{ VX_NATIVE(K32, TryAcquireSRWLockShared, BOOLEAN, (PSRWLOCK l), (l));
    ULONG_PTR v = (ULONG_PTR)l->Ptr;
    for (;;) {
        ULONG_PTR nv;
        if (v == 0) nv = 0x11;                                  /* free -> 1 shared owner */
        else if ((v & 0xF) == 1 && (v >> 4)) nv = v + 0x10;     /* shared, no waiters */
        else return FALSE;
        {
            ULONG_PTR seen = (ULONG_PTR)InterlockedCompareExchangePointer(&l->Ptr, (PVOID)nv, (PVOID)v);
            if (seen == v) return TRUE;
            v = seen;
        }
    }
}
#endif

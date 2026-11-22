/* kernelvx - thread pool functions added in Windows 8.
 *
 * SetThreadpoolTimerEx / SetThreadpoolWaitEx are SetThreadpoolTimer /
 * SetThreadpoolWait (Vista) that also say whether the earlier setting was
 * cancelled before its callback was queued: TRUE then, FALSE otherwise -
 * also when a callback is running or about to start (Microsoft Learn,
 * threadpoolapiset.h). Callers free the callback's data on TRUE, so a wrong
 * TRUE is a use-after-free, a wrong FALSE at worst a leak.
 *
 * Vista's pool does not tell. So VistaExt remembers each timer's and wait's
 * last setting (wrapping Create/Set/Close for the program's own calls) and
 * answers TRUE only when it can prove it:
 *  - a timer: Windows' own Set has returned (so the old setting is gone) and
 *    its due time still lies ahead by more than a margin for clock
 *    granularity. Relative times are measured on the interrupt clock, which
 *    also runs during sleep, so the estimate is never later than the real
 *    expiry. A periodic timer counts only before its first expiry.
 *  - a wait: the same for its time-out, and the object is a process or a
 *    thread that is still not signalled after Windows' Set returned (their
 *    signal is permanent; an event or semaphore can be consumed or reset by
 *    the pool's wait, so for those the answer is FALSE).
 * Anything unknown (an object created elsewhere, a setting never seen) is
 * FALSE. Windows 8 and later have both: their .def forwards there. */
#include "../common/vx.h"

#if VX_OS < 602

#define MARGIN_100NS (50 * 10000)       /* 50 ms: three timer ticks */
#define NBUCK 64

typedef struct prec {
    struct prec *next;
    void *obj;                          /* PTP_TIMER / PTP_WAIT */
    BOOL armed;
    BOOL abs;                           /* expiry on the system clock (else interrupt clock) */
    BOOL never;                         /* no due time / time-out: never expires by itself */
    ULONGLONG expiry;
    DWORD period;
    HANDLE h;                           /* a wait's object */
} prec;

static struct { SRWLOCK lock; prec *head; } g_pb[NBUCK];

static unsigned pbucket(void *o) { ULONG_PTR x = (ULONG_PTR)o >> 4; return (unsigned)((x ^ (x >> 7)) % NBUCK); }

static ULONGLONG sys_now(void)
{
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static ULONGLONG int_now(void)
{
    volatile LONG *p = VX_KUSER_INTERRUPT_TIME;
    LONG hi1, hi2;
    ULONG lo;
    do { hi1 = p[1]; lo = (ULONG)p[0]; hi2 = p[2]; } while (hi1 != hi2);
    return ((ULONGLONG)(ULONG)hi1 << 32) | lo;
}

/* the record for o (created if asked), with its bucket locked */
static prec *find_locked(void *o, BOOL create, unsigned *b)
{
    prec *r;
    *b = pbucket(o);
    AcquireSRWLockExclusive(&g_pb[*b].lock);
    for (r = g_pb[*b].head; r; r = r->next) if (r->obj == o) return r;
    if (!create || !(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r)))) return NULL;
    r->obj = o;
    r->next = g_pb[*b].head;
    g_pb[*b].head = r;
    return r;
}

static void forget(void *o)
{
    prec **pp, *r;
    unsigned b = pbucket(o);
    AcquireSRWLockExclusive(&g_pb[b].lock);
    for (pp = &g_pb[b].head; *pp; pp = &(*pp)->next)
        if ((*pp)->obj == o) { r = *pp; *pp = r->next; HeapFree(GetProcessHeap(), 0, r); break; }
    ReleaseSRWLockExclusive(&g_pb[b].lock);
}

/* a new setting: when it would expire */
static void arm(prec *r, const FILETIME *due, DWORD period)
{
    LONGLONG d;
    r->period = period;
    if (!due) { r->armed = FALSE; return; }
    r->armed = TRUE;
    r->never = FALSE;
    d = ((LONGLONG)due->dwHighDateTime << 32) | due->dwLowDateTime;
    if (d > 0) { r->abs = TRUE; r->expiry = (ULONGLONG)d; }
    else { r->abs = FALSE; r->expiry = int_now() + (ULONGLONG)(-d); }     /* 0: now */
}

/* was r's setting certainly still waiting for its time? (checked after
 * Windows' Set returned, so that setting is cancelled already) */
static BOOL not_due_yet(const prec *r)
{
    ULONGLONG now;
    if (r->never) return TRUE;
    now = r->abs ? sys_now() : int_now();
    return now + MARGIN_100NS < r->expiry;
}

/* ---- timers ------------------------------------------------------------------ */
VX_EXPORT PTP_TIMER WINAPI vx_CreateThreadpoolTimer(PTP_TIMER_CALLBACK cb, PVOID ctx, PTP_CALLBACK_ENVIRON env)
{
    PTP_TIMER t = CreateThreadpoolTimer(cb, ctx, env);
    unsigned b;
    prec *r;
    if (t && (r = find_locked(t, TRUE, &b))) r->armed = FALSE;   /* a reused address starts afresh */
    if (t) ReleaseSRWLockExclusive(&g_pb[pbucket(t)].lock);
    return t;
}

VX_EXPORT void WINAPI vx_SetThreadpoolTimer(PTP_TIMER t, PFILETIME due, DWORD period, DWORD window)
{
    unsigned b;
    prec *r = find_locked(t, TRUE, &b);
    SetThreadpoolTimer(t, due, period, window);
    if (r) arm(r, due, period);
    ReleaseSRWLockExclusive(&g_pb[b].lock);
}

VX_EXPORT BOOL WINAPI vx_SetThreadpoolTimerEx(PTP_TIMER t, PFILETIME due, DWORD period, DWORD window)
{ VX_NATIVE(K32, SetThreadpoolTimerEx, BOOL, (PTP_TIMER t, PFILETIME due, DWORD period, DWORD window), (t, due, period, window));
    unsigned b;
    prec *r = find_locked(t, TRUE, &b);
    BOOL was = r && r->armed, cancelled;
    prec old;
    if (r) old = *r;
    SetThreadpoolTimer(t, due, period, window);
    /* a periodic timer may have expired once already: only before its first expiry */
    cancelled = was && not_due_yet(&old);
    if (r) arm(r, due, period);
    ReleaseSRWLockExclusive(&g_pb[b].lock);
    return cancelled;
}

VX_EXPORT void WINAPI vx_CloseThreadpoolTimer(PTP_TIMER t)
{
    forget(t);
    CloseThreadpoolTimer(t);
}

/* ---- waits ---------------------------------------------------------------------- */
typedef LONG (WINAPI *nqo_fn)(HANDLE, int, PVOID, ULONG, PULONG);
LONG WINAPI NtQueryObject(HANDLE, int, PVOID, ULONG, PULONG);

/* a process or thread: a signal nothing can take back */
static BOOL permanent_signal(HANDLE h)
{
    union { struct { USHORT len, max; WCHAR *buf; } name; BYTE raw[512]; } ti;
    static const WCHAR *k[] = { L"Process", L"Thread" };
    unsigned i;
    SIZE_T n;
    if (NtQueryObject(h, 2 /* ObjectTypeInformation */, &ti, sizeof(ti) - sizeof(WCHAR), NULL) < 0 || !ti.name.buf) return FALSE;
    n = ti.name.len / sizeof(WCHAR);
    for (i = 0; i < 2; i++) if (vx_wcslen(k[i]) == n && !vx_wcsnicmp(ti.name.buf, k[i], n)) return TRUE;
    return FALSE;
}

VX_EXPORT PTP_WAIT WINAPI vx_CreateThreadpoolWait(PTP_WAIT_CALLBACK cb, PVOID ctx, PTP_CALLBACK_ENVIRON env)
{
    PTP_WAIT w = CreateThreadpoolWait(cb, ctx, env);
    unsigned b;
    prec *r;
    if (w && (r = find_locked(w, TRUE, &b))) r->armed = FALSE;
    if (w) ReleaseSRWLockExclusive(&g_pb[pbucket(w)].lock);
    return w;
}

static void arm_wait(prec *r, HANDLE h, const FILETIME *timeout)
{
    r->h = h;
    if (!h) { r->armed = FALSE; return; }
    if (timeout) { arm(r, timeout, 0); return; }
    r->armed = TRUE;                    /* no time-out: only the object ends it */
    r->never = TRUE;
    r->abs = FALSE;
    r->expiry = 0;
    r->period = 0;
}

VX_EXPORT void WINAPI vx_SetThreadpoolWait(PTP_WAIT w, HANDLE h, PFILETIME timeout)
{
    unsigned b;
    prec *r = find_locked(w, TRUE, &b);
    SetThreadpoolWait(w, h, timeout);
    if (r) arm_wait(r, h, timeout);
    ReleaseSRWLockExclusive(&g_pb[b].lock);
}

VX_EXPORT BOOL WINAPI vx_SetThreadpoolWaitEx(PTP_WAIT w, HANDLE h, PFILETIME timeout, PVOID reserved)
{ VX_NATIVE(K32, SetThreadpoolWaitEx, BOOL, (PTP_WAIT w, HANDLE h, PFILETIME timeout, PVOID reserved), (w, h, timeout, reserved));
    unsigned b;
    prec *r, old;
    BOOL was, cancelled = FALSE;
    r = find_locked(w, TRUE, &b);
    was = r && r->armed;
    if (r) old = *r;
    SetThreadpoolWait(w, h, timeout);
    if (was && not_due_yet(&old) && permanent_signal(old.h) && WaitForSingleObject(old.h, 0) == WAIT_TIMEOUT)
        cancelled = TRUE;
    if (r) arm_wait(r, h, timeout);
    ReleaseSRWLockExclusive(&g_pb[b].lock);
    return cancelled;
}

VX_EXPORT void WINAPI vx_CloseThreadpoolWait(PTP_WAIT w)
{
    forget(w);
    CloseThreadpoolWait(w);
}

#endif

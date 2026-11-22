/* kernelvx - thread, process, power and memory functions. */
#include "../common/vx.h"

extern DWORD vx_tls_errmode;

VX_EXPORT void WINAPI vx_GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high)
{ VX_NATIVE_VOID(K32, GetCurrentThreadStackLimits, (PULONG_PTR low, PULONG_PTR high), (low, high));
    BYTE *teb = (BYTE *)NtCurrentTeb();
    *low = *(ULONG_PTR *)(teb + VX_TEB_DEALLOCSTACK);
    *high = (ULONG_PTR)((NT_TIB *)teb)->StackBase;
}

VX_EXPORT HRESULT WINAPI vx_SetThreadDescription(HANDLE h, PCWSTR d) { VX_NATIVE(K32, SetThreadDescription, HRESULT, (HANDLE h, PCWSTR d), (h, d)); return S_OK; }

VX_EXPORT HRESULT WINAPI vx_GetThreadDescription(HANDLE h, PWSTR *d)
{ VX_NATIVE(K32, GetThreadDescription, HRESULT, (HANDLE h, PWSTR *d), (h, d));
    *d = LocalAlloc(LPTR, sizeof(WCHAR));
    return *d ? S_OK : E_OUTOFMEMORY;
}

/* Thread/process information classes added in Windows 8 and 10.
 *
 * Memory priority is the page priority Vista already has (the same 0-7
 * scale; Windows 8's MEMORY_PRIORITY_* values 1-5 are its lower part), set
 * and read through ntdll's ThreadPagePriority (24) and ProcessPagePriority
 * (39) classes, so Windows really applies it.
 *
 * Power throttling (Windows 10) and the leap-second opt-out are hints about
 * features this Windows does not have: the program gets what it would get
 * with the hint ignored, which Microsoft documents as allowed. The thread
 * dynamic-code opt-out is already true here (no Arbitrary Code Guard).
 * Anything else this Windows cannot do fails with ERROR_NOT_SUPPORTED. */
LONG WINAPI NtSetInformationThread(HANDLE, int, PVOID, ULONG);
LONG WINAPI NtQueryInformationThread(HANDLE, int, PVOID, ULONG, PULONG);
LONG WINAPI NtSetInformationProcess(HANDLE, int, PVOID, ULONG);
LONG WINAPI NtQueryInformationProcess(HANDLE, int, PVOID, ULONG, PULONG);
ULONG WINAPI RtlNtStatusToDosError(LONG);

#define VX_THREAD_PAGE_PRIORITY  24
#define VX_PROCESS_PAGE_PRIORITY 39
#define VX_MEMORY_PRIORITY_NORMAL 5

static BOOL nt_result(LONG st)
{
    if (st >= 0) return TRUE;
    SetLastError(RtlNtStatusToDosError(st));
    return FALSE;
}

static BOOL page_priority(HANDLE h, BOOL thread, BOOL set, LPVOID info, DWORD size)
{
    ULONG v;
    if (!info || size != sizeof(ULONG)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (set) {
        v = *(ULONG *)info;
        if (v > VX_MEMORY_PRIORITY_NORMAL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        return nt_result(thread ? NtSetInformationThread(h, VX_THREAD_PAGE_PRIORITY, &v, sizeof(v))
                                : NtSetInformationProcess(h, VX_PROCESS_PAGE_PRIORITY, &v, sizeof(v)));
    }
    if (!nt_result(thread ? NtQueryInformationThread(h, VX_THREAD_PAGE_PRIORITY, &v, sizeof(v), NULL)
                          : NtQueryInformationProcess(h, VX_PROCESS_PAGE_PRIORITY, &v, sizeof(v), NULL)))
        return FALSE;
    *(ULONG *)info = v;
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_SetThreadInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, SetThreadInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    switch (cls) {
    case 0: /* ThreadMemoryPriority */
        return page_priority(h, TRUE, TRUE, info, size);
    case 2: /* ThreadDynamicCodePolicy: DWORD, THREAD_DYNAMIC_CODE_ALLOW (1) */
        if (!info || size != sizeof(DWORD)) break;
        return TRUE;
    case 3: /* ThreadPowerThrottling: THREAD_POWER_THROTTLING_STATE (3 ULONGs, version 1) */
        if (!info || size != 3 * sizeof(ULONG) || *(ULONG *)info != 1) break;
        return TRUE;
    case 1: /* ThreadAbsoluteCpuPriority */
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

VX_EXPORT BOOL WINAPI vx_GetThreadInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, GetThreadInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    if (cls == 0 /* ThreadMemoryPriority */) return page_priority(h, TRUE, FALSE, info, size);
    SetLastError(cls >= 1 && cls <= 3 ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
    return FALSE;
}

VX_EXPORT BOOL WINAPI vx_SetProcessInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, SetProcessInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    switch (cls) {
    case 0: /* ProcessMemoryPriority */
        return page_priority(h, FALSE, TRUE, info, size);
    case 4: /* ProcessPowerThrottling: PROCESS_POWER_THROTTLING_STATE (3 ULONGs, version 1) */
        if (!info || size != 3 * sizeof(ULONG) || *(ULONG *)info != 1) break;
        return TRUE;
    case 8: /* ProcessLeapSecondInfo: PROCESS_LEAP_SECOND_INFO (2 ULONGs); no leap seconds here */
        if (!info || size != 2 * sizeof(ULONG)) break;
        return TRUE;
    case 1:  /* ProcessMemoryExhaustionInfo (fail fast on commit failure) */
    case 3:  /* ProcessInPrivateInfo */
    case 5: case 6: case 10: case 11:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

VX_EXPORT BOOL WINAPI vx_GetProcessInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, GetProcessInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    if (cls == 0 /* ProcessMemoryPriority */) return page_priority(h, FALSE, FALSE, info, size);
    SetLastError(cls >= 1 && cls <= 11 ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
    return FALSE;
}

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HANDLE WINAPI vx_CreateRemoteThreadEx(HANDLE proc, LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
        LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPVOID attrs, LPDWORD tid)
{ VX_NATIVE(K32, CreateRemoteThreadEx, HANDLE, (HANDLE proc, LPSECURITY_ATTRIBUTES sa, SIZE_T stack,         LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPVOID attrs, LPDWORD tid), (proc, sa, stack, start, param, flags, attrs, tid));
    /* Attribute lists here only carry group affinity / ideal processor. */
    return CreateRemoteThread(proc, sa, stack, start, param, flags, tid);
}
#endif

/* Per-thread error mode (Windows 7), stored per thread. */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT DWORD WINAPI vx_GetThreadErrorMode(void)
{ VX_NATIVE(K32, GetThreadErrorMode, DWORD, (void), ());
    return vx_tls_errmode == TLS_OUT_OF_INDEXES ? 0 : (DWORD)(ULONG_PTR)TlsGetValue(vx_tls_errmode);
}
#endif

/* Vista has only the process-wide error mode. Programs set the thread mode
 * to SEM_FAILCRITICALERRORS around LoadLibrary calls that may fail, so that
 * no error box appears; while any thread has it set, it is set for the
 * process too, and the process's own setting comes back afterwards. */
static volatile LONG g_fce_threads;
static volatile LONG g_fce_added;

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_SetThreadErrorMode(DWORD mode, LPDWORD old)
{ VX_NATIVE(K32, SetThreadErrorMode, BOOL, (DWORD mode, LPDWORD old), (mode, old));
    DWORD prev;
    if (mode & ~(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    prev = vx_GetThreadErrorMode();
    if (old) *old = prev;
    if (vx_tls_errmode != TLS_OUT_OF_INDEXES) TlsSetValue(vx_tls_errmode, (LPVOID)(ULONG_PTR)mode);
    if ((mode & SEM_FAILCRITICALERRORS) && !(prev & SEM_FAILCRITICALERRORS)) {
        if (InterlockedIncrement(&g_fce_threads) == 1) {
            UINT pm = GetErrorMode();
            SetErrorMode(pm | SEM_FAILCRITICALERRORS);
            InterlockedExchange(&g_fce_added, !(pm & SEM_FAILCRITICALERRORS));
        }
    } else if (!(mode & SEM_FAILCRITICALERRORS) && (prev & SEM_FAILCRITICALERRORS)) {
        if (InterlockedDecrement(&g_fce_threads) == 0 && InterlockedExchange(&g_fce_added, 0)) {
            SetErrorMode(GetErrorMode() & ~SEM_FAILCRITICALERRORS);
        }
    }
    return TRUE;
}
#endif

/* Process mitigation policies (Windows 8). Vista and 7 can enforce only DEP
 * (SetProcessDEPPolicy, for 32-bit processes; 64-bit ones always have it).
 * Every other policy is a protection this Windows cannot give, so asking to
 * turn one on fails with ERROR_NOT_SUPPORTED: a program must not believe it
 * is protected when it is not (a sandbox that thinks dynamic code or
 * unsigned DLLs are blocked). Asking for nothing (all flags 0) is already
 * true here and succeeds. Programs that ran on Windows 7 before it had this
 * function cope with its absence, so failing is what they expect. */
VX_EXPORT BOOL WINAPI vx_SetProcessMitigationPolicy(int policy, PVOID buf, SIZE_T len)
{ VX_NATIVE(K32, SetProcessMitigationPolicy, BOOL, (int policy, PVOID buf, SIZE_T len), (policy, buf, len));
    DWORD flags;
    if (!buf || len < sizeof(DWORD) || policy < 0 || policy > 20) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    flags = *(DWORD *)buf;
    if (policy == 0 /* ProcessDEPPolicy: DWORD Flags (1 Enable, 2 DisableAtlThunkEmulation); BOOLEAN Permanent */) {
        if (len < 8) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
#ifdef _WIN64
        if (flags & 1) return TRUE;                  /* always on for 64-bit processes */
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
#else
        {
            BOOL WINAPI SetProcessDEPPolicy(DWORD);  /* Vista SP1+ */
            /* PROCESS_DEP_ENABLE 1, PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION 2: the same bits */
            return SetProcessDEPPolicy(flags & 1 ? flags & 3 : 0);
        }
#endif
    }
    if (!flags) return TRUE;
    VX_TRACE("SetProcessMitigationPolicy: a protection this Windows lacks was asked for");
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* Answers as Windows 8+ gives them for a process with no extra mitigations:
 * all off, except DEP, which is always on for 64-bit processes (and as set
 * for 32-bit ones). Programs check that DEP is on. */
VX_EXPORT BOOL WINAPI vx_GetProcessMitigationPolicy(HANDLE h, int policy, PVOID buf, SIZE_T len)
{ VX_NATIVE(K32, GetProcessMitigationPolicy, BOOL, (HANDLE h, int policy, PVOID buf, SIZE_T len), (h, policy, buf, len));
    if (!buf || !len || policy < 0 || policy > 20) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    vx_memset(buf, 0, len);
    if (policy == 0 /* ProcessDEPPolicy: DWORD Flags; BOOLEAN Permanent */ && len >= 8) {
#ifdef _WIN64
        *(DWORD *)buf = 1 | 2;                 /* Enable, DisableAtlThunkEmulation */
        ((BYTE *)buf)[4] = TRUE;               /* Permanent */
#else
        BOOL WINAPI GetProcessDEPPolicy(HANDLE, LPDWORD, PBOOL);   /* Vista SP1+ */
        DWORD flags = 0;
        BOOL perm = FALSE;
        if (GetProcessDEPPolicy(h, &flags, &perm)) {
            *(DWORD *)buf = (flags & 1 ? 1 : 0) | (flags & 2 ? 2 : 0);   /* ENABLE, DISABLE_ATL_THUNK_EMULATION */
            ((BYTE *)buf)[4] = (BYTE)perm;
        }
#endif
    }
    return TRUE;
}

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT void WINAPI vx_RaiseFailFastException(PEXCEPTION_RECORD rec, PCONTEXT ctx, DWORD flags)
{ VX_NATIVE_VOID(K32, RaiseFailFastException, (PEXCEPTION_RECORD rec, PCONTEXT ctx, DWORD flags), (rec, ctx, flags));
    TerminateProcess(GetCurrentProcess(), rec ? rec->ExceptionCode : 0xC0000602 /* STATUS_FAIL_FAST_EXCEPTION */);
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period,
        PTIMERAPCROUTINE fn, LPVOID arg, PVOID reason, ULONG delay)
{ VX_NATIVE(K32, SetWaitableTimerEx, BOOL, (HANDLE h, const LARGE_INTEGER *due, LONG period,         PTIMERAPCROUTINE fn, LPVOID arg, PVOID reason, ULONG delay), (h, due, period, fn, arg, reason, delay));
    return SetWaitableTimer(h, due, period, fn, arg, FALSE);
}
#endif

/* ---- power requests (Windows 7) -------------------------------------------
 * Vista has only SetThreadExecutionState, whose "continuous" state belongs to
 * the calling thread and ends with it, and is one state, not a count. So the
 * requests are counted here, per request and type, and one helper thread
 * (started at the first request) holds the combined state for the process:
 * a request set on a thread that then ends still holds, and clearing one
 * request leaves the others in force, as on Windows 7.
 * Types: 0 display, 1 system, 2 away mode, 3 execution (Windows 8; on Vista
 * the same as "system": the process keeps running because the PC stays on).
 * Closing a request's handle without clearing it keeps it in force until
 * that handle value comes back for a new request, or the process ends
 * (Windows 7 ends it at once; VistaExt cannot see the CloseHandle). */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
#define VX_PREQ_MAX 64
static struct { HANDLE h; LONG n[4]; } g_preq[VX_PREQ_MAX];
static LONG g_ptotal[4];
static SRWLOCK g_plock = SRWLOCK_INIT;
static HANDLE g_pwake;              /* tells the helper thread to apply the counts */

static DWORD WINAPI power_thread(LPVOID unused)
{
    for (;;) {
        EXECUTION_STATE es = ES_CONTINUOUS;
        WaitForSingleObject(g_pwake, INFINITE);
        AcquireSRWLockShared(&g_plock);
        if (g_ptotal[0]) es |= ES_DISPLAY_REQUIRED;
        if (g_ptotal[1] || g_ptotal[2] || g_ptotal[3]) es |= ES_SYSTEM_REQUIRED;
        if (g_ptotal[2]) es |= 0x00000040;     /* ES_AWAYMODE_REQUIRED (needs SYSTEM_REQUIRED too) */
        ReleaseSRWLockShared(&g_plock);
        SetThreadExecutionState(es);
    }
}

/* the helper thread, started once; FALSE if it cannot be */
static BOOL power_helper(void)
{
    static volatile LONG state;     /* 0 not yet, 1 starting, 2 running, 3 failed */
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        HANDLE t = NULL;
        g_pwake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (g_pwake) t = CreateThread(NULL, 0, power_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        InterlockedExchange(&state, t ? 2 : 3);
    }
    while (state == 1) SwitchToThread();
    return state == 2;
}

static int preq_find(HANDLE h)
{
    int i;
    for (i = 0; i < VX_PREQ_MAX; i++) if (g_preq[i].h == h && h) return i;
    return -1;
}

VX_EXPORT HANDLE WINAPI vx_PowerCreateRequest(PVOID ctx)
{ VX_NATIVE(K32, PowerCreateRequest, HANDLE, (PVOID ctx), (ctx));
    HANDLE h;
    int i;
    if (!ctx) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    if (!(h = CreateEventW(NULL, TRUE, FALSE, NULL))) return INVALID_HANDLE_VALUE;   /* the request's own handle */
    AcquireSRWLockExclusive(&g_plock);
    /* an entry with this handle value belongs to a request whose handle was
     * closed (open handles are unique): that request has ended */
    if ((i = preq_find(h)) >= 0) {
        int t;
        for (t = 0; t < 4; t++) g_ptotal[t] -= g_preq[i].n[t];
        g_preq[i].h = NULL;
        if (g_pwake) SetEvent(g_pwake);
    }
    for (i = 0; i < VX_PREQ_MAX && g_preq[i].h; i++) ;
    if (i < VX_PREQ_MAX) { g_preq[i].h = h; g_preq[i].n[0] = g_preq[i].n[1] = g_preq[i].n[2] = g_preq[i].n[3] = 0; }
    ReleaseSRWLockExclusive(&g_plock);
    if (i == VX_PREQ_MAX) { CloseHandle(h); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    return h;
}

static BOOL power_change(HANDLE h, int type, BOOL set)
{
    int i;
    BOOL ok = TRUE;
    if (type < 0 || type > 3) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (set && !power_helper()) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    AcquireSRWLockExclusive(&g_plock);
    if ((i = preq_find(h)) < 0) { SetLastError(ERROR_INVALID_HANDLE); ok = FALSE; }
    else if (set) { g_preq[i].n[type]++; g_ptotal[type]++; }
    else if (!g_preq[i].n[type]) { SetLastError(ERROR_INVALID_PARAMETER); ok = FALSE; }   /* nothing set to clear */
    else { g_preq[i].n[type]--; g_ptotal[type]--; }
    ReleaseSRWLockExclusive(&g_plock);
    if (ok) SetEvent(g_pwake);
    return ok;
}

VX_EXPORT BOOL WINAPI vx_PowerSetRequest(HANDLE h, int type)
{ VX_NATIVE(K32, PowerSetRequest, BOOL, (HANDLE h, int type), (h, type));
    return power_change(h, type, TRUE);
}

VX_EXPORT BOOL WINAPI vx_PowerClearRequest(HANDLE h, int type)
{ VX_NATIVE(K32, PowerClearRequest, BOOL, (HANDLE h, int type), (h, type));
    return power_change(h, type, FALSE);
}
#endif

/* ---- memory (Windows 8 / 8.1) -------------------------------------------- */
VX_EXPORT BOOL WINAPI vx_PrefetchVirtualMemory(HANDLE h, ULONG_PTR n, PVOID entries, ULONG flags) { VX_NATIVE(K32, PrefetchVirtualMemory, BOOL, (HANDLE h, ULONG_PTR n, PVOID entries, ULONG flags), (h, n, entries, flags)); return TRUE; }

/* Offered memory may be discarded; keeping it is always a valid outcome. */
VX_EXPORT DWORD WINAPI vx_OfferVirtualMemory(PVOID a, SIZE_T s, int prio) { VX_NATIVE(K32, OfferVirtualMemory, DWORD, (PVOID a, SIZE_T s, int prio), (a, s, prio)); return ERROR_SUCCESS; }
VX_EXPORT DWORD WINAPI vx_ReclaimVirtualMemory(const void *a, SIZE_T s) { VX_NATIVE(K32, ReclaimVirtualMemory, DWORD, (const void *a, SIZE_T s), (a, s)); return ERROR_SUCCESS; }

VX_EXPORT DWORD WINAPI vx_DiscardVirtualMemory(PVOID a, SIZE_T s)
{ VX_NATIVE(K32, DiscardVirtualMemory, DWORD, (PVOID a, SIZE_T s), (a, s));
    return VirtualAlloc(a, s, MEM_RESET, PAGE_READWRITE) ? ERROR_SUCCESS : GetLastError();
}

VX_EXPORT BOOL WINAPI vx_GetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, DWORD ms, BOOL alertable)
{ VX_NATIVE(K32, GetOverlappedResultEx, BOOL, (HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, DWORD ms, BOOL alertable), (h, ov, bytes, ms, alertable));
    if (ms) {
        HANDLE ev = ov->hEvent ? (HANDLE)((ULONG_PTR)ov->hEvent & ~(ULONG_PTR)1) : h;
        DWORD r;
        if (ov->Internal == STATUS_PENDING) {
            r = WaitForSingleObjectEx(ev, ms, alertable);
            if (r == WAIT_TIMEOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
            if (r == WAIT_IO_COMPLETION) { SetLastError(WAIT_IO_COMPLETION); return FALSE; }
            if (r == WAIT_FAILED) return FALSE;
        }
    }
    return GetOverlappedResult(h, ov, bytes, FALSE);
}

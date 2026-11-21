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

/* Thread/process information classes added in Windows 8. Memory priority,
 * power throttling and similar hints have no Vista equivalent; accepting them
 * silently matches what the hint means (best effort). */
VX_EXPORT BOOL WINAPI vx_SetThreadInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, SetThreadInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    if (cls < 0 || cls > 4) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_GetThreadInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, GetThreadInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    if (cls == 0 /* ThreadMemoryPriority */ && size >= sizeof(ULONG)) {
        *(ULONG *)info = 5;   /* MEMORY_PRIORITY_NORMAL */
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

VX_EXPORT BOOL WINAPI vx_SetProcessInformation(HANDLE h, int cls, LPVOID info, DWORD size) { VX_NATIVE(K32, SetProcessInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size)); return TRUE; }

VX_EXPORT BOOL WINAPI vx_GetProcessInformation(HANDLE h, int cls, LPVOID info, DWORD size)
{ VX_NATIVE(K32, GetProcessInformation, BOOL, (HANDLE h, int cls, LPVOID info, DWORD size), (h, cls, info, size));
    if (cls == 0 /* ProcessMemoryPriority */ && size >= sizeof(ULONG)) {
        *(ULONG *)info = 5;
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
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

/* Process mitigation policies (Windows 8): Vista enforces none of them, so
 * "set" succeeds as a no-op and "get" reports every policy as off. */
VX_EXPORT BOOL WINAPI vx_SetProcessMitigationPolicy(int policy, PVOID buf, SIZE_T len) { VX_NATIVE(K32, SetProcessMitigationPolicy, BOOL, (int policy, PVOID buf, SIZE_T len), (policy, buf, len)); return TRUE; }

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

/* ---- power requests (Windows 7) -> SetThreadExecutionState --------------- */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HANDLE WINAPI vx_PowerCreateRequest(PVOID ctx)
{ VX_NATIVE(K32, PowerCreateRequest, HANDLE, (PVOID ctx), (ctx));
    return CreateEventW(NULL, TRUE, FALSE, NULL);    /* any closable handle */
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_PowerSetRequest(HANDLE h, int type)
{ VX_NATIVE(K32, PowerSetRequest, BOOL, (HANDLE h, int type), (h, type));
    EXECUTION_STATE es = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    if (type == 0) es |= ES_DISPLAY_REQUIRED;          /* PowerRequestDisplayRequired */
    else if (type == 2) es |= 0x00000040; /* ES_AWAYMODE_REQUIRED */    /* PowerRequestAwayModeRequired */
    return SetThreadExecutionState(es) != 0;
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_PowerClearRequest(HANDLE h, int type)
{ VX_NATIVE(K32, PowerClearRequest, BOOL, (HANDLE h, int type), (h, type));
    return SetThreadExecutionState(ES_CONTINUOUS) != 0;
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

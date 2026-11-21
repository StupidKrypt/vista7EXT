/* procstat - measurements for VistaExt's own processes (tests/vm/perf.cmd).
 *   procstat show <name.exe>...       memory, threads, handles, CPU time and
 *                                     context switches of every such process
 *   procstat idle <seconds> <name>... the CPU time and the wake-ups (context
 *                                     switches) they cause in that time
 *   procstat run <program> [args]     run it (in a job): elapsed time, CPU
 *                                     time, peak memory of all its processes
 *   procstat ready <program> [args]   start it, time until it waits for input
 *   procstat service <name> <pipe>    start a stopped service, time until it
 *                                     runs and its pipe answers
 * Lines start with "PERF " so the log can be searched. */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"
#include <winternl.h>

/* the documented-by-everyone-but-Microsoft layout of NtQuerySystemInformation
 * (SystemProcessInformation = 5), the same on Vista to 10 */
typedef struct {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    LONG Priority, BasePriority;
    ULONG ContextSwitches, ThreadState, WaitReason;
} thread_info;

typedef struct {
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER Reserved[3];
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE ProcessId, ParentId;
    ULONG HandleCount, SessionId;
    ULONG_PTR PageDirectoryBase;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
           QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER IoCounters[6];
    thread_info Threads[1];
} proc_info;

/* NtQueryInformationProcess(ProcessVmCounters = 3): still answers after the
 * process has ended, while its handle is open */
typedef struct {
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
           QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} vm_counters;

/* 64-bit division without the compiler's helper (no runtime library) */
static ULONGLONG udiv(ULONGLONG n, ULONGLONG d)
{
    ULONGLONG q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    return q;
}

typedef struct { ULONGLONG cpu100ns; ULONGLONG cswitch; SIZE_T ws, priv; ULONG threads, handles, n; } totals;

static BYTE *snapshot(void)
{
    ULONG size = 1 << 20, got;
    BYTE *b;
    for (;;) {
        if (!(b = VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE))) return NULL;
        if (NtQuerySystemInformation(SystemProcessInformation, b, size, &got) >= 0) return b;
        VirtualFree(b, 0, MEM_RELEASE);
        size *= 2;
        if (size > (64u << 20)) return NULL;
    }
}

static BOOL name_is(const UNICODE_STRING *u, const WCHAR *n)
{
    SIZE_T l = vx_wcslen(n);
    return u->Buffer && u->Length / 2 == l && !vx_wcsnicmp(u->Buffer, n, l);
}

static totals sum(BYTE *b, const WCHAR *name)
{
    totals t;
    proc_info *p = (proc_info *)b;
    vx_memset(&t, 0, sizeof(t));
    for (;;) {
        if (name_is(&p->ImageName, name)) {
            ULONG i;
            t.n++;
            t.cpu100ns += p->UserTime.QuadPart + p->KernelTime.QuadPart;
            t.ws += p->WorkingSetSize;
            t.priv += p->PagefileUsage;
            t.threads += p->NumberOfThreads;
            t.handles += p->HandleCount;
            for (i = 0; i < p->NumberOfThreads; i++) t.cswitch += p->Threads[i].ContextSwitches;
        }
        if (!p->NextEntryOffset) break;
        p = (proc_info *)((BYTE *)p + p->NextEntryOffset);
    }
    return t;
}

/* context switches between two snapshots, thread by thread: a thread that
 * ended in between is left out (its count is gone from the second one),
 * one that started is counted from zero */
static ULONGLONG switches_between(BYTE *a, BYTE *b, const WCHAR *name)
{
    ULONGLONG n = 0;
    proc_info *q = (proc_info *)b;
    for (;;) {
        if (name_is(&q->ImageName, name)) {
            ULONG i, k;
            proc_info *o = (proc_info *)a, *same = NULL;
            for (;;) {
                if (o->ProcessId == q->ProcessId && o->CreateTime.QuadPart == q->CreateTime.QuadPart) { same = o; break; }
                if (!o->NextEntryOffset) break;
                o = (proc_info *)((BYTE *)o + o->NextEntryOffset);
            }
            for (i = 0; i < q->NumberOfThreads; i++) {
                ULONG before = 0;
                if (same)
                    for (k = 0; k < same->NumberOfThreads; k++)
                        if (same->Threads[k].ClientId.UniqueThread == q->Threads[i].ClientId.UniqueThread &&
                            same->Threads[k].CreateTime.QuadPart == q->Threads[i].CreateTime.QuadPart) {
                            before = same->Threads[k].ContextSwitches;
                            break;
                        }
                n += q->Threads[i].ContextSwitches - before;
            }
        }
        if (!q->NextEntryOffset) break;
        q = (proc_info *)((BYTE *)q + q->NextEntryOffset);
    }
    return n;
}

static ULONGLONG ms_since(LARGE_INTEGER a, LARGE_INTEGER f)
{
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    return udiv((ULONGLONG)(b.QuadPart - a.QuadPart) * 1000, (ULONGLONG)f.QuadPart);
}

/* the arguments as one command line, quoted only where needed: cmd /c
 * takes the rest of its line literally and strips the first and last quote
 * when there are more than two, so quoting every argument breaks
 * "cmd /c <script> <args>" */
static void command_line(int argc, WCHAR **argv, int from, WCHAR *out, SIZE_T cch)
{
    int i;
    out[0] = 0;
    for (i = from; i < argc; i++) {
        const WCHAR *a = argv[i];
        BOOL q = !a[0];
        for (; *a; a++) if (*a == ' ' || *a == '\t') q = TRUE;
        if (i > from) vx_wcscatn(out, L" ", cch);
        if (q) vx_wcscatn(out, L"\"", cch);
        vx_wcscatn(out, argv[i], cch);
        if (q) vx_wcscatn(out, L"\"", cch);
    }
}

int wmain(int argc, WCHAR **argv)
{
    LARGE_INTEGER f, t0;
    int i;
    QueryPerformanceFrequency(&f);
    if (argc > 2 && !vx_wcsicmp(argv[1], L"show")) {
        BYTE *b = snapshot();
        if (!b) return 1;
        for (i = 2; i < argc; i++) {
            totals t = sum(b, argv[i]);
            con_print(L"PERF %s: %u process(es), working set %u KB, private %u KB, %u threads, %u handles, CPU %u ms\r\n",
                      argv[i], t.n, (DWORD)(t.ws / 1024), (DWORD)(t.priv / 1024), t.threads, t.handles,
                      (DWORD)udiv(t.cpu100ns, 10000));
        }
        return 0;
    }
    if (argc > 3 && !vx_wcsicmp(argv[1], L"idle")) {
        DWORD secs = 0;
        WCHAR *s = argv[2];
        BYTE *a, *b;
        while (*s >= '0' && *s <= '9') secs = secs * 10 + (*s++ - '0');
        if (!(a = snapshot())) return 1;
        Sleep(secs * 1000);
        if (!(b = snapshot())) return 1;
        for (i = 3; i < argc; i++) {
            totals x = sum(a, argv[i]), y = sum(b, argv[i]);
            con_print(L"PERF %s idle %u s: CPU %u ms, %u wake-ups (context switches), working set %u KB, private %u KB\r\n",
                      argv[i], secs, (DWORD)udiv(y.cpu100ns - x.cpu100ns, 10000), (DWORD)switches_between(a, b, argv[i]),
                      (DWORD)(y.ws / 1024), (DWORD)(y.priv / 1024));
        }
        return 0;
    }
    if (argc > 2 && (!vx_wcsicmp(argv[1], L"run") || !vx_wcsicmp(argv[1], L"ready"))) {
        static WCHAR cmd[8192];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc;
        BOOL ready = !vx_wcsicmp(argv[1], L"ready");
        DWORD code = 0;
        command_line(argc, argv, 2, cmd, 8192);
        vx_memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        QueryPerformanceCounter(&t0);
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi)) {
            con_print(L"PERF cannot start %s (error %u)\r\n", argv[2], GetLastError());
            return 1;
        }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        if (ready) {
            DWORD r = WaitForInputIdle(pi.hProcess, 120000);
            con_print(L"PERF %s ready after %u ms%s\r\n", argv[2], (DWORD)ms_since(t0, f), r ? L" (did not become idle)" : L"");
            return 0;                                  /* it keeps running */
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        vx_memset(&lim, 0, sizeof(lim));
        vx_memset(&acc, 0, sizeof(acc));
        if (job) {
            QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim), NULL);
            QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof(acc), NULL);
        }
        {   /* peak memory: committed (charged), and in RAM (working set) of the program itself */
            vm_counters vc;
            ULONG got = 0;
            vx_memset(&vc, 0, sizeof(vc));
            NtQueryInformationProcess(pi.hProcess, (PROCESSINFOCLASS)3, &vc, sizeof(vc), &got);
            con_print(L"PERF %s: %u ms, CPU %u ms, peak memory %u KB, peak in RAM %u KB, exit %d\r\n", argv[2],
                      (DWORD)ms_since(t0, f), (DWORD)udiv((ULONGLONG)(acc.TotalUserTime.QuadPart + acc.TotalKernelTime.QuadPart), 10000),
                      (DWORD)(lim.PeakProcessMemoryUsed / 1024), (DWORD)(vc.PeakWorkingSetSize / 1024), (int)code);
        }
        return (int)code;
    }
    if (argc > 3 && !vx_wcsicmp(argv[1], L"service")) {
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s;
        SERVICE_STATUS st;
        WCHAR rep[512], req[] = L"STATUS";
        DWORD got, running = 0;
        if (!m || !(s = OpenServiceW(m, argv[2], SERVICE_START | SERVICE_QUERY_STATUS))) return 1;
        QueryPerformanceCounter(&t0);
        if (!StartServiceW(s, 0, NULL)) { con_print(L"PERF cannot start %s (error %u)\r\n", argv[2], GetLastError()); return 1; }
        while (QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_RUNNING && ms_since(t0, f) < 120000) Sleep(10);
        running = (DWORD)ms_since(t0, f);
        while (!CallNamedPipeW(argv[3], req, sizeof(req), rep, sizeof(rep), &got, 1000) && ms_since(t0, f) < 120000) Sleep(10);
        con_print(L"PERF service %s: running after %u ms, answering after %u ms\r\n", argv[2], running, (DWORD)ms_since(t0, f));
        return 0;
    }
    con_print(L"usage: procstat show|idle|run|ready|service ...\r\n");
    return 2;
}

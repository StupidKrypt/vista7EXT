/* vxbench - how much VistaExt costs a program, and how fast its own
 * functions are. Prints "BENCH <name>: <value> <unit>" lines.
 *
 * Built twice: vxbench.exe imports Windows 8 functions (WaitOnAddress, ...)
 * from kernel32, so it needs VistaExt (file or in-memory mode); with
 * -DVX_BENCH_NATIVE (vxbench-native.exe) it imports nothing new and runs on
 * plain Windows, as the baseline. "vxbench /child" exits at once (process
 * start-up time). */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

#ifndef VX_BENCH_NATIVE
#define p_WaitOnAddress WaitOnAddress
#define p_WakeByAddressSingle WakeByAddressSingle
#define p_WakeByAddressAll WakeByAddressAll
#define p_GetSystemTimePreciseAsFileTime GetSystemTimePreciseAsFileTime
#define HAVE_WIN8 1
#else
static BOOL (WINAPI *p_WaitOnAddress)(volatile VOID *, PVOID, SIZE_T, DWORD);
static void (WINAPI *p_WakeByAddressSingle)(PVOID);
static void (WINAPI *p_WakeByAddressAll)(PVOID);
static void (WINAPI *p_GetSystemTimePreciseAsFileTime)(LPFILETIME);
#define HAVE_WIN8 (p_WaitOnAddress != NULL)
#endif

static LONGLONG g_freq;
static LONGLONG now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* 64-bit division without the compiler's helper functions (no runtime
 * library is linked, and 32-bit code would need one) */
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

/* elapsed QPC ticks -> ns (unit 1000000000) or us (1000000) per operation */
static DWORD per_op(LONGLONG ticks, LONGLONG ops, LONGLONG unit)
{
    ULONGLONG us_total = udiv((ULONGLONG)ticks * 1000000, (ULONGLONG)g_freq);   /* microseconds */
    return (DWORD)udiv(us_total * (unit == 1000000000 ? 1000 : 1), (ULONGLONG)ops);
}

static void report(const WCHAR *name, DWORD v, const WCHAR *unit) { con_print(L"BENCH %s: %u %s\r\n", name, v, unit); }

/* ---- plain Windows functions (what the verifier could slow down) ---------- */
static void bench_heap(void)
{
    HANDLE h = GetProcessHeap();
    LONGLONG t = now();
    int i;
    for (i = 0; i < 1000000; i++) HeapFree(h, 0, HeapAlloc(h, 0, 64 + (i & 255)));
    report(L"heap alloc+free (1 thread)", per_op(now() - t, 1000000, 1000000000), L"ns");
}

static DWORD WINAPI heap_thread(LPVOID unused)
{
    HANDLE h = GetProcessHeap();
    int i;
    for (i = 0; i < 300000; i++) HeapFree(h, 0, HeapAlloc(h, 0, 32 + (i & 127)));
    return 0;
}

static void run_threads(LPTHREAD_START_ROUTINE fn, int n, LPVOID arg)
{
    HANDLE th[16];
    int i;
    for (i = 0; i < n; i++) th[i] = CreateThread(NULL, 0, fn, arg, 0, NULL);
    WaitForMultipleObjects(n, th, TRUE, INFINITE);
    for (i = 0; i < n; i++) CloseHandle(th[i]);
}

static void bench_heap4(void)
{
    LONGLONG t = now();
    run_threads(heap_thread, 4, NULL);
    report(L"heap alloc+free (4 threads)", per_op(now() - t, 1200000, 1000000000), L"ns");
}

static void bench_locks(void)
{
    CRITICAL_SECTION cs;
    SRWLOCK srw = SRWLOCK_INIT;
    LONGLONG t;
    int i;
    InitializeCriticalSection(&cs);
    t = now();
    for (i = 0; i < 5000000; i++) { EnterCriticalSection(&cs); LeaveCriticalSection(&cs); }
    report(L"critical section enter+leave", per_op(now() - t, 5000000, 1000000000), L"ns");
    DeleteCriticalSection(&cs);
    t = now();
    for (i = 0; i < 5000000; i++) { AcquireSRWLockExclusive(&srw); ReleaseSRWLockExclusive(&srw); }
    report(L"SRW lock acquire+release", per_op(now() - t, 5000000, 1000000000), L"ns");
}

static DWORD WINAPI nothing(LPVOID unused) { return 0; }

static void bench_threads(void)
{
    LONGLONG t = now();
    int i;
    for (i = 0; i < 500; i++) {
        HANDLE h = CreateThread(NULL, 0, nothing, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    report(L"thread start+end", per_op(now() - t, 500, 1000000), L"us");
}

static void bench_loadlib(void)
{
    WCHAR p[MAX_PATH];
    SIZE_T n;
    LONGLONG t;
    int i;
    GetModuleFileNameW(NULL, p, MAX_PATH);
    n = vx_wcslen(p);
    while (n && p[n - 1] != '\\') n--;
    vx_wcscpyn(p + n, L"benchdll.dll", MAX_PATH - n);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    t = now();
    for (i = 0; i < 300; i++) {
        HMODULE h = LoadLibraryW(p);
        if (!h) { con_print(L"BENCH benchdll.dll does not load (error %u)\r\n", GetLastError()); return; }
        FreeLibrary(h);
    }
    report(L"LoadLibrary+FreeLibrary of a program DLL", per_op(now() - t, 300, 1000000), L"us");
}

static void bench_getproc(void)
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    LONGLONG t = now();
    int i;
    for (i = 0; i < 1000000; i++) GetProcAddress(k, "GetTickCount");
    report(L"GetProcAddress(kernel32, existing)", per_op(now() - t, 1000000, 1000000000), L"ns");
    t = now();
    for (i = 0; i < 200000; i++) GetProcAddress(k, "WaitOnAddress");
    report(L"GetProcAddress(kernel32, Windows 8 function)", per_op(now() - t, 200000, 1000000000), L"ns");
    {   /* a module VistaExt has no companion for */
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        if (!g) g = LoadLibraryW(L"gdi32.dll");
        t = now();
        for (i = 0; i < 200000; i++) GetProcAddress(g, "NoSuchFunctionAnywhere");
        report(L"GetProcAddress(gdi32, missing)", per_op(now() - t, 200000, 1000000000), L"ns");
    }
}

/* ---- what VistaExt's hooks cost every call ---------------------------------
 * registry values of the CurrentVersion key (answered by vxload while a
 * version is told), another value of the same key, a value elsewhere, and
 * GetVersionEx (kernelvx) */
static void bench_registry(void)
{
    HKEY cv = NULL, other = NULL;
    WCHAR buf[128];
    DWORD cb, type;
    LONGLONG t;
    int i;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &cv);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE, &other);
    if (!cv || !other) return;
    t = now();
    for (i = 0; i < 100000; i++) { cb = sizeof(buf); RegQueryValueExW(cv, L"CurrentBuildNumber", NULL, &type, (BYTE *)buf, &cb); }
    report(L"registry: CurrentVersion\\CurrentBuildNumber", per_op(now() - t, 100000, 1000000000), L"ns");
    t = now();
    for (i = 0; i < 100000; i++) { cb = sizeof(buf); RegQueryValueExW(cv, L"SystemRoot", NULL, &type, (BYTE *)buf, &cb); }
    report(L"registry: CurrentVersion\\SystemRoot (not a version value)", per_op(now() - t, 100000, 1000000000), L"ns");
    t = now();
    for (i = 0; i < 100000; i++) { cb = sizeof(buf); RegQueryValueExW(other, L"PendingFileRenameOperationsX", NULL, &type, (BYTE *)buf, &cb); }
    report(L"registry: a value elsewhere (missing)", per_op(now() - t, 100000, 1000000000), L"ns");
    RegCloseKey(cv);
    RegCloseKey(other);
    {
        OSVERSIONINFOEXW vi;
        t = now();
        for (i = 0; i < 1000000; i++) { vi.dwOSVersionInfoSize = sizeof(vi); GetVersionExW((OSVERSIONINFOW *)&vi); }
        report(L"GetVersionEx", per_op(now() - t, 1000000, 1000000000), L"ns");
        con_print(L"BENCH info: told Windows %u.%u.%u\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
}

static void bench_mapping(void)
{
    /* an unnamed section with a DACL (as Chromium makes them) */
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    BYTE aclbuf[64];
    PACL acl = (PACL)aclbuf;
    LONGLONG t;
    int i;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    InitializeAcl(acl, sizeof(aclbuf), ACL_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    t = now();
    for (i = 0; i < 3000; i++) {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 65536, NULL);
        if (h) CloseHandle(h);
    }
    report(L"CreateFileMapping (unnamed, with DACL)", per_op(now() - t, 3000, 1000000000), L"ns");
}

static void bench_dpi(void)
{
    UINT (WINAPI *dpi)(HWND) = (void *)GetProcAddress(LoadLibraryW(L"user32.dll"), "GetDpiForWindow");
    HWND w = GetDesktopWindow();
    LONGLONG t;
    int i;
    if (!dpi) return;
    t = now();
    for (i = 0; i < 200000; i++) dpi(w);
    report(L"GetDpiForWindow", per_op(now() - t, 200000, 1000000000), L"ns");
}

static void bench_start(void)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 20];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    LONGLONG t;
    int i;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    vx_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    t = now();
    for (i = 0; i < 40; i++) {
        cmd[0] = '"';
        vx_wcscpyn(cmd + 1, exe, MAX_PATH);
        vx_wcscatn(cmd, L"\" /child", MAX_PATH + 20);
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return;
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    report(L"process start+exit", per_op(now() - t, 40, 1000000), L"us");
}

/* ---- VistaExt's Windows 8 functions ---------------------------------------- */
/* The previous VistaExt design (one condition variable per hash bucket, every
 * wake wakes the whole bucket), measured in the same run for comparison. */
static struct { SRWLOCK lock; CONDITION_VARIABLE cv; } g_old[128];
static unsigned old_bucket(volatile void *a) { return (unsigned)(((ULONG_PTR)a >> 3) % 128); }
static BOOL WINAPI old_wait(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms)
{
    unsigned b = old_bucket(addr);
    BOOL ok = TRUE;
    AcquireSRWLockExclusive(&g_old[b].lock);
    if (*(volatile LONG *)addr == *(LONG *)cmp) ok = SleepConditionVariableSRW(&g_old[b].cv, &g_old[b].lock, ms, 0);
    ReleaseSRWLockExclusive(&g_old[b].lock);
    return ok;
}
static void WINAPI old_wake(PVOID addr)
{
    unsigned b = old_bucket(addr);
    AcquireSRWLockExclusive(&g_old[b].lock);
    WakeAllConditionVariable(&g_old[b].cv);
    ReleaseSRWLockExclusive(&g_old[b].lock);
}
static BOOL (WINAPI *w_wait)(volatile VOID *, PVOID, SIZE_T, DWORD);
static void (WINAPI *w_wake)(PVOID);
static const WCHAR *w_name;

static void report2(const WCHAR *name, DWORD v, const WCHAR *unit)
{
    WCHAR b[160];
    wsprintfW(b, L"%s [%s]", name, w_name);
    report(b, v, unit);
}
static volatile LONG g_turn;
#define ROUNDS 30000

static void wait_for(volatile LONG *a, LONG want)
{
    for (;;) {
        LONG v = *a;
        if (v == want) return;
        w_wait(a, &v, sizeof(v), INFINITE);
    }
}

static DWORD WINAPI pong(LPVOID unused)
{
    int i;
    for (i = 0; i < ROUNDS; i++) {
        wait_for(&g_turn, 1);
        InterlockedExchange(&g_turn, 0);
        w_wake((PVOID)&g_turn);
    }
    return 0;
}

static void bench_pingpong(void)
{
    HANDLE th;
    LONGLONG t;
    int i;
    g_turn = 0;
    th = CreateThread(NULL, 0, pong, NULL, 0, NULL);
    t = now();
    for (i = 0; i < ROUNDS; i++) {
        InterlockedExchange(&g_turn, 1);
        w_wake((PVOID)&g_turn);
        wait_for(&g_turn, 0);
    }
    report2(L"WaitOnAddress ping-pong round trip", per_op(now() - t, ROUNDS, 1000000000), L"ns");
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}

/* a lock built the way Rust's standard library builds its Mutex on Windows */
static volatile LONG g_mutex;
static volatile LONG g_counter;

static void futex_lock(volatile LONG *m)
{
    LONG two = 2;
    if (InterlockedCompareExchange(m, 1, 0) == 0) return;
    while (InterlockedExchange(m, 2) != 0) w_wait(m, &two, sizeof(two), INFINITE);
}
static void futex_unlock(volatile LONG *m)
{
    if (InterlockedExchange(m, 0) == 2) w_wake((PVOID)m);
}

static DWORD WINAPI mutex_thread(LPVOID unused)
{
    int i, k;
    for (i = 0; i < 100000; i++) {
        futex_lock(&g_mutex);
        for (k = 0; k < 20; k++) g_counter++;
        futex_unlock(&g_mutex);
    }
    return 0;
}

static void bench_mutex(void)
{
    LONGLONG t = now();
    run_threads(mutex_thread, 4, NULL);
    report2(L"WaitOnAddress mutex, 4 threads (per lock)", per_op(now() - t, 400000, 1000000000), L"ns");
}

/* a semaphore with many sleepers on one address: each release should wake
 * one of them, not all */
static volatile LONG g_tokens, g_left;

static DWORD WINAPI consumer(LPVOID unused)
{
    for (;;) {
        LONG c = g_tokens;
        if (c > 0) {
            if (InterlockedCompareExchange(&g_tokens, c - 1, c) == c) {
                if (InterlockedDecrement(&g_left) < 0) return 0;
            }
            continue;
        }
        if (g_left < 0) return 0;
        w_wait(&g_tokens, &c, sizeof(c), 50);
    }
}

static void bench_semaphore(void)
{
    HANDLE th[8];
    LONGLONG t;
    int i;
    g_tokens = 0;
    g_left = 20000;
    for (i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    Sleep(50);
    t = now();
    for (i = 0; i < 20000 + 8; i++) {
        InterlockedIncrement(&g_tokens);
        w_wake((PVOID)&g_tokens);
        if ((i & 15) == 15) SwitchToThread();
    }
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    report2(L"WaitOnAddress semaphore, 8 sleepers (per release)", per_op(now() - t, 20000, 1000000000), L"ns");
    for (i = 0; i < 8; i++) CloseHandle(th[i]);
}

static void bench_time(void)
{
    FILETIME a, b;
    ULONGLONG prev = 0, cur, steps = 0, first = 0, last = 0;
    LONGLONG t = now(), end;
    int i;
    for (i = 0; i < 1000000; i++) p_GetSystemTimePreciseAsFileTime(&a);
    report(L"GetSystemTimePreciseAsFileTime call", per_op(now() - t, 1000000, 1000000000), L"ns");
    /* resolution: average step between distinct values over 100 ms */
    end = now() + (LONGLONG)udiv((ULONGLONG)g_freq, 10);
    while (now() < end) {
        p_GetSystemTimePreciseAsFileTime(&b);
        cur = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
        if (cur != prev) { if (!first) first = cur; else steps++; last = cur; prev = cur; }
    }
    report(L"GetSystemTimePreciseAsFileTime resolution", steps ? (DWORD)udiv(last - first, 10 * steps) : 0, L"us");
    /* never more than 2 ms away from the normal clock */
    for (i = 0; i < 200000; i++) {
        ULONGLONG p, c;
        p_GetSystemTimePreciseAsFileTime(&a);
        GetSystemTimeAsFileTime(&b);
        p = ((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime;
        c = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
        if (p > c + 160000 + 20000 || p + 160000 + 20000 < c) {
            con_print(L"BENCH precise time is off from the system clock by more than 2 ms\r\n");
            break;
        }
    }
}

/* where a function the program imports really goes (verifier.dll = hooked) */
static void owner(const WCHAR *what, const void *fn)
{
    HMODULE m = NULL;
    WCHAR path[MAX_PATH] = L"?", *b;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &m);
    if (m) GetModuleFileNameW(m, path, MAX_PATH);
    for (b = path + vx_wcslen(path); b > path && b[-1] != '\\'; b--) ;
    con_print(L"INFO %s goes to %s\r\n", what, b);
}

static void diagnostics(void)
{
    BYTE *peb = *(BYTE **)((BYTE *)NtCurrentTeb() + (sizeof(void *) == 8 ? 0x60 : 0x30));
    ULONG gf = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0xBC : 0x68)), lfh = 99;
    HeapQueryInformation(GetProcessHeap(), HeapCompatibilityInformation, &lfh, sizeof(lfh), NULL);
    con_print(L"INFO global flags 0x%x, process heap type %u (2 = low-fragmentation heap), verifier.dll %s\r\n",
              gf, lfh, GetModuleHandleW(L"verifier.dll") ? L"loaded" : L"not loaded");
    owner(L"HeapAlloc", (const void *)HeapAlloc);
    owner(L"EnterCriticalSection", (const void *)EnterCriticalSection);
    owner(L"WaitForSingleObject", (const void *)WaitForSingleObject);
}

int wmain(int argc, WCHAR **argv)
{
    LARGE_INTEGER f;
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/child")) return 0;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
#ifdef VX_BENCH_NATIVE
    {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        p_WaitOnAddress = (void *)GetProcAddress(k, "WaitOnAddress");
        p_WakeByAddressSingle = (void *)GetProcAddress(k, "WakeByAddressSingle");
        p_WakeByAddressAll = (void *)GetProcAddress(k, "WakeByAddressAll");
        p_GetSystemTimePreciseAsFileTime = (void *)GetProcAddress(k, "GetSystemTimePreciseAsFileTime");
    }
#endif
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    diagnostics();
    bench_start();
    bench_heap();
    bench_heap4();
    bench_locks();
    bench_threads();
    bench_loadlib();
    bench_getproc();
    bench_registry();
    bench_mapping();
    bench_dpi();
    if (HAVE_WIN8) {
        int round;
        for (round = 0; round < 2; round++) {   /* alternate, so both see the same conditions */
            w_wait = p_WaitOnAddress; w_wake = p_WakeByAddressSingle; w_name = L"VistaExt";
            bench_pingpong(); bench_mutex(); bench_semaphore();
            w_wait = old_wait; w_wake = old_wake; w_name = L"old design";
            bench_pingpong(); bench_mutex(); bench_semaphore();
        }
        bench_time();
    }
    return 0;
}

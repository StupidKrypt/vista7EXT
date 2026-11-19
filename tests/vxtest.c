/* Test program: imports newer-Windows functions straight from kernel32 and
 * user32, like a modern program would. On real Vista (and under the test's
 * fake "Vista" system folder) it only loads after vxpatch. */
#include "../src/common/vx.h"
#include "../src/common/console.h"
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>

/* The functions are declared by the SDK headers; the test links them
 * against its own import library that says they live in kernel32/user32. */

LONG WINAPI GetCurrentPackageFullName(UINT32 *, PWSTR);   /* appmodel.h */
LONG WINAPI RtlGetVersion(OSVERSIONINFOW *);
void WINAPI RtlGetNtVersionNumbers(DWORD *, DWORD *, DWORD *);
DWORD WINAPI SHGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD *, void *, DWORD *);   /* shlwapi: a Windows DLL reads it */

static int fails;
#define CHECK(c, what) do { if (c) con_print(L"  ok    %s\r\n", what); else { con_print(L"  FAIL  %s\r\n", what); fails++; } } while (0)

static volatile LONG g_flag;
static SYNCHRONIZATION_BARRIER g_barrier;
static volatile LONG g_through;

static volatile LONG g_two, g_woke;
static DWORD WINAPI sleeper(LPVOID unused)
{
    LONG zero = 0;
    WaitOnAddress(&g_two, &zero, sizeof(zero), 5000);
    InterlockedIncrement(&g_woke);
    return 0;
}

static DWORD WINAPI waker(LPVOID p)
{
    Sleep(100);
    InterlockedExchange(&g_flag, 1);
    WakeByAddressAll((PVOID)&g_flag);
    return 0;
}

static DWORD WINAPI barrier_thread(LPVOID p)
{
    EnterSynchronizationBarrier(&g_barrier, 0);
    InterlockedIncrement(&g_through);
    return 0;
}

/* ---- /expectall: the same version everywhere a program may look ---------- */
#define CVKEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
static BOOL reg_sz(const WCHAR *name, WCHAR *out, DWORD cch)
{
    HKEY k;
    DWORD type, cb = (cch - 1) * sizeof(WCHAR);
    LONG r;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CVKEY, 0, KEY_QUERY_VALUE, &k)) return FALSE;
    r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb);
    RegCloseKey(k);
    if (r || type != REG_SZ) { out[0] = 0; return FALSE; }
    out[cb / sizeof(WCHAR)] = 0;
    return TRUE;
}

static BOOL reg_dw(const WCHAR *name, DWORD *v)
{
    HKEY k;
    DWORD type, cb = sizeof(*v);
    LONG r;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CVKEY, 0, KEY_QUERY_VALUE, &k)) return FALSE;
    r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)v, &cb);
    RegCloseKey(k);
    return !r && type == REG_DWORD;
}

static BOOL starts(const WCHAR *s, const WCHAR *p) { return !vx_wcsnicmp(s, p, vx_wcslen(p)); }
static BOOL has(const WCHAR *s, const WCHAR *w)
{
    SIZE_T n = vx_wcslen(w);
    for (; s && *s; s++) if (!vx_wcsnicmp(s, w, n)) return TRUE;
    return FALSE;
}

static const WCHAR *product_prefix(DWORD maj, DWORD min)
{
    if (maj >= 10) return L"Windows 10";       /* also Windows 11: Windows itself says "Windows 10" here */
    if (maj == 6 && min == 3) return L"Windows 8.1";
    if (maj == 6 && min == 2) return L"Windows 8";
    if (maj == 6 && min == 1) return L"Windows 7";
    return L"Windows Vista";
}

static void check_everywhere(DWORD maj, DWORD min, DWORD build)
{
    WCHAR want[32], v[260];
    DWORD a = 0, b = 0, c = 0, d;
    wsprintfW(want, L"%u", build);
    RtlGetNtVersionNumbers(&a, &b, &c);
    con_print(L"  info  RtlGetNtVersionNumbers %u.%u.%u\r\n", a, b, c & 0xFFFF);
    CHECK(a == maj && b == min && (c & 0xFFFF) == build, L"RtlGetNtVersionNumbers reports the chosen version");
    reg_sz(L"CurrentBuildNumber", v, 260);
    con_print(L"  info  registry CurrentBuildNumber %s\r\n", v);
    CHECK(!vx_wcsicmp(v, want), L"registry: CurrentBuildNumber is the chosen build");
    reg_sz(L"CurrentBuild", v, 260);
    CHECK(!vx_wcsicmp(v, want), L"registry: CurrentBuild is the chosen build");
    reg_sz(L"CurrentVersion", v, 260);
    con_print(L"  info  registry CurrentVersion %s\r\n", v);
    if (maj >= 10) {
        CHECK(!vx_wcsicmp(v, L"6.3"), L"registry: CurrentVersion 6.3, as Windows 10/11 have it");
        CHECK(reg_dw(L"CurrentMajorVersionNumber", &d) && d == maj, L"registry: CurrentMajorVersionNumber");
        CHECK(reg_dw(L"CurrentMinorVersionNumber", &d) && d == min, L"registry: CurrentMinorVersionNumber");
        reg_sz(L"DisplayVersion", v, 260);
        con_print(L"  info  registry DisplayVersion %s\r\n", v);
    } else {
        WCHAR cv[16];
        wsprintfW(cv, L"%u.%u", maj, min);
        CHECK(!vx_wcsicmp(v, cv), L"registry: CurrentVersion is the chosen version");
        CHECK(!reg_dw(L"CurrentMajorVersionNumber", &d), L"registry: no CurrentMajorVersionNumber (only Windows 10+ has it)");
    }
    reg_sz(L"ProductName", v, 260);
    con_print(L"  info  registry ProductName %s\r\n", v);
    CHECK(starts(v, product_prefix(maj, min)), L"registry: ProductName names the chosen Windows");
    {   /* read through a Windows DLL (shlwapi -> advapi32 -> ntdll) */
        DWORD type, cb = sizeof(v);
        v[0] = 0;
        SHGetValueW(HKEY_LOCAL_MACHINE, CVKEY, L"CurrentBuildNumber", &type, v, &cb);
        CHECK(!vx_wcsicmp(v, want), L"registry through Windows' shlwapi: CurrentBuildNumber is the chosen build");
    }
    {   /* WMI: Win32_OperatingSystem */
        IWbemLocator *loc = NULL;
        IWbemServices *svc = NULL;
        IEnumWbemClassObject *en = NULL;
        IWbemClassObject *o = NULL;
        ULONG got = 0;
        VARIANT var;
        BOOL ver = FALSE, bld = FALSE, cap = FALSE;
        BSTR ns = SysAllocString(L"ROOT\\CIMV2"), wql = SysAllocString(L"WQL"),
             q = SysAllocString(L"SELECT Version, BuildNumber, Caption FROM Win32_OperatingSystem");
        WCHAR wv[40];
        wsprintfW(wv, L"%u.%u.%u", maj, min, build);
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
        if (SUCCEEDED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc)) &&
            SUCCEEDED(loc->lpVtbl->ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc)) &&
            SUCCEEDED(CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                                        RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE)) &&
            SUCCEEDED(svc->lpVtbl->ExecQuery(svc, wql, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en)) &&
            SUCCEEDED(en->lpVtbl->Next(en, 20000, 1, &o, &got)) && got == 1) {
            VariantInit(&var);
            if (SUCCEEDED(o->lpVtbl->Get(o, L"Version", 0, &var, NULL, NULL)) && V_VT(&var) == VT_BSTR) {
                con_print(L"  info  WMI Version %s\r\n", V_BSTR(&var));
                ver = !vx_wcsicmp(V_BSTR(&var), wv);
            }
            VariantClear(&var);
            if (SUCCEEDED(o->lpVtbl->Get(o, L"BuildNumber", 0, &var, NULL, NULL)) && V_VT(&var) == VT_BSTR)
                bld = !vx_wcsicmp(V_BSTR(&var), want);
            VariantClear(&var);
            if (SUCCEEDED(o->lpVtbl->Get(o, L"Caption", 0, &var, NULL, NULL)) && V_VT(&var) == VT_BSTR) {
                con_print(L"  info  WMI Caption %s\r\n", V_BSTR(&var));
                cap = has(V_BSTR(&var), maj >= 10 ? (build >= 22000 ? L"Windows 11" : L"Windows 10") : product_prefix(maj, min));
            }
            VariantClear(&var);
            o->lpVtbl->Release(o);
        } else con_print(L"  info  WMI query failed\r\n");
        CHECK(ver, L"WMI Win32_OperatingSystem.Version is the chosen version");
        CHECK(bld, L"WMI Win32_OperatingSystem.BuildNumber is the chosen build");
        CHECK(cap, L"WMI Win32_OperatingSystem.Caption names the chosen Windows");
        if (en) en->lpVtbl->Release(en);
        if (svc) svc->lpVtbl->Release(svc);
        if (loc) loc->lpVtbl->Release(loc);
        SysFreeString(ns); SysFreeString(wql); SysFreeString(q);
    }
}

int wmain(int argc, WCHAR **argv)
{
    FILETIME ft;
    SRWLOCK l = SRWLOCK_INIT;
    LONG zero = 0;
    DWORD len = 0, t0;
    ULONG_PTR lo, hi;
    UINT32 plen = 0;
    OSVERSIONINFOW vi;
    HANDLE th[3];
    int i;
    FARPROC p;

    con_print(L"VistaExt self-test (%s)\r\n", sizeof(void *) == 8 ? L"64-bit" : L"32-bit");

    GetSystemTimePreciseAsFileTime(&ft);
    CHECK(ft.dwHighDateTime > 0x01D00000, L"GetSystemTimePreciseAsFileTime");

    {   /* shared memory protected by an empty DACL (what Chromium does): a
         * read-only handle must not be duplicable as writable */
        ACL acl;
        SECURITY_DESCRIPTOR sd;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
        HANDLE sec, ro = NULL, rw = NULL;
        InitializeAcl(&acl, sizeof(acl), ACL_REVISION);
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, &acl, FALSE);
        sec = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 65536, NULL);
        CHECK(sec != NULL, L"CreateFileMappingW (unnamed, empty DACL)");
        if (sec) {
            DuplicateHandle(GetCurrentProcess(), sec, GetCurrentProcess(), &ro, FILE_MAP_READ, FALSE, 0);
            CHECK(ro && !DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &rw, FILE_MAP_WRITE, FALSE, 0),
                  L"read-only shared memory cannot be made writable (Chromium's check)");
            if (rw) CloseHandle(rw);
            if (ro) CloseHandle(ro);
            CloseHandle(sec);
        }
    }

    t0 = GetTickCount();
    CloseHandle(CreateThread(NULL, 0, waker, NULL, 0, NULL));
    while (!g_flag) WaitOnAddress(&g_flag, &zero, sizeof(zero), 5000);
    CHECK(g_flag == 1 && GetTickCount() - t0 < 4000, L"WaitOnAddress / WakeByAddressAll");
    CHECK(!WaitOnAddress(&g_flag, (PVOID)&g_flag, 4, 50) && GetLastError() == ERROR_TIMEOUT, L"WaitOnAddress timeout");
    CHECK(!WaitOnAddress(&g_flag, (PVOID)&g_flag, 4, 0) && GetLastError() == ERROR_TIMEOUT, L"WaitOnAddress with no wait");
    {   /* WakeByAddressSingle wakes one of two sleepers, WakeByAddressAll the rest */
        LONG one = 1;
        HANDLE s1 = CreateThread(NULL, 0, sleeper, NULL, 0, NULL), s2 = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
        CHECK(WaitOnAddress(&g_flag, &zero, sizeof(zero), 5000), L"WaitOnAddress returns at once when the value differs");
        Sleep(300);
        WakeByAddressSingle((PVOID)&g_two);
        Sleep(300);
        CHECK(g_woke == 1, L"WakeByAddressSingle wakes exactly one waiter");
        WakeByAddressAll((PVOID)&g_two);
        WaitForSingleObject(s1, 4000);
        WaitForSingleObject(s2, 4000);
        CHECK(g_woke == 2, L"WakeByAddressAll wakes the others");
        CloseHandle(s1); CloseHandle(s2);
        (void)one;
    }
    {   /* precise time: finer than the 15.6 ms tick, never behind itself in
         * one thread by more than a tick, close to the normal clock */
        FILETIME a, b;
        ULONGLONG prev = 0, cur, c, distinct = 0;
        DWORD end = GetTickCount() + 100;
        BOOL close_ok = TRUE, forward = TRUE;
        while (GetTickCount() < end) {
            GetSystemTimePreciseAsFileTime(&a);
            GetSystemTimeAsFileTime(&b);
            cur = ((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime;
            c = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
            /* read before the normal clock: up to one tick (15.6 ms) behind it if a
             * tick came in between, never further, and never ahead by more */
            if (cur > c + 160000 + 20000 || cur + 160000 + 20000 < c) close_ok = FALSE;
            if (prev && cur + 160000 < prev) forward = FALSE;
            if (cur != prev) distinct++;
            prev = cur;
        }
        CHECK(close_ok, L"GetSystemTimePreciseAsFileTime stays within a tick of the system clock");
        CHECK(forward, L"GetSystemTimePreciseAsFileTime does not go back");
        {   /* finer than the tick where the performance counter is cheap (in
             * a VM it often is not, and the normal clock is used instead) */
            LARGE_INTEGER f, q0, q1;
            int k;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&q0);
            for (k = 0; k < 16; k++) QueryPerformanceCounter(&q1);
            if ((q1.QuadPart - q0.QuadPart) * 10000000 < f.QuadPart * 24)
                CHECK(distinct > 50, L"GetSystemTimePreciseAsFileTime is finer than the timer tick");
            else
                con_print(L"  info  slow performance counter here: precise time = normal clock\r\n");
        }
    }

    CHECK(GetActiveProcessorCount(0xFFFF) >= 1, L"GetActiveProcessorCount");
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    {
        BYTE *buf = HeapAlloc(GetProcessHeap(), 0, len);
        BOOL ok = buf && GetLogicalProcessorInformationEx(RelationAll, (void *)buf, &len);
        DWORD off = 0, cores = 0;
        while (ok && off < len) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)(buf + off);
            if (e->Relationship == RelationProcessorCore) cores++;
            off += e->Size;
        }
        CHECK(ok && cores >= 1, L"GetLogicalProcessorInformationEx");
    }

    CHECK(TryAcquireSRWLockExclusive(&l) && !TryAcquireSRWLockExclusive(&l), L"TryAcquireSRWLockExclusive");
    ReleaseSRWLockExclusive(&l);
    CHECK(TryAcquireSRWLockExclusive(&l), L"SRW lock released and usable again");
    ReleaseSRWLockExclusive(&l);

    CHECK(GetCurrentPackageFullName(&plen, NULL) == 15700, L"GetCurrentPackageFullName = no package");
    CHECK(FindStringOrdinal(0x00400000, L"Hello World", -1, L"world", -1, TRUE) == 6, L"FindStringOrdinal");
    GetCurrentThreadStackLimits(&lo, &hi);
    CHECK(lo < (ULONG_PTR)&lo && (ULONG_PTR)&lo < hi, L"GetCurrentThreadStackLimits");

    CHECK(InitializeSynchronizationBarrier(&g_barrier, 4, 0), L"InitializeSynchronizationBarrier");
    for (i = 0; i < 3; i++) th[i] = CreateThread(NULL, 0, barrier_thread, NULL, 0, NULL);
    Sleep(100);
    CHECK(g_through == 0, L"barrier holds threads until all arrive");
    EnterSynchronizationBarrier(&g_barrier, 0);
    WaitForMultipleObjects(3, th, TRUE, 5000);
    CHECK(g_through == 3, L"barrier releases all threads");
    DeleteSynchronizationBarrier(&g_barrier);

    CHECK(GetDpiForSystem() >= 96, L"GetDpiForSystem");
    CHECK(GetSystemMetricsForDpi(SM_CYCAPTION, 192) >= GetSystemMetrics(SM_CYCAPTION), L"GetSystemMetricsForDpi");

    CHECK(LoadLibraryExW(L"version.dll", NULL, 0x800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */) != NULL,
          L"LoadLibraryEx with LOAD_LIBRARY_SEARCH_SYSTEM32");

    /* GetProcAddress probe for a function the system kernel32 lacks */
    p = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AppPolicyGetProcessTerminationMethod");
    CHECK(p != NULL, L"GetProcAddress finds VistaExt additions via kernel32");

    /* a companion keeps the export numbers of the DLL it stands in for:
       programs import shell32 by number (Edge: ILFree #155, IsUserAnAdmin
       #680), and the companion must give them the same functions */
    {
        HMODULE sv = LoadLibraryW(L"shellvx.dll"), sh = GetModuleHandleW(L"shell32.dll");
        if (!sh) sh = LoadLibraryW(L"shell32.dll");
        CHECK(sv && sh &&
              GetProcAddress(sv, MAKEINTRESOURCEA(155)) == GetProcAddress(sh, "ILFree") &&
              GetProcAddress(sv, MAKEINTRESOURCEA(680)) == GetProcAddress(sh, "IsUserAnAdmin"),
              L"shellvx keeps shell32's export numbers (#155 ILFree, #680 IsUserAnAdmin)");
    }

    /* Windows 10's device-family query, looked up from ntdll the way
       Chromium-based programs do (Edge stops when it is missing) */
    {
        void (WINAPI *dfi)(ULONGLONG *, DWORD *, DWORD *) = (void (WINAPI *)(ULONGLONG *, DWORD *, DWORD *))
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetDeviceFamilyInfoEnum");
        ULONGLONG uap = 0;
        DWORD fam = 0, form = 99;
        if (dfi) dfi(&uap, &fam, &form);
        CHECK(dfi && fam == 3, L"RtlGetDeviceFamilyInfoEnum found via ntdll, reports a desktop");
    }

    /* What the program sees: GetVersionEx (VistaExt answers it for patched
     * programs) and the PEB fields RtlGetVersion reads on real Windows.
     * Wine's own RtlGetVersion ignores the PEB, so it is info only here. */
    {
        OSVERSIONINFOW rv;
        BYTE *peb = *(BYTE **)((BYTE *)NtCurrentTeb() + (sizeof(void *) == 8 ? 0x60 : 0x30));
        DWORD pmaj = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x118 : 0xA4));
        DWORD pmin = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x11C : 0xA8));
        DWORD pbld = *(USHORT *)(peb + (sizeof(void *) == 8 ? 0x120 : 0xAC));
        vi.dwOSVersionInfoSize = sizeof(vi);
        GetVersionExW(&vi);
        rv.dwOSVersionInfoSize = sizeof(rv);
        RtlGetVersion(&rv);
        con_print(L"  info  GetVersionEx %u.%u.%u \"%s\" | PEB %u.%u.%u | Wine RtlGetVersion %u.%u.%u\r\n",
                  vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, vi.szCSDVersion,
                  pmaj, pmin, pbld, rv.dwMajorVersion, rv.dwMinorVersion, rv.dwBuildNumber);
        con_print(L"  info  command line: %s\r\n", GetCommandLineW());
        {
            WCHAR wv[MAX_PATH] = L"(not set)";
            GetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", wv, MAX_PATH);
            con_print(L"  info  WEBVIEW2_BROWSER_EXECUTABLE_FOLDER=%s\r\n", wv);
        }
        if (argc > 1 && (!vx_wcsnicmp(argv[1], L"/expect:", 8) || !vx_wcsnicmp(argv[1], L"/expectall:", 11))) {
            DWORD maj = 0, min = 0, build = 0;
            BOOL all = argv[1][7] != ':';
            WCHAR *q = argv[1] + (all ? 11 : 8);
            OSVERSIONINFOEXW want;
            while (*q >= '0' && *q <= '9') maj = maj * 10 + (*q++ - '0');
            if (*q == '.') q++;
            while (*q >= '0' && *q <= '9') min = min * 10 + (*q++ - '0');
            if (*q == '.') q++;
            while (*q >= '0' && *q <= '9') build = build * 10 + (*q++ - '0');
            CHECK(vi.dwMajorVersion == maj && vi.dwMinorVersion == min && vi.dwBuildNumber == build,
                  L"GetVersionEx reports the chosen version");
            CHECK(pmaj == maj && pmin == min && pbld == build, L"PEB holds the chosen version (RtlGetVersion on Windows)");
            vx_memset(&want, 0, sizeof(want));
            want.dwOSVersionInfoSize = sizeof(want);
            want.dwMajorVersion = maj; want.dwMinorVersion = min;
            CHECK(VerifyVersionInfoW(&want, VER_MAJORVERSION | VER_MINORVERSION,
                  VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL)),
                  L"VerifyVersionInfo(>= chosen version) is TRUE");
            want.dwMinorVersion = min + 1;
            CHECK(!VerifyVersionInfoW(&want, VER_MAJORVERSION | VER_MINORVERSION,
                  VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL)),
                  L"VerifyVersionInfo(> chosen version) is FALSE");
            /* the file version of kernel32 (what Chromium checks), answered
               by versvx for programs with a version setting */
            {
                static BYTE fvi[8192];
                VS_FIXEDFILEINFO *f = NULL;
                UINT fl = 0;
                BOOL got = GetFileVersionInfoW(L"kernel32.dll", 0, sizeof(fvi), fvi) &&
                           VerQueryValueW(fvi, L"\\", (void **)&f, &fl) && f && fl >= sizeof(*f);
                if (got)
                    con_print(L"  info  kernel32 file version %u.%u.%u\r\n", HIWORD(f->dwFileVersionMS),
                              LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS));
                CHECK(got && HIWORD(f->dwFileVersionMS) == maj && LOWORD(f->dwFileVersionMS) == min &&
                      HIWORD(f->dwFileVersionLS) == build, L"kernel32's file version is the chosen version");
            }
            if (all) check_everywhere(maj, min, build);
        }
    }
    con_print(fails ? L"%d test(s) FAILED\r\n" : L"all tests passed\r\n", fails);
    return fails;
}

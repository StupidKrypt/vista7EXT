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

/* ---- behaviour checks --------------------------------------------------------
 * Each function is checked against what Microsoft documents for it. impl()
 * gives VistaExt's own implementation (NULL where this build forwards the
 * name to Windows), any() whichever one a program gets: Windows' own where
 * it has one, which then also confirms the expected answers. */
/* a DLL of VistaExt's: it carries the ownership marker (src/common/dll.rc,
 * RCDATA 100), which Windows' own DLLs of the same name (combase on 8) lack */
static BOOL vistaext_module(HMODULE m)
{
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(100), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(m, r) : NULL;
    const char *s = g ? (const char *)LockResource(g) : NULL;
    static const char mark[] = "\nVistaExt-owned-file";
    DWORD i;
    if (!s || SizeofResource(m, r) < sizeof(mark) - 1) return FALSE;
    for (i = 0; i < sizeof(mark) - 1; i++) if (s[i] != mark[i]) return FALSE;
    return TRUE;
}

static FARPROC impl(const WCHAR *dll, const char *name)
{
    HMODULE m = LoadLibraryW(dll);
    FARPROC p = m ? GetProcAddress(m, name) : NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !vistaext_module(m)) return NULL;
    if (!VirtualQuery((void *)p, &mbi, sizeof(mbi)) || mbi.AllocationBase != (void *)m) return NULL;
    return p;                                   /* code inside the companion: VistaExt's own */
}

static FARPROC any(const WCHAR *dll, const char *name)
{
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

static BOOL on_wine(void) { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL; }

/* apartments: thread A asks without COM, then joins the MTA and stays in
 * it until thread B (also in the MTA) has asked; so no thread ever uses an
 * apartment another one could end under it */
typedef HRESULT (WINAPI *rgai_fn)(UINT64 *);
static struct { rgai_fn f; UINT64 none_id, a_id, b_id; HRESULT none_hr, a_hr, b_hr; HANDLE a_in, b_done; } g_apt;

static DWORD WINAPI apt_a(LPVOID unused)
{
    g_apt.none_hr = g_apt.f(&g_apt.none_id);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_apt.a_hr = g_apt.f(&g_apt.a_id);
    SetEvent(g_apt.a_in);
    WaitForSingleObject(g_apt.b_done, 10000);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI apt_b(LPVOID unused)
{
    WaitForSingleObject(g_apt.a_in, 10000);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_apt.b_hr = g_apt.f(&g_apt.b_id);
    CoUninitialize();
    SetEvent(g_apt.b_done);
    return 0;
}

static volatile LONG g_power_calls;
static volatile ULONG g_power_type;
static ULONG CALLBACK power_cb(PVOID ctx, ULONG type, PVOID setting)
{
    g_power_type = type;
    InterlockedIncrement(&g_power_calls);
    return 0;
}

typedef struct { ULONG v; LONG status; ULONG64 opts; PVOID recs; PVOID res; } dns_res_t;
static HANDLE g_dns_ev, g_wh_ev;
static volatile LONG g_dns_done;
static volatile DWORD g_wh_status, g_wh_error;
static void WINAPI dns_cb(PVOID ctx, dns_res_t *r) { g_dns_done = 1; SetEvent(g_dns_ev); }
typedef void *HINTERNET_;
static void CALLBACK wh_cb(HINTERNET_ h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
    if (status == 0x00200000 && info) g_wh_error = ((DWORD *)info)[sizeof(DWORD_PTR) / sizeof(DWORD)];   /* WINHTTP_ASYNC_RESULT.dwError */
    g_wh_status = status;
    SetEvent(g_wh_ev);
}

static volatile LONG g_tp_fired;
static void CALLBACK tp_timer_cb(PTP_CALLBACK_INSTANCE i, PVOID c, PTP_TIMER t) { InterlockedIncrement(&g_tp_fired); }
static void CALLBACK tp_wait_cb(PTP_CALLBACK_INSTANCE i, PVOID c, PTP_WAIT w, TP_WAIT_RESULT r) { InterlockedIncrement(&g_tp_fired); }
static DWORD WINAPI wait_on(LPVOID ev) { WaitForSingleObject((HANDLE)ev, 20000); return 0; }

static DWORD CALLBACK device_cb(HANDLE n, PVOID ctx, int action, PVOID data, DWORD size) { return 0; }

static void behaviour_checks(void)
{
    WCHAR me[MAX_PATH], dir[MAX_PATH], plant[MAX_PATH], *slash = NULL, *q;
    FARPROC f;

    con_print(L"  info  behaviour checks (VistaExt's own code where it has it, else Windows')\r\n");
    GetModuleFileNameW(NULL, me, MAX_PATH);
    vx_wcscpyn(dir, me, MAX_PATH);
    for (q = dir; *q; q++) if (*q == '\\') slash = q;
    if (slash) slash[1] = 0;

    /* LOAD_LIBRARY_SEARCH_SYSTEM32: a DLL that is only in the program's
     * folder must not be found (no fallback to the normal search) */
    vx_wcscpyn(plant, dir, MAX_PATH);
    vx_wcscatn(plant, L"vxplanted.dll", MAX_PATH);
    if (CopyFileW(me, plant, FALSE)) {
        HMODULE h = LoadLibraryExW(L"vxplanted.dll", NULL, 0x800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
        CHECK(!h, L"LoadLibraryEx(SEARCH_SYSTEM32) does not load a same-named DLL from the program's folder");
        if (h) FreeLibrary(h);
        DeleteFileW(plant);
    } else con_print(L"  info  could not make the planted DLL (error %u)\r\n", GetLastError());

    /* SetProcessMitigationPolicy: only what this Windows can enforce */
    if ((f = impl(L"kernelvx.dll", "SetProcessMitigationPolicy"))) {
        BOOL (WINAPI *spmp)(int, PVOID, SIZE_T) = (BOOL (WINAPI *)(int, PVOID, SIZE_T))f;
        DWORD pol[2] = { 1, 0 }, none = 0;
        SetLastError(0);
        CHECK(!spmp(6 /* ProcessExtensionPointDisablePolicy */, &pol[0], sizeof(DWORD)) && GetLastError() == ERROR_NOT_SUPPORTED,
              L"SetProcessMitigationPolicy: a protection Vista/7 lack fails with ERROR_NOT_SUPPORTED");
        CHECK(spmp(6, &none, sizeof(none)), L"SetProcessMitigationPolicy: asking for nothing succeeds");
        CHECK(!spmp(6, NULL, 4) && GetLastError() == ERROR_INVALID_PARAMETER, L"SetProcessMitigationPolicy: no buffer is refused");
#ifdef _WIN64
        CHECK(spmp(0 /* ProcessDEPPolicy */, pol, 8), L"SetProcessMitigationPolicy: DEP on, as 64-bit always has it");
#endif
    }

    /* memory priority goes to Windows' page priority; other classes answer honestly */
    if ((f = any(L"kernel32.dll", "SetThreadInformation"))) {
        BOOL (WINAPI *sti)(HANDLE, int, PVOID, DWORD) = (BOOL (WINAPI *)(HANDLE, int, PVOID, DWORD))f;
        BOOL (WINAPI *gti)(HANDLE, int, PVOID, DWORD) = (BOOL (WINAPI *)(HANDLE, int, PVOID, DWORD))any(L"kernel32.dll", "GetThreadInformation");
        ULONG lo = 2, got = 0, normal = 5;
        BOOL set = sti(GetCurrentThread(), 0 /* ThreadMemoryPriority */, &lo, sizeof(lo));
        BOOL read = set && gti && gti(GetCurrentThread(), 0, &got, sizeof(got));
        if (!set && on_wine()) con_print(L"  info  Wine has no thread page priority (error %u)\r\n", GetLastError());
        else CHECK(set && read && got == 2, L"SetThreadInformation/GetThreadInformation: memory priority is applied and read back");
        sti(GetCurrentThread(), 0, &normal, sizeof(normal));
        SetLastError(0);
        {
            BOOL r = sti(GetCurrentThread(), 0, &lo, 2);
            DWORD e = GetLastError();
            if (!impl(L"kernelvx.dll", "SetThreadInformation"))
                con_print(L"  info  Windows' own SetThreadInformation with a wrong size: %d, error %u\r\n", r, e);
            else {
                CHECK(!r && e == ERROR_INVALID_PARAMETER, L"SetThreadInformation: wrong size refused");
                CHECK(!sti(GetCurrentThread(), 4, &lo, sizeof(lo)) && GetLastError() == ERROR_INVALID_PARAMETER,
                      L"SetThreadInformation: unknown class refused");
            }
        }
    }

    /* power requests: counted per request and type */
    if ((f = impl(L"kernelvx.dll", "PowerCreateRequest"))) {
        HANDLE (WINAPI *pcr)(PVOID) = (HANDLE (WINAPI *)(PVOID))f;
        BOOL (WINAPI *psr)(HANDLE, int) = (BOOL (WINAPI *)(HANDLE, int))impl(L"kernelvx.dll", "PowerSetRequest");
        BOOL (WINAPI *pcl)(HANDLE, int) = (BOOL (WINAPI *)(HANDLE, int))impl(L"kernelvx.dll", "PowerClearRequest");
        struct { ULONG ver; DWORD flags; PWSTR s; } rc = { 0, 1 /* POWER_REQUEST_CONTEXT_SIMPLE_STRING */, L"VistaExt test" };
        HANDLE r = pcr(&rc);
        BOOL ok = r != INVALID_HANDLE_VALUE && psr && pcl;
        CHECK(ok, L"PowerCreateRequest");
        if (ok) {
            CHECK(psr(r, 0) && psr(r, 1), L"PowerSetRequest: display and system");
            CHECK(pcl(r, 0), L"PowerClearRequest: display");
            CHECK(!pcl(r, 0) && GetLastError() == ERROR_INVALID_PARAMETER, L"PowerClearRequest: a type not set is refused");
            CHECK(pcl(r, 1), L"PowerClearRequest: system (the other request was still held)");
            CHECK(!psr(r, 9) && GetLastError() == ERROR_INVALID_PARAMETER, L"PowerSetRequest: unknown type refused");
            CHECK(!psr((HANDLE)(ULONG_PTR)0x1234, 0) && GetLastError() == ERROR_INVALID_HANDLE, L"PowerSetRequest: not a request handle");
            CloseHandle(r);
        }
    }

    /* ResolveLocaleName: a language alone gets its default locale */
    if ((f = any(L"kernel32.dll", "ResolveLocaleName"))) {
        int (WINAPI *rln)(LPCWSTR, LPWSTR, int) = (int (WINAPI *)(LPCWSTR, LPWSTR, int))f;
        WCHAR o[LOCALE_NAME_MAX_LENGTH];
        o[0] = 0; rln(L"en", o, LOCALE_NAME_MAX_LENGTH);
        con_print(L"  info  ResolveLocaleName en -> %s\r\n", o);
        CHECK(!vx_wcsicmp(o, L"en-US"), L"ResolveLocaleName(\"en\") is en-US");
        o[0] = 0; rln(L"de", o, LOCALE_NAME_MAX_LENGTH);
        CHECK(!vx_wcsicmp(o, L"de-DE"), L"ResolveLocaleName(\"de\") is de-DE");
        o[0] = 0; rln(L"de-AT", o, LOCALE_NAME_MAX_LENGTH);
        CHECK(!vx_wcsicmp(o, L"de-AT"), L"ResolveLocaleName(\"de-AT\") stays de-AT");
        o[0] = 'x';
        CHECK(rln(L"", o, LOCALE_NAME_MAX_LENGTH) == 1 && !o[0], L"ResolveLocaleName(\"\") is the invariant locale");
    }

    /* storage geometry and alignment from the file's real volume */
    {
        HANDLE h = CreateFileW(me, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        ULONG si[7], al = 99;
        if (h != INVALID_HANDLE_VALUE) {
            BOOL a = GetFileInformationByHandleEx(h, (FILE_INFO_BY_HANDLE_CLASS)16 /* FileStorageInfo */, si, sizeof(si));
            DWORD ea = GetLastError();
            BOOL b = GetFileInformationByHandleEx(h, (FILE_INFO_BY_HANDLE_CLASS)17 /* FileAlignmentInfo */, &al, sizeof(al));
            DWORD eb = GetLastError();
            if (a) con_print(L"  info  FileStorageInfo: logical %u, physical %u, offset %u\r\n", si[0], si[1], si[5]);
            else con_print(L"  info  FileStorageInfo: error %u\r\n", ea);
            if (!b) con_print(L"  info  FileAlignmentInfo: error %u\r\n", eb);
            CHECK(a && si[0] >= 512 && !(si[0] & (si[0] - 1)) && si[1] >= si[0] && !(si[1] & (si[1] - 1)),
                  L"GetFileInformationByHandleEx(FileStorageInfo): real sector sizes");
            if (!b && on_wine() && eb == ERROR_INVALID_FUNCTION) con_print(L"  info  Wine has no FileAlignmentInformation\r\n");
            else CHECK(b && al < 4096, L"GetFileInformationByHandleEx(FileAlignmentInfo)");
            CloseHandle(h);
        }
    }

    /* PathAllocCanonicalize's flags */
    /* (the PATHCCH_ENSURE_* and CANONICALIZE_SLASHES flags are Windows 10's:
     * Windows 8's own function is only reported) */
    if ((f = any(L"kernelbase.dll", "PathAllocCanonicalize")) && !impl(L"kernelvx.dll", "PathAllocCanonicalize")) {
        HRESULT (WINAPI *pac)(PCWSTR, ULONG, PWSTR *) = (HRESULT (WINAPI *)(PCWSTR, ULONG, PWSTR *))f;
        PWSTR o = NULL;
        HRESULT hr = pac(L"C:\\a\\..\\b", 0x20, &o);
        con_print(L"  info  Windows' own PathAllocCanonicalize(ENSURE_TRAILING_SLASH): 0x%08x %s\r\n", (UINT)hr, o ? o : L"-");
        LocalFree(o); o = NULL;
        hr = pac(L"C:\\a", 0x1000, &o);
        con_print(L"  info  Windows' own PathAllocCanonicalize(unknown flag): 0x%08x\r\n", (UINT)hr);
        LocalFree(o);
    }
    if ((f = impl(L"kernelvx.dll", "PathAllocCanonicalize"))) {
        HRESULT (WINAPI *pac)(PCWSTR, ULONG, PWSTR *) = (HRESULT (WINAPI *)(PCWSTR, ULONG, PWSTR *))f;
        PWSTR o = NULL;
        HRESULT hr;
        hr = pac(L"C:\\a\\..\\b", 0x20 /* ENSURE_TRAILING_SLASH */, &o);
        CHECK(SUCCEEDED(hr) && o && !vx_wcsicmp(o, L"C:\\b\\"), L"PathAllocCanonicalize: ENSURE_TRAILING_SLASH");
        LocalFree(o); o = NULL;
        hr = pac(L"C:\\a\\.\\b", 0x10 /* ENSURE_IS_EXTENDED_LENGTH_PATH */, &o);
        CHECK(SUCCEEDED(hr) && o && !vx_wcsicmp(o, L"\\\\?\\C:\\a\\b"), L"PathAllocCanonicalize: ENSURE_IS_EXTENDED_LENGTH_PATH");
        LocalFree(o); o = NULL;
        hr = pac(L"\\\\srv\\share\\x", 0x10, &o);
        CHECK(SUCCEEDED(hr) && o && !vx_wcsicmp(o, L"\\\\?\\UNC\\srv\\share\\x"), L"PathAllocCanonicalize: extended form of a UNC path");
        LocalFree(o); o = NULL;
        hr = pac(L"C:/a/b/../c", 0x40 /* CANONICALIZE_SLASHES */, &o);
        CHECK(SUCCEEDED(hr) && o && !vx_wcsicmp(o, L"C:\\a\\c"), L"PathAllocCanonicalize: CANONICALIZE_SLASHES");
        LocalFree(o); o = NULL;
        CHECK(pac(L"C:\\a", 0x1000, &o) == E_INVALIDARG && !o, L"PathAllocCanonicalize: unknown flag refused");
    }

    /* screen-capture exclusion: Vista cannot, so it must not say it did */
    if ((f = impl(L"uservx.dll", "SetWindowDisplayAffinity"))) {
        BOOL (WINAPI *swda)(HWND, DWORD) = (BOOL (WINAPI *)(HWND, DWORD))f;
        HWND w = CreateWindowExW(0, L"STATIC", L"vxtest", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
        CHECK(w && !swda(w, 1 /* WDA_MONITOR */) && GetLastError() == ERROR_NOT_SUPPORTED,
              L"SetWindowDisplayAffinity(WDA_MONITOR) fails with ERROR_NOT_SUPPORTED");
        CHECK(w && swda(w, 0), L"SetWindowDisplayAffinity(WDA_NONE) succeeds");
        if (w) DestroyWindow(w);
    }

    /* apartments: threads in the MTA share one; a thread without COM has
     * none, unless the process has an MTA, which such a thread then belongs
     * to (Microsoft's "implicit MTA") */
    if ((g_apt.f = (rgai_fn)any(L"combase.dll", "RoGetApartmentIdentifier"))) {
        HANDLE ta, tb;
        g_apt.a_in = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_apt.b_done = CreateEventW(NULL, TRUE, FALSE, NULL);
        ta = CreateThread(NULL, 0, apt_a, NULL, 0, NULL);
        tb = CreateThread(NULL, 0, apt_b, NULL, 0, NULL);
        WaitForSingleObject(ta, 15000); WaitForSingleObject(tb, 15000);
        CloseHandle(ta); CloseHandle(tb); CloseHandle(g_apt.a_in); CloseHandle(g_apt.b_done);
        if (on_wine() || !impl(L"combase.dll", "RoGetApartmentIdentifier"))
            con_print(L"  info  Windows' (or Wine's) own combase answers here (without COM: 0x%08x)\r\n", (UINT)g_apt.none_hr);
        else CHECK(g_apt.none_hr == CO_E_NOTINITIALIZED || (g_apt.none_hr == S_OK && g_apt.none_id == g_apt.a_id),
                   L"RoGetApartmentIdentifier without COM: none, or the implicit MTA");
        con_print(L"  info  RoGetApartmentIdentifier: no COM 0x%08x/%08x%08x, A 0x%08x/%08x%08x, B 0x%08x/%08x%08x\r\n",
                  (UINT)g_apt.none_hr, (UINT)(g_apt.none_id >> 32), (UINT)g_apt.none_id,
                  (UINT)g_apt.a_hr, (UINT)(g_apt.a_id >> 32), (UINT)g_apt.a_id,
                  (UINT)g_apt.b_hr, (UINT)(g_apt.b_id >> 32), (UINT)g_apt.b_id);
        /* Windows 8's own refuses desktop programs (ERROR_API_UNAVAILABLE): only reported */
        if (impl(L"combase.dll", "RoGetApartmentIdentifier") || on_wine())
            CHECK(g_apt.a_hr == S_OK && g_apt.b_hr == S_OK && g_apt.a_id == g_apt.b_id,
                  L"RoGetApartmentIdentifier: two threads in the MTA share one apartment");
    }

    /* power-setting callbacks: Windows reports the current value right after
     * registering; unregistering twice fails */
    {
        /* VistaExt's (what programs get through the power-setting API set),
         * else Windows' own (Windows 8); Windows 7's own powrprof has the
         * function but refuses callbacks: only reported */
        DWORD (WINAPI *reg)(LPCGUID, DWORD, HANDLE, PVOID *) =
            (DWORD (WINAPI *)(LPCGUID, DWORD, HANDLE, PVOID *))impl(L"kernelvx.dll", "PowerSettingRegisterNotification");
        DWORD (WINAPI *unreg)(PVOID) = (DWORD (WINAPI *)(PVOID))impl(L"kernelvx.dll", "PowerSettingUnregisterNotification");
        if (reg && any(L"powrprof.dll", "PowerSettingRegisterNotification")) {
            DWORD (WINAPI *own)(LPCGUID, DWORD, HANDLE, PVOID *) =
                (DWORD (WINAPI *)(LPCGUID, DWORD, HANDLE, PVOID *))any(L"powrprof.dll", "PowerSettingRegisterNotification");
            DWORD (WINAPI *ownu)(PVOID) = (DWORD (WINAPI *)(PVOID))any(L"powrprof.dll", "PowerSettingUnregisterNotification");
            static const GUID g0 = { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
            struct { PVOID cb; PVOID ctx; } p0 = { (PVOID)power_cb, NULL };
            PVOID h0 = NULL;
            DWORD e0 = own(&g0, 2, (HANDLE)&p0, &h0);
            con_print(L"  info  Windows' own powrprof PowerSettingRegisterNotification (callback form): error %u\r\n", e0);
            if (!e0 && h0 && ownu) ownu(h0);
            Sleep(300);
            g_power_calls = 0;
        }
        if (!reg) reg = (DWORD (WINAPI *)(LPCGUID, DWORD, HANDLE, PVOID *))any(L"powrprof.dll", "PowerSettingRegisterNotification");
        if (!unreg) unreg = (DWORD (WINAPI *)(PVOID))any(L"powrprof.dll", "PowerSettingUnregisterNotification");
        if (reg && unreg) {
            static const GUID acdc = { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
            struct { PVOID cb; PVOID ctx; } p = { (PVOID)power_cb, NULL };
            PVOID h = NULL;
            DWORD e = reg(&acdc, 2 /* DEVICE_NOTIFY_CALLBACK */, (HANDLE)&p, &h), t;
            if (e) con_print(L"  info  PowerSettingRegisterNotification: error %u\r\n", e);
            CHECK(!e && h, L"PowerSettingRegisterNotification (callback form)");
            for (t = 0; !e && t < 50 && !g_power_calls; t++) Sleep(100);
            if (!g_power_calls && on_wine()) con_print(L"  info  Wine sends no power-setting notifications\r\n");
            else CHECK(g_power_calls > 0 && g_power_type == 0x8013, L"... and the current value comes right away (PBT_POWERSETTINGCHANGE)");
            CHECK(!e && unreg(h) == ERROR_SUCCESS, L"PowerSettingUnregisterNotification");
        }
    }
    if ((f = impl(L"uservx.dll", "RegisterSuspendResumeNotification"))) {
        HANDLE (WINAPI *rsr)(HANDLE, DWORD) = (HANDLE (WINAPI *)(HANDLE, DWORD))f;
        BOOL (WINAPI *ursr)(HANDLE) = (BOOL (WINAPI *)(HANDLE))impl(L"uservx.dll", "UnregisterSuspendResumeNotification");
        struct { PVOID cb; PVOID ctx; } p = { (PVOID)power_cb, NULL };
        HANDLE h = rsr((HANDLE)&p, 2 /* DEVICE_NOTIFY_CALLBACK */);
        CHECK(h != NULL, L"RegisterSuspendResumeNotification (callback form)");
        CHECK(h && ursr && ursr(h), L"UnregisterSuspendResumeNotification");
        CHECK(h && ursr && !ursr(h), L"UnregisterSuspendResumeNotification: a second time fails");
        CHECK(!rsr(NULL, 2) && GetLastError() == ERROR_INVALID_PARAMETER, L"RegisterSuspendResumeNotification: no callback refused");
    }

    /* device notifications */
    {
        typedef DWORD (WINAPI *cmr_fn)(PVOID, PVOID, PVOID, HANDLE *);
        typedef DWORD (WINAPI *cmu_fn)(HANDLE);
        cmr_fn cmr = (cmr_fn)impl(L"cfgmgrvx.dll", "CM_Register_Notification");
        cmu_fn cmu = (cmu_fn)impl(L"cfgmgrvx.dll", "CM_Unregister_Notification");
        BOOL ours = cmr != NULL;
        if (!cmr) { cmr = (cmr_fn)any(L"cfgmgr32.dll", "CM_Register_Notification"); cmu = (cmu_fn)any(L"cfgmgr32.dll", "CM_Unregister_Notification"); }
        if (cmr && cmu) {
            struct { DWORD cb, flags; int type; DWORD res; union { GUID g; HANDLE h; WCHAR id[200]; } u; } flt;
            HANDLE n = NULL;
            vx_memset(&flt, 0, sizeof(flt));
            flt.cb = sizeof(flt);
            flt.flags = 1;               /* CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES */
            CHECK(sizeof(flt) == 416, L"(the test's CM_NOTIFY_FILTER has Windows' size)");
            CHECK(cmr(&flt, NULL, (PVOID)device_cb, &n) == 0 && n, L"CM_Register_Notification: all device interfaces");
            CHECK(n && cmu(n) == 0, L"CM_Unregister_Notification");
            if (ours) {
                CHECK(cmu(n) == 0x1F, L"CM_Unregister_Notification: a second time fails (CR_INVALID_DATA)");
                flt.type = 2;            /* device instance */
                flt.flags = 2;
                CHECK(cmr(&flt, NULL, (PVOID)device_cb, &n) == 0x34, L"CM_Register_Notification: device instances are not on this Windows (CR_CALL_NOT_IMPLEMENTED)");
                flt.type = 0;
                flt.flags = 0;
                flt.cb = 100;
                CHECK(cmr(&flt, NULL, (PVOID)device_cb, &n) == 0x1F, L"CM_Register_Notification: wrong size refused");
            }
        }
    }

    /* GetHostNameW: the name gethostname gives, in UTF-16; too small a buffer is WSAEFAULT */
    {
        typedef int (WINAPI *ghnw_fn)(PWSTR, int);
        typedef int (WINAPI *ghn_fn)(char *, int);
        typedef int (WINAPI *wsas_fn)(WORD, void *);
        typedef int (WINAPI *wsae_fn)(void);
        ghnw_fn ghnw = (ghnw_fn)any(L"ws2_32.dll", "GetHostNameW");
        ghn_fn ghn = (ghn_fn)any(L"ws2_32.dll", "gethostname");
        wsas_fn wsas = (wsas_fn)any(L"ws2_32.dll", "WSAStartup");
        wsae_fn wsae = (wsae_fn)any(L"ws2_32.dll", "WSAGetLastError");
        static BYTE wsad[512];
        if (ghnw && ghn && wsas && wsae && !wsas(0x0202, wsad)) {
            WCHAR w[257], a2w[257];
            char a[257];
            int r = ghnw(w, 257);
            con_print(L"  info  GetHostNameW: %s\r\n", r ? L"(failed)" : w);
            CHECK(!r && !ghn(a, sizeof(a)) && MultiByteToWideChar(CP_ACP, 0, a, -1, a2w, 257) && !vx_wcsicmp(w, a2w),
                  L"GetHostNameW gives the name gethostname gives");
            CHECK(ghnw(w, 1) == -1 && wsae() == 10014 /* WSAEFAULT */, L"GetHostNameW: too small a buffer is WSAEFAULT");
        } else CHECK(ghnw != NULL, L"GetHostNameW is found through ws2_32");
    }

    /* SetThreadpoolTimerEx: TRUE only for a setting cancelled before it came due */
    {
        BOOL (WINAPI *ste)(PTP_TIMER, PFILETIME, DWORD, DWORD) =
            (BOOL (WINAPI *)(PTP_TIMER, PFILETIME, DWORD, DWORD))any(L"kernel32.dll", "SetThreadpoolTimerEx");
        PTP_TIMER t = CreateThreadpoolTimer(tp_timer_cb, NULL, NULL);
        CHECK(ste && t, L"SetThreadpoolTimerEx is found");
        if (ste && t) {
            LARGE_INTEGER d;
            FILETIME ft;
            CHECK(!ste(t, NULL, 0, 0), L"SetThreadpoolTimerEx: a timer never set was not cancelled (FALSE)");
            d.QuadPart = -100000000LL;                        /* in 10 s */
            ft.dwLowDateTime = d.LowPart; ft.dwHighDateTime = (DWORD)d.HighPart;
            SetThreadpoolTimer(t, &ft, 0, 0);
            CHECK(ste(t, NULL, 0, 0), L"SetThreadpoolTimerEx: stopping a timer due in 10 s cancels it (TRUE)");
            d.QuadPart = -10000LL;                            /* in 1 ms */
            ft.dwLowDateTime = d.LowPart; ft.dwHighDateTime = (DWORD)d.HighPart;
            g_tp_fired = 0;
            SetThreadpoolTimer(t, &ft, 0, 0);
            Sleep(300);
            CHECK(g_tp_fired == 1 && !ste(t, NULL, 0, 0), L"SetThreadpoolTimerEx: a timer that already fired was not cancelled (FALSE)");
            WaitForThreadpoolTimerCallbacks(t, TRUE);
            CloseThreadpoolTimer(t);
        }
    }

    /* SetThreadpoolWaitEx: a thread that has not ended can be proved cancelled */
    {
        BOOL (WINAPI *swe)(PTP_WAIT, HANDLE, PFILETIME, PVOID) =
            (BOOL (WINAPI *)(PTP_WAIT, HANDLE, PFILETIME, PVOID))any(L"kernel32.dll", "SetThreadpoolWaitEx");
        PTP_WAIT w = CreateThreadpoolWait(tp_wait_cb, NULL, NULL);
        HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL), th = CreateThread(NULL, 0, wait_on, stop, 0, NULL);
        CHECK(swe && w && th, L"SetThreadpoolWaitEx is found");
        if (swe && w && th) {
            SetThreadpoolWait(w, th, NULL);
            CHECK(swe(w, NULL, NULL, NULL), L"SetThreadpoolWaitEx: stopping a wait for a running thread cancels it (TRUE)");
            g_tp_fired = 0;
            SetThreadpoolWait(w, th, NULL);
            SetEvent(stop);
            WaitForSingleObject(th, 5000);
            Sleep(300);
            CHECK(g_tp_fired == 1 && !swe(w, NULL, NULL, NULL), L"SetThreadpoolWaitEx: a wait that already ended was not cancelled (FALSE)");
            WaitForThreadpoolWaitCallbacks(w, TRUE);
            CloseThreadpoolWait(w);
        }
        if (th) CloseHandle(th);
        CloseHandle(stop);
    }

    /* CONTEXT helpers (Windows 7 SP1; VistaExt's on Vista) */
    if ((f = any(L"kernel32.dll", "InitializeContext"))) {
        BOOL (WINAPI *ic)(PVOID, DWORD, PCONTEXT *, PDWORD) = (BOOL (WINAPI *)(PVOID, DWORD, PCONTEXT *, PDWORD))f;
        BOOL (WINAPI *cc)(PCONTEXT, DWORD, PCONTEXT) = (BOOL (WINAPI *)(PCONTEXT, DWORD, PCONTEXT))any(L"kernel32.dll", "CopyContext");
        PVOID (WINAPI *lx)(PCONTEXT, DWORD, PDWORD) = (PVOID (WINAPI *)(PCONTEXT, DWORD, PDWORD))any(L"kernel32.dll", "LocateXStateFeature");
        BOOL (WINAPI *ic2)(PVOID, DWORD, PCONTEXT *, PDWORD, ULONG64) =
            (BOOL (WINAPI *)(PVOID, DWORD, PCONTEXT *, PDWORD, ULONG64))any(L"kernel32.dll", "InitializeContext2");
        static BYTE b1[sizeof(CONTEXT) + 64], b2[sizeof(CONTEXT) + 64];
        DWORD len = 0, l2;
        PCONTEXT c1 = NULL, c2 = NULL;
        CHECK(!ic(NULL, CONTEXT_FULL, NULL, &len) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && len >= sizeof(CONTEXT),
              L"InitializeContext: the size it needs");
        l2 = sizeof(b1);
        CHECK(ic(b1, CONTEXT_FULL, &c1, &l2) && c1 && !((ULONG_PTR)c1 & (sizeof(void *) == 8 ? 15 : 3)) && c1->ContextFlags == CONTEXT_FULL,
              L"InitializeContext: an aligned CONTEXT in the buffer");
        l2 = sizeof(b2);
        if (c1 && ic(b2, CONTEXT_INTEGER, &c2, &l2) && cc) {
            c1->ContextFlags = CONTEXT_FULL;
            GetThreadContext(GetCurrentThread(), c1);
            {
                BOOL r = cc(c2, CONTEXT_FULL, c1);
                DWORD e = GetLastError();
                /* Windows 7's own accepts it (and copies what both hold): so must VistaExt's */
                con_print(L"  info  CopyContext with more flags than the destination has: %d, error %u\r\n", r, e);
                CHECK(r, L"CopyContext: more flags than the destination has are accepted, as Windows 7 does");
            }
            CHECK(cc(c2, CONTEXT_INTEGER, c1) &&
#ifdef _WIN64
                  c2->Rbx == c1->Rbx && c2->R12 == c1->R12,
#else
                  c2->Ebx == c1->Ebx && c2->Esi == c1->Esi,
#endif
                  L"CopyContext: the integer registers are copied");
        }
        if (lx && c1) CHECK(!lx(c1, 2, NULL), L"LocateXStateFeature: none without CONTEXT_XSTATE");
        if (impl(L"kernelvx.dll", "InitializeContext")) {
            len = sizeof(b1);
            CHECK(!ic(b1, CONTEXT_ALL | 0x40 /* CONTEXT_XSTATE */, &c1, &len) && GetLastError() == ERROR_NOT_SUPPORTED,
                  L"InitializeContext: no extended state on Vista (ERROR_NOT_SUPPORTED)");
        }
        if (ic2) {
            len = 0;
            CHECK(!ic2(NULL, CONTEXT_FULL, NULL, &len, 0) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && len >= sizeof(CONTEXT),
                  L"InitializeContext2: the size it needs");
        }
    }

    /* Windows 7 moved OpenProcessToken into kernel32: found there on Vista too */
    CHECK(any(L"kernel32.dll", "OpenProcessToken") != NULL, L"OpenProcessToken is found through kernel32");
    if ((f = impl(L"uservx.dll", "GetPointerPenInfoHistory"))) {
        UINT32 n = 0;
        CHECK(!((BOOL (WINAPI *)(UINT32, UINT32 *, PVOID))f)(1, &n, NULL) && GetLastError() == ERROR_INVALID_PARAMETER,
              L"GetPointerPenInfoHistory: no such pointer here");
    }
    {
        HMODULE fb = LoadLibraryW(L"api-ms-win-core-fibers-l1-1-2.dll");
        if (!fb && on_wine()) con_print(L"  info  (no VistaExt API-set files under Wine)\r\n");
        else CHECK(fb && GetProcAddress(fb, "FlsGetValue2"), L"api-ms-win-core-fibers-l1-1-2 has FlsGetValue2");
    }

    /* d3d12.dll: no Direct3D 12 here, said cleanly; Windows' export numbers */
    if ((f = impl(L"d3d12.dll", "D3D12CreateDevice"))) {
        HMODULE d = GetModuleHandleW(L"d3d12.dll");
        void *dev = (void *)1;
        static const GUID iid_dev = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
        HRESULT hr = ((HRESULT (WINAPI *)(IUnknown *, int, REFIID, void **))f)(NULL, 0xb000, &iid_dev, &dev);
        CHECK(hr == (HRESULT)0x887A0004 && !dev, L"D3D12CreateDevice: DXGI_ERROR_UNSUPPORTED, no device");
        CHECK(GetProcAddress(d, MAKEINTRESOURCEA(101)) == f && GetProcAddress(d, MAKEINTRESOURCEA(102)) == GetProcAddress(d, "D3D12GetDebugInterface"),
              L"d3d12.dll: #101 is D3D12CreateDevice, #102 D3D12GetDebugInterface");
    }

    /* CreateDXGIFactory2 = CreateDXGIFactory1 with flags 0 or DEBUG */
    {
        HRESULT (WINAPI *f2)(UINT, REFIID, void **) = (HRESULT (WINAPI *)(UINT, REFIID, void **))any(L"dxgi.dll", "CreateDXGIFactory2");
        HRESULT (WINAPI *f1)(REFIID, void **) = (HRESULT (WINAPI *)(REFIID, void **))any(L"dxgi.dll", "CreateDXGIFactory1");
        static const GUID iid_f1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
        CHECK(f2 != NULL, L"CreateDXGIFactory2 is found through dxgi");
        if (f2) {
            IUnknown *a = NULL, *b = NULL;
            HRESULT h1 = f1 ? f1(&iid_f1, (void **)&a) : E_NOTIMPL, h2 = f2(0, &iid_f1, (void **)&b);
            con_print(L"  info  CreateDXGIFactory1 0x%08x, CreateDXGIFactory2 0x%08x\r\n", (UINT)h1, (UINT)h2);
            CHECK(h1 == h2 && (FAILED(h2) || b), L"CreateDXGIFactory2(0) answers as CreateDXGIFactory1");
            if (a) a->lpVtbl->Release(a);
            if (b) b->lpVtbl->Release(b);
            /* VistaExt's own answer: where it uses its own code (VX_NO_NATIVE=1);
             * otherwise it hands the call to Windows' own where there is one */
            if ((f = impl(L"dxvx.dll", "CreateDXGIFactory2")) && GetEnvironmentVariableW(L"VX_NO_NATIVE", NULL, 0)) {
                b = NULL;
                CHECK(((HRESULT (WINAPI *)(UINT, REFIID, void **))f)(2, &iid_f1, (void **)&b) == (HRESULT)0x887A0001 && !b,
                      L"CreateDXGIFactory2: an unknown flag is DXGI_ERROR_INVALID_CALL");
            }
        }
    }

    /* DnsQueryEx: the local machine's own name, synchronously and with a callback */
    {
        typedef struct { ULONG v; PCWSTR name; WORD type; ULONG64 opts; PVOID servers; ULONG iface; PVOID cb; PVOID ctx; } req_t;
        DWORD (WINAPI *dqe)(req_t *, dns_res_t *, PVOID) = (DWORD (WINAPI *)(req_t *, dns_res_t *, PVOID))any(L"dnsapi.dll", "DnsQueryEx");
        void (WINAPI *rlf)(PVOID, int) = (void (WINAPI *)(PVOID, int))any(L"dnsapi.dll", "DnsRecordListFree");
        CHECK(dqe != NULL, L"DnsQueryEx is found through dnsapi");
        if (dqe && rlf) {
            req_t q;
            dns_res_t r;
            static BYTE cancel[32];
            DWORD st;
            static WCHAR me[256];
            DWORD mn = 256;
            vx_memset(&q, 0, sizeof(q)); vx_memset(&r, 0, sizeof(r));
            /* this PC by name (Windows 8's own refuses a NULL name, which Microsoft documents as "this PC") */
            GetComputerNameExW(ComputerNameDnsHostname, me, &mn);
            q.v = 1; q.name = me; q.type = 1 /* A */; r.v = 1;
            st = dqe(&q, &r, NULL);
            con_print(L"  info  DnsQueryEx (this PC, A): %u\r\n", st);
            if (st && on_wine()) con_print(L"  info  (Wine's DNS answers differently)\r\n");
            else CHECK(st == 0 && r.status == 0 && r.recs, L"DnsQueryEx: the local machine's address, synchronously");
            if (r.recs) rlf(r.recs, 1);
            vx_memset(&r, 0, sizeof(r)); r.v = 1;
            q.cb = (PVOID)dns_cb;
            g_dns_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            st = dqe(&q, &r, cancel);
            if (st == 9506 /* DNS_REQUEST_PENDING */) WaitForSingleObject(g_dns_ev, 20000);
            if (!(st == 9506 || st == 0) && on_wine()) con_print(L"  info  (Wine's DNS answers differently: %u)\r\n", st);
            else CHECK((st == 9506 && g_dns_done && r.status == 0 && r.recs) || (st == 0 && r.recs),
                       L"DnsQueryEx: the same with a completion callback");
            if (r.recs) rlf(r.recs, 1);
            CloseHandle(g_dns_ev);
            q.v = 7; r.v = 1;
            CHECK(dqe(&q, &r, NULL) == ERROR_INVALID_PARAMETER, L"DnsQueryEx: a wrong version is ERROR_INVALID_PARAMETER");
        }
    }

    /* the WinHTTP proxy resolver: ERROR_IO_PENDING, then one of its two callbacks */
    {
        typedef HINTERNET_ (WINAPI *open_fn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
        typedef PVOID (WINAPI *ssc_fn)(HINTERNET_, PVOID, DWORD, DWORD_PTR);
        typedef DWORD (WINAPI *cpr_fn)(HINTERNET_, HINTERNET_ *);
        typedef DWORD (WINAPI *gpe_fn)(HINTERNET_, PCWSTR, PVOID, DWORD_PTR);
        typedef DWORD (WINAPI *gpr_fn)(HINTERNET_, PVOID);
        typedef void (WINAPI *fpr_fn)(PVOID);
        typedef BOOL (WINAPI *close_fn)(HINTERNET_);
        open_fn wopen = (open_fn)any(L"winhttp.dll", "WinHttpOpen");
        ssc_fn ssc = (ssc_fn)any(L"winhttp.dll", "WinHttpSetStatusCallback");
        cpr_fn cpr = (cpr_fn)any(L"winhttp.dll", "WinHttpCreateProxyResolver");
        gpe_fn gpe = (gpe_fn)any(L"winhttp.dll", "WinHttpGetProxyForUrlEx");
        gpr_fn gpr = (gpr_fn)any(L"winhttp.dll", "WinHttpGetProxyResult");
        fpr_fn fpr = (fpr_fn)any(L"winhttp.dll", "WinHttpFreeProxyResult");
        close_fn wclose = (close_fn)any(L"winhttp.dll", "WinHttpCloseHandle");
        CHECK(cpr && gpe && gpr && fpr, L"the WinHTTP proxy resolver is found through winhttp");
        if (wopen && ssc && cpr && gpe && gpr && fpr && wclose) {
            HINTERNET_ s = wopen(L"vxtest", 1 /* NO_PROXY */, NULL, NULL, 0x10000000 /* WINHTTP_FLAG_ASYNC */), res = NULL;
            struct { DWORD flags, autodetect; PCWSTR pac; PVOID r1; DWORD r2; BOOL logon; } opt = { 1 /* AUTO_DETECT */, 3 /* DHCP|DNS */, NULL, NULL, 0, TRUE };
            DWORD e;
            g_wh_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            ssc(s, (PVOID)wh_cb, 0x01000000 | 0x00200000 /* GETPROXYFORURL_COMPLETE | REQUEST_ERROR */, 0);
            CHECK(s && cpr(s, &res) == 0 && res, L"WinHttpCreateProxyResolver");
            e = res ? gpe(res, L"http://www.example.com/", &opt, 7) : 0;
            CHECK(e == ERROR_IO_PENDING, L"WinHttpGetProxyForUrlEx: ERROR_IO_PENDING");
            if (e == ERROR_IO_PENDING) {
                WaitForSingleObject(g_wh_ev, 60000);
                con_print(L"  info  proxy resolver callback 0x%08x, error %u\r\n", g_wh_status, g_wh_error);
                CHECK(g_wh_status == 0x01000000 || g_wh_status == 0x00200000, L"... and its callback comes (complete or error)");
                if (g_wh_status == 0x01000000) {
                    struct { DWORD n; PVOID e; } pr = { 0, NULL };
                    CHECK(gpr(res, &pr) == 0 && pr.n >= 1, L"WinHttpGetProxyResult");
                    fpr(&pr);
                }
            }
            if (res) wclose(res);
            if (s) wclose(s);
            CloseHandle(g_wh_ev);
        }
        if ((f = impl(L"winhtvx.dll", "WinHttpWebSocketCompleteUpgrade")))
            CHECK(!((HINTERNET_ (WINAPI *)(HINTERNET_, DWORD_PTR))f)(NULL, 0) && GetLastError() == ERROR_NOT_SUPPORTED,
                  L"WinHttpWebSocketCompleteUpgrade: no WinHTTP WebSockets here (ERROR_NOT_SUPPORTED)");
    }

    /* AppContainers: none before Windows 8 */
    if ((f = impl(L"uenvvx.dll", "DeriveAppContainerSidFromAppContainerName"))) {
        PSID sid = (PSID)1;
        HRESULT hr = ((HRESULT (WINAPI *)(PCWSTR, PSID *))f)(L"vxtest", &sid);
        CHECK(hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !sid, L"DeriveAppContainerSidFromAppContainerName: no AppContainers here");
    }

    /* the API set Edge imports RoFailFastWithErrorContext through */
    CHECK(any(L"api-ms-win-core-winrt-error-l1-1-0.dll", "RoFailFastWithErrorContext") != NULL,
          L"api-ms-win-core-winrt-error-l1-1-0 has RoFailFastWithErrorContext");
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

    /* /wait:<seconds>: only stay running (a program in use, for the
     * uninstall and switch-off tests); it still needs VistaExt to start */
    if (argc > 1 && !vx_wcsnicmp(argv[1], L"/wait:", 6)) {
        DWORD secs = 0;
        for (i = 6; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) secs = secs * 10 + (argv[1][i] - '0');
        GetSystemTimePreciseAsFileTime(&ft);
        Sleep(secs * 1000);
        return 0;
    }
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
    behaviour_checks();
    con_print(fails ? L"%d test(s) FAILED\r\n" : L"all tests passed\r\n", fails);
    return fails;
}

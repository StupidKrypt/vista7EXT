/* kernelvx.dll - kernel32 companion for Windows Vista.
 * Patched programs import kernelvx.dll instead of kernel32.dll. Everything
 * Vista already has is forwarded to kernel32 (see the generated .def); the
 * functions here are either missing on Vista or wrapped to fix behaviour.
 *
 * This file: process start-up (Windows version reporting, extra command line
 * arguments, WebView2 runtime), GetProcAddress fallback and DLL loading. */
#include "../common/vx.h"
#include "../common/companions.h"

static vx_layer g_layer;
static WCHAR *g_cmdW;
static char  *g_cmdA;
DWORD vx_tls_errmode = TLS_OUT_OF_INDEXES;

/* ---- Windows version reporting ------------------------------------------
 * GetVersion, GetVersionEx, VerifyVersionInfo and RtlGetVersion all read the
 * OS version from the PEB, so changing it there covers every caller in the
 * process (this is what Microsoft's own "version lie" shims do). */
#ifdef _WIN64
#define PEB_OFF_TEB   0x60
#define PEB_MAJOR     0x118
#define PEB_MINOR     0x11C
#define PEB_BUILD     0x120
#define PEB_CSDNUM    0x122
#define PEB_CSDSTR    0x2E8
#else
#define PEB_OFF_TEB   0x30
#define PEB_MAJOR     0xA4
#define PEB_MINOR     0xA8
#define PEB_BUILD     0xAC
#define PEB_CSDNUM    0xAE
#define PEB_CSDSTR    0x1F0
#endif

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } VX_USTR;

static BYTE *get_peb(void)
{
    return *(BYTE **)((BYTE *)NtCurrentTeb() + PEB_OFF_TEB);
}

static void apply_version(const vx_osver *v)
{
    BYTE *peb = get_peb();
    VX_USTR *csd = (VX_USTR *)(peb + PEB_CSDSTR);
    *(ULONG *)(peb + PEB_MAJOR) = v->major;
    *(ULONG *)(peb + PEB_MINOR) = v->minor;
    *(USHORT *)(peb + PEB_BUILD) = (USHORT)v->build;
    *(USHORT *)(peb + PEB_CSDNUM) = (USHORT)(v->spmajor << 8);
    csd->Buffer = (PWSTR)v->csd;
    csd->Length = (USHORT)(vx_wcslen(v->csd) * sizeof(WCHAR));
    csd->MaximumLength = csd->Length + sizeof(WCHAR);
}

/* The patched program's own version calls also come here, so the setting
 * holds even where a system reads the version from somewhere else (and
 * Windows 8.1+'s "manifest" cap on GetVersionEx does not apply). */
static void fix_vi(OSVERSIONINFOW *vi, BOOL wide)
{
    const vx_osver *v = g_layer.ver;
    vi->dwMajorVersion = v->major;
    vi->dwMinorVersion = v->minor;
    vi->dwBuildNumber = v->build;
    vi->dwPlatformId = VER_PLATFORM_WIN32_NT;
    if (wide) vx_wcscpyn(vi->szCSDVersion, v->csd, 128);
    else WideCharToMultiByte(CP_ACP, 0, v->csd, -1, ((OSVERSIONINFOA *)vi)->szCSDVersion, 128, NULL, NULL);
    if (vi->dwOSVersionInfoSize >= (wide ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOEXA))) {
        if (wide) { ((OSVERSIONINFOEXW *)vi)->wServicePackMajor = v->spmajor; ((OSVERSIONINFOEXW *)vi)->wServicePackMinor = 0; }
        else      { ((OSVERSIONINFOEXA *)vi)->wServicePackMajor = v->spmajor; ((OSVERSIONINFOEXA *)vi)->wServicePackMinor = 0; }
    }
}

VX_EXPORT BOOL WINAPI vx_GetVersionExW(LPOSVERSIONINFOW vi)
{
    if (!GetVersionExW(vi)) return FALSE;
    if (g_layer.ver) fix_vi(vi, TRUE);
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_GetVersionExA(LPOSVERSIONINFOA vi)
{
    if (!GetVersionExA(vi)) return FALSE;
    if (g_layer.ver) fix_vi((OSVERSIONINFOW *)vi, FALSE);
    return TRUE;
}

VX_EXPORT DWORD WINAPI vx_GetVersion(void)
{
    const vx_osver *v = g_layer.ver;
    if (!v) return GetVersion();
    return (v->build << 16) | (v->minor << 8) | v->major;
}

static int cmp_cond(ULONGLONG have, ULONGLONG want, BYTE cond)
{
    switch (cond) {
    case VER_EQUAL:         return have == want;
    case VER_GREATER:       return have > want;
    case VER_GREATER_EQUAL: return have >= want;
    case VER_LESS:          return have < want;
    case VER_LESS_EQUAL:    return have <= want;
    }
    return 0;
}

static BYTE cond_of(ULONGLONG mask, DWORD type)
{
    int bit = 0;
    while (!(type & 1)) { type >>= 1; bit++; }
    return (BYTE)((mask >> (bit * 3)) & 7);
}

/* Same rules as RtlVerifyVersionInfo: major, minor and service pack are
 * compared together as one version using the first condition given. */
VX_EXPORT BOOL WINAPI vx_VerifyVersionInfoW(LPOSVERSIONINFOEXW vi, DWORD type, DWORDLONG mask)
{
    const vx_osver *v = g_layer.ver;
    DWORD vtypes = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR | VER_BUILDNUMBER;
    DWORD rest = type & ~vtypes;
    if (!v || !(type & vtypes)) return VerifyVersionInfoW(vi, type, mask);
    if (type & (VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR)) {
        DWORD first = (type & VER_MAJORVERSION) ? VER_MAJORVERSION : (type & VER_MINORVERSION) ? VER_MINORVERSION :
                      (type & VER_SERVICEPACKMAJOR) ? VER_SERVICEPACKMAJOR : VER_SERVICEPACKMINOR;
        ULONGLONG have = 0, want = 0;
        if (type & VER_MAJORVERSION)     { have |= (ULONGLONG)v->major << 48;   want |= (ULONGLONG)vi->dwMajorVersion << 48; }
        if (type & VER_MINORVERSION)     { have |= (ULONGLONG)v->minor << 32;   want |= (ULONGLONG)vi->dwMinorVersion << 32; }
        if (type & VER_SERVICEPACKMAJOR) { have |= (ULONGLONG)v->spmajor << 16; want |= (ULONGLONG)vi->wServicePackMajor << 16; }
        if (type & VER_SERVICEPACKMINOR) { want |= vi->wServicePackMinor; }
        if (!cmp_cond(have, want, cond_of(mask, first))) { SetLastError(ERROR_OLD_WIN_VERSION); return FALSE; }
    }
    if ((type & VER_BUILDNUMBER) && !cmp_cond(v->build, vi->dwBuildNumber, cond_of(mask, VER_BUILDNUMBER))) {
        SetLastError(ERROR_OLD_WIN_VERSION);
        return FALSE;
    }
    return rest ? VerifyVersionInfoW(vi, rest, mask) : TRUE;
}

VX_EXPORT BOOL WINAPI vx_VerifyVersionInfoA(LPOSVERSIONINFOEXA vi, DWORD type, DWORDLONG mask)
{
    OSVERSIONINFOEXW w;
    if (!g_layer.ver) return VerifyVersionInfoA(vi, type, mask);
    vx_memset(&w, 0, sizeof(w));
    w.dwOSVersionInfoSize = sizeof(w);
    w.dwMajorVersion = vi->dwMajorVersion; w.dwMinorVersion = vi->dwMinorVersion;
    w.dwBuildNumber = vi->dwBuildNumber; w.dwPlatformId = vi->dwPlatformId;
    w.wServicePackMajor = vi->wServicePackMajor; w.wServicePackMinor = vi->wServicePackMinor;
    w.wSuiteMask = vi->wSuiteMask; w.wProductType = vi->wProductType;
    return vx_VerifyVersionInfoW(&w, type, mask);
}

/* ---- extra command line arguments ---------------------------------------
 * kernel32 caches the command line before we run, so the patched program
 * gets the extended line through these two wrappers. */
static void build_cmdline(void)
{
    const WCHAR *orig = GetCommandLineW();
    SIZE_T n = vx_wcslen(orig) + vx_wcslen(g_layer.args) + 2;
    int na;
    g_cmdW = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
    if (!g_cmdW) return;
    vx_wcscpyn(g_cmdW, orig, n);
    vx_wcscatn(g_cmdW, L" ", n);
    vx_wcscatn(g_cmdW, g_layer.args, n);
    na = WideCharToMultiByte(CP_ACP, 0, g_cmdW, -1, NULL, 0, NULL, NULL);
    g_cmdA = HeapAlloc(GetProcessHeap(), 0, na);
    if (g_cmdA) WideCharToMultiByte(CP_ACP, 0, g_cmdW, -1, g_cmdA, na, NULL, NULL);
}

static BOOL has_type_switch(const WCHAR *c)
{
    for (; *c; c++)
        if ((c[0] == ' ' || c[0] == '\t') && c[1] == '-' && c[2] == '-' && !vx_wcsnicmp(c + 3, L"type=", 5)) return TRUE;
    return FALSE;
}

VX_EXPORT LPWSTR WINAPI vx_GetCommandLineW(void) { return g_cmdW ? g_cmdW : GetCommandLineW(); }
VX_EXPORT LPSTR  WINAPI vx_GetCommandLineA(void) { return g_cmdA ? g_cmdA : GetCommandLineA(); }

/* ---- WebView2 ------------------------------------------------------------
 * If vx-webview2-setup registered a WebView2 runtime, point the WebView2
 * loader at it (it reads these environment variables) in every patched
 * program. The WEBVIEW2 setting itself only makes sure a program that needs
 * no other change still loads kernelvx, so that this runs. */
static void setup_webview2(void)
{
    HKEY k;
    WCHAR buf[MAX_PATH * 2];
    DWORD cb, type;
#ifdef _WIN64
    const WCHAR *folder_value = L"RuntimeFolder64";
#else
    const WCHAR *folder_value = L"RuntimeFolder32";
#endif
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_WEBVIEW2, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k))
        return;
    cb = sizeof(buf) - sizeof(WCHAR);
    if (!GetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", NULL, 0) &&
        !RegQueryValueExW(k, folder_value, NULL, &type, (BYTE *)buf, &cb) && type == REG_SZ) {
        buf[cb / sizeof(WCHAR)] = 0;
        SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", buf);
    }
    cb = sizeof(buf) - sizeof(WCHAR);
    if (!GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", NULL, 0) &&
        !RegQueryValueExW(k, L"BrowserArguments", NULL, &type, (BYTE *)buf, &cb) && type == REG_SZ) {
        buf[cb / sizeof(WCHAR)] = 0;
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", buf);
    }
    RegCloseKey(k);
}

/* ---- GetProcAddress fallback --------------------------------------------
 * Programs often probe for newer functions with
 * GetProcAddress(GetModuleHandle("kernel32"), "Name"). When the real DLL does
 * not have the function, look in the matching VistaExt companion. */
#define VX_W_(s) L##s
#define VX_W(s) VX_W_(s)
static const struct { const WCHAR *orig, *comp; } g_map[] = {
#define VX_COMPANION(dll, comp, numbers) { VX_W(#dll) L".dll", VX_W(#comp) L".dll" },
    VX_COMPANIONS
#undef VX_COMPANION
};

/* The companion for a module (FALSE if none). Programs call GetProcAddress
 * thousands of times at start-up, for hundreds of modules, so this asks the
 * loader nothing: the module's name comes from its own export directory,
 * which for these system DLLs is their file name. The companions themselves
 * are looked up once and never unloaded (they pin themselves). */
static struct {
    HMODULE comp; BYTE *lo, *hi; volatile LONG state;
    const char **impl; DWORD nimpl;     /* names the companion implements itself (not forwarders), sorted */
} g_comp[sizeof(g_map) / sizeof(g_map[0])];

/* The companion's own functions, from its export table: everything else it
 * exports only forwards to the system DLL, which GetProcAddress on that DLL
 * already found. The table's names are sorted, and so is this list. */
static void list_impl(unsigned m)
{
    BYTE *b = (BYTE *)g_comp[m].comp;
    IMAGE_NT_HEADERS *nh = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nh->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *ed;
    DWORD *names, *funcs, i, n = 0;
    WORD *ords;
    const char **list;
    if (!d->VirtualAddress) return;
    ed = (IMAGE_EXPORT_DIRECTORY *)(b + d->VirtualAddress);
    names = (DWORD *)(b + ed->AddressOfNames);
    funcs = (DWORD *)(b + ed->AddressOfFunctions);
    ords = (WORD *)(b + ed->AddressOfNameOrdinals);
    if (!(list = HeapAlloc(GetProcessHeap(), 0, (ed->NumberOfNames + 1) * sizeof(char *)))) return;
    for (i = 0; i < ed->NumberOfNames; i++) {
        DWORD rva = funcs[ords[i]];
        if (rva >= d->VirtualAddress && rva < d->VirtualAddress + d->Size) continue;   /* forwarder */
        list[n++] = (const char *)b + names[i];
    }
    g_comp[m].impl = list;
    g_comp[m].nimpl = n;
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static BOOL implemented(unsigned m, const char *name)
{
    DWORD lo = 0, hi = g_comp[m].nimpl;
    if (!g_comp[m].impl) return TRUE;                /* no list: ask the companion */
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        int r = str_cmp(name, g_comp[m].impl[mid]);
        if (!r) return TRUE;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return FALSE;
}

static int ascii_wicmp(const char *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = (WCHAR)(unsigned char)*a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 1;
        if (!x) return 0;
    }
}

/* the export directory's DLL name, or NULL; "known" = h is a loaded image */
static const char *export_name(HMODULE h, BOOL known)
{
    BYTE *base = (BYTE *)h;
    IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nh;
    IMAGE_DATA_DIRECTORY *dir;
    DWORD size;
    if ((ULONG_PTR)h & 0xFFFF) return NULL;          /* data-file handles and the like */
    if (!known) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(h, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE || mbi.AllocationBase != h) return NULL;
    }
    if (dh->e_magic != IMAGE_DOS_SIGNATURE || dh->e_lfanew <= 0 || dh->e_lfanew > 0x1000) return NULL;
    nh = (IMAGE_NT_HEADERS *)(base + dh->e_lfanew);
    if (nh->Signature != IMAGE_NT_SIGNATURE) return NULL;
    dir = &nh->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    size = nh->OptionalHeader.SizeOfImage;
    if (nh->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT || !dir->VirtualAddress ||
        dir->VirtualAddress + sizeof(IMAGE_EXPORT_DIRECTORY) > size) return NULL;
    {
        DWORD rva = ((IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress))->Name;
        return rva && rva < size - 16 ? (const char *)base + rva : NULL;
    }
}

static BOOL companion_of(HMODULE h, BOOL known, HMODULE *comp, BYTE **lo, BYTE **hi, unsigned *idx)
{
    const char *name = export_name(h, known);
    unsigned m;
    if (!name) return FALSE;
    for (m = 0; m < sizeof(g_map) / sizeof(g_map[0]); m++) {
        if (ascii_wicmp(name, g_map[m].orig)) continue;
        if (g_comp[m].state != 2) {
            HMODULE c = GetModuleHandleW(g_map[m].comp);
            if (!c) c = LoadLibraryW(g_map[m].comp);
            if (InterlockedCompareExchange(&g_comp[m].state, 1, 0) == 0) {
                if (c && c != h) {
                    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)c + ((IMAGE_DOS_HEADER *)c)->e_lfanew);
                    g_comp[m].comp = c;
                    g_comp[m].lo = (BYTE *)c;
                    g_comp[m].hi = (BYTE *)c + nt->OptionalHeader.SizeOfImage;
                    list_impl(m);
                }
                InterlockedExchange(&g_comp[m].state, 2);   /* the entry is complete */
            } else {
                while (g_comp[m].state != 2) SwitchToThread();
            }
        }
        if (!g_comp[m].comp || g_comp[m].comp == h) return FALSE;
        *comp = g_comp[m].comp; *lo = g_comp[m].lo; *hi = g_comp[m].hi; *idx = m;
        return TRUE;
    }
    return FALSE;
}

/* GetProcAddress for a system DLL that has a VistaExt companion:
 *  - a function the DLL lacks comes from the companion;
 *  - a function the companion implements itself, although the DLL has it too
 *    (the Windows version wrappers, LoadLibraryEx, InitializeCriticalSectionEx
 *    and the like), also comes from the companion, so a program that looks it
 *    up gets the same behaviour as one that imports it. */
VX_EXPORT FARPROC WINAPI vx_GetProcAddress(HMODULE h, LPCSTR name)
{
    DWORD err;
    FARPROC p = GetProcAddress(h, name), c;
    HMODULE comp;
    BYTE *lo, *hi;
    unsigned m;
    if (!h || !((ULONG_PTR)name >> 16)) return p;          /* by ordinal */
    err = GetLastError();
    if (!companion_of(h, p != NULL, &comp, &lo, &hi, &m)) { SetLastError(err); return p; }
    if (p && !implemented(m, name)) { SetLastError(err); return p; }   /* the usual case: nothing to change */
    c = GetProcAddress(comp, name);
    if (p) {
        if (c && (BYTE *)c >= lo && (BYTE *)c < hi) p = c;  /* implemented by VistaExt */
        SetLastError(err);
        return p;
    }
    if (vx_tracing) {
        char b[160] = "GetProcAddress ";
        const char *s2 = c ? ": from VistaExt" : ": NOT FOUND";
        SIZE_T n = 15, i;
        for (i = 0; name[i] && n < 120; i++) b[n++] = name[i];
        while (*s2) b[n++] = *s2++;
        b[n] = 0;
        vx_trace(b);
    }
    SetLastError(c ? 0 : err);
    return c;
}

/* ---- DLL search flags (Windows 7 KB2533623 / Windows 8) ------------------
 * Vista's LoadLibraryEx rejects the LOAD_LIBRARY_SEARCH_* flags with
 * ERROR_INVALID_PARAMETER, which breaks many modern programs at start-up. */
#define VX_SEARCH_DLL_LOAD_DIR   0x00000100
#define VX_SEARCH_SYSTEM32       0x00000800
#define VX_SEARCH_MASK           0x0000FF00
#define VX_REQUIRE_SIGNED        0x00000080

static DWORD g_default_dirs;

static BOOL has_path(LPCWSTR n)
{
    for (; *n; n++) if (*n == '\\' || *n == '/' || *n == ':') return TRUE;
    return FALSE;
}

VX_EXPORT HMODULE WINAPI vx_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    static int native = -1;     /* Windows 7 with KB2533623, or later */
    DWORD search, plain;
    if (native < 0) native = vx_native(K32, "AddDllDirectory") != NULL;
    if (native) return LoadLibraryExW(name, file, flags);
    search = flags & VX_SEARCH_MASK;
    plain = flags & ~(VX_SEARCH_MASK | VX_REQUIRE_SIGNED);
    if (!search && !(flags & VX_REQUIRE_SIGNED) && g_default_dirs && !(flags & LOAD_WITH_ALTERED_SEARCH_PATH))
        search = g_default_dirs;
    if (name && search) {
        if (search == VX_SEARCH_SYSTEM32 && !has_path(name)) {
            WCHAR path[MAX_PATH];
            UINT n = GetSystemDirectoryW(path, MAX_PATH);
            if (n && n + vx_wcslen(name) + 2 < MAX_PATH) {
                HMODULE h;
                vx_wcscatn(path, L"\\", MAX_PATH);
                vx_wcscatn(path, name, MAX_PATH);
                h = LoadLibraryExW(path, file, plain | LOAD_WITH_ALTERED_SEARCH_PATH);
                if (h) return h;
            }
        }
        if ((search & VX_SEARCH_DLL_LOAD_DIR) && has_path(name))
            plain |= LOAD_WITH_ALTERED_SEARCH_PATH;
    }
    return LoadLibraryExW(name, file, plain);
}

VX_EXPORT HMODULE WINAPI vx_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    WCHAR w[MAX_PATH];
    if (!name) return vx_LoadLibraryExW(NULL, file, flags);
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH)) return NULL;
    return vx_LoadLibraryExW(w, file, flags);
}

VX_EXPORT BOOL WINAPI vx_SetDefaultDllDirectories(DWORD flags)
{
    VX_NATIVE_UPDATE(K32, SetDefaultDllDirectories, BOOL, (DWORD), (flags));
    if (!(flags & VX_SEARCH_MASK)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    g_default_dirs = flags & VX_SEARCH_MASK;
    /* Removing the current directory from the search path is the main
     * security point of this API; SetDllDirectory("") does that on Vista. */
    SetDllDirectoryW(L"");
    return TRUE;
}

/* AddDllDirectory: emulated by prepending the folder to PATH, which the
 * standard search order includes. The cookie is a heap copy of the path. */
VX_EXPORT PVOID WINAPI vx_AddDllDirectory(PCWSTR dir)
{
    WCHAR *copy, *path;
    DWORD n, dl;
    VX_NATIVE_UPDATE(K32, AddDllDirectory, PVOID, (PCWSTR), (dir));
    if (!dir || !has_path(dir)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    dl = (DWORD)vx_wcslen(dir);
    copy = HeapAlloc(GetProcessHeap(), 0, (dl + 1) * sizeof(WCHAR));
    if (!copy) return NULL;
    vx_wcscpyn(copy, dir, dl + 1);
    n = GetEnvironmentVariableW(L"PATH", NULL, 0);
    path = HeapAlloc(GetProcessHeap(), 0, (n + dl + 2) * sizeof(WCHAR));
    if (path) {
        vx_wcscpyn(path, dir, dl + 1);
        path[dl] = ';';
        path[dl + 1] = 0;
        if (n) GetEnvironmentVariableW(L"PATH", path + dl + 1, n);
        SetEnvironmentVariableW(L"PATH", path);
        HeapFree(GetProcessHeap(), 0, path);
    }
    return copy;
}

VX_EXPORT BOOL WINAPI vx_RemoveDllDirectory(PVOID cookie)
{
    WCHAR *dir = cookie, *path, *p;
    DWORD n, dl;
    VX_NATIVE_UPDATE(K32, RemoveDllDirectory, BOOL, (PVOID), (cookie));
    if (!dir) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    dl = (DWORD)vx_wcslen(dir);
    n = GetEnvironmentVariableW(L"PATH", NULL, 0);
    path = n ? HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR)) : NULL;
    if (path && GetEnvironmentVariableW(L"PATH", path, n)) {
        for (p = path; *p; ) {
            WCHAR *e = p;
            while (*e && *e != ';') e++;
            if ((DWORD)(e - p) == dl && !vx_wcsnicmp(p, dir, dl)) {
                if (*e == ';') e++;
                memmove(p, e, (vx_wcslen(e) + 1) * sizeof(WCHAR));
                SetEnvironmentVariableW(L"PATH", path);
                break;
            }
            p = *e ? e + 1 : e;
        }
    }
    if (path) HeapFree(GetProcessHeap(), 0, path);
    HeapFree(GetProcessHeap(), 0, dir);
    return TRUE;
}

VX_EXPORT HMODULE WINAPI vx_LoadPackagedLibrary(LPCWSTR name, DWORD reserved)
{
    SetLastError(15700 /* APPMODEL_ERROR_NO_PACKAGE */);
    return NULL;
}

/* ---- start-up ----------------------------------------------------------- */
/* ---- crash window ---------------------------------------------------------
 * For a windowed program's main process, kernelvx starts vxwatch.exe, which
 * waits for the program to end. If it ends with a crash code, vxwatch shows
 * what happened and lets the user restart it told another Windows version.
 * The program records its last serious exception (and, for programs that
 * stop themselves on a failed check, the message they formatted) in a small
 * shared block; vxwatch gets that and a handle to the process by
 * inheritance, so it works even if the program dies at once. */
static vx_watch *g_watch;
static HANDLE g_watch_sec;

static void hex32(WCHAR *o, ULONG_PTR v)
{
    int i, n = sizeof(ULONG_PTR) * 2, lead = 1, k = 0;
    o[k++] = '0'; o[k++] = 'x';
    for (i = n - 1; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 15);
        if (d || !lead || i == 0) { o[k++] = L"0123456789abcdef"[d]; lead = 0; }
    }
    o[k] = 0;
}

static BOOL serious(DWORD c)
{
    switch (c) {
    case 0xC0000005: case 0x80000003: case 0xC000001D: case 0xC0000409: case 0xC0000374:
    case 0xC00000FD: case 0xC0000094: case 0xC0000096: case 0xC0000602: case 0xC0000417:
        return TRUE;
    }
    return FALSE;
}

/* The program's own last words: programs that stop themselves on a failed
 * check usually format the reason on the stack just before (Chromium's
 * "FATAL: Check failed", C/C++ "Assertion failed", Rust "panicked at",
 * .NET and Qt "Unhandled exception" / "fatal error", abort messages). The
 * first text near the stack pointer, ANSI or UTF-16, of at least 20
 * characters that contains one of these words is kept. */
static const char *const g_last_words[] = { "fatal", "check failed", "assert", "panic", "unhandled", "abort", "terminate" };

static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* run of n characters at s (step 1 = ANSI, 2 = UTF-16): does it contain a keyword? */
static BOOL has_last_words(const BYTE *s, ULONG_PTR n, int step)
{
    unsigned w;
    ULONG_PTR k, j;
    for (w = 0; w < sizeof(g_last_words) / sizeof(g_last_words[0]); w++) {
        const char *kw = g_last_words[w];
        ULONG_PTR kl = vx_strlen(kw);
        for (k = 0; k + kl <= n; k++) {
            for (j = 0; j < kl && low((char)s[(k + j) * step]) == kw[j]; j++) ;
            if (j == kl) return TRUE;
        }
    }
    return FALSE;
}

static BOOL printable(BYTE c) { return c >= 0x20 && c < 0x7f; }

static void stack_message(ULONG_PTR sp)
{
    ULONG_PTR top = (ULONG_PTR)((NT_TIB *)NtCurrentTeb())->StackBase, end = sp + 16384, i;
    const BYTE *m = (const BYTE *)sp;
    int step;
    if (!sp || sp >= top) return;
    if (end > top) end = top;
    for (step = 1; step <= 2; step++) {
        ULONG_PTR run = 0, start = 0, align;
        for (align = 0; align < (ULONG_PTR)step; align++) {
            run = 0;
            for (i = align; i + step <= end - sp; i += step) {
                BOOL ok = printable(m[i]) && (step == 1 || m[i + 1] == 0);
                if (ok) { if (!run) start = i; run++; continue; }
                if (run >= 20 && has_last_words(m + start, run, step)) {
                    ULONG_PTR n = run < 399 ? run : 399, q;
                    for (q = 0; q < n; q++) g_watch->message[q] = (WCHAR)m[start + q * step];
                    g_watch->message[n] = 0;
                    return;
                }
                run = 0;
            }
        }
    }
}

/* Was this process started by the same program (a helper process of a
 * multi-process program: browsers' renderers, Electron helpers, ...)? Then
 * its main process is the one users see and restart. */
LONG WINAPI NtQueryInformationProcess(HANDLE, int, PVOID, ULONG, PULONG);

static BOOL started_by_itself(const WCHAR *exe)
{
    struct { LONG ExitStatus; PVOID Peb; ULONG_PTR Affinity; LONG Prio; ULONG_PTR Pid, ParentPid; } pbi;
    WCHAR parent[MAX_PATH];
    DWORD n = MAX_PATH;
    HANDLE h;
    BOOL same = FALSE;
    if (NtQueryInformationProcess(GetCurrentProcess(), 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), NULL) < 0)
        return FALSE;
    if (!(h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, FALSE, (DWORD)pbi.ParentPid))) return FALSE;
    if (QueryFullProcessImageNameW(h, 0, parent, &n)) {
        GetLongPathNameW(parent, parent, MAX_PATH);
        same = !vx_wcsicmp(parent, exe);
    }
    CloseHandle(h);
    return same;
}

/* The module an address belongs to, without the loader's lock (an
 * exception can come while another thread holds it): the image mapping that
 * contains the address, and that mapping's file name. */
LONG WINAPI NtQueryVirtualMemory(HANDLE, PVOID, int, PVOID, SIZE_T, PSIZE_T);

static void where_of(void *a)
{
    static union { struct { USHORT len, max; WCHAR *buf; } us; BYTE raw[sizeof(void *) * 2 + 1040]; } nb;
    MEMORY_BASIC_INFORMATION mbi;
    WCHAR *w = g_watch->where, *b, *e;
    SIZE_T n;
    if (VirtualQuery(a, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && mbi.AllocationBase &&
        NtQueryVirtualMemory(GetCurrentProcess(), mbi.AllocationBase, 2 /* MemoryMappedFilenameInformation */,
                             &nb, sizeof(nb) - sizeof(WCHAR), NULL) >= 0 && nb.us.buf && nb.us.len) {
        e = nb.us.buf + nb.us.len / sizeof(WCHAR);
        for (b = e; b > nb.us.buf && b[-1] != '\\'; b--) ;
        for (n = 0; b < e && n < 100; ) w[n++] = *b++;
        w[n++] = '+';
        hex32(w + n, (ULONG_PTR)a - (ULONG_PTR)mbi.AllocationBase);
    } else hex32(w, (ULONG_PTR)a);
}

/* Called for every exception in the program, also for ones it handles
 * itself (Java, .NET and WebAssembly use access violations on purpose), so
 * it does very little: no loader calls, little stack (it may run on the
 * last page of an overflowed stack), and one thread at a time. */
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG busy;
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    ULONG_PTR sp;
    if (!g_watch || !serious(c) || InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_watch->code = (LONG)c;
    where_of(ep->ExceptionRecord->ExceptionAddress);
#ifdef _WIN64
    sp = ep->ContextRecord->Rsp;
#else
    sp = ep->ContextRecord->Esp;
#endif
    if (c == 0x80000003 || c == 0xC000001D || c == 0xC0000409) stack_message(sp);
    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void put_dec(WCHAR *o, ULONG_PTR v)
{
    WCHAR t[24];
    int n = 0, k = 0;
    do { t[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = t[--n];
    o[k] = 0;
}

static DWORD WINAPI watch_launch(LPVOID unused)
{
    WCHAR path[MAX_PATH + 20], cmd[MAX_PATH + 80];
    HANDLE self = GetCurrentProcess(), hp = NULL, hs = NULL, list[2];
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T sz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attr = NULL;
    BOOL ok = FALSE;
    GetSystemDirectoryW(path, MAX_PATH);
    vx_wcscatn(path, L"\\vxwatch.exe", MAX_PATH + 20);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    if (!DuplicateHandle(self, self, self, &hp, SYNCHRONIZE | PROCESS_QUERY_INFORMATION, TRUE, 0) ||
        !DuplicateHandle(self, g_watch_sec, self, &hs, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
    /* only these two handles go to vxwatch, none of the program's own */
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    if (!(attr = HeapAlloc(GetProcessHeap(), 0, sz)) || !InitializeProcThreadAttributeList(attr, 1, 0, &sz)) goto done;
    list[0] = hp; list[1] = hs;
    if (!UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(list), NULL, NULL)) goto done;
    vx_memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr;
    cmd[0] = '"';
    vx_wcscpyn(cmd + 1, path, MAX_PATH + 20);
    vx_wcscatn(cmd, L"\" ", MAX_PATH + 80);
    put_dec(cmd + vx_wcslen(cmd), (ULONG_PTR)hp);
    vx_wcscatn(cmd, L" ", MAX_PATH + 80);
    put_dec(cmd + vx_wcslen(cmd), (ULONG_PTR)hs);
    /* outside the program's job if allowed, so that it survives the program */
    ok = CreateProcessW(path, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_BREAKAWAY_FROM_JOB,
                        NULL, NULL, &si.StartupInfo, &pi) ||
         CreateProcessW(path, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
done:
    if (attr) { DeleteProcThreadAttributeList(attr); HeapFree(GetProcessHeap(), 0, attr); }
    if (hp) CloseHandle(hp);
    if (hs) CloseHandle(hs);
    return 0;
}

static BOOL crash_window_off(const WCHAR *exe)
{
    HKEY k;
    BOOL off = FALSE;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, VX_REG_NOCRASHWND, 0, KEY_QUERY_VALUE, &k)) {
        off = !RegQueryValueExW(k, exe, NULL, NULL, NULL, NULL);
        RegCloseKey(k);
    }
    return off;
}

static void start_watch(const WCHAR *exe)
{
    IMAGE_NT_HEADERS *nh;
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    const WCHAR *c = GetCommandLineW();
    HANDLE th;
    if (GetEnvironmentVariableW(L"VX_NO_WATCH", NULL, 0) || !base) return;
    nh = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nh->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI) return;   /* windowed programs only */
    /* helper processes of multi-process programs: the main process has the window */
    if (started_by_itself(exe)) return;
    if (crash_window_off(exe)) return;
    if (!(g_watch_sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(vx_watch), NULL))) return;
    if (!(g_watch = MapViewOfFile(g_watch_sec, FILE_MAP_WRITE, 0, 0, sizeof(vx_watch)))) return;
    g_watch->magic = VX_WATCH_MAGIC;
    vx_wcscpyn(g_watch->exe, exe, MAX_PATH);
    vx_wcscpyn(g_watch->cmdline, c, 4096);
    GetCurrentDirectoryW(MAX_PATH, g_watch->cwd);
    AddVectoredExceptionHandler(0, watch_veh);
    /* VistaExt's window replaces Windows Error Reporting's "has stopped
     * working" box, which would otherwise come first and keep the program
     * alive until it is closed */
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    /* the thread starts once the loader is done (no process start under the loader lock) */
    if ((th = CreateThread(NULL, 0, watch_launch, NULL, 0, NULL))) CloseHandle(th);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR exe[MAX_PATH];
        DisableThreadLibraryCalls(inst);
        vx_pin((void *)DllMainCRTStartup);
        vx_tls_errmode = TlsAlloc();
        if (vx_tracing < 0) vx_tracing = GetEnvironmentVariableW(L"VX_TRACE", NULL, 0) != 0;
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        /* Settings are saved under the long path; a program started through
         * a short 8.3 path (C:\PROGRA~1\...) must still find them. */
        if (n && n < MAX_PATH) GetLongPathNameW(exe, exe, MAX_PATH);
        if (n && n < MAX_PATH) start_watch(exe);
        if (n && n < MAX_PATH && vx_effective_layer(exe, &g_layer)) {
            if (g_layer.ver) apply_version(g_layer.ver);
            /* Chromium/Electron helper processes (same program, "--type=...")
             * get their switches from the main process, and some of them
             * (the crash reporter) refuse options they do not know */
            if (g_layer.args[0] && !has_type_switch(GetCommandLineW())) build_cmdline();
        }
        setup_webview2();
    }
    return TRUE;
}

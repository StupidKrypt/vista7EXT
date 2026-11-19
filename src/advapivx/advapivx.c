/* advapivx.dll - advapi32 companion for Windows Vista (event tracing, 7/8). */
#include "../common/vx.h"
#include <evntprov.h>

VX_EXPORT ULONG WINAPI vx_EventSetInformation(REGHANDLE h, int cls, PVOID info, ULONG len)
{ VX_NATIVE(L"advapi32.dll", EventSetInformation, ULONG, (REGHANDLE h, int cls, PVOID info, ULONG len), (h, cls, info, len));
    return ERROR_NOT_SUPPORTED;   /* provider traits etc.: callers ignore this */
}

VX_EXPORT ULONG WINAPI vx_EventWriteTransfer(REGHANDLE h, PCEVENT_DESCRIPTOR d, LPCGUID act, LPCGUID rel,
        ULONG n, PEVENT_DATA_DESCRIPTOR data)
{ VX_NATIVE(L"advapi32.dll", EventWriteTransfer, ULONG, (REGHANDLE h, PCEVENT_DESCRIPTOR d, LPCGUID act, LPCGUID rel,         ULONG n, PEVENT_DATA_DESCRIPTOR data), (h, d, act, rel, n, data));
    return EventWrite(h, d, n, data);
}

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT ULONG WINAPI vx_EventWriteEx(REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG64 filter, ULONG flags,
        LPCGUID act, LPCGUID rel, ULONG n, PEVENT_DATA_DESCRIPTOR data)
{ VX_NATIVE(L"advapi32.dll", EventWriteEx, ULONG, (REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG64 filter, ULONG flags,         LPCGUID act, LPCGUID rel, ULONG n, PEVENT_DATA_DESCRIPTOR data), (h, d, filter, flags, act, rel, n, data));
    return EventWrite(h, d, n, data);
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT ULONG WINAPI vx_TraceSetInformation(ULONG64 h, int cls, PVOID info, ULONG len) { VX_NATIVE(L"advapi32.dll", TraceSetInformation, ULONG, (ULONG64 h, int cls, PVOID info, ULONG len), (h, cls, info, len)); return ERROR_NOT_SUPPORTED; }
#endif
VX_EXPORT ULONG WINAPI vx_TraceQueryInformation(ULONG64 h, int cls, PVOID info, ULONG len, PULONG ret) { VX_NATIVE(L"advapi32.dll", TraceQueryInformation, ULONG, (ULONG64 h, int cls, PVOID info, ULONG len, PULONG ret), (h, cls, info, len, ret)); return ERROR_NOT_SUPPORTED; }

/* ---- drivers that start with Windows -------------------------------------------
 * A setup told another Windows version (VistaExt's setting) may pick files
 * made for that Windows. For a driver Windows loads while it starts (boot or
 * system start), that could keep this Windows from starting at all. So a
 * program told another version is not allowed to create one, or to set one
 * to start that way; the call fails with "access denied", which setups
 * report as a failed install. The program is then set to see this Windows'
 * real version (for this user), so running it again gives it the truth. */
static BOOL version_told(WCHAR *label, SIZE_T cch)
{
    typedef void (NTAPI *fn)(DWORD *, DWORD *, DWORD *);
    static fn real;
    DWORD rmaj = 0, rmin = 0, rbld = 0, maj, min, bld;
    BYTE *peb = *(BYTE **)((BYTE *)NtCurrentTeb() + (sizeof(void *) == 8 ? 0x60 : 0x30));
    if (!real) real = (fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
    if (!real) return FALSE;
    real(&rmaj, &rmin, &rbld);
    maj = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x118 : 0xA4));
    min = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x11C : 0xA8));
    bld = *(USHORT *)(peb + (sizeof(void *) == 8 ? 0x120 : 0xAC));
    if (maj == rmaj && min == rmin && bld == (rbld & 0xFFFF)) return FALSE;
    {   /* "10.0.19045" without wsprintf (no user32 here) */
        DWORD v[3] = { maj, min, bld };
        int i;
        label[0] = 0;
        for (i = 0; i < 3; i++) {
            WCHAR t[12], one[2] = { 0, 0 };
            int n = 0;
            DWORD x = v[i];
            do { t[n++] = (WCHAR)('0' + x % 10); x /= 10; } while (x);
            while (n) { one[0] = t[--n]; vx_wcscatn(label, one, cch); }
            if (i < 2) vx_wcscatn(label, L".", cch);
        }
    }
    return TRUE;
}

static BOOL boot_driver(DWORD type, DWORD start)
{
    return type != SERVICE_NO_CHANGE && (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) &&
           (start == SERVICE_BOOT_START || start == SERVICE_SYSTEM_START);
}

static BOOL refuse_driver(const WCHAR *service)
{
    WCHAR ver[40], exe[MAX_PATH], m[1200];
    HKEY k;
    if (!version_told(ver, 40)) return FALSE;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    GetLongPathNameW(exe, exe, MAX_PATH);
    VX_TRACE("refused: a driver that starts with Windows, from a program told another version");
    OutputDebugStringW(L"VistaExt: refused to install a driver that starts with Windows (the program is told another Windows version)\n");
    /* the next run sees the real version */
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, VX_REG_LAYERS, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
        RegSetValueExW(k, exe, 0, REG_SZ, (const BYTE *)L"REAL", sizeof(L"REAL"));
        RegCloseKey(k);
    }
    {
        HMODULE u = LoadLibraryW(L"user32.dll");
        int (WINAPI *mb)(HWND, LPCWSTR, LPCWSTR, UINT) =
            u ? (int (WINAPI *)(HWND, LPCWSTR, LPCWSTR, UINT))(void *)GetProcAddress(u, "MessageBoxW") : NULL;
        m[0] = 0;
        vx_wcscatn(m, L"VistaExt stopped this setup from installing a driver that Windows loads while it starts (\"", 1200);
        vx_wcscatn(m, service ? service : L"?", 1200);
        vx_wcscatn(m, L"\").\n\nThe setup was told it runs on Windows ", 1200);
        vx_wcscatn(m, ver, 1200);
        vx_wcscatn(m, L", and a driver made for that Windows could keep this one from starting.\n\n"
                      L"Run the setup again: it now sees this Windows' real version. If it then refuses this Windows, "
                      L"the program is not made for it.\n\n", 1200);
        vx_wcscatn(m, exe, 1200);
        if (mb && !GetEnvironmentVariableW(L"VX_NO_DIALOGS", NULL, 0)) mb(NULL, m, L"VistaExt", 0x30 | 0x10000 /* MB_ICONWARNING | MB_SETFOREGROUND */);
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return TRUE;
}

VX_EXPORT SC_HANDLE WINAPI vx_CreateServiceW(SC_HANDLE m, LPCWSTR name, LPCWSTR disp, DWORD access, DWORD type, DWORD start,
                                             DWORD err, LPCWSTR path, LPCWSTR group, LPDWORD tag, LPCWSTR deps,
                                             LPCWSTR user, LPCWSTR pw)
{
    if (boot_driver(type, start) && refuse_driver(name)) return NULL;
    return CreateServiceW(m, name, disp, access, type, start, err, path, group, tag, deps, user, pw);
}

VX_EXPORT SC_HANDLE WINAPI vx_CreateServiceA(SC_HANDLE m, LPCSTR name, LPCSTR disp, DWORD access, DWORD type, DWORD start,
                                             DWORD err, LPCSTR path, LPCSTR group, LPDWORD tag, LPCSTR deps,
                                             LPCSTR user, LPCSTR pw)
{
    if (boot_driver(type, start)) {
        WCHAR w[256] = L"";
        if (name) MultiByteToWideChar(CP_ACP, 0, name, -1, w, 256);
        if (refuse_driver(w)) return NULL;
    }
    return CreateServiceA(m, name, disp, access, type, start, err, path, group, tag, deps, user, pw);
}

/* "no change" for the type: ask the service what it is */
static BOOL service_is_driver(SC_HANDLE h, DWORD type)
{
    QUERY_SERVICE_CONFIGW *c;
    DWORD need = 0;
    BOOL r = TRUE;                                  /* unknown: careful */
    if (type != SERVICE_NO_CHANGE) return (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) != 0;
    QueryServiceConfigW(h, NULL, 0, &need);
    if (need && (c = HeapAlloc(GetProcessHeap(), 0, need))) {
        if (QueryServiceConfigW(h, c, need, &need)) r = (c->dwServiceType & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) != 0;
        HeapFree(GetProcessHeap(), 0, c);
    }
    return r;
}

VX_EXPORT BOOL WINAPI vx_ChangeServiceConfigW(SC_HANDLE h, DWORD type, DWORD start, DWORD err, LPCWSTR path, LPCWSTR group,
                                              LPDWORD tag, LPCWSTR deps, LPCWSTR user, LPCWSTR pw, LPCWSTR disp)
{
    if ((start == SERVICE_BOOT_START || start == SERVICE_SYSTEM_START) && service_is_driver(h, type) && refuse_driver(disp))
        return FALSE;
    return ChangeServiceConfigW(h, type, start, err, path, group, tag, deps, user, pw, disp);
}

VX_EXPORT BOOL WINAPI vx_ChangeServiceConfigA(SC_HANDLE h, DWORD type, DWORD start, DWORD err, LPCSTR path, LPCSTR group,
                                              LPDWORD tag, LPCSTR deps, LPCSTR user, LPCSTR pw, LPCSTR disp)
{
    if ((start == SERVICE_BOOT_START || start == SERVICE_SYSTEM_START) && service_is_driver(h, type) && refuse_driver(NULL))
        return FALSE;
    return ChangeServiceConfigA(h, type, start, err, path, group, tag, deps, user, pw, disp);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

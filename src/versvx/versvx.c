/* versvx.dll - version.dll companion. Some programs (Chromium since it
 * dropped Windows 7) read the "real" Windows version from the file version
 * of kernel32.dll rather than asking Windows, and refuse to run below
 * Windows 10. When the program has a Windows version setting, the file
 * versions of kernel32, kernelbase and ntdll are reported as that version.
 * Everything else is forwarded to version.dll (generated .def). */
#include "../common/vx.h"

static vx_layer g_layer;

static BOOL is_core(const WCHAR *file)
{
    static const WCHAR *k[] = { L"kernel32.dll", L"kernelbase.dll", L"ntdll.dll" };
    const WCHAR *b = file + vx_wcslen(file);
    unsigned i;
    while (b > file && b[-1] != '\\' && b[-1] != '/') b--;
    for (i = 0; i < 3; i++) if (!vx_wcsicmp(b, k[i])) return TRUE;
    return FALSE;
}

/* rewrite VS_FIXEDFILEINFO (found by its signature) in a version block */
static void fake(const WCHAR *file, void *data, DWORD len)
{
    const vx_osver *v = g_layer.ver;
    BYTE *p = data;
    DWORD i;
    if (!v || !data || !file || !is_core(file)) return;
    VX_TRACE("version of a core DLL reported as the chosen Windows");
    for (i = 0; i + sizeof(VS_FIXEDFILEINFO) <= len; i += 4) {
        VS_FIXEDFILEINFO *f = (VS_FIXEDFILEINFO *)(p + i);
        if (f->dwSignature != 0xFEEF04BD) continue;
        f->dwFileVersionMS = f->dwProductVersionMS = (v->major << 16) | v->minor;
        f->dwFileVersionLS = f->dwProductVersionLS = (v->build << 16) | 1;
        return;
    }
}

VX_EXPORT BOOL WINAPI vx_GetFileVersionInfoW(LPCWSTR file, DWORD h, DWORD len, LPVOID data)
{
    if (!GetFileVersionInfoW(file, h, len, data)) return FALSE;
    fake(file, data, len);
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD h, DWORD len, LPVOID data)
{
    if (!GetFileVersionInfoExW(flags, file, h, len, data)) return FALSE;
    fake(file, data, len);
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_GetFileVersionInfoA(LPCSTR file, DWORD h, DWORD len, LPVOID data)
{
    WCHAR w[MAX_PATH];
    if (!GetFileVersionInfoA(file, h, len, data)) return FALSE;
    if (file && MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH)) fake(w, data, len);
    return TRUE;
}

/* GetFileVersionInfoExA is Windows 8+ */
VX_EXPORT BOOL WINAPI vx_GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD h, DWORD len, LPVOID data)
{
    WCHAR w[MAX_PATH];
    if (!file || !MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return vx_GetFileVersionInfoExW(flags, w, h, len, data);
}

VX_EXPORT DWORD WINAPI vx_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR file, LPDWORD h)
{
    WCHAR w[MAX_PATH];
    if (!file || !MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return GetFileVersionInfoSizeExW(flags, w, h);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        DisableThreadLibraryCalls(inst);
        vx_pin((void *)DllMainCRTStartup);
        if (n && n < MAX_PATH) {
            GetLongPathNameW(exe, exe, MAX_PATH);
            vx_effective_layer(exe, &g_layer);
        }
    }
    return TRUE;
}

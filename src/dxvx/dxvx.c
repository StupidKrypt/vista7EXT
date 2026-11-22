/* dxvx.dll - dxgi companion for Windows Vista, 7 and 8. Added:
 * CreateDXGIFactory2 (Windows 8.1). Microsoft: flags 0 or
 * DXGI_CREATE_FACTORY_DEBUG (load DXGIDebug.dll if present), and "otherwise
 * behaves" like CreateDXGIFactory1, which Vista (with its Platform Update),
 * 7 and 8 have. So it is CreateDXGIFactory1: an interface this Windows' DXGI
 * lacks (IDXGIFactory2 on Windows 7 without its Platform Update) fails as
 * DXGI itself fails it. Windows 8.1's own is used where it is there (the
 * Windows 8 build also runs on 8.1). Everything else is forwarded to dxgi. */
#include "../common/vx.h"

#define DXGI L"dxgi.dll"
#define VX_DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001L)
#define VX_DXGI_ERROR_UNSUPPORTED  ((HRESULT)0x887A0004L)

VX_EXPORT HRESULT WINAPI vx_CreateDXGIFactory2(UINT flags, REFIID riid, void **out)
{
    typedef HRESULT (WINAPI *f1_fn)(REFIID, void **);
    HMODULE d;
    f1_fn f1;
    VX_NATIVE_UPDATE(DXGI, CreateDXGIFactory2, HRESULT, (UINT flags, REFIID riid, void **out), (flags, riid, out));
    if (!out) return VX_DXGI_ERROR_INVALID_CALL;
    *out = NULL;
    if (flags & ~1u) return VX_DXGI_ERROR_INVALID_CALL;
    if (flags & 1) {                         /* DXGI_CREATE_FACTORY_DEBUG: the debug layer, if installed */
        WCHAR p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (n && n + 16 < MAX_PATH) { vx_wcscatn(p, L"\\dxgidebug.dll", MAX_PATH); LoadLibraryW(p); }
    }
    d = GetModuleHandleW(DXGI);
    if (!d) d = LoadLibraryW(DXGI);
    f1 = d ? (f1_fn)(void *)GetProcAddress(d, "CreateDXGIFactory1") : NULL;
    if (!f1) return VX_DXGI_ERROR_UNSUPPORTED;      /* Vista without its Platform Update */
    return f1(riid, out);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

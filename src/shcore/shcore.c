/* shcore.dll for Windows Vista: DPI functions (Windows 8.1). The stream and
 * registry helpers are forwarded to shlwapi (see the generated .def). */
#include "../common/vx.h"

static UINT system_dpi(void)
{
    HDC dc = GetDC(NULL);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d ? d : 96;
}

VX_EXPORT HRESULT WINAPI vx_GetDpiForMonitor(HMONITOR m, int type, UINT *x, UINT *y)
{
    if (!x || !y) return E_INVALIDARG;
    *x = *y = system_dpi();      /* Vista has one DPI for every monitor */
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_SetProcessDpiAwareness(int v)
{
    if (v < 0 || v > 2) return E_INVALIDARG;
    if (v) SetProcessDPIAware();
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_GetProcessDpiAwareness(HANDLE p, int *v)
{
    if (!v) return E_INVALIDARG;
    *v = IsProcessDPIAware() ? 1 : 0;
    return S_OK;
}

static int scale_factor(void)
{
    static const int f[] = { 100, 120, 125, 140, 150, 160, 175, 180, 200, 225, 250, 300, 350, 400, 450, 500 };
    int want = MulDiv(system_dpi(), 100, 96), best = 100, i;
    for (i = 0; i < 16; i++) {
        int d1 = f[i] - want, d2 = best - want;
        if ((d1 < 0 ? -d1 : d1) < (d2 < 0 ? -d2 : d2)) best = f[i];
    }
    return best;
}

VX_EXPORT HRESULT WINAPI vx_GetScaleFactorForMonitor(HMONITOR m, int *s) { if (!s) return E_INVALIDARG; *s = scale_factor(); return S_OK; }
VX_EXPORT int WINAPI vx_GetScaleFactorForDevice(int dev) { return scale_factor(); }
VX_EXPORT UINT WINAPI vx_GetDpiForShellUIComponent(int c) { return system_dpi(); }
VX_EXPORT HRESULT WINAPI vx_RegisterScaleChangeNotifications(int dev, HWND h, UINT msg, DWORD *cookie) { return E_NOTIMPL; }
VX_EXPORT HRESULT WINAPI vx_RevokeScaleChangeNotifications(int dev, DWORD cookie) { return E_NOTIMPL; }
VX_EXPORT HRESULT WINAPI vx_RegisterScaleChangeEvent(HANDLE ev, DWORD_PTR *cookie) { return E_NOTIMPL; }
VX_EXPORT HRESULT WINAPI vx_UnregisterScaleChangeEvent(DWORD_PTR cookie) { return E_NOTIMPL; }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

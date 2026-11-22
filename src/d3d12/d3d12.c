/* d3d12.dll for Windows Vista, 7 and 8, which have no Direct3D 12 (it came
 * with Windows 10). Programs built with Direct3D 12 support often import it
 * directly (Qt's rendering layer: OBS); without the DLL they cannot start at
 * all. Every function here fails cleanly, as Direct3D 12 does on a PC whose
 * graphics driver has no Direct3D 12 support: D3D12CreateDevice returns
 * DXGI_ERROR_UNSUPPORTED, and the program uses Direct3D 11 instead, as it
 * would there. Nothing is pretended: no device, interface or blob is ever
 * returned. A program's own d3d12.dll (Direct3D 12 on Windows 7 for some
 * games) is found first and is not affected.
 * Export numbers: the ones Windows' d3d12.dll has (Qt imports #101 and #102),
 * as listed in third_party/winexports/d3d12.ord. */
#include "../common/vx.h"
#include <objbase.h>

#define VX_DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004L)

static HRESULT none(void **a, void **b)
{
    if (a) *a = NULL;
    if (b) *b = NULL;
    VX_TRACE("Direct3D 12 asked for: not on this Windows");
    return VX_DXGI_ERROR_UNSUPPORTED;
}

VX_EXPORT HRESULT WINAPI vx_D3D12CreateDevice(IUnknown *adapter, int level, REFIID riid, void **dev) { return none(dev, NULL); }
VX_EXPORT HRESULT WINAPI vx_D3D12GetDebugInterface(REFIID riid, void **dbg) { return none(dbg, NULL); }
VX_EXPORT HRESULT WINAPI vx_D3D12GetInterface(REFCLSID clsid, REFIID riid, void **out) { return none(out, NULL); }
VX_EXPORT HRESULT WINAPI vx_D3D12EnableExperimentalFeatures(UINT n, const IID *ids, void *cfg, UINT *sizes) { return VX_DXGI_ERROR_UNSUPPORTED; }
VX_EXPORT HRESULT WINAPI vx_D3D12SerializeRootSignature(const void *desc, int ver, void **blob, void **err) { return none(blob, err); }
VX_EXPORT HRESULT WINAPI vx_D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **err) { return none(blob, err); }
VX_EXPORT HRESULT WINAPI vx_D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return none(out, NULL); }
VX_EXPORT HRESULT WINAPI vx_D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return none(out, NULL); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

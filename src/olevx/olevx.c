/* olevx.dll - ole32 companion for Windows Vista and 7.
 * CoGetApartmentType (Windows 7) asks COM's object context, as Windows 7 does;
 * RoGetAgileReference (Windows 8.1, also in ole32) is combase's. */
#include "../common/vx.h"
#include <objbase.h>
#include <objidl.h>

#define OLE L"ole32.dll"

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_CoGetApartmentType(APTTYPE *type, APTTYPEQUALIFIER *qual)
{ VX_NATIVE(OLE, CoGetApartmentType, HRESULT, (APTTYPE *type, APTTYPEQUALIFIER *qual), (type, qual));
    IComThreadingInfo *ti;
    HRESULT hr;
    if (!type || !qual) return E_INVALIDARG;
    *type = APTTYPE_CURRENT;
    *qual = APTTYPEQUALIFIER_NONE;
    hr = CoGetObjectContext(&IID_IComThreadingInfo, (void **)&ti);
    if (FAILED(hr)) return CO_E_NOTINITIALIZED;
    hr = ti->lpVtbl->GetCurrentApartmentType(ti, type);
    ti->lpVtbl->Release(ti);
    return hr;
}
#endif

VX_EXPORT HRESULT WINAPI vx_RoGetAgileReference(int options, REFIID riid, IUnknown *unk, void **ref)
{
    static HRESULT (WINAPI *p)(int, REFIID, IUnknown *, void **);
    if (!p) {
        HMODULE h = LoadLibraryW(L"combase.dll");
        p = h ? (HRESULT (WINAPI *)(int, REFIID, IUnknown *, void **))(void *)GetProcAddress(h, "RoGetAgileReference") : NULL;
    }
    if (!p) { if (ref) *ref = NULL; return E_NOTIMPL; }
    return p(options, riid, unk, ref);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

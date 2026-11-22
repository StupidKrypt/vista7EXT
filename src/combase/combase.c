/* combase.dll for Windows Vista. COM functions are forwarded to ole32 (see
 * the generated .def). Implemented here: Windows Runtime strings (HSTRING),
 * Ro* entry points that report "Windows Runtime class not available", and
 * CoIncrement/DecrementMTAUsage (Windows 8). */
#include "../common/vx.h"
#include <objbase.h>

/* Our HSTRING layout. It must fit inside HSTRING_HEADER (5 pointers) so that
 * WindowsCreateStringReference can build one in caller memory. */
typedef struct {
    UINT32 flags;          /* 1 = reference string (not owned) */
    UINT32 len;
    const WCHAR *buf;
} vx_hstr;
#define HS_REF 1

typedef vx_hstr *HSTR;
static const WCHAR g_empty[1] = { 0 };

static HSTR alloc_str(UINT32 len, WCHAR **data)
{
    vx_hstr *s = HeapAlloc(GetProcessHeap(), 0, sizeof(vx_hstr) + (len + 1) * sizeof(WCHAR));
    if (!s) return NULL;
    *data = (WCHAR *)(s + 1);
    (*data)[len] = 0;
    s->flags = 0;
    s->len = len;
    s->buf = *data;
    return s;
}

VX_EXPORT HRESULT WINAPI vx_WindowsCreateString(const WCHAR *src, UINT32 len, HSTR *out)
{
    WCHAR *d;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!len) return S_OK;
    if (!src) return E_POINTER;
    if (!(*out = alloc_str(len, &d))) return E_OUTOFMEMORY;
    vx_memcpy(d, src, len * sizeof(WCHAR));
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsCreateStringReference(const WCHAR *src, UINT32 len, void *header, HSTR *out)
{
    vx_hstr *s = header;
    if (!out || !header) return E_INVALIDARG;
    *out = NULL;
    if (!len) return S_OK;
    if (!src || src[len]) return E_INVALIDARG;   /* must be NUL-terminated */
    s->flags = HS_REF;
    s->len = len;
    s->buf = src;
    *out = s;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsDeleteString(HSTR s)
{
    if (s && !(s->flags & HS_REF)) HeapFree(GetProcessHeap(), 0, s);
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsDuplicateString(HSTR s, HSTR *out)
{
    if (!out) return E_INVALIDARG;
    if (!s) { *out = NULL; return S_OK; }
    return vx_WindowsCreateString(s->buf, s->len, out);
}

VX_EXPORT UINT32 WINAPI vx_WindowsGetStringLen(HSTR s) { return s ? s->len : 0; }

VX_EXPORT const WCHAR *WINAPI vx_WindowsGetStringRawBuffer(HSTR s, UINT32 *len)
{
    if (len) *len = s ? s->len : 0;
    return s ? s->buf : g_empty;
}

VX_EXPORT BOOL WINAPI vx_WindowsIsStringEmpty(HSTR s) { return !s || !s->len; }

VX_EXPORT HRESULT WINAPI vx_WindowsStringHasEmbeddedNull(HSTR s, BOOL *res)
{
    UINT32 i;
    if (!res) return E_INVALIDARG;
    *res = FALSE;
    for (i = 0; s && i < s->len; i++) if (!s->buf[i]) { *res = TRUE; break; }
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsCompareStringOrdinal(HSTR a, HSTR b, INT32 *res)
{
    UINT32 la = a ? a->len : 0, lb = b ? b->len : 0, i;
    const WCHAR *pa = a ? a->buf : g_empty, *pb = b ? b->buf : g_empty;
    if (!res) return E_INVALIDARG;
    for (i = 0; i < la && i < lb; i++)
        if (pa[i] != pb[i]) { *res = pa[i] < pb[i] ? -1 : 1; return S_OK; }
    *res = la == lb ? 0 : (la < lb ? -1 : 1);
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsConcatString(HSTR a, HSTR b, HSTR *out)
{
    UINT32 la = a ? a->len : 0, lb = b ? b->len : 0;
    WCHAR *d;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!la && !lb) return S_OK;
    if (!(*out = alloc_str(la + lb, &d))) return E_OUTOFMEMORY;
    if (la) vx_memcpy(d, a->buf, la * sizeof(WCHAR));
    if (lb) vx_memcpy(d + la, b->buf, lb * sizeof(WCHAR));
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsSubstringWithSpecifiedLength(HSTR s, UINT32 start, UINT32 len, HSTR *out)
{
    UINT32 sl = s ? s->len : 0;
    if (!out) return E_INVALIDARG;
    if (start > sl || len > sl - start) return E_BOUNDS;
    return vx_WindowsCreateString(s ? s->buf + start : NULL, len, out);
}

VX_EXPORT HRESULT WINAPI vx_WindowsSubstring(HSTR s, UINT32 start, HSTR *out)
{
    UINT32 sl = s ? s->len : 0;
    if (start > sl) return E_BOUNDS;
    return vx_WindowsSubstringWithSpecifiedLength(s, start, sl - start, out);
}

VX_EXPORT HRESULT WINAPI vx_WindowsPreallocateStringBuffer(UINT32 len, WCHAR **buf, HSTR *handle)
{
    WCHAR *d;
    HSTR s;
    if (!buf || !handle) return E_POINTER;
    if (!len) { *buf = (WCHAR *)g_empty; *handle = NULL; return S_OK; }
    if (!(s = alloc_str(len, &d))) return E_OUTOFMEMORY;
    *buf = d;
    *handle = s;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsPromoteStringBuffer(HSTR handle, HSTR *out)
{
    if (!out) return E_POINTER;
    *out = handle;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_WindowsDeleteStringBuffer(HSTR handle) { return vx_WindowsDeleteString(handle); }

/* ---- Windows Runtime activation ------------------------------------------
 * No Windows Runtime classes exist on Vista. Programs written for Windows
 * 7/8.1 compatibility already treat "class not registered" as "feature not
 * available" and carry on. */
VX_EXPORT HRESULT WINAPI vx_RoInitialize(int type)
{
    HRESULT hr = CoInitializeEx(NULL, type == 1 ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
    return hr;
}
VX_EXPORT void WINAPI vx_RoUninitialize(void) { CoUninitialize(); }
VX_EXPORT HRESULT WINAPI vx_RoGetActivationFactory(HSTR cls, REFIID riid, void **f) { VX_TRACE("RoGetActivationFactory"); if (f) *f = NULL; return REGDB_E_CLASSNOTREG; }
VX_EXPORT HRESULT WINAPI vx_RoActivateInstance(HSTR cls, void **inst) { VX_TRACE("RoActivateInstance"); if (inst) *inst = NULL; return REGDB_E_CLASSNOTREG; }
VX_EXPORT HRESULT WINAPI vx_RoRegisterActivationFactories(HSTR *c, void *f, UINT32 n, void *cookie) { return E_NOTIMPL; }
VX_EXPORT void WINAPI vx_RoRevokeActivationFactories(void *cookie) { }
/* The calling thread's apartment, as COM itself sees it (IComThreadingInfo,
 * Vista): every thread in the multithreaded apartment shares one identifier,
 * as do callers in the neutral one; a single-threaded apartment is its
 * thread. Without COM there is no apartment: CO_E_NOTINITIALIZED. The
 * values are opaque, as Microsoft documents them. */
static HRESULT apartment_id(UINT64 *id)
{
    IComThreadingInfo *ti;
    APTTYPE t;
    HRESULT hr;
    if (!id) return E_POINTER;
    *id = 0;
    if (FAILED(CoGetObjectContext(&IID_IComThreadingInfo, (void **)&ti))) return CO_E_NOTINITIALIZED;
    hr = ti->lpVtbl->GetCurrentApartmentType(ti, &t);
    ti->lpVtbl->Release(ti);
    if (FAILED(hr)) return hr;
    if (t == APTTYPE_MTA) *id = 0x4D54410000000000ULL | GetCurrentProcessId();        /* "MTA" */
    else if (t == APTTYPE_NA) *id = 0x4E41000000000000ULL | GetCurrentProcessId();    /* "NA" */
    else *id = GetCurrentThreadId();                                               /* STA, main STA */
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_RoGetApartmentIdentifier(UINT64 *id) { return apartment_id(id); }
VX_EXPORT BOOL WINAPI vx_RoOriginateError(HRESULT hr, HSTR msg) { return FALSE; }
VX_EXPORT BOOL WINAPI vx_RoOriginateErrorW(HRESULT hr, UINT n, PCWSTR msg) { return FALSE; }
VX_EXPORT BOOL WINAPI vx_RoOriginateLanguageException(HRESULT hr, HSTR msg, void *e) { return FALSE; }
VX_EXPORT BOOL WINAPI vx_RoTransformError(HRESULT a, HRESULT b, HSTR msg) { return FALSE; }
VX_EXPORT BOOL WINAPI vx_RoTransformErrorW(HRESULT a, HRESULT b, UINT n, PCWSTR msg) { return FALSE; }
VX_EXPORT HRESULT WINAPI vx_GetRestrictedErrorInfo(void **info) { *info = NULL; return S_FALSE; }
VX_EXPORT HRESULT WINAPI vx_SetRestrictedErrorInfo(void *info) { return S_OK; }
VX_EXPORT BOOL WINAPI vx_RoGetMatchingRestrictedErrorInfo(HRESULT hr, void **info) { *info = NULL; return FALSE; }
VX_EXPORT HRESULT WINAPI vx_RoReportUnhandledError(void *info) { return S_OK; }
VX_EXPORT HRESULT WINAPI vx_RoGetErrorReportingFlags(UINT32 *f) { *f = 1; return S_OK; }
VX_EXPORT HRESULT WINAPI vx_RoSetErrorReportingFlags(UINT32 f) { return S_OK; }
VX_EXPORT void WINAPI vx_RoFailFastWithErrorContext(HRESULT hr) { TerminateProcess(GetCurrentProcess(), (UINT)hr); }
VX_EXPORT HRESULT WINAPI vx_RoClearError(void) { return S_OK; }
VX_EXPORT HRESULT WINAPI vx_RoGetParameterizedTypeInstanceIID(UINT32 n, PCWSTR *names, const void *meta, GUID *iid, void **ext) { return E_NOTIMPL; }

/* ---- RoGetAgileReference (Windows 8.1) -------------------------------------
 * An agile reference lets another apartment get its own usable proxy for an
 * object. The Global Interface Table (in COM since Windows 2000) does exactly
 * that: the object is registered once, and Resolve gets it back marshalled
 * for the calling apartment. */
typedef struct agile agile;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(agile *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(agile *);
    ULONG (STDMETHODCALLTYPE *Release)(agile *);
    HRESULT (STDMETHODCALLTYPE *Resolve)(agile *, REFIID, void **);
} agile_vtbl;
struct agile { const agile_vtbl *vtbl; volatile LONG ref; DWORD cookie; IID iid; };

static const IID IID_vx_IAgileReference = { 0xc03f6a43, 0x65a4, 0x9818, { 0x98, 0x7e, 0xe0, 0xb8, 0x10, 0xd2, 0xa6, 0xf2 } };
static IGlobalInterfaceTable *volatile g_git;

static IGlobalInterfaceTable *git(void)
{
    IGlobalInterfaceTable *g = g_git;
    if (!g && SUCCEEDED(CoCreateInstance(&CLSID_StdGlobalInterfaceTable, NULL, CLSCTX_INPROC_SERVER,
                                         &IID_IGlobalInterfaceTable, (void **)&g))) {
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_git, g, NULL)) {
            g->lpVtbl->Release(g);          /* another thread was first */
            g = g_git;
        }
    }
    return g;
}

static HRESULT STDMETHODCALLTYPE ag_qi(agile *a, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_vx_IAgileReference)) {
        *out = a;
        InterlockedIncrement(&a->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ag_addref(agile *a) { return (ULONG)InterlockedIncrement(&a->ref); }
static ULONG STDMETHODCALLTYPE ag_release(agile *a)
{
    LONG r = InterlockedDecrement(&a->ref);
    if (!r) {
        IGlobalInterfaceTable *g = git();
        if (g) g->lpVtbl->RevokeInterfaceFromGlobal(g, a->cookie);
        HeapFree(GetProcessHeap(), 0, a);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ag_resolve(agile *a, REFIID riid, void **out)
{
    IGlobalInterfaceTable *g = git();
    IUnknown *u = NULL;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!g) return E_UNEXPECTED;
    hr = g->lpVtbl->GetInterfaceFromGlobal(g, a->cookie, &a->iid, (void **)&u);
    if (FAILED(hr)) return hr;
    hr = u->lpVtbl->QueryInterface(u, riid, out);
    u->lpVtbl->Release(u);
    return hr;
}
static const agile_vtbl g_agile_vtbl = { ag_qi, ag_addref, ag_release, ag_resolve };

VX_EXPORT HRESULT WINAPI vx_RoGetAgileReference(int options, REFIID riid, IUnknown *unk, void **ref)
{
    IGlobalInterfaceTable *g;
    agile *a;
    HRESULT hr;
    if (!ref) return E_POINTER;
    *ref = NULL;
    if (!unk || !riid || options < 0 || options > 1) return E_INVALIDARG;
    if (!(g = git())) return CO_E_NOTINITIALIZED;
    if (!(a = HeapAlloc(GetProcessHeap(), 0, sizeof(*a)))) return E_OUTOFMEMORY;
    a->vtbl = &g_agile_vtbl;
    a->ref = 1;
    a->iid = *riid;
    hr = g->lpVtbl->RegisterInterfaceInGlobal(g, unk, riid, &a->cookie);
    if (FAILED(hr)) { HeapFree(GetProcessHeap(), 0, a); return hr; }
    *ref = a;
    return S_OK;
}

/* RoRegisterForApartmentShutdown (Windows 8): accepted, but the callback is
 * never made; it only lets caches be emptied early when an apartment ends. */
static LONG g_shutdown_cookie;
VX_EXPORT HRESULT WINAPI vx_RoRegisterForApartmentShutdown(IUnknown *cb, UINT64 *id, HANDLE *cookie)
{
    HRESULT hr;
    if (!cb || !id || !cookie) return E_INVALIDARG;
    if (FAILED(hr = apartment_id(id))) return hr;
    *cookie = (HANDLE)(ULONG_PTR)InterlockedIncrement(&g_shutdown_cookie);
    return S_OK;
}
VX_EXPORT HRESULT WINAPI vx_RoUnregisterForApartmentShutdown(HANDLE cookie) { return cookie ? S_OK : E_INVALIDARG; }

/* ---- CoIncrementMTAUsage (Windows 8): keep the MTA alive with a thread
 * that sits in it until the cookie is released. ---------------------------- */
typedef struct { HANDLE ready, quit; } mta_ctx;

static DWORD WINAPI mta_thread(LPVOID p)
{
    mta_ctx *c = p;
    HANDLE quit = c->quit;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetEvent(c->ready);                 /* c is gone after this */
    WaitForSingleObject(quit, INFINITE);
    CloseHandle(quit);
    CoUninitialize();
    return 0;
}

VX_EXPORT HRESULT WINAPI vx_CoIncrementMTAUsage(HANDLE *cookie)
{
    mta_ctx c;
    HANDLE t;
    if (!cookie) return E_INVALIDARG;
    c.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!c.ready || !c.quit) return E_OUTOFMEMORY;
    t = CreateThread(NULL, 0, mta_thread, &c, 0, NULL);
    if (!t) { CloseHandle(c.ready); CloseHandle(c.quit); return E_OUTOFMEMORY; }
    WaitForSingleObject(c.ready, INFINITE);
    CloseHandle(c.ready);
    CloseHandle(t);
    *cookie = c.quit;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_CoDecrementMTAUsage(HANDLE cookie)
{
    return SetEvent(cookie) ? S_OK : E_INVALIDARG;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

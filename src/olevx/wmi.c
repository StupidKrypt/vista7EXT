/* olevx: WMI's answer about the operating system, for a program told another
 * Windows version.
 *
 * Win32_OperatingSystem (Version, BuildNumber, Caption, service pack) comes
 * from WMI's own service, which knows nothing of the version a program is
 * told. So the WMI locator a program creates is wrapped: the services and
 * enumerators it hands out are wrapped too, and the operating-system objects
 * they return answer those properties from the PEB (where kernelvx put the
 * told version, the same source as GetVersionEx and RtlGetVersion). Every
 * other object and call goes to WMI unchanged. Without a version setting
 * (the PEB says the real version) nothing is wrapped at all.
 *
 * Programs reach this two ways: their own CoCreateInstance calls (olevx is
 * their ole32), and, in the in-memory mode, calls Windows' own DLLs make
 * for them (vxload hooks ole32's exports and asks VxWmiWrap). VxWmiUnwrap
 * gives COM back its own object where it needs it (CoSetProxyBlanket).
 *
 * Not covered: asynchronous queries (results come through the program's own
 * sink, unchanged), and objects marshalled to another apartment. */
#include "../common/vx.h"
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>
#include <winternl.h>

enum { K_LOC, K_SVC, K_ENUM, K_OBJ, K_SEC };
typedef struct {
    const void *vt;
    LONG ref;
    IUnknown *in;          /* the real object (one reference held) */
    int kind;
    IUnknown *owner;       /* K_SEC: the wrapper it was asked from */
} wrap;

#define INNER(type, This) ((type *)((wrap *)(This))->in)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#include "wmi_fwd.h"

static const IWbemLocatorVtbl g_loc_vt;
static const IWbemServicesVtbl g_svc_vt;
static const IEnumWbemClassObjectVtbl g_enum_vt;
static const IWbemClassObjectVtbl g_obj_vt;
static const IClientSecurityVtbl g_sec_vt;

static BOOL ours(const void *p)
{
    const void *vt;
    if (!p) return FALSE;
    vt = *(const void *const *)p;
    return vt == &g_loc_vt || vt == &g_svc_vt || vt == &g_enum_vt || vt == &g_obj_vt || vt == &g_sec_vt;
}

/* ---- the told version (PEB) ------------------------------------------------ */
typedef struct { DWORD maj, min, build, sp; WCHAR csd[64]; } told;

static BOOL version_told(told *t)
{
    typedef void (NTAPI *fn)(DWORD *, DWORD *, DWORD *);
    static fn real;
    static DWORD rmaj, rmin, rbld;
    BYTE *peb = *(BYTE **)((BYTE *)NtCurrentTeb() + (sizeof(void *) == 8 ? 0x60 : 0x30));
    const UNICODE_STRING *c = (const UNICODE_STRING *)(peb + (sizeof(void *) == 8 ? 0x2E8 : 0x1F0));
    SIZE_T i;
    if (!real) {
        real = (fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
        if (!real) return FALSE;
        real(&rmaj, &rmin, &rbld);
        rbld &= 0xFFFF;
    }
    t->maj = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x118 : 0xA4));
    t->min = *(ULONG *)(peb + (sizeof(void *) == 8 ? 0x11C : 0xA8));
    t->build = *(USHORT *)(peb + (sizeof(void *) == 8 ? 0x120 : 0xAC));
    t->sp = *(USHORT *)(peb + (sizeof(void *) == 8 ? 0x122 : 0xAE)) >> 8;
    for (i = 0; c->Buffer && i < c->Length / sizeof(WCHAR) && i < 63; i++) t->csd[i] = c->Buffer[i];
    t->csd[i] = 0;
    return t->maj != rmaj || t->min != rmin || t->build != rbld;
}

/* ---- wrapping ---------------------------------------------------------------- */
static void *make(const void *vt, IUnknown *in, int kind)
{
    wrap *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return in;                      /* no memory: the real object, unchanged */
    w->vt = vt;
    w->ref = 1;
    w->in = in;
    w->kind = kind;
    return w;
}

static BOOL is_os_object(IWbemClassObject *o)
{
    VARIANT v;
    BOOL r = FALSE;
    VariantInit(&v);
    if (SUCCEEDED(o->lpVtbl->Get(o, L"__CLASS", 0, &v, NULL, NULL)) && V_VT(&v) == VT_BSTR && V_BSTR(&v))
        r = !vx_wcsicmp(V_BSTR(&v), L"Win32_OperatingSystem") || !vx_wcsicmp(V_BSTR(&v), L"CIM_OperatingSystem");
    VariantClear(&v);
    if (!r) r = o->lpVtbl->InheritsFrom(o, L"CIM_OperatingSystem") == WBEM_S_NO_ERROR;
    return r;
}

/* takes over the caller's reference */
static IWbemClassObject *wrap_obj(IWbemClassObject *o)
{
    if (!o || ours(o) || !is_os_object(o)) return o;
    return make(&g_obj_vt, (IUnknown *)o, K_OBJ);
}
static IWbemServices *wrap_svc(IWbemServices *s) { return !s || ours(s) ? s : make(&g_svc_vt, (IUnknown *)s, K_SVC); }
static IEnumWbemClassObject *wrap_enum(IEnumWbemClassObject *e) { return !e || ours(e) ? e : make(&g_enum_vt, (IUnknown *)e, K_ENUM); }

/* ---- IUnknown ------------------------------------------------------------------- */
static ULONG STDMETHODCALLTYPE w_AddRef(IUnknown *This) { return (ULONG)InterlockedIncrement(&((wrap *)This)->ref); }
static ULONG STDMETHODCALLTYPE w_Release(IUnknown *This)
{
    wrap *w = (wrap *)This;
    LONG r = InterlockedDecrement(&w->ref);
    if (!r) {
        w->in->lpVtbl->Release(w->in);
        if (w->owner) w->owner->lpVtbl->Release(w->owner);
        HeapFree(GetProcessHeap(), 0, w);
    }
    return (ULONG)r;
}

static HRESULT qi(IUnknown *This, REFIID riid, void **out, const IID *own)
{
    wrap *w = (wrap *)This;
    HRESULT hr;
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, own)) {
        w_AddRef(This);
        *out = This;
        return S_OK;
    }
    hr = w->in->lpVtbl->QueryInterface(w->in, riid, out);
    /* a proxy's security settings: through a wrapper that knows ours */
    if (SUCCEEDED(hr) && *out && IsEqualIID(riid, &IID_IClientSecurity) && (w->kind == K_SVC || w->kind == K_ENUM)) {
        wrap *s = make(&g_sec_vt, (IUnknown *)*out, K_SEC);
        if ((void *)s != *out) { s->owner = This; w_AddRef(This); *out = s; }
    }
    return hr;
}

#define IUNK(I, own) \
    static HRESULT STDMETHODCALLTYPE qi_##I(I *This, REFIID riid, void **out) { return qi((IUnknown *)This, riid, out, own); }
IUNK(IWbemLocator, &IID_IWbemLocator)
IUNK(IWbemServices, &IID_IWbemServices)
IUNK(IEnumWbemClassObject, &IID_IEnumWbemClassObject)
IUNK(IWbemClassObject, &IID_IWbemClassObject)
IUNK(IClientSecurity, &IID_IClientSecurity)
#define ADDREF(I) .QueryInterface = qi_##I, .AddRef = (ULONG (STDMETHODCALLTYPE *)(I *))w_AddRef, \
                  .Release = (ULONG (STDMETHODCALLTYPE *)(I *))w_Release

/* ---- the locator, services and enumerators: hand out wrapped ones -------------- */
static HRESULT STDMETHODCALLTYPE loc_ConnectServer(IWbemLocator *This, const BSTR res, const BSTR user, const BSTR pw,
                                                   const BSTR loc, LONG flags, const BSTR auth, IWbemContext *ctx,
                                                   IWbemServices **ns)
{
    HRESULT hr = fw_IWbemLocator_ConnectServer(This, res, user, pw, loc, flags, auth, ctx, ns);
    if (SUCCEEDED(hr) && ns) *ns = wrap_svc(*ns);
    return hr;
}

static HRESULT STDMETHODCALLTYPE svc_OpenNamespace(IWbemServices *This, const BSTR ns, LONG flags, IWbemContext *ctx,
                                                   IWbemServices **out, IWbemCallResult **res)
{
    HRESULT hr = fw_IWbemServices_OpenNamespace(This, ns, flags, ctx, out, res);
    if (SUCCEEDED(hr) && out) *out = wrap_svc(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE svc_GetObject(IWbemServices *This, const BSTR path, LONG flags, IWbemContext *ctx,
                                               IWbemClassObject **obj, IWbemCallResult **res)
{
    HRESULT hr = fw_IWbemServices_GetObject(This, path, flags, ctx, obj, res);
    if (SUCCEEDED(hr) && obj) *obj = wrap_obj(*obj);
    return hr;
}

static HRESULT STDMETHODCALLTYPE svc_CreateInstanceEnum(IWbemServices *This, const BSTR filter, LONG flags,
                                                        IWbemContext *ctx, IEnumWbemClassObject **e)
{
    HRESULT hr = fw_IWbemServices_CreateInstanceEnum(This, filter, flags, ctx, e);
    if (SUCCEEDED(hr) && e) *e = wrap_enum(*e);
    return hr;
}

static HRESULT STDMETHODCALLTYPE svc_ExecQuery(IWbemServices *This, const BSTR lang, const BSTR q, LONG flags,
                                               IWbemContext *ctx, IEnumWbemClassObject **e)
{
    HRESULT hr = fw_IWbemServices_ExecQuery(This, lang, q, flags, ctx, e);
    if (SUCCEEDED(hr) && e) *e = wrap_enum(*e);
    return hr;
}

static HRESULT STDMETHODCALLTYPE enum_Next(IEnumWbemClassObject *This, LONG timeout, ULONG n, IWbemClassObject **objs,
                                           ULONG *got)
{
    HRESULT hr = fw_IEnumWbemClassObject_Next(This, timeout, n, objs, got);
    ULONG i;
    if (SUCCEEDED(hr) && objs && got) for (i = 0; i < *got && i < n; i++) objs[i] = wrap_obj(objs[i]);
    return hr;
}

static HRESULT STDMETHODCALLTYPE enum_Clone(IEnumWbemClassObject *This, IEnumWbemClassObject **e)
{
    HRESULT hr = fw_IEnumWbemClassObject_Clone(This, e);
    if (SUCCEEDED(hr) && e) *e = wrap_enum(*e);
    return hr;
}

/* ---- the operating-system object --------------------------------------------- */
static BOOL has(const WCHAR *s, const WCHAR *w)
{
    SIZE_T i, j;
    for (i = 0; s && s[i]; i++) {
        for (j = 0; w[j] && s[i + j] && (s[i + j] | 32) == (w[j] | 32); j++) ;
        if (!w[j]) return TRUE;
    }
    return FALSE;
}

/* "Microsoft Windows 10 Pro": the edition carried over from the real caption */
static void caption(const told *t, const WCHAR *real, WCHAR *out, SIZE_T cch)
{
    const WCHAR *e;
    out[0] = 0;
    vx_wcscatn(out, L"Microsoft Windows ", cch);
    if (t->maj >= 10) {
        vx_wcscatn(out, t->build >= 22000 ? L"11 " : L"10 ", cch);
        e = has(real, L"Enterprise") ? L"Enterprise" : (has(real, L"Home") || has(real, L"Starter")) ? L"Home" : L"Pro";
    } else {
        e = has(real, L"Enterprise") ? L"Enterprise" : has(real, L"Ultimate") ? L"Ultimate" :
            has(real, L"Home Premium") ? L"Home Premium" : has(real, L"Home Basic") ? L"Home Basic" :
            has(real, L"Starter") ? L"Starter" : L"Professional";
        if (t->maj == 6 && t->min == 0) { vx_wcscatn(out, L"Vista ", cch); if (e[0] == 'P') e = L"Business"; }
        else if (t->maj == 6 && t->min == 1) vx_wcscatn(out, L"7 ", cch);
        else if (t->maj == 6 && t->min == 2) { vx_wcscatn(out, L"8 ", cch); if (e[0] != 'E') e = L"Pro"; }
        else { vx_wcscatn(out, L"8.1 ", cch); if (e[0] != 'E') e = L"Pro"; }
    }
    vx_wcscatn(out, e, cch);
}

static void cat_num(WCHAR *d, SIZE_T cch, DWORD v)     /* no wsprintf: olevx does not load user32 */
{
    WCHAR t[12], one[2] = { 0, 0 };
    int n = 0;
    do { t[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n) { one[0] = t[--n]; vx_wcscatn(d, one, cch); }
}

static void put_bstr(VARIANT *v, const WCHAR *s)
{
    BSTR b = SysAllocString(s);
    if (!b) return;
    VariantClear(v);
    V_VT(v) = VT_BSTR;
    V_BSTR(v) = b;
}

static void put_int(VARIANT *v, LONG n)
{
    VariantClear(v);
    V_VT(v) = VT_I4;
    V_I4(v) = n;
}

/* change one property value (as Get or Next returned it) to the told version */
static void fake(const WCHAR *name, VARIANT *v)
{
    told t;
    WCHAR s[300];
    if (!name || !v || !version_told(&t)) return;
    if (!vx_wcsicmp(name, L"Version")) {
        s[0] = 0;
        cat_num(s, 300, t.maj); vx_wcscatn(s, L".", 300);
        cat_num(s, 300, t.min); vx_wcscatn(s, L".", 300);
        cat_num(s, 300, t.build);
        put_bstr(v, s);
    } else if (!vx_wcsicmp(name, L"BuildNumber")) {
        s[0] = 0;
        cat_num(s, 300, t.build);
        put_bstr(v, s);
    } else if (!vx_wcsicmp(name, L"Caption") || !vx_wcsicmp(name, L"Name")) {
        const WCHAR *real = V_VT(v) == VT_BSTR ? V_BSTR(v) : NULL, *rest = NULL;
        if (real) for (rest = real; *rest && *rest != '|'; rest++) ;   /* Name: "<caption>|C:\Windows|..." */
        caption(&t, real, s, 200);
        if (rest && *rest == '|' && !vx_wcsicmp(name, L"Name")) vx_wcscatn(s, rest, 300);
        put_bstr(v, s);
    } else if (!vx_wcsicmp(name, L"ServicePackMajorVersion")) {
        put_int(v, (LONG)t.sp);
    } else if (!vx_wcsicmp(name, L"ServicePackMinorVersion")) {
        put_int(v, 0);
    } else if (!vx_wcsicmp(name, L"CSDVersion")) {
        if (t.csd[0]) put_bstr(v, t.csd);
        else { VariantClear(v); V_VT(v) = VT_NULL; }
    }
}

static HRESULT STDMETHODCALLTYPE obj_Get(IWbemClassObject *This, LPCWSTR name, LONG flags, VARIANT *v, CIMTYPE *type,
                                         LONG *flavor)
{
    HRESULT hr = fw_IWbemClassObject_Get(This, name, flags, v, type, flavor);
    if (SUCCEEDED(hr) && v) fake(name, v);
    return hr;
}

static HRESULT STDMETHODCALLTYPE obj_Next(IWbemClassObject *This, LONG flags, BSTR *name, VARIANT *v, CIMTYPE *type,
                                          LONG *flavor)
{
    HRESULT hr = fw_IWbemClassObject_Next(This, flags, name, v, type, flavor);
    if (hr == WBEM_S_NO_ERROR && name && *name && v) fake(*name, v);
    return hr;
}

static HRESULT STDMETHODCALLTYPE obj_Clone(IWbemClassObject *This, IWbemClassObject **copy)
{
    HRESULT hr = fw_IWbemClassObject_Clone(This, copy);
    if (SUCCEEDED(hr) && copy) *copy = wrap_obj(*copy);
    return hr;
}

/* ---- a proxy's security settings ------------------------------------------- */
static IUnknown *unwrap(IUnknown *p) { return ours(p) ? ((wrap *)p)->in : p; }

static HRESULT STDMETHODCALLTYPE sec_QueryBlanket(IClientSecurity *This, IUnknown *proxy, DWORD *a, DWORD *b,
                                                  OLECHAR **c, DWORD *d, DWORD *e, void **f, DWORD *g)
{
    return fw_IClientSecurity_QueryBlanket(This, unwrap(proxy), a, b, c, d, e, f, g);
}

static HRESULT STDMETHODCALLTYPE sec_SetBlanket(IClientSecurity *This, IUnknown *proxy, DWORD a, DWORD b, OLECHAR *c,
                                                DWORD d, DWORD e, void *f, DWORD g)
{
    return fw_IClientSecurity_SetBlanket(This, unwrap(proxy), a, b, c, d, e, f, g);
}

static HRESULT STDMETHODCALLTYPE sec_CopyProxy(IClientSecurity *This, IUnknown *proxy, IUnknown **copy)
{
    int kind = ours(proxy) ? ((wrap *)proxy)->kind : -1;
    HRESULT hr = fw_IClientSecurity_CopyProxy(This, unwrap(proxy), copy);
    if (SUCCEEDED(hr) && copy && *copy) {
        if (kind == K_SVC) *copy = (IUnknown *)wrap_svc((IWbemServices *)*copy);
        else if (kind == K_ENUM) *copy = (IUnknown *)wrap_enum((IEnumWbemClassObject *)*copy);
    }
    return hr;
}

static const IWbemLocatorVtbl g_loc_vt = { FW_IWbemLocator_VTBL, ADDREF(IWbemLocator), .ConnectServer = loc_ConnectServer };
static const IWbemServicesVtbl g_svc_vt = { FW_IWbemServices_VTBL, ADDREF(IWbemServices),
    .OpenNamespace = svc_OpenNamespace, .GetObject = svc_GetObject, .CreateInstanceEnum = svc_CreateInstanceEnum,
    .ExecQuery = svc_ExecQuery };
static const IEnumWbemClassObjectVtbl g_enum_vt = { FW_IEnumWbemClassObject_VTBL, ADDREF(IEnumWbemClassObject),
    .Next = enum_Next, .Clone = enum_Clone };
static const IWbemClassObjectVtbl g_obj_vt = { FW_IWbemClassObject_VTBL, ADDREF(IWbemClassObject),
    .Get = obj_Get, .Next = obj_Next, .Clone = obj_Clone };
static const IClientSecurityVtbl g_sec_vt = { FW_IClientSecurity_VTBL, ADDREF(IClientSecurity),
    .QueryBlanket = sec_QueryBlanket, .SetBlanket = sec_SetBlanket, .CopyProxy = sec_CopyProxy };
#pragma clang diagnostic pop

/* ---- entry points ---------------------------------------------------------------- */
/* a new WMI locator (CoCreateInstance's result for "iid"): wrapped if the
 * program is told another version; takes over the reference */
VX_EXPORT void *WINAPI vx_VxWmiWrap(void *obj, REFIID iid)
{
    told t;
    IWbemLocator *loc = NULL;
    if (!obj || ours(obj) || !version_told(&t)) return obj;
    if (iid && IsEqualIID(iid, &IID_IWbemLocator)) return make(&g_loc_vt, obj, K_LOC);
    /* asked for another interface (IUnknown): wrap only if it is a locator */
    if (iid && IsEqualIID(iid, &IID_IUnknown) &&
        SUCCEEDED(((IUnknown *)obj)->lpVtbl->QueryInterface((IUnknown *)obj, &IID_IWbemLocator, (void **)&loc))) {
        ((IUnknown *)obj)->lpVtbl->Release((IUnknown *)obj);
        return make(&g_loc_vt, (IUnknown *)loc, K_LOC);
    }
    return obj;
}

/* COM's own object behind a wrapper (no reference taken) */
VX_EXPORT void *WINAPI vx_VxWmiUnwrap(void *obj, REFIID iid)
{
    return unwrap(obj);
}

static const CLSID g_clsid_locator = { 0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };

/* the program's own calls (its ole32 imports come here) */
VX_EXPORT HRESULT WINAPI vx_CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out)
{
    HRESULT hr = CoCreateInstance(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && out && *out && clsid && IsEqualCLSID(clsid, &g_clsid_locator)) *out = vx_VxWmiWrap(*out, iid);
    return hr;
}

VX_EXPORT HRESULT WINAPI vx_CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *srv, DWORD n,
                                               MULTI_QI *mqi)
{
    HRESULT hr = CoCreateInstanceEx(clsid, outer, ctx, srv, n, mqi);
    DWORD i;
    if (SUCCEEDED(hr) && mqi && clsid && IsEqualCLSID(clsid, &g_clsid_locator))
        for (i = 0; i < n; i++) if (SUCCEEDED(mqi[i].hr) && mqi[i].pItf) mqi[i].pItf = vx_VxWmiWrap(mqi[i].pItf, mqi[i].pIID);
    return hr;
}

VX_EXPORT HRESULT WINAPI vx_CoSetProxyBlanket(IUnknown *proxy, DWORD authn, DWORD authz, OLECHAR *server, DWORD level,
                                              DWORD imp, RPC_AUTH_IDENTITY_HANDLE id, DWORD caps)
{
    return CoSetProxyBlanket(unwrap(proxy), authn, authz, server, level, imp, id, caps);
}

VX_EXPORT HRESULT WINAPI vx_CoQueryProxyBlanket(IUnknown *proxy, DWORD *authn, DWORD *authz, OLECHAR **server,
                                                DWORD *level, DWORD *imp, RPC_AUTH_IDENTITY_HANDLE *id, DWORD *caps)
{
    return CoQueryProxyBlanket(unwrap(proxy), authn, authz, server, level, imp, id, caps);
}

VX_EXPORT HRESULT WINAPI vx_CoCopyProxy(IUnknown *proxy, IUnknown **copy)
{
    int kind = ours(proxy) ? ((wrap *)proxy)->kind : -1;
    HRESULT hr = CoCopyProxy(unwrap(proxy), copy);
    if (SUCCEEDED(hr) && copy && *copy) {
        if (kind == K_SVC) *copy = (IUnknown *)wrap_svc((IWbemServices *)*copy);
        else if (kind == K_ENUM) *copy = (IUnknown *)wrap_enum((IEnumWbemClassObject *)*copy);
    }
    return hr;
}

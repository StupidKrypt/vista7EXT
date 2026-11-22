/* winhtvx.dll - winhttp companion for Windows Vista and 7.
 *
 * Added (Windows 8): the asynchronous proxy resolver - WinHttpCreateProxyResolver,
 * WinHttpGetProxyForUrlEx, WinHttpGetProxyResult, WinHttpFreeProxyResult -
 * and the WebSocket functions.
 *
 * Microsoft Learn (winhttp.h): a resolver is made from a session; the
 * session's status callback (set before) is the resolver's too.
 * WinHttpGetProxyForUrlEx always works asynchronously: ERROR_IO_PENDING, then
 * WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE (the result is fetched with
 * WinHttpGetProxyResult and freed with WinHttpFreeProxyResult) or
 * WINHTTP_CALLBACK_STATUS_REQUEST_ERROR. Here the work is Vista's
 * synchronous WinHttpGetProxyForUrl, on a thread-pool thread, with the same
 * options; its WINHTTP_PROXY_INFO becomes the list of entries.
 *
 * To call the right callback, the program's WinHttpSetStatusCallback calls
 * are recorded (and still go to WinHTTP), and WinHttpCloseHandle knows a
 * resolver from WinHTTP's own handles. A resolver closed while its work runs
 * gets no callback.
 *
 * WebSockets over WinHTTP need Windows 8's HTTP stack: upgrading a request
 * fails with ERROR_NOT_SUPPORTED, so no WebSocket handle ever exists and the
 * other WebSocket functions fail with ERROR_INVALID_HANDLE. */
#include "../common/vx.h"
#include <winhttp.h>

#define WH L"winhttp.dll"

#if VX_OS < 602   /* Windows 8 and later have it all: their .def forwards there */

#define VX_STATUS_GETPROXYFORURL_COMPLETE 0x01000000
#define VX_FLAG_GETPROXYFORURL_COMPLETE   0x01000000
#define VX_API_GET_PROXY_FOR_URL 6

typedef struct { BOOL fProxy, fBypass; INTERNET_SCHEME ProxyScheme; PWSTR pwszProxy; INTERNET_PORT ProxyPort; } VX_PROXY_ENTRY;
typedef struct { DWORD cEntries; VX_PROXY_ENTRY *pEntries; } VX_PROXY_RESULT;

/* ---- callbacks recorded per handle ------------------------------------------ */
typedef struct cbrec { struct cbrec *next; HINTERNET h; WINHTTP_STATUS_CALLBACK cb; DWORD flags; } cbrec;
static cbrec *g_cbs;
static SRWLOCK g_cblock = SRWLOCK_INIT;

static void cb_set(HINTERNET h, WINHTTP_STATUS_CALLBACK cb, DWORD flags)
{
    cbrec *r;
    AcquireSRWLockExclusive(&g_cblock);
    for (r = g_cbs; r && r->h != h; r = r->next) ;
    if (!r && cb && (r = HeapAlloc(GetProcessHeap(), 0, sizeof(*r)))) { r->h = h; r->next = g_cbs; g_cbs = r; }
    if (r) { r->cb = cb; r->flags = flags; }
    ReleaseSRWLockExclusive(&g_cblock);
}

static BOOL cb_get(HINTERNET h, WINHTTP_STATUS_CALLBACK *cb, DWORD *flags)
{
    cbrec *r;
    AcquireSRWLockShared(&g_cblock);
    for (r = g_cbs; r && r->h != h; r = r->next) ;
    if (r) { *cb = r->cb; *flags = r->flags; }
    ReleaseSRWLockShared(&g_cblock);
    return r && r->cb;
}

static void cb_forget(HINTERNET h)
{
    cbrec **pp, *r;
    AcquireSRWLockExclusive(&g_cblock);
    for (pp = &g_cbs; *pp; pp = &(*pp)->next) if ((*pp)->h == h) { r = *pp; *pp = r->next; HeapFree(GetProcessHeap(), 0, r); break; }
    ReleaseSRWLockExclusive(&g_cblock);
}

/* ---- resolvers ------------------------------------------------------------------ */
typedef struct resolver {
    struct resolver *next;
    DWORD magic;
    HINTERNET session;
    WINHTTP_STATUS_CALLBACK cb;
    DWORD flags;
    volatile LONG busy, closed;
    volatile LONG refs;                 /* 1 for the handle, 1 while work runs: the last one frees */
    BOOL done;                          /* a result is there */
    WINHTTP_PROXY_INFO info;            /* WinHttpGetProxyForUrl's answer (strings GlobalAlloc'd by WinHTTP) */
    /* the request being worked on */
    WCHAR *url;
    WINHTTP_AUTOPROXY_OPTIONS opts;
    WCHAR *pac;
    DWORD_PTR ctx;
} resolver;
#define RES_MAGIC 0x52485856u           /* "VXHR" */
static resolver *g_res;
static SRWLOCK g_rlock = SRWLOCK_INIT;

/* is h a live resolver of ours? (the lock is held on success, shared) */
static resolver *res_find(HINTERNET h)
{
    resolver *r;
    AcquireSRWLockShared(&g_rlock);
    for (r = g_res; r && (HINTERNET)r != h; r = r->next) ;
    if (r && r->magic == RES_MAGIC && !r->closed) return r;
    ReleaseSRWLockShared(&g_rlock);
    return NULL;
}

static void info_free(WINHTTP_PROXY_INFO *i)
{
    if (i->lpszProxy) GlobalFree(i->lpszProxy);
    if (i->lpszProxyBypass) GlobalFree(i->lpszProxyBypass);
    vx_memset(i, 0, sizeof(*i));
}

static void res_free(resolver *r)
{
    resolver **pp;
    AcquireSRWLockExclusive(&g_rlock);
    for (pp = &g_res; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    ReleaseSRWLockExclusive(&g_rlock);
    r->magic = 0;
    info_free(&r->info);
    if (r->url) HeapFree(GetProcessHeap(), 0, r->url);
    if (r->pac) HeapFree(GetProcessHeap(), 0, r->pac);
    HeapFree(GetProcessHeap(), 0, r);
}

static void res_release(resolver *r)
{
    if (!InterlockedDecrement(&r->refs)) res_free(r);
}

static WCHAR *dup(const WCHAR *s)
{
    SIZE_T n;
    WCHAR *d;
    if (!s) return NULL;
    n = vx_wcslen(s) + 1;
    if ((d = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR)))) vx_memcpy(d, s, n * sizeof(WCHAR));
    return d;
}

static DWORD WINAPI resolve_proc(LPVOID p)
{
    resolver *r = p;
    WINHTTP_PROXY_INFO info;
    DWORD err = 0;
    WINHTTP_ASYNC_RESULT ar;
    vx_memset(&info, 0, sizeof(info));
    if (!WinHttpGetProxyForUrl(r->session, r->url, &r->opts, &info)) err = GetLastError();
    info_free(&r->info);
    if (!err) { r->info = info; r->done = TRUE; }
    InterlockedExchange(&r->busy, 0);               /* (WinHttpGetProxyResult works from here on) */
    if (r->closed || !r->cb) { res_release(r); return 0; }     /* closed meanwhile: no callback */
    if (!err) {
        if (r->flags & VX_FLAG_GETPROXYFORURL_COMPLETE)
            r->cb((HINTERNET)r, r->ctx, VX_STATUS_GETPROXYFORURL_COMPLETE, NULL, 0);
    } else if (r->flags & WINHTTP_CALLBACK_FLAG_REQUEST_ERROR) {
        ar.dwResult = VX_API_GET_PROXY_FOR_URL;
        ar.dwError = err;
        r->cb((HINTERNET)r, r->ctx, WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &ar, sizeof(ar));
    }
    res_release(r);                                  /* the callback may have closed it */
    return 0;
}

VX_EXPORT DWORD WINAPI vx_WinHttpCreateProxyResolver(HINTERNET session, HINTERNET *out)
{ VX_NATIVE(WH, WinHttpCreateProxyResolver, DWORD, (HINTERNET session, HINTERNET *out), (session, out));
    resolver *r;
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!session) return ERROR_INVALID_HANDLE;
    if (!(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r)))) return ERROR_NOT_ENOUGH_MEMORY;
    r->magic = RES_MAGIC;
    r->refs = 1;
    r->session = session;
    cb_get(session, &r->cb, &r->flags);                /* the session's callback is the resolver's */
    AcquireSRWLockExclusive(&g_rlock);
    r->next = g_res;
    g_res = r;
    ReleaseSRWLockExclusive(&g_rlock);
    *out = (HINTERNET)r;
    return ERROR_SUCCESS;
}

VX_EXPORT DWORD WINAPI vx_WinHttpGetProxyForUrlEx(HINTERNET h, PCWSTR url, WINHTTP_AUTOPROXY_OPTIONS *opts, DWORD_PTR ctx)
{ VX_NATIVE(WH, WinHttpGetProxyForUrlEx, DWORD, (HINTERNET h, PCWSTR url, WINHTTP_AUTOPROXY_OPTIONS *opts, DWORD_PTR ctx), (h, url, opts, ctx));
    resolver *r = res_find(h);
    DWORD e = ERROR_IO_PENDING;
    if (!r) return ERROR_WINHTTP_INCORRECT_HANDLE_TYPE;
    if (!url || !opts) e = ERROR_INVALID_PARAMETER;
    else if (InterlockedCompareExchange(&r->busy, 1, 0)) e = ERROR_WINHTTP_INCORRECT_HANDLE_STATE;   /* one at a time */
    ReleaseSRWLockShared(&g_rlock);
    if (e != ERROR_IO_PENDING) return e;
    if (r->url) HeapFree(GetProcessHeap(), 0, r->url);
    if (r->pac) HeapFree(GetProcessHeap(), 0, r->pac);
    r->url = dup(url);
    r->opts = *opts;
    r->pac = dup(opts->lpszAutoConfigUrl);
    r->opts.lpszAutoConfigUrl = r->pac;
    r->ctx = ctx;
    r->done = FALSE;
    if (!r->url || (opts->lpszAutoConfigUrl && !r->pac)) { InterlockedExchange(&r->busy, 0); return ERROR_NOT_ENOUGH_MEMORY; }
    InterlockedIncrement(&r->refs);                  /* the work's */
    if (!QueueUserWorkItem(resolve_proc, r, WT_EXECUTELONGFUNCTION)) {
        e = GetLastError();
        InterlockedExchange(&r->busy, 0);
        res_release(r);
        return e;
    }
    return ERROR_IO_PENDING;
}

/* "host:port" (with an optional "scheme=" in front) -> one entry */
static BOOL entry_of(const WCHAR *s, SIZE_T n, VX_PROXY_ENTRY *e)
{
    SIZE_T i, c = n, h0 = 0;
    DWORD port = 0;
    e->fProxy = TRUE;
    e->fBypass = FALSE;
    e->ProxyScheme = INTERNET_SCHEME_HTTP;
    for (i = 0; i < n; i++) if (s[i] == '=') {
        if (i == 5 && !vx_wcsnicmp(s, L"https", 5)) e->ProxyScheme = INTERNET_SCHEME_HTTPS;
        else if (i == 5 && !vx_wcsnicmp(s, L"socks", 5)) e->ProxyScheme = INTERNET_SCHEME_SOCKS;
        h0 = i + 1;
        break;
    }
    for (i = n; i > h0; i--) if (s[i - 1] == ':') { c = i - 1; break; }
    if (c < n) for (i = c + 1; i < n && s[i] >= '0' && s[i] <= '9'; i++) port = port * 10 + (s[i] - '0');
    e->ProxyPort = (INTERNET_PORT)(port ? port : INTERNET_DEFAULT_HTTP_PORT);
    if (c <= h0 || !(e->pwszProxy = GlobalAlloc(GPTR, (c - h0 + 1) * sizeof(WCHAR)))) return FALSE;
    vx_memcpy(e->pwszProxy, s + h0, (c - h0) * sizeof(WCHAR));
    return TRUE;
}

VX_EXPORT DWORD WINAPI vx_WinHttpGetProxyResult(HINTERNET h, VX_PROXY_RESULT *out)
{ VX_NATIVE(WH, WinHttpGetProxyResult, DWORD, (HINTERNET h, VX_PROXY_RESULT *out), (h, out));
    resolver *r = res_find(h);
    DWORD e = ERROR_SUCCESS, n = 0, k = 0;
    const WCHAR *p;
    if (!r) return ERROR_WINHTTP_INCORRECT_HANDLE_TYPE;
    if (!out) e = ERROR_INVALID_PARAMETER;
    else if (r->busy || !r->done) e = ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (e) { ReleaseSRWLockShared(&g_rlock); return e; }
    out->cEntries = 0;
    out->pEntries = NULL;
    if (r->info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && r->info.lpszProxy) {
        for (p = r->info.lpszProxy; *p; p++) if ((p == r->info.lpszProxy || p[-1] == ';' || p[-1] == ' ') && *p != ';' && *p != ' ') n++;
    }
    if (!n) n = 1;                                       /* no proxy: one direct entry */
    if (!(out->pEntries = GlobalAlloc(GPTR, n * sizeof(VX_PROXY_ENTRY)))) { ReleaseSRWLockShared(&g_rlock); return ERROR_NOT_ENOUGH_MEMORY; }
    if (r->info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && r->info.lpszProxy) {
        p = r->info.lpszProxy;
        while (*p && k < n) {
            const WCHAR *s;
            while (*p == ';' || *p == ' ') p++;
            s = p;
            while (*p && *p != ';' && *p != ' ') p++;
            if (p > s && entry_of(s, (SIZE_T)(p - s), &out->pEntries[k])) k++;
        }
    }
    if (!k) out->pEntries[k++].fProxy = FALSE;           /* DIRECT */
    out->cEntries = k;
    ReleaseSRWLockShared(&g_rlock);
    return ERROR_SUCCESS;
}

VX_EXPORT VOID WINAPI vx_WinHttpFreeProxyResult(VX_PROXY_RESULT *res)
{ VX_NATIVE_VOID(WH, WinHttpFreeProxyResult, (VX_PROXY_RESULT *res), (res));
    DWORD i;
    if (!res) return;
    for (i = 0; res->pEntries && i < res->cEntries; i++) if (res->pEntries[i].pwszProxy) GlobalFree(res->pEntries[i].pwszProxy);
    if (res->pEntries) GlobalFree(res->pEntries);
    res->cEntries = 0;
    res->pEntries = NULL;
}

/* ---- the program's calls on WinHTTP handles -------------------------------------- */
VX_EXPORT WINHTTP_STATUS_CALLBACK WINAPI vx_WinHttpSetStatusCallback(HINTERNET h, WINHTTP_STATUS_CALLBACK cb, DWORD flags, DWORD_PTR reserved)
{
    resolver *r = res_find(h);
    WINHTTP_STATUS_CALLBACK old;
    if (r) {                                             /* on a resolver: kept with it */
        old = r->cb;
        r->cb = cb;
        r->flags = flags;
        ReleaseSRWLockShared(&g_rlock);
        return old;
    }
    old = WinHttpSetStatusCallback(h, cb, flags, reserved);
    if (old != WINHTTP_INVALID_STATUS_CALLBACK) cb_set(h, cb, flags);
    return old;
}

VX_EXPORT BOOL WINAPI vx_WinHttpCloseHandle(HINTERNET h)
{
    resolver *r = res_find(h);
    if (r) {
        LONG was = InterlockedExchange(&r->closed, 1);
        ReleaseSRWLockShared(&g_rlock);
        if (!was) res_release(r);                        /* the handle's reference (the work keeps its own) */
        return TRUE;
    }
    cb_forget(h);
    return WinHttpCloseHandle(h);
}

/* ---- WebSockets: not on this Windows ------------------------------------------------ */
VX_EXPORT HINTERNET WINAPI vx_WinHttpWebSocketCompleteUpgrade(HINTERNET req, DWORD_PTR ctx)
{ VX_NATIVE(WH, WinHttpWebSocketCompleteUpgrade, HINTERNET, (HINTERNET req, DWORD_PTR ctx), (req, ctx));
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}
VX_EXPORT DWORD WINAPI vx_WinHttpWebSocketSend(HINTERNET ws, int type, PVOID buf, DWORD len) { VX_NATIVE(WH, WinHttpWebSocketSend, DWORD, (HINTERNET ws, int type, PVOID buf, DWORD len), (ws, type, buf, len)); return ERROR_INVALID_HANDLE; }
VX_EXPORT DWORD WINAPI vx_WinHttpWebSocketReceive(HINTERNET ws, PVOID buf, DWORD len, DWORD *got, int *type) { VX_NATIVE(WH, WinHttpWebSocketReceive, DWORD, (HINTERNET ws, PVOID buf, DWORD len, DWORD *got, int *type), (ws, buf, len, got, type)); return ERROR_INVALID_HANDLE; }
VX_EXPORT DWORD WINAPI vx_WinHttpWebSocketShutdown(HINTERNET ws, USHORT st, PVOID r, DWORD n) { VX_NATIVE(WH, WinHttpWebSocketShutdown, DWORD, (HINTERNET ws, USHORT st, PVOID r, DWORD n), (ws, st, r, n)); return ERROR_INVALID_HANDLE; }
VX_EXPORT DWORD WINAPI vx_WinHttpWebSocketClose(HINTERNET ws, USHORT st, PVOID r, DWORD n) { VX_NATIVE(WH, WinHttpWebSocketClose, DWORD, (HINTERNET ws, USHORT st, PVOID r, DWORD n), (ws, st, r, n)); return ERROR_INVALID_HANDLE; }
VX_EXPORT DWORD WINAPI vx_WinHttpWebSocketQueryCloseStatus(HINTERNET ws, USHORT *st, PVOID r, DWORD n, DWORD *got) { VX_NATIVE(WH, WinHttpWebSocketQueryCloseStatus, DWORD, (HINTERNET ws, USHORT *st, PVOID r, DWORD n, DWORD *got), (ws, st, r, n, got)); return ERROR_INVALID_HANDLE; }

#endif

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

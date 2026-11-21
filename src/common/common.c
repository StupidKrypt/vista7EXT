/* VistaExt - CRT replacements and layer (compatibility setting) parsing. */
#include "vx.h"
#include "releases.h"

/* The compiler may emit calls to these, so they keep their standard names. */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

void *vx_memset(void *d, int c, SIZE_T n) { return memset(d, c, n); }
void *vx_memcpy(void *d, const void *s, SIZE_T n) { return memcpy(d, s, n); }

static WCHAR lw(WCHAR c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static char  lc(char c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

SIZE_T vx_wcslen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
SIZE_T vx_strlen(const char *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
int vx_wcsicmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && lw(*a) == lw(*b)) { a++; b++; }
    return lw(*a) - lw(*b);
}
int vx_wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (; n; n--, a++, b++) {
        if (lw(*a) != lw(*b)) return lw(*a) - lw(*b);
        if (!*a) return 0;
    }
    return 0;
}
int vx_stricmp(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) { a++; b++; }
    return lc(*a) - lc(*b);
}
void vx_wcscpyn(WCHAR *d, const WCHAR *s, SIZE_T cch)
{
    SIZE_T i = 0;
    if (!cch) return;
    for (; i + 1 < cch && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
void vx_wcscatn(WCHAR *d, const WCHAR *s, SIZE_T cch)
{
    SIZE_T n = vx_wcslen(d);
    if (n < cch) vx_wcscpyn(d + n, s, cch - n);
}

int vx_tracing = -1;

void vx_trace(const char *what)
{
    char b[200] = "vx: ";
    SIZE_T n = 4;
    if (vx_tracing < 0) vx_tracing = GetEnvironmentVariableW(L"VX_TRACE", NULL, 0) != 0;
    if (!vx_tracing) return;
    while (*what && n < sizeof(b) - 2) b[n++] = *what++;     /* no wsprintf: not every DLL has user32 */
    b[n++] = '\n';
    b[n] = 0;
    OutputDebugStringA(b);
}

FARPROC vx_native(const WCHAR *module, const char *name)
{
    static int off = -1;     /* VX_NO_NATIVE=1: always use VistaExt's versions (testing) */
    HMODULE h;
    if (off < 0) off = GetEnvironmentVariableW(L"VX_NO_NATIVE", NULL, 0) != 0;
    if (vx_tracing < 0) vx_tracing = GetEnvironmentVariableW(L"VX_TRACE", NULL, 0) != 0;
    if (off) return NULL;
    h = GetModuleHandleW(module);
    if (!h) h = LoadLibraryW(module);
    return h ? GetProcAddress(h, name) : NULL;
}

/* KUSER_SHARED_DATA.NtMajorVersion / NtMinorVersion: the kernel's own copy,
 * which the per-program version setting does not change. */
void vx_real_version(DWORD *major, DWORD *minor)
{
    *major = VX_KUSER_NT_MAJOR;
    *minor = VX_KUSER_NT_MINOR;
}

const vx_osver vx_osvers[] = {
    { L"VISTA", L"Windows Vista SP2",  6, 0,  6002, 2, L"Service Pack 2" },
    { L"WIN7",  L"Windows 7 SP1",      6, 1,  7601, 1, L"Service Pack 1" },
    { L"WIN8",  L"Windows 8",          6, 2,  9200, 0, L"" },
    { L"WIN81", L"Windows 8.1",        6, 3,  9600, 0, L"" },
    { L"WIN10", L"Windows 10 (22H2)", 10, 0, 19045, 0, L"" },
    { L"WIN11", L"Windows 11 (23H2)", 10, 0, 22631, 0, L"" },
};
const int vx_osver_count = sizeof(vx_osvers) / sizeof(vx_osvers[0]);
const vx_osver vx_osver_real = { L"REAL", L"This Windows' own version", 0, 0, 0, 0, L"" };

static BOOL num(const WCHAR **s, DWORD *v, DWORD max)
{
    DWORD x = 0;
    const WCHAR *p = *s;
    if (*p < '0' || *p > '9') return FALSE;
    while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); if (x > max) return FALSE; p++; }
    *v = x;
    *s = p;
    return TRUE;
}

void vx_cat_num(WCHAR *out, SIZE_T cch, ULONG_PTR v)
{
    WCHAR t[24];
    int n = 0;
    SIZE_T l = vx_wcslen(out);
    do { t[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n && l + 1 < cch) out[l++] = t[--n];
    out[l] = 0;
}

BOOL vx_parse_custom(const WCHAR *s, vx_osver *v, WCHAR *tok, SIZE_T ctok, WCHAR *csd, SIZE_T ccsd)
{
    DWORD ma, mi, b, sp = 0;
    if (vx_wcsnicmp(s, L"VER:", 4)) return FALSE;
    s += 4;
    if (!num(&s, &ma, 99) || ma < 5 || *s++ != '.' || !num(&s, &mi, 99) || *s++ != '.' || !num(&s, &b, 65535)) return FALSE;
    if (*s == '.') { s++; if (!num(&s, &sp, 9)) return FALSE; }
    if (*s && *s != ' ') return FALSE;
    tok[0] = 0;
    vx_wcscatn(tok, L"VER:", ctok);
    vx_cat_num(tok, ctok, ma); vx_wcscatn(tok, L".", ctok);
    vx_cat_num(tok, ctok, mi); vx_wcscatn(tok, L".", ctok);
    vx_cat_num(tok, ctok, b);
    csd[0] = 0;
    if (sp) {
        vx_wcscatn(tok, L".", ctok); vx_cat_num(tok, ctok, sp);
        vx_wcscatn(csd, L"Service Pack ", ccsd); vx_cat_num(csd, ccsd, sp);
    }
    v->token = tok;
    v->label = L"Custom";
    v->major = ma; v->minor = mi; v->build = b;
    v->spmajor = (WORD)sp;
    v->csd = csd;
    return TRUE;
}

/* Windows 10 and 11 name their releases; programs read these from the
 * registry (DisplayVersion, ReleaseId) next to the build number. */
const WCHAR *vx_release_name(DWORD build, const WCHAR **releaseid)
{
    const vx_release *r = vx_find_release(build);
    if (releaseid) *releaseid = r ? r->rel : NULL;
    return r ? r->disp : NULL;
}

void vx_parse_layer(const WCHAR *s, vx_layer *out)
{
    WCHAR tok[64];
    int i;
    vx_memset(out, 0, sizeof(*out));
    if (!s) return;
    while (*s) {
        SIZE_T n = 0;
        while (*s == ' ') s++;
        if (!*s) break;
        if (!vx_wcsnicmp(s, L"ARGS:", 5)) {      /* rest of the string */
            vx_wcscpyn(out->args, s + 5, sizeof(out->args) / sizeof(WCHAR));
            break;
        }
        if (!vx_wcsnicmp(s, L"VER:", 4)) {
            if (vx_parse_custom(s, &out->custom, out->customtok, 40, out->customcsd, 24)) {
                out->ver = &out->custom;
                out->real = FALSE;
            }
            while (*s && *s != ' ') s++;
            continue;
        }
        while (s[n] && s[n] != ' ' && n < 63) { tok[n] = s[n]; n++; }
        tok[n] = 0;
        s += n;
        while (*s && *s != ' ') s++;
        if (!vx_wcsicmp(tok, L"WEBVIEW2")) { out->webview2 = TRUE; continue; }
        if (!vx_wcsicmp(tok, L"OFF")) { out->off = TRUE; continue; }
        if (!vx_wcsicmp(tok, L"REAL")) { out->real = TRUE; out->ver = NULL; continue; }
        for (i = 0; i < vx_osver_count; i++)
            if (!vx_wcsicmp(tok, vx_osvers[i].token)) { out->ver = &vx_osvers[i]; out->real = FALSE; }
    }
}

void vx_copy_layer(vx_layer *to, const vx_layer *from)
{
    vx_memcpy(to, from, sizeof(*to));
    if (from->ver == &from->custom) {
        to->ver = &to->custom;
        to->custom.token = to->customtok;
        to->custom.csd = to->customcsd;
    }
}

DWORD vx_setting(const WCHAR *name, DWORD def)
{
    HKEY k;
    DWORD v = def, cb = sizeof(v), type;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = def;
        RegCloseKey(k);
    }
    return v;
}

int vx_restart_state(void)
{
    HKEY k;
    ULONGLONG t = 0, now, boot;
    DWORD cb = sizeof(t), type;
    FILETIME ft;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return 0;
    if (RegQueryValueExW(k, L"RestartNeeded", NULL, &type, (BYTE *)&t, &cb) || type != REG_QWORD) t = 0;
    RegCloseKey(k);
    if (!t) return 0;
    GetSystemTimeAsFileTime(&ft);
    now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    boot = now - (ULONGLONG)GetTickCount64() * 10000;
    return t >= boot ? 1 : -1;
}

BOOL vx_global_version(WCHAR *buf, DWORD cch)
{
    HKEY k;
    DWORD type, cb = (cch - 1) * sizeof(WCHAR);
    LONG r;
    buf[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    r = RegQueryValueExW(k, VX_REG_GLOBALVER, NULL, &type, (BYTE *)buf, &cb);
    RegCloseKey(k);
    if (r || type != REG_SZ) { buf[0] = 0; return FALSE; }
    buf[cb / sizeof(WCHAR)] = 0;
    return buf[0] != 0;
}

BOOL vx_effective_layer(const WCHAR *exepath, vx_layer *out)
{
    WCHAR layer[600], g[64];
    BOOL own = vx_lookup_layer(exepath, layer, 600);
    vx_parse_layer(own ? layer : L"", out);
    if (!out->ver && !out->real && vx_global_version(g, 64)) {
        vx_layer gl;
        vx_parse_layer(g, &gl);
        if (gl.ver) {
            if (gl.ver == &gl.custom) {
                vx_memcpy(&out->custom, &gl.custom, sizeof(out->custom));
                vx_memcpy(out->customtok, gl.customtok, sizeof(out->customtok));
                vx_memcpy(out->customcsd, gl.customcsd, sizeof(out->customcsd));
                out->custom.token = out->customtok;
                out->custom.csd = out->customcsd;
                out->ver = &out->custom;
            } else out->ver = gl.ver;
            return TRUE;
        }
    }
    return own;
}

static BOOL query(HKEY k, const WCHAR *name, WCHAR *buf, DWORD cch)
{
    DWORD type, cb = cch * sizeof(WCHAR);
    if (!k || RegQueryValueExW(k, name, NULL, &type, (BYTE *)buf, &cb) || type != REG_SZ) return FALSE;
    /* REG_SZ data is not always NUL-terminated */
    buf[cb / sizeof(WCHAR) < cch ? cb / sizeof(WCHAR) : cch - 1] = 0;
    return TRUE;
}

/* the program's own setting, else its folder's, its parent folder's... (the
 * user's before the machine's). Both keys are opened once per lookup: this
 * runs as every program VistaExt runs starts. */
BOOL vx_lookup_layer(const WCHAR *exepath, WCHAR *buf, DWORD cch)
{
    WCHAR path[MAX_PATH + 2];
    SIZE_T n;
    HKEY cu = NULL, lm = NULL;
    BOOL r = FALSE;
    RegOpenKeyExW(HKEY_CURRENT_USER, VX_REG_LAYERS, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &cu);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_LAYERS, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &lm);
    if (!cu && !lm) return FALSE;
    vx_wcscpyn(path, exepath, MAX_PATH);
    r = query(cu, path, buf, cch) || query(lm, path, buf, cch);
    /* walk up: "C:\a\b\" then "C:\a\" ... */
    n = vx_wcslen(path);
    while (!r && n > 3) {
        while (n && path[n - 1] != '\\') n--;
        if (n <= 3) break;
        path[n] = 0;                       /* keep trailing backslash */
        r = query(cu, path, buf, cch) || query(lm, path, buf, cch);
        n--;                               /* step over that backslash */
    }
    if (cu) RegCloseKey(cu);
    if (lm) RegCloseKey(lm);
    return r;
}

/* ---- writing settings (used by the tools, needs administrator rights) --- */
LONG vx_set_layer(const WCHAR *path, const WCHAR *layer)
{
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_LAYERS, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL);
    if (r) return r;
    if (layer && *layer)
        r = RegSetValueExW(k, path, 0, REG_SZ, (const BYTE *)layer, (DWORD)((vx_wcslen(layer) + 1) * sizeof(WCHAR)));
    else {
        r = RegDeleteValueW(k, path);
        if (r == ERROR_FILE_NOT_FOUND) r = 0;
    }
    RegCloseKey(k);
    return r;
}

/* Build a layer string from its parts. */
void vx_make_layer(WCHAR *out, SIZE_T cch, const vx_osver *ver, BOOL webview2, const WCHAR *args)
{
    out[0] = 0;
    if (ver) vx_wcscatn(out, ver->token, cch);
    if (webview2) { if (out[0]) vx_wcscatn(out, L" ", cch); vx_wcscatn(out, L"WEBVIEW2", cch); }
    if (args && *args) {
        if (out[0]) vx_wcscatn(out, L" ", cch);
        vx_wcscatn(out, L"ARGS:", cch);
        vx_wcscatn(out, args, cch);
    }
}

const vx_osver *vx_find_osver(const WCHAR *token)
{
    int i;
    for (i = 0; i < vx_osver_count; i++) if (!vx_wcsicmp(token, vx_osvers[i].token)) return &vx_osvers[i];
    if (!vx_wcsicmp(token, L"REAL")) return &vx_osver_real;
    return NULL;
}

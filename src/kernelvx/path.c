/* kernelvx - PathCch* and PathAlloc* (Windows 8, api-ms-win-core-path-l1-1-0):
 * the length-checked, long-path-aware successors of shlwapi's Path*. Only '\'
 * separates path parts, as in Windows' own versions. */
#include "../common/vx.h"

#define PATHCCH_ALLOW_LONG_PATHS   0x01
#define PATHCCH_MAX_CCH            0x8000
#define E_TOOSMALL   HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
#define E_TOOLONG    HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE)

static BOOL pre(const WCHAR *s, const WCHAR *p)
{
    for (; *p; s++, p++) {
        WCHAR a = *s, b = *p;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return FALSE;
    }
    return TRUE;
}

static BOOL drive(const WCHAR *s) { return ((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':'; }

/* skip "server\share\" (UNC) */
static SIZE_T unc_len(const WCHAR *s, SIZE_T start)
{
    SIZE_T i = start;
    int parts = 0;
    while (s[i] && parts < 2) {
        while (s[i] && s[i] != '\\') i++;
        parts++;
        if (s[i] == '\\') i++;
    }
    return i;
}

/* length of the root: "C:\", "C:", "\", "\\server\share\", "\\?\C:\",
 * "\\?\UNC\server\share\", "\\?\Volume{...}\" */
static SIZE_T root_len(const WCHAR *s)
{
    if (pre(s, L"\\\\?\\UNC\\")) return unc_len(s, 8);
    if (pre(s, L"\\\\?\\")) {
        SIZE_T i = 4;
        if (drive(s + 4)) return s[6] == '\\' ? 7 : 6;
        while (s[i] && s[i] != '\\') i++;
        return s[i] == '\\' ? i + 1 : i;
    }
    if (s[0] == '\\' && s[1] == '\\') return unc_len(s, 2);
    if (drive(s)) return s[2] == '\\' ? 3 : 2;
    if (s[0] == '\\') return 1;
    return 0;
}

static HRESULT canon(PWSTR out, SIZE_T cch, PCWSTR in, ULONG flags)
{
    SIZE_T n, r, o, i, max = (flags & PATHCCH_ALLOW_LONG_PATHS) ? PATHCCH_MAX_CCH : MAX_PATH;
    WCHAR *buf;
    BOOL trail;
    if (!out || !in || !cch || cch > PATHCCH_MAX_CCH) return E_INVALIDARG;
    n = vx_wcslen(in);
    if (!(buf = HeapAlloc(GetProcessHeap(), 0, (n + 4) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    r = root_len(in);
    for (o = 0; o < r; o++) buf[o] = in[o];
    if (r == 2 && drive(in)) buf[o++] = '\\';          /* "C:" -> "C:\" */
    r = o;
    trail = n > r && in[n - 1] == '\\';
    for (i = root_len(in); i < n; ) {
        SIZE_T s = i, l;
        while (i < n && in[i] != '\\') i++;
        l = i - s;
        if (i < n) i++;
        if (!l || (l == 1 && in[s] == '.')) continue;
        if (l == 2 && in[s] == '.' && in[s + 1] == '.') {   /* up one part, never above the root */
            if (o > r) {
                o--;
                while (o > r && buf[o - 1] != '\\') o--;
            }
            continue;
        }
        if (o > r && buf[o - 1] != '\\') buf[o++] = '\\';
        while (s < i && in[s] != '\\') buf[o++] = in[s++];
    }
    if (trail && o > r && buf[o - 1] != '\\') buf[o++] = '\\';
    if (!o) buf[o++] = '\\';
    buf[o] = 0;
    if (o + 1 > max) { HeapFree(GetProcessHeap(), 0, buf); return E_TOOLONG; }
    if (o + 1 > cch) { HeapFree(GetProcessHeap(), 0, buf); return E_TOOSMALL; }
    vx_memcpy(out, buf, (o + 1) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, buf);
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchCanonicalizeEx(PWSTR out, SIZE_T cch, PCWSTR in, ULONG flags) { return canon(out, cch, in, flags); }
VX_EXPORT HRESULT WINAPI vx_PathCchCanonicalize(PWSTR out, SIZE_T cch, PCWSTR in) { return canon(out, cch, in, 0); }

static HRESULT combine(PWSTR out, SIZE_T cch, PCWSTR a, PCWSTR b, ULONG flags)
{
    SIZE_T la, lb;
    WCHAR *t;
    HRESULT hr;
    if (!out || !cch) return E_INVALIDARG;
    if (!a && !b) return E_INVALIDARG;
    if (!b || !*b) return canon(out, cch, a ? a : L"", flags);
    if (!a || !*a || root_len(b)) {
        /* "\x" keeps the drive of a, like PathCombine */
        if (a && *a && b[0] == '\\' && b[1] != '\\' && drive(a)) {
            WCHAR d[3] = { a[0], ':', 0 };
            la = 2;
            lb = vx_wcslen(b);
            if (!(t = HeapAlloc(GetProcessHeap(), 0, (la + lb + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
            vx_memcpy(t, d, la * sizeof(WCHAR));
            vx_memcpy(t + la, b, (lb + 1) * sizeof(WCHAR));
            hr = canon(out, cch, t, flags);
            HeapFree(GetProcessHeap(), 0, t);
            return hr;
        }
        return canon(out, cch, b, flags);
    }
    la = vx_wcslen(a);
    lb = vx_wcslen(b);
    if (!(t = HeapAlloc(GetProcessHeap(), 0, (la + lb + 2) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    vx_memcpy(t, a, la * sizeof(WCHAR));
    if (la && t[la - 1] != '\\') t[la++] = '\\';
    vx_memcpy(t + la, b, (lb + 1) * sizeof(WCHAR));
    hr = canon(out, cch, t, flags);
    HeapFree(GetProcessHeap(), 0, t);
    return hr;
}

VX_EXPORT HRESULT WINAPI vx_PathCchCombineEx(PWSTR out, SIZE_T cch, PCWSTR a, PCWSTR b, ULONG flags) { return combine(out, cch, a, b, flags); }
VX_EXPORT HRESULT WINAPI vx_PathCchCombine(PWSTR out, SIZE_T cch, PCWSTR a, PCWSTR b) { return combine(out, cch, a, b, 0); }

VX_EXPORT HRESULT WINAPI vx_PathCchAppendEx(PWSTR path, SIZE_T cch, PCWSTR more, ULONG flags)
{
    WCHAR *t;
    HRESULT hr;
    SIZE_T n;
    if (!path || !cch) return E_INVALIDARG;
    n = vx_wcslen(path);
    if (!(t = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    vx_memcpy(t, path, (n + 1) * sizeof(WCHAR));
    /* an absolute "more" is appended as a relative part, like Windows does */
    if (more) while (*more == '\\' && more[1] != '\\') more++;
    hr = combine(path, cch, t, more, flags);
    HeapFree(GetProcessHeap(), 0, t);
    return hr;
}
VX_EXPORT HRESULT WINAPI vx_PathCchAppend(PWSTR path, SIZE_T cch, PCWSTR more) { return vx_PathCchAppendEx(path, cch, more, 0); }

VX_EXPORT HRESULT WINAPI vx_PathCchAddBackslashEx(PWSTR path, SIZE_T cch, PWSTR *end, SIZE_T *rem)
{
    SIZE_T n;
    if (!path || !cch) return E_INVALIDARG;
    n = vx_wcslen(path);
    if (n >= cch) return E_INVALIDARG;
    if (n && path[n - 1] == '\\') {
        if (end) *end = path + n;
        if (rem) *rem = cch - n;
        return S_FALSE;
    }
    if (n + 2 > cch) { if (end) *end = NULL; if (rem) *rem = 0; return E_TOOSMALL; }
    path[n] = '\\';
    path[n + 1] = 0;
    if (end) *end = path + n + 1;
    if (rem) *rem = cch - n - 1;
    return S_OK;
}
VX_EXPORT HRESULT WINAPI vx_PathCchAddBackslash(PWSTR path, SIZE_T cch) { return vx_PathCchAddBackslashEx(path, cch, NULL, NULL); }

VX_EXPORT HRESULT WINAPI vx_PathCchRemoveBackslashEx(PWSTR path, SIZE_T cch, PWSTR *end, SIZE_T *rem)
{
    SIZE_T n;
    if (!path || !cch) return E_INVALIDARG;
    n = vx_wcslen(path);
    if (n >= cch) return E_INVALIDARG;
    if (n && path[n - 1] == '\\' && n > root_len(path)) {
        path[--n] = 0;
        if (end) *end = path + n;
        if (rem) *rem = cch - n;
        return S_OK;
    }
    if (end) *end = path + (n && path[n - 1] == '\\' ? n - 1 : n);
    if (rem) *rem = cch - (end ? (SIZE_T)(*end - path) : n);
    return S_FALSE;
}
VX_EXPORT HRESULT WINAPI vx_PathCchRemoveBackslash(PWSTR path, SIZE_T cch) { return vx_PathCchRemoveBackslashEx(path, cch, NULL, NULL); }

VX_EXPORT HRESULT WINAPI vx_PathCchRemoveFileSpec(PWSTR path, SIZE_T cch)
{
    SIZE_T n, r, i;
    if (!path || !cch) return E_INVALIDARG;
    n = vx_wcslen(path);
    r = root_len(path);
    if (n <= r) return S_FALSE;
    for (i = n; i > r && path[i - 1] != '\\'; i--) ;
    if (i > r) i--;                     /* drop the backslash too, but keep the root's */
    path[i] = 0;
    return S_OK;
}

static WCHAR *ext_of(PWSTR path)
{
    SIZE_T n = vx_wcslen(path), i;
    for (i = n; i > 0 && path[i - 1] != '\\'; i--)
        if (path[i - 1] == '.') return path + i - 1;
    return path + n;
}

VX_EXPORT HRESULT WINAPI vx_PathCchFindExtension(PWSTR path, SIZE_T cch, PWSTR *ext)
{
    if (!ext) return E_INVALIDARG;
    *ext = NULL;
    if (!path || !cch || vx_wcslen(path) >= cch) return E_INVALIDARG;
    *ext = ext_of(path);
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchRemoveExtension(PWSTR path, SIZE_T cch)
{
    WCHAR *e;
    if (!path || !cch || vx_wcslen(path) >= cch) return E_INVALIDARG;
    e = ext_of(path);
    if (!*e) return S_FALSE;
    *e = 0;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchAddExtension(PWSTR path, SIZE_T cch, PCWSTR ext)
{
    SIZE_T n, l;
    BOOL dot;
    if (!path || !cch || !ext) return E_INVALIDARG;
    n = vx_wcslen(path);
    if (n >= cch) return E_INVALIDARG;
    dot = ext[0] == '.';
    l = vx_wcslen(ext);
    if (!l || (dot && l == 1)) return S_OK;
    if (*ext_of(path)) return S_FALSE;
    if (n + l + (dot ? 0 : 1) + 1 > cch) return E_TOOSMALL;
    if (!dot) path[n++] = '.';
    vx_memcpy(path + n, ext, (l + 1) * sizeof(WCHAR));
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchRenameExtension(PWSTR path, SIZE_T cch, PCWSTR ext)
{
    HRESULT hr = vx_PathCchRemoveExtension(path, cch);
    if (FAILED(hr)) return hr;
    hr = vx_PathCchAddExtension(path, cch, ext);
    return FAILED(hr) ? hr : S_OK;
}

VX_EXPORT BOOL WINAPI vx_PathCchIsRoot(PCWSTR path)
{
    SIZE_T r, n;
    if (!path || !*path) return FALSE;
    r = root_len(path);
    n = vx_wcslen(path);
    /* "C:\", "\", "\\server\share" (with or without the final backslash) */
    return r && r == n && !(r == 2 && drive(path));
}

VX_EXPORT HRESULT WINAPI vx_PathCchSkipRoot(PCWSTR path, PCWSTR *after)
{
    SIZE_T r;
    if (!path || !after) return E_INVALIDARG;
    r = root_len(path);
    if (!r) return E_INVALIDARG;
    *after = path + r;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchStripToRoot(PWSTR path, SIZE_T cch)
{
    SIZE_T r;
    if (!path || !cch || vx_wcslen(path) >= cch) return E_INVALIDARG;
    r = root_len(path);
    if (!r) return E_INVALIDARG;
    if (!path[r]) return S_FALSE;
    path[r] = 0;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathCchStripPrefix(PWSTR path, SIZE_T cch)
{
    SIZE_T n;
    if (!path || !cch || (n = vx_wcslen(path)) >= cch) return E_INVALIDARG;
    if (pre(path, L"\\\\?\\UNC\\")) {          /* \\?\UNC\server -> \\server */
        memmove(path + 2, path + 8, (n - 8 + 1) * sizeof(WCHAR));
        return S_OK;
    }
    if (pre(path, L"\\\\?\\") && drive(path + 4)) {
        memmove(path, path + 4, (n - 4 + 1) * sizeof(WCHAR));
        return S_OK;
    }
    return S_FALSE;
}

static HRESULT alloc_result(const WCHAR *tmp, PWSTR *out)
{
    SIZE_T l = vx_wcslen(tmp) + 1;
    if (!(*out = LocalAlloc(LMEM_FIXED, l * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    vx_memcpy(*out, tmp, l * sizeof(WCHAR));
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PathAllocCanonicalize(PCWSTR in, ULONG flags, PWSTR *out)
{
    WCHAR *t;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!in) return E_INVALIDARG;
    if (!(t = HeapAlloc(GetProcessHeap(), 0, PATHCCH_MAX_CCH * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    hr = canon(t, PATHCCH_MAX_CCH, in, flags);
    if (SUCCEEDED(hr)) hr = alloc_result(t, out);
    HeapFree(GetProcessHeap(), 0, t);
    return hr;
}

VX_EXPORT HRESULT WINAPI vx_PathAllocCombine(PCWSTR a, PCWSTR b, ULONG flags, PWSTR *out)
{
    WCHAR *t;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(t = HeapAlloc(GetProcessHeap(), 0, PATHCCH_MAX_CCH * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    hr = combine(t, PATHCCH_MAX_CCH, a, b, flags);
    if (SUCCEEDED(hr)) hr = alloc_result(t, out);
    HeapFree(GetProcessHeap(), 0, t);
    return hr;
}

VX_EXPORT BOOL WINAPI vx_PathIsUNCEx(PCWSTR path, PCWSTR *server)
{
    if (server) *server = NULL;
    if (!path) return FALSE;
    if (pre(path, L"\\\\?\\UNC\\")) { if (server) *server = path + 8; return TRUE; }
    if (path[0] == '\\' && path[1] == '\\' && path[2] != '?') { if (server) *server = path + 2; return TRUE; }
    return FALSE;
}

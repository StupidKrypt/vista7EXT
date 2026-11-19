/* VistaExt patch library.
 *
 * How a program is made to use VistaExt: each import descriptor names a DLL
 * ("kernel32.dll"). If the program needs a function this Windows' kernel32
 * lacks, the name is rewritten in place to the companion ("kernelvx.dll"),
 * which exports everything kernel32 has (as forwarders) plus the additions.
 * Companion names are never longer than the originals, so no new sections
 * are needed and the file layout is unchanged. The original is kept as
 * <file>.vxorig so it can be restored. */
#include "patchlib.h"
#include "../common/companions.h"

/* ordinals: the companion exports the original's ordinal numbers too, so
 * imports by number can be switched (see ../common/companions.h) */
static const struct { const char *orig, *comp; BOOL ordinals; } g_map[] = {
#define VX_COMPANION(dll, comp, numbers) { #dll ".dll", #comp ".dll", numbers },
    VX_COMPANIONS
#undef VX_COMPANION
};
#define NMAP (sizeof(g_map) / sizeof(g_map[0]))

/* ---- logging ------------------------------------------------------------- */
typedef struct { vxp_log_fn fn; void *ctx; } logger;

static void vxlog(logger *l, const WCHAR *fmt, ...)
{
    WCHAR buf[1100];
    va_list ap;
    if (!l->fn) return;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    l->fn(l->ctx, buf);
}

/* ---- small helpers ------------------------------------------------------- */
static int scmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void *xalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static BYTE *read_file(const WCHAR *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD hi, got;
    BYTE *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(h, &hi);
    if (hi || *size < 0x200 || *size > 0x40000000 || !(buf = xalloc(*size))) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, *size, &got, NULL) || got != *size) { xfree(buf); CloseHandle(h); return NULL; }
    CloseHandle(h);
    return buf;
}

static BOOL file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- PE parsing ---------------------------------------------------------- */
typedef struct {
    BYTE *d;
    DWORD size;
    BOOL is64;
    ULONGLONG base;
    DWORD nt;                  /* offset of "PE\0\0" */
    IMAGE_SECTION_HEADER *sec;
    WORD nsec;
    IMAGE_DATA_DIRECTORY *dir;
    DWORD ndir;
    DWORD checksum_off;
    DWORD opt;                 /* offset of the optional header */
} pe;

static BOOL pe_open(pe *p, BYTE *d, DWORD size)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)d;
    IMAGE_FILE_HEADER *fh;
    WORD magic;
    DWORD opt;
    vx_memset(p, 0, sizeof(*p));
    if (size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    if (dos->e_lfanew < 0 || (DWORD)dos->e_lfanew + 0x108 > size || *(DWORD *)(d + dos->e_lfanew) != IMAGE_NT_SIGNATURE) return FALSE;
    p->d = d; p->size = size; p->nt = dos->e_lfanew;
    fh = (IMAGE_FILE_HEADER *)(d + p->nt + 4);
    opt = p->nt + 4 + sizeof(*fh);
    p->opt = opt;
    magic = *(WORD *)(d + opt);
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 *o = (void *)(d + opt);
        p->is64 = TRUE; p->base = o->ImageBase; p->dir = o->DataDirectory; p->ndir = o->NumberOfRvaAndSizes;
        p->checksum_off = opt + 64;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 *o = (void *)(d + opt);
        p->base = o->ImageBase; p->dir = o->DataDirectory; p->ndir = o->NumberOfRvaAndSizes;
        p->checksum_off = opt + 64;
    } else return FALSE;
    if (p->ndir > 16) p->ndir = 16;
    p->nsec = fh->NumberOfSections;
    p->sec = (IMAGE_SECTION_HEADER *)(d + opt + fh->SizeOfOptionalHeader);
    if ((BYTE *)(p->sec + p->nsec) > d + size) return FALSE;
    return TRUE;
}

/* file offset for an RVA, or 0 if outside the file */
static DWORD rva2off(pe *p, DWORD rva)
{
    WORD i;
    for (i = 0; i < p->nsec; i++) {
        IMAGE_SECTION_HEADER *s = &p->sec[i];
        DWORD vs = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (rva >= s->VirtualAddress && rva < s->VirtualAddress + vs) {
            DWORD off = rva - s->VirtualAddress;
            if (off >= s->SizeOfRawData) return 0;
            off += s->PointerToRawData;
            return off < p->size ? off : 0;
        }
    }
    return rva < p->size && rva < 0x1000 ? rva : 0;
}

static const char *pe_str(pe *p, DWORD rva)
{
    DWORD o = rva2off(p, rva), i;
    if (!o) return NULL;
    for (i = o; i < p->size; i++) if (!p->d[i]) return (const char *)p->d + o;
    return NULL;
}

/* ---- export sets (cached) -------------------------------------------------
 * One entry per DLL path looked at (system, program folder, companions). The
 * cache grows as needed: a large program can import from 100+ DLLs. */
typedef struct {
    WCHAR path[MAX_PATH];
    BOOL found;
    DWORD n;
    char **names;   /* sorted */
    char **fwd;     /* forwarder target "DLL.Function", or NULL if implemented */
    DWORD ordbase, nords;
    BYTE *ords;     /* ords[i]: ordinal ordbase+i is exported */
    BOOL vistaext;  /* one of VistaExt's own DLLs (has the ownership marker) */
} expset;

static expset **g_cache;
static DWORD g_ncache, g_capcache;

static void sort_names(char **a, char **f, DWORD n)
{
    /* shell sort keeps this dependency-free */
    DWORD gap, i, j;
    for (gap = n / 2; gap; gap /= 2)
        for (i = gap; i < n; i++) {
            char *t = a[i], *tf = f[i];
            for (j = i; j >= gap && scmp(a[j - gap], t) > 0; j -= gap) { a[j] = a[j - gap]; f[j] = f[j - gap]; }
            a[j] = t; f[j] = tf;
        }
}

static expset *get_exports(const WCHAR *path)
{
    DWORD i, size;
    BYTE *d;
    pe p;
    expset *e;
    for (i = 0; i < g_ncache; i++) if (!vx_wcsicmp(g_cache[i]->path, path)) return g_cache[i];
    if (g_ncache == g_capcache) {
        DWORD cap = g_capcache ? g_capcache * 2 : 64;
        expset **nc = g_cache ? HeapReAlloc(GetProcessHeap(), 0, g_cache, cap * sizeof(*nc)) : xalloc(cap * sizeof(*nc));
        if (!nc) return NULL;
        g_cache = nc;
        g_capcache = cap;
    }
    if (!(e = xalloc(sizeof(*e)))) return NULL;
    vx_memset(e, 0, sizeof(*e));
    vx_wcscpyn(e->path, path, MAX_PATH);
    g_cache[g_ncache++] = e;
    if (!(d = read_file(path, &size))) return e;
    e->found = TRUE;
    {   /* the marker from src/common/dll.rc */
        static const char mk[] = "\nVistaExt-owned-file\n";
        DWORD k;
        for (k = 0; k + sizeof(mk) - 1 <= size && !e->vistaext; k++)
            if (d[k] == '\n' && !memcmp(d + k, mk, sizeof(mk) - 1)) e->vistaext = TRUE;
    }
    if (pe_open(&p, d, size) && p.ndir > 0 && p.dir[0].VirtualAddress) {
        DWORD erva = p.dir[0].VirtualAddress, esz = p.dir[0].Size, eo = rva2off(&p, erva);
        IMAGE_EXPORT_DIRECTORY *ed = eo && eo + sizeof(IMAGE_EXPORT_DIRECTORY) <= size ? (void *)(d + eo) : NULL;
        if (ed) {
            DWORD no = rva2off(&p, ed->AddressOfNames), oo = rva2off(&p, ed->AddressOfNameOrdinals),
                  fo = rva2off(&p, ed->AddressOfFunctions);
            DWORD n = ed->NumberOfNames, total = 0;
            if (no && oo && fo && n < 0x100000 && no + n * 4 <= size && oo + n * 2 <= size &&
                fo + ed->NumberOfFunctions * 4 <= size) {
                for (i = 0; i < n; i++) {
                    const char *s = pe_str(&p, ((DWORD *)(d + no))[i]);
                    WORD ord = ((WORD *)(d + oo))[i];
                    DWORD frva = ord < ed->NumberOfFunctions ? ((DWORD *)(d + fo))[ord] : 0;
                    if (!s) continue;
                    total += (DWORD)vx_strlen(s) + 1;
                    if (frva >= erva && frva < erva + esz && (s = pe_str(&p, frva))) total += (DWORD)vx_strlen(s) + 1;
                }
                e->names = xalloc(n * 2 * sizeof(char *) + total);
                if (e->names) {
                    char *blob;
                    e->fwd = e->names + n;
                    blob = (char *)(e->fwd + n);
                    for (i = 0; i < n; i++) {
                        const char *s = pe_str(&p, ((DWORD *)(d + no))[i]), *f = NULL;
                        WORD ord = ((WORD *)(d + oo))[i];
                        DWORD frva = ord < ed->NumberOfFunctions ? ((DWORD *)(d + fo))[ord] : 0;
                        SIZE_T l;
                        if (!s) continue;
                        if (frva >= erva && frva < erva + esz) f = pe_str(&p, frva);
                        l = vx_strlen(s) + 1;
                        vx_memcpy(blob, s, l);
                        e->names[e->n] = blob;
                        blob += l;
                        e->fwd[e->n] = NULL;
                        if (f) {
                            l = vx_strlen(f) + 1;
                            vx_memcpy(blob, f, l);
                            e->fwd[e->n] = blob;
                            blob += l;
                        }
                        e->n++;
                    }
                    sort_names(e->names, e->fwd, e->n);
                }
                if (ed->NumberOfFunctions < 0x10000 && (e->ords = xalloc(ed->NumberOfFunctions + 1))) {
                    e->ordbase = ed->Base;
                    e->nords = ed->NumberOfFunctions;
                    for (i = 0; i < e->nords; i++) e->ords[i] = ((DWORD *)(d + fo))[i] != 0;
                }
            }
        }
    }
    xfree(d);
    return e;
}

static int find(expset *e, const char *name)
{
    DWORD lo = 0, hi = e ? e->n : 0;
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        int c = scmp(e->names[mid], name);
        if (!c) return (int)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* -1 = not exported, 0 = implemented, 1 = forwarder */
static int lookup(expset *e, const char *name)
{
    int i = find(e, name);
    return i < 0 ? -1 : e->fwd[i] ? 1 : 0;
}

/* ---- system directory for an image's bitness ----------------------------- */
static void sys_dir(BOOL image64, WCHAR *out)
{
    BOOL wow = FALSE;
    out[0] = 0;
    if (GetEnvironmentVariableW(L"VX_SYSDIR", out, MAX_PATH)) return;    /* tests */
    IsWow64Process(GetCurrentProcess(), &wow);
#ifdef _WIN64
    if (image64) GetSystemDirectoryW(out, MAX_PATH);
    else GetSystemWow64DirectoryW(out, MAX_PATH);
#else
    if (image64 && wow) {                       /* 32-bit tool, 64-bit program */
        GetWindowsDirectoryW(out, MAX_PATH);
        vx_wcscatn(out, L"\\Sysnative", MAX_PATH);
    } else GetSystemDirectoryW(out, MAX_PATH);   /* WOW64 redirects to SysWOW64 */
#endif
}

static void join(WCHAR *out, const WCHAR *dir, const char *name)
{
    SIZE_T n;
    vx_wcscpyn(out, dir, MAX_PATH);
    vx_wcscatn(out, L"\\", MAX_PATH);
    n = vx_wcslen(out);
    while (*name && n + 1 < MAX_PATH) out[n++] = (unsigned char)*name++;
    out[n] = 0;
}

static BOOL starts_i(const char *s, const char *pre)
{
    while (*pre) if ((*s++ | 32) != (*pre++ | 32)) return FALSE;
    return TRUE;
}

static BOOL is_apiset(const char *n) { return starts_i(n, "api-") || starts_i(n, "ext-"); }

/* Known DLLs: always loaded from System32, whatever a program ships */
static BOOL is_known(const char *n)
{
    static const char *k[] = { "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
        "sechost.dll", "rpcrt4.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "comdlg32.dll",
        "msvcrt.dll", "ws2_32.dll", "nsi.dll", "psapi.dll", "setupapi.dll", "imagehlp.dll", "normaliz.dll", "lpk.dll",
        "usp10.dll", "wldap32.dll", "difxapi.dll", "clbcatq.dll", "imm32.dll", "msctf.dll" };
    unsigned i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (!vx_stricmp(n, k[i])) return TRUE;
    return FALSE;
}

/* ---- one import descriptor ----------------------------------------------- */
typedef struct {
    pe *p;
    logger *log;
    const WCHAR *sysdir, *appdir;
    DWORD flags;
    BOOL changed;
    int unsupported;
    BOOL header_done;
    const WCHAR *path;
    BOOL survey;        /* first pass: only find out which DLLs need a companion */
    DWORD need;         /* bit per g_map entry: some import table of it needs one */
} job;

/* Where the loader would take <dll> from for this program: Known DLLs from
 * System32; API sets Windows itself has (Windows 7's built-in ones, whose
 * stub files are in System32) from there; any other DLL, including API sets
 * that are just files (VistaExt's, or Vista's), from the program's folder
 * first, as the loader searches. Returns NULL if it is in neither place. */
static expset *locate(job *j, const char *dll)
{
    WCHAR sp[MAX_PATH], ap[MAX_PATH];
    expset *s, *a;
    join(sp, j->sysdir, dll);
    s = get_exports(sp);
    if (is_known(dll) || !j->appdir[0]) return s && s->found ? s : NULL;
    join(ap, j->appdir, dll);
    if (is_apiset(dll) && s && s->found && !s->vistaext) return s;
    a = file_exists(ap) ? get_exports(ap) : NULL;
    if (a && a->found) return a;
    return s && s->found ? s : NULL;
}

/* Does <dll>!<name> exist for this program? Follows forwarders, so a name a
 * DLL only forwards to a function this Windows lacks counts as missing (the
 * C runtime update's API-set DLLs forward to kernel32 functions that only
 * newer Windows has). */
static BOOL resolves(job *j, const char *dll, const char *name, int depth)
{
    char buf[100];
    const char *f, *dot, *q;
    expset *e;
    SIZE_T dl = vx_strlen(dll);
    int i;
    if (depth > 6) return TRUE;                 /* a long chain: assume it is fine */
    if (dl + 5 >= sizeof(buf)) return FALSE;
    vx_memcpy(buf, dll, dl + 1);
    if (dl < 4 || vx_stricmp(buf + dl - 4, ".dll")) vx_memcpy(buf + dl, ".dll", 5);
    e = locate(j, buf);
    /* Windows' own DLLs forward to API sets that its loader resolves itself */
    if (!e || !e->found) return depth > 0 && is_apiset(buf);
    if (name[0] == '#') return TRUE;            /* forwarded by number */
    if ((i = find(e, name)) < 0) return FALSE;
    if (!(f = e->fwd[i])) return TRUE;
    for (dot = NULL, q = f; *q; q++) if (*q == '.') dot = q;
    if (!dot || dot == f || (SIZE_T)(dot - f) >= sizeof(buf)) return FALSE;
    vx_memcpy(buf, f, (SIZE_T)(dot - f));
    buf[dot - f] = 0;
    return resolves(j, buf, dot + 1, depth + 1);
}

static void header(job *j)
{
    if (!j->header_done) { vxlog(j->log, L"%s", j->path); j->header_done = TRUE; }
}

/* Read the names imported through a thunk array. */
static DWORD thunk_names(pe *p, DWORD rva, const char **names, DWORD max, DWORD *ordinals)
{
    DWORD o = rva2off(p, rva), n = 0;
    *ordinals = 0;
    if (!o) return 0;
    for (;;) {
        ULONGLONG v;
        if (o + (p->is64 ? 8 : 4) > p->size) break;
        v = p->is64 ? *(ULONGLONG *)(p->d + o) : *(DWORD *)(p->d + o);
        if (!v) break;
        if (p->is64 ? (v >> 63) : (v >> 31)) (*ordinals)++;
        else if (n < max) {
            const char *s = pe_str(p, (DWORD)(v & 0x7FFFFFFF) + 2);
            if (s) names[n++] = s;
        }
        o += p->is64 ? 8 : 4;
    }
    return n;
}

#define MAXNAMES 4096
static const char *g_names[MAXNAMES];

static void list_names(job *j, const WCHAR *prefix, const char **names, DWORD n)
{
    WCHAR line[1024];
    DWORD i, k = 0;
    SIZE_T len;
    vx_wcscpyn(line, prefix, 1024);
    len = vx_wcslen(line);
    for (i = 0; i < n && k < 12; i++) {
        const char *s;
        if (!names[i]) continue;
        if (k) { line[len++] = ','; line[len++] = ' '; }
        for (s = names[i]; *s && len < 1000; s++) line[len++] = (unsigned char)*s;
        k++;
    }
    if (n > k) { line[len] = 0; vx_wcscatn(line, L", ...", 1024); }
    else line[len] = 0;
    vxlog(j->log, L"%s", line);
}

/* API-set imports ("api-ms-win-core-synch-l1-2-0.dll"). On Vista they are
 * files (VistaExt's, or the Universal C Runtime update's); Windows 7 has the
 * older ones built in and finds newer ones as files. When this Windows
 * cannot provide every function imported through one (for example the C
 * Runtime update's own copy lacks the Windows 8 functions), the import is
 * pointed at the VistaExt DLL that has them all. Its name is shorter, so it
 * fits in place. */
static const char *g_apiset_hosts[] = { "kernelvx.dll", "advapivx.dll", "uservx.dll", "shellvx.dll", "combase.dll", "shcore.dll",
                                        "olevx.dll", "ntvx.dll", "ws2vx.dll", "cfgmgrvx.dll" };

static void do_apiset(job *j, DWORD name_rva, DWORD thunk_rva, BOOL delay, const char *dll, const WCHAR *wdll)
{
    DWORD n, ord, i, missing = 0, w = 0;
    unsigned t;
    /* a copy shipped with the program is checked like Windows' own: older
     * redistributable copies forward to functions only newer Windows has */
    n = thunk_names(j->p, thunk_rva, g_names, MAXNAMES, &ord);
    for (i = 0; i < n; i++) if (!resolves(j, dll, g_names[i], 0)) missing++;
    if (!missing) return;
    header(j);
    if (starts_i(dll, "api-ms-win-crt-")) {
        vxlog(j->log, L"  %s: needs the Universal C Runtime (Windows update KB2999226)", wdll);
        j->unsupported += (int)missing;
        return;
    }
    for (t = 0; !ord && t < sizeof(g_apiset_hosts) / sizeof(g_apiset_hosts[0]); t++) {
        const char *c = g_apiset_hosts[t];
        if (vx_strlen(c) > vx_strlen(dll)) continue;
        for (i = 0; i < n && resolves(j, c, g_names[i], 0); i++) ;
        if (i < n) continue;
        vxlog(j->log, L"  %s -> %hs (%d function(s) this Windows lacks%s)", wdll, c, (int)missing,
              delay ? L", loaded on demand" : L"");
        vx_memcpy(j->p->d + rva2off(j->p, name_rva), c, vx_strlen(c) + 1);
        j->changed = TRUE;
        return;
    }
    for (i = 0; i < n; i++) if (!resolves(j, dll, g_names[i], 0)) g_names[w++] = g_names[i];
    list_names(j, L"  ", g_names, w);
    vxlog(j->log, L"    ^ %d missing from %s%s", (int)w, wdll,
          ord ? L" (it also imports by number, so it is left unchanged)" : L", no VistaExt replacement");
    j->unsupported += (int)w;
}

static BOOL has_ordinal(expset *e, DWORD o)
{
    return e && e->ords && o >= e->ordbase && o - e->ordbase < e->nords && e->ords[o - e->ordbase];
}

/* every ordinal imported through this table exists in both DLLs */
static BOOL ordinals_ok(job *j, DWORD thunk_rva, expset *real, expset *comp)
{
    pe *p = j->p;
    DWORD o = rva2off(p, thunk_rva);
    if (!o) return FALSE;
    for (;;) {
        ULONGLONG v;
        if (o + (p->is64 ? 8 : 4) > p->size) return FALSE;
        v = p->is64 ? *(ULONGLONG *)(p->d + o) : *(DWORD *)(p->d + o);
        if (!v) return TRUE;
        if ((p->is64 ? (v >> 63) : (v >> 31)) &&
            (!has_ordinal(comp, (DWORD)(v & 0xFFFF)) || (real && real->found && !has_ordinal(real, (DWORD)(v & 0xFFFF)))))
            return FALSE;
        o += p->is64 ? 8 : 4;
    }
}

/* name_rva: where the DLL name string lives (rewritten when redirecting) */
static void do_descriptor(job *j, DWORD name_rva, DWORD thunk_rva, BOOL delay)
{
    pe *p = j->p;
    const char *dll = pe_str(p, name_rva);
    WCHAR path[MAX_PATH], wdll[64];
    expset *real, *comp = NULL;
    DWORD n, ord, i, missing = 0, notexp = 0, uncovered = 0;
    int m = -1;
    unsigned k;
    if (!dll || !thunk_rva) return;
    for (k = 0; k < NMAP; k++) if (!vx_stricmp(dll, g_map[k].orig)) m = (int)k;
    for (i = 0; dll[i] && i < 63; i++) wdll[i] = (unsigned char)dll[i];
    wdll[i] = 0;

    real = m >= 0 ? NULL : locate(j, dll);
    if (m >= 0) { join(path, j->sysdir, dll); real = get_exports(path); }
    if (j->survey) {
        /* A program can import one DLL through several tables (common with
         * mixed import libraries). If any of them needs the companion, all of
         * them are switched, so the program gets VistaExt's GetProcAddress,
         * LoadLibraryEx and version functions whichever table they are in. */
        if (m >= 0 && real && real->found) {
            n = thunk_names(p, thunk_rva, g_names, MAXNAMES, &ord);
            for (i = 0; i < n; i++) if (lookup(real, g_names[i]) < 0) { j->need |= 1u << m; break; }
        }
        return;
    }
    if (m < 0 && is_apiset(dll)) { do_apiset(j, name_rva, thunk_rva, delay, dll, wdll); return; }
    if (!real || !real->found) {
        join(path, j->appdir, dll);
        if (m < 0 && file_exists(path)) return;              /* ships with the program */
        if (m < 0) {
            header(j);
            vxlog(j->log, L"  %s: DLL not found on this system%s", wdll, delay ? L" (loaded on demand)" : L"");
            j->unsupported++;
            return;
        }
    }

    n = thunk_names(p, thunk_rva, g_names, MAXNAMES, &ord);
    for (i = 0; i < n; i++) if (lookup(real, g_names[i]) < 0) missing++;

    if (m < 0) {
        if (missing) {
            const char **miss = g_names;      /* compact the missing ones in place */
            DWORD w = 0;
            for (i = 0; i < n; i++) if (lookup(real, g_names[i]) < 0) miss[w++] = g_names[i];
            header(j);
            list_names(j, L"  ", miss, w);
            vxlog(j->log, L"    ^ %d missing from %s, no VistaExt replacement", (int)w, wdll);
            j->unsupported += (int)w;
        }
        return;
    }

    if (!missing && !(j->need & (1u << m)) && !((j->flags & VXP_FORCE) && !vx_stricmp(dll, "kernel32.dll")) &&
        !((j->flags & VXP_LAYER) && !vx_stricmp(dll, "version.dll"))) return;

    {
        WCHAR cdll[64];
        const char *c = g_map[m].comp;
        for (i = 0; c[i]; i++) cdll[i] = (unsigned char)c[i];
        cdll[i] = 0;
        join(path, j->sysdir, c);
        comp = get_exports(path);
        header(j);
        if (!comp || !comp->found) {
            vxlog(j->log, L"  %s: %d functions missing, but %s is not installed", wdll, (int)missing, cdll);
            j->unsupported += (int)missing;
            return;
        }
        for (i = 0; i < n; i++) {
            int inreal = lookup(real, g_names[i]) >= 0, c2 = lookup(comp, g_names[i]);
            if (c2 < 0) { notexp++; if (inreal) continue; }
            if (!inreal && !resolves(j, c, g_names[i], 0)) { uncovered++; continue; }
            g_names[i] = NULL;    /* fine: keep only problem names for the listing */
        }
        if (uncovered || notexp) {
            const char **bad = g_names;
            DWORD w = 0;
            for (i = 0; i < n; i++) if (g_names[i]) bad[w++] = g_names[i];
            list_names(j, L"  still missing: ", bad, w);
            j->unsupported += (int)uncovered;
        }
        if (ord && !(g_map[m].ordinals && ordinals_ok(j, thunk_rva, real, comp))) {
            vxlog(j->log, L"  %s: imports %d functions by number; left unchanged", wdll, (int)ord);
            return;
        }
        if (notexp) {
            vxlog(j->log, L"  %s: %s lacks %d of its functions; left unchanged", wdll, cdll, (int)notexp);
            return;
        }
        if (missing)
            vxlog(j->log, L"  %s -> %s (%d of %d functions added by VistaExt%s)", wdll, cdll,
                 (int)(missing - uncovered), (int)missing, delay ? L", loaded on demand" : L"");
        else if (j->need & (1u << m))
            vxlog(j->log, L"  %s -> %s (more functions from %s, switched along)", wdll, cdll, wdll);
        else
            vxlog(j->log, L"  %s -> %s (for the compatibility settings)", wdll, cdll);
        {
            DWORD o = rva2off(p, name_rva);
            SIZE_T l = vx_strlen(c) + 1;
            vx_memcpy(p->d + o, c, l);
        }
        j->changed = TRUE;
    }
}

/* PE checksum (as computed by imagehlp!CheckSumMappedFile) */
static DWORD pe_checksum(BYTE *d, DWORD size, DWORD csoff)
{
    ULONGLONG sum = 0;
    DWORD i;
    for (i = 0; i + 1 < size; i += 2) {
        if (i == csoff || i == csoff + 2) continue;
        sum += *(WORD *)(d + i);
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if (size & 1) { sum += d[size - 1]; sum = (sum & 0xFFFF) + (sum >> 16); }
    sum = (sum & 0xFFFF) + (sum >> 16);
    return (DWORD)sum + size;
}

/* Read-only files (programs copied from a CD, some installers) are made
 * writable for the moment and get the attribute back afterwards. */
static DWORD unprotect(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
        return a;
    }
    return 0;
}

static BOOL write_file(const WCHAR *path, BYTE *d, DWORD size)
{
    DWORD attr = unprotect(path), put, err;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    BOOL ok = FALSE;
    if (h != INVALID_HANDLE_VALUE) {
        ok = WriteFile(h, d, size, &put, NULL) && put == size && SetEndOfFile(h);
        CloseHandle(h);
    }
    err = GetLastError();
    if (attr) SetFileAttributesW(path, attr);
    SetLastError(err);
    return ok;
}

/* Is the file (d, size) the backup b itself, or b with VistaExt's changes?
 * Patching changes a few bytes only (DLL names, bound imports, checksum). A
 * program update changes the size or far more bytes: then b is outdated. */
static BOOL same_but_patched(const BYTE *d, DWORD size, const WCHAR *b)
{
    DWORD bsize, i, diff = 0;
    BYTE *o = read_file(b, &bsize);
    BOOL ok;
    if (!o) return FALSE;
    ok = bsize == size;
    for (i = 0; ok && i < size; i++) if (d[i] != o[i] && ++diff > 4096) ok = FALSE;
    xfree(o);
    return ok;
}

/* Patched files are listed under HKLM\SOFTWARE\VistaExt\Patched so that
 * "vxpatch /undoall" (run by the uninstaller) can put every one back. */
#define VX_REG_PATCHED VX_REG_ROOT L"\\Patched"

/* vxauto works on files in a user's own folders AS that user (so that the
 * user cannot trick the SYSTEM service into writing elsewhere); the list of
 * patched files is still written as the service itself. */
HANDLE vxp_unimpersonate(void)
{
    HANDLE t = NULL;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &t)) return NULL;
    RevertToSelf();
    return t;
}

void vxp_reimpersonate(HANDLE t)
{
    if (!t) return;
    SetThreadToken(NULL, t);
    CloseHandle(t);
}

static void record_patched(const WCHAR *path, BOOL add)
{
    HKEY k;
    HANDLE imp = vxp_unimpersonate();
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_PATCHED, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
        if (add) RegSetValueExW(k, path, 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
        else RegDeleteValueW(k, path);
        RegCloseKey(k);
    }
    vxp_reimpersonate(imp);
}

/* A file with more than one name (hard link) or a link itself: writing it
 * would change another file too - possibly one this user may not change. */
static BOOL several_names(const WCHAR *path)
{
    BY_HANDLE_FILE_INFORMATION fi;
    BOOL r = FALSE;
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileInformationByHandle(h, &fi))
        r = fi.nNumberOfLinks > 1 || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(h);
    return r;
}

static void backup_name(const WCHAR *path, WCHAR *out);

/* ---- in-memory mode: registering a program for vxload.dll -----------------
 * Image File Execution Options make Windows load vxload.dll into the program
 * before its imports are resolved; HKLM\SOFTWARE\VistaExt\Memory lists the
 * full paths vxload acts for (IFEO only knows file names). Values VistaExt
 * finds there (another tool's GlobalFlag or verifier DLLs) are kept and put
 * back when the program is unregistered. Both registry views are written on
 * 64-bit Windows. */
#define VX_REG_MEMORY VX_REG_ROOT L"\\Memory"
#define VX_IFEO L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\"
#define FLG_APPLICATION_VERIFIER 0x100

static int reg_views(REGSAM *v)
{
    BOOL wow = FALSE;
    if (sizeof(void *) == 8 || (IsWow64Process(GetCurrentProcess(), &wow) && wow)) {
        v[0] = KEY_WOW64_64KEY;
        v[1] = KEY_WOW64_32KEY;
        return 2;
    }
    v[0] = 0;
    return 1;
}

static const WCHAR *file_part(const WCHAR *p)
{
    const WCHAR *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

/* remove (or check for) a word in a space/comma separated list */
static BOOL list_drop(WCHAR *list, const WCHAR *word, BOOL drop)
{
    WCHAR *s = list, *w;
    SIZE_T n = vx_wcslen(word);
    BOOL found = FALSE;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        w = s;
        while (*s && *s != ' ' && *s != ',') s++;
        if ((SIZE_T)(s - w) == n && !vx_wcsnicmp(w, word, n)) {
            found = TRUE;
            if (drop) {
                WCHAR *t = w, *f = s;
                while (*f == ' ' || *f == ',') f++;
                while ((*t++ = *f++)) ;
                s = w;
            }
        }
    }
    if (drop) {                                   /* trailing separators */
        SIZE_T l = vx_wcslen(list);
        while (l && (list[l - 1] == ' ' || list[l - 1] == ',')) list[--l] = 0;
    }
    return found;
}

static BOOL qdword(HKEY k, const WCHAR *name, DWORD *v)
{
    DWORD type, cb = sizeof(*v);
    return !RegQueryValueExW(k, name, NULL, &type, (BYTE *)v, &cb) && type == REG_DWORD;
}

static void sdword(HKEY k, const WCHAR *name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

static void ifeo_set(const WCHAR *exe, REGSAM view, BOOL on)
{
    WCHAR key[MAX_PATH + 100], dl[600];
    HKEY k;
    DWORD v, type, cb;
    vx_wcscpyn(key, VX_IFEO, MAX_PATH + 100);
    vx_wcscatn(key, exe, MAX_PATH + 100);
    if (on) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view, NULL, &k, NULL)) return;
        if (qdword(k, L"VistaExt", &v)) { RegCloseKey(k); return; }      /* already set up */
        if (qdword(k, L"GlobalFlag", &v)) { sdword(k, L"VistaExtPrevGlobalFlag", v); v |= FLG_APPLICATION_VERIFIER; }
        else v = FLG_APPLICATION_VERIFIER;
        sdword(k, L"GlobalFlag", v);
        cb = sizeof(dl) - 2 * sizeof(WCHAR);
        if (!RegQueryValueExW(k, L"VerifierDlls", NULL, &type, (BYTE *)dl, &cb) && type == REG_SZ) {
            dl[cb / sizeof(WCHAR)] = 0;
            if (!list_drop(dl, L"vxload.dll", FALSE)) {
                vx_wcscatn(dl, dl[0] ? L" vxload.dll" : L"vxload.dll", 600);
                RegSetValueExW(k, L"VerifierDlls", 0, REG_SZ, (const BYTE *)dl, (DWORD)((vx_wcslen(dl) + 1) * sizeof(WCHAR)));
            }
        } else
            RegSetValueExW(k, L"VerifierDlls", 0, REG_SZ, (const BYTE *)L"vxload.dll", sizeof(L"vxload.dll"));
        /* any value turns off the verifier's own default checks (heap, handles, locks) */
        if (!qdword(k, L"VerifierFlags", &v)) { sdword(k, L"VerifierFlags", 0x80000000); sdword(k, L"VistaExtSetVerifierFlags", 1); }
        sdword(k, L"VistaExt", 1);
        RegCloseKey(k);
        return;
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view, &k)) return;
    if (!qdword(k, L"VistaExt", &v)) { RegCloseKey(k); return; }          /* not ours */
    if (qdword(k, L"VistaExtPrevGlobalFlag", &v)) sdword(k, L"GlobalFlag", v);
    else RegDeleteValueW(k, L"GlobalFlag");
    cb = sizeof(dl) - 2 * sizeof(WCHAR);
    if (!RegQueryValueExW(k, L"VerifierDlls", NULL, &type, (BYTE *)dl, &cb) && type == REG_SZ) {
        dl[cb / sizeof(WCHAR)] = 0;
        list_drop(dl, L"vxload.dll", TRUE);
        if (dl[0]) RegSetValueExW(k, L"VerifierDlls", 0, REG_SZ, (const BYTE *)dl, (DWORD)((vx_wcslen(dl) + 1) * sizeof(WCHAR)));
        else RegDeleteValueW(k, L"VerifierDlls");
    }
    if (qdword(k, L"VistaExtSetVerifierFlags", &v)) RegDeleteValueW(k, L"VerifierFlags");
    RegDeleteValueW(k, L"VistaExtPrevGlobalFlag");
    RegDeleteValueW(k, L"VistaExtSetVerifierFlags");
    RegDeleteValueW(k, L"VistaExt");
    {
        DWORD nk = 1, nv = 1;
        BOOL empty = !RegQueryInfoKeyW(k, NULL, NULL, NULL, &nk, NULL, NULL, &nv, NULL, NULL, NULL, NULL) && !nk && !nv;
        RegCloseKey(k);
        if (empty) {                                /* the key was VistaExt's alone */
            HKEY parent;
            if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options",
                               0, KEY_ALL_ACCESS | view, &parent)) {
                RegDeleteKeyW(parent, exe);
                RegCloseKey(parent);
            }
        }
    }
}

BOOL vxp_memory_registered(const WCHAR *path)
{
    HKEY k;
    BOOL r;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_MEMORY, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    r = !RegQueryValueExW(k, path, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r;
}

/* another registered program with the same file name (IFEO is shared) */
static BOOL name_still_used(const WCHAR *exe)
{
    HKEY k;
    WCHAR name[MAX_PATH + 8];
    DWORD i, n;
    BOOL used = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_MEMORY, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    for (i = 0; !used; i++) {
        n = MAX_PATH + 8;
        if (RegEnumValueW(k, i, name, &n, NULL, NULL, NULL, NULL)) break;
        if (!vx_wcsicmp(file_part(name), exe)) used = TRUE;
    }
    RegCloseKey(k);
    return used;
}

/* Is VistaExt switched on (HKLM\SOFTWARE\VistaExt "Enabled", default on)? */
BOOL vxp_enabled(void)
{
    HKEY k;
    DWORD v = 1, cb = sizeof(v), type;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        if (RegQueryValueExW(k, L"Enabled", NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = 1;
        RegCloseKey(k);
    }
    return v != 0;
}

/* Does Windows have a program of this name? Image File Execution Options go
 * by file name, so VistaExt never registers such a name: Windows' program
 * would get the verifier too, and would not start at all if vxload.dll were
 * missing. Checked: the Windows folder, System32, SysWOW64, wbem, and the
 * Windows programs in Program Files. */
BOOL vxp_windows_name(const WCHAR *name)
{
    static const WCHAR *sub[] = { L"", L"\\System32", L"\\SysWOW64", L"\\System32\\wbem", L"\\SysWOW64\\wbem",
                                  L"\\System32\\oobe", L"\\System32\\sysprep", L"\\ehome" };
    static const WCHAR *pf[] = { L"Windows Defender", L"Internet Explorer", L"Windows Sidebar", L"Windows Media Player",
                                 L"Windows Mail", L"Windows Photo Viewer", L"Windows Journal", L"Windows Calendar",
                                 L"Windows Collaboration", L"Windows NT\\Accessories", L"Movie Maker", L"DVD Maker" };
    static const WCHAR *env[] = { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" };
    WCHAR p[MAX_PATH + 40], w[MAX_PATH];
    unsigned i, e;
    BOOL wow = FALSE;
    if (!name || !name[0] || vx_wcslen(name) > 100) return TRUE;
    if (!GetWindowsDirectoryW(w, MAX_PATH)) return TRUE;
    IsWow64Process(GetCurrentProcess(), &wow);
    for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
        /* a 32-bit caller on 64-bit Windows: the real System32 is "Sysnative" */
        wsprintfW(p, L"%s%s\\%s", w, (wow && !vx_wcsnicmp(sub[i], L"\\System32", 9)) ? L"\\Sysnative" : sub[i], name);
        if (wow && !vx_wcsnicmp(sub[i], L"\\System32\\", 10)) wsprintfW(p, L"%s\\Sysnative%s\\%s", w, sub[i] + 9, name);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    for (e = 0; e < 3; e++) {
        WCHAR root[MAX_PATH];
        if (!GetEnvironmentVariableW(env[e], root, MAX_PATH)) continue;
        for (i = 0; i < sizeof(pf) / sizeof(pf[0]); i++) {
            wsprintfW(p, L"%s\\%s\\%s", root, pf[i], name);
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return TRUE;
        }
    }
    return FALSE;
}

static void mem_register_as(const WCHAR *path, BOOL on, const WCHAR *data);
static void mem_register(const WCHAR *path, BOOL on) { mem_register_as(path, on, L""); }
static void mem_register_as(const WCHAR *path, BOOL on, const WCHAR *data)
{
    REGSAM v[2];
    int nv = reg_views(v), i;
    HKEY k;
    HANDLE imp;
    if (on && vxp_windows_name(file_part(path))) return;      /* never (see above) */
    imp = vxp_unimpersonate();
    for (i = 0; i < nv; i++) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_MEMORY, 0, NULL, 0, KEY_SET_VALUE | v[i], NULL, &k, NULL)) continue;
        if (on) RegSetValueExW(k, path, 0, REG_SZ, (const BYTE *)data, (DWORD)((vx_wcslen(data) + 1) * sizeof(WCHAR)));
        else RegDeleteValueW(k, path);
        RegCloseKey(k);
    }
    /* while VistaExt is switched off, only the list is kept: the switch
     * writes the Image File Execution Options when it is turned on again */
    if ((on && vxp_enabled()) || (!on && !name_still_used(file_part(path))))
        for (i = 0; i < nv; i++) ifeo_set(file_part(path), v[i], on);
    vxp_reimpersonate(imp);
}

/* the on/off switch: Image File Execution Options for every registered
 * program (the list itself stays). Returns the number of programs. */
int vxp_memory_switch(BOOL on, vxp_log_fn fn, void *ctx)
{
    logger lg = { fn, ctx };
    REGSAM v[2];
    int nv = reg_views(v), i, count = 0;
    HKEY k;
    WCHAR name[MAX_PATH + 8];
    DWORD n, idx;
    HANDLE imp = vxp_unimpersonate();
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_MEMORY, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        for (idx = 0; ; idx++) {
            n = MAX_PATH + 8;
            if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
            if (on && vxp_windows_name(file_part(name))) continue;
            for (i = 0; i < nv; i++) ifeo_set(file_part(name), v[i], on);
            count++;
        }
        RegCloseKey(k);
    }
    vxlog(&lg, on ? L"%d program(s): VistaExt redirects their imports again when they start"
                  : L"%d program(s): Image File Execution Options removed (the list is kept for switching on)", count);
    vxp_reimpersonate(imp);
    return count;
}

BOOL vxp_memory_available(void)
{
    WCHAR p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    vx_wcscatn(p, L"\\vxload.dll", MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

DWORD vxp_default_mode(void)
{
    HKEY k;
    WCHAR m[16] = L"";
    DWORD type, cb = sizeof(m) - sizeof(WCHAR);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        if (RegQueryValueExW(k, L"Mode", NULL, &type, (BYTE *)m, &cb) || type != REG_SZ) m[0] = 0;
        RegCloseKey(k);
    }
    if (!vx_wcsicmp(m, L"file") || !vxp_memory_available()) return 0;
    return VXP_MEMORY;
}

/* does anything in the folder of this program (and below) need VistaExt? */
BOOL vxp_folder_needs(const WCHAR *file)
{
    WCHAR dir[MAX_PATH];
    SIZE_T n;
    vxp_stats probe = { 0 };
    vx_wcscpyn(dir, file, MAX_PATH);
    n = vx_wcslen(dir);
    while (n && dir[n - 1] != '\\') n--;
    if (n <= 3) return FALSE;                      /* never walk a whole drive */
    dir[n - 1] = 0;
    vxp_process_dir(dir, VXP_DRYRUN | VXP_QUIET, NULL, NULL, &probe);
    return probe.patched > 0;
}

/* The mode for one program or folder: the default. (A per-program choice
 * belongs here; Firefox was suspected of needing file mode, but the crash
 * came from VistaExt's process-attribute handling, see kernelvx misc.c.) */
DWORD vxp_mode_for(const WCHAR *path)
{
    return vxp_default_mode();
}

/* undo: every registration under a folder (or of one file) */
static int mem_unregister_under(const WCHAR *prefix, BOOL folder, vxp_log_fn fn, void *ctx)
{
    logger lg = { fn, ctx };
    HKEY k;
    WCHAR name[MAX_PATH + 8];
    DWORD n, i = 0;
    int count = 0;
    SIZE_T pl = vx_wcslen(prefix);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_MEMORY, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return 0;
    for (;;) {
        n = MAX_PATH + 8;
        if (RegEnumValueW(k, i, name, &n, NULL, NULL, NULL, NULL)) break;
        if (!prefix || (folder ? (!vx_wcsnicmp(name, prefix, pl) && (name[pl] == '\\' || (pl && prefix[pl - 1] == '\\')))
                               : !vx_wcsicmp(name, prefix))) {
            mem_register(name, FALSE);
            vxlog(&lg, L"%s: no longer redirected in memory", name);
            count++;
            continue;                               /* the list shrank: same index again */
        }
        i++;
    }
    RegCloseKey(k);
    return count;
}

/* A running program's file cannot be written. (Read-only files would also
 * refuse; they are made writable when restored, so they are not "in use".) */
static BOOL in_use(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path), err;
    HANDLE h;
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_READONLY)) return FALSE;
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }
    err = GetLastError();
    return err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION;
}

int vxp_restore_all(vxp_log_fn fn, void *ctx, int *failed)
{
    logger lg = { fn, ctx };
    HKEY k;
    WCHAR name[MAX_PATH + 8], b[MAX_PATH + 8];
    DWORD n, idx = 0, count = 0;
    int r, busy = 0;
    if (failed) *failed = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_PATCHED, 0, KEY_READ | KEY_SET_VALUE | VX_REG_VIEW, &k)) return 0;
    /* All or nothing: if any patched program is running, restore none, so
     * that VistaExt stays complete and every patched program keeps working
     * until the user has closed them all. */
    for (idx = 0; ; idx++) {
        n = MAX_PATH + 8;
        if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
        backup_name(name, b);
        if (file_exists(b) && in_use(name)) {
            vxlog(&lg, L"%s: in use, close it first", name);
            busy++;
        }
    }
    if (busy) {
        RegCloseKey(k);
        if (failed) *failed = busy;
        return 0;
    }
    idx = 0;
    for (;;) {
        n = MAX_PATH + 8;
        if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
        if ((r = vxp_restore_file(name, fn, ctx)) < 0) {
            /* still patched (in use?): keep it listed for the next try */
            if (failed) (*failed)++;
            idx++;
            continue;
        }
        if (r > 0) count++;
        RegDeleteValueW(k, name);          /* restored or nothing left to restore */
    }
    RegCloseKey(k);
    count += (DWORD)mem_unregister_under(NULL, FALSE, fn, ctx);
    return (int)count;
}

static void backup_name(const WCHAR *path, WCHAR *out)
{
    vx_wcscpyn(out, path, MAX_PATH + 8);
    vx_wcscatn(out, L".vxorig", MAX_PATH + 8);
}

BOOL vxp_has_backup(const WCHAR *path)
{
    WCHAR b[MAX_PATH + 8];
    backup_name(path, b);
    return file_exists(b);
}

/* every import table: normal and delay-loaded */
static void walk_imports(job *j)
{
    pe *p = j->p;
    /* normal imports */
    if (p->ndir > 1 && p->dir[1].VirtualAddress) {
        DWORD o = rva2off(p, p->dir[1].VirtualAddress);
        while (o && o + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= p->size) {
            IMAGE_IMPORT_DESCRIPTOR *id = (void *)(p->d + o);
            if (!id->Name) break;
            /* a bound descriptor without a name table cannot be re-resolved */
            if (id->OriginalFirstThunk || !id->TimeDateStamp)
                do_descriptor(j, id->Name, id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk, FALSE);
            o += sizeof(*id);
        }
    }
    /* delay-loaded imports */
    if (p->ndir > 13 && p->dir[13].VirtualAddress) {
        DWORD o = rva2off(p, p->dir[13].VirtualAddress);
        while (o && o + 32 <= p->size) {
            DWORD *dd = (DWORD *)(p->d + o);   /* grAttrs, rvaDLLName, rvaHmod, rvaIAT, rvaINT, ... */
            DWORD name = dd[1], intab = dd[4];
            if (!name) break;
            if (!(dd[0] & 1)) { name -= (DWORD)p->base; intab -= (DWORD)p->base; }   /* old VA format */
            do_descriptor(j, name, intab, TRUE);
            o += 32;
        }
    }
}

int vxp_process_file(const WCHAR *path, DWORD flags, vxp_log_fn fn, void *ctx, vxp_stats *st)
{
    logger lg = { fn, ctx };
    WCHAR sysdir[MAX_PATH], appdir[MAX_PATH];
    DWORD size, i;
    BYTE *d;
    pe p;
    job j;
    int ret = 0;
    BOOL mem_done = FALSE;

    if (st) st->files++;
    if (!(d = read_file(path, &size))) {
        vxlog(&lg, L"%s: cannot read the file", path);
        if (st) st->failed++;
        return -1;
    }
    if (!pe_open(&p, d, size)) { xfree(d); if (st) st->files--; return 0; }    /* not a program */

    vx_memset(&j, 0, sizeof(j));
    sys_dir(p.is64, sysdir);
    vx_wcscpyn(appdir, path, MAX_PATH);
    for (i = (DWORD)vx_wcslen(appdir); i && appdir[i - 1] != '\\'; i--) ;
    appdir[i ? i - 1 : 0] = 0;
    j.p = &p; j.log = &lg; j.sysdir = sysdir; j.appdir = appdir; j.flags = flags; j.path = path;
    j.survey = TRUE;
    if (!(flags & VXP_QUIET)) header(&j);

    walk_imports(&j);          /* which DLLs need a companion */
    j.survey = FALSE;
    walk_imports(&j);          /* switch them */

    if (flags & VXP_MEMORY) {
        /* in-memory mode: the switch is done by vxload while the program
         * loads; the file keeps its imports. A program (.exe) is registered
         * if it or its folder needs VistaExt. */
        BOOL is_dll = (*(WORD *)(d + p.nt + 4 + 18) & IMAGE_FILE_DLL) != 0;
        BOOL want = j.changed || (flags & (VXP_FORCE | VXP_REGALL));
        if (j.changed) {
            header(&j);
            vxlog(&lg, L"  (in memory while it runs: the file itself keeps these imports)");
            xfree(d);                              /* the unchanged file, for the header check below */
            if (!(d = read_file(path, &size)) || !pe_open(&p, d, size)) {
                if (d) xfree(d);
                if (st) st->failed++;
                return -1;
            }
        }
        j.changed = FALSE;
        if (want && !is_dll && vxp_windows_name(file_part(path))) {
            /* registrations go by file name: one for this name would also
             * reach Windows' own program of that name */
            header(&j);
            vxlog(&lg, L"  not registered: Windows has a program called %s too, and VistaExt never changes how Windows' own programs start",
                  file_part(path));
        } else if (want && !is_dll && !vxp_memory_registered(path)) {
            header(&j);
            vxlog(&lg, L"  registered: VistaExt redirects its imports in memory when it starts");
            if (!(flags & VXP_DRYRUN)) mem_register_as(path, TRUE, (flags & VXP_SETUP) ? L"setup" : L"");
            mem_done = TRUE;
        }
    }

    /* Windows will not start a program whose header asks for a newer
     * Windows ("is not a valid Win32 application"): programs built for
     * Windows 10 say subsystem 10.0. Lower it to this Windows' version. */
    {
        WORD *ss = (WORD *)(d + p.opt + 48);      /* Major/MinorSubsystemVersion, PE32 and PE32+ */
        const DWORD maj = VX_OS / 100, min = VX_OS % 100;     /* this build's Windows: 6.0 or 6.1 */
        if (ss[0] > maj || (ss[0] == maj && ss[1] > min)) {
            header(&j);
            vxlog(&lg, L"  header says it needs Windows %u.%u; set to %u.%u so this Windows starts it",
                  ss[0], ss[1], maj, min);
            ss[0] = (WORD)maj;
            ss[1] = (WORD)min;
            j.changed = TRUE;
        }
    }

    if (st) st->unsupported += j.unsupported;
    if (mem_done && !j.changed) {
        if (st) st->patched++;
        ret = 1;
    }
    if (j.changed) {
        /* bound imports point at the old DLLs' addresses: drop them so the
         * loader resolves everything fresh */
        if (!(flags & VXP_MEMORY) && p.ndir > 11 && p.dir[11].VirtualAddress) {
            DWORD o = rva2off(&p, p.dir[1].VirtualAddress);
            p.dir[11].VirtualAddress = p.dir[11].Size = 0;
            while (o && o + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= size) {
                IMAGE_IMPORT_DESCRIPTOR *id = (void *)(d + o);
                if (!id->Name) break;
                if (id->OriginalFirstThunk) id->TimeDateStamp = 0;
                o += sizeof(*id);
            }
        }
        if (*(DWORD *)(d + p.checksum_off)) *(DWORD *)(d + p.checksum_off) = pe_checksum(d, size, p.checksum_off);
        if (st) st->patched++;
        ret = 1;
        if (!(flags & VXP_DRYRUN)) {
            WCHAR b[MAX_PATH + 8];
            backup_name(path, b);
            BOOL have = !(flags & VXP_NOBACKUP) && file_exists(b);
            if (have && !same_but_patched(d, size, b)) {
                /* the program was updated since an earlier patch: the old
                 * backup would put the old version back on undo */
                vxlog(&lg, L"  the program changed since it was last patched; its backup is renewed");
                have = FALSE;
            }
            if (several_names(path) || several_names(b)) {
                vxlog(&lg, L"  the file or its backup is a link to another file (hard link); left alone for safety");
                ret = -1;
            } else if (!(flags & VXP_NOBACKUP) && !have &&
                       (unprotect(b), DeleteFileW(b), !CopyFileW(path, b, TRUE))) {   /* a new file, never through a link */
                vxlog(&lg, L"  could not save a backup copy (error %d); nothing changed", (int)GetLastError());
                ret = -1;
            } else if (!write_file(path, d, size)) {
                vxlog(&lg, L"  could not write the file (error %d). Is the program running, or do you need administrator rights?",
                     (int)GetLastError());
                ret = -1;
            } else {
                vxlog(&lg, L"  patched (original kept as .vxorig)");
                if (!(flags & VXP_NOBACKUP)) record_patched(path, TRUE);
            }
            if (ret < 0 && st) { st->failed++; st->patched--; }
        }
    } else if (!(flags & VXP_QUIET) && !j.unsupported) {
        vxlog(&lg, L"  nothing to change");
    }
    xfree(d);
    return ret;
}

static BOOL ends_with(const WCHAR *s, const WCHAR *suf)
{
    SIZE_T a = vx_wcslen(s), b = vx_wcslen(suf);
    return a >= b && !vx_wcsicmp(s + a - b, suf);
}

typedef int (*walk_fn)(const WCHAR *path, void *arg);

static int walk(const WCHAR *dir, walk_fn fn, void *arg, int depth)
{
    WCHAR pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;
    if (depth > 12) return 0;
    vx_wcscpyn(pat, dir, MAX_PATH);
    vx_wcscatn(pat, L"\\*", MAX_PATH);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!vx_wcsicmp(fd.cFileName, L".") || !vx_wcsicmp(fd.cFileName, L"..")) continue;
        vx_wcscpyn(full, dir, MAX_PATH);
        vx_wcscatn(full, L"\\", MAX_PATH);
        vx_wcscatn(full, fd.cFileName, MAX_PATH);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) count += walk(full, fn, arg, depth + 1);
        } else count += fn(full, arg);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

typedef struct { DWORD flags; vxp_log_fn fn; void *ctx; vxp_stats *st; } dirjob;

static int patch_one(const WCHAR *path, void *arg)
{
    dirjob *dj = arg;
    DWORD f = dj->flags;
    if (!ends_with(path, L".exe") && !ends_with(path, L".dll")) return 0;
    if (ends_with(path, L".dll")) f &= ~VXP_FORCE;          /* settings only matter for programs */
    return vxp_process_file(path, f | VXP_QUIET, dj->fn, dj->ctx, dj->st) == 1;
}

int vxp_process_dir(const WCHAR *dir, DWORD flags, vxp_log_fn fn, void *ctx, vxp_stats *st)
{
    dirjob dj = { flags, fn, ctx, st };
    if ((flags & VXP_MEMORY) && !(flags & VXP_REGALL)) {
        /* in memory, a program is registered when anything in its folder
         * needs VistaExt (a DLL next to it, typically) */
        vxp_stats probe = { 0 };
        dirjob pj = { (flags & ~(VXP_MEMORY | VXP_FORCE)) | VXP_DRYRUN | VXP_QUIET, NULL, NULL, &probe };
        walk(dir, patch_one, &pj, 0);
        if (probe.patched || (flags & VXP_FORCE)) dj.flags |= VXP_REGALL;
    }
    return walk(dir, patch_one, &dj, 0);
}

int vxp_restore_file(const WCHAR *path, vxp_log_fn fn, void *ctx)
{
    logger lg = { fn, ctx };
    WCHAR b[MAX_PATH + 8];
    DWORD size, attr;
    BYTE *d;
    backup_name(path, b);
    if (mem_unregister_under(path, FALSE, fn, ctx) && !file_exists(b)) return 1;
    if (!file_exists(b)) return 0;
    if (!file_exists(path)) {
        /* the program was removed: do not bring a file of it back */
        DeleteFileW(b);
        record_patched(path, FALSE);
        return 0;
    }
    if ((d = read_file(path, &size))) {
        BOOL ours = same_but_patched(d, size, b);
        xfree(d);
        if (!ours) {
            /* updated or reinstalled since it was patched: the backup is an
             * older version, so putting it back would downgrade the program */
            vxlog(&lg, L"%s: changed since VistaExt patched it (updated?), left as it is; old backup removed", path);
            unprotect(b);
            DeleteFileW(b);
            record_patched(path, FALSE);
            return 0;
        }
    }
    attr = unprotect(path);
    if (!MoveFileExW(b, path, MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        if (attr) SetFileAttributesW(path, attr);
        vxlog(&lg, L"%s: could not restore (error %d). Is the program running?", path, (int)err);
        return -1;
    }
    vxlog(&lg, L"%s: original restored", path);
    record_patched(path, FALSE);
    return 1;
}

typedef struct { vxp_log_fn fn; void *ctx; int failed; } restjob;

static int restore_one(const WCHAR *path, void *arg)
{
    restjob *r = arg;
    WCHAR orig[MAX_PATH];
    SIZE_T n;
    if (!ends_with(path, L".vxorig")) return 0;
    vx_wcscpyn(orig, path, MAX_PATH);
    n = vx_wcslen(orig);
    if (n < 8) return 0;
    orig[n - 7] = 0;
    switch (vxp_restore_file(orig, r->fn, r->ctx)) {
    case 1: return 1;
    case -1: r->failed++; break;
    }
    return 0;
}

int vxp_restore_dir(const WCHAR *dir, vxp_log_fn fn, void *ctx, int *failed)
{
    restjob r = { fn, ctx, 0 };
    int n = walk(dir, restore_one, &r, 0);
    n += mem_unregister_under(dir, TRUE, fn, ctx);
    if (failed) *failed = r.failed;
    return n;
}

/* ---- recommended settings --------------------------------------------- */
static BOOL exists_in(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    vx_wcscpyn(p, dir, MAX_PATH);
    vx_wcscatn(p, L"\\", MAX_PATH);
    vx_wcscatn(p, name, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Chromium/Electron: V8's and ICU's data files, which every Chromium build
 * ships whatever the browser renames (Edge: msedge_elf.dll), or
 * chrome_elf.dll, Chromium's resource packs or an Electron app.asar - in the
 * folder or one level below (versioned folders) */
static BOOL chromium_in(const WCHAR *dir, int depth)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL found = FALSE;
    if ((exists_in(dir, L"icudtl.dat") &&
         (exists_in(dir, L"v8_context_snapshot.bin") || exists_in(dir, L"snapshot_blob.bin"))) ||
        exists_in(dir, L"chrome_elf.dll") || exists_in(dir, L"resources\\app.asar") ||
        (exists_in(dir, L"chrome_100_percent.pak") && exists_in(dir, L"libEGL.dll")))
        return TRUE;
    if (depth >= 1) return FALSE;
    vx_wcscpyn(pat, dir, MAX_PATH);
    vx_wcscatn(pat, L"\\*", MAX_PATH);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        vx_wcscpyn(sub, dir, MAX_PATH);
        vx_wcscatn(sub, L"\\", MAX_PATH);
        vx_wcscatn(sub, fd.cFileName, MAX_PATH);
        found = chromium_in(sub, depth + 1);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static BOOL needs_win10(const WCHAR *file)
{
    DWORD size;
    BYTE *d = read_file(file, &size);
    pe p;
    BOOL r = FALSE;
    if (!d) return FALSE;
    if (pe_open(&p, d, size)) r = *(WORD *)(d + p.opt + 48) >= 10;
    xfree(d);
    return r;
}

BOOL vxp_recommend(const WCHAR *path, WCHAR *layer, SIZE_T cch)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH];
    DWORD a = GetFileAttributesW(path);
    SIZE_T n;
    BOOL win10 = FALSE;
    layer[0] = 0;
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    vx_wcscpyn(dir, path, MAX_PATH);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        n = vx_wcslen(dir);
        while (n && dir[n - 1] != '\\') n--;
        if (n) dir[n - 1] = 0;
        win10 = needs_win10(path);
    } else {
        WIN32_FIND_DATAW fd;
        HANDLE h;
        vx_wcscpyn(pat, dir, MAX_PATH);
        vx_wcscatn(pat, L"\\*.exe", MAX_PATH);
        if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                WCHAR f[MAX_PATH];
                vx_wcscpyn(f, dir, MAX_PATH);
                vx_wcscatn(f, L"\\", MAX_PATH);
                vx_wcscatn(f, fd.cFileName, MAX_PATH);
                win10 = needs_win10(f);
            } while (!win10 && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    /* the Windows version itself comes from the global setting (tray icon),
     * which is Windows 10 unless the user chose another; only a program
     * whose header asks for Windows 10 and finds no global setting gets it */
    if (chromium_in(dir, 0)) {
        WCHAR g[64];
        vx_make_layer(layer, cch, vx_global_version(g, 64) ? NULL : vx_find_osver(L"WIN10"), FALSE, VXP_CHROMIUM_ARGS);
        return TRUE;
    }
    if (win10) {
        WCHAR g[64];
        if (!vx_global_version(g, 64)) vx_make_layer(layer, cch, vx_find_osver(L"WIN10"), FALSE, NULL);
    }
    return FALSE;
}

/* the patch flags a setting needs: VistaExt must be loaded to tell a version
 * (its own, or the global one) or add arguments, and version.dll imports go
 * to versvx when a version is told */
DWORD vxp_flags_for_layer(const WCHAR *layer)
{
    vx_layer l, g;
    WCHAR gv[64];
    DWORD f = 0;
    vx_parse_layer(layer, &l);
    if (layer && layer[0]) f |= VXP_FORCE;
    if (l.ver) f |= VXP_LAYER;
    if (!l.ver && !l.real && vx_global_version(gv, 64)) {
        vx_parse_layer(gv, &g);
        if (g.ver) f |= VXP_FORCE | VXP_LAYER;
    }
    return f;
}

/* ---- Explorer entries and .msi double-click --------------------------------
 * "VistaExt compatibility..." on programs and folders, "Run with VistaExt" on
 * programs, "Install with VistaExt fixes" on .msi packages, and .msi
 * double-click through msifix (a package that needs no change is installed as
 * it is). The .msi command found before is kept (MsiOpenCommand) and put back
 * when they are removed. Used by VistaExt Center (/register, /unregister) and
 * by the on/off switch (vxauto). */
#define MSI_OPEN L"SOFTWARE\\Classes\\Msi.Package\\shell\\Open\\command"

static BOOL contains_i(const WCHAR *h, const WCHAR *n)
{
    SIZE_T ln = vx_wcslen(n);
    for (; *h; h++) if (!vx_wcsnicmp(h, n, ln)) return TRUE;
    return FALSE;
}

static void set_sz(const WCHAR *sub, const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((vx_wcslen(val) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

static BOOL get_sz(const WCHAR *sub, const WCHAR *name, WCHAR *out, DWORD cch)
{
    HKEY k;
    DWORD cb = (cch - 1) * sizeof(WCHAR), type;
    LONG r;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb);
    RegCloseKey(k);
    if (r || (type != REG_SZ && type != REG_EXPAND_SZ)) { out[0] = 0; return FALSE; }
    out[cb / sizeof(WCHAR)] = 0;
    return TRUE;
}

void vxp_shell_entries(BOOL on, const WCHAR *dir)
{
    static const WCHAR *keys[] = {
        L"SOFTWARE\\Classes\\exefile\\shell\\VistaExt",
        L"SOFTWARE\\Classes\\Directory\\shell\\VistaExt",
        L"SOFTWARE\\Classes\\Msi.Package\\shell\\VistaExtInstall",
        L"SOFTWARE\\Classes\\exefile\\shell\\VistaExtRun",
    };
    WCHAR cmd[MAX_PATH * 2], sub[200], prev[MAX_PATH * 2];
    int i;
    if (!on) {
        for (i = 0; i < 4; i++) {
            wsprintfW(sub, L"%s\\command", keys[i]);
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub);
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, keys[i]);
        }
        /* give .msi double-click its own command back */
        if (get_sz(VX_REG_ROOT, L"MsiOpenCommand", prev, MAX_PATH * 2) && prev[0] &&
            get_sz(MSI_OPEN, NULL, cmd, MAX_PATH * 2) && contains_i(cmd, L"msifix.exe"))
            set_sz(MSI_OPEN, NULL, prev);
        return;
    }
    wsprintfW(sub, L"%s\\command", keys[0]);
    set_sz(keys[0], NULL, L"VistaExt compatibility...");
    wsprintfW(cmd, L"\"%svxcenter.exe\" \"%%1\"", dir);
    set_sz(sub, NULL, cmd);

    wsprintfW(sub, L"%s\\command", keys[1]);
    set_sz(keys[1], NULL, L"VistaExt: make programs in this folder compatible...");
    set_sz(sub, NULL, cmd);

    wsprintfW(sub, L"%s\\command", keys[2]);
    set_sz(keys[2], NULL, L"Install with VistaExt fixes");
    wsprintfW(cmd, L"\"%smsifix.exe\" /install \"%%1\"", dir);
    set_sz(sub, NULL, cmd);

    if (get_sz(MSI_OPEN, NULL, prev, MAX_PATH * 2) && !contains_i(prev, L"msifix.exe"))
        set_sz(VX_REG_ROOT, L"MsiOpenCommand", prev);
    set_sz(MSI_OPEN, NULL, cmd);

    wsprintfW(sub, L"%s\\command", keys[3]);
    set_sz(keys[3], NULL, L"Run with VistaExt");
    wsprintfW(cmd, L"\"%svxcenter.exe\" /run \"%%1\"", dir);
    set_sz(sub, NULL, cmd);
}

/* ---- programs that were removed ---------------------------------------------
 * When a program is uninstalled, what VistaExt kept for it goes too: its
 * registration (and Image File Execution Options), a backup next to where it
 * was, the entry in the patched list and its own setting. Nothing happens
 * while the file exists. */
int vxp_forget(const WCHAR *path, vxp_log_fn fn, void *ctx)
{
    logger lg = { fn, ctx };
    WCHAR b[MAX_PATH + 8];
    int n = 0;
    HANDLE imp;
    if (file_exists(path)) return 0;
    n += mem_unregister_under(path, FALSE, fn, ctx);
    backup_name(path, b);
    if (file_exists(b)) { unprotect(b); if (DeleteFileW(b)) n++; }
    imp = vxp_unimpersonate();
    record_patched(path, FALSE);
    {
        HKEY k;
        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_LAYERS, 0, KEY_SET_VALUE | VX_REG_VIEW, &k)) {
            if (!RegDeleteValueW(k, path)) n++;
            RegCloseKey(k);
        }
    }
    vxp_reimpersonate(imp);
    if (n) vxlog(&lg, L"%s: removed from this PC; what VistaExt kept for it is removed too", path);
    return n;
}

static BOOL gone(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p), e = GetLastError();
    return a == INVALID_FILE_ATTRIBUTES && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);
}

/* every list: entries whose file or folder no longer exists (only local
 * paths: a program on a removable or network drive may just be away) */
int vxp_cleanup_missing(vxp_log_fn fn, void *ctx)
{
    static const WCHAR *keys[] = { VX_REG_MEMORY, VX_REG_PATCHED, VX_REG_LAYERS };
    logger lg = { fn, ctx };
    WCHAR name[MAX_PATH + 8], *list;
    DWORD n, idx, used;
    unsigned i;
    int count = 0;
    const DWORD cap = 64 * 1024;
    if (!(list = HeapAlloc(GetProcessHeap(), 0, cap * sizeof(WCHAR)))) return 0;
    for (i = 0; i < 3; i++) {
        HKEY k;
        WCHAR *p;
        used = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) continue;
        for (idx = 0; ; idx++) {
            SIZE_T l;
            WCHAR root[4];
            n = MAX_PATH + 8;
            if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
            l = vx_wcslen(name);
            if (l < 4 || name[1] != ':' || name[2] != '\\') continue;
            root[0] = name[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            if (name[l - 1] == '\\') {
                WCHAR d[MAX_PATH + 8];
                vx_wcscpyn(d, name, MAX_PATH + 8);
                d[l - 1] = 0;
                if (!gone(d)) continue;
            } else if (!gone(name)) continue;
            if (used + l + 2 >= cap) break;
            vx_memcpy(list + used, name, (l + 1) * sizeof(WCHAR));
            used += (DWORD)l + 1;
        }
        RegCloseKey(k);
        list[used] = 0;
        for (p = list; *p; p += vx_wcslen(p) + 1) {
            SIZE_T l = vx_wcslen(p);
            if (p[l - 1] == '\\') {
                if (!vx_set_layer(p, NULL)) { vxlog(&lg, L"%s: folder removed; its setting is removed too", p); count++; }
            } else
                count += vxp_forget(p, fn, ctx) ? 1 : 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, list);
    return count;
}

/* the files the on/off switch puts back when VistaExt is turned off */
int vxp_patched_list(WCHAR *buf, DWORD cch)
{
    HKEY k;
    WCHAR name[MAX_PATH + 8];
    DWORD n, idx, used = 0;
    int count = 0;
    if (cch) buf[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_PATCHED, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return 0;
    for (idx = 0; ; idx++) {
        DWORD l;
        n = MAX_PATH + 8;
        if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
        l = (DWORD)vx_wcslen(name);
        if (used + l + 2 >= cch) break;
        vx_memcpy(buf + used, name, (l + 1) * sizeof(WCHAR));
        used += l + 1;
        buf[used] = 0;
        count++;
    }
    RegCloseKey(k);
    return count;
}

/* the switch: a file it put back (or will put back at the next start) */
void vxp_unlist_patched(const WCHAR *path) { record_patched(path, FALSE); }

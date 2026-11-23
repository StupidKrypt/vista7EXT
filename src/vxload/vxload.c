/* vxload.dll - VistaExt's in-memory mode.
 *
 * Instead of changing a program's files, VistaExt can redirect its imports
 * while it loads. Windows loads "verifier provider" DLLs named in the
 * program's Image File Execution Options very early: after the program is
 * mapped, before any of its imports are resolved (the mechanism VxKex uses on
 * Windows 7). vxpatch /memory registers this DLL for a program:
 *
 *   HKLM\...\Image File Execution Options\<name>.exe
 *       GlobalFlag    |= 0x100         (FLG_APPLICATION_VERIFIER)
 *       VerifierDlls  += vxload.dll
 *       VerifierFlags  = 0x80000000    (no verifier checks of its own)
 *   HKLM\SOFTWARE\VistaExt\Memory  "<full path of the .exe>"
 *
 * Image File Execution Options go by file name only, so vxload first checks
 * that this exact program (full path) was registered; any other program of
 * the same name is left alone.
 *
 * For the program and every DLL it loads from outside the Windows folder, the
 * import names are rewritten in memory, the same way vxpatch rewrites them
 * on disk: kernel32.dll -> kernelvx.dll, user32.dll -> uservx.dll, ... and an
 * API-set name api-ms-win-X -> vxs-ms-win-X (VistaExt's own copy of that API
 * set, which Windows' API-set table does not intercept), when that copy
 * exports every function imported from it. A DLL Windows does not have at
 * all (shcore.dll, d3d12.dll, combase.dll on Vista and 7, Vista's
 * bcryptprimitives.dll) is VistaExt's under a name of its own
 * (companions.h, VX_NEWDLLS), so that programs VistaExt does not run never
 * find it: here the import is pointed at it, unless Windows has that DLL
 * after all (Windows 8.1's shcore) or the program ships its own. The new
 * names are never longer, so they are written in place. Windows' own DLLs
 * keep their imports.
 *
 * When the program is told another Windows version, two places in Windows'
 * DLLs are changed, in this process only, so that what it reads elsewhere
 * agrees with the version it is told (see "version_hooks" below):
 *   - ntdll's NtQueryValueKey: a 5-byte jump at the start of its stub (the
 *     original stub runs from a copy), for the version values under
 *     HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion. The stub's bytes are
 *     checked first; unknown code is left alone.
 *   - ole32's export entries for CoCreateInstance(Ex) and CoSetProxyBlanket
 *     (WMI's Win32_OperatingSystem), when ole32 is loaded.
 *
 * Windows 8's loader resolves a DLL's imports before it reports the DLL as
 * loaded (Vista and 7 report it first), so on Windows 8 the names are also
 * rewritten right after the loader maps the DLL: ntdll's NtMapViewOfSection
 * gets the same kind of jump as NtQueryValueKey (see "map_hook" below).
 *
 * This DLL is loaded before kernel32: it may only use ntdll. */
#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include "../common/companions.h"
#include "../common/ntinternals.h"
#include "../common/releases.h"

#define DLL_PROCESS_VERIFIER 4
#ifndef NtCurrentProcess
#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif

typedef struct { PWCHAR name; ULONG flags; PVOID addr; PVOID thunks; } vrf_dll;
typedef struct {
    ULONG Length;
    vrf_dll *ProviderDlls;
    PVOID ProviderDllLoadCallback;
    PVOID ProviderDllUnloadCallback;
    PWSTR VerifierImage;
    ULONG VerifierFlags;
    ULONG VerifierDebug;
    PVOID RtlpGetStackTraceAddress;
    PVOID RtlpDebugPageHeapCreate;
    PVOID RtlpDebugPageHeapDestroy;
    PVOID ProviderNtdllHeapFreeCallback;
} vrf_provider;

static vrf_dll g_none[1];
static void NTAPI verifier_load(PWSTR name, PVOID base, SIZE_T size, PVOID reserved);
static vrf_provider g_prov = { sizeof(vrf_provider), g_none, (PVOID)verifier_load };
static BOOL g_on, g_trace;
static void *g_cookie;

/* ---- small helpers (no C runtime, no kernel32) ------------------------------ */
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static WCHAR lcw(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }

static BOOL ieq(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return !*a && !*b;
}

static SIZE_T slen(const char *s) { SIZE_T n = 0; while (s[n]) n++; return n; }

/* debug output the way OutputDebugString sends it (DbgPrint is filtered out
 * by default); only with a debugger attached, which handles the exception */
static void trace(const char *what, const char *a, const char *b)
{
    char m[300];
    SIZE_T n = 0;
    const char *parts[6] = { "vx: vxload ", what, a ? " " : "", a ? a : "", b ? " -> " : "", b ? b : "" };
    EXCEPTION_RECORD er;
    int i;
    if (!g_trace || !NtCurrentTeb()->Peb->BeingDebugged) return;
    for (i = 0; i < 6; i++) { const char *p = parts[i]; while (*p && n < sizeof(m) - 2) m[n++] = *p++; }
    m[n++] = '\n';
    m[n] = 0;
    for (i = 0; i < (int)sizeof(er); i++) ((BYTE *)&er)[i] = 0;
    er.ExceptionCode = 0x40010006;               /* DBG_PRINTEXCEPTION_C */
    er.NumberParameters = 2;
    er.ExceptionInformation[0] = n + 1;
    er.ExceptionInformation[1] = (ULONG_PTR)m;
    RtlRaiseException(&er);
}

static void trace_hex(const char *what, ULONG v)
{
    char b[12];
    int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> ((7 - i) * 4)) & 15];
    b[10] = 0;
    trace(what, b, NULL);
}

static void trace_w(const char *what, const WCHAR *w, SIZE_T n)
{
    char b[260];
    SIZE_T i;
    for (i = 0; i < n && i < 259; i++) b[i] = w[i] < 128 ? (char)w[i] : '?';
    b[i] = 0;
    trace(what, b, NULL);
}

static BOOL env_set(const WCHAR *name)
{
    const WCHAR *e = (const WCHAR *)NtCurrentTeb()->Peb->ProcessParameters->Environment;
    SIZE_T n = 0;
    while (name[n]) n++;
    while (e && *e) {
        SIZE_T i = 0;
        while (i < n && lcw(e[i]) == lcw(name[i])) i++;
        if (i == n && e[i] == '=') return TRUE;
        while (*e) e++;
        e++;
    }
    return FALSE;
}

#if VX_OS == 601
/* ---- Windows 7: VistaExt's API sets in this process's API-set map ----------
 * Windows resolves "api-ms-win-*" names through the API-set map its loader
 * reads from the PEB (PEB->ApiSetMap, Windows 7 and later; Vista has none
 * and its DLLs use no API sets). In a program told a newer Windows, Windows'
 * own DLLs take the newer Windows' paths and load API sets Windows 7 lacks
 * (its shell, told Windows 10, loads api-ms-win-core-url-l1-1-0 and refuses
 * to start programs without it). Up to VistaExt 1.2 its api-ms-win-* files
 * in System32 answered that - for every program, also those VistaExt does
 * not run. Here, for this process only: a copy of Windows' map with an
 * entry for each API set Windows lacks and VistaExt has, pointing at its
 * vxs-ms-* copy in System32. Windows' own entries stay as they are (never
 * overridden), no file of Windows' is shadowed, and programs this one starts
 * get Windows' map: the map is per process, not inherited. (The same is done
 * by VxKex for Windows 7; written here from the format, see tools/apiset.py.)
 * Version 2 layout: ULONG Version, Count; Count entries {NameOffset,
 * NameLength, DataOffset}, names without "api-" and ".dll", sorted ignoring
 * case (the loader searches them by halves); data {Count; {NameOffset,
 * NameLength, ValueOffset, ValueLength}...}; offsets from the map's start. */
#ifdef _WIN64
#define VX_PEB_APISETMAP 0x68
#else
#define VX_PEB_APISETMAP 0x38
#endif
typedef struct { ULONG NameOffset, NameLength, DataOffset; } apiset_entry2;
typedef struct { ULONG NameOffset, NameLength, ValueOffset, ValueLength; } apiset_value2;

static WCHAR upw(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }

static int name_cmp(const WCHAR *a, ULONG al, const WCHAR *b, ULONG bl)    /* lengths in characters */
{
    ULONG i;
    for (i = 0; i < al && i < bl; i++) {
        WCHAR x = upw(a[i]), y = upw(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return al == bl ? 0 : al < bl ? -1 : 1;
}

#define MAX_NEW_SETS 400
static void extend_apiset_map(void)
{
    static BYTE listing[65536];
    static struct { WCHAR name[80]; ULONG len; } add[MAX_NEW_SETS];
    static ULONG order[64 + MAX_NEW_SETS * 2];
    BYTE *peb = (BYTE *)NtCurrentTeb()->Peb, *old = *(BYTE **)(peb + VX_PEB_APISETMAP), *map, *p;
    const WCHAR *root = VX_KUSER_SYSTEM_ROOT;
    static WCHAR dir[300];
    UNICODE_STRING us, mask;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE h;
    NTSTATUS st;
    ULONG oc, nadd = 0, i, j, total, oldsize = 0, k;
    SIZE_T n = 0, size;
    BOOL first = TRUE;
    apiset_entry2 *oe, *ne;
    if (!old || ((ULONG *)old)[0] != 2 || (oc = ((ULONG *)old)[1]) == 0 || oc > 64) return;   /* not Windows 7's map */
    oe = (apiset_entry2 *)(old + 8);
    /* the old map's size: up to its furthest string or value */
    for (i = 0; i < oc; i++) {
        apiset_value2 *v = (apiset_value2 *)(old + oe[i].DataOffset + 4);
        ULONG vc = *(ULONG *)(old + oe[i].DataOffset);
        if (oe[i].NameOffset + oe[i].NameLength > oldsize) oldsize = oe[i].NameOffset + oe[i].NameLength;
        if (oe[i].DataOffset + 4 + vc * sizeof(apiset_value2) > oldsize) oldsize = oe[i].DataOffset + 4 + vc * sizeof(apiset_value2);
        for (j = 0; j < vc && vc < 16; j++) {
            if (v[j].NameOffset + v[j].NameLength > oldsize) oldsize = v[j].NameOffset + v[j].NameLength;
            if (v[j].ValueOffset + v[j].ValueLength > oldsize) oldsize = v[j].ValueOffset + v[j].ValueLength;
        }
    }
    /* VistaExt's copies: System32\vxs-ms-*.dll (SysWOW64 in a 32-bit process:
     * the file system redirection applies here too) */
    for (; root[n] && n < 250; n++) dir[n] = root[n];
    for (i = 0; "\\System32"[i]; i++) dir[n++] = "\\System32"[i];
    {
        static WCHAR full[320];
        SIZE_T f = 0;
        for (i = 0; "\\??\\"[i]; i++) full[f++] = "\\??\\"[i];
        for (i = 0; i < n; i++) full[f++] = dir[i];
        us.Buffer = full; us.Length = (USHORT)(f * sizeof(WCHAR)); us.MaximumLength = us.Length;
    }
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE) < 0) return;
    mask.Buffer = (PWSTR)L"vxs-ms-*.dll";
    mask.Length = 12 * sizeof(WCHAR);
    mask.MaximumLength = mask.Length;
    for (;;) {
        FILE_NAMES_INFORMATION *fi;
        st = NtQueryDirectoryFile(h, NULL, NULL, NULL, &io, listing, sizeof(listing), FileNamesInformation, FALSE,
                                  first ? &mask : NULL, first);
        first = FALSE;
        if (st < 0) break;
        for (fi = (FILE_NAMES_INFORMATION *)listing; ; fi = (FILE_NAMES_INFORMATION *)((BYTE *)fi + fi->NextEntryOffset)) {
            ULONG len = fi->FileNameLength / sizeof(WCHAR);
            /* "vxs-ms-win-core-url-l1-1-0.dll": the set is "ms-win-core-url-l1-1-0" */
            if (len > 8 && len <= 80 && nadd < MAX_NEW_SETS) {
                BOOL have = FALSE;
                const WCHAR *nm = fi->FileName + 4;
                ULONG nl = len - 8;
                for (i = 0; i < oc && !have; i++)
                    have = !name_cmp(nm, nl, (const WCHAR *)(old + oe[i].NameOffset), oe[i].NameLength / sizeof(WCHAR));
                if (!have) {                              /* Windows' own entry stays */
                    for (i = 0; i < len && i < 80; i++) add[nadd].name[i] = fi->FileName[i];
                    add[nadd].len = len;                  /* the whole file name; the set name is [4, len - 4) */
                    nadd++;
                }
            }
            if (!fi->NextEntryOffset) break;
        }
    }
    NtClose(h);
    if (!nadd) return;
    /* the new map: header and entries, then the old map's bytes (its strings
     * and values, their offsets moved along), then the new names, values
     * and host names */
    total = oc + nadd;
    {
        ULONG head = 8 + total * sizeof(apiset_entry2), base = (head + 7) & ~7u, tail = base + ((oldsize + 7) & ~7u);
        SIZE_T need = tail + nadd * (80 * 2 + 4 + sizeof(apiset_value2) + 80 * 2 + 16);
        BYTE *q;
        void *mem = NULL;
        size = need;
        if (NtAllocateVirtualMemory(NtCurrentProcess(), &mem, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) < 0) return;
        map = mem;
        ((ULONG *)map)[0] = 2;
        ((ULONG *)map)[1] = total;
        for (i = 0; i < oldsize; i++) map[base + i] = old[i];
        /* entries, sorted: merge the old (sorted) and the new names; each
         * as {index < oc: old entry, else new} in order[] */
        for (i = 0; i < total; i++) order[i] = i;
        for (i = 1; i < total; i++) {                       /* insertion sort, a few hundred names */
            ULONG x = order[i];
            const WCHAR *xn = x < oc ? (const WCHAR *)(old + oe[x].NameOffset) : add[x - oc].name + 4;
            ULONG xl = x < oc ? oe[x].NameLength / sizeof(WCHAR) : add[x - oc].len - 8;
            for (j = i; j > 0; j--) {
                ULONG y = order[j - 1];
                const WCHAR *yn = y < oc ? (const WCHAR *)(old + oe[y].NameOffset) : add[y - oc].name + 4;
                ULONG yl = y < oc ? oe[y].NameLength / sizeof(WCHAR) : add[y - oc].len - 8;
                if (name_cmp(yn, yl, xn, xl) <= 0) break;
                order[j] = y;
            }
            order[j] = x;
        }
        ne = (apiset_entry2 *)(map + 8);
        q = map + tail;
        /* the old values: their offsets move by "base" (each value array once) */
        for (i = 0; i < oc; i++) {
            ULONG vc = *(ULONG *)(map + base + oe[i].DataOffset);
            apiset_value2 *v = (apiset_value2 *)(map + base + oe[i].DataOffset + 4);
            BOOL done = FALSE;
            for (k = 0; k < i && !done; k++) done = oe[k].DataOffset == oe[i].DataOffset;
            if (done || vc >= 16) continue;
            for (j = 0; j < vc; j++) { v[j].NameOffset += base; v[j].ValueOffset += base; }
        }
        for (i = 0; i < total; i++) {
            ULONG x = order[i];
            if (x < oc) {
                ne[i].NameOffset = oe[x].NameOffset + base;
                ne[i].NameLength = oe[x].NameLength;
                ne[i].DataOffset = oe[x].DataOffset + base;
            } else {
                const WCHAR *fn = add[x - oc].name;
                ULONG fl = add[x - oc].len, nl = fl - 8, o;
                apiset_value2 *v;
                /* the set's name */
                ne[i].NameOffset = (ULONG)(q - map);
                ne[i].NameLength = nl * sizeof(WCHAR);
                for (o = 0; o < nl; o++) ((WCHAR *)q)[o] = fn[4 + o];
                q += (nl * sizeof(WCHAR) + 7) & ~7u;
                /* its value: the host is VistaExt's copy, by its file name */
                ne[i].DataOffset = (ULONG)(q - map);
                *(ULONG *)q = 1;
                v = (apiset_value2 *)(q + 4);
                q += (4 + sizeof(apiset_value2) + 7) & ~7u;
                v->NameOffset = 0;
                v->NameLength = 0;
                v->ValueOffset = (ULONG)(q - map);
                v->ValueLength = fl * sizeof(WCHAR);
                for (o = 0; o < fl; o++) ((WCHAR *)q)[o] = fn[o];
                q += (fl * sizeof(WCHAR) + 7) & ~7u;
            }
        }
        {   /* read-only, like Windows' own map */
            ULONG oldprot;
            void *pp = mem;
            SIZE_T ps = size;
            NtProtectVirtualMemory(NtCurrentProcess(), &pp, &ps, PAGE_READONLY, &oldprot);
        }
        *(BYTE **)(peb + VX_PEB_APISETMAP) = map;
        trace_hex("API sets added to this process's map:", nadd);
    }
}
#endif

/* write into a (read-only, copy-on-write) page of a loaded image; its own
 * protection is put back afterwards */
static void poke(void *dst, const void *src, SIZE_T n)
{
    void *p = dst;
    SIZE_T sz = n;
    ULONG old;
    SIZE_T i;
    /* writable AND executable while it is written: the page may hold code
     * that runs meanwhile - ntdll's stub for NtProtectVirtualMemory itself
     * sits next to NtQueryValueKey's */
    if (NtProtectVirtualMemory(NtCurrentProcess(), &p, &sz, PAGE_EXECUTE_READWRITE, &old) < 0) return;
    for (i = 0; i < n; i++) ((BYTE *)dst)[i] = ((const BYTE *)src)[i];
    p = dst; sz = n;
    NtProtectVirtualMemory(NtCurrentProcess(), &p, &sz, old, &old);
}

/* ---- is this program registered? ---------------------------------------- */
static BOOL listed(HANDLE k, UNICODE_STRING *vn)
{
    BYTE buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 16];
    ULONG got;
    NTSTATUS s = NtQueryValueKey(k, vn, KeyValuePartialInformation, buf, sizeof(buf), &got);
    return s >= 0 || s == STATUS_BUFFER_OVERFLOW;
}

/* A program is registered under its long path; Windows may start it under
 * a short one (C:\Users\ADMINI~1\AppData\Local\Temp\... is what Temp hands
 * out). Each part with a "~" is looked up in its folder, which gives its
 * long name (what GetLongPathName does; kernel32 is not loaded yet). */
static BOOL long_path(const UNICODE_STRING *img, WCHAR *out, USHORT cch, UNICODE_STRING *res)
{
    static WCHAR dir[MAX_PATH + 8];
    static BYTE info[sizeof(FILE_BOTH_DIRECTORY_INFORMATION) + 520];
    SIZE_T n = img->Length / sizeof(WCHAR), i = 0, o = 0, d;
    BOOL changed = FALSE;
    if (n < 4 || n > MAX_PATH || img->Buffer[1] != ':' || img->Buffer[2] != '\\') return FALSE;
    out[o++] = img->Buffer[0]; out[o++] = ':';
    i = 2;
    while (i < n) {
        SIZE_T s = ++i, e, k;                        /* after the backslash */
        BOOL tilde = FALSE;
        while (i < n && img->Buffer[i] != '\\') { if (img->Buffer[i] == '~') tilde = TRUE; i++; }
        e = i;
        if (o + 1 >= cch) return FALSE;
        out[o++] = '\\';
        if (tilde) {
            UNICODE_STRING us, mask;
            OBJECT_ATTRIBUTES oa;
            IO_STATUS_BLOCK io;
            HANDLE h;
            FILE_BOTH_DIRECTORY_INFORMATION *fi = (void *)info;
            NTSTATUS st;
            /* the folder so far: "\??\" + out */
            d = 0;
            for (k = 0; "\\??\\"[k]; k++) dir[d++] = "\\??\\"[k];
            for (k = 0; k < o; k++) dir[d++] = out[k];
            us.Buffer = dir; us.Length = (USHORT)(d * sizeof(WCHAR)); us.MaximumLength = us.Length;
            InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
            st = NtOpenFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE);
            if (st < 0) { trace_hex("long path: folder open failed", (ULONG)st); return FALSE; }
            mask.Buffer = img->Buffer + s; mask.Length = (USHORT)((e - s) * sizeof(WCHAR)); mask.MaximumLength = mask.Length;
            st = NtQueryDirectoryFile(h, NULL, NULL, NULL, &io, info, sizeof(info) - 2, FileBothDirectoryInformation, TRUE,
                                      &mask, TRUE);
            NtClose(h);
            if (st < 0) { trace_hex("long path: lookup failed", (ULONG)st); return FALSE; }
            for (k = 0; k < fi->FileNameLength / sizeof(WCHAR); k++) {
                if (o + 1 >= cch) return FALSE;
                out[o++] = fi->FileName[k];
            }
            changed = TRUE;
        } else {
            for (k = s; k < e; k++) { if (o + 1 >= cch) return FALSE; out[o++] = img->Buffer[k]; }
        }
    }
    if (!changed) return FALSE;
    res->Buffer = out; res->Length = (USHORT)(o * sizeof(WCHAR)); res->MaximumLength = res->Length;
    trace_w("long path", out, o);
    return TRUE;
}

static BOOL registered(void)
{
    static const WCHAR keyname[] = L"\\Registry\\Machine\\SOFTWARE\\VistaExt\\Memory";
    static WCHAR lp[MAX_PATH + 4];
    UNICODE_STRING kn, vn, lv;
    OBJECT_ATTRIBUTES oa;
    HANDLE k;
    BOOL r;
    vn = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    if (vn.Length > 8 && vn.Buffer[0] == '\\' && vn.Buffer[1] == '?' && vn.Buffer[2] == '?' && vn.Buffer[3] == '\\') {
        vn.Buffer += 4;                              /* "\??\C:\..." */
        vn.Length -= 8;
        vn.MaximumLength = vn.Length;
    }
    kn.Buffer = (PWSTR)keyname;
    kn.Length = sizeof(keyname) - sizeof(WCHAR);
    kn.MaximumLength = sizeof(keyname);
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&k, KEY_QUERY_VALUE, &oa) < 0) return FALSE;
    trace_w("image path", vn.Buffer, vn.Length / sizeof(WCHAR));
    r = listed(k, &vn) || (long_path(&vn, lp, MAX_PATH + 4, &lv) && listed(k, &lv));
    NtClose(k);
    return r;
}

/* ---- VistaExt's API-set copies --------------------------------------------
 * Programs import from dozens of API sets, and every DLL they load imports
 * from the same ones again, so each copy is opened and mapped (as data) only
 * once per process and kept: whether it exists, and its sorted export names
 * for a binary search, as Windows' loader does it. All of this runs under the
 * loader's lock, one thread at a time. */
typedef struct { const BYTE *d; SIZE_T size; const IMAGE_SECTION_HEADER *sec; WORD nsec; } file_pe;

static DWORD f_off(const file_pe *f, DWORD rva)
{
    WORD i;
    for (i = 0; i < f->nsec; i++)
        if (rva >= f->sec[i].VirtualAddress && rva < f->sec[i].VirtualAddress + f->sec[i].SizeOfRawData)
            return rva - f->sec[i].VirtualAddress + f->sec[i].PointerToRawData;
    return 0;
}

typedef struct {
    char name[96];          /* "vxs-ms-win-core-synch-l1-2-0.dll", lower case */
    BOOL ok;                /* the copy exists and is readable */
    file_pe f;
    const DWORD *names;     /* file offsets of the export names, sorted */
    DWORD nnames;
} apiset_copy;

#define NCOPIES 160
static apiset_copy g_copies[NCOPIES];
static unsigned g_ncopies;

static int scmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static BOOL seq(const char *a, const char *b) { return !scmp(a, b); }

static void open_copy(apiset_copy *c)
{
    static WCHAR path[600];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE f = NULL, sec = NULL;
    void *view = NULL;
    SIZE_T vsize = 0, n = 0, i;
    const WCHAR *root = VX_KUSER_SYSTEM_ROOT;
    const IMAGE_DOS_HEADER *dh;
    const IMAGE_NT_HEADERS *nh;
    const IMAGE_EXPORT_DIRECTORY *ed;
    DWORD expdir, o, no;
    for (i = 0; "\\??\\"[i]; i++) path[n++] = "\\??\\"[i];
    for (i = 0; root[i] && n < 400; i++) path[n++] = root[i];
    for (i = 0; "\\System32\\"[i]; i++) path[n++] = "\\System32\\"[i];
    for (i = 0; c->name[i]; i++) path[n++] = (unsigned char)c->name[i];
    path[n] = 0;
    us.Buffer = path;
    us.Length = (USHORT)(n * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenFile(&f, FILE_READ_DATA | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE) < 0) return;
    if (NtCreateSection(&sec, SECTION_MAP_READ, NULL, NULL, PAGE_READONLY, SEC_COMMIT, f) >= 0)
        NtMapViewOfSection(sec, NtCurrentProcess(), &view, 0, 0, NULL, &vsize, ViewUnmap, 0, PAGE_READONLY);
    if (sec) NtClose(sec);
    NtClose(f);
    if (!view) return;
    dh = view;
    if (vsize < 0x200 || dh->e_magic != IMAGE_DOS_SIGNATURE || (SIZE_T)dh->e_lfanew + 0x200 > vsize) goto bad;
    nh = (const void *)((const BYTE *)view + dh->e_lfanew);
    if (nh->Signature != IMAGE_NT_SIGNATURE) goto bad;
    c->f.d = view; c->f.size = vsize;
    c->f.nsec = nh->FileHeader.NumberOfSections;
    c->f.sec = (const void *)((const BYTE *)&nh->OptionalHeader + nh->FileHeader.SizeOfOptionalHeader);
    if ((const BYTE *)(c->f.sec + c->f.nsec) > (const BYTE *)view + vsize) goto bad;
    if (nh->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        expdir = ((const IMAGE_NT_HEADERS64 *)nh)->OptionalHeader.DataDirectory[0].VirtualAddress;
    else
        expdir = ((const IMAGE_NT_HEADERS32 *)nh)->OptionalHeader.DataDirectory[0].VirtualAddress;
    o = f_off(&c->f, expdir);
    if (!o || o + sizeof(*ed) > vsize) goto bad;
    ed = (const void *)((const BYTE *)view + o);
    no = f_off(&c->f, ed->AddressOfNames);
    if (!no || no + 4 * (SIZE_T)ed->NumberOfNames > vsize) goto bad;
    c->names = (const void *)((const BYTE *)view + no);
    c->nnames = ed->NumberOfNames;
    c->ok = TRUE;               /* the view stays mapped for the life of the process */
    return;
bad:
    NtUnmapViewOfSection(NtCurrentProcess(), view);
}

static const char *copy_name(const apiset_copy *c, DWORD i)
{
    DWORD so = f_off(&c->f, c->names[i]);
    return so && so < c->f.size ? (const char *)c->f.d + so : "";
}

/* Is name exported by the copy? The names are sorted (the loader relies on
 * that too); an unsorted file still works through the slow path. */
static BOOL copy_exports(const apiset_copy *c, const char *name)
{
    DWORD lo = 0, hi = c->nnames, i;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        int r = scmp(name, copy_name(c, mid));
        if (!r) return TRUE;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    for (i = 0; i < c->nnames; i++) if (seq(name, copy_name(c, i))) return TRUE;
    return FALSE;
}

static apiset_copy *find_copy(const char *name)
{
    unsigned i;
    apiset_copy *c;
    SIZE_T n;
    for (i = 0; i < g_ncopies; i++) if (seq(g_copies[i].name, name)) return &g_copies[i];
    if (g_ncopies >= NCOPIES) return NULL;
    c = &g_copies[g_ncopies++];
    for (n = 0; name[n] && n < sizeof(c->name) - 1; n++) c->name[n] = name[n];
    c->name[n] = 0;
    open_copy(c);
    return c;
}

/* the INT of an import: names imported (NULL entry = by number) */
typedef struct { const BYTE *base; DWORD rva; BOOL is64; } thunks;

static BOOL thunk_at(const thunks *t, DWORD i, const char **name)
{
    ULONGLONG v;
    if (t->is64) v = ((const ULONGLONG *)(t->base + t->rva))[i];
    else v = ((const DWORD *)(t->base + t->rva))[i];
    if (!v) return FALSE;
    if (t->is64 ? (v >> 63) : (v >> 31)) *name = NULL;
    else *name = (const char *)(t->base + (DWORD)v + 2);
    return TRUE;
}

/* "api-ms-win-X.dll" -> "vxs-ms-win-X.dll" ("ext-" -> "vxe-") if VistaExt's
 * copy exists and exports every function imported from it */
static BOOL apiset_target(const char *dll, char *out, const thunks *t)
{
    apiset_copy *c;
    const char *nm;
    DWORD k;
    SIZE_T i;
    if (slen(dll) > 90) return FALSE;
    for (i = 0; dll[i]; i++) out[i] = lc(dll[i]);
    out[i] = 0;
    out[0] = 'v'; out[1] = 'x'; out[2] = out[2] == 'i' ? 's' : 'e';     /* api- / ext- */
    if (!(c = find_copy(out)) || !c->ok) return FALSE;
    for (k = 0; thunk_at(t, k, &nm); k++)
        if (!nm || !copy_exports(c, nm)) return FALSE;      /* by number, or missing: leave it */
    return TRUE;
}

/* ---- rewriting one image's import names ---------------------------------- */
static const struct { const char *from, *to; BOOL ordinals; } g_map[] = {
#define VX_COMPANION(dll, comp, numbers) { #dll ".dll", #comp ".dll", numbers },
    VX_COMPANIONS
#undef VX_COMPANION
};

/* DLLs Windows lacks, VistaExt's under its own names; state: 0 not yet
 * looked at, 1 VistaExt's copy is used, 2 Windows has the DLL itself */
static struct { const char *from, *to; int state; } g_new[] = {
#define VX_NEWDLL(dll, priv) { #dll ".dll", #priv ".dll", 0 },
    VX_NEWDLLS
#undef VX_NEWDLL
};

/* does <dir (count characters)>\<name> exist? dir: a Win32 path */
static BOOL file_in(const WCHAR *dir, SIZE_T count, const char *name)
{
    static WCHAR path[700];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_BASIC_INFORMATION fbi;
    SIZE_T n = 0, i;
    if (count > 500 || slen(name) > 100) return FALSE;
    for (i = 0; "\\??\\"[i]; i++) path[n++] = "\\??\\"[i];
    for (i = 0; i < count; i++) path[n++] = dir[i];
    path[n++] = '\\';
    for (i = 0; name[i]; i++) path[n++] = (unsigned char)name[i];
    path[n] = 0;
    us.Buffer = path;
    us.Length = (USHORT)(n * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtQueryAttributesFile(&oa, &fbi) >= 0;
}

/* the program's own folder: the .exe's (where the loader looks first) */
static BOOL in_program_dir(const char *name)
{
    UNICODE_STRING img = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    SIZE_T n = img.Length / sizeof(WCHAR);
    const WCHAR *b = img.Buffer;
    if (n > 4 && b[0] == '\\' && b[1] == '?' && b[2] == '?' && b[3] == '\\') { b += 4; n -= 4; }
    while (n && b[n - 1] != '\\') n--;
    return n > 1 && file_in(b, n - 1, name);
}

static const char *newdll_target(const char *dll)
{
    unsigned m;
    for (m = 0; m < sizeof(g_new) / sizeof(g_new[0]); m++) {
        const WCHAR *root = VX_KUSER_SYSTEM_ROOT;
        SIZE_T n = 0;
        static WCHAR sys[300];
        if (!ieq(dll, g_new[m].from)) continue;
        if (!g_new[m].state) {
            /* System32 (SysWOW64 in a 32-bit process: the file system
             * redirection applies to this call too) */
            for (; root[n] && n < 250; n++) sys[n] = root[n];
            sys[n++] = '\\'; sys[n++] = 'S'; sys[n++] = 'y'; sys[n++] = 's'; sys[n++] = 't'; sys[n++] = 'e';
            sys[n++] = 'm'; sys[n++] = '3'; sys[n++] = '2';
            g_new[m].state = file_in(sys, n, g_new[m].from) ? 2 : 1;
        }
        if (g_new[m].state == 2 || in_program_dir(g_new[m].from)) return NULL;
        return g_new[m].to;
    }
    return NULL;
}

/* the new name for one import (NULL: leave it) */
static const char *target(const char *dll, const thunks *t, char *buf)
{
    unsigned m;
    const char *nm;
    DWORD k;
    for (m = 0; m < sizeof(g_map) / sizeof(g_map[0]); m++) {
        if (!ieq(dll, g_map[m].from)) continue;
        if (!g_map[m].ordinals)
            for (k = 0; thunk_at(t, k, &nm); k++) if (!nm) return NULL;   /* by number: the companion may differ */
        return g_map[m].to;
    }
    {
        const char *nd = newdll_target(dll);
        if (nd) return nd;
    }
    if ((lc(dll[0]) == 'a' && lc(dll[1]) == 'p' && lc(dll[2]) == 'i' && dll[3] == '-') ||
        (lc(dll[0]) == 'e' && lc(dll[1]) == 'x' && lc(dll[2]) == 't' && dll[3] == '-'))
        return apiset_target(dll, buf, t) ? buf : NULL;
    return NULL;
}

static void rename_import(BYTE *base, DWORD name_rva, const char *to)
{
    char *dll = (char *)(base + name_rva);
    trace("import", dll, to);
    poke(dll, to, slen(to) + 1);
}

static void rewrite(BYTE *base)
{
    IMAGE_NT_HEADERS *nh = RtlImageNtHeader((HMODULE)base);
    IMAGE_DATA_DIRECTORY *dir;
    BOOL is64, changed = FALSE;
    char buf[128];
    if (!nh) return;
    is64 = nh->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    dir = is64 ? ((IMAGE_NT_HEADERS64 *)nh)->OptionalHeader.DataDirectory
               : ((IMAGE_NT_HEADERS32 *)nh)->OptionalHeader.DataDirectory;
    if (dir[1].VirtualAddress) {
        IMAGE_IMPORT_DESCRIPTOR *id = (void *)(base + dir[1].VirtualAddress);
        for (; id->Name; id++) {
            thunks t = { base, id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk, is64 };
            const char *to;
            if (!t.rva || !(to = target((const char *)base + id->Name, &t, buf))) continue;
            rename_import(base, id->Name, to);
            if (id->TimeDateStamp && id->OriginalFirstThunk) {      /* bound: resolve afresh */
                DWORD zero = 0;
                poke(&id->TimeDateStamp, &zero, sizeof(zero));
            }
            changed = TRUE;
        }
    }
    if (dir[13].VirtualAddress) {                    /* delay-loaded: the program's helper reads the name later */
        DWORD *dd = (DWORD *)(base + dir[13].VirtualAddress);
        for (; dd[1]; dd += 8) {                     /* attributes, name, hmodule, IAT, INT, ... */
            thunks t = { base, dd[4], is64 };
            const char *to;
            if (!(dd[0] & 1) || !t.rva) continue;   /* old VA format: leave it */
            if ((to = target((const char *)base + dd[1], &t, buf))) rename_import(base, dd[1], to);
        }
    }
    if (changed && dir[11].VirtualAddress) {          /* bound import table: drop it */
        IMAGE_DATA_DIRECTORY none = { 0, 0 };
        poke(&dir[11], &none, sizeof(none));
    }
}

/* a DLL from the Windows folder is never touched */
static BOOL in_windows(const UNICODE_STRING *full)
{
    const WCHAR *root = VX_KUSER_SYSTEM_ROOT;
    SIZE_T i, n = full ? full->Length / sizeof(WCHAR) : 0;
    if (!n) return TRUE;
    for (i = 0; root[i]; i++) if (i >= n || lcw(full->Buffer[i]) != lcw(root[i])) return FALSE;
    return i < n && full->Buffer[i] == '\\';
}

/* one rewrite at a time (the loader's lock covers most, not every mapping) */
static RTL_SRWLOCK g_rw_lock = RTL_SRWLOCK_INIT;

static void loaded(PVOID base, const UNICODE_STRING *full)
{
    if (!g_on || !base || in_windows(full)) return;
    RtlAcquireSRWLockExclusive(&g_rw_lock);
    rewrite(base);                    /* (names already rewritten at mapping are left as they are) */
    RtlReleaseSRWLockExclusive(&g_rw_lock);
}

static void ole32_loaded(PVOID base, const UNICODE_STRING *full);
static BOOL g_hooks_on;
static void CALLBACK dll_notification(ULONG reason, LDR_DLL_NOTIFICATION_DATA *data, void *ctx)
{
    if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */ && data) {
        if (g_hooks_on) ole32_loaded(data->Loaded.DllBase, data->Loaded.FullDllName);
        loaded(data->Loaded.DllBase, data->Loaded.FullDllName);
    }
}

/* the verifier's own load callback, in case it comes first on some Windows */
static void NTAPI verifier_load(PWSTR name, PVOID base, SIZE_T size, PVOID reserved)
{
    LDR_DATA_TABLE_ENTRY *e = NULL;
    if (g_on && LdrFindEntryForAddress(base, &e) >= 0 && e) loaded(base, &e->FullDllName);
}

/* ---- keeping the verifier out of the way ------------------------------------
 * Windows loads its standard verifier provider, verifier.dll, before this
 * one. Even with every check off (VerifierFlags 0x80000000, which vxpatch
 * sets) it hooks some hundred functions - heap, critical sections, waits,
 * TLS, files, registry - and turns on the debug "page heap" for every heap.
 * Measured on Windows 7: HeapAlloc+HeapFree 21 ns -> 770 ns, a critical
 * section 3 -> 17 ns, process start 2 -> 17 ms.
 *
 * When the verifier is on only for VistaExt, both are undone here, before
 * Windows applies them: verifier.dll's hook lists are emptied, and the page
 * heap flag it added to the process's global flags is taken away again.
 * Nothing is changed unless verifier.dll's provider descriptor is found and
 * looks exactly as expected, and nothing if someone chose real verifier
 * checks or page heap for this program. VX_KEEP_VERIFIER=1 turns this off. */
typedef struct { const char *name; PVOID old_addr, new_addr; } vrf_thunk;
static vrf_thunk g_no_thunks[1];

#define FLG_HEAP_PAGE_ALLOCS 0x02000000
#define VX_VERIFIER_FLAGS    0x80000000

static BOOL inside(const void *p, const BYTE *base, SIZE_T size)
{
    return (const BYTE *)p >= base && (const BYTE *)p < base + size;
}

static LDR_DATA_TABLE_ENTRY *find_module(const WCHAR *name)
{
    LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList, *e;
    for (e = head->Flink; e && e != head; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY *m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        const UNICODE_STRING *full = &m->FullDllName;
        SIZE_T n = full->Length / sizeof(WCHAR), b = n, i;
        while (b && full->Buffer[b - 1] != '\\') b--;
        for (i = 0; name[i] && b + i < n && lcw(full->Buffer[b + i]) == name[i]; i++) ;
        if (!name[i] && b + i == n) return m;
    }
    return NULL;
}

/* this program's GlobalFlag as set in its Image File Execution Options */
static BOOL ifeo_globalflag(ULONG *out)
{
    static WCHAR path[400];
    static const WCHAR pre[] = L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
    const UNICODE_STRING *img = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    SIZE_T n = 0, i, b = img->Length / sizeof(WCHAR), e = b;
    UNICODE_STRING kn, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE k;
    union { KEY_VALUE_PARTIAL_INFORMATION i; BYTE b[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64]; } v;
    ULONG got, x = 0;
    NTSTATUS st;
    while (b && img->Buffer[b - 1] != '\\') b--;
    if (e - b > 120) return FALSE;
    for (i = 0; pre[i]; i++) path[n++] = pre[i];
    for (i = b; i < e; i++) path[n++] = img->Buffer[i];
    path[n] = 0;
    kn.Buffer = path; kn.Length = (USHORT)(n * sizeof(WCHAR)); kn.MaximumLength = kn.Length + sizeof(WCHAR);
    vn.Buffer = (PWSTR)L"GlobalFlag"; vn.Length = 10 * sizeof(WCHAR); vn.MaximumLength = vn.Length + sizeof(WCHAR);
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&k, KEY_QUERY_VALUE, &oa) < 0) return FALSE;
    st = NtQueryValueKey(k, &vn, KeyValuePartialInformation, &v, sizeof(v) - 2, &got);
    NtClose(k);
    if (st < 0) return FALSE;
    if (v.i.Type == REG_DWORD && v.i.DataLength == 4) { *out = *(ULONG *)v.i.Data; return TRUE; }
    if (v.i.Type == REG_SZ) {                        /* gflags may write "0x02000100" */
        const WCHAR *s = (const WCHAR *)v.i.Data;
        SIZE_T len = v.i.DataLength / sizeof(WCHAR);
        i = 0;
        if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
        for (; i < len && s[i]; i++) {
            WCHAR c = lcw(s[i]);
            if (c >= '0' && c <= '9') x = x * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') x = x * 16 + (c - 'a' + 10);
            else return FALSE;
        }
        *out = x;
        return TRUE;
    }
    return FALSE;
}

static void quiet_verifier(void)
{
    LDR_DATA_TABLE_ENTRY *v = find_module(L"verifier.dll"), *nt = find_module(L"ntdll.dll");
    const BYTE *vb, *nb;
    SIZE_T vsize, nsize;
    IMAGE_NT_HEADERS *nh;
    IMAGE_SECTION_HEADER *sec;
    vrf_provider *desc = NULL;
    WORD i;
    ULONG gf;
    if (!v || !nt || env_set(L"VX_KEEP_VERIFIER")) return;
    vb = v->DllBase; nb = nt->DllBase;
    if (!(nh = RtlImageNtHeader((HMODULE)vb)) || !RtlImageNtHeader((HMODULE)nb)) return;
    vsize = nh->OptionalHeader.SizeOfImage;
    nsize = RtlImageNtHeader((HMODULE)nb)->OptionalHeader.SizeOfImage;
    /* the descriptor verifier.dll handed to Windows: in one of its writable
     * sections, with its own hook list and callbacks, and the fields Windows
     * fills in afterwards already filled in (a stack trace function in ntdll) */
    sec = IMAGE_FIRST_SECTION(nh);
    for (i = 0; i < nh->FileHeader.NumberOfSections && !desc; i++, sec++) {
        const BYTE *p = vb + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        for (; p + sizeof(vrf_provider) <= end; p += sizeof(void *)) {
            const vrf_provider *d = (const vrf_provider *)p;
            if (d->Length == sizeof(vrf_provider) && inside(d->ProviderDlls, vb, vsize) &&
                inside(d->ProviderDllLoadCallback, vb, vsize) && inside(d->RtlpGetStackTraceAddress, nb, nsize) &&
                d->VerifierImage) {
                if (desc) return;                    /* two candidates: not sure, leave it */
                desc = (vrf_provider *)d;
            }
        }
    }
    if (!desc) { trace("verifier.dll: descriptor not found, left as it is", NULL, NULL); return; }
    if (desc->VerifierFlags != VX_VERIFIER_FLAGS) {
        trace("verifier.dll: verifier checks chosen for this program, left as they are", NULL, NULL);
        return;
    }
    /* empty every hook list (verifier.dll's own code only reaches the old
     * functions through hooks that are now never installed) */
    {
        vrf_dll *dl = desc->ProviderDlls;
        SIZE_T k;
        for (k = 0; k < 64 && inside(dl, vb, vsize) && dl->name; k++, dl++) {
            if (!inside(dl->name, vb, vsize)) return;
            dl->thunks = g_no_thunks;
        }
        trace("verifier.dll: hooks switched off", NULL, NULL);
    }
    /* page heap: only if it was not asked for */
    if ((NtCurrentTeb()->Peb->NtGlobalFlag & FLG_HEAP_PAGE_ALLOCS) && ifeo_globalflag(&gf) && !(gf & FLG_HEAP_PAGE_ALLOCS)) {
        NtCurrentTeb()->Peb->NtGlobalFlag &= ~FLG_HEAP_PAGE_ALLOCS;
        trace("verifier.dll: page heap switched off", NULL, NULL);
    }
}

/* ---- one Windows version in the registry too --------------------------------
 * A version setting changes what GetVersionEx, RtlGetVersion and
 * VerifyVersionInfo report (kernelvx writes it into the PEB). Programs and
 * installers also read HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
 * (CurrentBuild, ProductName, DisplayVersion, ...), often through Windows'
 * own DLLs (advapi32, shlwapi, .NET). Windows' verifier hooks the imports of
 * every DLL in the process, Windows' own included, for functions a provider
 * names, so this provider names ntdll's NtQueryValueKey: while the PEB says
 * another version than the real one, those values of that key are answered
 * from the PEB, the way that Windows itself would show them. Nothing else is
 * touched, and nothing at all without a version setting. */
typedef NTSTATUS (NTAPI *qvk_fn)(HANDLE, PUNICODE_STRING, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
static NTSTATUS NTAPI hook_qvk(HANDLE key, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS cls, PVOID buf, ULONG len, PULONG res);
static HRESULT WINAPI hook_cci(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out);
static HRESULT WINAPI hook_ccie(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *srv, DWORD n, MULTI_QI *mqi);
static HRESULT WINAPI hook_cspb(IUnknown *p, DWORD a, DWORD b, OLECHAR *c, DWORD d, DWORD e, RPC_AUTH_IDENTITY_HANDLE f, DWORD g);
/* the real functions, once redirected */
static void *g_real_qvk, *g_real_cci, *g_real_ccie, *g_real_cspb;

static ULONG g_real_build;            /* this Windows' build, from ntdll */


/* is the program told another version than the real one? */
static BOOL version_told(ULONG *maj, ULONG *min, ULONG *bld)
{
    vx_peb_version(maj, min, bld);
    return *maj != VX_KUSER_NT_MAJOR || *min != VX_KUSER_NT_MINOR ||
           (g_real_build && *bld != g_real_build);
}

static BOOL ueq(const UNICODE_STRING *u, const WCHAR *w)
{
    SIZE_T n = u->Length / sizeof(WCHAR), i;
    for (i = 0; i < n && w[i]; i++) if (lcw(u->Buffer[i]) != lcw(w[i])) return FALSE;
    return i == n && !w[i];
}

enum { V_CURVER, V_BUILD, V_BUILDNUM, V_MAJORNUM, V_MINORNUM, V_CSD, V_CSDBUILD, V_PRODUCT, V_EDITION,
       V_RELEASEID, V_DISPLAYVER, V_UBR, V_BUILDLAB, V_BUILDLABEX, V_COUNT };
static const WCHAR *g_vnames[V_COUNT] = { L"CurrentVersion", L"CurrentBuild", L"CurrentBuildNumber",
    L"CurrentMajorVersionNumber", L"CurrentMinorVersionNumber", L"CSDVersion", L"CSDBuildNumber", L"ProductName",
    L"EditionID", L"ReleaseId", L"DisplayVersion", L"UBR", L"BuildLab", L"BuildLabEx" };

/* HKLM\SOFTWARE\[Wow6432Node\]Microsoft\Windows NT\CurrentVersion ? */
static BOOL is_cv_key(HANDLE key)
{
    static const WCHAR pre[] = L"\\REGISTRY\\MACHINE\\SOFTWARE\\", suf[] = L"\\Microsoft\\Windows NT\\CurrentVersion";
    union { KEY_NAME_INFORMATION k; BYTE b[sizeof(KEY_NAME_INFORMATION) + 400]; } u;
    ULONG got = 0, n, i, np = sizeof(pre) / sizeof(WCHAR) - 1, ns = sizeof(suf) / sizeof(WCHAR) - 1;
    if (NtQueryKey(key, KeyNameInformation, &u, sizeof(u) - 2, &got) < 0) return FALSE;
    n = u.k.NameLength / sizeof(WCHAR);
    if (n != np - 1 + ns && n != np + 11 + ns) return FALSE;     /* "SOFTWARE" or "SOFTWARE\Wow6432Node" */
    for (i = 0; i < np - 1; i++) if (lcw(u.k.Name[i]) != lcw(pre[i])) return FALSE;
    for (i = 0; i < ns; i++) if (lcw(u.k.Name[n - ns + i]) != lcw(suf[i])) return FALSE;
    return TRUE;
}

static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
static void wcat(WCHAR *d, SIZE_T cch, const WCHAR *s) { SIZE_T n = wlen(d); while (*s && n + 1 < cch) d[n++] = *s++; d[n] = 0; }
static void wnum(WCHAR *d, SIZE_T cch, ULONG v)
{
    WCHAR t[12];
    int n = 0;
    do { t[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n) { WCHAR c[2] = { t[--n], 0 }; wcat(d, cch, c); }
}


static NTSTATUS call_real_qvk(HANDLE key, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS cls, PVOID buf, ULONG len, PULONG res)
{
    qvk_fn f = (qvk_fn)g_real_qvk;
    return f ? f(key, name, cls, buf, len, res) : NtQueryValueKey(key, name, cls, buf, len, res);
}

/* the real string value (for the edition) */
static BOOL real_sz(HANDLE key, const WCHAR *vname, WCHAR *out, SIZE_T cch)
{
    union { KEY_VALUE_PARTIAL_INFORMATION i; BYTE b[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 200]; } v;
    UNICODE_STRING us;
    ULONG got, n, i;
    us.Buffer = (PWSTR)vname; us.Length = (USHORT)(wlen(vname) * sizeof(WCHAR)); us.MaximumLength = us.Length;
    out[0] = 0;
    if (call_real_qvk(key, &us, KeyValuePartialInformation, &v, sizeof(v) - 2, &got) < 0 || v.i.Type != REG_SZ) return FALSE;
    n = v.i.DataLength / sizeof(WCHAR);
    for (i = 0; i < n && i + 1 < cch && ((WCHAR *)v.i.Data)[i]; i++) out[i] = ((WCHAR *)v.i.Data)[i];
    out[i] = 0;
    return TRUE;
}

static BOOL has(const WCHAR *s, const WCHAR *w)
{
    SIZE_T i, j;
    for (i = 0; s[i]; i++) {
        for (j = 0; w[j] && s[i + j] && lcw(s[i + j]) == lcw(w[j]); j++) ;
        if (!w[j]) return TRUE;
    }
    return FALSE;
}

/* the value as the told Windows has it: type and data (FALSE: it has none) */
static BOOL fake_value(HANDLE key, int which, ULONG maj, ULONG min, ULONG bld, ULONG *type, BYTE *data, ULONG *size)
{
    WCHAR *w = (WCHAR *)data, ed[64];
    const vx_release *r = vx_find_release(bld);
    BOOL ten = maj >= 10;
    *type = REG_SZ;
    w[0] = 0;
    switch (which) {
    case V_CURVER:                          /* Windows 10 and 11 still say 6.3 here */
        if (ten) wcat(w, 64, L"6.3");
        else { wnum(w, 64, maj); wcat(w, 64, L"."); wnum(w, 64, min); }
        break;
    case V_BUILD: case V_BUILDNUM: wnum(w, 64, bld); break;
    case V_MAJORNUM: case V_MINORNUM:
        if (!ten) return FALSE;
        *type = REG_DWORD; *(ULONG *)data = which == V_MAJORNUM ? maj : min; *size = 4;
        return TRUE;
    case V_CSD: {
        const UNICODE_STRING *c = (const UNICODE_STRING *)(vx_peb() + VX_PEB_CSDSTR);
        SIZE_T k;
        if (!c->Length || !c->Buffer) return FALSE;
        for (k = 0; k < c->Length / sizeof(WCHAR) && k < 63; k++) w[k] = c->Buffer[k];
        w[k] = 0;
        break;
    }
    case V_CSDBUILD: return FALSE;           /* only the real Vista/7 have it: then not asked here */
    case V_EDITION: case V_PRODUCT: {
        const WCHAR *e;                      /* the edition, carried over to the told Windows */
        real_sz(key, L"EditionID", ed, 64);
        if (ten) {
            e = has(ed, L"Enterprise") ? L"Enterprise" : (has(ed, L"Home") || has(ed, L"Starter")) ? L"Home" : L"Pro";
            if (which == V_EDITION) wcat(w, 64, e[0] == 'P' ? L"Professional" : e[0] == 'H' ? L"Core" : L"Enterprise");
            /* Windows 11 too says "Windows 10" here; programs go by the build */
            else { wcat(w, 64, L"Windows 10 "); wcat(w, 64, e); }
        } else {
            if (which == V_EDITION) { wcat(w, 64, ed); break; }
            e = has(ed, L"Enterprise") ? L"Enterprise" : has(ed, L"Ultimate") ? L"Ultimate" :
                has(ed, L"HomePremium") ? L"Home Premium" : has(ed, L"HomeBasic") ? L"Home Basic" :
                has(ed, L"Starter") ? L"Starter" : L"Professional";
            if (maj == 6 && min == 0) { wcat(w, 64, L"Windows Vista (TM) "); if (e[0] == 'P') e = L"Business"; }
            else if (maj == 6 && min == 1) wcat(w, 64, L"Windows 7 ");
            else if (maj == 6 && min == 2) { wcat(w, 64, L"Windows 8 "); if (e[0] != 'E') e = L"Pro"; }
            else { wcat(w, 64, L"Windows 8.1 "); if (e[0] != 'E') e = L"Pro"; }
            wcat(w, 64, e);
        }
        break;
    }
    case V_RELEASEID: case V_DISPLAYVER:
        if (!ten || !r) return FALSE;
        wcat(w, 64, which == V_RELEASEID ? r->rel : r->disp);
        break;
    case V_UBR:
        if (!ten) return FALSE;
        *type = REG_DWORD; *(ULONG *)data = r ? r->ubr : 0; *size = 4;
        return TRUE;
    case V_BUILDLAB: case V_BUILDLABEX:
        if (r && r->lab && ten) wcat(w, 64, r->lab);
        else { wnum(w, 64, bld); wcat(w, 64, L".winmain.000000-0000"); }
        if (which == V_BUILDLABEX) {                    /* "19041.1.amd64fre.vb_release.191206-1406" */
            WCHAR t[96] = L"";
            SIZE_T k = 0;
            wnum(t, 96, bld);
            wcat(t, 96, sizeof(void *) == 8 ? L".1.amd64fre" : L".1.x86fre");
            while (w[k] && w[k] != '.') k++;
            wcat(t, 96, w + k);
            w[0] = 0;
            wcat(w, 64, t);
        }
        break;
    }
    *size = (ULONG)((wlen(w) + 1) * sizeof(WCHAR));
    return TRUE;
}

static NTSTATUS NTAPI hook_qvk(HANDLE key, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS cls, PVOID buf, ULONG len, PULONG res)
{
    ULONG maj, min, bld, type, size = 0, hdr, need, nl;
    BYTE data[160];
    int which = -1, i;
    if (name && name->Length && name->Buffer && name->Length < 64 && version_told(&maj, &min, &bld))
        for (i = 0; i < V_COUNT; i++) if (ueq(name, g_vnames[i])) { which = i; break; }
    if (g_trace) {
        static LONG calls;
        if (name && name->Buffer && InterlockedIncrement(&calls) <= 40) trace_w("NtQueryValueKey", name->Buffer, name->Length / 2);
    }
    if (which < 0 || (cls != KeyValueBasicInformation && cls != KeyValueFullInformation &&
                      cls != KeyValuePartialInformation && cls != KeyValuePartialInformationAlign64) ||
        !is_cv_key(key))
        return call_real_qvk(key, name, cls, buf, len, res);
    if (which == V_CSDBUILD && maj == 6 && min <= 1) return call_real_qvk(key, name, cls, buf, len, res);
    if (!fake_value(key, which, maj, min, bld, &type, data, &size)) return (NTSTATUS)0xC0000034;  /* not found */
    trace("registry: version value answered from the PEB", NULL, NULL);
    nl = name->Length;
    switch (cls) {
    case KeyValueBasicInformation:   hdr = FIELD_OFFSET(KEY_VALUE_BASIC_INFORMATION, Name); need = hdr + nl; break;
    case KeyValueFullInformation:    hdr = FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name); need = ((hdr + nl + 7) & ~7u) + size; break;
    case KeyValuePartialInformation: hdr = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data); need = hdr + size; break;
    default:                         hdr = 8; need = hdr + size; break;      /* ...Align64: type, length, data */
    }
    if (res) *res = need;
    if (len < hdr) return (NTSTATUS)0xC0000023;                          /* STATUS_BUFFER_TOO_SMALL */
    {
        BYTE *o = buf;
        ULONG k;
        switch (cls) {
        case KeyValueBasicInformation: {
            KEY_VALUE_BASIC_INFORMATION *b = buf;
            b->TitleIndex = 0; b->Type = type; b->NameLength = nl;
            for (k = 0; k < nl && hdr + k < len; k++) o[hdr + k] = ((BYTE *)name->Buffer)[k];
            break;
        }
        case KeyValueFullInformation: {
            KEY_VALUE_FULL_INFORMATION *f = buf;
            ULONG off = (hdr + nl + 7) & ~7u;
            f->TitleIndex = 0; f->Type = type; f->NameLength = nl; f->DataOffset = off; f->DataLength = size;
            for (k = 0; k < nl && hdr + k < len; k++) o[hdr + k] = ((BYTE *)name->Buffer)[k];
            for (k = 0; k < size && off + k < len; k++) o[off + k] = data[k];
            break;
        }
        case KeyValuePartialInformation: {
            KEY_VALUE_PARTIAL_INFORMATION *p = buf;
            p->TitleIndex = 0; p->Type = type; p->DataLength = size;
            for (k = 0; k < size && hdr + k < len; k++) o[hdr + k] = data[k];
            break;
        }
        default:
            ((ULONG *)buf)[0] = type; ((ULONG *)buf)[1] = size;
            for (k = 0; k < size && hdr + k < len; k++) o[hdr + k] = data[k];
            break;
        }
    }
    return len < need ? STATUS_BUFFER_OVERFLOW : 0;
}

/* ---- and in WMI ------------------------------------------------------------
 * Win32_OperatingSystem comes from WMI's service, which knows nothing of the
 * program's setting. olevx (VistaExt's ole32 companion) wraps the WMI locator
 * a program creates, so the Version, BuildNumber, Caption and service pack of
 * the operating system objects it gets back match the told version too. The
 * hooks here catch the locators that Windows' own DLLs create for it (.NET's
 * System.Management, wbemdisp), and take the wrapper off again where COM
 * itself needs its own object (CoSetProxyBlanket). */
typedef void *(WINAPI *wrap_fn)(void *obj, REFIID iid);
static wrap_fn g_wrap, g_unwrap;
static BOOL wmi_hooks(void)
{
    static LONG tried;
    UNICODE_STRING dll;
    ANSI_STRING fn;
    HMODULE h = NULL;
    if (g_wrap) return TRUE;
    if (tried) return FALSE;
    tried = 1;
    dll.Buffer = (PWSTR)L"olevx.dll"; dll.Length = 9 * sizeof(WCHAR); dll.MaximumLength = dll.Length + 2;
    if (LdrLoadDll(NULL, NULL, &dll, &h) < 0 || !h) return FALSE;
    fn.Buffer = (PCHAR)"VxWmiUnwrap"; fn.Length = 11; fn.MaximumLength = 12;
    if (LdrGetProcedureAddress(h, &fn, 0, (void **)&g_unwrap) < 0) return FALSE;
    fn.Buffer = (PCHAR)"VxWmiWrap"; fn.Length = 9; fn.MaximumLength = 10;
    if (LdrGetProcedureAddress(h, &fn, 0, (void **)&g_wrap) < 0) { g_unwrap = NULL; return FALSE; }
    return TRUE;
}

static const GUID g_clsid_locator = { 0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };
static BOOL guid_eq(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

typedef HRESULT (WINAPI *cci_fn)(REFCLSID, IUnknown *, DWORD, REFIID, void **);
typedef HRESULT (WINAPI *ccie_fn)(REFCLSID, IUnknown *, DWORD, COSERVERINFO *, DWORD, MULTI_QI *);
typedef HRESULT (WINAPI *cspb_fn)(IUnknown *, DWORD, DWORD, OLECHAR *, DWORD, DWORD, RPC_AUTH_IDENTITY_HANDLE, DWORD);

static HRESULT WINAPI hook_cci(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out)
{
    ULONG a, b, c;
    HRESULT hr = ((cci_fn)g_real_cci)(clsid, outer, ctx, iid, out);
    if (hr >= 0 && out && *out && clsid && guid_eq(clsid, &g_clsid_locator) && version_told(&a, &b, &c) && wmi_hooks())
        *out = g_wrap(*out, iid);
    return hr;
}

static HRESULT WINAPI hook_ccie(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *srv, DWORD n, MULTI_QI *mqi)
{
    ULONG a, b, c, i;
    HRESULT hr = ((ccie_fn)g_real_ccie)(clsid, outer, ctx, srv, n, mqi);
    if (hr >= 0 && mqi && clsid && guid_eq(clsid, &g_clsid_locator) && version_told(&a, &b, &c) && wmi_hooks())
        for (i = 0; i < n; i++) if (mqi[i].hr >= 0 && mqi[i].pItf) mqi[i].pItf = g_wrap(mqi[i].pItf, mqi[i].pIID);
    return hr;
}

static HRESULT WINAPI hook_cspb(IUnknown *p, DWORD a, DWORD b, OLECHAR *c, DWORD d, DWORD e, RPC_AUTH_IDENTITY_HANDLE f, DWORD g)
{
    if (p && g_unwrap) p = g_unwrap(p, NULL);
    return ((cspb_fn)g_real_cspb)(p, a, b, c, d, e, f, g);
}

/* ---- redirecting a Windows function, in this process only ---------------------
 * The DLL's export entry is pointed at VistaExt's function, so every DLL
 * loaded from now on (and GetProcAddress) gets it; DLLs already loaded get
 * their import entries changed. This does not change Windows' code (only
 * stub_hook below does, for NtQueryValueKey).
 * An export entry is 32 bits relative to the DLL: on x64 it points at a
 * small jump placed right after the DLL, which goes on to the function. */
static void *jump_near(BYTE *base, SIZE_T size, void *target)
{
#ifdef _WIN64
    static BYTE *pages[4], *owner[4];
    static SIZE_T used[4];
    unsigned i;
    BYTE *p = NULL;
    BYTE code[16] = { 0xFF, 0x25, 0, 0, 0, 0 };                    /* jmp [rip+0] */
    for (i = 0; i < 4 && owner[i] && owner[i] != base; i++) ;
    if (i == 4) return NULL;
    if (!owner[i]) {
        /* read and execute only: each jump is written through poke(), like
         * changes to Windows' own code (another thread may already run an
         * earlier jump on this page while one is added) */
        ULONG_PTR a = ((ULONG_PTR)base + size + 0xFFFF) & ~(ULONG_PTR)0xFFFF;
        for (; a < (ULONG_PTR)base + 0x7FFF0000; a += 0x10000) {
            void *q = (void *)a;
            SIZE_T sz = 4096;
            if (NtAllocateVirtualMemory(NtCurrentProcess(), &q, 0, &sz, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READ) >= 0) {
                pages[i] = q;
                owner[i] = base;
                break;
            }
        }
        if (!owner[i]) return NULL;
    }
    if (used[i] + 16 > 4096) return NULL;
    p = pages[i] + used[i];
    used[i] += 16;
    *(void **)(code + 6) = target;
    poke(p, code, 14);
    return p;
#else
    (void)base; (void)size;
    return target;
#endif
}

/* point the export "name" of the DLL at "hook"; returns the real function.
 * *real is set first: once the export entry points at the hook, another
 * thread's GetProcAddress can reach it, and the hook calls *real. */
static void *export_hook(BYTE *base, const char *name, void *hook, void **real)
{
    IMAGE_NT_HEADERS *nh = RtlImageNtHeader((HMODULE)base);
    IMAGE_DATA_DIRECTORY *dd;
    IMAGE_EXPORT_DIRECTORY *ed;
    DWORD *names, *funcs, i, rva;
    WORD *ords;
    BYTE *j;
    if (!nh) return NULL;
    dd = nh->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? &((IMAGE_NT_HEADERS64 *)nh)->OptionalHeader.DataDirectory[0]
                                                                  : &((IMAGE_NT_HEADERS32 *)nh)->OptionalHeader.DataDirectory[0];
    if (!dd->VirtualAddress) return NULL;
    ed = (void *)(base + dd->VirtualAddress);
    names = (DWORD *)(base + ed->AddressOfNames);
    ords = (WORD *)(base + ed->AddressOfNameOrdinals);
    funcs = (DWORD *)(base + ed->AddressOfFunctions);
    for (i = 0; i < ed->NumberOfNames; i++) {
        if (!seq((const char *)base + names[i], name)) continue;
        rva = funcs[ords[i]];
        if (rva >= dd->VirtualAddress && rva < dd->VirtualAddress + dd->Size) return NULL;    /* a forwarder */
        if (!(j = jump_near(base, nh->OptionalHeader.SizeOfImage, hook))) return NULL;
        if (!*real) *real = base + rva;
        {
            DWORD nr = (DWORD)((BYTE *)j - base);
            poke(&funcs[ords[i]], &nr, sizeof(nr));
        }
        return base + rva;
    }
    return NULL;
}

/* import entries of loaded DLLs that hold "real": now "hook" */
static void import_hook(void *real, void *hook)
{
    LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InLoadOrderModuleList, *e;
    HMODULE self = NULL;
    RtlPcToFileHeader((void *)import_hook, (void **)&self);
    for (e = head->Flink; e && e != head; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY *m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        BYTE *base = m->DllBase;
        IMAGE_NT_HEADERS *nh;
        IMAGE_DATA_DIRECTORY *dir;
        IMAGE_IMPORT_DESCRIPTOR *id;
        if (!base || (HMODULE)base == self || !(nh = RtlImageNtHeader((HMODULE)base))) continue;
        dir = nh->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? ((IMAGE_NT_HEADERS64 *)nh)->OptionalHeader.DataDirectory
                                                                       : ((IMAGE_NT_HEADERS32 *)nh)->OptionalHeader.DataDirectory;
        if (!dir[1].VirtualAddress) continue;
        for (id = (void *)(base + dir[1].VirtualAddress); id->Name; id++) {
            void **iat = (void **)(base + id->FirstThunk);
            for (; id->FirstThunk && *iat; iat++)
                if (*iat == real) { poke(iat, &hook, sizeof(hook)); trace_w("import entry redirected in", m->BaseDllName.Buffer, m->BaseDllName.Length / 2); }
        }
    }
}

static void redirect(BYTE *dll, const char *name, void *hook, void **real)
{
    void *r = export_hook(dll, name, hook, real);
    if (!r) return;
    import_hook(r, hook);
    trace("redirected", name, NULL);
}

/* ole32 arrives later: when it is loaded, before anything is linked to it */
static void ole32_loaded(PVOID base, const UNICODE_STRING *full)
{
    static const WCHAR o[] = L"ole32.dll";
    SIZE_T n = full ? full->Length / sizeof(WCHAR) : 0, i, b = n;
    if (g_real_cci || !base || n < 9) return;
    while (b && full->Buffer[b - 1] != '\\') b--;
    for (i = 0; o[i]; i++) if (b + i >= n || lcw(full->Buffer[b + i]) != o[i]) return;
    if (b + i != n) return;
    redirect(base, "CoCreateInstance", (void *)hook_cci, &g_real_cci);
    redirect(base, "CoCreateInstanceEx", (void *)hook_ccie, &g_real_ccie);
    redirect(base, "CoSetProxyBlanket", (void *)hook_cspb, &g_real_cspb);
}

/* NtQueryValueKey itself: its code in ntdll is a short system-call stub
 * that does not depend on where it runs, so it is copied, and the original
 * gets a jump to VistaExt's function at its start. Every caller then goes
 * through it, also code that kept the address from before (Windows' own DLLs
 * do). Only the known stub shapes of Vista and 7 are changed:
 *   x64:         mov r10,rcx / mov eax,N / syscall / ret
 *   x86:         mov eax,N / mov edx,7FFE0300h / call [edx] / ret 14h
 *   x86 (WOW64): mov eax,N / mov ecx,M (or xor ecx,ecx) / lea edx,[esp+4] /
 *                call fs:[0C0h] / add esp,4 / ret 14h
 * and Windows 8's 32-bit stub on 64-bit Windows (read from its SysWOW64\ntdll.dll):
 *   x86 (WOW64): mov eax,N / call fs:[0C0h] / ret n
 * (Windows 8's x64 stub has the x64 shape above.) */
static SIZE_T stub_length(const BYTE *p)
{
#ifdef _WIN64
    if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8 && p[8] == 0x0F && p[9] == 0x05 && p[10] == 0xC3) return 11;
#else
    if (p[0] != 0xB8) return 0;
    if (p[5] == 0xBA && p[10] == 0xFF && p[11] == 0x12 && p[12] == 0xC2) return 15;               /* native */
    if (p[5] == 0xB9 && p[10] == 0x8D && p[11] == 0x54 && p[12] == 0x24 && p[13] == 0x04 && p[14] == 0x64 &&
        p[15] == 0xFF && p[16] == 0x15 && p[21] == 0x83 && p[22] == 0xC4 && p[23] == 0x04 && p[24] == 0xC2) return 27;
    if (p[5] == 0x33 && p[6] == 0xC9 && p[7] == 0x8D && p[8] == 0x54 && p[9] == 0x24 && p[10] == 0x04 && p[11] == 0x64 &&
        p[12] == 0xFF && p[13] == 0x15 && p[18] == 0x83 && p[19] == 0xC4 && p[20] == 0x04 && p[21] == 0xC2) return 24;
    if (p[5] == 0x64 && p[6] == 0xFF && p[7] == 0x15 && p[8] == 0xC0 && p[9] == 0 && p[10] == 0 && p[11] == 0 && p[12] == 0xC2) return 15;
#endif
    return 0;
}

static BOOL stub_hook(BYTE *nt, const char *name, void *hook, void **real)
{
    ANSI_STRING fn;
    BYTE *p = NULL, *copy = NULL, *target, jmp[5];
    SIZE_T len, i, sz = 4096;
    ULONG old;
    LDR_DATA_TABLE_ENTRY *m = find_module(L"ntdll.dll");
    fn.Buffer = (PCHAR)name; fn.Length = (USHORT)slen(name); fn.MaximumLength = fn.Length + 1;
    if (!m || LdrGetProcedureAddress((HMODULE)nt, &fn, 0, (void **)&p) < 0 || !p) return FALSE;
    if (!(len = stub_length(p))) { trace("unknown code, left alone:", name, NULL); return FALSE; }
    /* the original stub, copied (it runs anywhere) */
    if (NtAllocateVirtualMemory(NtCurrentProcess(), (void **)&copy, 0, &sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) < 0)
        return FALSE;
    for (i = 0; i < len; i++) copy[i] = p[i];
    NtProtectVirtualMemory(NtCurrentProcess(), (void **)&copy, &sz, PAGE_EXECUTE_READ, &old);
    *real = copy;
    /* a 5-byte jump at its start: to the hook (x86), or to a jump to it
     * placed within 2 GB of ntdll (x64) */
    if (!(target = jump_near(nt, m->SizeOfImage, hook))) { *real = NULL; return FALSE; }
    jmp[0] = 0xE9;
    *(LONG *)(jmp + 1) = (LONG)(target - (p + 5));
    poke(p, jmp, 5);
    trace("its code now goes to VistaExt's:", name, NULL);
    return TRUE;
}

#if VX_OS >= 602 || VX_OS == 0
/* ---- rewriting a DLL's imports as soon as it is mapped (Windows 8) --------
 * After every successful mapping into this process of an image from
 * outside the Windows folder, its import names are rewritten, before the
 * loader resolves them. The Windows folder is recognised by the same kind
 * of name the mapping reports (\Device\HarddiskVolumeN\Windows\...): the
 * folder ntdll itself was mapped from, one level up. */
typedef NTSTATUS (NTAPI *mvs_fn)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, const LARGE_INTEGER *, SIZE_T *,
                                 SECTION_INHERIT, ULONG, ULONG);
static void *g_real_mvs;
static WCHAR g_winnt[300];
static SIZE_T g_winnt_len;
typedef union { UNICODE_STRING us; BYTE raw[sizeof(UNICODE_STRING) + 1040]; } mapped_name;

static BOOL name_of_mapping(void *base, mapped_name *nb)
{
    return NtQueryVirtualMemory(NtCurrentProcess(), base, (MEMORY_INFORMATION_CLASS)2 /* MemoryMappedFilenameInformation */,
                                nb, sizeof(*nb) - sizeof(WCHAR), NULL) >= 0 && nb->us.Buffer && nb->us.Length;
}

static BOOL windows_folder_nt(void)
{
    LDR_DATA_TABLE_ENTRY *nt = find_module(L"ntdll.dll");
    mapped_name nb;
    SIZE_T n, i, cut = 0, seen = 0;
    if (!nt || !name_of_mapping(nt->DllBase, &nb)) return FALSE;
    n = nb.us.Length / sizeof(WCHAR);
    for (i = n; i > 0; i--) if (nb.us.Buffer[i - 1] == '\\' && ++seen == 2) { cut = i - 1; break; }   /* drop \System32\ntdll.dll */
    if (!cut || cut >= sizeof(g_winnt) / sizeof(WCHAR)) return FALSE;
    for (i = 0; i < cut; i++) g_winnt[i] = nb.us.Buffer[i];
    g_winnt[cut] = 0;
    g_winnt_len = cut;
    return TRUE;
}

static BOOL in_windows_nt(const UNICODE_STRING *u)
{
    SIZE_T i, n = u->Length / sizeof(WCHAR);
    if (!g_winnt_len || n <= g_winnt_len) return !g_winnt_len;          /* unknown: never touched */
    for (i = 0; i < g_winnt_len; i++) if (lcw(u->Buffer[i]) != lcw(g_winnt[i])) return FALSE;
    return u->Buffer[g_winnt_len] == '\\';
}

static NTSTATUS NTAPI hook_mvs(HANDLE sec, HANDLE proc, PVOID *base, ULONG_PTR zb, SIZE_T commit, const LARGE_INTEGER *off,
                               SIZE_T *view, SECTION_INHERIT inh, ULONG at, ULONG prot)
{
    NTSTATUS st = ((mvs_fn)g_real_mvs)(sec, proc, base, zb, commit, off, view, inh, at, prot);
    MEMORY_BASIC_INFORMATION mbi;
    mapped_name nb;
    if (st >= 0 && g_on && proc == NtCurrentProcess() && base && *base &&
        NtQueryVirtualMemory(proc, *base, MemoryBasicInformation, &mbi, sizeof(mbi), NULL) >= 0 &&
        mbi.Type == MEM_IMAGE && mbi.AllocationBase == *base && name_of_mapping(*base, &nb) && !in_windows_nt(&nb.us)) {
        trace_w("mapped:", nb.us.Buffer, nb.us.Length / sizeof(WCHAR));
        RtlAcquireSRWLockExclusive(&g_rw_lock);
        rewrite(*base);
        RtlReleaseSRWLockExclusive(&g_rw_lock);
    }
    return st;
}

static void map_hook(void)
{
    LDR_DATA_TABLE_ENTRY *nt = find_module(L"ntdll.dll");
    if (env_set(L"VX_NO_MAPHOOK")) return;
    if (!nt || !windows_folder_nt()) { trace("Windows folder not found: DLLs are rewritten when loaded only", NULL, NULL); return; }
    stub_hook(nt->DllBase, "NtMapViewOfSection", (void *)hook_mvs, &g_real_mvs);
}
#endif

static void version_hooks(void)
{
    LDR_DATA_TABLE_ENTRY *nt = find_module(L"ntdll.dll"), *ole = find_module(L"ole32.dll");
    if (nt) stub_hook(nt->DllBase, "NtQueryValueKey", (void *)hook_qvk, &g_real_qvk);
    if (ole) ole32_loaded(ole->DllBase, &ole->FullDllName);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_VERIFIER) {
        if (reserved) *(vrf_provider **)reserved = &g_prov;
        g_trace = env_set(L"VX_TRACE");
        /* Safe Mode: Windows as it is, for every program */
        g_on = !VX_KUSER_SAFE_BOOT && registered();
        trace(g_on ? "on for this program" : "not registered for this program (or Safe Mode)", NULL, NULL);
        /* the verifier was switched on for VistaExt (by file name): keep it
         * out of the way also in programs of the same name that VistaExt
         * leaves alone */
        quiet_verifier();
        if (g_on) {
            ULONG a, b;
            RtlGetNtVersionNumbers(&a, &b, &g_real_build);
            g_real_build &= 0xFFFF;
            if (!env_set(L"VX_NO_REGHOOK")) { g_hooks_on = TRUE; version_hooks(); }
#if VX_OS >= 602 || VX_OS == 0
            map_hook();
#endif
#if VX_OS == 601
            extend_apiset_map();
#endif
            rewrite((BYTE *)NtCurrentTeb()->Peb->ImageBaseAddress);
            LdrRegisterDllNotification(0, dll_notification, NULL, &g_cookie);
        }
    }
    return TRUE;
}

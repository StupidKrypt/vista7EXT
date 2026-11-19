/* VistaExt - shared definitions.
 * Every exported API implementation is named vx_<Name>; tools/gen.py maps
 * vx_<Name> to the public export <Name> when it writes the .def files. */
#ifndef VX_H
#define VX_H

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>

#define VX_EXPORT /* marker only, export list comes from the .def */

/* Registry layout (always the 64-bit view, see VX_REG_VIEW). */
#define VX_REG_ROOT      L"SOFTWARE\\VistaExt"
#define VX_REG_LAYERS    L"SOFTWARE\\VistaExt\\Layers"
#define VX_REG_WEBVIEW2  L"SOFTWARE\\VistaExt\\WebView2"
#define VX_REG_VIEW      KEY_WOW64_64KEY

/* Windows versions a program can be told it is running on. */
typedef struct {
    const WCHAR *token;    /* layer token stored in the registry, e.g. L"WIN10" */
    const WCHAR *label;    /* text shown in VistaExt Center */
    DWORD major, minor, build;
    WORD  spmajor;         /* service pack */
    const WCHAR *csd;      /* e.g. L"Service Pack 1" */
} vx_osver;

extern const vx_osver vx_osvers[];
extern const int vx_osver_count;
/* "REAL": stored for a program that must see the real version even when a
 * global version is set (vx_make_layer writes it, vx_parse_layer sets .real) */
extern const vx_osver vx_osver_real;

/* Settings for the current process, read once at load. */
typedef struct {
    BOOL  off;             /* "OFF": the user undid VistaExt here; automatic mode leaves it alone */
    BOOL  real;            /* "REAL": this program is told the real version, whatever the global setting */
    const vx_osver *ver;   /* NULL = report the real version */
    BOOL  webview2;        /* point WebView2 at the VistaExt runtime */
    WCHAR args[512];       /* extra command line arguments to append */
    vx_osver custom;       /* "VER:10.0.26100[.sp]": a version given by number (ver points here) */
    WCHAR customtok[40], customcsd[24];
} vx_layer;

/* Parse a layer string such as L"WIN10 WEBVIEW2 ARGS:--no-sandbox". */
void vx_parse_layer(const WCHAR *s, vx_layer *out);
/* Look up the layer for an exe path: exact path first, then each parent
 * folder registered with a trailing backslash. Returns FALSE if none. */
BOOL vx_lookup_layer(const WCHAR *exepath, WCHAR *buf, DWORD cch);
/* The Windows version every program VistaExt runs is told, unless it has
 * its own setting: HKLM\SOFTWARE\VistaExt "Version" (a token such as
 * L"WIN10", L"VER:10.0.26100", or L"REAL"). FALSE if none is set. */
#define VX_REG_GLOBALVER L"Version"
BOOL vx_global_version(WCHAR *buf, DWORD cch);
/* The program's own setting, with the global version filled in when it has
 * none (and is not "REAL"). FALSE if neither applies. */
BOOL vx_effective_layer(const WCHAR *exepath, vx_layer *out);
/* copy a parsed layer (ver may point into the source's own custom field) */
void vx_copy_layer(vx_layer *to, const vx_layer *from);

/* Tools: store a layer for an exe path or a folder path ending in '\'.
 * An empty layer deletes the setting. Returns a Win32 error code. */
LONG vx_set_layer(const WCHAR *path, const WCHAR *layer);
void vx_make_layer(WCHAR *out, SIZE_T cch, const vx_osver *ver, BOOL webview2, const WCHAR *args);
const vx_osver *vx_find_osver(const WCHAR *token);
/* "VER:a.b.c[.sp]" -> custom version; FALSE if malformed or out of range
 * (major 5-99, minor 0-99, build 0-65535, service pack 0-9) */
BOOL vx_parse_custom(const WCHAR *s, vx_osver *v, WCHAR *tok, SIZE_T ctok, WCHAR *csd, SIZE_T ccsd);
/* Windows 10/11 release name for a build ("22H2"), or NULL */
const WCHAR *vx_release_name(DWORD build, const WCHAR **releaseid);

/* Crash window: kernelvx shares this with vxwatch.exe, which waits for the
 * program and, if it ends with a crash code, explains it and offers another
 * Windows version setting. */
#define VX_WATCH_MAGIC 0x31575856u                 /* "VXW1" */
typedef struct {
    DWORD magic;
    volatile LONG code;    /* last serious exception seen */
    WCHAR where[128];      /* module+offset of it */
    WCHAR message[400];    /* the program's own last words, if found (Chromium's FATAL lines) */
    WCHAR exe[MAX_PATH];
    WCHAR cwd[MAX_PATH];
    WCHAR cmdline[4096];
} vx_watch;
/* HKCU: programs whose crash window the user turned off (value = exe path) */
#define VX_REG_NOCRASHWND L"SOFTWARE\\VistaExt\\NoCrashWindow"

/* Which Windows this build is for. build.sh builds every component once per
 * Windows: -DVX_OS=600 (Vista SP2) or -DVX_OS=601 (Windows 7 SP1), each for
 * x86 and x64. Test tools that belong to neither leave it at 0. */
#ifndef VX_OS
#define VX_OS 0
#endif
#define VX_VISTA (VX_OS == 600)
#define VX_WIN7  (VX_OS == 601)

/* Every function a companion implements is one its Windows lacks: the
 * Windows 7 build leaves out what Windows 7 already has (its .def forwards
 * those names to Windows instead, tools/gen.py), so nothing has to ask at
 * run time whether Windows has the function. VX_NATIVE only reports the call
 * (VX_TRACE). VX_NATIVE_UPDATE is for functions a Windows *update* can add
 * (KB2533623: AddDllDirectory & co. on Vista and 7): those still use
 * Windows' own one when it is there. Built with VX_OS 0 (tools), both look. */
FARPROC vx_native(const WCHAR *module, const char *name);
/* The pointer is stored before the "looked up" flag (both volatile, so the
 * compiler keeps that order; x86/x64 keep it in hardware), so no thread can
 * see the flag without the pointer and mix the two implementations. */
#define VX_NATIVE_UPDATE(mod, name, rettype, params, args) \
    do { static rettype (WINAPI * volatile p_) params; static volatile LONG st_; \
         if (!st_) { p_ = (rettype (WINAPI *) params)(void *)vx_native(mod, #name); st_ = 1; } \
         if (p_) return p_ args; VX_TRACE(#name); } while (0)
#define VX_NATIVE_UPDATE_VOID(mod, name, params, args) \
    do { static void (WINAPI * volatile p_) params; static volatile LONG st_; \
         if (!st_) { p_ = (void (WINAPI *) params)(void *)vx_native(mod, #name); st_ = 1; } \
         if (p_) { p_ args; return; } VX_TRACE(#name); } while (0)
#if VX_OS
#define VX_NATIVE(mod, name, rettype, params, args) VX_TRACE(#name)
#define VX_NATIVE_VOID(mod, name, params, args) VX_TRACE(#name)
#else
#define VX_NATIVE VX_NATIVE_UPDATE
#define VX_NATIVE_VOID VX_NATIVE_UPDATE_VOID
#endif

/* Diagnosing programs: with the environment variable VX_TRACE=1, the first
 * calls of each VistaExt replacement are reported with OutputDebugString
 * (seen by a debugger, DebugView or tests/vm/vxdbg). */
extern int vx_tracing;
void vx_trace(const char *what);
#define VX_TRACE(what) \
    do { static volatile LONG tc_; \
         if (vx_tracing && InterlockedIncrement(&tc_) <= 3) vx_trace(what); } while (0)
#define K32 L"kernel32.dll"
#define U32 L"user32.dll"

/* Keep this DLL loaded until the process ends, as Windows' own DLLs are in
 * practice: programs take a function's address from shcore/combase and free
 * the DLL, which leaves a dangling pointer if it was the last reference (on
 * Vista/7 these DLLs are VistaExt's, and nothing else holds them). */
void vx_pin(void *addr_in_dll);

/* Real Windows version, unaffected by the version a program is told. */
void vx_real_version(DWORD *major, DWORD *minor);

/* tiny CRT replacements (no CRT is linked) */
void  *vx_memset(void *d, int c, SIZE_T n);
void  *vx_memcpy(void *d, const void *s, SIZE_T n);
int    vx_wcsicmp(const WCHAR *a, const WCHAR *b);
int    vx_wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
SIZE_T vx_wcslen(const WCHAR *s);
SIZE_T vx_strlen(const char *s);
void   vx_wcscpyn(WCHAR *d, const WCHAR *s, SIZE_T cch);
void   vx_wcscatn(WCHAR *d, const WCHAR *s, SIZE_T cch);
int    vx_stricmp(const char *a, const char *b);

#endif

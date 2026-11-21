/* ntvx.dll - ntdll companion for Windows Vista and 7. Everything ntdll has is
 * forwarded (generated .def); added here: functions from Windows 7 that Vista
 * lacks, and the Windows 8 "growable function tables" that JIT compilers
 * (Firefox's SpiderMonkey, V8, .NET) use to register unwind data for code
 * they generate. */
#include "../common/vx.h"
#include <winternl.h>

#define NTDLL L"ntdll.dll"

/* Windows 7: NtOpenKey with open options (backup/restore and the like); the
 * options only widen access checks, so the plain open is the safe fallback */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT NTSTATUS NTAPI vx_NtOpenKeyEx(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG options)
{ VX_NATIVE(NTDLL, NtOpenKeyEx, NTSTATUS, (PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG options), (key, access, attr, options));
    return NtOpenKey(key, access, attr);
}
VX_EXPORT NTSTATUS NTAPI vx_ZwOpenKeyEx(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG options)
{ VX_NATIVE(NTDLL, ZwOpenKeyEx, NTSTATUS, (PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG options), (key, access, attr, options));
    return NtOpenKey(key, access, attr);
}
#endif

/* Windows 7 */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOLEAN NTAPI vx_RtlQueryPerformanceCounter(LARGE_INTEGER *c)
{ VX_NATIVE(NTDLL, RtlQueryPerformanceCounter, BOOLEAN, (LARGE_INTEGER *c), (c));
    return (BOOLEAN)QueryPerformanceCounter(c);
}
VX_EXPORT BOOLEAN NTAPI vx_RtlQueryPerformanceFrequency(LARGE_INTEGER *f)
{ VX_NATIVE(NTDLL, RtlQueryPerformanceFrequency, BOOLEAN, (LARGE_INTEGER *f), (f));
    return (BOOLEAN)QueryPerformanceFrequency(f);
}
#endif

/* Windows 10: what kind of device this is. Always a desktop PC here, and the
 * "UAP" version is the Windows version the program is told (the PEB, which
 * VistaExt's version setting fills in; otherwise the real one). Chromium-based
 * programs (Edge) stop at start-up when this is missing. */
VX_EXPORT void NTAPI vx_RtlGetDeviceFamilyInfoEnum(ULONGLONG *uap, DWORD *family, DWORD *form)
{
    VX_NATIVE_VOID(NTDLL, RtlGetDeviceFamilyInfoEnum, (ULONGLONG *uap, DWORD *family, DWORD *form), (uap, family, form));
    if (uap) {
        ULONG maj, min, bld;
        vx_peb_version(&maj, &min, &bld);
        *uap = ((ULONGLONG)maj & 0xFFFF) << 48 | ((ULONGLONG)min & 0xFFFF) << 32 | (ULONGLONG)bld << 16;
    }
    if (family) *family = 3;    /* DEVICEFAMILYINFOENUM_DESKTOP */
    if (form) *form = 0;        /* DEVICEFAMILYDEVICEFORM_UNKNOWN */
}

/* RtlGetNtVersionNumbers: Windows' own one returns numbers built into ntdll,
 * not the PEB, so a program told another version would see two different
 * versions. When VistaExt changed the PEB (a version setting), this answers
 * from the PEB like GetVersionEx and RtlGetVersion. */
VX_EXPORT void NTAPI vx_RtlGetNtVersionNumbers(DWORD *major, DWORD *minor, DWORD *build)
{
    typedef void (NTAPI *fn)(DWORD *, DWORD *, DWORD *);
    static fn real;
    ULONG pmaj, pmin, pbld;
    DWORD rmaj = 0, rmin = 0, rbld = 0;
    vx_peb_version(&pmaj, &pmin, &pbld);
    if (!real) real = (fn)(void *)GetProcAddress(GetModuleHandleW(NTDLL), "RtlGetNtVersionNumbers");
    if (real) real(&rmaj, &rmin, &rbld);
    if (real && pmaj == rmaj && pmin == rmin && pbld == (rbld & 0xFFFF)) {
        if (major) *major = rmaj;
        if (minor) *minor = rmin;
        if (build) *build = rbld;
        return;
    }
    if (major) *major = pmaj;
    if (minor) *minor = pmin;
    if (build) *build = pbld | 0xF0000000;      /* the "free build" flag, as Windows sets it */
}

#ifdef _WIN64
/* ---- growable function tables (Windows 8, x64) ---------------------------
 * The table is registered with a lookup callback (Vista+), which searches the
 * caller's array; growing it only changes the number of entries in use. */
typedef struct {
    PRUNTIME_FUNCTION table;
    volatile DWORD count;
    DWORD max;
    DWORD64 base;
} vx_gft;

static PRUNTIME_FUNCTION CALLBACK gft_lookup(DWORD64 pc, PVOID ctx)
{
    vx_gft *g = ctx;
    DWORD rva = (DWORD)(pc - g->base), lo = 0, hi = g->count;
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        PRUNTIME_FUNCTION f = &g->table[mid];
        if (rva < f->BeginAddress) hi = mid;
        else if (rva >= f->EndAddress) lo = mid + 1;
        else return f;
    }
    return NULL;
}

VX_EXPORT DWORD NTAPI vx_RtlAddGrowableFunctionTable(PVOID *dyn, PRUNTIME_FUNCTION table, DWORD count, DWORD max,
                                                     ULONG_PTR base, ULONG_PTR end)
{ VX_NATIVE(NTDLL, RtlAddGrowableFunctionTable, DWORD, (PVOID *dyn, PRUNTIME_FUNCTION table, DWORD count, DWORD max, ULONG_PTR base, ULONG_PTR end), (dyn, table, count, max, base, end));
    vx_gft *g;
    if (!dyn || !table || count > max || end <= base) return 0xC000000D;   /* STATUS_INVALID_PARAMETER */
    if (!(g = HeapAlloc(GetProcessHeap(), 0, sizeof(*g)))) return 0xC0000017;   /* STATUS_NO_MEMORY */
    g->table = table; g->count = count; g->max = max; g->base = base;
    if (!RtlInstallFunctionTableCallback((DWORD64)g | 3, base, (DWORD)(end - base), gft_lookup, g, NULL)) {
        HeapFree(GetProcessHeap(), 0, g);
        return 0xC0000001;                                              /* STATUS_UNSUCCESSFUL */
    }
    *dyn = g;
    return 0;
}

VX_EXPORT void NTAPI vx_RtlGrowFunctionTable(PVOID dyn, DWORD count)
{ VX_NATIVE_VOID(NTDLL, RtlGrowFunctionTable, (PVOID dyn, DWORD count), (dyn, count));
    vx_gft *g = dyn;
    if (g && count <= g->max) g->count = count;
}

VX_EXPORT void NTAPI vx_RtlDeleteGrowableFunctionTable(PVOID dyn)
{ VX_NATIVE_VOID(NTDLL, RtlDeleteGrowableFunctionTable, (PVOID dyn), (dyn));
    if (!dyn) return;
    RtlDeleteFunctionTable((PRUNTIME_FUNCTION)((DWORD64)dyn | 3));
    HeapFree(GetProcessHeap(), 0, dyn);
}
#endif

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

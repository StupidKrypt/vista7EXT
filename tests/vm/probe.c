/* probe [/crash]
 * What a program VistaExt does not run finds on this Windows: for each DLL
 * name Windows may lack, whether LoadLibrary loads it, and whether what it
 * loads is VistaExt's (its "VistaExt-owned-file" marker). The output must be
 * the same before and after installing VistaExt (run.cmd compares them).
 * Exit code: how many of them are VistaExt's (0 for a program it does not
 * run). It uses only what Vista has itself, so it starts on every Windows.
 *   /crash   crashes on purpose (a native program's crash, for the
 *            diagnostics test: Windows reports it to its event log)
 *   /ldr <dll>    loads <dll> with ntdll's LdrLoadDll - as Windows' own
 *                 DLLs end up doing, past VistaExt's kernelvx: exit 0 if
 *                 it loads, 1 if not
 *   /child <dll>  the same in vxpchild.exe (an unregistered copy next to
 *                 this one), started with CreateProcess: its exit code,
 *                 plus 10 if this process loads <dll> itself */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

static const WCHAR *g_names[] = {
    L"combase.dll", L"shcore.dll", L"d3d12.dll", L"kernelbase.dll", L"bcryptprimitives.dll",
    L"api-ms-win-core-synch-l1-2-0.dll", L"api-ms-win-core-winrt-l1-1-0.dll",
    L"api-ms-win-core-processthreads-l1-1-3.dll", L"api-ms-win-core-path-l1-1-0.dll",
    L"ext-ms-win-ntuser-window-l1-1-0.dll",
};

static BOOL vistaext_owned(HMODULE h)
{
    HRSRC r = FindResourceW(h, MAKEINTRESOURCEW(100), (LPCWSTR)RT_RCDATA);
    const char *d;
    DWORD n, i;
    if (!r || !(d = LockResource(LoadResource(h, r)))) return FALSE;
    n = SizeofResource(h, r);
    for (i = 0; i + 19 <= n; i++) if (!memcmp(d + i, "VistaExt-owned-file", 19)) return TRUE;
    return FALSE;
}

/* ntdll's LdrLoadDll (no import: looked up, like the loader's own callers) */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } probe_ustr;
typedef LONG (NTAPI *ldrload_fn)(PWSTR path, PULONG flags, probe_ustr *name, PVOID *h);

static int ldr_loads(const WCHAR *dll)
{
    ldrload_fn ld = (ldrload_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll");
    probe_ustr us;
    PVOID h = NULL;
    if (!ld) return 0;
    us.Buffer = (PWSTR)dll;
    us.Length = (USHORT)(vx_wcslen(dll) * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    return ld(NULL, NULL, &us, &h) >= 0 && h;
}

int wmain(int argc, WCHAR **argv)
{
    unsigned i;
    int ours = 0;
    if (argc > 2 && !vx_wcsicmp(argv[1], L"/ldr")) {
        int ok = ldr_loads(argv[2]);
        con_print(L"%s: %s\r\n", argv[2], ok ? L"loads" : L"does not load");
        return ok ? 0 : 1;
    }
    if (argc > 2 && !vx_wcsicmp(argv[1], L"/child")) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 120];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD code = 99, n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        while (n && exe[n - 1] != '\\') n--;
        exe[n] = 0;
        vx_wcscatn(exe, L"vxpchild.exe", MAX_PATH);
        wsprintfW(cmd, L"\"%s\" /ldr %s", exe, argv[2]);
        vx_memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        n = ldr_loads(argv[2]);
        con_print(L"this process: %s; child: exit %u\r\n", n ? L"loads it" : L"does not", code);
        return (int)code + (n ? 10 : 0);
    }
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/crash")) {
        volatile int *p = NULL;
        *p = 1;
        return 9;
    }
    for (i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        HMODULE h = LoadLibraryW(g_names[i]);
        if (!h) { con_print(L"%s: not found\r\n", g_names[i]); continue; }
        if (vistaext_owned(h)) { con_print(L"%s: VistaExt's\r\n", g_names[i]); ours++; }
        else con_print(L"%s: Windows'\r\n", g_names[i]);
    }
    return ours;
}

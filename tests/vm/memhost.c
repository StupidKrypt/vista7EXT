/* memhost - loads memdll.dll at start and memdll2.dll (a copy) with
 * LoadLibrary later; exit 0 when both work, 2/3 when the later one fails. */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

int __cdecl memdll_check(void);

int wmain(int argc, WCHAR **argv)
{
    WCHAR p[MAX_PATH];
    SIZE_T n;
    HMODULE h;
    int (__cdecl *f)(void);
    int a = memdll_check();
    con_print(L"memhost: DLL loaded at start: %s\r\n", a ? L"works" : L"FAILS");
    GetModuleFileNameW(NULL, p, MAX_PATH);
    n = vx_wcslen(p);
    while (n && p[n - 1] != '\\') n--;
    vx_wcscpyn(p + n, L"memdll2.dll", MAX_PATH - n);
    if (!(h = LoadLibraryW(p))) { con_print(L"memhost: memdll2.dll does not load (error %u)\r\n", GetLastError()); return 2; }
    f = (int (__cdecl *)(void))GetProcAddress(h, "memdll_check");
    if (!f || !f()) { con_print(L"memhost: DLL loaded later FAILS\r\n"); return 3; }
    con_print(L"memhost: DLL loaded later: works\r\n");
    return a ? 0 : 1;
}

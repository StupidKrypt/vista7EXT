/* benchdll - an ordinary program DLL (imports only kernel32 functions Vista
 * has), loaded and freed in a loop by vxbench. */
#include <windows.h>

__declspec(dllexport) DWORD __cdecl benchdll_ticks(void) { return GetTickCount(); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}

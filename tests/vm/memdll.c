/* memdll.dll - a DLL (not a program) that needs functions Vista/7 lack,
 * imported straight from kernel32. memhost loads it at start (static import)
 * and a copy of it later (LoadLibrary): in VistaExt's in-memory mode both
 * must have their imports redirected while they load. */
#include "../../src/common/vx.h"

__declspec(dllexport) int __cdecl memdll_check(void)
{
    FILETIME ft;
    volatile LONG v = 0;
    LONG zero = 0;
    GetSystemTimePreciseAsFileTime(&ft);
    WakeByAddressAll((PVOID)&v);
    return ft.dwHighDateTime > 0x01D00000 && !WaitOnAddress(&v, &zero, sizeof(zero), 10) && GetLastError() == ERROR_TIMEOUT;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID p) { return TRUE; }

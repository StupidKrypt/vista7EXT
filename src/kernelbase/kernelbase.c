/* kernelbase.dll for Windows Vista: Vista has no kernelbase, so this one is
 * pure forwarders to kernelvx.dll (generated .def). */
#include "../common/vx.h"

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

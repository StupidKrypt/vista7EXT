/* ws2vx.dll - ws2_32 companion for Windows Vista and 7. Added: the Windows 8
 * calls for asynchronous name lookups. Vista/7's GetAddrInfoExW never starts
 * an asynchronous lookup (it rejects an OVERLAPPED), so there is never one to
 * cancel or collect; Chromium-based programs import these at start-up. */
#include "../common/vx.h"
#include <winsock2.h>

#define WS2 L"ws2_32.dll"

VX_EXPORT INT WINAPI vx_GetAddrInfoExCancel(LPHANDLE h)
{ VX_NATIVE(WS2, GetAddrInfoExCancel, INT, (LPHANDLE h), (h));
    return WSA_INVALID_HANDLE;
}

VX_EXPORT INT WINAPI vx_GetAddrInfoExOverlappedResult(LPOVERLAPPED ov)
{ VX_NATIVE(WS2, GetAddrInfoExOverlappedResult, INT, (LPOVERLAPPED ov), (ov));
    return WSA_INVALID_PARAMETER;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}

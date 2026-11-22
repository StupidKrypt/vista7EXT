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

/* GetHostNameW (Windows 8): the host name in UTF-16, the same name
 * gethostname gives (Microsoft: "returns zero" on success, else SOCKET_ERROR
 * with WSAEFAULT for no buffer or one too small - namelen counts WCHARs -,
 * WSANOTINITIALISED before WSAStartup, WSAENETDOWN). gethostname (Vista)
 * checks all of that; its answer is in the ANSI code page, which cannot hold
 * every name, so the Unicode DNS host name is given instead when it is the
 * same name. */
#if VX_OS < 602   /* Windows 8 and later have it: their .def forwards there */
VX_EXPORT INT WINAPI vx_GetHostNameW(PWSTR name, INT namelen)
{ VX_NATIVE(WS2, GetHostNameW, INT, (PWSTR name, INT namelen), (name, namelen));
    char a[257];
    WCHAR w[257], dns[257];
    DWORD dn = 257;
    int n;
    if (gethostname(a, sizeof(a)) == SOCKET_ERROR) return SOCKET_ERROR;   /* its own error: WSANOTINITIALISED... */
    a[256] = 0;
    if (!(n = MultiByteToWideChar(CP_ACP, 0, a, -1, w, 257))) { WSASetLastError(WSAEFAULT); return SOCKET_ERROR; }
    if (GetComputerNameExW(ComputerNameDnsHostname, dns, &dn) && dn && dn < 257) {
        char back[257];
        BOOL lossy = FALSE;
        if (WideCharToMultiByte(CP_ACP, 0, dns, -1, back, sizeof(back), NULL, &lossy) && !lstrcmpiA(back, a)) {
            vx_wcscpyn(w, dns, 257);                 /* the same name, without the code page's losses */
            n = (int)vx_wcslen(w) + 1;
        }
    }
    if (!name || namelen < n) { WSASetLastError(WSAEFAULT); return SOCKET_ERROR; }
    vx_memcpy(name, w, n * sizeof(WCHAR));
    return 0;
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

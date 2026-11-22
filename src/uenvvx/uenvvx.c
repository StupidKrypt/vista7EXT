/* uenvvx.dll - userenv companion for Windows Vista and 7. Added: the
 * AppContainer profile functions of Windows 8. AppContainers (the sandbox
 * of Store apps and of some browsers' sandboxed processes) do not exist
 * before Windows 8, so each fails cleanly with
 * HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); no SID or path is ever handed
 * out. Microsoft Learn (userenv.h): E_INVALIDARG for a missing argument;
 * the SID of a successful call would be freed with FreeSid. */
#include "../common/vx.h"

#define UENV L"userenv.dll"
#define NOT_HERE HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)

#if VX_OS < 602   /* Windows 8 and later have them: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_DeriveAppContainerSidFromAppContainerName(PCWSTR name, PSID *sid)
{ VX_NATIVE(UENV, DeriveAppContainerSidFromAppContainerName, HRESULT, (PCWSTR name, PSID *sid), (name, sid));
    if (sid) *sid = NULL;
    if (!name || !sid) return E_INVALIDARG;
    return NOT_HERE;
}

VX_EXPORT HRESULT WINAPI vx_CreateAppContainerProfile(PCWSTR name, PCWSTR disp, PCWSTR desc, PVOID caps, DWORD ncaps, PSID *sid)
{ VX_NATIVE(UENV, CreateAppContainerProfile, HRESULT, (PCWSTR name, PCWSTR disp, PCWSTR desc, PVOID caps, DWORD ncaps, PSID *sid),
            (name, disp, desc, caps, ncaps, sid));
    if (sid) *sid = NULL;
    if (!name || !disp || !desc || !sid) return E_INVALIDARG;
    return NOT_HERE;
}

VX_EXPORT HRESULT WINAPI vx_DeleteAppContainerProfile(PCWSTR name)
{ VX_NATIVE(UENV, DeleteAppContainerProfile, HRESULT, (PCWSTR name), (name));
    return name ? NOT_HERE : E_INVALIDARG;
}

VX_EXPORT HRESULT WINAPI vx_GetAppContainerFolderPath(PCWSTR sid, PWSTR *path)
{ VX_NATIVE(UENV, GetAppContainerFolderPath, HRESULT, (PCWSTR sid, PWSTR *path), (sid, path));
    if (path) *path = NULL;
    return sid && path ? NOT_HERE : E_INVALIDARG;
}

VX_EXPORT HRESULT WINAPI vx_GetAppContainerRegistryLocation(REGSAM access, HKEY *key)
{ VX_NATIVE(UENV, GetAppContainerRegistryLocation, HRESULT, (REGSAM access, HKEY *key), (access, key));
    if (key) *key = NULL;
    return key ? NOT_HERE : E_INVALIDARG;
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

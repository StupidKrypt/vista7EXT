/* cfgmgrvx.dll - cfgmgr32 companion for Windows Vista and 7.
 * CM_Register_Notification (Windows 8) succeeds but never calls back: device
 * arrival/removal notices are simply not delivered (programs also get them
 * through WM_DEVICECHANGE, which works as before). */
#include "../common/vx.h"

typedef DWORD CONFIGRET;
#define CFG L"cfgmgr32.dll"
#define CR_SUCCESS               0x00
#define CR_OUT_OF_MEMORY         0x02
#define CR_INVALID_POINTER       0x03
#define CR_INVALID_FLAG          0x04
#define CR_INVALID_DEVNODE       0x05
#define CR_INVALID_DATA          0x1F
#define CR_BUFFER_SMALL          0x1A
#define CR_NO_SUCH_DEVNODE       0x0D
#define CR_NO_SUCH_VALUE         0x25
#define CR_ACCESS_DENIED         0x33
#define CR_CALL_NOT_IMPLEMENTED  0x34
#define CR_INVALID_PROPERTY      0x35

static LONG g_dummy;

VX_EXPORT CONFIGRET WINAPI vx_CM_Register_Notification(PVOID filter, PVOID ctx, PVOID cb, HANDLE *h)
{ VX_NATIVE(CFG, CM_Register_Notification, CONFIGRET, (PVOID filter, PVOID ctx, PVOID cb, HANDLE *h), (filter, ctx, cb, h));
    if (!filter || !cb || !h) return CR_INVALID_POINTER;
    *h = (HANDLE)&g_dummy;
    return CR_SUCCESS;
}

VX_EXPORT CONFIGRET WINAPI vx_CM_Unregister_Notification(HANDLE h)
{ VX_NATIVE(CFG, CM_Unregister_Notification, CONFIGRET, (HANDLE h), (h));
    return CR_SUCCESS;
}

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT DWORD WINAPI vx_CM_MapCrToWin32Err(CONFIGRET cr, DWORD dflt)
{ VX_NATIVE(CFG, CM_MapCrToWin32Err, DWORD, (CONFIGRET cr, DWORD dflt), (cr, dflt));
    switch (cr) {
    case CR_SUCCESS: return ERROR_SUCCESS;
    case CR_OUT_OF_MEMORY: return ERROR_NOT_ENOUGH_MEMORY;
    case CR_INVALID_POINTER: return ERROR_INVALID_USER_BUFFER;
    case CR_INVALID_FLAG: return ERROR_INVALID_FLAGS;
    case CR_INVALID_DEVNODE: case CR_INVALID_DATA: case CR_INVALID_PROPERTY: return ERROR_INVALID_DATA;
    case CR_BUFFER_SMALL: return ERROR_INSUFFICIENT_BUFFER;
    case CR_NO_SUCH_DEVNODE: case CR_NO_SUCH_VALUE: return ERROR_NOT_FOUND;
    case CR_ACCESS_DENIED: return ERROR_ACCESS_DENIED;
    case CR_CALL_NOT_IMPLEMENTED: return ERROR_CALL_NOT_IMPLEMENTED;
    }
    return dflt;
}
#endif

/* device properties by key (Windows 7): not available on Vista */
#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT CONFIGRET WINAPI vx_CM_Get_DevNode_PropertyW(DWORD dev, const void *key, ULONG *type, BYTE *buf, ULONG *size, ULONG flags)
{ VX_NATIVE(CFG, CM_Get_DevNode_PropertyW, CONFIGRET, (DWORD dev, const void *key, ULONG *type, BYTE *buf, ULONG *size, ULONG flags), (dev, key, type, buf, size, flags));
    return CR_CALL_NOT_IMPLEMENTED;
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

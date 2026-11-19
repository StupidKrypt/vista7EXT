/* Power and app-state notifications from Windows 8 and 10 (powrprof, the
 * power-setting and psm-appnotify API sets). Chromium-based programs (Edge)
 * load them on demand and stop when they are missing.
 *
 * Vista and 7 have no callback form of these notifications. Registering
 * succeeds and nothing is ever reported, which is what a program sees on
 * Windows 10 while the setting doesn't change. The window-message form of
 * power-setting notifications exists on Vista, and is passed on to it. */
#include "../common/vx.h"

/* a registration that never fires: its handle is the address of this */
static BYTE g_quiet;
#define QUIET ((HANDLE)&g_quiet)

#define DEVICE_NOTIFY_WINDOW_HANDLE_ 0
#define POWRPROF L"powrprof.dll"

/* user32's own registration (Vista+), found only if the program has user32
 * loaded: a window handle means it has */
typedef HANDLE (WINAPI *reg_fn)(HANDLE, LPCGUID, DWORD);
typedef BOOL (WINAPI *unreg_fn)(HANDLE);
static FARPROC user32_fn(const char *name)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    return u ? GetProcAddress(u, name) : NULL;
}

/* Windows 8 (powrprof) */
VX_EXPORT DWORD WINAPI vx_PowerSettingRegisterNotification(LPCGUID setting, DWORD flags, HANDLE recipient, PVOID *handle)
{
    VX_NATIVE(POWRPROF, PowerSettingRegisterNotification, DWORD, (LPCGUID setting, DWORD flags, HANDLE recipient, PVOID *handle),
              (setting, flags, recipient, handle));
    if (!setting || !recipient || !handle) return ERROR_INVALID_PARAMETER;
    if (flags == DEVICE_NOTIFY_WINDOW_HANDLE_) {
        reg_fn reg = (reg_fn)(void *)user32_fn("RegisterPowerSettingNotification");
        HANDLE h = reg ? reg(recipient, setting, DEVICE_NOTIFY_WINDOW_HANDLE_) : NULL;
        if (!h) return reg ? GetLastError() : ERROR_INVALID_PARAMETER;
        *handle = h;
        return ERROR_SUCCESS;
    }
    *handle = QUIET;            /* callback form: registered, never called */
    return ERROR_SUCCESS;
}

VX_EXPORT DWORD WINAPI vx_PowerSettingUnregisterNotification(PVOID handle)
{
    VX_NATIVE(POWRPROF, PowerSettingUnregisterNotification, DWORD, (PVOID handle), (handle));
    if (!handle) return ERROR_INVALID_PARAMETER;
    if (handle != QUIET) {
        unreg_fn unreg = (unreg_fn)(void *)user32_fn("UnregisterPowerSettingNotification");
        if (!unreg || !unreg(handle)) return unreg ? GetLastError() : ERROR_INVALID_HANDLE;
    }
    return ERROR_SUCCESS;
}

/* Windows 10 1809: "effective power mode" (battery saver, best performance...) */
VX_EXPORT HRESULT WINAPI vx_PowerRegisterForEffectivePowerModeNotifications(ULONG version, PVOID callback, PVOID context, PVOID *handle)
{
    VX_NATIVE(POWRPROF, PowerRegisterForEffectivePowerModeNotifications, HRESULT,
              (ULONG version, PVOID callback, PVOID context, PVOID *handle), (version, callback, context, handle));
    if (!callback || !handle) return E_INVALIDARG;
    *handle = QUIET;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PowerUnregisterFromEffectivePowerModeNotifications(PVOID handle)
{
    VX_NATIVE(POWRPROF, PowerUnregisterFromEffectivePowerModeNotifications, HRESULT, (PVOID handle), (handle));
    return handle == QUIET ? S_OK : E_INVALIDARG;
}

/* Windows 8: PowerDeterminePlatformRole with a version argument; Vista has
 * the plain one (version 1's answers; version 2 only adds "slate") */
VX_EXPORT DWORD WINAPI vx_PowerDeterminePlatformRoleEx(ULONG version)
{
    static DWORD (WINAPI * volatile role)(void);
    static volatile LONG looked;
    VX_NATIVE(POWRPROF, PowerDeterminePlatformRoleEx, DWORD, (ULONG version), (version));
    if (!looked) {
        HMODULE m = LoadLibraryW(L"powrprof.dll");
        role = m ? (DWORD (WINAPI *)(void))(void *)GetProcAddress(m, "PowerDeterminePlatformRole") : NULL;
        looked = 1;
    }
    return role ? role() : 0;   /* PlatformRoleUnspecified */
}

/* Windows 8.1 (psm-appnotify): a desktop program is never suspended */
VX_EXPORT DWORD WINAPI vx_RegisterAppStateChangeNotification(PVOID callback, PVOID context, PVOID *registration)
{
    VX_NATIVE(K32, RegisterAppStateChangeNotification, DWORD, (PVOID callback, PVOID context, PVOID *registration),
              (callback, context, registration));
    if (!callback || !registration) return ERROR_INVALID_PARAMETER;
    *registration = QUIET;
    return ERROR_SUCCESS;
}

VX_EXPORT void WINAPI vx_UnregisterAppStateChangeNotification(PVOID registration)
{
    VX_NATIVE_VOID(K32, UnregisterAppStateChangeNotification, (PVOID registration), (registration));
}

/* Power and app-state notifications from Windows 8 and 10 (powrprof, the
 * power-setting and psm-appnotify API sets). Chromium-based programs (Edge)
 * load them on demand and stop when they are missing.
 *
 * Windows' own function is used wherever this Windows has one (powrprof's on
 * Windows 8, kernel.appcore's on 8.1), looked up at run time: these live in
 * other DLLs than kernel32, so the build cannot forward them.
 *
 * Vista and 7 have no callback form of power-setting and suspend/resume
 * notifications, but they have the window form (RegisterPowerSettingNotification,
 * and WM_POWERBROADCAST to every top-level window). So a callback
 * registration gets one hidden top-level window, on a thread of its own,
 * started at the first one: the window receives what Windows sends and calls
 * the registered callbacks, on that thread, as Windows calls them on one of
 * its own. Windows sends a power setting's current value right after it is
 * registered, so the first callback comes at once, as on Windows 8.
 * Unregistering runs on the window's thread too, so no callback of a
 * registration is running or still to come once that returns.
 *
 * Effective power mode (Windows 10) has no counterpart here: registering
 * succeeds and it never changes, as on a PC whose mode is never changed. */
#include "../common/vx.h"

#define DEVICE_NOTIFY_WINDOW_HANDLE_ 0
#define DEVICE_NOTIFY_CALLBACK_      2
#define POWRPROF L"powrprof.dll"
#define APPCORE  L"kernel.appcore.dll"

#define VX_PBT_APMSUSPEND          0x0004
#define VX_PBT_APMRESUMESUSPEND    0x0007
#define VX_PBT_APMRESUMEAUTOMATIC  0x0012
#define VX_PBT_POWERSETTINGCHANGE  0x8013

/* a registration that never fires: its handle is the address of this */
static BYTE g_quiet;
#define QUIET ((HANDLE)&g_quiet)

/* user32's functions (Vista+), loaded only by a program that asks */
typedef HANDLE (WINAPI *reg_fn)(HANDLE, LPCGUID, DWORD);
typedef BOOL (WINAPI *unreg_fn)(HANDLE);
#define user32_fn vx_user32

/* ---- the notification window --------------------------------------------- */
typedef ULONG (CALLBACK *notify_cb)(PVOID ctx, ULONG type, PVOID setting);
typedef struct { notify_cb cb; PVOID ctx; } VX_DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;

typedef struct vx_preg {
    struct vx_preg *next;
    struct vx_preg *dead;           /* removed while callbacks were being made: freed after */
    DWORD magic;                    /* VX_PREG_MAGIC while registered */
    BOOL suspend;                   /* suspend/resume (else a power setting) */
    GUID setting;
    notify_cb cb;
    PVOID ctx;
    HANDLE hpn;                     /* Windows' registration for the window */
} vx_preg;
#define VX_PREG_MAGIC 0x47455250u   /* "PREG" */

static LRESULT CALLBACK pwnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static vx_notifywnd g_pw = { L"VistaExtPowerNotify", pwnd_proc, NULL, TRUE };
static vx_preg *g_pregs;            /* touched only on the window's thread */
static vx_preg *g_dead;             /* removed during a delivery, freed when it ends */
static int g_delivering;
#define WM_VX_ADD    (WM_APP + 1)
#define WM_VX_REMOVE (WM_APP + 2)

static BOOL same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    int i;
    for (i = 0; i < (int)sizeof(GUID); i++) if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static LRESULT CALLBACK pwnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vx_preg *r, **pp;
    switch (msg) {
    case WM_VX_ADD: {               /* lp: the registration; returns a Win32 error */
        r = (vx_preg *)lp;
        if (!r->suspend) {
            reg_fn reg = (reg_fn)(void *)user32_fn("RegisterPowerSettingNotification");
            if (!reg || !(r->hpn = reg(h, &r->setting, DEVICE_NOTIFY_WINDOW_HANDLE_))) return reg ? GetLastError() : ERROR_NOT_SUPPORTED;
        }
        r->magic = VX_PREG_MAGIC;
        r->next = g_pregs;
        g_pregs = r;
        return ERROR_SUCCESS;
    }
    case WM_VX_REMOVE:              /* lp: the registration; returns a Win32 error */
        for (pp = &g_pregs; *pp; pp = &(*pp)->next) {
            if (*pp != (vx_preg *)lp) continue;
            r = *pp;
            *pp = r->next;
            if (r->hpn) {
                unreg_fn unreg = (unreg_fn)(void *)user32_fn("UnregisterPowerSettingNotification");
                if (unreg) unreg(r->hpn);
            }
            r->magic = 0;
            /* a callback unregistering (itself or another) during a
             * delivery: the loop may still step through r, whose "next"
             * stays valid; it is freed when the delivery ends */
            if (g_delivering) { r->dead = g_dead; g_dead = r; }
            else HeapFree(GetProcessHeap(), 0, r);
            return ERROR_SUCCESS;
        }
        return ERROR_INVALID_HANDLE;
    case WM_POWERBROADCAST:
        g_delivering++;
        if (wp == VX_PBT_POWERSETTINGCHANGE && lp) {
            const GUID *g = (const GUID *)lp;            /* POWERBROADCAST_SETTING starts with the GUID */
            for (r = g_pregs; r; r = r->next)
                if (r->magic == VX_PREG_MAGIC && !r->suspend && same_guid(&r->setting, g))
                    r->cb(r->ctx, VX_PBT_POWERSETTINGCHANGE, (PVOID)lp);
        } else if (wp == VX_PBT_APMSUSPEND || wp == VX_PBT_APMRESUMESUSPEND || wp == VX_PBT_APMRESUMEAUTOMATIC) {
            for (r = g_pregs; r; r = r->next)
                if (r->magic == VX_PREG_MAGIC && r->suspend) r->cb(r->ctx, (ULONG)wp, NULL);
        }
        if (!--g_delivering)
            while (g_dead) { r = g_dead; g_dead = r->dead; HeapFree(GetProcessHeap(), 0, r); }
        return TRUE;
    }
    return vx_defproc(h, msg, wp, lp);
}

static HWND power_window(void)
{
    if (!g_pw.module) g_pw.module = GetModuleHandleW(L"kernelvx.dll");
    return vx_notify_window(&g_pw);
}

static DWORD callback_register(BOOL suspend, LPCGUID setting, VX_DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS *p, PVOID *handle)
{
    HWND w;
    vx_preg *r;
    DWORD e;
    if (!p || !p->cb) return ERROR_INVALID_PARAMETER;
    if (!(w = power_window())) return ERROR_NOT_SUPPORTED;
    if (!(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r)))) return ERROR_NOT_ENOUGH_MEMORY;
    r->suspend = suspend;
    if (setting) r->setting = *setting;
    r->cb = p->cb;
    r->ctx = p->ctx;
    if ((e = (DWORD)vx_send(w, WM_VX_ADD, 0, (LPARAM)r))) { HeapFree(GetProcessHeap(), 0, r); return e; }
    *handle = r;
    return ERROR_SUCCESS;
}

static DWORD callback_unregister(PVOID handle)
{
    if (!g_pw.hwnd) return ERROR_INVALID_HANDLE;
    return (DWORD)vx_send(g_pw.hwnd, WM_VX_REMOVE, 0, (LPARAM)handle);
}

/* uservx's RegisterSuspendResumeNotification (callback form) comes here */
VX_EXPORT DWORD WINAPI vx_VxSuspendResumeRegister(PVOID params, PVOID *handle)
{
    if (!handle) return ERROR_INVALID_PARAMETER;
    return callback_register(TRUE, NULL, params, handle);
}

VX_EXPORT DWORD WINAPI vx_VxSuspendResumeUnregister(PVOID handle)
{
    return callback_unregister(handle);
}

/* ---- Windows 8 (powrprof) ---------------------------------------------------
 * Windows 7's powrprof has these too (undocumented there) but refuses the
 * callback form: Windows' own is asked first, and VistaExt's window only
 * takes a callback registration Windows refused. */
typedef DWORD (WINAPI *psrn_fn)(LPCGUID, DWORD, HANDLE, PVOID *);
typedef DWORD (WINAPI *psun_fn)(PVOID);

static FARPROC powrprof_fn(const char *name) { return vx_native(POWRPROF, name); }   /* (VX_NO_NATIVE: none) */

VX_EXPORT DWORD WINAPI vx_PowerSettingRegisterNotification(LPCGUID setting, DWORD flags, HANDLE recipient, PVOID *handle)
{
    psrn_fn own = (psrn_fn)(void *)powrprof_fn("PowerSettingRegisterNotification");
    DWORD e = ERROR_NOT_SUPPORTED;
    VX_TRACE("PowerSettingRegisterNotification");
    if (own && (e = own(setting, flags, recipient, handle)) == ERROR_SUCCESS) return e;
    if (!setting || !recipient || !handle) return ERROR_INVALID_PARAMETER;
    if (flags == DEVICE_NOTIFY_CALLBACK_)
        return callback_register(FALSE, setting, (VX_DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS *)recipient, handle);
    if (own) return e;                        /* Windows' answer for the window form */
    if (flags == DEVICE_NOTIFY_WINDOW_HANDLE_) {
        reg_fn reg = (reg_fn)(void *)user32_fn("RegisterPowerSettingNotification");
        HANDLE h = reg ? reg(recipient, setting, DEVICE_NOTIFY_WINDOW_HANDLE_) : NULL;
        if (!h) return reg ? GetLastError() : ERROR_INVALID_PARAMETER;
        *handle = h;
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_PARAMETER;
}

VX_EXPORT DWORD WINAPI vx_PowerSettingUnregisterNotification(PVOID handle)
{
    psun_fn own = (psun_fn)(void *)powrprof_fn("PowerSettingUnregisterNotification");
    VX_TRACE("PowerSettingUnregisterNotification");
    if (!handle) return ERROR_INVALID_PARAMETER;
    /* one of VistaExt's (its window's list knows it), else Windows' own or a window registration */
    if (g_pw.hwnd && callback_unregister(handle) == ERROR_SUCCESS) return ERROR_SUCCESS;
    if (own) return own(handle);
    {
        unreg_fn unreg = (unreg_fn)(void *)user32_fn("UnregisterPowerSettingNotification");
        if (!unreg || !unreg(handle)) return unreg ? GetLastError() : ERROR_INVALID_HANDLE;
    }
    return ERROR_SUCCESS;
}

/* Windows 10 1809: "effective power mode" (battery saver, best performance...) */
VX_EXPORT HRESULT WINAPI vx_PowerRegisterForEffectivePowerModeNotifications(ULONG version, PVOID callback, PVOID context, PVOID *handle)
{
    VX_NATIVE_UPDATE(POWRPROF, PowerRegisterForEffectivePowerModeNotifications, HRESULT,
                     (ULONG version, PVOID callback, PVOID context, PVOID *handle), (version, callback, context, handle));
    if (!callback || !handle) return E_INVALIDARG;
    *handle = QUIET;
    return S_OK;
}

VX_EXPORT HRESULT WINAPI vx_PowerUnregisterFromEffectivePowerModeNotifications(PVOID handle)
{
    VX_NATIVE_UPDATE(POWRPROF, PowerUnregisterFromEffectivePowerModeNotifications, HRESULT, (PVOID handle), (handle));
    return handle == QUIET ? S_OK : E_INVALIDARG;
}

/* Windows 8: PowerDeterminePlatformRole with a version argument; Vista has
 * the plain one (version 1's answers; version 2 only adds "slate") */
VX_EXPORT DWORD WINAPI vx_PowerDeterminePlatformRoleEx(ULONG version)
{
    static DWORD (WINAPI * volatile role)(void);
    static volatile LONG looked;
    VX_NATIVE_UPDATE(POWRPROF, PowerDeterminePlatformRoleEx, DWORD, (ULONG version), (version));
    if (!looked) {
        HMODULE m = LoadLibraryW(POWRPROF);
        role = m ? (DWORD (WINAPI *)(void))(void *)GetProcAddress(m, "PowerDeterminePlatformRole") : NULL;
        looked = 1;
    }
    return role ? role() : 0;   /* PlatformRoleUnspecified */
}

/* Windows 8.1 (psm-appnotify): a desktop program is never suspended */
VX_EXPORT DWORD WINAPI vx_RegisterAppStateChangeNotification(PVOID callback, PVOID context, PVOID *registration)
{
    VX_NATIVE_UPDATE(APPCORE, RegisterAppStateChangeNotification, DWORD, (PVOID callback, PVOID context, PVOID *registration),
                     (callback, context, registration));
    if (!callback || !registration) return ERROR_INVALID_PARAMETER;
    *registration = QUIET;
    return ERROR_SUCCESS;
}

VX_EXPORT void WINAPI vx_UnregisterAppStateChangeNotification(PVOID registration)
{
    VX_NATIVE_UPDATE_VOID(APPCORE, UnregisterAppStateChangeNotification, (PVOID registration), (registration));
}

/* uservx.dll - user32 companion for Windows Vista: DPI awareness (8.1/10),
 * touch, pointer and gesture input (7/8), display configuration (7). */
#include "../common/vx.h"

/* ---- DPI ------------------------------------------------------------------- */
/* The system DPI is fixed for a logon session on Vista/7 (changing it needs
 * a new logon), and programs ask for it on every layout pass, so it is read
 * from a screen DC once. */
static UINT system_dpi(void)
{
    static volatile LONG cached;
    LONG d = cached;
    if (!d) {
        HDC dc = GetDC(NULL);
        d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 0;
        if (dc) ReleaseDC(NULL, dc);
        if (d <= 0) return 96;
        InterlockedExchange(&cached, d);
    }
    return (UINT)d;
}

/* Pseudo handles defined by the SDK: -1 unaware, -2 system aware,
 * -3 per monitor, -4 per monitor v2, -5 unaware (GDI scaled). */
#define CTX(x) ((HANDLE)(LONG_PTR)(x))

static int awareness_of(HANDLE ctx)
{
    switch ((LONG_PTR)ctx) {
    case -1: case -5: return 0;   /* DPI_AWARENESS_UNAWARE */
    case -2: return 1;            /* SYSTEM_AWARE */
    case -3: case -4: return 2;   /* PER_MONITOR_AWARE */
    }
    return -1;                    /* INVALID */
}

static HANDLE current_ctx(void) { return IsProcessDPIAware() ? CTX(-2) : CTX(-1); }

VX_EXPORT UINT WINAPI vx_GetDpiForSystem(void) { VX_NATIVE(U32, GetDpiForSystem, UINT, (void), ()); return system_dpi(); }
VX_EXPORT UINT WINAPI vx_GetDpiForWindow(HWND h)
{ VX_NATIVE(U32, GetDpiForWindow, UINT, (HWND h), (h));
    if (!IsWindow(h)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    return system_dpi();
}
VX_EXPORT UINT WINAPI vx_GetSystemDpiForProcess(HANDLE p) { VX_NATIVE(U32, GetSystemDpiForProcess, UINT, (HANDLE p), (p)); return system_dpi(); }
VX_EXPORT BOOL WINAPI vx_SetProcessDpiAwarenessContext(HANDLE ctx)
{ VX_NATIVE(U32, SetProcessDpiAwarenessContext, BOOL, (HANDLE ctx), (ctx));
    int a = awareness_of(ctx);
    if (a < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (a > 0) SetProcessDPIAware();     /* Vista has a single "aware" mode */
    return TRUE;
}
VX_EXPORT HANDLE WINAPI vx_GetThreadDpiAwarenessContext(void) { VX_NATIVE(U32, GetThreadDpiAwarenessContext, HANDLE, (void), ()); return current_ctx(); }
VX_EXPORT HANDLE WINAPI vx_SetThreadDpiAwarenessContext(HANDLE ctx)
{ VX_NATIVE(U32, SetThreadDpiAwarenessContext, HANDLE, (HANDLE ctx), (ctx));
    if (awareness_of(ctx) < 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return current_ctx();
}
VX_EXPORT HANDLE WINAPI vx_GetWindowDpiAwarenessContext(HWND h) { VX_NATIVE(U32, GetWindowDpiAwarenessContext, HANDLE, (HWND h), (h)); return current_ctx(); }
VX_EXPORT HANDLE WINAPI vx_GetDpiAwarenessContextForProcess(HANDLE p) { VX_NATIVE(U32, GetDpiAwarenessContextForProcess, HANDLE, (HANDLE p), (p)); return current_ctx(); }
VX_EXPORT int WINAPI vx_GetAwarenessFromDpiAwarenessContext(HANDLE ctx) { VX_NATIVE(U32, GetAwarenessFromDpiAwarenessContext, int, (HANDLE ctx), (ctx)); return awareness_of(ctx); }
VX_EXPORT BOOL WINAPI vx_AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) { VX_NATIVE(U32, AreDpiAwarenessContextsEqual, BOOL, (HANDLE a, HANDLE b), (a, b)); return awareness_of(a) == awareness_of(b); }
VX_EXPORT BOOL WINAPI vx_IsValidDpiAwarenessContext(HANDLE ctx) { VX_NATIVE(U32, IsValidDpiAwarenessContext, BOOL, (HANDLE ctx), (ctx)); return awareness_of(ctx) >= 0; }
VX_EXPORT UINT WINAPI vx_GetDpiFromDpiAwarenessContext(HANDLE ctx) { VX_NATIVE(U32, GetDpiFromDpiAwarenessContext, UINT, (HANDLE ctx), (ctx)); int a = awareness_of(ctx); return a == 2 ? 0 : a == 1 ? system_dpi() : a == 0 ? 96 : 0; }
VX_EXPORT BOOL WINAPI vx_EnableNonClientDpiScaling(HWND h) { VX_NATIVE(U32, EnableNonClientDpiScaling, BOOL, (HWND h), (h)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_SetProcessDpiAwarenessInternal(int v) { VX_NATIVE(U32, SetProcessDpiAwarenessInternal, BOOL, (int v), (v)); if (v) SetProcessDPIAware(); return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetProcessDpiAwarenessInternal(HANDLE p, int *v) { VX_NATIVE(U32, GetProcessDpiAwarenessInternal, BOOL, (HANDLE p, int *v), (p, v)); *v = IsProcessDPIAware() ? 1 : 0; return TRUE; }

static BOOL is_size_metric(int i)
{
    static const BYTE m[] = { 2,3,4,5,6,7,8,9,10,11,12,13,14,15,20,21,28,29,30,31,32,33,34,35,36,37,38,39,
                              45,46,49,50,51,52,53,54,55,71,72,92 };
    unsigned k;
    for (k = 0; k < sizeof(m); k++) if (m[k] == i) return TRUE;
    return FALSE;
}

VX_EXPORT int WINAPI vx_GetSystemMetricsForDpi(int i, UINT dpi)
{ VX_NATIVE(U32, GetSystemMetricsForDpi, int, (int i, UINT dpi), (i, dpi));
    int v = GetSystemMetrics(i);
    UINT s = system_dpi();
    if (!dpi || dpi == s || !is_size_metric(i)) return v;
    return MulDiv(v, dpi, s);
}

VX_EXPORT BOOL WINAPI vx_AdjustWindowRectExForDpi(LPRECT rc, DWORD style, BOOL menu, DWORD ex, UINT dpi)
{ VX_NATIVE(U32, AdjustWindowRectExForDpi, BOOL, (LPRECT rc, DWORD style, BOOL menu, DWORD ex, UINT dpi), (rc, style, menu, ex, dpi));
    return AdjustWindowRectEx(rc, style, menu, ex);
}

VX_EXPORT BOOL WINAPI vx_SystemParametersInfoForDpi(UINT a, UINT p, PVOID pv, UINT ini, UINT dpi)
{ VX_NATIVE(U32, SystemParametersInfoForDpi, BOOL, (UINT a, UINT p, PVOID pv, UINT ini, UINT dpi), (a, p, pv, ini, dpi));
    return SystemParametersInfoW(a, p, pv, ini);
}

VX_EXPORT BOOL WINAPI vx_LogicalToPhysicalPointForPerMonitorDPI(HWND h, LPPOINT pt) { VX_NATIVE(U32, LogicalToPhysicalPointForPerMonitorDPI, BOOL, (HWND h, LPPOINT pt), (h, pt)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_PhysicalToLogicalPointForPerMonitorDPI(HWND h, LPPOINT pt) { VX_NATIVE(U32, PhysicalToLogicalPointForPerMonitorDPI, BOOL, (HWND h, LPPOINT pt), (h, pt)); return TRUE; }

/* ---- message filter (Windows 7) ------------------------------------------ */
typedef struct { DWORD cbSize, ExtStatus; } VX_CHANGEFILTERSTRUCT;
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_ChangeWindowMessageFilterEx(HWND h, UINT msg, DWORD action, VX_CHANGEFILTERSTRUCT *s)
{ VX_NATIVE(U32, ChangeWindowMessageFilterEx, BOOL, (HWND h, UINT msg, DWORD action, VX_CHANGEFILTERSTRUCT *s), (h, msg, action, s));
    if (s) s->ExtStatus = 0;
    if (action == 0) return TRUE;                       /* MSGFLT_RESET */
    return ChangeWindowMessageFilter(msg, action == 1 ? MSGFLT_ADD : MSGFLT_REMOVE);
}
#endif

/* ---- touch / gesture / pointer input: Vista has none, so report "no
 * touch hardware" in the ways programs already handle. ---------------------- */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_RegisterTouchWindow(HWND h, ULONG f) { VX_NATIVE(U32, RegisterTouchWindow, BOOL, (HWND h, ULONG f), (h, f)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_UnregisterTouchWindow(HWND h) { VX_NATIVE(U32, UnregisterTouchWindow, BOOL, (HWND h), (h)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_IsTouchWindow(HWND h, PULONG f) { VX_NATIVE(U32, IsTouchWindow, BOOL, (HWND h, PULONG f), (h, f)); if (f) *f = 0; return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetTouchInputInfo(HANDLE h, UINT n, PVOID in, int sz) { VX_NATIVE(U32, GetTouchInputInfo, BOOL, (HANDLE h, UINT n, PVOID in, int sz), (h, n, in, sz)); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
VX_EXPORT BOOL WINAPI vx_CloseTouchInputHandle(HANDLE h) { VX_NATIVE(U32, CloseTouchInputHandle, BOOL, (HANDLE h), (h)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetGestureInfo(HANDLE h, PVOID g) { VX_NATIVE(U32, GetGestureInfo, BOOL, (HANDLE h, PVOID g), (h, g)); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetGestureExtraArgs(HANDLE h, UINT n, PBYTE b) { VX_NATIVE(U32, GetGestureExtraArgs, BOOL, (HANDLE h, UINT n, PBYTE b), (h, n, b)); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
VX_EXPORT BOOL WINAPI vx_CloseGestureInfoHandle(HANDLE h) { VX_NATIVE(U32, CloseGestureInfoHandle, BOOL, (HANDLE h), (h)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_SetGestureConfig(HWND h, DWORD r, UINT n, PVOID c, UINT sz) { VX_NATIVE(U32, SetGestureConfig, BOOL, (HWND h, DWORD r, UINT n, PVOID c, UINT sz), (h, r, n, c, sz)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetGestureConfig(HWND h, DWORD r, DWORD f, PUINT n, PVOID c, UINT sz) { VX_NATIVE(U32, GetGestureConfig, BOOL, (HWND h, DWORD r, DWORD f, PUINT n, PVOID c, UINT sz), (h, r, f, n, c, sz)); if (n) *n = 0; return TRUE; }
#endif
VX_EXPORT BOOL WINAPI vx_EnableMouseInPointer(BOOL on) { VX_NATIVE(U32, EnableMouseInPointer, BOOL, (BOOL on), (on)); if (on) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; } return TRUE; }
VX_EXPORT BOOL WINAPI vx_IsMouseInPointerEnabled(void) { VX_NATIVE(U32, IsMouseInPointerEnabled, BOOL, (void), ()); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerType(UINT32 id, int *t) { VX_NATIVE(U32, GetPointerType, BOOL, (UINT32 id, int *t), (id, t)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerInfo(UINT32 id, PVOID i) { VX_NATIVE(U32, GetPointerInfo, BOOL, (UINT32 id, PVOID i), (id, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerInfoHistory(UINT32 id, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerInfoHistory, BOOL, (UINT32 id, UINT32 *n, PVOID i), (id, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerFrameInfo(UINT32 id, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerFrameInfo, BOOL, (UINT32 id, UINT32 *n, PVOID i), (id, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerFrameInfoHistory(UINT32 id, UINT32 *e, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerFrameInfoHistory, BOOL, (UINT32 id, UINT32 *e, UINT32 *n, PVOID i), (id, e, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerFrameTouchInfoHistory(UINT32 id, UINT32 *e, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerFrameTouchInfoHistory, BOOL, (UINT32 id, UINT32 *e, UINT32 *n, PVOID i), (id, e, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerPenInfoHistory(UINT32 id, UINT32 *e, PVOID i) { VX_NATIVE(U32, GetPointerPenInfoHistory, BOOL, (UINT32 id, UINT32 *e, PVOID i), (id, e, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerTouchInfo(UINT32 id, PVOID i) { VX_NATIVE(U32, GetPointerTouchInfo, BOOL, (UINT32 id, PVOID i), (id, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerFrameTouchInfo(UINT32 id, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerFrameTouchInfo, BOOL, (UINT32 id, UINT32 *n, PVOID i), (id, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerPenInfo(UINT32 id, PVOID i) { VX_NATIVE(U32, GetPointerPenInfo, BOOL, (UINT32 id, PVOID i), (id, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerFramePenInfo(UINT32 id, UINT32 *n, PVOID i) { VX_NATIVE(U32, GetPointerFramePenInfo, BOOL, (UINT32 id, UINT32 *n, PVOID i), (id, n, i)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerDevices(UINT32 *n, PVOID d) { VX_NATIVE(U32, GetPointerDevices, BOOL, (UINT32 *n, PVOID d), (n, d)); if (n) *n = 0; return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetPointerDevice(HANDLE h, PVOID d) { VX_NATIVE(U32, GetPointerDevice, BOOL, (HANDLE h, PVOID d), (h, d)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_GetPointerDeviceRects(HANDLE h, RECT *a, RECT *b) { VX_NATIVE(U32, GetPointerDeviceRects, BOOL, (HANDLE h, RECT *a, RECT *b), (h, a, b)); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
VX_EXPORT BOOL WINAPI vx_RegisterPointerDeviceNotifications(HWND h, BOOL r) { VX_NATIVE(U32, RegisterPointerDeviceNotifications, BOOL, (HWND h, BOOL r), (h, r)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_RegisterPointerInputTarget(HWND h, int t) { VX_NATIVE(U32, RegisterPointerInputTarget, BOOL, (HWND h, int t), (h, t)); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
VX_EXPORT BOOL WINAPI vx_UnregisterPointerInputTarget(HWND h, int t) { VX_NATIVE(U32, UnregisterPointerInputTarget, BOOL, (HWND h, int t), (h, t)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_SkipPointerFrameMessages(UINT32 id) { VX_NATIVE(U32, SkipPointerFrameMessages, BOOL, (UINT32 id), (id)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_InitializeTouchInjection(UINT32 n, DWORD m) { VX_NATIVE(U32, InitializeTouchInjection, BOOL, (UINT32 n, DWORD m), (n, m)); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
VX_EXPORT BOOL WINAPI vx_InjectTouchInput(UINT32 n, PVOID c) { VX_NATIVE(U32, InjectTouchInput, BOOL, (UINT32 n, PVOID c), (n, c)); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
VX_EXPORT BOOL WINAPI vx_SetWindowFeedbackSetting(HWND h, int f, DWORD fl, UINT32 sz, const void *c) { VX_NATIVE(U32, SetWindowFeedbackSetting, BOOL, (HWND h, int f, DWORD fl, UINT32 sz, const void *c), (h, f, fl, sz, c)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetWindowFeedbackSetting(HWND h, int f, DWORD fl, UINT32 *sz, void *c) { VX_NATIVE(U32, GetWindowFeedbackSetting, BOOL, (HWND h, int f, DWORD fl, UINT32 *sz, void *c), (h, f, fl, sz, c)); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }

typedef struct { int deviceType; int originId; } VX_INPUT_MESSAGE_SOURCE;
VX_EXPORT BOOL WINAPI vx_GetCurrentInputMessageSource(VX_INPUT_MESSAGE_SOURCE *s) { VX_NATIVE(U32, GetCurrentInputMessageSource, BOOL, (VX_INPUT_MESSAGE_SOURCE *s), (s)); s->deviceType = 0; s->originId = 0; return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetCIMSSM(VX_INPUT_MESSAGE_SOURCE *s) { VX_NATIVE(U32, GetCIMSSM, BOOL, (VX_INPUT_MESSAGE_SOURCE *s), (s)); s->deviceType = 0; s->originId = 0; return TRUE; }

/* ---- display / window ------------------------------------------------------ */
VX_EXPORT BOOL WINAPI vx_SetDisplayAutoRotationPreferences(int o) { VX_NATIVE(U32, SetDisplayAutoRotationPreferences, BOOL, (int o), (o)); return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetDisplayAutoRotationPreferences(int *o) { VX_NATIVE(U32, GetDisplayAutoRotationPreferences, BOOL, (int *o), (o)); *o = 0; return TRUE; }
VX_EXPORT BOOL WINAPI vx_GetAutoRotationState(int *s) { VX_NATIVE(U32, GetAutoRotationState, BOOL, (int *s), (s)); *s = 0x10; /* AR_NOSENSOR */ return TRUE; }
/* Suspend/resume notifications (Windows 8). Vista and 7 broadcast them
 * (WM_POWERBROADCAST) to every top-level window, so a top-level window needs
 * nothing more. The callback form, and a window that gets no broadcasts
 * (message-only or a child), go to kernelvx's notification window, which
 * calls the callback or passes the message on. */
typedef ULONG (CALLBACK *sr_cb)(PVOID, ULONG, PVOID);
typedef struct { sr_cb cb; PVOID ctx; } VX_SR_PARAMS;
static BYTE g_toplevel_reg;         /* the handle for a top-level window's registration */

static ULONG CALLBACK forward_to_window(PVOID ctx, ULONG type, PVOID setting)
{
    SendMessageW((HWND)ctx, WM_POWERBROADCAST, type, 0);
    return 0;
}

static FARPROC kvx(const char *name)
{
    HMODULE k = GetModuleHandleW(L"kernelvx.dll");
    if (!k) k = LoadLibraryW(L"kernelvx.dll");
    return k ? GetProcAddress(k, name) : NULL;
}

VX_EXPORT HANDLE WINAPI vx_RegisterSuspendResumeNotification(HANDLE r, DWORD f)
{ VX_NATIVE(U32, RegisterSuspendResumeNotification, HANDLE, (HANDLE r, DWORD f), (r, f));
    DWORD (WINAPI *reg)(PVOID, PVOID *) = (DWORD (WINAPI *)(PVOID, PVOID *))(void *)kvx("VxSuspendResumeRegister");
    VX_SR_PARAMS fw, *p = r;
    PVOID h = NULL;
    DWORD e;
    if (f == 0 /* DEVICE_NOTIFY_WINDOW_HANDLE */) {
        if (!IsWindow((HWND)r)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
        if (GetAncestor((HWND)r, GA_PARENT) == GetDesktopWindow()) return (HANDLE)&g_toplevel_reg;
        fw.cb = forward_to_window;
        fw.ctx = r;
        p = &fw;
    } else if (f != 2 /* DEVICE_NOTIFY_CALLBACK */ || !p || !p->cb) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!reg) { SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
    if ((e = reg(p, &h))) { SetLastError(e); return NULL; }
    return h;
}

VX_EXPORT BOOL WINAPI vx_UnregisterSuspendResumeNotification(HANDLE h)
{ VX_NATIVE(U32, UnregisterSuspendResumeNotification, BOOL, (HANDLE h), (h));
    DWORD (WINAPI *unreg)(PVOID) = (DWORD (WINAPI *)(PVOID))(void *)kvx("VxSuspendResumeUnregister");
    DWORD e;
    if (h == (HANDLE)&g_toplevel_reg) return TRUE;
    if (!h || !unreg) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if ((e = unreg(h))) { SetLastError(e); return FALSE; }
    return TRUE;
}
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
/* Keeping a window out of screen captures (Windows 7, with desktop
 * composition): Vista cannot, so asking for it fails, as on Windows 7 with
 * composition off. A program (a password manager, a messenger's "screen
 * security") must not believe its window is hidden from captures when it is
 * not. WDA_NONE, the state every window has here, succeeds. */
VX_EXPORT BOOL WINAPI vx_SetWindowDisplayAffinity(HWND h, DWORD a)
{ VX_NATIVE(U32, SetWindowDisplayAffinity, BOOL, (HWND h, DWORD a), (h, a));
    if (!IsWindow(h)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (a == 0 /* WDA_NONE */) return TRUE;
    SetLastError(a == 1 /* WDA_MONITOR */ || a == 0x11 /* WDA_EXCLUDEFROMCAPTURE */ ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
    return FALSE;
}
VX_EXPORT BOOL WINAPI vx_GetWindowDisplayAffinity(HWND h, DWORD *a)
{ VX_NATIVE(U32, GetWindowDisplayAffinity, BOOL, (HWND h, DWORD *a), (h, a));
    if (!a) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!IsWindow(h)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    *a = 0;
    return TRUE;
}
VX_EXPORT LONG WINAPI vx_GetDisplayConfigBufferSizes(UINT32 f, UINT32 *np, UINT32 *nm) { VX_NATIVE(U32, GetDisplayConfigBufferSizes, LONG, (UINT32 f, UINT32 *np, UINT32 *nm), (f, np, nm)); return ERROR_NOT_SUPPORTED; }
VX_EXPORT LONG WINAPI vx_QueryDisplayConfig(UINT32 f, UINT32 *np, PVOID p, UINT32 *nm, PVOID m, PVOID t) { VX_NATIVE(U32, QueryDisplayConfig, LONG, (UINT32 f, UINT32 *np, PVOID p, UINT32 *nm, PVOID m, PVOID t), (f, np, p, nm, m, t)); return ERROR_NOT_SUPPORTED; }
VX_EXPORT LONG WINAPI vx_DisplayConfigGetDeviceInfo(PVOID p) { VX_NATIVE(U32, DisplayConfigGetDeviceInfo, LONG, (PVOID p), (p)); return ERROR_NOT_SUPPORTED; }
VX_EXPORT LONG WINAPI vx_DisplayConfigSetDeviceInfo(PVOID p) { VX_NATIVE(U32, DisplayConfigSetDeviceInfo, LONG, (PVOID p), (p)); return ERROR_NOT_SUPPORTED; }
VX_EXPORT LONG WINAPI vx_SetDisplayConfig(UINT32 np, PVOID p, UINT32 nm, PVOID m, UINT32 f) { VX_NATIVE(U32, SetDisplayConfig, LONG, (UINT32 np, PVOID p, UINT32 nm, PVOID m, UINT32 f), (np, p, nm, m, f)); return ERROR_NOT_SUPPORTED; }
#endif
VX_EXPORT BOOL WINAPI vx_IsImmersiveProcess(HANDLE p) { VX_NATIVE(U32, IsImmersiveProcess, BOOL, (HANDLE p), (p)); return FALSE; }
VX_EXPORT BOOL WINAPI vx_IsWindowArranged(HWND h) { VX_NATIVE(U32, IsWindowArranged, BOOL, (HWND h), (h)); return FALSE; }
VX_EXPORT UINT_PTR WINAPI vx_SetCoalescableTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, ULONG tol) { VX_NATIVE(U32, SetCoalescableTimer, UINT_PTR, (HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, ULONG tol), (h, id, ms, fn, tol)); return SetTimer(h, id, ms, fn); }

/* CalculatePopupWindowPosition (Windows 7): place the popup at the anchor,
 * flipped and clamped to stay on the anchor's monitor. */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT BOOL WINAPI vx_CalculatePopupWindowPosition(const POINT *anchor, const SIZE *size, UINT flags, RECT *exclude, RECT *out)
{ VX_NATIVE(U32, CalculatePopupWindowPosition, BOOL, (const POINT *anchor, const SIZE *size, UINT flags, RECT *exclude, RECT *out), (anchor, size, flags, exclude, out));
    MONITORINFO mi;
    HMONITOR mon = MonitorFromPoint(*anchor, MONITOR_DEFAULTTONEAREST);
    LONG x = anchor->x, y = anchor->y;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;
    if (flags & TPM_RIGHTALIGN) x -= size->cx; else if (flags & TPM_CENTERALIGN) x -= size->cx / 2;
    if (flags & TPM_BOTTOMALIGN) y -= size->cy; else if (flags & TPM_VCENTERALIGN) y -= size->cy / 2;
    if (x + size->cx > mi.rcWork.right) x = mi.rcWork.right - size->cx;
    if (y + size->cy > mi.rcWork.bottom) y = anchor->y - size->cy;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    out->left = x; out->top = y; out->right = x + size->cx; out->bottom = y + size->cy;
    return TRUE;
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

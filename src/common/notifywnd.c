/* A hidden window on a thread of its own, for notifications that Vista and 7
 * deliver only to windows (power settings, suspend/resume, device arrival)
 * while Windows 8 also offers them as callbacks. One per kind, started at
 * the first registration. user32 is loaded only then: the DLLs using this do
 * not import it, so programs that never ask stay without it. */
#include "vx.h"

FARPROC vx_user32(const char *name)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) u = LoadLibraryW(L"user32.dll");
    return u ? GetProcAddress(u, name) : NULL;
}

LRESULT vx_defproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    static LRESULT (WINAPI * volatile def)(HWND, UINT, WPARAM, LPARAM);
    if (!def) def = (LRESULT (WINAPI *)(HWND, UINT, WPARAM, LPARAM))(void *)vx_user32("DefWindowProcW");
    return def ? def(h, msg, wp, lp) : 0;
}

LRESULT vx_send(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    static LRESULT (WINAPI * volatile send)(HWND, UINT, WPARAM, LPARAM);
    if (!send) send = (LRESULT (WINAPI *)(HWND, UINT, WPARAM, LPARAM))(void *)vx_user32("SendMessageW");
    return send ? send(h, msg, wp, lp) : 0;
}

typedef struct { vx_notifywnd *w; HANDLE ready; } start_args;

static DWORD WINAPI notify_thread(LPVOID p)
{
    typedef ATOM (WINAPI *regclass_fn)(const WNDCLASSW *);
    typedef HWND (WINAPI *create_fn)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    typedef BOOL (WINAPI *getmsg_fn)(LPMSG, HWND, UINT, UINT);
    typedef LRESULT (WINAPI *dispatch_fn)(const MSG *);
    start_args *a = p;
    vx_notifywnd *w = a->w;
    regclass_fn regclass = (regclass_fn)(void *)vx_user32("RegisterClassW");
    create_fn create = (create_fn)(void *)vx_user32("CreateWindowExW");
    getmsg_fn getmsg = (getmsg_fn)(void *)vx_user32("GetMessageW");
    dispatch_fn dispatch = (dispatch_fn)(void *)vx_user32("DispatchMessageW");
    WNDCLASSW wc;
    MSG m;
    if (regclass && create && getmsg && dispatch) {
        vx_memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = w->proc;
        wc.hInstance = w->module;
        wc.lpszClassName = w->cls;
        regclass(&wc);
        /* top-level when broadcasts must reach it (suspend/resume go to
         * top-level windows only); message-only otherwise. Never shown. */
        w->hwnd = create(WS_EX_TOOLWINDOW, w->cls, L"", WS_POPUP, 0, 0, 0, 0,
                         w->toplevel ? NULL : HWND_MESSAGE, NULL, w->module, NULL);
    }
    SetEvent(a->ready);                 /* a is gone after this */
    if (!w->hwnd) return 0;
    while (getmsg(&m, NULL, 0, 0) > 0) dispatch(&m);
    return 0;
}

HWND vx_notify_window(vx_notifywnd *w)
{
    if (InterlockedCompareExchange(&w->state, 1, 0) == 0) {
        start_args a;
        HANDLE t = NULL;
        a.w = w;
        a.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (a.ready && (t = CreateThread(NULL, 0, notify_thread, &a, 0, NULL))) {
            WaitForSingleObject(a.ready, INFINITE);
            CloseHandle(t);
        }
        if (a.ready) CloseHandle(a.ready);
        InterlockedExchange(&w->state, 2);
    }
    while (w->state == 1) SwitchToThread();
    return w->hwnd;
}

/* vxwatch.exe - VistaExt's crash window.
 *
 *   vxwatch <process handle> <shared block handle>     (started by kernelvx)
 *
 * kernelvx starts this for the main process of a windowed program. It waits
 * for the program to end. A normal end: it ends too, silently. An end with a
 * crash code: it shows what happened in a few lines and lets the user restart
 * the program told another Windows version (saved for this user, so no
 * administrator rights are needed; it takes priority over the setting made
 * in VistaExt Center).
 *
 *   vxwatch /demo      shows the window with made-up details (tests) */
#include "../common/vx.h"
#include <shellapi.h>

static vx_watch *g_w;
static DWORD g_code;
static HWND g_wnd, g_combo, g_check;
static HFONT g_font, g_bold;
static WCHAR g_details[3000];

enum { ID_RESTART = 100, ID_COPY, ID_CLOSE, ID_COMBO, ID_CHECK };

static const WCHAR *describe(DWORD c)
{
    switch (c) {
    case 0xC0000005: return L"memory access violation (it used memory it should not have)";
    case 0x80000003: return L"it stopped itself on purpose (a check inside it failed)";
    case 0xC000001D: return L"it stopped itself, or needs a newer processor (illegal instruction)";
    case 0xC0000409: return L"it stopped itself (security check or \"fail fast\")";
    case 0xC0000602: return L"it stopped itself (\"fail fast\")";
    case 0xC0000374: return L"its memory got damaged (heap corruption)";
    case 0xC00000FD: return L"stack overflow";
    case 0xC0000094: return L"division by zero";
    case 0xC0000096: return L"privileged instruction";
    case 0xC0000417: return L"the C runtime found an invalid parameter";
    case 0xC0000135: return L"a DLL it needs is missing";
    case 0xC0000139: return L"a function it needs is missing from Windows (entry point not found)";
    case 0xC000007B: return L"a DLL of the wrong kind (32/64-bit) or a damaged file";
    case 0xC0000142: return L"a DLL it needs could not start";
    case 0xE06D7363: return L"an error inside it that nothing caught (C++ exception)";
    }
    return NULL;
}

static const WCHAR *exe_name(void)
{
    const WCHAR *p = g_w->exe, *b = p;
    for (; *p; p++) if (*p == '\\') b = p + 1;
    return b;
}

static const vx_osver *current_ver(WCHAR *layer, DWORD cch, vx_layer *l)
{
    layer[0] = 0;
    vx_lookup_layer(g_w->exe, layer, cch);
    vx_effective_layer(g_w->exe, l);       /* the program's own, or the global one */
    return l->ver;
}

/* ---- actions --------------------------------------------------------------- */
static void save_choice(void)
{
    WCHAR layer[700], cur[600];
    vx_layer l;
    HKEY k;
    LRESULT sel = SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    const vx_osver *v = sel > 0 && sel <= vx_osver_count ? &vx_osvers[sel - 1] : &vx_osver_real;
    current_ver(cur, 600, &l);
    vx_make_layer(layer, 700, v, l.webview2, l.args);
    /* this user's own setting: no administrator rights needed */
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, VX_REG_LAYERS, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
        RegSetValueExW(k, g_w->exe, 0, REG_SZ, (const BYTE *)layer, (DWORD)((vx_wcslen(layer) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

static void save_quiet(void)
{
    HKEY k;
    DWORD one = 1;
    if (SendMessageW(g_check, BM_GETCHECK, 0, 0) != BST_CHECKED) return;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, VX_REG_NOCRASHWND, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
        RegSetValueExW(k, g_w->exe, 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        RegCloseKey(k);
    }
}

static void restart(void)
{
    SHELLEXECUTEINFOW se;
    const WCHAR *a = g_w->cmdline;
    /* the arguments: the command line after the program's own name */
    if (*a == '"') { a++; while (*a && *a != '"') a++; if (*a) a++; }
    else while (*a && *a != ' ' && *a != '\t') a++;
    while (*a == ' ' || *a == '\t') a++;
    vx_memset(&se, 0, sizeof(se));
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOASYNC;
    se.lpFile = g_w->exe;
    se.lpParameters = *a ? a : NULL;
    se.lpDirectory = g_w->cwd[0] ? g_w->cwd : NULL;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se))
        MessageBoxW(g_wnd, L"The program could not be started again.", L"VistaExt", MB_ICONWARNING);
}

static void copy_details(void)
{
    HGLOBAL h;
    SIZE_T n = (vx_wcslen(g_details) + 1) * sizeof(WCHAR);
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if ((h = GlobalAlloc(GMEM_MOVEABLE, n))) {
        vx_memcpy(GlobalLock(h), g_details, n);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

/* ---- window ---------------------------------------------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RESTART: save_quiet(); save_choice(); restart(); DestroyWindow(h); return 0;
        case ID_COPY: copy_details(); return 0;
        case ID_CLOSE: case IDCANCEL: save_quiet(); DestroyWindow(h); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE: save_quiet(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND ctl(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int hgt, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hgt, g_wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, FALSE);
    return c;
}

static int text_height(const WCHAR *t, HFONT f, int width)
{
    HDC dc = GetDC(NULL);
    RECT r = { 0, 0, width, 0 };
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return r.bottom;
}

static void show_window(void)
{
    WNDCLASSW wc;
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    WCHAR title[MAX_PATH + 40], head[MAX_PATH + 60], body[2400], layer[600], line[700];
    vx_layer l;
    const vx_osver *cur = current_ver(layer, 600, &l);
    const WCHAR *what = describe(g_code);
    MSG m;
    int W = 500, pad = 16, x = pad + 48, tw = W - x - pad, y = pad, hh, i, bh;
    RECT r;

    vx_memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 5 / 4;
    g_bold = CreateFontIndirectW(&lf);

    wsprintfW(head, L"%s stopped unexpectedly", exe_name());
    wsprintfW(body, L"Error 0x%08X: %s.", g_code, what ? what : L"it ended with an error code");
    if (g_w->code == (LONG)g_code && g_w->where[0]) {
        wsprintfW(line, L"\nWhere: %s", g_w->where);
        vx_wcscatn(body, line, 2400);
    }
    if (g_w->message[0]) {
        wsprintfW(line, L"\nIts last message: %s", g_w->message);
        vx_wcscatn(body, line, 2400);
    }
    wsprintfW(line, L"\n\nVistaExt tells it that it runs on: %s.\nA program sometimes takes paths that this Windows does not support "
                    L"when it is told a newer version, or refuses to run when told an older one. Try another:",
              cur ? cur->label : L"this Windows' own version");
    vx_wcscatn(body, line, 2400);
    wsprintfW(g_details, L"%s\r\nProgram: %s\r\nCommand line: %s\r\n", head, g_w->exe, g_w->cmdline);
    {
        WCHAR b2[2400];
        WCHAR *s = body, *d = b2;
        while (*s && d < b2 + 2390) { if (*s == '\n') *d++ = '\r'; *d++ = *s++; }
        *d = 0;
        vx_wcscatn(g_details, b2, 3000);
    }

    vx_memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"VistaExtCrash";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_ERROR);
    RegisterClassW(&wc);
    wsprintfW(title, L"%s - VistaExt", exe_name());
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, wc.lpszClassName, title,
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, W, 200, NULL, NULL, wc.hInstance, NULL);

    ctl(L"STATIC", NULL, SS_ICON, pad, y, 32, 32, 0, g_font);
    SendMessageW(GetWindow(g_wnd, GW_CHILD), STM_SETICON, (WPARAM)LoadIconW(NULL, (LPCWSTR)IDI_ERROR), 0);
    hh = text_height(head, g_bold, tw);
    ctl(L"STATIC", head, SS_NOPREFIX, x, y, tw, hh, 0, g_bold);
    y += hh + 8;
    hh = text_height(body, g_font, tw);
    ctl(L"STATIC", body, SS_NOPREFIX, x, y, tw, hh, 0, g_font);
    y += hh + 10;
    g_combo = ctl(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, tw, 300, ID_COMBO, g_font);
    SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)L"This Windows' own version");
    for (i = 0; i < vx_osver_count; i++) SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)vx_osvers[i].label);
    /* suggest a step: from a newer version one down, from none Windows 10 */
    {
        int c = 0, s;
        for (i = 0; i < vx_osver_count; i++) if (cur == &vx_osvers[i]) c = i + 1;
        s = c > 1 ? c - 1 : (c == 0 ? 0 : 0);
        if (c == 0) for (i = 0; i < vx_osver_count; i++) if (!vx_wcsicmp(vx_osvers[i].token, L"WIN10")) s = i + 1;
        SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)s, 0);
    }
    GetWindowRect(g_combo, &r);
    y += 26 + 8;
    wsprintfW(line, L"Don't show this again for %s", exe_name());
    g_check = ctl(L"BUTTON", line, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, tw, 20, ID_CHECK, g_font);
    y += 30;
    bh = 26;
    ctl(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, W - pad - 80 - 6, y, 80, bh, ID_CLOSE, g_font);
    ctl(L"BUTTON", L"Copy details", BS_PUSHBUTTON | WS_TABSTOP, W - pad - 80 - 6 - 8 - 100, y, 100, bh, ID_COPY, g_font);
    ctl(L"BUTTON", L"Restart with this setting", BS_DEFPUSHBUTTON | WS_TABSTOP, W - pad - 80 - 6 - 8 - 100 - 8 - 170, y, 170,
        bh, ID_RESTART, g_font);
    y += bh + pad;
    {
        RECT wr = { 0, 0, W, y };
        AdjustWindowRectEx(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        SetWindowPos(g_wnd, NULL, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
                     (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 3, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOZORDER);
    }
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
    MessageBeep(MB_ICONERROR);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static BOOL crash_code(DWORD c)
{
    return describe(c) != NULL || (c >= 0xC0000000 && c != 0xC000013A /* Ctrl+C */);
}

/* Every crash is also written to %LOCALAPPDATA%\VistaExt\crashes.log (one
 * line each, UTF-8, started afresh above 256 KB): the window can be closed
 * or turned off, and support needs the details later. */
static void log_crash(void)
{
    WCHAR path[MAX_PATH + 40], line[1200];
    char u8[3600];
    SYSTEMTIME st;
    HANDLE f;
    DWORD n, w, len;
    const WCHAR *d = describe(g_code);
    if (!(n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) || n >= MAX_PATH) return;
    vx_wcscatn(path, L"\\VistaExt", MAX_PATH + 40);
    CreateDirectoryW(path, NULL);
    vx_wcscatn(path, L"\\crashes.log", MAX_PATH + 40);
    GetLocalTime(&st);
    wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u  %s  exit 0x%08X (%s)  at %s  %s\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_w->exe, g_code,
              d ? d : L"stopped with an error", g_w->where[0] ? g_w->where : L"?", g_w->message);
    len = (DWORD)WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    if (!len) return;
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(f, NULL) > 256 * 1024) {           /* start afresh */
        CloseHandle(f);
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    WriteFile(f, u8, len - 1, &w, NULL);
    CloseHandle(f);
}

static ULONG_PTR num(const WCHAR *s)
{
    ULONG_PTR v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (ULONG_PTR)(*s++ - '0');
    return v;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cl, int show)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HANDLE proc, sec;
    if (argc >= 2 && !vx_wcsicmp(argv[1], L"/demo")) {
        static vx_watch demo;
        g_w = &demo;
        g_code = 0x80000003;
        vx_wcscpyn(demo.exe, argc >= 3 ? argv[2] : L"C:\\Program Files\\Example\\example.exe", MAX_PATH);
        vx_wcscpyn(demo.cmdline, L"\"example.exe\"", 4096);
        demo.code = (LONG)g_code;
        vx_wcscpyn(demo.where, L"chrome.dll+0xc355254", 128);
        vx_wcscpyn(demo.message, L"[2448:2912:1118/181213.363:FATAL:gpu_data_manager_impl_private.cc:385] GPU process isn't usable. Goodbye.", 400);
        show_window();
        return 0;
    }
    if (argc < 3) return 2;
    proc = (HANDLE)num(argv[1]);
    sec = (HANDLE)num(argv[2]);
    if (!(g_w = MapViewOfFile(sec, FILE_MAP_READ, 0, 0, sizeof(vx_watch))) || g_w->magic != VX_WATCH_MAGIC) return 3;
    WaitForSingleObject(proc, INFINITE);
    if (!GetExitCodeProcess(proc, &g_code) || !crash_code(g_code)) return 0;
    {   /* The block was written by the program that just crashed - perhaps
         * by the very bug that crashed it. Work on a copy whose texts are
         * certain to end inside their fields. */
        static vx_watch copy;
        vx_memcpy(&copy, g_w, sizeof(copy));
        copy.where[sizeof(copy.where) / sizeof(WCHAR) - 1] = 0;
        copy.message[sizeof(copy.message) / sizeof(WCHAR) - 1] = 0;
        copy.exe[MAX_PATH - 1] = 0;
        copy.cwd[MAX_PATH - 1] = 0;
        copy.cmdline[sizeof(copy.cmdline) / sizeof(WCHAR) - 1] = 0;
        UnmapViewOfFile(g_w);
        g_w = &copy;
    }
    log_crash();
    {   /* tests: VX_WATCH_TEST_CHOICE=<WIN10...> answers the window at once */
        WCHAR t[32];
        if (GetEnvironmentVariableW(L"VX_WATCH_TEST_CHOICE", t, 32)) {
            const vx_osver *v = vx_find_osver(t);
            WCHAR layer[700], cur[600];
            vx_layer l;
            HKEY k;
            current_ver(cur, 600, &l);
            vx_make_layer(layer, 700, v, l.webview2, l.args);
            if (!RegCreateKeyExW(HKEY_CURRENT_USER, VX_REG_LAYERS, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
                RegSetValueExW(k, g_w->exe, 0, REG_SZ, (const BYTE *)layer, (DWORD)((vx_wcslen(layer) + 1) * sizeof(WCHAR)));
                RegCloseKey(k);
            }
            SetEnvironmentVariableW(L"VX_WATCH_TEST_CHOICE", NULL);
            restart();
            return 0;
        }
    }
    show_window();
    return 0;
}

void WinMainCRTStartup(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
}

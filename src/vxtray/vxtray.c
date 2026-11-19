/* vxtray.exe - VistaExt's icon in the notification area: the only part of
 * VistaExt the user sees while it works. Everything else runs hidden (the
 * service, vxload inside programs).
 *
 * Its menu (left or right click):
 *   - VistaExt on / off for the whole system (asks the service; switching
 *     off puts Windows back as it is without VistaExt, no restart needed
 *     unless a changed program is running - then it says so)
 *   - the Windows version programs are told: presets with their real
 *     numbers, this Windows' own version, or a custom version
 *   - VistaExt Center (settings), the log, "Start with Windows", Exit
 *
 * It runs as the signed-in user. Changes go through VistaExt's service
 * (\\.\pipe\VistaExt), which accepts them from administrators; for a
 * standard user, or if the service is not running, vxauto.exe is started
 * with administrator rights instead (a UAC prompt).
 *
 *   vxtray.exe              show the icon (one per user session)
 *   vxtray.exe /autostart   the same, unless this user turned "Start with
 *                           Windows" off (the Run entry passes this) */
#include "../common/vx.h"
#include <shellapi.h>
#include <shlobj.h>

#define WM_TRAY      (WM_APP + 1)
#define ID_TIMER     1
#define IDM_TOGGLE   100
#define IDM_REAL     101
#define IDM_CUSTOM   102
#define IDM_CENTER   103
#define IDM_LOG      104
#define IDM_AUTO     105
#define IDM_EXIT     106
#define IDM_SETUPS   107
#define IDM_VER0     200            /* + index into vx_osvers */
#define VX_REG_TRAY  L"SOFTWARE\\VistaExt\\Tray"     /* HKCU: this user's choices */

static HINSTANCE g_inst;
static HWND g_wnd;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;
static BOOL g_on = TRUE, g_restart, g_added;
static DWORD g_waiting;       /* switched off while changed programs ran: put back once closed */
static WCHAR g_ver[40], g_label[80];
static const WCHAR *TITLE = L"VistaExt";

/* ---- state (read from the registry: anyone may read it) ---------------------- */
static void describe(const WCHAR *tok, WCHAR *out, SIZE_T cch)
{
    const vx_osver *v = vx_find_osver(tok);
    vx_osver cv;
    WCHAR t[40], csd[24];
    if (!tok[0] || (v && v == &vx_osver_real)) { vx_wcscpyn(out, L"this Windows' own version", cch); return; }
    if (v) { wsprintfW(out, L"%s (%u.%u.%u)", v->label, v->major, v->minor, v->build); return; }
    if (vx_parse_custom(tok, &cv, t, 40, csd, 24)) {
        wsprintfW(out, L"custom version %u.%u.%u", cv.major, cv.minor, cv.build);
        if (cv.spmajor) { WCHAR sp[24]; wsprintfW(sp, L" SP%u", cv.spmajor); vx_wcscatn(out, sp, cch); }
        return;
    }
    vx_wcscpyn(out, tok, cch);
}

static BOOL restart_pending(void)
{
    HKEY k;
    ULONGLONG t = 0, now, boot;
    DWORD cb = sizeof(t), type;
    FILETIME ft;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    if (RegQueryValueExW(k, L"RestartNeeded", NULL, &type, (BYTE *)&t, &cb) || type != REG_QWORD) t = 0;
    RegCloseKey(k);
    GetSystemTimeAsFileTime(&ft);
    now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    boot = now - (ULONGLONG)GetTickCount64() * 10000;
    return t && t >= boot;
}

static BOOL setups(void)
{
    HKEY k;
    DWORD v = 1, cb = sizeof(v), type;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        if (RegQueryValueExW(k, L"Setups", NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = 1;
        RegCloseKey(k);
    }
    return v != 0;
}

static BOOL enabled(void)
{
    HKEY k;
    DWORD v = 1, cb = sizeof(v), type;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        if (RegQueryValueExW(k, L"Enabled", NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = 1;
        RegCloseKey(k);
    }
    return v != 0;
}

static void read_state(void)
{
    WCHAR v[64];
    g_on = enabled();
    if (!vx_global_version(v, 64)) v[0] = 0;
    vx_wcscpyn(g_ver, v[0] ? v : L"REAL", 40);
    describe(v, g_label, 80);
    g_restart = restart_pending();
    {
        HKEY k;
        g_waiting = 0;
        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT L"\\RestoreLater", 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
            RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &g_waiting, NULL, NULL, NULL, NULL);
            RegCloseKey(k);
        }
    }
}

/* ---- the icon ---------------------------------------------------------------- */
static void update_icon(BOOL add)
{
    WCHAR tip[128];
    g_nid.cbSize = sizeof(g_nid);       /* Vista's size: balloons, and its own icon sizes */
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(g_on ? 1 : 2), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (g_on) wsprintfW(tip, L"VistaExt is on\nPrograms are told: %s", g_label);
    else wsprintfW(tip, L"VistaExt is off\n(Windows works as it does without it)");
    if (g_restart) vx_wcscatn(tip, L"\nRestart Windows to finish", 128);
    else if (!g_on && g_waiting) vx_wcscatn(tip, L"\nWaiting for changed programs to close", 128);
    vx_wcscpyn(g_nid.szTip, tip, sizeof(g_nid.szTip) / sizeof(WCHAR));
    if (add || !g_added) g_added = Shell_NotifyIconW(NIM_ADD, &g_nid);
    else Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = NULL;
}

static void balloon(const WCHAR *title, const WCHAR *text, DWORD icon)
{
    g_nid.uFlags = NIF_INFO;
    vx_wcscpyn(g_nid.szInfoTitle, title, sizeof(g_nid.szInfoTitle) / sizeof(WCHAR));
    vx_wcscpyn(g_nid.szInfo, text, sizeof(g_nid.szInfo) / sizeof(WCHAR));
    g_nid.dwInfoFlags = icon;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ---- changing things ---------------------------------------------------------- */
static void tool_path(const WCHAR *exe, WCHAR *out)
{
    SIZE_T n;
    GetModuleFileNameW(NULL, out, MAX_PATH);
    n = vx_wcslen(out);
    while (n && out[n - 1] != '\\') n--;
    out[n] = 0;
    vx_wcscatn(out, exe, MAX_PATH);
}

/* ask the service; FALSE: it is not there, or says no (then: as administrator) */
static BOOL ask_service(const WCHAR *req, WCHAR *rep, DWORD cch)
{
    DWORD got = 0;
    rep[0] = 0;
    if (!CallNamedPipeW(L"\\\\.\\pipe\\VistaExt", (void *)req, (DWORD)((vx_wcslen(req) + 1) * sizeof(WCHAR)), rep,
                        (cch - 1) * sizeof(WCHAR), &got, 60000))
        return FALSE;
    rep[got / sizeof(WCHAR) < cch ? got / sizeof(WCHAR) : cch - 1] = 0;
    return vx_wcsnicmp(rep, L"error=", 6) != 0;
}

static BOOL run_elevated(const WCHAR *args)
{
    SHELLEXECUTEINFOW se;
    WCHAR exe[MAX_PATH];
    DWORD code = 1;
    tool_path(L"vxauto.exe", exe);
    vx_memset(&se, 0, sizeof(se));
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    se.hwnd = g_wnd;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = args;
    se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se) || !se.hProcess) return FALSE;
    WaitForSingleObject(se.hProcess, 120000);
    GetExitCodeProcess(se.hProcess, &code);
    CloseHandle(se.hProcess);
    return code == 0 || code == 3;          /* 3: done, restart needed */
}

static void change(const WCHAR *req, const WCHAR *args)
{
    WCHAR rep[512];
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    BOOL ok = ask_service(req, rep, 512) || run_elevated(args);
    SetCursor(old);
    read_state();
    update_icon(FALSE);
    if (!ok) {
        MessageBoxW(g_wnd, L"The change was not made. It needs administrator rights, and VistaExt's service "
                           L"(\"VistaExt automatic mode\") should be running.", TITLE, MB_ICONWARNING);
        return;
    }
    if (!vx_wcsnicmp(req, L"SETUPS", 6)) {
        balloon(L"Setup programs", setups() ? L"Setup programs you download are told the chosen Windows version too "
                                              L"(except Microsoft's own and driver packages)."
                                            : L"Setup programs now see this Windows' real version, unless they cannot run "
                                              L"on it without VistaExt.", NIIF_INFO);
    } else if (!vx_wcsnicmp(req, L"SETVER", 6)) {
        WCHAR m[200];
        wsprintfW(m, L"Programs started from now on are told %s. Programs that are running keep what they "
                     L"were told until you restart them.", g_label);
        balloon(L"Windows version changed", m, NIIF_INFO);
    } else if (g_on)
        balloon(L"VistaExt is on", L"Programs made for newer Windows start again with VistaExt.", NIIF_INFO);
    else if (g_restart)
        balloon(L"VistaExt is off", L"Some of VistaExt's files were in use. Restart Windows to finish.", NIIF_WARNING);
    else if (g_waiting) {
        WCHAR m[256];
        wsprintfW(m, L"Windows works as it does without VistaExt. %u program(s) VistaExt changed are running: each "
                     L"is put back as soon as you close it.", g_waiting);
        balloon(L"VistaExt is off", m, NIIF_INFO);
    } else
        balloon(L"VistaExt is off", L"Windows works as it does without VistaExt. Programs that are running keep it "
                                    L"until you close them.", NIIF_INFO);
}

static void set_version(const WCHAR *tok)
{
    WCHAR req[64], args[64];
    wsprintfW(req, L"SETVER %s", tok);
    wsprintfW(args, L"/setver %s", tok);
    change(req, args);
}

/* ---- custom version dialog -------------------------------------------------------- */
static HWND g_dlg, g_ed[4];

static LRESULT CALLBACK dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK) {
            WCHAR f[4][8], tok[48];
            vx_osver cv;
            WCHAR t[40], csd[24];
            int i;
            for (i = 0; i < 4; i++) GetWindowTextW(g_ed[i], f[i], 8);
            wsprintfW(tok, L"VER:%s.%s.%s.%s", f[0], f[1], f[2], f[3][0] ? f[3] : L"0");
            if (!vx_parse_custom(tok, &cv, t, 40, csd, 24)) {
                MessageBoxW(h, L"Please enter numbers: major 5-99, minor 0-99, build 0-65535, service pack 0-9.\n\n"
                               L"Examples: Windows 10 22H2 = 10.0.19045, Windows 11 24H2 = 10.0.26100, "
                               L"Windows 8.1 = 6.3.9600.", TITLE, MB_ICONWARNING);
                return 0;
            }

            set_version(t);
        }
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { g_dlg = NULL; return 0; }
    return DefWindowProcW(h, m, wp, lp);
}

static void custom_dialog(void)
{
    static const WCHAR *names[4] = { L"Major", L"Minor", L"Build", L"Service pack" };
    WNDCLASSW wc;
    NONCLIENTMETRICSW ncm;
    HFONT font;
    vx_osver cv;
    WCHAR t[40], csd[24], init[4][8];
    int i, x = 16, y = 16;
    MSG msg;
    if (g_dlg) { SetForegroundWindow(g_dlg); return; }
    vx_memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    font = CreateFontIndirectW(&ncm.lfMessageFont);
    vx_memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"VistaExtCustomVersion";
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    /* start from the version in use */
    {
        const vx_osver *v = vx_find_osver(g_ver);
        if (!v || v == &vx_osver_real) v = vx_parse_custom(g_ver, &cv, t, 40, csd, 24) ? &cv : vx_find_osver(L"WIN10");
        wsprintfW(init[0], L"%u", v->major); wsprintfW(init[1], L"%u", v->minor);
        wsprintfW(init[2], L"%u", v->build); wsprintfW(init[3], L"%u", v->spmajor);
    }
    g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, wc.lpszClassName, L"VistaExt - custom Windows version",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, (GetSystemMetrics(SM_CXSCREEN) - 420) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - 230) / 2, 420, 230, g_wnd, NULL, g_inst, NULL);
    if (!g_dlg) return;
#define CTL(cls, text, style, X, Y, W, H, id) \
    SendMessageW(CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), X, Y, W, H, g_dlg, (HMENU)(INT_PTR)(id), g_inst, NULL), \
                 WM_SETFONT, (WPARAM)font, TRUE)
    CTL(L"STATIC", L"The Windows version programs are told, as numbers. For experts: a version a program "
                   L"does not expect can make it take paths this Windows does not support.", 0, x, y, 380, 48, -1);
    y += 56;
    for (i = 0; i < 4; i++) {
        CTL(L"STATIC", names[i], 0, x + i * 95, y, 90, 18, -1);
        g_ed[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", init[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                  x + i * 95, y + 20, 80, 23, g_dlg, NULL, g_inst, NULL);
        SendMessageW(g_ed[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    y += 60;
    CTL(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 220, y, 80, 26, IDOK);
    CTL(L"BUTTON", L"Cancel", WS_TABSTOP, 310, y, 80, 26, IDCANCEL);
#undef CTL
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
    SetFocus(g_ed[2]);
    while (g_dlg && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DeleteObject(font);
}

/* ---- "Start with Windows" (this user's choice; the Run entry is the installer's) - */
static BOOL autostart_on(void)
{
    HKEY k;
    DWORD v = 1, cb = sizeof(v), type;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, VX_REG_TRAY, 0, KEY_QUERY_VALUE, &k)) {
        if (RegQueryValueExW(k, L"Autostart", NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = 1;
        RegCloseKey(k);
    }
    return v != 0;
}

static void set_autostart(BOOL on)
{
    HKEY k;
    DWORD v = on;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, VX_REG_TRAY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
        RegSetValueExW(k, L"Autostart", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* ---- the menu ------------------------------------------------------------------------ */
static void show_menu(void)
{
    HMENU m = CreatePopupMenu(), vm = CreatePopupMenu();
    WCHAR line[160];
    POINT pt;
    int i, cmd;
    BOOL custom = !vx_wcsnicmp(g_ver, L"VER:", 4);
    read_state();
    wsprintfW(line, g_on ? L"VistaExt is on - programs are told %s" : L"VistaExt is off", g_label);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
    if (g_restart) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Restart Windows to finish the last change");
    else if (!g_on && g_waiting) {
        wsprintfW(line, L"%u changed program(s) are put back when closed", g_waiting);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_on ? MF_CHECKED : 0), IDM_TOGGLE, L"&Enabled (compatibility for programs made for newer Windows)");
    for (i = 0; i < vx_osver_count; i++) {
        const vx_osver *v = &vx_osvers[i];
        wsprintfW(line, L"%s\t%u.%u.%u", v->label, v->major, v->minor, v->build);
        AppendMenuW(vm, MF_STRING, IDM_VER0 + i, line);
        if (!vx_wcsicmp(g_ver, v->token)) CheckMenuRadioItem(vm, IDM_VER0 + i, IDM_VER0 + i, IDM_VER0 + i, MF_BYCOMMAND);
    }
    AppendMenuW(vm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(vm, MF_STRING, IDM_REAL, L"This Windows' own version (no change)");
    if (!vx_wcsicmp(g_ver, L"REAL")) CheckMenuRadioItem(vm, IDM_REAL, IDM_REAL, IDM_REAL, MF_BYCOMMAND);
    if (custom) wsprintfW(line, L"Custom: %s...", g_ver + 4);
    else vx_wcscpyn(line, L"Custom...", 160);
    AppendMenuW(vm, MF_STRING, IDM_CUSTOM, line);
    if (custom) CheckMenuRadioItem(vm, IDM_CUSTOM, IDM_CUSTOM, IDM_CUSTOM, MF_BYCOMMAND);
    AppendMenuW(vm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(vm, MF_STRING | (setups() ? MF_CHECKED : 0), IDM_SETUPS,
                L"Setup programs are told it too (not Microsoft's, not drivers)");
    AppendMenuW(m, MF_POPUP | (g_on ? 0 : MF_GRAYED), (UINT_PTR)vm, L"Windows &version told to programs");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_CENTER, L"&Settings for one program (VistaExt Center)...");
    AppendMenuW(m, MF_STRING, IDM_LOG, L"View &log");
    AppendMenuW(m, MF_STRING | (autostart_on() ? MF_CHECKED : 0), IDM_AUTO, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit (VistaExt keeps working)");
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_TOGGLE:
        if (g_on && MessageBoxW(g_wnd, L"Turn VistaExt off?\n\nWindows then works exactly as it does without VistaExt: "
                                       L"programs that need it will not start until you turn it on again. Your settings "
                                       L"are kept.", TITLE, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
            break;
        change(g_on ? L"DISABLE" : L"ENABLE", g_on ? L"/disable" : L"/enable");
        break;
    case IDM_REAL: set_version(L"REAL"); break;
    case IDM_SETUPS: {
        BOOL on = !setups();
        change(on ? L"SETUPS 1" : L"SETUPS 0", on ? L"/setups 1" : L"/setups 0");
        break;
    }
    case IDM_CUSTOM: custom_dialog(); break;
    case IDM_CENTER: {
        WCHAR exe[MAX_PATH];
        tool_path(L"vxcenter.exe", exe);
        ShellExecuteW(g_wnd, NULL, exe, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case IDM_LOG: {
        WCHAR log[MAX_PATH], arg[MAX_PATH + 4];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, log))) break;
        vx_wcscatn(log, L"\\VistaExt\\VistaExt.log", MAX_PATH);
        if (GetFileAttributesW(log) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(g_wnd, L"Nothing has been logged yet.", TITLE, MB_ICONINFORMATION);
            break;
        }
        wsprintfW(arg, L"\"%s\"", log);
        ShellExecuteW(g_wnd, NULL, L"notepad.exe", arg, NULL, SW_SHOWNORMAL);
        break;
    }
    case IDM_AUTO: set_autostart(!autostart_on()); break;
    case IDM_EXIT: DestroyWindow(g_wnd); break;
    default:
        if (cmd >= IDM_VER0 && cmd < IDM_VER0 + vx_osver_count) set_version(vx_osvers[cmd - IDM_VER0].token);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == g_taskbar_created && m) { g_added = FALSE; update_icon(TRUE); return 0; }   /* Explorer restarted */
    switch (m) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) show_menu();
        return 0;
    case WM_TIMER: {
        BOOL on = g_on, rs = g_restart;
        DWORD wt = g_waiting;
        WCHAR v[40];
        vx_wcscpyn(v, g_ver, 40);
        read_state();
        if (on != g_on || rs != g_restart || wt != g_waiting || vx_wcsicmp(v, g_ver)) update_icon(FALSE);
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cl, int show)
{
    WNDCLASSW wc;
    MSG msg;
    int argc = 0, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HANDLE one;
    g_inst = inst;
    for (i = 1; argv && i < argc; i++)
        if (!vx_wcsicmp(argv[i], L"/autostart") && !autostart_on()) return 0;
    /* one icon per user session */
    one = CreateMutexW(NULL, FALSE, L"Local\\VistaExtTray");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    vx_memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VistaExtTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, TITLE, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    read_state();
    /* Explorer may not have its notification area yet right after logon */
    for (i = 0; i < 30; i++) {
        update_icon(TRUE);
        if (g_added) break;
        Sleep(2000);
    }
    SetTimer(g_wnd, ID_TIMER, 3000, NULL);
    for (i = 1; argv && i < argc; i++)          /* tests: open the menu at once, for a picture */
        if (!vx_wcsicmp(argv[i], L"/showmenu")) PostMessageW(g_wnd, WM_TRAY, 1, WM_RBUTTONUP);
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

void WinMainCRTStartup(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
}

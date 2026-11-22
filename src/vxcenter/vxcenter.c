/* VistaExt (vxcenter.exe) - the window the Start menu's "VistaExt" opens:
 *  - VistaExt on or off for the whole system (the compatibility layer; also
 *    in Safe Mode, where the service does not run),
 *  - pick a program (or folder), choose which Windows version it should see,
 *    and make it run on Vista / 7 / 8,
 *  - it starts the tray icon (as the signed-in user) if it is not running.
 * Also sets up the Explorer right-click entries and the Start menu folder
 * (/register, /unregister). */
#include "../vxpatch/patchlib.h"
#include "../common/userrun.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shlguid.h>

enum { IDC_PATH = 100, IDC_BROWSE, IDC_BROWSEDIR, IDC_VER, IDC_WV2, IDC_CHROMIUM, IDC_ARGS,
       IDC_CHECK, IDC_APPLY, IDC_UNDO, IDC_RUN, IDC_CLOSE, IDC_LOG, IDC_STATUS, IDC_ONOFF, IDC_ONTEXT };

#define CHROMIUM_ARGS VXP_CHROMIUM_ARGS

#define WM_VX_DONE (WM_APP + 1)
#define WM_VX_SWITCHED (WM_APP + 2)   /* the on/off switch finished: wp = it worked, lp = restart needed */

static HWND g_wnd, g_log;
static volatile LONG g_busy;    /* a check/apply/undo runs on the worker thread */
static HFONT g_font, g_bold;
static UINT g_dpi = 96;
static WCHAR g_path[MAX_PATH];
static WCHAR g_custom[40];    /* the program's own version given by number ("VER:..."), if any */
static BOOL g_isdir;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static HWND item(int id) { return GetDlgItem(g_wnd, id); }

/* ---- log pane ------------------------------------------------------------ */
static void log_line(void *ctx, const WCHAR *line)
{
    int n = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void log_clear(void) { SetWindowTextW(g_log, L""); }

static void status(const WCHAR *s) { SetWindowTextW(item(IDC_STATUS), s); }

/* ---- settings <-> controls ------------------------------------------------ */
static void settings_key_path(WCHAR *out)
{
    vx_wcscpyn(out, g_path, MAX_PATH);
    if (g_isdir) {
        SIZE_T n = vx_wcslen(out);
        if (n && out[n - 1] != '\\') vx_wcscatn(out, L"\\", MAX_PATH);
    }
}

static void load_settings(void)
{
    WCHAR key[MAX_PATH], probe[MAX_PATH + 4], layer[600] = L"";
    vx_layer l;
    int i, sel = 0;
    settings_key_path(key);
    vx_wcscpyn(probe, key, MAX_PATH);
    if (g_isdir) vx_wcscatn(probe, L"x", MAX_PATH + 4);   /* lookup walks up to the folder */
    if (!vx_lookup_layer(probe, layer, 600) && vxp_recommend(g_path, layer, 600) | (layer[0] != 0))
        status(L"Recommended settings for this program are selected. Click Apply.");
    vx_parse_layer(layer, &l);
    /* 0: the global setting, 1: the real version, then the presets, then
     * (only if this program has one) its own version given by number */
    {
        HWND cb = item(IDC_VER);
        while (SendMessageW(cb, CB_GETCOUNT, 0, 0) > vx_osver_count + 2) SendMessageW(cb, CB_DELETESTRING, vx_osver_count + 2, 0);
        g_custom[0] = 0;
        if (l.real) sel = 1;
        for (i = 0; i < vx_osver_count; i++) if (l.ver == &vx_osvers[i]) sel = i + 2;
        if (l.ver == &l.custom) {
            WCHAR t[80];
            vx_wcscpyn(g_custom, l.customtok, 40);
            wsprintfW(t, L"Custom: %u.%u.%u", l.custom.major, l.custom.minor, l.custom.build);
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
            sel = vx_osver_count + 2;
        }
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
    }
    CheckDlgButton(g_wnd, IDC_WV2, l.webview2 ? BST_CHECKED : BST_UNCHECKED);
    {
        /* split off the Chromium preset so it shows as the checkbox */
        WCHAR *a = l.args;
        SIZE_T cl = vx_wcslen(CHROMIUM_ARGS);
        BOOL chromium = !vx_wcsnicmp(a, CHROMIUM_ARGS, cl) && (a[cl] == 0 || a[cl] == ' ');
        CheckDlgButton(g_wnd, IDC_CHROMIUM, chromium ? BST_CHECKED : BST_UNCHECKED);
        if (chromium) { a += cl; while (*a == ' ') a++; }
        SetWindowTextW(item(IDC_ARGS), a);
    }
}

static void read_controls(WCHAR *layer, SIZE_T cch)
{
    WCHAR args[512], all[600];
    int sel = (int)SendMessageW(item(IDC_VER), CB_GETCURSEL, 0, 0);
    GetWindowTextW(item(IDC_ARGS), args, 512);
    all[0] = 0;
    if (IsDlgButtonChecked(g_wnd, IDC_CHROMIUM)) vx_wcscatn(all, CHROMIUM_ARGS, 600);
    if (args[0]) { if (all[0]) vx_wcscatn(all, L" ", 600); vx_wcscatn(all, args, 600); }
    {
        vx_osver cv;
        WCHAR tok[40], csd[24];
        const vx_osver *v = NULL;
        if (sel == 1) v = &vx_osver_real;
        else if (sel >= 2 && sel < vx_osver_count + 2) v = &vx_osvers[sel - 2];
        else if (sel == vx_osver_count + 2 && g_custom[0] && vx_parse_custom(g_custom, &cv, tok, 40, csd, 24)) v = &cv;
        vx_make_layer(layer, cch, v, IsDlgButtonChecked(g_wnd, IDC_WV2), all);
    }
}

static BOOL take_path(void)
{
    WCHAR raw[MAX_PATH];
    DWORD a;
    SIZE_T n;
    GetWindowTextW(item(IDC_PATH), raw, MAX_PATH);
    n = vx_wcslen(raw);
    if (n > 1 && raw[0] == '"' && raw[n - 1] == '"') { raw[n - 1] = 0; memmove(raw, raw + 1, n * sizeof(WCHAR)); }
    if (!raw[0] || !GetFullPathNameW(raw, MAX_PATH, g_path, NULL) ||
        (a = GetFileAttributesW(g_path)) == INVALID_FILE_ATTRIBUTES) {
        status(L"Choose a program (.exe) or a folder first.");
        return FALSE;
    }
    g_isdir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    n = vx_wcslen(g_path);
    if (g_isdir && n > 3 && g_path[n - 1] == '\\') g_path[n - 1] = 0;
    EnableWindow(item(IDC_RUN), !g_isdir);
    return TRUE;
}

static void summarize(const vxp_stats *st, BOOL dry)
{
    WCHAR s[300];
    if (st->failed)
        wsprintfW(s, L"%d file(s) could not be changed. Close the program and try again.", st->failed);
    else if (st->unsupported)
        wsprintfW(s, L"%d function(s) are still missing after VistaExt. The program may not start, "
                     L"or a feature may not work (details below).", st->unsupported);
    else if (st->patched)
        wsprintfW(s, dry ? L"Ready: %d file(s) will be adjusted when you click Apply."
                         : L"Done: %d file(s) adjusted. The program should now run.", st->patched);
    else
        wsprintfW(s, dry ? L"No changes needed for this Windows version." : L"Settings saved. No files needed changes.");
    status(s);
}

/* ---- work on a worker thread ----------------------------------------------
 * Checking or patching a large program (Chromium-based apps have 100+ MB
 * DLLs) takes a while; the window stays responsive meanwhile, and its
 * controls are disabled so only one job runs at a time. */
enum { OP_CHECK, OP_APPLY, OP_UNDO };
typedef struct {
    int op;
    BOOL isdir;
    WCHAR path[MAX_PATH], key[MAX_PATH], layer[600];
    vxp_stats st;
    int restored, failed;
} work;

static DWORD WINAPI worker(LPVOID p)
{
    work *w = p;
    LONG r;
    switch (w->op) {
    case OP_CHECK:
        if (w->isdir) vxp_process_dir(w->path, VXP_DRYRUN, log_line, NULL, &w->st);
        else vxp_process_file(w->path, VXP_DRYRUN, log_line, NULL, &w->st);
        break;
    case OP_APPLY: {
        DWORD flags = vxp_flags_for_layer(w->layer);  /* settings need VistaExt loaded */
        flags |= vxp_mode_for(w->path);
        if ((flags & VXP_MEMORY) && !w->isdir && vxp_folder_needs(w->path)) flags |= VXP_REGALL;
        if (w->isdir) vxp_process_dir(w->path, flags, log_line, NULL, &w->st);
        else vxp_process_file(w->path, flags, log_line, NULL, &w->st);
        if ((r = vx_set_layer(w->key, w->layer))) {
            WCHAR m[120];
            wsprintfW(m, L"Could not save the settings (error %d).", (int)r);
            log_line(NULL, m);
            w->st.failed++;
        } else if (w->layer[0]) {
            WCHAR m[700];
            wsprintfW(m, L"Settings saved: %s", w->layer);
            log_line(NULL, m);
        }
        break;
    }
    case OP_UNDO:
        if (w->isdir) w->restored = vxp_restore_dir(w->path, log_line, NULL, &w->failed);
        else if ((w->restored = vxp_restore_file(w->path, log_line, NULL)) < 0) { w->restored = 0; w->failed = 1; }
        vx_set_layer(w->key, L"OFF");     /* automatic mode: leave it alone from now on */
        break;
    }
    PostMessageW(g_wnd, WM_VX_DONE, 0, (LPARAM)w);
    return 0;
}

static void set_busy(BOOL on)
{
    static const int ids[] = { IDC_PATH, IDC_BROWSE, IDC_BROWSEDIR, IDC_VER, IDC_WV2, IDC_CHROMIUM, IDC_ARGS,
                               IDC_CHECK, IDC_APPLY, IDC_UNDO, IDC_RUN, IDC_ONOFF };
    unsigned i;
    g_busy = on;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) EnableWindow(item(ids[i]), !on);
    if (!on) EnableWindow(item(IDC_RUN), !g_isdir);
}

static void start_work(int op)
{
    work *w;
    HANDLE t;
    if (g_busy || !take_path()) return;
    if (!(w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w)))) return;
    w->op = op;
    w->isdir = g_isdir;
    vx_wcscpyn(w->path, g_path, MAX_PATH);
    settings_key_path(w->key);
    if (op == OP_APPLY) read_controls(w->layer, 600);
    log_clear();
    status(op == OP_CHECK ? L"Checking..." : op == OP_APPLY ? L"Making the changes..." : L"Putting the original files back...");
    set_busy(TRUE);
    if (!(t = CreateThread(NULL, 0, worker, w, 0, NULL))) {
        set_busy(FALSE);
        HeapFree(GetProcessHeap(), 0, w);
        status(L"Could not start the job.");
        return;
    }
    CloseHandle(t);
}

static void work_done(work *w)
{
    WCHAR m[160];
    set_busy(FALSE);
    switch (w->op) {
    case OP_CHECK: summarize(&w->st, TRUE); break;
    case OP_APPLY: summarize(&w->st, FALSE); break;
    case OP_UNDO:
        if (w->failed) wsprintfW(m, L"%d file(s) restored, %d could not be: close the program and click Undo again.", w->restored, w->failed);
        else wsprintfW(m, L"Undone: %d file(s) restored and the settings removed.", w->restored);
        status(m);
        load_settings();
        break;
    }
    HeapFree(GetProcessHeap(), 0, w);
}

static void run_check(void) { start_work(OP_CHECK); }
static void run_apply(void) { start_work(OP_APPLY); }
static void run_undo(void) { start_work(OP_UNDO); }

/* ---- starting the program as the normal user ------------------------------
 * VistaExt Center runs as administrator, and a program it started directly
 * would too (no sandbox in browsers, no drag and drop from Explorer). Ask
 * Explorer, which runs as the user, to start it instead. */

static void run_program(void)
{
    WCHAR dir[MAX_PATH];
    SIZE_T n;
    if (g_busy || !take_path() || g_isdir) return;
    vx_wcscpyn(dir, g_path, MAX_PATH);
    n = vx_wcslen(dir);
    while (n && dir[n - 1] != '\\') n--;
    if (n > 3) n--;                                   /* drop the backslash, but keep "C:\\" */
    dir[n] = 0;
    if (!vx_run_as_user(g_path, NULL, dir)) ShellExecuteW(g_wnd, NULL, g_path, NULL, dir, SW_SHOWNORMAL);
}

static void browse_file(void)
{
    OPENFILENAMEW ofn;
    WCHAR f[MAX_PATH] = L"";
    vx_memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = f;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(item(IDC_PATH), f);
        if (take_path()) { load_settings(); run_check(); }
    }
}

static void browse_dir(void)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    WCHAR f[MAX_PATH];
    vx_memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Choose the folder of the program. Every program in it gets the same settings.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if ((pidl = SHBrowseForFolderW(&bi))) {
        if (SHGetPathFromIDListW(pidl, f)) {
            SetWindowTextW(item(IDC_PATH), f);
            if (take_path()) { load_settings(); run_check(); }
        }
        CoTaskMemFree(pidl);
    }
}

/* ---- VistaExt on / off --------------------------------------------------------
 * The same switch as the tray icon's: asks VistaExt's service, and where it
 * is not running (Safe Mode, automatic mode not installed) runs
 * "vxauto.exe /disable" or "/enable" itself (this window already runs as
 * administrator). On a worker thread: switching moves files and puts patched
 * programs back, which takes a few seconds. */
static void tool_path(const WCHAR *exe, WCHAR *out)
{
    SIZE_T n;
    GetModuleFileNameW(NULL, out, MAX_PATH);
    n = vx_wcslen(out);
    while (n && out[n - 1] != '\\') n--;
    out[n] = 0;
    vx_wcscatn(out, exe, MAX_PATH);
}

static void show_onoff(void)
{
    BOOL on = vx_setting(L"Enabled", 1) != 0;
    BOOL rs = vx_restart_state() > 0;
    WCHAR t[200];
    wsprintfW(t, on ? L"VistaExt is ON: programs made for newer Windows can run on this PC.%s"
                    : L"VistaExt is OFF: Windows works exactly as it does without VistaExt.%s",
              rs ? L" Restart Windows to finish the last change." : L"");
    SetWindowTextW(item(IDC_ONTEXT), t);
    SetWindowTextW(item(IDC_ONOFF), on ? L"Turn off" : L"Turn on");
}

static DWORD WINAPI switch_worker(LPVOID p)
{
    BOOL on = p != NULL, ok = FALSE, restart = FALSE;
    WCHAR rep[512], exe[MAX_PATH], cmd[MAX_PATH + 32];
    const WCHAR *req = on ? L"ENABLE" : L"DISABLE";
    DWORD got = 0;
    if (CallNamedPipeW(L"\\\\.\\pipe\\VistaExt", (void *)req, (DWORD)((vx_wcslen(req) + 1) * sizeof(WCHAR)), rep,
                       sizeof(rep) - sizeof(WCHAR), &got, 120000)) {
        rep[got / sizeof(WCHAR)] = 0;
        ok = vx_wcsnicmp(rep, L"error=", 6) != 0;
    }
    if (!ok) {                                 /* no service: do it here */
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        tool_path(L"vxauto.exe", exe);
        wsprintfW(cmd, L"\"%s\" %s", exe, on ? L"/enable" : L"/disable");
        vx_memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 300000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        ok = code == 0 || code == 3;          /* 3: done, Windows must restart to finish */
        restart = code == 3;
    }
    PostMessageW(g_wnd, WM_VX_SWITCHED, ok, restart);
    return 0;
}

static void run_switch(void)
{
    BOOL on = vx_setting(L"Enabled", 1) != 0;
    HANDLE t;
    if (g_busy) return;
    if (on && MessageBoxW(g_wnd, L"Turn VistaExt off?\n\nWindows then works exactly as it does without VistaExt: "
                                 L"programs that need it will not start until you turn it on again. Your settings "
                                 L"are kept.", L"VistaExt", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    set_busy(TRUE);
    status(on ? L"Turning VistaExt off..." : L"Turning VistaExt on...");
    if (!(t = CreateThread(NULL, 0, switch_worker, on ? NULL : (LPVOID)1, 0, NULL))) {
        set_busy(FALSE);
        status(L"Could not start the switch.");
        return;
    }
    CloseHandle(t);
}

static void switched(BOOL ok, BOOL restart)
{
    BOOL on = vx_setting(L"Enabled", 1) != 0;
    set_busy(FALSE);
    show_onoff();
    if (!ok) status(L"The switch did not finish. See the log (tray icon > View log).");
    else if (restart || vx_restart_state() > 0) status(L"Done. Some files were in use: restart Windows to finish.");
    else status(on ? L"VistaExt is on." : L"VistaExt is off. Programs that are running keep it until they are closed.");
}

/* the tray icon belongs to the signed-in user: started through Explorer, not
 * as administrator like this window */
static void ensure_tray(void)
{
    WCHAR exe[MAX_PATH];
    if (FindWindowW(L"VistaExtTray", NULL)) return;
    tool_path(L"vxtray.exe", exe);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
    vx_run_as_user(exe, NULL, NULL);
}

/* ---- window -------------------------------------------------------------- */
static HWND add(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void build_ui(void)
{
    int i, y = 12;
    HWND h;
    add(L"BUTTON", L"VistaExt for the whole PC", BS_GROUPBOX, 12, y, 564, 52, -1, 0);
    add(L"STATIC", L"", SS_LEFT, 24, y + 20, 440, 28, IDC_ONTEXT, 0);
    add(L"BUTTON", L"", WS_TABSTOP, 476, y + 17, 90, 26, IDC_ONOFF, 0);
    show_onoff();
    y += 62;
    h = add(L"STATIC", L"Make a program made for newer Windows run on this PC.", 0, 12, y, 560, 20, -1, 0);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_bold, TRUE);
    y += 30;
    add(L"STATIC", L"Program or folder:", 0, 12, y + 3, 120, 18, -1, 0);
    add(L"EDIT", g_path, WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 300, 23, IDC_PATH, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Program...", WS_TABSTOP, 436, y - 1, 70, 25, IDC_BROWSE, 0);
    add(L"BUTTON", L"Folder...", WS_TABSTOP, 510, y - 1, 66, 25, IDC_BROWSEDIR, 0);
    y += 36;
    add(L"BUTTON", L"Compatibility settings", BS_GROUPBOX, 12, y, 564, 150, -1, 0);
    y += 24;
    add(L"STATIC", L"Tell it it runs on:", 0, 24, y + 3, 110, 18, -1, 0);
    h = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 140, y, 260, 200, IDC_VER, 0);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"The global setting (VistaExt's tray icon)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"This Windows' own version (no change)");
    for (i = 0; i < vx_osver_count; i++) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)vx_osvers[i].label);
    SendMessageW(h, CB_SETCURSEL, 0, 0);
    y += 30;
    add(L"BUTTON", L"Chromium or Electron program (browsers, Discord-style apps): turn off sandbox and GPU",
        WS_TABSTOP | BS_AUTOCHECKBOX, 24, y, 540, 20, IDC_CHROMIUM, 0);
    y += 24;
    add(L"BUTTON", L"Uses Microsoft WebView2 (needs the runtime set up with vx-webview2-setup)",
        WS_TABSTOP | BS_AUTOCHECKBOX, 24, y, 540, 20, IDC_WV2, 0);
    y += 28;
    add(L"STATIC", L"Extra start-up options:", 0, 24, y + 3, 130, 18, -1, 0);
    add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 160, y, 404, 23, IDC_ARGS, WS_EX_CLIENTEDGE);
    y += 46;
    add(L"BUTTON", L"Check", WS_TABSTOP, 12, y, 90, 28, IDC_CHECK, 0);
    add(L"BUTTON", L"Apply", WS_TABSTOP | BS_DEFPUSHBUTTON, 108, y, 90, 28, IDC_APPLY, 0);
    add(L"BUTTON", L"Undo all changes", WS_TABSTOP, 204, y, 120, 28, IDC_UNDO, 0);
    add(L"BUTTON", L"Run program", WS_TABSTOP, 376, y, 100, 28, IDC_RUN, 0);
    add(L"BUTTON", L"Close", WS_TABSTOP, 482, y, 94, 28, IDC_CLOSE, 0);
    y += 38;
    add(L"STATIC", L"Choose a program, then click Check to see what it needs.", SS_LEFT, 12, y, 564, 36, IDC_STATUS, 0);
    y += 40;
    g_log = add(L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                12, y, 564, 138, IDC_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);   /* no 32K cut-off for big folders */
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE: browse_file(); return 0;
        case IDC_BROWSEDIR: browse_dir(); return 0;
        case IDC_CHECK: run_check(); return 0;
        case IDC_APPLY: run_apply(); return 0;
        case IDC_UNDO:
            if (!g_busy && MessageBoxW(h, L"Put back the original files and remove the VistaExt settings?", L"VistaExt",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) run_undo();
            return 0;
        case IDC_RUN: run_program(); return 0;
        case IDC_ONOFF: run_switch(); return 0;
        case IDC_CLOSE: case IDCANCEL: SendMessageW(h, WM_CLOSE, 0, 0); return 0;
        case IDOK: run_apply(); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        break;
    case WM_VX_DONE: work_done((work *)lp); return 0;
    case WM_VX_SWITCHED: switched((BOOL)wp, (BOOL)lp); return 0;
    case WM_CLOSE:
        /* closing now would stop the worker in the middle of writing a file */
        if (g_busy) { status(L"Please wait: VistaExt is still working on the files."); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- Explorer integration ------------------------------------------------ */
/* Start menu (all users): a folder "VistaExt" with two entries - VistaExt
 * (this window: the on/off switch, also in Safe Mode, and the settings; it
 * starts the tray icon) and its uninstaller. Earlier versions put their
 * shortcuts straight into Programs: those are removed. */
static const WCHAR *g_old_links[] = {
    L"VistaExt Center.lnk", L"VistaExt tray icon.lnk", L"Turn off VistaExt (also in Safe Mode).lnk",
};
#define SM_FOLDER L"VistaExt"

static void link_path(const WCHAR *sub, const WCHAR *name, WCHAR *out)
{
    out[0] = 0;
    SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, out);
    if (sub) { vx_wcscatn(out, L"\\", MAX_PATH); vx_wcscatn(out, sub, MAX_PATH); }
    if (name) { vx_wcscatn(out, L"\\", MAX_PATH); vx_wcscatn(out, name, MAX_PATH); }
}

static void make_link(const WCHAR *lnk, const WCHAR *target, const WCHAR *args, const WCHAR *desc, const WCHAR *icon)
{
    IShellLinkW *sl;
    IPersistFile *pf;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetArguments(sl, args);
    sl->lpVtbl->SetDescription(sl, desc);
    if (icon) sl->lpVtbl->SetIconLocation(sl, icon, 0);
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        pf->lpVtbl->Save(pf, lnk, TRUE);
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
}

/* the Explorer entries and .msi double-click (patchlib, shared with the
 * on/off switch: added only while VistaExt is on, so an update over a
 * switched-off VistaExt leaves it off), and the Start menu folder */
static int do_register(BOOL on)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], lnk[MAX_PATH], target[MAX_PATH], sys[MAX_PATH], args[MAX_PATH + 40];
    SIZE_T n;
    unsigned i;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    vx_wcscpyn(dir, exe, MAX_PATH);
    n = vx_wcslen(dir);
    while (n && dir[n - 1] != '\\') n--;
    dir[n] = 0;
    vxp_shell_entries(on && vx_setting(L"Enabled", 1) != 0, dir);
    for (i = 0; i < sizeof(g_old_links) / sizeof(g_old_links[0]); i++) {
        link_path(NULL, g_old_links[i], lnk);
        DeleteFileW(lnk);
    }
    link_path(SM_FOLDER, L"VistaExt.lnk", lnk);
    DeleteFileW(lnk);
    link_path(SM_FOLDER, L"Uninstall VistaExt.lnk", lnk);
    DeleteFileW(lnk);
    link_path(SM_FOLDER, NULL, lnk);
    if (!on) { RemoveDirectoryW(lnk); return 0; }
    CreateDirectoryW(lnk, NULL);
    link_path(SM_FOLDER, L"VistaExt.lnk", lnk);
    make_link(lnk, exe, L"", L"Turn VistaExt on or off, and set up programs made for newer Windows", NULL);
    /* the uninstaller: VistaExtSetup.exe (it asks for administrator rights
     * itself), or uninstall.cmd where VistaExt was installed without it */
    vx_wcscpyn(target, dir, MAX_PATH);
    vx_wcscatn(target, L"VistaExtSetup.exe", MAX_PATH);
    link_path(SM_FOLDER, L"Uninstall VistaExt.lnk", lnk);
    if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES)
        make_link(lnk, target, L"/uninstall", L"Remove VistaExt from this PC", target);
    else {
        vx_wcscpyn(target, dir, MAX_PATH);
        vx_wcscatn(target, L"uninstall.cmd", MAX_PATH);
        GetSystemDirectoryW(sys, MAX_PATH);
        vx_wcscatn(sys, L"\\cmd.exe", MAX_PATH);
        wsprintfW(args, L"/c \"\"%s\"\"", target);
        if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
            IShellLinkW *sl;
            make_link(lnk, sys, args, L"Remove VistaExt from this PC (run as administrator)", exe);
            /* uninstall.cmd needs administrator rights: "Run as administrator" */
            if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
                IPersistFile *pf;
                IShellLinkDataList *dl;
                if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
                    if (SUCCEEDED(pf->lpVtbl->Load(pf, lnk, STGM_READWRITE)) &&
                        SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IShellLinkDataList, (void **)&dl))) {
                        DWORD f = 0;
                        dl->lpVtbl->GetFlags(dl, &f);
                        dl->lpVtbl->SetFlags(dl, f | SLDF_RUNAS_USER);
                        dl->lpVtbl->Release(dl);
                        pf->lpVtbl->Save(pf, lnk, TRUE);
                    }
                    pf->lpVtbl->Release(pf);
                }
                sl->lpVtbl->Release(sl);
            }
        }
    }
    return 0;
}

/* "Open with VistaExt compatibility mode" (Explorer's right-click menu, on a
 * program or a shortcut to one): the program's saved settings or the
 * recommended ones, patch or register it, then start it - as the signed-in
 * user, through Explorer, like a double-click (this runs as administrator;
 * a program that needs administrator rights asks for them itself). A
 * shortcut is followed: its program, arguments and folder. */
static BOOL read_shortcut(const WCHAR *lnk, WCHAR *target, WCHAR *args, int cargs, WCHAR *dir)
{
    IShellLinkW *sl;
    IPersistFile *pf;
    BOOL ok = FALSE;
    target[0] = args[0] = dir[0] = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return FALSE;
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SUCCEEDED(pf->lpVtbl->Load(pf, lnk, STGM_READ)) &&
            sl->lpVtbl->GetPath(sl, target, MAX_PATH, NULL, 0) == S_OK && target[0]) {
            sl->lpVtbl->GetArguments(sl, args, cargs);
            sl->lpVtbl->GetWorkingDirectory(sl, dir, MAX_PATH);
            ok = TRUE;
        }
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

static BOOL ends_with_i(const WCHAR *s, const WCHAR *end)
{
    SIZE_T n = vx_wcslen(s), e = vx_wcslen(end);
    return n >= e && !vx_wcsicmp(s + n - e, end);
}

static int do_run(const WCHAR *file)
{
    WCHAR layer[600], full[MAX_PATH], dir[MAX_PATH], m[MAX_PATH + 200], args[1024];
    vxp_stats st = { 0 };
    DWORD flags = 0;
    SIZE_T n;
    if (!GetFullPathNameW(file, MAX_PATH, full, NULL)) return 2;
    args[0] = dir[0] = 0;
    if (ends_with_i(full, L".lnk")) {
        WCHAR lnk[MAX_PATH];
        vx_wcscpyn(lnk, full, MAX_PATH);
        if (!read_shortcut(lnk, full, args, 1024, dir)) {
            wsprintfW(m, L"%s\n\nThis shortcut does not lead to a file on this PC.", lnk);
            MessageBoxW(NULL, m, L"VistaExt", MB_ICONWARNING);
            return 2;
        }
    }
    if (!ends_with_i(full, L".exe") || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        wsprintfW(m, L"%s\n\nThis is not a program. VistaExt's compatibility mode is for programs (.exe).", full);
        MessageBoxW(NULL, m, L"VistaExt", MB_ICONINFORMATION);
        return 2;
    }
    if (!vx_lookup_layer(full, layer, 600)) vxp_recommend(full, layer, 600);
    flags |= vxp_flags_for_layer(layer);
    flags |= vxp_mode_for(full);          /* in memory: works for setups on a CD too */
    if ((flags & VXP_MEMORY) && vxp_folder_needs(full)) flags |= VXP_REGALL;
    if (vxp_process_file(full, flags | VXP_QUIET, NULL, NULL, &st) < 0) {
        wsprintfW(m, L"%s could not be changed (is it running, or on a CD?). It is started as it is.", full);
        MessageBoxW(NULL, m, L"VistaExt", MB_ICONWARNING);
    } else if (layer[0]) vx_set_layer(full, layer);
    if (!dir[0]) {
        vx_wcscpyn(dir, full, MAX_PATH);
        n = vx_wcslen(dir);
        while (n && dir[n - 1] != '\\') n--;
        if (n > 3) n--;
        dir[n] = 0;
    }
    if (vx_run_as_user(full, args, dir)) return 0;
    /* no Explorer to ask (another shell): started from here */
    return (INT_PTR)ShellExecuteW(NULL, NULL, full, args[0] ? args : NULL, dir, SW_SHOWNORMAL) > 32 ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    NONCLIENTMETRICSW ncm;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    MSG m;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HDC dc;
    RECT rc;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/register")) return do_register(TRUE);
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/unregister")) return do_register(FALSE);
    if (argc > 2 && !vx_wcsicmp(argv[1], L"/run")) return do_run(argv[2]);
    if (argc > 1) vx_wcscpyn(g_path, argv[argc - 1], MAX_PATH);

    InitCommonControlsEx(&icc);
    dc = GetDC(NULL);
    g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    ncm.cbSize = sizeof(ncm) - sizeof(int);   /* Vista-sized structure */
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    vx_memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"VistaExtCenter";
    RegisterClassExW(&wc);

    rc.left = rc.top = 0; rc.right = S(588); rc.bottom = S(520);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"VistaExt",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, inst, NULL);
    build_ui();
    ensure_tray();
    if (g_path[0] && take_path()) { load_settings(); run_check(); }
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return 0;
}

void WinMainCRTStartup(void)
{
    STARTUPINFOW si;
    si.cb = sizeof(si);
    GetStartupInfoW(&si);
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(),
                               si.dwFlags & STARTF_USESHOWWINDOW ? si.wShowWindow : SW_SHOWDEFAULT));
}

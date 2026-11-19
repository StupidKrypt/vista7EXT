/* VistaExtSetup.exe - installs or removes VistaExt.
 *
 *   VistaExtSetup.exe              the setup wizard:
 *                                  1. a warning (Accept / Decline: Decline
 *                                     changes nothing),
 *                                  2. backup: a System Restore point (checked
 *                                     by default) and a way to Windows Backup,
 *                                  3. install: the build for this Windows and
 *                                     architecture, the service, the tray
 *                                     icon and its autostart,
 *                                  4. finish: start the tray icon now.
 *   VistaExtSetup.exe /uninstall   remove (Programs and Features runs this)
 *   /quiet                         no screens, no questions (deployment, tests);
 *                                  /restorepoint makes the restore point there too
 *   /noauto                        install without the automatic mode
 *
 * The release folder is packed at the end of this file (tools/mkpayload.py:
 * entries "u16 path length, UTF-16 path, u32 size, data", then a trailer
 * "VXPAYLD2" + u64 offset of the first entry). Installing unpacks it to a
 * temporary folder and runs its install.cmd /quiet, hidden; its output is
 * kept as %ProgramData%\VistaExt\setup.log. Nothing is unpacked or changed
 * before the user has accepted the warning and chosen the backup. */
#include "../common/vx.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shlguid.h>

static BOOL g_quiet;
static HWND g_wnd;
static WCHAR g_log[MAX_PATH];
static const WCHAR *TITLE = L"VistaExt Setup";

static void msg(const WCHAR *text, UINT icon)
{
    if (!g_quiet) MessageBoxW(g_wnd, text, TITLE, icon | MB_SETFOREGROUND);
}

/* ---- a small "please wait" window (uninstall) ------------------------------------ */
static void show_progress(const WCHAR *text)
{
    HWND st;
    if (g_quiet) return;
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"STATIC", TITLE, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                            (GetSystemMetrics(SM_CXSCREEN) - 420) / 2, (GetSystemMetrics(SM_CYSCREEN) - 110) / 2,
                            420, 110, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_wnd) return;
    st = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 25, 390, 40, g_wnd, NULL, NULL, NULL);
    SendMessageW(st, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    UpdateWindow(g_wnd);
}

/* wait for a process while keeping the window responsive */
static DWORD wait_pumping(HANDLE h)
{
    DWORD code = 1;
    MSG m;
    while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0)
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    GetExitCodeProcess(h, &code);
    return code;
}

/* run "cmd /c <script> <args>" hidden, output to g_log */
static DWORD run_script(const WCHAR *script, const WCHAR *args)
{
    WCHAR cmd[MAX_PATH * 3], sys[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out = CreateFileW(g_log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL), nul;
    DWORD code;
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    wsprintfW(cmd, L"\"%s\\cmd.exe\" /c \"\"%s\" %s\"", sys, script, args);
    vx_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = out;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return 9009;
    }
    CloseHandle(pi.hThread);
    code = wait_pumping(pi.hProcess);
    CloseHandle(pi.hProcess);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return code;
}

/* the first log line containing <what>, as text */
static BOOL log_line(const char *what, WCHAR *out, int cch)
{
    HANDLE h = CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char buf[16384];
    DWORD got = 0, i, wl = (DWORD)vx_strlen(what);
    BOOL ok = FALSE;
    out[0] = 0;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    for (i = 0; i + wl <= got && !ok; i++) {
        if (memcmp(buf + i, what, wl)) continue;
        {
            DWORD s = i, e = i;
            while (s && buf[s - 1] != '\n') s--;
            while (e < got && buf[e] != '\r' && buf[e] != '\n') e++;
            while (s < e && buf[s] == ' ') s++;
            buf[e] = 0;
            MultiByteToWideChar(CP_OEMCP, 0, buf + s, -1, out, cch);
            ok = TRUE;
        }
    }
    return ok;
}

/* ---- payload ------------------------------------------------------------------ */
static BOOL make_dirs(WCHAR *path)          /* creates the folders of a file path */
{
    WCHAR *p;
    for (p = path + 3; *p; p++)
        if (*p == '\\') { *p = 0; CreateDirectoryW(path, NULL); *p = '\\'; }
    return TRUE;
}

static BOOL extract(const WCHAR *dest)
{
    WCHAR self[MAX_PATH], path[MAX_PATH * 2];
    HANDLE f, o;
    LARGE_INTEGER sz, pos;
    char tail[16];
    ULONGLONG off;
    DWORD got;
    BYTE *buf = HeapAlloc(GetProcessHeap(), 0, 1 << 20);
    BOOL ok = FALSE;
    if (!buf) return FALSE;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) goto done;
    GetFileSizeEx(f, &sz);
    pos.QuadPart = sz.QuadPart - 16;
    SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
    if (!ReadFile(f, tail, 16, &got, NULL) || got != 16 || memcmp(tail, "VXPAYLD2", 8)) goto done;
    off = *(ULONGLONG *)(tail + 8);
    pos.QuadPart = (LONGLONG)off;
    SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
    for (;;) {
        WORD pl;
        DWORD size, left;
        WCHAR rel[MAX_PATH];
        if (!ReadFile(f, &pl, 2, &got, NULL) || got != 2) goto done;
        if (!pl) break;
        if (pl >= MAX_PATH || !ReadFile(f, rel, pl * 2, &got, NULL) || got != pl * 2u) goto done;
        rel[pl] = 0;
        if (!ReadFile(f, &size, 4, &got, NULL) || got != 4) goto done;
        wsprintfW(path, L"%s\\%s", dest, rel);
        make_dirs(path);
        if (size == 0xFFFFFFFF) {           /* the same file as an earlier one: copy that */
            WCHAR same[MAX_PATH], from[MAX_PATH * 2];
            if (!ReadFile(f, &pl, 2, &got, NULL) || got != 2 || !pl || pl >= MAX_PATH ||
                !ReadFile(f, same, pl * 2, &got, NULL) || got != pl * 2u) goto done;
            same[pl] = 0;
            wsprintfW(from, L"%s\\%s", dest, same);
            if (!CopyFileW(from, path, FALSE)) goto done;
            continue;
        }
        o = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (o == INVALID_HANDLE_VALUE) goto done;
        for (left = size; left; ) {
            DWORD n = left > (1u << 20) ? (1u << 20) : left, put;
            if (!ReadFile(f, buf, n, &got, NULL) || got != n || !WriteFile(o, buf, n, &put, NULL)) { CloseHandle(o); goto done; }
            left -= n;
        }
        CloseHandle(o);
    }
    ok = TRUE;
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static void remove_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH * 2], p[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wsprintfW(pat, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
            wsprintfW(p, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* ---- checks ------------------------------------------------------------------ */
static BOOL sys_has(const WCHAR *dll)
{
    WCHAR p[MAX_PATH];
    BOOL wow = FALSE;
    /* the 64-bit System32 on 64-bit Windows (this setup is 32-bit) */
    IsWow64Process(GetCurrentProcess(), &wow);
    GetWindowsDirectoryW(p, MAX_PATH);
    vx_wcscatn(p, wow ? L"\\Sysnative\\" : L"\\System32\\", MAX_PATH);
    vx_wcscatn(p, dll, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void prerequisites(WCHAR *out, SIZE_T cch, DWORD maj, DWORD min)
{
    out[0] = 0;
    if (!sys_has(L"ucrtbase.dll"))
        vx_wcscatn(out, L"\n\n- Microsoft's Universal C Runtime update (KB2999226) is not installed. Many "
                        L"current programs (Firefox among them) need it. Get it from Windows Update or "
                        L"Microsoft's website.", cch);
    if (maj == 6 && min == 0 && (!sys_has(L"dwrite.dll") || !sys_has(L"d3d11.dll")))
        vx_wcscatn(out, L"\n\n- The Platform Update for Windows Vista (KB971644) is not installed. Programs "
                        L"built on Chromium or Electron (browsers, Discord, Epic Games, Spotify...) cannot "
                        L"start without it. Get it from Windows Update or Microsoft's website.", cch);
}

static void setup_log_path(void)
{
    WCHAR d[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, d))) GetTempPathW(MAX_PATH, d);
    vx_wcscatn(d, L"\\VistaExt", MAX_PATH);
    CreateDirectoryW(d, NULL);
    wsprintfW(g_log, L"%s\\setup.log", d);
}

/* ---- the install --------------------------------------------------------------- */
typedef struct {
    BOOL noauto;
    DWORD code;              /* install.cmd's exit code; 5: unpacking failed */
    WCHAR line[300];         /* its summary line */
} install_job;

static DWORD WINAPI install_thread(LPVOID p)
{
    install_job *j = p;
    WCHAR tmp[MAX_PATH], dir[MAX_PATH], script[MAX_PATH + 32], self[MAX_PATH], to[MAX_PATH + 32];
    setup_log_path();
    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(dir, L"%sVistaExtSetup.%u", tmp, GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    if (!extract(dir)) {
        remove_tree(dir);
        j->code = 5;
        return 0;
    }
    /* a copy of this setup goes with it, for uninstalling from Programs and Features */
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wsprintfW(to, L"%s\\VistaExtSetup.exe", dir);
    CopyFileW(self, to, FALSE);
    wsprintfW(script, L"%s\\install.cmd", dir);
    j->code = run_script(script, j->noauto ? L"/quiet /force /noauto" : L"/quiet /force");
    remove_tree(dir);
    if (j->code) log_line("Please", j->line, 300);
    else log_line("file(s) added", j->line, 300);
    return 0;
}

/* ---- System Restore ---------------------------------------------------------------- */
/* srrestoreptapi.h's structures, which are byte-packed */
#pragma pack(push, 1)
typedef struct { DWORD dwEventType, dwRestorePtType; INT64 llSequenceNumber; WCHAR szDescription[256]; } rp_info;
typedef struct { DWORD nStatus; INT64 llSequenceNumber; } rp_status;
#pragma pack(pop)
typedef BOOL (WINAPI *srset_fn)(rp_info *, rp_status *);
static INT64 g_rp_seq;
static srset_fn g_srset;

/* 0: done; otherwise a Windows error code */
static DWORD restore_point(BOOL begin, BOOL ok)
{
    rp_info ri;
    rp_status st;
    if (!g_srset) {
        HMODULE h = LoadLibraryW(L"srclient.dll");
        if (!h || !(g_srset = (srset_fn)(void *)GetProcAddress(h, "SRSetRestorePointW"))) return ERROR_SERVICE_DISABLED;
    }
    vx_memset(&ri, 0, sizeof(ri));
    vx_memset(&st, 0, sizeof(st));
    ri.dwEventType = begin ? 100 /* BEGIN_SYSTEM_CHANGE */ : 101 /* END_SYSTEM_CHANGE */;
    ri.dwRestorePtType = begin ? 0 /* APPLICATION_INSTALL */ : (ok ? 0 : 13 /* CANCELLED_OPERATION */);
    ri.llSequenceNumber = begin ? 0 : g_rp_seq;
    vx_wcscpyn(ri.szDescription, L"Before installing VistaExt", 256);
    if (!g_srset(&ri, &st)) return st.nStatus ? st.nStatus : ERROR_GEN_FAILURE;
    if (begin) g_rp_seq = st.llSequenceNumber;
    return 0;
}

static const WCHAR *rp_reason(DWORD e)
{
    switch (e) {
    case ERROR_SERVICE_DISABLED: return L"System Restore is turned off on this PC (Control Panel > System > System Protection).";
    case ERROR_DISK_FULL:        return L"There is not enough free disk space for a restore point.";
    case ERROR_ACCESS_DENIED:    return L"Windows refused (access denied).";
    case ERROR_TIMEOUT:          return L"Windows did not answer in time.";
    }
    return L"Windows reported an error.";
}

/* ---- starting the tray icon as the signed-in user (setup runs as administrator) ---- */
#undef ShellExecute          /* shellapi.h's A/W macro would rename the COM method */
static BOOL run_as_user(const WCHAR *file, const WCHAR *args)
{
    IShellWindows *sw = NULL;
    IDispatch *disp = NULL, *bg = NULL, *app = NULL;
    IServiceProvider *sp = NULL;
    IShellBrowser *sb = NULL;
    IShellView *sv = NULL;
    IShellFolderViewDual *fv = NULL;
    IShellDispatch2 *sd = NULL;
    VARIANT loc, none, vargs, vshow;
    long hwnd = 0;
    BOOL ok = FALSE;
    VariantInit(&loc); VariantInit(&none); VariantInit(&vargs); VariantInit(&vshow);
    loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, &IID_IShellWindows, (void **)&sw)) &&
        sw->lpVtbl->FindWindowSW(sw, &loc, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp &&
        SUCCEEDED(disp->lpVtbl->QueryInterface(disp, &IID_IServiceProvider, (void **)&sp)) &&
        SUCCEEDED(sp->lpVtbl->QueryService(sp, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&sb)) &&
        SUCCEEDED(sb->lpVtbl->QueryActiveShellView(sb, &sv)) &&
        SUCCEEDED(sv->lpVtbl->GetItemObject(sv, SVGIO_BACKGROUND, &IID_IDispatch, (void **)&bg)) &&
        SUCCEEDED(bg->lpVtbl->QueryInterface(bg, &IID_IShellFolderViewDual, (void **)&fv)) &&
        SUCCEEDED(fv->lpVtbl->get_Application(fv, &app)) &&
        SUCCEEDED(app->lpVtbl->QueryInterface(app, &IID_IShellDispatch2, (void **)&sd))) {
        BSTR bfile = SysAllocString(file);
        vargs.vt = VT_BSTR; vargs.bstrVal = SysAllocString(args);
        vshow.vt = VT_I4; vshow.lVal = SW_SHOWNORMAL;
        ok = bfile && vargs.bstrVal && SUCCEEDED(sd->lpVtbl->ShellExecute(sd, bfile, vargs, none, none, vshow));
        SysFreeString(bfile);
        VariantClear(&vargs);
    }
    if (sd) sd->lpVtbl->Release(sd);
    if (app) app->lpVtbl->Release(app);
    if (fv) fv->lpVtbl->Release(fv);
    if (bg) bg->lpVtbl->Release(bg);
    if (sv) sv->lpVtbl->Release(sv);
    if (sb) sb->lpVtbl->Release(sb);
    if (sp) sp->lpVtbl->Release(sp);
    if (disp) disp->lpVtbl->Release(disp);
    if (sw) sw->lpVtbl->Release(sw);
    return ok;
}

static void start_tray(void)
{
    WCHAR exe[MAX_PATH];
    GetEnvironmentVariableW(L"ProgramW6432", exe, MAX_PATH) || GetEnvironmentVariableW(L"ProgramFiles", exe, MAX_PATH);
    vx_wcscatn(exe, L"\\VistaExt\\vxtray.exe", MAX_PATH);
    if (!run_as_user(exe, L"")) ShellExecuteW(NULL, NULL, exe, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the wizard --------------------------------------------------------------------- */
enum { P_WARN, P_BACKUP, P_INSTALL, P_FINISH };
enum { ID_BACK = 10, ID_NEXT, ID_CANCEL, ID_RP, ID_BACKUP, ID_TRAY, ID_PROGRESS };
#define WM_DONE (WM_APP + 1)
static HWND g_title, g_text, g_next, g_back, g_cancel, g_rp, g_bk, g_tray, g_prog;
static HFONT g_font, g_big;
static int g_page;
static install_job g_job;
static BOOL g_noauto, g_rp_made;
static DWORD g_maj, g_min;
static int g_result = 4;      /* exit code: 4 = declined/cancelled */

static void prerequisites(WCHAR *out, SIZE_T cch, DWORD maj, DWORD min);

static void page(int p)
{
    WCHAR t[2600], pre[1200];
    BOOL wow = FALSE;
    g_page = p;
    IsWow64Process(GetCurrentProcess(), &wow);
    ShowWindow(g_rp, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_bk, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tray, p == P_FINISH && !g_job.code ? SW_SHOW : SW_HIDE);
    ShowWindow(g_prog, p == P_INSTALL ? SW_SHOW : SW_HIDE);
    ShowWindow(g_back, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_cancel, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_next, p == P_INSTALL ? SW_HIDE : SW_SHOW);
    switch (p) {
    case P_WARN:
        SetWindowTextW(g_title, L"Before you install: VistaExt is experimental");
        SetWindowTextW(g_text,
            L"VistaExt is still in development. It adds files to Windows (it never replaces Windows' own) and "
            L"changes how programs made for newer Windows start on this PC.\n\n"
            L"It could damage your Windows installation: programs may behave wrongly or lose data, and in the worst "
            L"case Windows may need to be repaired or reinstalled. Use it at your own risk, on a PC whose important "
            L"files are backed up.\n\n"
            L"VistaExt is free software and comes with no warranty (GNU General Public License 2).\n\n"
            L"Do you accept this risk?");
        SetWindowTextW(g_next, L"&Accept");
        ShowWindow(g_cancel, SW_SHOW);
        SetWindowTextW(g_cancel, L"&Decline");
        SetFocus(g_cancel);
        break;
    case P_BACKUP:
        SetWindowTextW(g_title, L"Back up first");
        SetWindowTextW(g_text,
            L"A System Restore point lets you put Windows' system files and settings back to how they are now, "
            L"if something goes wrong (Control Panel > Recovery, or from the Windows DVD's repair options).\n\n"
            L"A restore point does not save your own files. For a full copy of the PC (a system image) or of your "
            L"files, use Windows Backup.");
        SetWindowTextW(g_next, L"&Install");
        SetWindowTextW(g_cancel, L"Cancel");
        SetFocus(g_next);
        break;
    case P_INSTALL:
        SetWindowTextW(g_title, L"Installing VistaExt");
        wsprintfW(t, L"Installing the VistaExt build for Windows %s (%s), the background service and the tray icon.\n\n"
                     L"This takes a minute.", g_min == 0 ? L"Vista" : L"7", wow || sizeof(void *) == 8 ? L"64-bit" : L"32-bit");
        SetWindowTextW(g_text, t);
        break;
    case P_FINISH:
        prerequisites(pre, 1200, g_maj, g_min);
        if (g_job.code) {
            SetWindowTextW(g_title, L"VistaExt was not installed");
            wsprintfW(t, L"%s\n\n%s\n\nDetails: %s%s", g_job.code == 5 ? L"The setup file is damaged (its contents could not "
                          L"be unpacked). Download it again." : L"Setup stopped with an error. Windows was not changed "
                          L"beyond the files listed in the details.", g_job.line, g_log,
                      g_rp_made ? L"\n\nThe restore point made before installing is kept." : L"");
        } else {
            SetWindowTextW(g_title, L"VistaExt is installed");
            wsprintfW(t, L"%s\n\nPrograms made for Windows 8, 10 and 11 that you install or download from now on are made "
                         L"compatible by themselves: double-click them as usual. VistaExt's icon in the notification area "
                         L"(next to the clock) turns it on or off and sets the Windows version programs are told.%s",
                      g_job.line, pre[0] ? L"\n\nAlso recommended:" : L"");
            vx_wcscatn(t, pre, 2600);
        }
        SetWindowTextW(g_text, t);
        SetWindowTextW(g_next, L"&Finish");
        SetFocus(g_next);
        break;
    }
}

static void begin_install(void)
{
    HANDLE th;
    if (IsDlgButtonChecked(g_wnd, ID_RP) == BST_CHECKED) {
        DWORD e;
        HCURSOR c = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        SetWindowTextW(g_title, L"Creating a restore point...");
        SetWindowTextW(g_text, L"Windows is saving a System Restore point. This can take a minute or two.");
        EnableWindow(g_next, FALSE); EnableWindow(g_back, FALSE); EnableWindow(g_cancel, FALSE);
        UpdateWindow(g_wnd);
        e = restore_point(TRUE, TRUE);
        SetCursor(c);
        EnableWindow(g_next, TRUE); EnableWindow(g_back, TRUE); EnableWindow(g_cancel, TRUE);
        if (e) {
            WCHAR m[600];
            wsprintfW(m, L"The restore point could not be created.\n\n%s (code %u)\n\nInstall VistaExt without a restore "
                         L"point?", rp_reason(e), e);
            if (MessageBoxW(g_wnd, m, TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) { page(P_BACKUP); return; }
        } else g_rp_made = TRUE;
    }
    page(P_INSTALL);
    SendMessageW(g_prog, PBM_SETMARQUEE, TRUE, 40);
    g_job.noauto = g_noauto;
    if ((th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)install_thread, &g_job, 0, NULL))) {
        /* the window waits for it without freezing */
        HANDLE w = th;
        MSG m;
        while (MsgWaitForMultipleObjects(1, &w, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0)
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) continue;   /* no closing mid-install */
                TranslateMessage(&m); DispatchMessageW(&m);
            }
        CloseHandle(th);
    } else g_job.code = 9009;
    if (g_rp_made) restore_point(FALSE, g_job.code == 0);
    g_result = (int)g_job.code;
    if (!g_job.code) CheckDlgButton(g_wnd, ID_TRAY, BST_CHECKED);
    page(P_FINISH);
}

static LRESULT CALLBACK wizproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEXT:
            if (g_page == P_WARN) page(P_BACKUP);
            else if (g_page == P_BACKUP) begin_install();
            else if (g_page == P_FINISH) {
                if (!g_job.code && IsDlgButtonChecked(h, ID_TRAY) == BST_CHECKED) start_tray();
                DestroyWindow(h);
            }
            return 0;
        case ID_BACK:
            if (g_page == P_BACKUP) page(P_WARN);
            return 0;
        case ID_CANCEL:
            DestroyWindow(h);                      /* Decline / Cancel: nothing was changed */
            return 0;
        case ID_BACKUP: {
            /* Windows Backup: "Backup and Restore (Center)" on both Vista and 7 */
            WCHAR p[MAX_PATH];
            GetSystemDirectoryW(p, MAX_PATH);
            vx_wcscatn(p, L"\\sdclt.exe", MAX_PATH);
            if ((INT_PTR)ShellExecuteW(h, NULL, p, NULL, NULL, SW_SHOWNORMAL) <= 32)
                MessageBoxW(h, L"Windows Backup could not be opened. Find it in Control Panel under \"Backup and Restore\".",
                            TITLE, MB_ICONINFORMATION);
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        if (g_page == P_INSTALL) return 0;       /* not while installing */
        DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_title) { SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int wizard(BOOL noauto)
{
    WNDCLASSW wc;
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    MSG m;
    const int W = 560, H = 420, pad = 20;
    RECT rc = { 0, 0, W, H };
    HINSTANCE inst = GetModuleHandleW(NULL);
    g_noauto = noauto;
    InitCommonControlsEx(&icc);
    vx_memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    lf.lfWeight = FW_BOLD;
    g_big = CreateFontIndirectW(&lf);
    vx_memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wizproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"VistaExtSetup";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, TITLE, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
                            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
#define MK(var, cls, text, style, x, y, w, h, id, font) \
    var = CreateWindowExW(0, cls, text, WS_CHILD | (style), x, y, w, h, g_wnd, (HMENU)(INT_PTR)(id), inst, NULL); \
    SendMessageW(var, WM_SETFONT, (WPARAM)(font), TRUE)
    {
        HWND band;
        MK(band, L"STATIC", L"", WS_VISIBLE | SS_WHITERECT, 0, 0, W, 64, -1, g_font);
    }
    MK(g_title, L"STATIC", L"", WS_VISIBLE | SS_NOPREFIX, pad, 18, W - 2 * pad, 32, -1, g_big);
    {
        HWND line;
        MK(line, L"STATIC", L"", WS_VISIBLE | SS_ETCHEDHORZ, 0, 64, W, 2, -1, g_font);
    }
    MK(g_text, L"STATIC", L"", WS_VISIBLE | SS_NOPREFIX, pad, 80, W - 2 * pad, 220, -1, g_font);
    MK(g_rp, L"BUTTON", L"Create a System Restore point before installing (recommended)", WS_TABSTOP | BS_AUTOCHECKBOX,
       pad, 250, W - 2 * pad, 22, ID_RP, g_font);
    MK(g_bk, L"BUTTON", L"Open Windows Backup...", WS_TABSTOP, pad, 282, 200, 28, ID_BACKUP, g_font);
    MK(g_tray, L"BUTTON", L"Start VistaExt's tray icon now", WS_TABSTOP | BS_AUTOCHECKBOX, pad, 310, W - 2 * pad, 22,
       ID_TRAY, g_font);
    MK(g_prog, PROGRESS_CLASSW, L"", PBS_MARQUEE, pad, 200, W - 2 * pad, 18, ID_PROGRESS, g_font);
    {
        HWND line;
        MK(line, L"STATIC", L"", WS_VISIBLE | SS_ETCHEDHORZ, 0, H - 56, W, 2, -1, g_font);
    }
    MK(g_back, L"BUTTON", L"< &Back", WS_TABSTOP, W - 3 * 100 - pad, H - 42, 90, 28, ID_BACK, g_font);
    MK(g_next, L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 2 * 100 - pad + 10, H - 42, 90, 28, ID_NEXT, g_font);
    MK(g_cancel, L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP, W - 100 - pad + 10, H - 42, 90, 28, ID_CANCEL, g_font);
#undef MK
    CheckDlgButton(g_wnd, ID_RP, BST_CHECKED);
    page(P_WARN);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g_result;
}

static int do_install(BOOL noauto, BOOL rp)
{
    vx_real_version(&g_maj, &g_min);
    if (!(g_maj == 6 && g_min <= 1)) {
        if (g_quiet) return 3;
        if (MessageBoxW(NULL, L"VistaExt is made for Windows Vista and Windows 7. This Windows is newer and "
                              L"already has what VistaExt adds.\n\nInstall anyway?", TITLE,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 3;
    }
    if (!g_quiet) return wizard(noauto);
    /* /quiet: the same steps, without screens */
    if (rp && restore_point(TRUE, TRUE) == 0) g_rp_made = TRUE;
    g_job.noauto = noauto;
    install_thread(&g_job);
    if (g_rp_made) restore_point(FALSE, g_job.code == 0);
    return (int)g_job.code;
}

static int do_uninstall(BOOL fromtemp)
{
    WCHAR vx[MAX_PATH], self[MAX_PATH], script[MAX_PATH + 32], m[1200], line[300];
    DWORD code;
    GetEnvironmentVariableW(L"ProgramW6432", vx, MAX_PATH) || GetEnvironmentVariableW(L"ProgramFiles", vx, MAX_PATH);
    vx_wcscatn(vx, L"\\VistaExt", MAX_PATH);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!fromtemp && !vx_wcsnicmp(self, vx, vx_wcslen(vx))) {
        /* running from the folder that is about to be removed: continue from a
         * temporary copy, which Windows deletes at the next restart */
        WCHAR tmp[MAX_PATH], copy[MAX_PATH + 40], cmd[MAX_PATH + 80];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        GetTempPathW(MAX_PATH, tmp);
        wsprintfW(copy, L"%sVistaExtUninstall.%u.exe", tmp, GetCurrentProcessId());
        if (CopyFileW(self, copy, FALSE)) {
            wsprintfW(cmd, L"\"%s\" /uninstall /fromtemp%s", copy, g_quiet ? L" /quiet" : L"");
            vx_memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                MoveFileExW(copy, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0;
            }
        }
    }
    if (!g_quiet && MessageBoxW(NULL, L"Remove VistaExt?\n\nEvery program it changed is put back the way it was. "
                                      L"Close those programs first.", TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 4;
    setup_log_path();
    wsprintfW(script, L"%s\\uninstall.cmd", vx);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        msg(L"VistaExt is not installed (uninstall.cmd was not found).", MB_ICONINFORMATION);
        return 0;
    }
    show_progress(L"Removing VistaExt and putting changed programs back...");
    code = run_script(script, L"/quiet");
    if (g_wnd) DestroyWindow(g_wnd), g_wnd = NULL;
    if (code) {
        log_line("could not be put back", line, 300);
        wsprintfW(m, L"VistaExt was not removed: some changed programs are running.\n\nClose them (or restart "
                     L"Windows) and remove VistaExt again. Nothing was changed.\n\nDetails: %s", g_log);
        msg(m, MB_ICONWARNING);
        return (int)code;
    }
    {   /* the last trace: setup.log (this removal's own log) and the folder go
         * when Windows restarts (Windows Installer packages msifix kept, if
         * any, stay: removing them would break repairing those programs) */
        WCHAR d[MAX_PATH];
        MoveFileExW(g_log, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, d))) {
            vx_wcscatn(d, L"\\VistaExt", MAX_PATH);
            MoveFileExW(d, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    if (log_line("in use", line, 300) || log_line("Restart Windows", line, 300)) {
        wsprintfW(m, L"VistaExt is removed, but some of its files are still in use. Restart Windows to finish.");
        msg(m, MB_ICONINFORMATION);
    } else msg(L"VistaExt has been removed.", MB_ICONINFORMATION);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cl, int show)
{
    int argc = 0, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL uninstall = FALSE, noauto = FALSE, fromtemp = FALSE, rp = FALSE;
    for (i = 1; argv && i < argc; i++) {
        if (!vx_wcsicmp(argv[i], L"/quiet") || !vx_wcsicmp(argv[i], L"/S")) g_quiet = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/uninstall")) uninstall = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/noauto")) noauto = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/fromtemp")) fromtemp = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/restorepoint")) rp = TRUE;
    }
    /* System Restore and Explorer are reached through COM */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL,
                         EOAC_DYNAMIC_CLOAKING, NULL);
    return uninstall ? do_uninstall(fromtemp) : do_install(noauto, rp);
}

void WinMainCRTStartup(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
}

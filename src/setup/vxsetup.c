/* VistaExtSetup.exe - installs or removes VistaExt.
 *
 *   VistaExtSetup.exe              the setup wizard:
 *                                  0. if VistaExt is installed already: its
 *                                     version, and Update (or Reinstall) or
 *                                     Uninstall,
 *                                  1. a warning (Accept / Decline: Decline
 *                                     changes nothing; not for an update),
 *                                  2. backup: a System Restore point (checked
 *                                     by default) and a way to Windows Backup,
 *                                  3. install: the build for this Windows and
 *                                     architecture, the service, the tray
 *                                     icon and its autostart,
 *                                  4. finish: start the tray icon now.
 *   VistaExtSetup.exe /uninstall   remove (Programs and Features and the Start
 *                                  menu run this): a warning that lists the
 *                                  programs that need VistaExt (they stay
 *                                  installed but stop starting), then a System
 *                                  Restore point, then VistaExt is switched
 *                                  off, then every file it added is removed
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
#include "../common/userrun.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shlguid.h>

/* this release; build.sh checks that it matches version.rci and
 * install.cmd's DisplayVersion */
#define VX_THIS_VERSION L"1.2.0"
#define VX_ARP_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VistaExt"

static BOOL g_quiet;
static HWND g_wnd;
static WCHAR g_log[MAX_PATH];
static const WCHAR *TITLE = L"VistaExt Setup";

static void msg(const WCHAR *text, UINT icon)
{
    if (!g_quiet) MessageBoxW(g_wnd, text, TITLE, icon | MB_SETFOREGROUND);
}

/* ---- a small "please wait" window (uninstall) ------------------------------------ */
static HWND g_progtext;
static void show_progress(const WCHAR *text)
{
    if (g_quiet) return;
    if (g_wnd && g_progtext) { SetWindowTextW(g_progtext, text); UpdateWindow(g_wnd); return; }
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"STATIC", TITLE, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                            (GetSystemMetrics(SM_CXSCREEN) - 460) / 2, (GetSystemMetrics(SM_CYSCREEN) - 120) / 2,
                            460, 120, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_wnd) return;
    g_progtext = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 22, 430, 56, g_wnd, NULL, NULL, NULL);
    SendMessageW(g_progtext, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    UpdateWindow(g_wnd);
}

static void close_progress(void)
{
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = g_progtext = NULL;
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

/* run a command line hidden, output to g_log (appended, or a new log) */
static DWORD run_hidden(WCHAR *cmd, BOOL append)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out = CreateFileW(g_log, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, NULL), nul;
    DWORD code;
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
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

/* "cmd /c <script> <args>" (a new log, or appended to it) */
static DWORD run_script_ex(const WCHAR *script, const WCHAR *args, BOOL append)
{
    WCHAR cmd[MAX_PATH * 3], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wsprintfW(cmd, L"\"%s\\cmd.exe\" /c \"\"%s\" %s\"", sys, script, args);
    return run_hidden(cmd, append);
}

static DWORD run_script(const WCHAR *script, const WCHAR *args) { return run_script_ex(script, args, FALSE); }

/* the first log line containing <what>, as text (the whole log is read:
 * the summary lines come last, after one line per file) */
static BOOL log_line(const char *what, WCHAR *out, int cch)
{
    HANDLE h = CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0, i, wl = (DWORD)vx_strlen(what);
    char *buf;
    BOOL ok = FALSE;
    out[0] = 0;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (16u << 20) || !(buf = HeapAlloc(GetProcessHeap(), 0, size + 1))) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
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
    HeapFree(GetProcessHeap(), 0, buf);
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

/* VistaExt's folder (the 64-bit Program Files on 64-bit Windows) */
static void vx_folder(WCHAR *out)
{
    GetEnvironmentVariableW(L"ProgramW6432", out, MAX_PATH) || GetEnvironmentVariableW(L"ProgramFiles", out, MAX_PATH);
    vx_wcscatn(out, L"\\VistaExt", MAX_PATH);
}

/* Is VistaExt installed? Its version from Programs and Features
 * ("1.2.0 (Beta 1.2)"), or "" if that entry is missing but its files are there */
static BOOL installed_version(WCHAR *ver, DWORD cch)
{
    HKEY k;
    WCHAR p[MAX_PATH + 20];
    BOOL found = FALSE;
    ver[0] = 0;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_ARP_KEY, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        DWORD t = 0, n = (cch - 1) * sizeof(WCHAR);
        if (RegQueryValueExW(k, L"DisplayVersion", NULL, &t, (BYTE *)ver, &n) || t != REG_SZ) ver[0] = 0;
        else ver[n / sizeof(WCHAR) < cch ? n / sizeof(WCHAR) : cch - 1] = 0;
        RegCloseKey(k);
        found = TRUE;
    }
    vx_folder(p);
    vx_wcscatn(p, L"\\uninstall.cmd", MAX_PATH + 20);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) found = TRUE;
    return found;
}

/* "1.0.0 (Beta 1)" against "1.2.0": <0 older, 0 the same, >0 newer; an
 * unreadable version counts as older */
static int cmp_version(const WCHAR *a, const WCHAR *b)
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned x = 0, y = 0;
        BOOL hx = FALSE;
        while (*a >= '0' && *a <= '9') { x = x * 10 + (unsigned)(*a++ - '0'); hx = TRUE; }
        while (*b >= '0' && *b <= '9') y = y * 10 + (unsigned)(*b++ - '0');
        if (!hx && i == 0) return -1;
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* Files the log says a running program still had (install: "<name>.vxoldN",
 * the old copy an update set aside; uninstall: VistaExt's files): deleted
 * when Windows restarts. With <and_off>, also the copy the switch moves into
 * VistaExt.off at the restart (switched off just before, while in use). The
 * paths in the log are the 64-bit ones, and this setup is 32-bit: file
 * system redirection is off meanwhile. Returns how many. */
static int delete_logged_at_restart(const char *key, BOOL and_off)
{
    HANDLE h = CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size = h != INVALID_HANDLE_VALUE ? GetFileSize(h, NULL) : 0, got = 0, i, kl = (DWORD)vx_strlen(key);
    char *b = size && size < (8u << 20) ? HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
    int later = 0;
    BOOL (WINAPI *off)(PVOID *) = (BOOL (WINAPI *)(PVOID *))(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                                "Wow64DisableWow64FsRedirection");
    BOOL (WINAPI *on)(PVOID) = (BOOL (WINAPI *)(PVOID))(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                           "Wow64RevertWow64FsRedirection");
    PVOID redir = NULL;
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    if (wow && off && !off(&redir)) wow = FALSE;
    if (b && ReadFile(h, b, size, &got, NULL)) {
        b[got] = 0;
        for (i = 0; i + kl < got; i++) {
            DWORD e;
            WCHAR w[MAX_PATH], o[MAX_PATH + 20];
            if (memcmp(b + i, key, kl)) continue;
            i += kl;
            for (e = i; e < got && b[e] != '\r' && b[e] != '\n'; e++) ;
            b[e] = 0;
            if (MultiByteToWideChar(CP_OEMCP, 0, b + i, -1, w, MAX_PATH) && MoveFileExW(w, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                later++;
                if (and_off) {
                    SIZE_T n = vx_wcslen(w);
                    while (n && w[n - 1] != '\\') n--;
                    /* (not for a file that is in VistaExt.off already) */
                    if (n >= 14 && !vx_wcsnicmp(w + n - 14, L"\\VistaExt.off\\", 14)) n = 0;
                    if (n) {
                        vx_memcpy(o, w, n * sizeof(WCHAR));
                        o[n] = 0;
                        vx_wcscatn(o, L"VistaExt.off\\", MAX_PATH + 20);
                        vx_wcscatn(o, w + n, MAX_PATH + 20);
                        MoveFileExW(o, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    }
                }
            }
            i = e;
        }
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (wow && on) on(redir);
    return later;
}

/* ---- the install --------------------------------------------------------------- */
typedef struct {
    BOOL noauto;
    DWORD code;              /* install.cmd's exit code; 5: unpacking failed */
    WCHAR line[300];         /* its summary line */
    int inuse;               /* files in use, replaced: the old copies go at the restart */
    BOOL off;                /* VistaExt was switched off before this update, and stays off */
    WCHAR offline[300];
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
    /* an update: the old copies running programs still have go at the restart */
    j->inuse = delete_logged_at_restart("old copy deleted at restart: ", FALSE);
    j->off = log_line("is switched off", j->offline, 300);
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
static BOOL g_rp_uninstall;   /* the restore point is for removing VistaExt */

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
    ri.dwRestorePtType = ok ? (g_rp_uninstall ? 1 /* APPLICATION_UNINSTALL */ : 0 /* APPLICATION_INSTALL */)
                            : 13 /* CANCELLED_OPERATION */;
    if (begin) ri.dwRestorePtType = g_rp_uninstall ? 1 : 0;
    ri.llSequenceNumber = begin ? 0 : g_rp_seq;
    vx_wcscpyn(ri.szDescription, g_rp_uninstall ? L"Before removing VistaExt" : L"Before installing VistaExt", 256);
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

static void start_tray(void)
{
    WCHAR exe[MAX_PATH];
    vx_folder(exe);
    vx_wcscatn(exe, L"\\vxtray.exe", MAX_PATH);
    if (!vx_run_as_user(exe, NULL, NULL)) ShellExecuteW(NULL, NULL, exe, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the wizard --------------------------------------------------------------------- */
enum { P_EXISTING, P_WARN, P_BACKUP, P_INSTALL, P_FINISH };
enum { ID_BACK = 10, ID_NEXT, ID_CANCEL, ID_RP, ID_BACKUP, ID_TRAY, ID_PROGRESS, ID_UPDATE, ID_REMOVE };
#define WM_DONE (WM_APP + 1)
static HWND g_title, g_text, g_next, g_back, g_cancel, g_rp, g_bk, g_tray, g_prog, g_upd, g_rem;
static BOOL g_existing;          /* VistaExt is installed already */
static WCHAR g_oldver[64];       /* its version ("" if unknown) */
static int g_cmp;                /* it against this one: <0 older, 0 the same, >0 newer */
static BOOL g_go_uninstall;      /* the user chose Uninstall on the first page */
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
    ShowWindow(g_upd, p == P_EXISTING ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rem, p == P_EXISTING ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rp, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_bk, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tray, p == P_FINISH && !g_job.code ? SW_SHOW : SW_HIDE);
    ShowWindow(g_prog, p == P_INSTALL ? SW_SHOW : SW_HIDE);
    ShowWindow(g_back, p == P_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_cancel, p == P_BACKUP || p == P_EXISTING ? SW_SHOW : SW_HIDE);
    ShowWindow(g_next, p == P_INSTALL ? SW_HIDE : SW_SHOW);
    switch (p) {
    case P_EXISTING: {
        const WCHAR *v = g_oldver[0] ? g_oldver : L"(an earlier version)";
        if (g_cmp < 0) {
            wsprintfW(t, L"VistaExt %s is installed", v);
            SetWindowTextW(g_title, t);
            wsprintfW(t, L"This setup has VistaExt %s, a newer version.\n\n"
                         L"Updating replaces VistaExt's files and keeps everything else: your settings, the Windows "
                         L"version programs are told, whether VistaExt is on or off, and the programs it made "
                         L"compatible (they keep working).\n\n"
                         L"Programs that are running keep the old version until they are closed; restart Windows "
                         L"after the update if some are.", VX_THIS_VERSION);
            SetWindowTextW(g_upd, L"&Update VistaExt to " VX_THIS_VERSION L" (recommended)");
        } else if (g_cmp == 0) {
            SetWindowTextW(g_title, L"VistaExt " VX_THIS_VERSION L" is already installed");
            wsprintfW(t, L"This is the version that is installed. Reinstalling puts back any of VistaExt's files "
                         L"that are missing or damaged and keeps your settings and the programs it made compatible.");
            SetWindowTextW(g_upd, L"&Reinstall VistaExt " VX_THIS_VERSION);
        } else {
            wsprintfW(t, L"A newer VistaExt is installed (%s)", v);
            SetWindowTextW(g_title, t);
            wsprintfW(t, L"This setup has VistaExt %s, which is older than the installed one. Installing it replaces the "
                         L"newer files; settings made only for the newer version may then do nothing.\n\n"
                         L"Unless you have a reason to go back, click Cancel and keep the version you have.", VX_THIS_VERSION);
            SetWindowTextW(g_upd, L"&Replace it with the older version " VX_THIS_VERSION);
        }
        vx_wcscatn(t, L"\n\nUninstalling removes VistaExt completely (a System Restore point is made first). "
                      L"Programs you installed stay installed, but those made for newer Windows stop starting.", 2600);
        SetWindowTextW(g_text, t);
        SetWindowTextW(g_rem, L"U&ninstall VistaExt");
        SetWindowTextW(g_next, L"&Next >");
        SetWindowTextW(g_cancel, L"Cancel");
        SetFocus(g_next);
        break;
    }
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
        SetWindowTextW(g_title, g_existing ? L"Updating VistaExt" : L"Installing VistaExt");
        wsprintfW(t, L"%s the VistaExt build for Windows %s (%s), the background service and the tray icon.\n\n"
                     L"This takes a minute.", g_existing ? L"Updating to" : L"Installing", g_min == 0 ? L"Vista" : g_min == 1 ? L"7" : g_min == 2 ? L"8" : L"8 (on 8.1)",
                  wow || sizeof(void *) == 8 ? L"64-bit" : L"32-bit");
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
            if (g_existing) {
                SetWindowTextW(g_title, L"VistaExt is updated to " VX_THIS_VERSION);
                wsprintfW(t, L"%s\n\nYour settings and the programs VistaExt made compatible are kept.", g_job.line);
            } else {
                SetWindowTextW(g_title, L"VistaExt is installed");
                wsprintfW(t, L"%s\n\nPrograms made for Windows 8, 10 and 11 that you install or download from now on are "
                             L"made compatible by themselves: double-click them as usual. \"VistaExt\" in the Start menu "
                             L"(and its icon next to the clock) turns it on or off and sets the Windows version programs "
                             L"are told.", g_job.line);
            }
            if (g_job.inuse)
                vx_wcscatn(t, L"\n\nSome of VistaExt's files were in use by running programs. They keep the old version "
                              L"until they are closed: restart Windows to finish.", 2600);
            if (g_job.off) vx_wcscatn(t, L"\n\nVistaExt is switched off, as it was before. Turn it on in \"VistaExt\" "
                                         L"(Start menu).", 2600);
            if (pre[0]) { vx_wcscatn(t, L"\n\nAlso recommended:", 2600); vx_wcscatn(t, pre, 2600); }
        }
        SetWindowTextW(g_text, t);
        SetWindowTextW(g_next, L"&Finish");
        SetFocus(g_next);
        break;
    }
}

/* creating the restore point takes a minute or two: in its own thread,
 * so the window keeps drawing and answering meanwhile */
typedef struct { BOOL begin, ok; DWORD result; } rp_job;
static DWORD WINAPI rp_thread(LPVOID p)
{
    rp_job *j = p;
    j->result = restore_point(j->begin, j->ok);
    return 0;
}

static DWORD restore_point_pumping(BOOL begin, BOOL ok)
{
    rp_job j = { begin, ok, 0 };
    HANDLE th = CreateThread(NULL, 0, rp_thread, &j, 0, NULL);
    if (!th) return restore_point(begin, ok);
    wait_pumping(th);
    CloseHandle(th);
    return j.result;
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
        e = restore_point_pumping(TRUE, TRUE);
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
    if (g_rp_made) restore_point_pumping(FALSE, g_job.code == 0);
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
            if (g_page == P_EXISTING) {
                if (IsDlgButtonChecked(h, ID_REMOVE) == BST_CHECKED) {
                    g_go_uninstall = TRUE;          /* after this window: the uninstall's own steps */
                    DestroyWindow(h);
                } else page(P_BACKUP);
            } else if (g_page == P_WARN) page(P_BACKUP);
            else if (g_page == P_BACKUP) begin_install();
            else if (g_page == P_FINISH) {
                if (!g_job.code && IsDlgButtonChecked(h, ID_TRAY) == BST_CHECKED) start_tray();
                DestroyWindow(h);
            }
            return 0;
        case ID_BACK:
            if (g_page == P_BACKUP) page(g_existing ? P_EXISTING : P_WARN);
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
        if (g_page == P_INSTALL || !IsWindowEnabled(g_cancel)) return 0;   /* not while installing or saving a restore point */
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
    MK(g_upd, L"BUTTON", L"", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, pad, 280, W - 2 * pad, 22, ID_UPDATE, g_font);
    MK(g_rem, L"BUTTON", L"", BS_AUTORADIOBUTTON, pad, 306, W - 2 * pad, 22, ID_REMOVE, g_font);
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
    CheckDlgButton(g_wnd, ID_UPDATE, BST_CHECKED);
    page(g_existing ? P_EXISTING : P_WARN);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g_result;
}

static int do_uninstall(BOOL fromtemp, BOOL rp);

static int do_install(BOOL noauto, BOOL rp)
{
    int r;
    vx_real_version(&g_maj, &g_min);
    if (!(g_maj == 6 && g_min <= 3)) {
        if (g_quiet) return 3;
        if (MessageBoxW(NULL, L"VistaExt is made for Windows Vista, 7, 8 and 8.1. This Windows is newer and "
                              L"already has what VistaExt adds.\n\nInstall anyway?", TITLE,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 3;
    }
    g_existing = installed_version(g_oldver, 64);
    g_cmp = g_existing ? cmp_version(g_oldver, VX_THIS_VERSION) : -1;
    if (!g_quiet) {
        r = wizard(noauto);
        if (g_go_uninstall) { g_wnd = NULL; return do_uninstall(FALSE, TRUE); }
        return r;
    }
    /* /quiet: the same steps, without screens */
    if (rp && restore_point(TRUE, TRUE) == 0) g_rp_made = TRUE;
    g_job.noauto = noauto;
    install_thread(&g_job);
    if (g_rp_made) restore_point(FALSE, g_job.code == 0);
    return (int)g_job.code;
}

/* The programs that need VistaExt: files it patched, programs registered
 * for the in-memory mode, and those it put back when it was switched off
 * (to patch them again when it is switched on). Shown by their folder
 * ("Mozilla Firefox"), each once; at most <max>. Returns how many there are. */
static int affected_programs(WCHAR *out, SIZE_T cch, int max)
{
    static const WCHAR *keys[] = { VX_REG_ROOT L"\\Patched", VX_REG_ROOT L"\\Memory", VX_REG_ROOT L"\\Repatch" };
    WCHAR seen[40][64], name[MAX_PATH + 8];
    int nseen = 0, total = 0;
    unsigned i;
    out[0] = 0;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        HKEY k;
        DWORD idx, n;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) continue;
        for (idx = 0; ; idx++) {
            WCHAR *e, *f, label[64];
            int j;
            n = MAX_PATH + 8;
            if (RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL)) break;
            if (n < 4 || name[1] != ':') continue;
            if (name[n - 1] == '\\') name[--n] = 0;            /* a folder */
            else {                                               /* a file: its folder */
                for (e = name + n; e > name && e[-1] != '\\'; e--) ;
                if (e > name) e[-1] = 0;
            }
            for (f = name + vx_wcslen(name); f > name && f[-1] != '\\'; f--) ;
            if (!*f) continue;
            vx_wcscpyn(label, f, 64);
            for (j = 0; j < nseen && vx_wcsicmp(seen[j], label); j++) ;
            if (j < nseen) continue;
            if (nseen < 40) vx_wcscpyn(seen[nseen++], label, 64);
            if (++total <= max) {
                vx_wcscatn(out, L"\n    - ", cch);
                vx_wcscatn(out, label, cch);
            }
        }
        RegCloseKey(k);
    }
    if (total > max) {
        WCHAR more[64];
        wsprintfW(more, L"\n    - and %d more", total - max);
        vx_wcscatn(out, more, cch);
    }
    return total;
}

/* Removing VistaExt: a warning, a System Restore point (the backup), the
 * switch off, then uninstall.cmd removes every file VistaExt added and puts
 * back every program file it changed. Programs the user installed are not
 * removed: they stay, and those made for newer Windows stop starting. */
static int do_uninstall(BOOL fromtemp, BOOL rp)
{
    WCHAR vx[MAX_PATH], self[MAX_PATH], script[MAX_PATH + 32], m[3000], progs[1500], exe[MAX_PATH + 32], cmd[MAX_PATH + 64];
    DWORD code;
    int nprogs, later;
    BOOL rp_made = FALSE;
    vx_folder(vx);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!fromtemp && !vx_wcsnicmp(self, vx, vx_wcslen(vx))) {
        /* running from the folder that is about to be removed: continue from a
         * temporary copy, which Windows deletes at the next restart */
        WCHAR tmp[MAX_PATH], copy[MAX_PATH + 40], c[MAX_PATH + 80];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        GetTempPathW(MAX_PATH, tmp);
        wsprintfW(copy, L"%sVistaExtUninstall.%u.exe", tmp, GetCurrentProcessId());
        if (CopyFileW(self, copy, FALSE)) {
            wsprintfW(c, L"\"%s\" /uninstall /fromtemp%s%s", copy, g_quiet ? L" /quiet" : L"", rp ? L" /restorepoint" : L"");
            vx_memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            if (CreateProcessW(NULL, c, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                MoveFileExW(copy, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0;
            }
        }
    }
    wsprintfW(script, L"%s\\uninstall.cmd", vx);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        msg(L"VistaExt is not installed (uninstall.cmd was not found).", MB_ICONINFORMATION);
        return 0;
    }
    nprogs = affected_programs(progs, 1500, 12);
    if (!g_quiet) {
        wsprintfW(m, L"Uninstall VistaExt?\n\n"
                     L"1. A System Restore point is created first (a backup of Windows' system files and settings).\n"
                     L"2. VistaExt's compatibility layer is turned off.\n"
                     L"3. Every file VistaExt installed is removed, with its service, tray icon, Start menu entries and "
                     L"settings. Program files it changed are put back the way they were.\n\n"
                     L"Programs you installed are NOT removed. But programs made for newer Windows will no longer "
                     L"start on this PC without VistaExt.");
        if (nprogs) {
            vx_wcscatn(m, L" These programs use VistaExt now; you will lose the ability to use them:", 3000);
            vx_wcscatn(m, progs, 3000);
        }
        vx_wcscatn(m, L"\n\nClose those programs before you continue.", 3000);
        if (MessageBoxW(NULL, m, L"Uninstall VistaExt", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
            return 4;
    }
    setup_log_path();
    { HANDLE h = CreateFileW(g_log, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

    /* 1. the backup */
    if (!g_quiet || rp) {
        DWORD e;
        show_progress(L"Creating a System Restore point (a backup of Windows' system files and settings)...\n\n"
                      L"This can take a minute or two.");
        g_rp_uninstall = TRUE;
        if ((e = restore_point_pumping(TRUE, TRUE))) {
            WCHAR q[600];
            wsprintfW(q, L"The restore point could not be created.\n\n%s (code %u)\n\nUninstall VistaExt without a restore "
                         L"point?", rp_reason(e), e);
            if (!g_quiet && MessageBoxW(g_wnd, q, L"Uninstall VistaExt", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                close_progress();
                return 4;
            }
        } else rp_made = TRUE;
    }

    /* 2. off: programs stop getting VistaExt, patched files are put back, its
     * system files move aside (also those of a VistaExt without this step
     * are then removed by uninstall.cmd the same way) */
    show_progress(L"Turning off VistaExt's compatibility layer...");
    wsprintfW(exe, L"%s\\vxauto.exe", vx);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
        wsprintfW(cmd, L"\"%s\" /disable", exe);
        run_hidden(cmd, TRUE);
    }

    /* 3. remove */
    show_progress(L"Removing VistaExt and putting changed programs back...");
    code = run_script_ex(script, L"/quiet", TRUE);
    /* uninstall.cmd deletes itself last when everything is gone, and cmd
     * then ends with an error of its own (the script it was reading is
     * gone): what counts is what it reported. It stops before removing
     * anything when a changed program is running ("Nothing has been
     * removed"), or without administrator rights. */
    {
        WCHAR l[300];
        if (log_line("Nothing has been removed", l, 300) || log_line("Run as administrator", l, 300)) { if (!code) code = 1; }
        else if (log_line("VistaExt has been removed", l, 300) || log_line("are in use. Restart Windows", l, 300)) code = 0;
    }
    if (rp_made) restore_point_pumping(FALSE, code == 0);
    close_progress();
    if (code) {
        wsprintfW(m, L"VistaExt is turned off, but it was not removed: some programs it changed are running.\n\n"
                     L"Close them (or restart Windows) and uninstall VistaExt again.\n\nDetails: %s", g_log);
        msg(m, MB_ICONWARNING);
        return (int)code;
    }
    /* files a running program still had loaded ("still in use" in the log):
     * deleted when Windows restarts, with what uninstall.cmd kept for a retry */
    later = delete_logged_at_restart("still in use: ", TRUE);
    if (later) {
        static const WCHAR *left[] = { L"installed.txt", L"uninstall.cmd", L"VistaExtSetup.exe", NULL };
        WCHAR f[MAX_PATH + 32], d[MAX_PATH + 32];
        int k;
        BOOL (WINAPI *off)(PVOID *) = (BOOL (WINAPI *)(PVOID *))(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                                    "Wow64DisableWow64FsRedirection");
        BOOL (WINAPI *on)(PVOID) = (BOOL (WINAPI *)(PVOID))(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                               "Wow64RevertWow64FsRedirection");
        PVOID redir = NULL;
        BOOL wow = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow);
        if (wow && off && !off(&redir)) wow = FALSE;
        for (k = 0; left[k]; k++) {
            wsprintfW(f, L"%s\\%s", vx, left[k]);
            if (GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES) MoveFileExW(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        MoveFileExW(vx, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);           /* the folder, once empty */
        for (k = 0; k < 2; k++) {                                      /* VistaExt.off, once empty */
            GetWindowsDirectoryW(d, MAX_PATH);
            vx_wcscatn(d, k ? L"\\SysWOW64\\VistaExt.off" : L"\\System32\\VistaExt.off", MAX_PATH + 32);
            if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) MoveFileExW(d, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        if (wow && on) on(redir);
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
    vx_wcscpyn(m, L"VistaExt has been removed.", 3000);
    if (rp_made) vx_wcscatn(m, L"\n\nA System Restore point \"Before removing VistaExt\" was created.", 3000);
    if (later) vx_wcscatn(m, L"\n\nSome of its files were still in use by running programs: they are deleted when "
                             L"Windows restarts.", 3000);
    if (nprogs) vx_wcscatn(m, L"\n\nThe programs you installed are still installed. Those made for newer Windows no "
                              L"longer start; remove them in Programs and Features if you do not need them.", 3000);
    msg(m, MB_ICONINFORMATION);
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
    return uninstall ? do_uninstall(fromtemp, rp) : do_install(noauto, rp);
}

void WinMainCRTStartup(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
}

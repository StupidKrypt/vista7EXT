/* vxauto - VistaExt's background service ("VistaExt service"): makes newly
 * installed or updated programs compatible without any user step, and is
 * where the on/off switch and the global Windows version are changed.
 *
 *   vxauto /install      create and start the service (setup does this)
 *   vxauto /uninstall    stop and remove it
 *   vxauto /scan         make every installed program compatible now
 *   vxauto /enable       switch VistaExt on       } also work without the
 *   vxauto /disable      switch VistaExt off      } service, e.g. in Safe
 *   vxauto /status       show the state           } Mode (as administrator)
 *   vxauto /setver <v>   the Windows version programs are told: WIN7, WIN8,
 *                        WIN81, WIN10, WIN11, REAL, or VER:10.0.26100[.sp]
 *   vxauto /setups 0|1   setup programs that run as they are are told the
 *                        chosen version too (1, the default) or not
 *   vxauto /prepare <exe>  what a starting program asks (tests)
 *   vxauto /pipe <request> a request to the running service, as the tray
 *                        icon sends it (STATUS, ENABLE, DISABLE, SETVER <v>,
 *                        SETUPS 0|1)
 *   vxauto /service      (run by the service manager)
 *
 * Programs: it watches Program Files, Program Files (x86) and every user's
 * AppData\Local and AppData\Roaming. When .exe/.dll files in a program's
 * folder change, it waits until the installer or updater has been quiet for
 * 20 s, then gives the folder the recommended settings (unless the user saved
 * some) and patches what needs it - exactly what VistaExt Center's Apply
 * does. When a program is removed, what VistaExt kept for it is removed too.
 *
 * Setup programs: a single .exe that appears in a user's Downloads or
 * Desktop, in their Temp folder, or in Internet Explorer's download cache is
 * looked at 2 s after it was written. If it cannot run on this Windows
 * without VistaExt (it needs functions this Windows lacks, or its header asks
 * for a newer Windows), it is registered for the in-memory mode, so that a
 * double-click starts it like on the Windows it was made for. Programs that
 * run as they are are left alone. A program VistaExt runs asks the service
 * (named pipe \\.\pipe\VistaExt, "PREPARE") before it starts another one, so
 * a setup's own helper programs, unpacked a moment before, get the same.
 *
 * Switch: "off" takes VistaExt out of Windows without a restart: Image File
 * Execution Options of registered programs are removed, patched files are
 * put back, the Explorer entries and the .msi handler go, and VistaExt's DLLs
 * move from System32/SysWOW64 into a subfolder (VistaExt.off) where no
 * program finds them. Programs already running keep what they loaded until
 * they are closed. "on" reverses each step. Each step can be repeated, and
 * the service finishes an interrupted switch when it starts.
 *
 * Safety: Windows' own folders and VistaExt's are skipped; a name Windows
 * has a program of is never registered; game folders and anti-cheat systems
 * are left alone; a file that is patched more than 3 times in an hour (a
 * self-repairing program putting its original back) is left alone;
 * HKLM\SOFTWARE\VistaExt\Auto = 0 turns the automatic part off. Every change
 * is logged to %ProgramData%\VistaExt\VistaExt.log. */
#include "../vxpatch/patchlib.h"
#include "../common/console.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <stdarg.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <mscat.h>
#ifndef WTS_SESSION_LOGON
#define WTS_SESSION_LOGON 5           /* winuser.h; missing from the Wine headers we build with */
#endif

static void logf(const WCHAR *fmt, ...);
#define SVC_NAME L"VistaExtAuto"
#define PIPE_NAME L"\\\\.\\pipe\\VistaExt"
#define QUIET_MS 20000
#define LOOSE_MS 2000         /* a single file: written completely 2 s ago */
#define GONE_MS 30000         /* removed, and not back after 30 s (an update replacing it) */
/* lists grow as needed; these limits only stop a runaway (each is logged) */
#define MAXPEND 4096
#define MAXFILES 4096
#define WATCH_GROUP 63        /* folders per watcher thread (WaitForMultipleObjects: 64 handles) */
#ifndef THREAD_MODE_BACKGROUND_BEGIN
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000   /* Vista+ */
#endif

static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_ss;
static HANDLE g_stop;
static CRITICAL_SECTION g_cs, g_logcs;
static WCHAR g_logpath[MAX_PATH];
static HANDLE g_switch;        /* one switch, patch or setting change at a time (service and command line) */

typedef struct { WCHAR dir[MAX_PATH]; DWORD last; int tries; } pending;
static pending *g_pend;
static int g_npend, g_cpend;

/* a watched folder. local: in a user's profile (work there as that user);
 * loose: single setup files, not program folders. Each is allocated once and
 * never moves (watchers and queued files keep pointers to it). */
typedef struct {
    WCHAR path[MAX_PATH], profile[MAX_PATH];
    BOOL local, loose;
    HANDLE dir, ev;           /* the watcher's: folder handle, completion event */
    OVERLAPPED ov;
    DWORD *buf;
} root;
#define MAXROOTS 4096
static root *g_roots[MAXROOTS];   /* never reallocated: other threads read it while sign-ins add to it */
static volatile LONG g_nroots;
static int g_nwatched;

/* single files: new ones to look at, removed ones to forget */
typedef struct { WCHAR path[MAX_PATH]; DWORD last; BOOL gone; const root *r; } pfile;
static pfile *g_files;
static int g_nfiles, g_cfiles;

static HANDLE g_work;         /* something was queued (auto-reset) */
static HANDLE g_logon;        /* a user signed in: watch their folders (auto-reset) */

/* make room for one more item in a growable list (under g_cs); FALSE at the limit */
static BOOL grow(void **list, int n, int *cap, SIZE_T item, int limit, const WCHAR *what)
{
    void *nl;
    int nc;
    if (n < *cap) return TRUE;
    if (n >= limit) {
        static LONG said;
        if (!InterlockedExchange(&said, 1))
            logf(L"more than %d %s waiting at once; the rest are looked at when the service starts next", limit, what);
        return FALSE;
    }
    nc = *cap ? *cap * 2 : 32;
    nl = *list ? HeapReAlloc(GetProcessHeap(), 0, *list, nc * item) : HeapAlloc(GetProcessHeap(), 0, nc * item);
    if (!nl) return FALSE;
    *list = nl;
    *cap = nc;
    return TRUE;
}

/* ---- log ------------------------------------------------------------------ */
static void logw(const WCHAR *line)
{
    HANDLE h, imp;
    char buf[1400];
    SYSTEMTIME t;
    int n;
    DWORD put;
    EnterCriticalSection(&g_logcs);
    imp = vxp_unimpersonate();          /* the log is written as the service */
    h = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileSize(h, NULL) > 4 * 1024 * 1024) {      /* keep it small */
            CloseHandle(h);
            h = CreateFileW(g_logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        }
        GetLocalTime(&t);
        n = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        n += WideCharToMultiByte(CP_UTF8, 0, line, -1, buf + n, (int)sizeof(buf) - n - 3, NULL, NULL) - 1;
        if (n > 0) { buf[n++] = '\r'; buf[n++] = '\n'; WriteFile(h, buf, n, &put, NULL); }
        CloseHandle(h);
    }
    vxp_reimpersonate(imp);
    LeaveCriticalSection(&g_logcs);
}

static void log_cb(void *ctx, const WCHAR *line) { logw(line); }

static void logf(const WCHAR *fmt, ...)
{
    WCHAR b[1100];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(b, fmt, ap);
    va_end(ap);
    logw(b);
}

static BOOL auto_on(void) { return vx_setting(L"Auto", 1) && vxp_enabled(); }

static void new_file(const WCHAR *path, const root *r);
static void reg_failed(LONG e, const WCHAR *key, const WCHAR *name);
#define VX_REG_LATER VX_REG_ROOT L"\\RestoreLater"   /* patched, running when switched off */
static int list_count(const WCHAR *key);
static int restore_later(void);
static void lock(void) { if (g_switch) WaitForSingleObject(g_switch, INFINITE); }
/* the same, waiting "ms" at most (a program waits for the answer) */
static BOOL lock_for(DWORD ms)
{
    DWORD w;
    if (!g_switch) return TRUE;
    w = WaitForSingleObject(g_switch, ms);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}
static void unlock(void) { if (g_switch) ReleaseMutex(g_switch); }

/* ---- which folder is "the program" ---------------------------------------- */
static BOOL skip_name(const WCHAR *n, BOOL local)
{
    static const WCHAR *pf[] = { L"Windows Defender", L"Windows Mail", L"Windows Media Player", L"Windows NT",
        L"Windows Photo Viewer", L"Windows Portable Devices", L"Windows Sidebar", L"Windows Journal",
        L"Windows Calendar", L"Windows Collaboration", L"Windows Photo Gallery", L"Internet Explorer",
        L"Common Files", L"MSBuild", L"Reference Assemblies", L"Uninstall Information", L"Microsoft.NET",
        L"Movie Maker", L"DVD Maker", L"Microsoft Games", L"VistaExt", L"Windows Mail", L"Windows Security" };
    static const WCHAR *lo[] = { L"Temp", L"Microsoft", L"VirtualStore", L"Packages", L"CrashDumps",
        L"Downloaded Installations", L"VistaExt", L"SquirrelTemp" };
    const WCHAR **l = local ? lo : pf;
    int i, c = local ? (int)(sizeof(lo) / sizeof(lo[0])) : (int)(sizeof(pf) / sizeof(pf[0]));
    for (i = 0; i < c; i++) if (!vx_wcsicmp(n, l[i])) return TRUE;
    return FALSE;
}

/* the program folder for a changed file: <root>\<name>, or under
 * AppData\Local\Programs: <root>\Programs\<name>. FALSE if skipped. */
static BOOL app_folder(const root *r, const WCHAR *rel, WCHAR *out)
{
    WCHAR first[MAX_PATH];
    SIZE_T i = 0, n;
    while (rel[i] && rel[i] != '\\') { first[i] = rel[i]; i++; }
    first[i] = 0;
    if (!rel[i] || skip_name(first, r->local)) return FALSE;          /* a file in the root itself */
    vx_wcscpyn(out, r->path, MAX_PATH);
    vx_wcscatn(out, L"\\", MAX_PATH);
    if (r->local && !vx_wcsicmp(first, L"Programs")) {
        const WCHAR *s = rel + i + 1;
        n = 0;
        while (s[n] && s[n] != '\\') n++;
        if (!s[n]) return FALSE;
        vx_wcscatn(out, L"Programs\\", MAX_PATH);
        vx_wcscpyn(first, s, n + 1 < MAX_PATH ? n + 1 : MAX_PATH);
    }
    vx_wcscatn(out, first, MAX_PATH);
    return TRUE;
}

static BOOL is_pe_name(const WCHAR *p, SIZE_T n)
{
    return n > 4 && (!vx_wcsnicmp(p + n - 4, L".exe", 4) || !vx_wcsnicmp(p + n - 4, L".dll", 4));
}

static void queue_at(const WCHAR *dir, DWORD when, int tries)
{
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_npend; i++)
        if (!vx_wcsicmp(g_pend[i].dir, dir)) { g_pend[i].last = when; break; }
    if (i == g_npend && grow((void **)&g_pend, g_npend, &g_cpend, sizeof(pending), MAXPEND, L"program folders")) {
        vx_wcscpyn(g_pend[g_npend].dir, dir, MAX_PATH);
        g_pend[g_npend].last = when;
        g_pend[g_npend].tries = tries;
        g_npend++;
    }
    LeaveCriticalSection(&g_cs);
    if (g_work) SetEvent(g_work);
}

static void queue(const WCHAR *dir) { queue_at(dir, GetTickCount(), 0); }

/* ---- loop protection: count patches per file ------------------------------ */
#define MAXHIST 512
static struct { WCHAR path[MAX_PATH]; DWORD first; int n; } g_hist[MAXHIST];
static int g_nhist;

static BOOL too_often(const WCHAR *path)
{
    int i;
    DWORD now = GetTickCount();
    for (i = 0; i < g_nhist; i++) {
        if (vx_wcsicmp(g_hist[i].path, path)) continue;
        if (now - g_hist[i].first > 3600 * 1000) { g_hist[i].first = now; g_hist[i].n = 0; }
        return ++g_hist[i].n > 3;
    }
    i = g_nhist < MAXHIST ? g_nhist++ : (int)(now % MAXHIST);
    vx_wcscpyn(g_hist[i].path, path, MAX_PATH);
    g_hist[i].first = now;
    g_hist[i].n = 1;
    return FALSE;
}

typedef struct { DWORD flags; int failed; } walkctx;

/* "OFF" saved for this file or folder (or a folder above it) */
static BOOL is_off(const WCHAR *path, BOOL dir)
{
    WCHAR p[MAX_PATH + 4], layer[600];
    vx_layer l;
    vx_wcscpyn(p, path, MAX_PATH);
    if (dir) vx_wcscatn(p, L"\\x", MAX_PATH + 4);
    if (!vx_lookup_layer(p, layer, 600)) return FALSE;
    vx_parse_layer(layer, &l);
    return l.off;
}

/* Games are never changed automatically: a game launcher's folder holds its
 * games too (Steam\steamapps), and a changed game file can look like
 * cheating to an anti-cheat system (and get the account banned). The user
 * can still choose to do it in VistaExt Center. */
static BOOL exists_in(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    vx_wcscpyn(p, dir, MAX_PATH);
    vx_wcscatn(p, L"\\", MAX_PATH);
    vx_wcscatn(p, name, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static BOOL is_game_dir(const WCHAR *dir, const WCHAR *name)
{
    static const WCHAR *names[] = { L"steamapps", L"SteamLibrary", L"EasyAntiCheat", L"BattlEye", L"EasyAntiCheat_EOS" };
    static const WCHAR *marks[] = { L"EasyAntiCheat", L"BattlEye", L"EasyAntiCheat_x64.dll", L"EasyAntiCheat_x86.dll",
        L"BEClient_x64.dll", L"BEClient.dll", L"EasyAntiCheat_EOS_Setup.exe", L"start_protected_game.exe",
        L".egstore" /* a game installed by Epic Games */ };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) if (name && !vx_wcsicmp(name, names[i])) return TRUE;
    for (i = 0; i < (int)(sizeof(marks) / sizeof(marks[0])); i++) if (exists_in(dir, marks[i])) return TRUE;
    return FALSE;
}

static int process_files(const WCHAR *dir, walkctx *w, int depth)
{
    WCHAR pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    if (depth > 12) return 0;
    if (is_game_dir(dir, NULL)) {
        WCHAR m[MAX_PATH + 100];
        wsprintfW(m, L"%s: a game (or a folder with an anti-cheat); not changed automatically", dir);
        logw(m);
        return 0;
    }
    vx_wcscpyn(pat, dir, MAX_PATH);
    vx_wcscatn(pat, L"\\*", MAX_PATH);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        SIZE_T l = vx_wcslen(fd.cFileName);
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
        vx_wcscpyn(full, dir, MAX_PATH);
        vx_wcscatn(full, L"\\", MAX_PATH);
        vx_wcscatn(full, fd.cFileName, MAX_PATH);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !is_game_dir(full, fd.cFileName))
                n += process_files(full, w, depth + 1);
            else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                WCHAR m[MAX_PATH + 100];
                wsprintfW(m, L"%s: games or anti-cheat; not changed automatically", full);
                logw(m);
            }
        } else if (is_pe_name(fd.cFileName, l)) {
            vxp_stats st = { 0 };
            DWORD f = w->flags;
            int r;
            if (!vx_wcsnicmp(fd.cFileName + l - 4, L".dll", 4)) f &= ~VXP_FORCE;
            if (is_off(full, FALSE)) continue;
            r = vxp_process_file(full, f | VXP_DRYRUN | VXP_QUIET, NULL, NULL, &st);
            if (r != 1) continue;
            if (too_often(full)) {
                WCHAR m[MAX_PATH + 100];
                wsprintfW(m, L"%s: keeps being put back by the program itself; left alone", full);
                logw(m);
                continue;
            }
            vx_memset(&st, 0, sizeof(st));
            if (vxp_process_file(full, f | VXP_QUIET, log_cb, NULL, &st) < 0) w->failed++;
            else n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* ---- users' own folders ----------------------------------------------------
 * A user can write in their AppData\Local, so the service (SYSTEM) must not
 * work there with its own rights: a planted link could make it write a file
 * the user may not change. Files there are read and written as that user,
 * who must be logged on (that is when they install programs anyway). */
static const root *root_of(const WCHAR *dir)
{
    int i;
    for (i = 0; i < g_nroots; i++) {
        SIZE_T n = vx_wcslen(g_roots[i]->path);
        if (!vx_wcsnicmp(dir, g_roots[i]->path, n) && dir[n] == '\\') return g_roots[i];
    }
    return NULL;
}

/* the command line (not the service) cannot ask for a signed-in user's token
 * (that needs SYSTEM's privilege): a folder of the user running it is done
 * with that user's own token; another user's is left to the service */
static HANDLE own_token_for(const root *r)
{
    HANDLE t;
    WCHAR prof[MAX_PATH];
    DWORD cch = MAX_PATH;
    SIZE_T plen = vx_wcslen(r->profile);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, &t)) return NULL;
    if (GetUserProfileDirectoryW(t, prof, &cch) && vx_wcslen(prof) == plen && !vx_wcsnicmp(prof, r->profile, plen)) return t;
    CloseHandle(t);
    return NULL;
}

static HANDLE user_token(const root *r)        /* NULL: that user is not logged on */
{
    WTS_SESSION_INFOW *s = NULL;
    DWORD n = 0, i;
    HANDLE tok = NULL;
    SIZE_T plen = vx_wcslen(r->profile);
    if (!plen) return NULL;
    if (!g_ssh) return own_token_for(r);
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &s, &n)) return NULL;
    for (i = 0; i < n && !tok; i++) {
        HANDLE t;
        WCHAR prof[MAX_PATH];
        DWORD cch = MAX_PATH;
        if (!WTSQueryUserToken(s[i].SessionId, &t)) continue;
        if (GetUserProfileDirectoryW(t, prof, &cch) && vx_wcslen(prof) == plen && !vx_wcsnicmp(prof, r->profile, plen))
            tok = t;
        else
            CloseHandle(t);
    }
    WTSFreeMemory(s);
    return tok;
}

static BOOL is_link(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static BOOL make_compatible_as(const WCHAR *dir, HANDLE user);

/* make one program folder compatible; FALSE: try again later (file in use,
 * or the owner of the folder is not logged on) */
static BOOL make_compatible(const WCHAR *dir)
{
    const root *r = root_of(dir);
    HANDLE tok = NULL;
    BOOL ok;
    WCHAR m[MAX_PATH + 100];
    if (is_link(dir)) {                         /* a junction: points somewhere else */
        wsprintfW(m, L"%s: is a link to another folder; left alone", dir);
        logw(m);
        return TRUE;
    }
    if (r && r->local) {
        if (!(tok = user_token(r))) {
            wsprintfW(m, g_ssh ? L"%s: its user is not logged on; tried again later"
                               : L"%s: another user's folder; VistaExt's service looks at it", dir);
            logw(m);
            return FALSE;
        }
    }
    ok = make_compatible_as(dir, tok);
    if (tok) CloseHandle(tok);
    return ok;
}

/* file work as "user" (if not NULL); settings are saved as the service */
static BOOL make_compatible_as(const WCHAR *dir, HANDLE user)
{
    WCHAR layer[600], key[MAX_PATH + 2], probe[MAX_PATH + 4], m[MAX_PATH + 700];
    vx_layer l;
    walkctx w = { 0, 0 };
    int n;
    BOOL ok = TRUE;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return TRUE;
    if (is_off(dir, TRUE)) return TRUE;            /* the user undid VistaExt here */
    if (user && !ImpersonateLoggedOnUser(user)) return FALSE;
    vx_wcscpyn(key, dir, MAX_PATH);
    vx_wcscatn(key, L"\\", MAX_PATH + 2);
    vx_wcscpyn(probe, key, MAX_PATH + 4);
    vx_wcscatn(probe, L"x", MAX_PATH + 4);
    layer[0] = 0;
    if (!vx_lookup_layer(probe, layer, 600)) {
        /* nothing saved by the user: the recommended settings, saved only if
         * the program needs VistaExt at all (decided below) */
        vxp_recommend(dir, layer, 600);
    }
    vx_parse_layer(layer, &l);
    w.flags |= vxp_flags_for_layer(layer);
    /* in memory: every program of a folder that needs VistaExt is registered */
    if ((w.flags |= vxp_mode_for(dir)) & VXP_MEMORY) w.flags |= VXP_REGALL;
    /* only programs that need something get settings: check without them */
    {
        vxp_stats st = { 0 };
        if (!vx_lookup_layer(probe, m, 600)) {
            /* does anything in the folder need VistaExt by itself? if not, the
             * program runs as it is and is left completely alone */
            WCHAR pat[MAX_PATH];
            vx_wcscpyn(pat, dir, MAX_PATH);
            if (!vxp_process_dir(pat, VXP_DRYRUN | VXP_QUIET, NULL, NULL, &st) && !st.unsupported) goto done;
            if (layer[0]) {
                HANDLE imp = vxp_unimpersonate();
                if (vx_set_layer(key, layer) == 0) {
                    wsprintfW(m, L"%s: settings %s", dir, layer);
                    logw(m);
                }
                vxp_reimpersonate(imp);
            }
        }
    }
    n = process_files(dir, &w, 0);
    if (n) { wsprintfW(m, L"%s: %d file(s) made compatible", dir, n); logw(m); }
    ok = w.failed == 0;
done:
    if (user) RevertToSelf();
    return ok;
}

/* "<pre><name>\x.exe" (a probe path for app_folder), bounded: FALSE if a
 * folder name is too long for it (NTFS allows 255 characters) */
static BOOL probe_rel(WCHAR *rel, const WCHAR *pre, const WCHAR *name)
{
    if (vx_wcslen(pre) + vx_wcslen(name) + 7 >= MAX_PATH) return FALSE;
    vx_wcscpyn(rel, pre, MAX_PATH);
    vx_wcscatn(rel, name, MAX_PATH);
    vx_wcscatn(rel, L"\\x.exe", MAX_PATH);
    return TRUE;
}

/* ---- folders that changed a lot at once ------------------------------------- */
static BOOL recent(const FILETIME *a, const FILETIME *b, ULONGLONG now)
{
    ULONGLONG x = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime;
    ULONGLONG y = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
    ULONGLONG hour = 3600ULL * 10000000ULL;
    return x + hour > now || y + hour > now;
}

/* Too many changes at once (a big install): Windows says only "overflow",
 * not what changed. Every program folder of this root that was created or
 * changed in the last hour is checked instead. */
static void queue_recent(const root *r)
{
    WCHAR pat[MAX_PATH], rel[MAX_PATH], app[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    FILETIME nowft;
    ULONGLONG now;
    int n = 0;
    GetSystemTimeAsFileTime(&nowft);
    now = ((ULONGLONG)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
    vx_wcscpyn(pat, r->path, MAX_PATH);
    vx_wcscatn(pat, L"\\*", MAX_PATH);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (r->local && !vx_wcsicmp(fd.cFileName, L"Programs")) {
            WIN32_FIND_DATAW f2;
            HANDLE h2;
            WCHAR pat2[MAX_PATH];
            vx_wcscpyn(pat2, r->path, MAX_PATH);
            vx_wcscatn(pat2, L"\\Programs\\*", MAX_PATH);
            if ((h2 = FindFirstFileW(pat2, &f2)) != INVALID_HANDLE_VALUE) {
                do {
                    if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f2.cFileName[0] == '.' ||
                        !recent(&f2.ftCreationTime, &f2.ftLastWriteTime, now)) continue;
                    if (probe_rel(rel, L"Programs\\", f2.cFileName) && app_folder(r, rel, app)) { queue(app); n++; }
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
            continue;
        }
        if (!recent(&fd.ftCreationTime, &fd.ftLastWriteTime, now)) continue;
        if (probe_rel(rel, L"", fd.cFileName) && app_folder(r, rel, app)) { queue(app); n++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    logf(L"many changes at once in %s; %d recently changed folder(s) will be checked", r->path, n);
}

/* a single file to look at (new) or to forget (removed); the latest event
 * for a path wins */
static void queue_file(const WCHAR *path, const root *r, BOOL gone)
{
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_nfiles; i++)
        if (!vx_wcsicmp(g_files[i].path, path)) break;
    if (i == g_nfiles) {
        if (!grow((void **)&g_files, g_nfiles, &g_cfiles, sizeof(pfile), MAXFILES, L"new or removed files")) {
            LeaveCriticalSection(&g_cs);
            return;
        }
        vx_wcscpyn(g_files[i].path, path, MAX_PATH);
        g_nfiles++;
    }
    g_files[i].last = GetTickCount();
    g_files[i].gone = gone;
    g_files[i].r = r;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_work);
}

/* places in a user's AppData\Local where setups run from: Temp (a setup's
 * own unpacked helpers, a downloader's real installer) and Internet
 * Explorer's download cache ("Run" starts the file from there) */
static BOOL loose_place(const root *r, const WCHAR *rel)
{
    static const WCHAR *pl[] = { L"Temp\\", L"Microsoft\\Windows\\Temporary Internet Files\\" };
    unsigned i;
    if (!r->local) return FALSE;
    for (i = 0; i < sizeof(pl) / sizeof(pl[0]); i++) if (!vx_wcsnicmp(rel, pl[i], vx_wcslen(pl[i]))) return TRUE;
    return FALSE;
}

/* ---- watching ----------------------------------------------------------------
 * One thread watches up to 63 folders (overlapped reads, one event each).
 * 64 KB per folder: a bigger burst only means "overflow", which is handled
 * (queue_recent). Only names and last-write times are watched: a size change
 * comes with a write anyway, and browsers' cache files would otherwise wake
 * the service with every write. */
#define WATCH_BUF (64 * 1024)
#define WATCH_WHAT (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)

static BOOL watch_start(root *r)
{
    vx_memset(&r->ov, 0, sizeof(r->ov));
    r->ov.hEvent = r->ev;
    return ReadDirectoryChangesW(r->dir, r->buf, WATCH_BUF, TRUE, WATCH_WHAT, NULL, &r->ov, NULL);
}

static void watch_handle(root *r, DWORD got)
{
    FILE_NOTIFY_INFORMATION *fi = (FILE_NOTIFY_INFORMATION *)r->buf;
    if (!got) { if (!r->loose) queue_recent(r); return; }                 /* overflow */
    for (;;) {
        WCHAR rel[MAX_PATH], app[MAX_PATH];
        SIZE_T n = fi->FileNameLength / sizeof(WCHAR);
        BOOL removed = fi->Action == FILE_ACTION_REMOVED || fi->Action == FILE_ACTION_RENAMED_OLD_NAME;
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        vx_memcpy(rel, fi->FileName, n * sizeof(WCHAR));
        rel[n] = 0;
        if (is_pe_name(rel, n)) {
            WCHAR full[MAX_PATH];
            vx_wcscpyn(full, r->path, MAX_PATH);
            vx_wcscatn(full, L"\\", MAX_PATH);
            vx_wcscatn(full, rel, MAX_PATH);
            if (removed) queue_file(full, r, TRUE);               /* uninstalled? */
            else if (r->loose || loose_place(r, rel)) {
                if (!vx_wcsnicmp(rel + n - 4, L".exe", 4)) queue_file(full, r, FALSE);
            } else if (app_folder(r, rel, app))
                queue(app);
        }
        if (!fi->NextEntryOffset) break;
        fi = (FILE_NOTIFY_INFORMATION *)((BYTE *)fi + fi->NextEntryOffset);
    }
}

typedef struct { int from, to; } watch_group;

static DWORD WINAPI watcher(LPVOID p)
{
    watch_group *g = p;
    HANDLE hs[WATCH_GROUP + 1];
    root *rs[WATCH_GROUP];
    int i, n = 0;
    hs[0] = g_stop;
    for (i = g->from; i < g->to; i++) {
        root *r = g_roots[i];
        r->dir = CreateFileW(r->path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        r->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        r->buf = HeapAlloc(GetProcessHeap(), 0, WATCH_BUF);
        if (r->dir == INVALID_HANDLE_VALUE || !r->ev || !r->buf || !watch_start(r)) {
            logf(L"%s: cannot be watched (error %u)", r->path, GetLastError());
            continue;
        }
        rs[n] = r;
        hs[++n] = r->ev;
    }
    HeapFree(GetProcessHeap(), 0, g);
    while (n) {
        DWORD w = WaitForMultipleObjects(n + 1, hs, FALSE, INFINITE), got;
        DWORD k;
        root *r;
        if (w == WAIT_OBJECT_0 || w > WAIT_OBJECT_0 + (DWORD)n) break;   /* stop, or a wait error */
        k = w - WAIT_OBJECT_0 - 1;
        r = rs[k];
        if (GetOverlappedResult(r->dir, &r->ov, &got, FALSE)) watch_handle(r, got);
        ResetEvent(r->ev);
        if (!watch_start(r)) {                        /* the folder is gone: stop watching it */
            logf(L"%s: no longer watched (error %u)", r->path, GetLastError());
            for (i = (int)k; i < n - 1; i++) { rs[i] = rs[i + 1]; hs[i + 1] = hs[i + 2]; }
            n--;
        }
    }
    for (i = 0; i < n; i++) CancelIo(rs[i]->dir);
    return 0;
}

/* watch the folders added since the last call (a thread per 63) */
static void watch_new(HANDLE *th, int *nth, int maxth)
{
    while (g_nwatched < g_nroots && *nth < maxth) {
        watch_group *g = HeapAlloc(GetProcessHeap(), 0, sizeof(*g));
        if (!g) return;
        g->from = g_nwatched;
        g->to = g_nroots - g_nwatched > WATCH_GROUP ? g_nwatched + WATCH_GROUP : g_nroots;
        g_nwatched = g->to;
        if ((th[*nth] = CreateThread(NULL, 0, watcher, g, 0, NULL))) (*nth)++;
        else HeapFree(GetProcessHeap(), 0, g);
    }
}

static void add_root(const WCHAR *p, const WCHAR *profile, BOOL loose)
{
    root *r;
    int i;
    if (!p[0] || GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    for (i = 0; i < g_nroots; i++) if (!vx_wcsicmp(g_roots[i]->path, p)) return;      /* already watched */
    if (g_nroots >= MAXROOTS || !(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r)))) return;
    vx_wcscpyn(r->path, p, MAX_PATH);
    vx_wcscpyn(r->profile, profile ? profile : L"", MAX_PATH);
    r->local = profile != NULL;
    r->loose = loose;
    r->dir = INVALID_HANDLE_VALUE;
    g_roots[g_nroots] = r;
    InterlockedIncrement(&g_nroots);          /* after the entry is complete (a full barrier) */
}

/* a profile's folders: programs installed per user (AppData\Local and
 * \Roaming) and where setups land (Downloads, Desktop). "token": the user is
 * signed in, so their real folders are known (they may be moved elsewhere);
 * otherwise the standard places in the profile. */
static void add_profile(const WCHAR *prof, HANDLE token)
{
    static const struct { const GUID *id; const WCHAR *sub; BOOL loose; } f[] = {
        { &FOLDERID_LocalAppData, L"\\AppData\\Local", FALSE },
        { &FOLDERID_RoamingAppData, L"\\AppData\\Roaming", FALSE },
        { &FOLDERID_Downloads, L"\\Downloads", TRUE },
        { &FOLDERID_Desktop, L"\\Desktop", TRUE },
    };
    unsigned i;
    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        WCHAR p[MAX_PATH], *kp = NULL;
        p[0] = 0;
        if (token && SUCCEEDED(SHGetKnownFolderPath(f[i].id, 0, token, &kp)) && kp) vx_wcscpyn(p, kp, MAX_PATH);
        if (kp) CoTaskMemFree(kp);
        if (!p[0]) { vx_wcscpyn(p, prof, MAX_PATH); vx_wcscatn(p, f[i].sub, MAX_PATH); }
        add_root(p, prof, f[i].loose);
    }
}

/* Program Files, and every user profile Windows knows (its ProfileList, so a
 * profile folder elsewhere than C:\Users is found too). System accounts
 * (S-1-5-18/19/20) have no programs of their own. */
static void find_roots(void)
{
    WCHAR p[MAX_PATH], sid[200];
    HKEY pl;
    DWORD i;
    if (GetEnvironmentVariableW(L"ProgramFiles", p, MAX_PATH)) add_root(p, NULL, FALSE);
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", p, MAX_PATH)) add_root(p, NULL, FALSE);
    if (GetEnvironmentVariableW(L"ProgramW6432", p, MAX_PATH)) add_root(p, NULL, FALSE);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0,
                      KEY_READ | VX_REG_VIEW, &pl)) return;
    for (i = 0; ; i++) {
        DWORD n = 200, cb, type;
        WCHAR raw[MAX_PATH], prof[MAX_PATH];
        HKEY k;
        if (RegEnumKeyExW(pl, i, sid, &n, NULL, NULL, NULL, NULL)) break;
        if (vx_wcsnicmp(sid, L"S-1-5-21-", 9)) continue;                  /* only real users */
        if (RegOpenKeyExW(pl, sid, 0, KEY_QUERY_VALUE, &k)) continue;
        cb = sizeof(raw) - sizeof(WCHAR);
        if (!RegQueryValueExW(k, L"ProfileImagePath", NULL, &type, (BYTE *)raw, &cb) &&
            (type == REG_EXPAND_SZ || type == REG_SZ)) {
            raw[cb / sizeof(WCHAR)] = 0;
            if (ExpandEnvironmentStringsW(raw, prof, MAX_PATH) && GetFileAttributesW(prof) != INVALID_FILE_ATTRIBUTES)
                add_profile(prof, NULL);
        }
        RegCloseKey(k);
    }
    RegCloseKey(pl);
}

/* every signed-in user: their real folders (moved Downloads, a new profile) */
static void find_session_roots(void)
{
    WTS_SESSION_INFOW *ss = NULL;
    DWORD n = 0, i;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &ss, &n)) return;
    for (i = 0; i < n; i++) {
        HANDLE t;
        WCHAR prof[MAX_PATH];
        DWORD cch = MAX_PATH;
        if (!WTSQueryUserToken(ss[i].SessionId, &t)) continue;
        if (GetUserProfileDirectoryW(t, prof, &cch)) add_profile(prof, t);
        CloseHandle(t);
    }
    WTSFreeMemory(ss);
}

/* every program folder in every root (first start, and /scan) */
static void scan_all(void)
{
    int i;
    logw(L"scanning installed programs");
    for (i = 0; i < g_nroots; i++) {
        WCHAR pat[MAX_PATH], rel[MAX_PATH], app[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (g_roots[i]->loose) {                 /* setups downloaded before VistaExt was installed */
            vx_wcscpyn(pat, g_roots[i]->path, MAX_PATH);
            vx_wcscatn(pat, L"\\*.exe", MAX_PATH);
            if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                vx_wcscpyn(rel, g_roots[i]->path, MAX_PATH);
                vx_wcscatn(rel, L"\\", MAX_PATH);
                vx_wcscatn(rel, fd.cFileName, MAX_PATH);
                new_file(rel, g_roots[i]);
            } while (FindNextFileW(h, &fd) && (!g_stop || WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0));
            FindClose(h);
            continue;
        }
        vx_wcscpyn(pat, g_roots[i]->path, MAX_PATH);
        vx_wcscatn(pat, L"\\*", MAX_PATH);
        if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (g_roots[i]->local && !vx_wcsicmp(fd.cFileName, L"Programs")) {
                WIN32_FIND_DATAW f2;
                HANDLE h2;
                WCHAR pat2[MAX_PATH];
                vx_wcscpyn(pat2, g_roots[i]->path, MAX_PATH);
                vx_wcscatn(pat2, L"\\Programs\\*", MAX_PATH);
                if ((h2 = FindFirstFileW(pat2, &f2)) != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f2.cFileName[0] == '.') continue;
                        if (probe_rel(rel, L"Programs\\", f2.cFileName) && app_folder(g_roots[i], rel, app)) make_compatible(app);
                    } while (FindNextFileW(h2, &f2) && WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0);
                    FindClose(h2);
                }
                continue;
            }
            if (probe_rel(rel, L"", fd.cFileName) && app_folder(g_roots[i], rel, app)) make_compatible(app);
        } while (FindNextFileW(h, &fd) && (!g_stop || WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0));
        FindClose(h);
    }
    logw(L"scan finished");
}

/* milliseconds until the next queued item is due; INFINITE if none */
static DWORD next_due(BOOL later)
{
    DWORD now = GetTickCount(), best = INFINITE;
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_nfiles; i++) {
        LONG left = (LONG)(g_files[i].last + (g_files[i].gone ? GONE_MS : LOOSE_MS) - now);
        DWORD d = left > 0 ? (DWORD)left : 0;
        if (d < best) best = d;
    }
    for (i = 0; i < g_npend; i++) {
        LONG left = (LONG)(g_pend[i].last + QUIET_MS - now);   /* signed: "last" may be in the future (a retry) */
        DWORD d = left > 0 ? (DWORD)left : 0;
        if (d < best) best = d;
    }
    LeaveCriticalSection(&g_cs);
    if (later && best > 10000) best = 10000;
    return best;
}

/* single files whose time has come: new setups (2 s after they were
 * written), removed programs (30 s after, so that an update replacing a
 * file does not count as removed) */
static void do_due_files(void)
{
    BOOL forgot = FALSE;
    for (;;) {
        pfile f;
        BOOL got = FALSE;
        int i;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < g_nfiles; i++)
            if ((LONG)(GetTickCount() - g_files[i].last) >= (g_files[i].gone ? GONE_MS : LOOSE_MS)) {
                f = g_files[i];
                g_files[i] = g_files[--g_nfiles];
                got = TRUE;
                break;
            }
        LeaveCriticalSection(&g_cs);
        if (!got) break;
        if (f.gone) {
            if (GetFileAttributesW(f.path) == INVALID_FILE_ATTRIBUTES) {
                lock();
                if (vxp_forget(f.path, log_cb, NULL)) forgot = TRUE;
                unlock();
            }
        } else new_file(f.path, f.r);
    }
    if (forgot) { lock(); vxp_cleanup_missing(log_cb, NULL); unlock(); }
}

/* program folders quiet for 20 s: made compatible; one with a file in use
 * (the program runs) is tried again a minute later, 10 times at most */
static void do_due_folders(void)
{
    for (;;) {
        WCHAR dir[MAX_PATH];
        int i, tries = 0;
        BOOL found = FALSE;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < g_npend; i++)
            if ((LONG)(GetTickCount() - g_pend[i].last) >= QUIET_MS) {
                vx_wcscpyn(dir, g_pend[i].dir, MAX_PATH);
                tries = g_pend[i].tries;
                g_pend[i] = g_pend[--g_npend];
                found = TRUE;
                break;
            }
        LeaveCriticalSection(&g_cs);
        if (!found) return;
        if (!auto_on()) continue;
        if (!make_compatible(dir)) {
            if (tries < 10) queue_at(dir, GetTickCount() + 40000, tries + 1);
            else logf(L"%s: still in use after 10 tries; left until it changes again", dir);
        }
    }
}

static DWORD WINAPI worker(LPVOID p)
{
    HKEY k;
    DWORD v = 0, cb = sizeof(v), type;
    BOOL scanned = FALSE;
    /* below-normal CPU priority, but normal disk priority: "background mode"
     * made Vista take minutes per big program while anything else used the
     * disk, and the user usually starts the program right after installing */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
        scanned = !RegQueryValueExW(k, L"AutoScanned", NULL, &type, (BYTE *)&v, &cb) && v;
        RegCloseKey(k);
    }
    if (!scanned && auto_on()) {
        scan_all();
        if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL)) {
            v = 1;
            RegSetValueExW(k, L"AutoScanned", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
            RegCloseKey(k);
        }
    }
    /* Sleep until something is due: a queued item's time, something newly
     * queued (g_work), or a change of VistaExt's settings (switched off from
     * the command line). Nothing queued, nothing to wait for: no wake-ups. */
    {
        HKEY rk = NULL;
        HANDLE regev = CreateEventW(NULL, FALSE, FALSE, NULL), hs[3];
        DWORD last_later = GetTickCount() - 10000;
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_NOTIFY | VX_REG_VIEW, &rk);
        if (rk && regev) RegNotifyChangeKeyValue(rk, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, regev, TRUE);
        hs[0] = g_stop; hs[1] = g_work; hs[2] = regev;
        for (;;) {
            /* switched off while changed programs ran: they are put back once
             * closed. Windows has no event for "this file is no longer in
             * use", so while one waits it is tried every 10 s (only then). */
            BOOL later = !vxp_enabled() && list_count(VX_REG_LATER) > 0;
            DWORD w = WaitForMultipleObjects(regev ? 3 : 2, hs, FALSE, next_due(later));
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
            if (w == WAIT_OBJECT_0 + 2 && rk)
                RegNotifyChangeKeyValue(rk, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, regev, TRUE);
            do_due_files();
            if (later && GetTickCount() - last_later >= 9000) {
                last_later = GetTickCount();
                lock(); restore_later(); unlock();
            }
            do_due_folders();
        }
        if (rk) RegCloseKey(rk);
        if (regev) CloseHandle(regev);
    }
    return 0;
}

/* ---- a single program: a setup in Downloads, or a program about to start --- */
static void file_dir(const WCHAR *path, WCHAR *dir)
{
    SIZE_T n;
    vx_wcscpyn(dir, path, MAX_PATH);
    n = vx_wcslen(dir);
    while (n && dir[n - 1] != '\\') n--;
    if (n) dir[n - 1] = 0;
}

static const WCHAR *base_name(const WCHAR *p)
{
    const WCHAR *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static BOOL under(const WCHAR *path, const WCHAR *dir)
{
    SIZE_T n = vx_wcslen(dir);
    return n && !vx_wcsnicmp(path, dir, n) && (path[n] == '\\' || dir[n - 1] == '\\');
}

/* Windows' folder, and VistaExt's own */
static BOOL system_place(const WCHAR *path)
{
    WCHAR w[MAX_PATH], me[MAX_PATH];
    if (GetWindowsDirectoryW(w, MAX_PATH) && under(path, w)) return TRUE;
    GetModuleFileNameW(NULL, me, MAX_PATH);
    file_dir(me, w);
    return under(path, w);
}

/* ---- setup programs ------------------------------------------------------------
 * Some setups run on this Windows as they are but refuse it ("requires
 * Windows 10"). Like Windows' own installer detection, VistaExt recognises
 * setup programs (name, version information, installer engine) and lets
 * them see the chosen Windows version too - but never Microsoft's own
 * (runtimes, updates: they choose what to install by the real Windows) and
 * never driver packages. A setup told another version that tries to install
 * a driver Windows loads while starting is stopped (advapivx), so a driver
 * made for another Windows cannot keep this one from starting.
 * HKLM\SOFTWARE\VistaExt "Setups" = 0 turns this off (tray icon menu). */
static BOOL setups_on(void)
{
    WCHAR g[64];
    vx_layer l;
    if (!vx_setting(L"Setups", 1) || !vx_global_version(g, 64)) return FALSE;
    vx_parse_layer(g, &l);
    return l.ver != NULL;                     /* "this Windows' own version": nothing to tell */
}

static BOOL contains_ci(const WCHAR *s, const WCHAR *w)
{
    SIZE_T n = vx_wcslen(w);
    for (; s && *s; s++) if (!vx_wcsnicmp(s, w, n)) return TRUE;
    return FALSE;
}

/* the version information strings that describe a file */
static BOOL version_strings_have(const WCHAR *path, const WCHAR **words, int nw)
{
    static const WCHAR *fields[] = { L"FileDescription", L"ProductName", L"InternalName", L"OriginalFilename" };
    DWORD h, size = GetFileVersionInfoSizeW(path, &h);
    BYTE *d;
    struct { WORD lang, cp; } *tr;
    UINT n, f, i;
    BOOL r = FALSE;
    if (!size || size > 1 << 20 || !(d = HeapAlloc(GetProcessHeap(), 0, size))) return FALSE;
    if (GetFileVersionInfoW(path, 0, size, d) && VerQueryValueW(d, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= 4) {
        for (f = 0; f < 4 && !r; f++) {
            WCHAR q[80], *v;
            UINT vl;
            wsprintfW(q, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, fields[f]);
            if (VerQueryValueW(d, q, (void **)&v, &vl) && vl)
                for (i = 0; i < (UINT)nw && !r; i++) r = contains_ci(v, words[i]);
        }
    }
    HeapFree(GetProcessHeap(), 0, d);
    return r;
}

/* the program's manifest names an installer engine */
static BOOL engine_manifest(const WCHAR *path)
{
    static const char *marks[] = { "Nullsoft", "JR.Inno.Setup", "InstallShield", "Squirrel", "WixBundle", "setup" };
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    HRSRC r;
    BOOL found = FALSE;
    if (!m) return FALSE;
    if ((r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24)))) {
        DWORD n = SizeofResource(m, r), i, j;
        const char *t = LockResource(LoadResource(m, r));
        for (j = 0; t && j < sizeof(marks) / sizeof(marks[0]) && !found; j++) {
            SIZE_T ml = vx_strlen(marks[j]);
            for (i = 0; i + ml <= n && !found; i++) {
                SIZE_T k = 0;
                while (k < ml && (t[i + k] | 32) == (marks[j][k] | 32)) k++;
                found = k == ml;
            }
        }
    }
    FreeLibrary(m);
    return found;
}

static BOOL is_setup(const WCHAR *path)
{
    static const WCHAR *words[] = { L"setup", L"install", L"instal", L"update", L"upgrade", L"bootstrap" };
    const WCHAR *b = base_name(path);
    int i;
    for (i = 0; i < 6; i++) if (contains_ci(b, words[i])) return TRUE;
    return version_strings_have(path, words, 6) || engine_manifest(path);
}

static BOOL looks_like_driver(const WCHAR *path)
{
    static const WCHAR *words[] = { L"driver" };
    return contains_ci(base_name(path), L"driver") || version_strings_have(path, words, 1);
}

/* a program's own signature: the PKCS#7 block the header's security
 * directory points at (a file offset). Only the headers and that block are
 * read - Windows' own CryptQueryObject on the file reads all of it, a minute
 * for a 200 MB setup on a slow disk. NULL: not signed this way. */
static BYTE *signature_block(const WCHAR *path, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE hdr[1024], *blob = NULL;
    DWORD got = 0, size, nt, off = 0, cb = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (ReadFile(f, hdr, sizeof(hdr), &got, NULL) && got >= 0x40 && hdr[0] == 'M' && hdr[1] == 'Z' &&
        (nt = *(DWORD *)(hdr + 0x3C)) <= got - 0x100 && *(DWORD *)(hdr + nt) == IMAGE_NT_SIGNATURE) {
        WORD magic = *(WORD *)(hdr + nt + 24);
        /* DataDirectory[4] (security): after the optional header's fixed part */
        DWORD dd = nt + 24 + (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? 112 : 96) + 4 * 8;
        if (dd + 8 <= got) { off = *(DWORD *)(hdr + dd); cb = *(DWORD *)(hdr + dd + 4); }
    }
    /* a WIN_CERTIFICATE: length, revision, type 2 (PKCS#7 signed data), the block */
    if (off && cb > 8 && cb <= (4u << 20) && off < size && cb <= size - off && (blob = HeapAlloc(GetProcessHeap(), 0, cb))) {
        if (SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) != off || !ReadFile(f, blob, cb, &got, NULL) || got != cb ||
            *(WORD *)(blob + 6) != WIN_CERT_TYPE_PKCS_SIGNED_DATA || *(DWORD *)blob < 8 || *(DWORD *)blob > cb) {
            HeapFree(GetProcessHeap(), 0, blob);
            blob = NULL;
        } else *len = *(DWORD *)blob;
    }
    CloseHandle(f);
    return blob;
}

/* the organization in a file's own (embedded) signature: 1 Microsoft,
 * 0 someone else, -1 not signed this way. The file is not hashed. */
static int embedded_by_microsoft(const WCHAR *path)
{
    HCERTSTORE store = NULL;
    HCRYPTMSG msg = NULL;
    DWORD enc = 0, ct, ft, n = 0, len = 0;
    CERT_BLOB cb;
    BYTE *blob = signature_block(path, &len);
    int r = 0;
    if (!blob) return -1;
    cb.cbData = len - 8;
    cb.pbData = blob + 8;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &cb, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED, CERT_QUERY_FORMAT_FLAG_BINARY,
                          0, &enc, &ct, &ft, &store, &msg, NULL)) {
        HeapFree(GetProcessHeap(), 0, blob);
        return -1;
    }
    HeapFree(GetProcessHeap(), 0, blob);
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &n) && n) {
        CMSG_SIGNER_INFO *si = HeapAlloc(GetProcessHeap(), 0, n);
        if (si && CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &n)) {
            CERT_INFO cinfo;
            PCCERT_CONTEXT c;
            vx_memset(&cinfo, 0, sizeof(cinfo));
            cinfo.Issuer = si->Issuer;
            cinfo.SerialNumber = si->SerialNumber;
            if ((c = CertFindCertificateInStore(store, enc, 0, CERT_FIND_SUBJECT_CERT, &cinfo, NULL))) {
                WCHAR org[256] = L"";
                CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, org, 256);
                r = contains_ci(org, L"Microsoft");
                CertFreeCertificateContext(c);
            }
        }
        if (si) HeapFree(GetProcessHeap(), 0, si);
    }
    if (store) CertCloseStore(store, 0);
    if (msg) CryptMsgClose(msg);
    return r;
}

/* signed by Microsoft: its own signature names Microsoft, or (no signature
 * of its own) Windows' catalogs list it. The signature is looked at first:
 * the catalog lookup hashes the whole file. */
static BOOL by_microsoft(const WCHAR *path)
{
    HCATADMIN ca;
    BOOL r = FALSE;
    int e = embedded_by_microsoft(path);
    if (e >= 0) return e == 1;
    if (CryptCATAdminAcquireContext(&ca, NULL, 0)) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            BYTE hash[64];
            DWORD hl = sizeof(hash);
            /* read once in 1 MB pieces first (one buffer, reused), so that
             * Windows' hash takes the file from the cache: on a slow disk its
             * own smaller reads take several times as long */
            BYTE *buf = HeapAlloc(GetProcessHeap(), 0, 1 << 20);
            DWORD got;
            if (buf) {
                while (ReadFile(f, buf, 1 << 20, &got, NULL) && got) ;
                HeapFree(GetProcessHeap(), 0, buf);
                SetFilePointer(f, 0, NULL, FILE_BEGIN);
            }
            if (CryptCATAdminCalcHashFromFileHandle(f, &hl, hash, 0)) {
                HCATINFO ci = CryptCATAdminEnumCatalogFromHash(ca, hash, hl, 0, NULL);
                if (ci) { r = TRUE; CryptCATAdminReleaseCatalogContext(ca, ci, 0); }
            }
            CloseHandle(f);
        }
        CryptCATAdminReleaseContext(ca, 0);
    }
    return r;
}

/* written in the last hour (part of what a setup is installing right now) */
static BOOL recent_file(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME now;
    ULONGLONG c, w, t;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return FALSE;
    GetSystemTimeAsFileTime(&now);
    t = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    c = ((ULONGLONG)a.ftCreationTime.dwHighDateTime << 32) | a.ftCreationTime.dwLowDateTime;
    w = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    if (w > c) c = w;
    return c + 3600ULL * 10000000ULL > t;
}

/* 1: VistaExt runs it (now, or already), 0: it runs as it is, -1: refused.
 * from_setup: a setup VistaExt runs is about to start it; waited_for: a
 * program waits for the answer (PREPARE) */
/* a step of looking at a program that took long (a slow disk, a huge file):
 * logged, so that the log says where the time went */
static void slow_step(const WCHAR *path, const WCHAR *step, DWORD since)
{
    DWORD ms = GetTickCount() - since;
    if (ms >= 2000) logf(L"%s: %s took %u ms", path, step, ms);
}

static int handle_single(const WCHAR *path, const WCHAR *why, BOOL from_setup, BOOL waited_for)
{
    WCHAR dir[MAX_PATH], layer[600];
    vxp_stats st = { 0 };
    SIZE_T n = vx_wcslen(path);
    DWORD a = GetFileAttributesW(path), flags;
    BOOL own, need, setup = FALSE;
    int r;
    if (n < 8 || path[1] != ':' || path[2] != '\\' || vx_wcsnicmp(path + n - 4, L".exe", 4)) return 0;
    if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return 0;
    if (system_place(path) || is_off(path, FALSE)) return 0;
    if (vxp_memory_registered(path)) return 1;
    file_dir(path, dir);
    /* can it run on this Windows by itself? (only what the file needs) */
    {
        DWORD t = GetTickCount();
        need = vxp_process_file(path, VXP_DRYRUN | VXP_QUIET, NULL, NULL, &st) == 1;
        slow_step(path, L"checking its imports", t);
    }
    if (!need) {
        /* a setup that refuses this Windows can still be told the chosen
         * version: one the user downloaded, or a new helper of a setup */
        DWORD t = GetTickCount();
        BOOL su = setups_on() && (from_setup ? recent_file(path) : is_setup(path)), ms;
        slow_step(path, L"recognising a setup", t);
        if (!su) return 0;
        t = GetTickCount();
        ms = by_microsoft(path);
        slow_step(path, L"checking its signature", t);
        if (ms) {
            logf(L"%s (%s): Microsoft's own setup: it sees this Windows' real version", path, why);
            return 0;
        }
        if (looks_like_driver(path)) {
            logf(L"%s (%s): a driver package: it sees this Windows' real version", path, why);
            return 0;
        }
        setup = TRUE;
    }
    if (is_game_dir(dir, NULL)) {
        logf(L"%s (%s): a game or anti-cheat folder; not changed automatically", path, why);
        return 0;
    }
    if (vxp_windows_name(base_name(path))) {
        logf(L"%s (%s): Windows has a program of the same name; left alone", path, why);
        return -1;
    }
    own = vx_lookup_layer(path, layer, 600);
    if (!own) vxp_recommend(path, layer, 600);
    flags = vxp_flags_for_layer(layer) | VXP_MEMORY | VXP_QUIET;
    if (setup) flags |= VXP_FORCE;                         /* registered although nothing is missing */
    if (setup || from_setup || is_setup(path)) flags |= VXP_SETUP;
    vx_memset(&st, 0, sizeof(st));
    logf(setup ? L"%s (%s): a setup program; it is told the chosen Windows version"
               : L"%s (%s): cannot run on this Windows by itself; made compatible", path, why);
    /* a program is waiting to start this one: if a switch or a scan holds
     * the lock, it starts as it is rather than waiting for them */
    if (!lock_for(waited_for ? 5000 : INFINITE)) {
        logf(L"%s: VistaExt was busy (switching or scanning); it starts as it is this time", path);
        return 0;
    }
    {
        DWORD t = GetTickCount();
        r = vxp_process_file(path, flags, log_cb, NULL, &st);
        slow_step(path, L"registering it", t);
    }
    unlock();
    if (r >= 0 && !own && layer[0]) {
        HANDLE imp = vxp_unimpersonate();
        LONG e = vx_set_layer(path, layer);
        vxp_reimpersonate(imp);
        reg_failed(e, VX_REG_LAYERS, path);
    }
    return r < 0 ? -1 : 1;
}

/* a new single file, looked at as its user */
static void new_file(const WCHAR *path, const root *r)
{
    HANDLE tok = NULL;
    if (!auto_on() || is_link(path)) return;
    if (r && r->local) {
        if (!(tok = user_token(r))) return;          /* not logged on: looked at when the scan runs */
        if (!ImpersonateLoggedOnUser(tok)) { CloseHandle(tok); return; }
    }
    handle_single(path, L"new file", FALSE, FALSE);
    if (tok) { RevertToSelf(); CloseHandle(tok); }
}

/* ---- the on/off switch --------------------------------------------------------
 * VistaExt's files in System32/SysWOW64 (listed in installed.txt) move into
 * a subfolder "VistaExt.off" of the same folder (same disk, so a file a
 * running program has loaded can be moved too), and back. */
#define VX_REG_REPATCH VX_REG_ROOT L"\\Repatch"

static void tool_dir(WCHAR *out)
{
    WCHAR me[MAX_PATH];
    GetModuleFileNameW(NULL, me, MAX_PATH);
    file_dir(me, out);
}

static BOOL has_marker(const WCHAR *path)
{
    static const char mark[] = "VistaExt-owned-file";
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL), m;
    const BYTE *d;
    DWORD size, i, ml = sizeof(mark) - 1;
    BOOL r = FALSE;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size > ml && (m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL))) {
        if ((d = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0))) {
            for (i = 0; i + ml <= size && !r; i++) if (d[i] == 'V' && !memcmp(d + i, mark, ml)) r = TRUE;
            UnmapViewOfFile(d);
        }
        CloseHandle(m);
    }
    CloseHandle(f);
    return r;
}

static void off_name(const WCHAR *path, WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    file_dir(path, dir);
    wsprintfW(out, L"%s\\VistaExt.off\\%s", dir, base_name(path));
}

typedef struct { int moved, kept, busy, reboot; } switch_stats;

/* every line of installed.txt (paths of the files VistaExt added to Windows).
 * install.cmd writes it with "echo", in the console's code page (OEM), and
 * uninstall.cmd reads it with "for /f" and findstr, which cannot read UTF-16:
 * so it stays in the OEM code page and is read as such. The paths are under
 * the Windows folder. */
static WCHAR *installed_list(void)
{
    WCHAR p[MAX_PATH + 20], *w;
    HANDLE f;
    DWORD size, got = 0, i, o = 0;
    char *b;
    tool_dir(p);
    vx_wcscatn(p, L"\\installed.txt", MAX_PATH + 20);
    f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    b = HeapAlloc(GetProcessHeap(), 0, size + 1);
    w = HeapAlloc(GetProcessHeap(), 0, (size + 2) * sizeof(WCHAR));
    if (!b || !w || !ReadFile(f, b, size, &got, NULL)) got = 0;
    CloseHandle(f);
    if (b) {
        int n = MultiByteToWideChar(CP_OEMCP, 0, b, (int)got, w, (int)size + 1);
        HeapFree(GetProcessHeap(), 0, b);
        if (n < 0) n = 0;
        /* lines -> "a\0b\0\0" */
        for (i = 0; i < (DWORD)n; i++) {
            if (w[i] == '\r') continue;
            w[o++] = w[i] == '\n' ? 0 : w[i];
            if (w[i] == '\n' && (o == 1 || !w[o - 2])) o--;          /* empty line */
        }
    }
    if (!w) return NULL;
    if (o && w[o - 1]) w[o++] = 0;
    w[o] = 0;
    return w;
}

static void move_system_files(BOOL on, switch_stats *ss)
{
    WCHAR *list = installed_list(), *p, off[MAX_PATH + 20], dir[MAX_PATH + 20];
    if (!list) { logw(L"installed.txt not found: VistaExt's system files left where they are"); return; }
    for (p = list; *p; p += vx_wcslen(p) + 1) {
        off_name(p, off);
        if (!on) {
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
            if (!has_marker(p)) { ss->kept++; continue; }             /* Windows' own now: never moved */
            file_dir(off, dir);
            CreateDirectoryW(dir, NULL);
            if (MoveFileExW(p, off, MOVEFILE_REPLACE_EXISTING)) ss->moved++;
            else if (MoveFileExW(p, off, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
                logf(L"%s: in use; moved when Windows restarts (error %u)", p, GetLastError());
                ss->reboot++;
            } else { logf(L"%s: could not be moved (error %u)", p, GetLastError()); ss->busy++; }
        } else {
            if (GetFileAttributesW(off) == INVALID_FILE_ATTRIBUTES) continue;
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
                /* already back, or Windows has its own file of that name now */
                if (!has_marker(p)) logf(L"%s: Windows has its own file of this name now; VistaExt's is not put back", p);
                else DeleteFileW(off);
                ss->kept++;
                continue;
            }
            if (MoveFileExW(off, p, 0)) ss->moved++;
            else { logf(L"%s: could not be put back (error %u)", p, GetLastError()); ss->busy++; }
        }
    }
    /* the holding folders, once empty */
    for (p = list; *p; p += vx_wcslen(p) + 1) {
        off_name(p, off);
        file_dir(off, dir);
        if (on) RemoveDirectoryW(dir);
    }
    HeapFree(GetProcessHeap(), 0, list);
}

/* VistaExt's own registry entries: a failure is logged (the change it was
 * part of is then incomplete; the log says which) */
static void reg_failed(LONG e, const WCHAR *key, const WCHAR *name)
{
    if (e) logf(L"could not write %s\\%s (error %d)", key, name && name[0] ? name : L"(default)", (int)e);
}

static void set_dword(const WCHAR *name, DWORD v)
{
    HKEY k;
    LONG e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL);
    if (!e) {
        e = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
    reg_failed(e, VX_REG_ROOT, name);
}

/* "a restart is needed": the time it was asked for, cleared once Windows has
 * started since */
static void need_restart(void)
{
    HKEY k;
    FILETIME t;
    LONG e;
    GetSystemTimeAsFileTime(&t);
    e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL);
    if (!e) {
        e = RegSetValueExW(k, L"RestartNeeded", 0, REG_QWORD, (const BYTE *)&t, sizeof(t));
        RegCloseKey(k);
    }
    reg_failed(e, VX_REG_ROOT, L"RestartNeeded");
}

static BOOL restart_needed(void)
{
    int r = vx_restart_state();
    HKEY k;
    if (r < 0 && !RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, KEY_SET_VALUE | VX_REG_VIEW, &k)) {
        RegDeleteValueW(k, L"RestartNeeded");                 /* Windows has restarted since */
        RegCloseKey(k);
    }
    return r > 0;
}

static void list_set_data(const WCHAR *key, const WCHAR *path, BOOL add, const WCHAR *data)
{
    HKEY k;
    LONG e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL);
    if (!e) {
        if (add) e = RegSetValueExW(k, path, 0, REG_SZ, (const BYTE *)data, (DWORD)((vx_wcslen(data) + 1) * sizeof(WCHAR)));
        else if ((e = RegDeleteValueW(k, path)) == ERROR_FILE_NOT_FOUND) e = 0;    /* not listed: nothing to remove */
        RegCloseKey(k);
    }
    reg_failed(e, key, path);
}

#define list_set(key, p, add) list_set_data(key, p, add, L"")
/* a file the switch put back; "memory": it was registered for the
 * in-memory mode too (and patched on disk only for its header) */
#define repatch_list(p, add) list_set(VX_REG_REPATCH, p, add)
static void repatch_add(const WCHAR *p, BOOL memory) { list_set_data(VX_REG_REPATCH, p, TRUE, memory ? L"memory" : L"file"); }

static BOOL repatch_memory(const WCHAR *p)
{
    HKEY k;
    WCHAR d[16] = L"";
    DWORD cb = sizeof(d) - sizeof(WCHAR), type;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_REPATCH, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    if (RegQueryValueExW(k, p, NULL, &type, (BYTE *)d, &cb) || type != REG_SZ) d[0] = 0;
    RegCloseKey(k);
    return !vx_wcsicmp(d, L"memory");
}

/* a list's entries as "a\0b\0\0": the files VistaExt patched, those the
 * switch put back (to patch again when switched on), or those it could not
 * put back yet. The buffer grows as needed: every entry is listed, however
 * many there are (a cut-off list would leave programs patched while VistaExt
 * says it is off). NULL: out of memory (logged). */
#define VX_REG_PATCHED_LIST VX_REG_ROOT L"\\Patched"
static WCHAR *list_entries(const WCHAR *key)
{
    HKEY k;
    WCHAR name[MAX_PATH + 8], *buf;
    DWORD n, idx, used = 0, cap = 16 * 1024;
    LONG e;
    if (!(buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(WCHAR)))) goto oom;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return buf;
    for (idx = 0; ; idx++) {
        DWORD l;
        n = MAX_PATH + 8;
        if ((e = RegEnumValueW(k, idx, name, &n, NULL, NULL, NULL, NULL))) {
            if (e == ERROR_MORE_DATA) continue;            /* not a path VistaExt wrote: skip it */
            break;
        }
        l = (DWORD)vx_wcslen(name);
        if (used + l + 2 >= cap) {
            WCHAR *nb = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, buf, cap * 2 * sizeof(WCHAR));
            if (!nb) { RegCloseKey(k); HeapFree(GetProcessHeap(), 0, buf); goto oom; }
            buf = nb;
            cap *= 2;
        }
        vx_memcpy(buf + used, name, (l + 1) * sizeof(WCHAR));
        used += l + 1;
        buf[used] = 0;
    }
    RegCloseKey(k);
    return buf;
oom:
    logf(L"%s: not enough memory to list its entries; nothing changed for them", key);
    return NULL;
}
#define repatch_entries() list_entries(VX_REG_REPATCH)

static int list_count(const WCHAR *key)
{
    HKEY k;
    DWORD n = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return 0;
    if (RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL)) n = 0;
    RegCloseKey(k);
    return (int)n;
}

/* switched off, a patched program was running: put it back once it is closed */
static int restore_later(void)
{
    WCHAR *list, *p;
    int left = 0;
    if (!(list = list_entries(VX_REG_LATER))) return 0;
    for (p = list; *p; p += vx_wcslen(p) + 1) {
        BOOL mem = vxp_memory_registered(p);
        int r = vxp_restore_file(p, log_cb, NULL);
        if (r > 0) { repatch_add(p, mem); logf(L"%s: closed, so its original is back now", p); }
        if (r >= 0) list_set(VX_REG_LATER, p, FALSE);
        else left++;
    }
    HeapFree(GetProcessHeap(), 0, list);
    return left;
}

/* turn VistaExt on or off; returns TRUE if a restart is needed to finish */
static BOOL do_switch(BOOL on, BOOL quiet_if_done)
{
    switch_stats fs = { 0 };
    WCHAR dir[MAX_PATH + 2], *list, *p;
    int progs, files = 0, later = 0;
    lock();
    tool_dir(dir);
    vx_wcscatn(dir, L"\\", MAX_PATH + 2);
    if (!quiet_if_done) logw(on ? L"switching VistaExt on" : L"switching VistaExt off");
    if (!on) {
        set_dword(L"Enabled", 0);                   /* first: nothing new gets VistaExt from here on */
        progs = vxp_memory_switch(FALSE, quiet_if_done ? NULL : log_cb, NULL);
        /* patched files: put back now, or as soon as a running one is closed */
        if ((list = list_entries(VX_REG_PATCHED_LIST))) {
            for (p = list; *p; p += vx_wcslen(p) + 1) {
                BOOL mem = vxp_memory_registered(p);
                int r = vxp_restore_file(p, log_cb, NULL);
                if (r > 0) { repatch_add(p, mem); files++; continue; }
                if (r < 0) {
                    logf(L"%s: running; its original is put back as soon as it is closed", p);
                    list_set(VX_REG_LATER, p, TRUE);
                    later++;
                } else vxp_unlist_patched(p);
            }
            HeapFree(GetProcessHeap(), 0, list);
        }
        vxp_shell_entries(FALSE, dir);
        move_system_files(FALSE, &fs);
    } else {
        /* still patched (they were running all along): nothing to put back */
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, VX_REG_LATER, VX_REG_VIEW, 0);
        move_system_files(TRUE, &fs);
        vxp_shell_entries(TRUE, dir);
        set_dword(L"Enabled", 1);                   /* before re-patching: registrations write their IFEO again */
        progs = vxp_memory_switch(TRUE, quiet_if_done ? NULL : log_cb, NULL);
        if ((list = repatch_entries())) {
            for (p = list; *p; p += vx_wcslen(p) + 1) {
                WCHAR layer[600];
                vxp_stats st = { 0 };
                DWORD flags;
                if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { repatch_list(p, FALSE); continue; }
                layer[0] = 0;
                if (!vx_lookup_layer(p, layer, 600)) vxp_recommend(p, layer, 600);
                /* the way it was done before: file mode, or in memory (+ header) */
                flags = vxp_flags_for_layer(layer) | (repatch_memory(p) ? VXP_MEMORY : 0) | VXP_QUIET;
                if ((flags & VXP_MEMORY) && vxp_folder_needs(p)) flags |= VXP_REGALL;
                if (vxp_process_file(p, flags, log_cb, NULL, &st) >= 0) { repatch_list(p, FALSE); files++; }
                else later++;
            }
            HeapFree(GetProcessHeap(), 0, list);
        }
    }
    if (!quiet_if_done || fs.moved || files || later)
        logf(L"VistaExt is %s: %d system file(s) moved %s, %d left (Windows' own or in use), %d program(s) %s, "
             L"%d patched file(s) %s%s",
             on ? L"on" : L"off", fs.moved, on ? L"back into place" : L"aside (VistaExt.off)", fs.kept + fs.busy, progs,
             on ? L"redirected again" : L"no longer redirected", files, on ? L"patched again" : L"put back",
             fs.reboot ? L"; Windows must restart to finish" : later ? L"; running ones follow when closed" : L"");
    if (fs.reboot) need_restart();
    unlock();
    return fs.reboot || restart_needed();
}

/* at start: finish a switch that was interrupted (power cut, crash) */
static void reconcile(void)
{
    WCHAR *list = repatch_entries();
    BOOL pending = list && list[0];
    if (list) HeapFree(GetProcessHeap(), 0, list);
    if (!vxp_enabled()) do_switch(FALSE, TRUE);
    else if (pending) do_switch(TRUE, TRUE);
    else {
        /* any file still aside? */
        WCHAR *l = installed_list(), *p, off[MAX_PATH + 20];
        BOOL aside = FALSE;
        for (p = l; l && *p && !aside; p += vx_wcslen(p) + 1) {
            off_name(p, off);
            aside = GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES;
        }
        if (l) HeapFree(GetProcessHeap(), 0, l);
        if (aside) do_switch(TRUE, TRUE);
    }
}

/* ---- the global Windows version -------------------------------------------------- */
static BOOL valid_version(const WCHAR *tok, const WCHAR **label, WCHAR *norm, SIZE_T cch)
{
    const vx_osver *v = vx_find_osver(tok);
    vx_osver cv;
    WCHAR t[40], csd[24];
    if (v) { *label = v->label; vx_wcscpyn(norm, v->token, cch); return TRUE; }
    if (vx_parse_custom(tok, &cv, t, 40, csd, 24)) { *label = L"Custom"; vx_wcscpyn(norm, t, cch); return TRUE; }
    return FALSE;
}

static int set_version(const WCHAR *tok)
{
    const WCHAR *label;
    WCHAR norm[40], *list, *p;
    HKEY k;
    LONG r;
    if (!valid_version(tok, &label, norm, 40)) return ERROR_INVALID_PARAMETER;
    lock();
    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT, 0, NULL, 0, KEY_SET_VALUE | VX_REG_VIEW, NULL, &k, NULL);
    if (!r) {
        r = RegSetValueExW(k, VX_REG_GLOBALVER, 0, REG_SZ, (const BYTE *)norm, (DWORD)((vx_wcslen(norm) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
    if (!r) {
        logf(L"programs VistaExt runs are now told: %s (%s); programs with their own setting keep it", label, norm);
        /* programs patched on disk (file mode) may need versvx now */
        if (vxp_enabled() && (list = list_entries(VX_REG_PATCHED_LIST))) {
            for (p = list; *p; p += vx_wcslen(p) + 1) {
                WCHAR layer[600] = L"";
                vxp_stats st = { 0 };
                if (!vx_lookup_layer(p, layer, 600)) vxp_recommend(p, layer, 600);
                if (!(vxp_mode_for(p) & VXP_MEMORY))
                    vxp_process_file(p, vxp_flags_for_layer(layer) | VXP_QUIET, log_cb, NULL, &st);
            }
            HeapFree(GetProcessHeap(), 0, list);
        }
    }
    unlock();
    return (int)r;
}

static void status_text(WCHAR *out, SIZE_T cch)
{
    WCHAR v[64] = L"", norm[40] = L"";
    const WCHAR *label = L"This Windows' own version";
    if (vx_global_version(v, 64)) valid_version(v, &label, norm, 40);
    DWORD su = vx_setting(L"Setups", 1);
    wsprintfW(out, L"enabled=%d\nversion=%s\nlabel=%s\nsetups=%d\nrestart=%d\nwaiting=%d\n", vxp_enabled() ? 1 : 0,
              norm[0] ? norm : L"REAL", label, su ? 1 : 0, restart_needed() ? 1 : 0, list_count(VX_REG_LATER));
}

/* ---- the pipe: the tray icon, and programs about to start another ------------- */
static BOOL client_is_admin(HANDLE pipe)
{
    HANDLE tok = NULL;
    BYTE ubuf[256];
    TOKEN_GROUPS *g = NULL;
    DWORD got = 0, i;
    BOOL r = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID admins = NULL, system = NULL;
    if (!ImpersonateNamedPipeClient(pipe)) return FALSE;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) tok = NULL;
    RevertToSelf();
    if (!tok) { logf(L"a change was asked for, but who asked could not be checked (error %u)", GetLastError()); return FALSE; }
    AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins);
    AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system);
    /* an administrator, also while UAC runs their programs without the
     * rights (the group is then "deny only"): VistaExt's own switch, like a
     * driver's tray tool. The group list is as long as it is (a domain user
     * can be in hundreds): asked for its size first. */
    GetTokenInformation(tok, TokenGroups, NULL, 0, &got);
    if (admins && got && (g = HeapAlloc(GetProcessHeap(), 0, got)) && GetTokenInformation(tok, TokenGroups, g, got, &got))
        for (i = 0; i < g->GroupCount && !r; i++)
            if (EqualSid(g->Groups[i].Sid, admins) && (g->Groups[i].Attributes & (SE_GROUP_ENABLED | SE_GROUP_USE_FOR_DENY_ONLY)))
                r = TRUE;
    if (g) HeapFree(GetProcessHeap(), 0, g);
    if (!r && system && GetTokenInformation(tok, TokenUser, ubuf, sizeof(ubuf), &got)) r = EqualSid(((TOKEN_USER *)ubuf)->User.Sid, system);
    if (admins) FreeSid(admins);
    if (system) FreeSid(system);
    CloseHandle(tok);
    return r;
}

static void handle_request(HANDLE pipe, WCHAR *req, WCHAR *rep, SIZE_T cch)
{
    WCHAR *arg = req;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    if (!vx_wcsicmp(req, L"STATUS")) { status_text(rep, cch); return; }
    if (!vx_wcsicmp(req, L"PREPARE")) {
        /* "PREPARE [setup ]<path>": "setup" when the program asking was
         * registered as a setup program */
        int r = 0;
        BOOL from_setup = !vx_wcsnicmp(arg, L"setup ", 6);
        if (from_setup) arg += 6;
        if (vxp_enabled() && auto_on() && ImpersonateNamedPipeClient(pipe)) {
            r = handle_single(arg, from_setup ? L"started by a setup" : L"about to start", from_setup, TRUE);
            RevertToSelf();
        }
        wsprintfW(rep, L"%d\n", r);
        return;
    }
    if (!vx_wcsicmp(req, L"ENABLE") || !vx_wcsicmp(req, L"DISABLE") || !vx_wcsicmp(req, L"SETVER") ||
        !vx_wcsicmp(req, L"SETUPS")) {
        if (!client_is_admin(pipe)) { vx_wcscpyn(rep, L"error=only an administrator can change this\n", cch); return; }
        if (!vx_wcsicmp(req, L"SETUPS")) {
            set_dword(L"Setups", arg[0] == '1');
            logf(arg[0] == '1' ? L"setup programs are told the chosen Windows version too"
                               : L"setup programs see the real Windows version unless they need VistaExt");
        } else if (!vx_wcsicmp(req, L"SETVER")) {
            int r = set_version(arg);
            if (r) { wsprintfW(rep, L"error=not a version VistaExt knows: %s\n", arg); return; }
        } else
            do_switch(!vx_wcsicmp(req, L"ENABLE"), FALSE);
        status_text(rep, cch);
        return;
    }
    vx_wcscpyn(rep, L"error=unknown request\n", cch);
}

/* the pipe is opened for overlapped I/O (so that the service can stop while
 * waiting for a client); each read and write waits for itself, 10 s at most */
static BOOL pipe_io(HANDLE h, void *buf, DWORD len, DWORD *done, BOOL write)
{
    OVERLAPPED ov;
    BOOL ok;
    vx_memset(&ov, 0, sizeof(ov));
    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return FALSE;
    ok = write ? WriteFile(h, buf, len, NULL, &ov) : ReadFile(h, buf, len, NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 10000) != WAIT_OBJECT_0) CancelIo(h);
        ok = GetOverlappedResult(h, &ov, done, TRUE);
    } else if (ok) ok = GetOverlappedResult(h, &ov, done, TRUE);
    CloseHandle(ov.hEvent);
    return ok;
}

static DWORD WINAPI pipe_client(LPVOID p)
{
    HANDLE h = p;
    WCHAR req[1024], rep[1024];
    DWORD got = 0, put;
    if (pipe_io(h, req, sizeof(req) - sizeof(WCHAR), &got, FALSE) && got >= sizeof(WCHAR)) {
        req[got / sizeof(WCHAR)] = 0;
        rep[0] = 0;
        handle_request(h, req, rep, 1024);
        pipe_io(h, rep, (DWORD)((vx_wcslen(rep) + 1) * sizeof(WCHAR)), &put, TRUE);
        FlushFileBuffers(h);
    }
    DisconnectNamedPipe(h);
    CloseHandle(h);
    return 0;
}

static DWORD WINAPI pipe_server(LPVOID unused)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL), hs[2];
    /* SYSTEM and administrators; any signed-in user may ask (the requests
     * check who is asking) */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1,
                                                              &sa.lpSecurityDescriptor, NULL) || !ev)
        return 0;
    hs[0] = g_stop; hs[1] = ev;
    for (;;) {
        OVERLAPPED ov;
        HANDLE h = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | 0x8 /* reject remote */,
                                    PIPE_UNLIMITED_INSTANCES, 2048, 2048, 0, &sa);
        BOOL ok;
        DWORD x;
        if (h == INVALID_HANDLE_VALUE) { if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break; continue; }
        vx_memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);
        ok = ConnectNamedPipe(h, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) { CancelIo(h); CloseHandle(h); break; }
            ok = GetOverlappedResult(h, &ov, &x, FALSE);
        } else if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) ok = TRUE;
        if (!ok) { CloseHandle(h); continue; }
        {
            /* each request on its own thread: a switch takes a moment, and
             * programs that are starting must not wait behind it */
            HANDLE th = CreateThread(NULL, 0, pipe_client, h, 0, NULL);
            if (th) CloseHandle(th);
            else { DisconnectNamedPipe(h); CloseHandle(h); }
        }
    }
    LocalFree(sa.lpSecurityDescriptor);
    CloseHandle(ev);
    return 0;
}

/* ---- service plumbing ----------------------------------------------------- */
static void report(DWORD state)
{
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                                         SERVICE_ACCEPT_SESSIONCHANGE : 0;
    SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI handler(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
    }
    if (ctl == SERVICE_CONTROL_SESSIONCHANGE && type == WTS_SESSION_LOGON && g_logon) SetEvent(g_logon);
    if (ctl == SERVICE_CONTROL_INTERROGATE) SetServiceStatus(g_ssh, &g_ss);
    return NO_ERROR;
}

/* one switch, patch or setting change at a time, between the service and
 * the command line (administrators and SYSTEM only) */
static HANDLE make_mutex(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    HANDLE m;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
                                                              &sa.lpSecurityDescriptor, NULL)) return NULL;
    m = CreateMutexW(&sa, FALSE, L"Global\\VistaExtSwitch");
    LocalFree(sa.lpSecurityDescriptor);
    return m;
}

static void init_common(void)
{
    WCHAR d[MAX_PATH];
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_logcs);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, d))) GetWindowsDirectoryW(d, MAX_PATH);
    vx_wcscatn(d, L"\\VistaExt", MAX_PATH);
    CreateDirectoryW(d, NULL);
    g_switch = make_mutex();
    vx_wcscpyn(g_logpath, d, MAX_PATH);
    vx_wcscatn(g_logpath, L"\\VistaExt.log", MAX_PATH);
    find_roots();
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    HANDLE th[64], hs[2];
    int nth = 0;
    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, handler, NULL);
    if (!g_ssh) return;
    report(SERVICE_START_PENDING);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_work = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_logon = CreateEventW(NULL, FALSE, FALSE, NULL);
    init_common();
    report(SERVICE_RUNNING);
    logf(L"service started (VistaExt is %s)", vxp_enabled() ? L"on" : L"off");
    reconcile();                                    /* finish an interrupted switch */
    lock();
    vxp_cleanup_missing(log_cb, NULL);              /* programs removed while it was not running */
    unlock();
    if ((th[nth] = CreateThread(NULL, 0, pipe_server, NULL, 0, NULL))) nth++;
    find_session_roots();                           /* signed-in users' real folders */
    watch_new(th, &nth, 62);
    if ((th[nth] = CreateThread(NULL, 0, worker, NULL, 0, NULL))) nth++;
    hs[0] = g_stop; hs[1] = g_logon;
    /* a user signed in: their real folders (a new profile, a moved Downloads) */
    while (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        find_session_roots();
        watch_new(th, &nth, 63);
    }
    WaitForMultipleObjects(nth, th, TRUE, 30000);
    logw(L"service stopped");
    report(SERVICE_STOPPED);
}

static int install_service(BOOL on)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS), s;
    WCHAR exe[MAX_PATH + 2], cmd[MAX_PATH + 32];
    SERVICE_STATUS st;
    if (!m) { con_print(L"Cannot open the service manager (error %u). Run as administrator.\r\n", GetLastError()); return 2; }
    if (!on) {
        if ((s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS))) {
            int i;
            ControlService(s, SERVICE_CONTROL_STOP, &st);
            for (i = 0; i < 60 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(500);
            DeleteService(s);
            CloseServiceHandle(s);
        }
        CloseServiceHandle(m);
        con_print(L"Automatic mode removed.\r\n");
        return 0;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, L"\"%s\" /service", exe);
    s = CreateServiceW(m, SVC_NAME, L"VistaExt automatic mode", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                       SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!s) { con_print(L"Cannot create the service (error %u).\r\n", GetLastError()); CloseServiceHandle(m); return 2; }
    {
        SERVICE_DESCRIPTIONW d = { L"Makes newly installed and updated programs run on this Windows (VistaExt)." };
        /* NOT a delayed start: programs started right after signing in need
         * it (a setup's helpers are looked at before they start; the tray
         * icon's switch; programs installed in the first minutes), and its
         * own start is small (measured: running in 10-70 ms, 4 MB, no work
         * until something changes). Turned off explicitly, for an update
         * over a build that had it on. */
        SERVICE_DELAYED_AUTO_START_INFO dl = { FALSE };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &dl);
    }
    StartServiceW(s, 0, NULL);
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    con_print(L"Automatic mode installed and started.\r\n");
    return 0;
}

static BOOL is_elevated(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID admins = NULL;
    BOOL r = FALSE;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        if (!CheckTokenMembership(NULL, admins, &r)) r = FALSE;
        FreeSid(admins);
    }
    return r;
}

static int print_status(void)
{
    WCHAR st[512];
    status_text(st, 512);
    con_write(st);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    BOOL pause = argc > 2 && !vx_wcsicmp(argv[argc - 1], L"/pause");
    int r = 2;
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/service")) {
        SERVICE_TABLE_ENTRYW t[] = { { SVC_NAME, svc_main }, { NULL, NULL } };
        return StartServiceCtrlDispatcherW(t) ? 0 : 1;
    }
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/install")) return install_service(TRUE);
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/uninstall")) return install_service(FALSE);
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/status")) { init_common(); return print_status(); }
    if (argc > 2 && !vx_wcsicmp(argv[1], L"/pipe")) {
        /* a request to the running service, as the tray icon sends it */
        WCHAR req[1024] = L"", rep[1024];
        DWORD got = 0;
        for (r = 2; r < argc; r++) {
            if (r > 2) vx_wcscatn(req, L" ", 1024);
            vx_wcscatn(req, argv[r], 1024);
        }
        if (!CallNamedPipeW(PIPE_NAME, req, (DWORD)((vx_wcslen(req) + 1) * sizeof(WCHAR)), rep, sizeof(rep) - sizeof(WCHAR),
                            &got, 120000)) {
            con_print(L"error=the service did not answer (error %u)\r\n", GetLastError());
            return 1;
        }
        rep[got / sizeof(WCHAR)] = 0;
        con_write(rep);
        return vx_wcsnicmp(rep, L"error=", 6) ? 0 : 1;
    }
    if (argc > 1 && (!vx_wcsicmp(argv[1], L"/enable") || !vx_wcsicmp(argv[1], L"/disable") ||
                     !vx_wcsicmp(argv[1], L"/setver") || !vx_wcsicmp(argv[1], L"/scan") || !vx_wcsicmp(argv[1], L"/prepare") ||
                     !vx_wcsicmp(argv[1], L"/setups"))) {
        if (!is_elevated()) {
            con_print(L"This needs administrator rights: run it from an administrator command prompt.\r\n");
            r = 5;
            goto out;
        }
        g_stop = NULL;
        init_common();
        if (!vx_wcsicmp(argv[1], L"/scan")) {
            con_print(L"Making installed programs compatible (log: %s) ...\r\n", g_logpath);
            scan_all();
            r = 0;
        } else if (!vx_wcsicmp(argv[1], L"/setver")) {
            if (argc < 3 || set_version(argv[2])) {
                con_print(L"usage: vxauto /setver WIN7|WIN8|WIN81|WIN10|WIN11|REAL|VER:<major>.<minor>.<build>[.<sp>]\r\n");
                r = 2;
            } else r = print_status();
        } else if (!vx_wcsicmp(argv[1], L"/setups")) {
            set_dword(L"Setups", argc > 2 && argv[2][0] == '1');
            logf(argc > 2 && argv[2][0] == '1' ? L"setup programs are told the chosen Windows version too"
                                               : L"setup programs see the real Windows version unless they need VistaExt");
            r = print_status();
        } else if (!vx_wcsicmp(argv[1], L"/prepare")) {
            r = argc > 2 ? handle_single(argv[2], L"asked from the command line", FALSE, FALSE) : -1;
            con_print(L"%d\r\n", r);
            r = r < 0 ? 1 : 0;
        } else {
            BOOL on = !vx_wcsicmp(argv[1], L"/enable");
            BOOL restart;
            con_print(on ? L"Switching VistaExt on...\r\n" : L"Switching VistaExt off...\r\n");
            restart = do_switch(on, FALSE);
            con_print(on ? L"VistaExt is on.\r\n" : L"VistaExt is off: Windows works as it does without VistaExt. "
                                                    L"Programs that are running keep it until they are closed.\r\n");
            if (restart) con_print(L"Restart Windows to finish (some files were in use).\r\n");
            con_print(L"Log: %s\r\n", g_logpath);
            r = restart ? 3 : 0;
        }
        goto out;
    }
    con_print(L"usage: vxauto /install | /uninstall | /scan | /enable | /disable | /status | /setver <version>\r\n");
out:
    if (pause) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        WCHAR c;
        DWORD n;
        con_print(L"\r\nPress Enter to close this window.\r\n");
        ReadConsoleW(in, &c, 1, &n, NULL);
    }
    return r;
}

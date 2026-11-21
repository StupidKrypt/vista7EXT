/* msifix - make modern .msi installer packages install on Vista and 7.
 *
 * Works on a copy of the package:
 *  1. Install checks such as "VersionNT >= 603" (needs Windows 8.1) are
 *     lowered to the Windows version this PC runs.
 *  2. On Vista only (Windows Installer 4.5): packages marked as needing
 *     Windows Installer 5.0 are re-marked 4.5, and the 5.0-only tables and
 *     actions are removed (service settings, extended permissions, shortcut
 *     properties). The tool lists anything it removes.
 *  3. Custom action DLLs stored inside the package are patched like
 *     programs, so they can use VistaExt too.
 * Then, with /install, the fixed copy is installed with msiexec. */
#include "../vxpatch/patchlib.h"
#include "../common/console.h"
#include <msi.h>
#include <msiquery.h>
#include <shlobj.h>

#ifndef PID_TEMPLATE
#define PID_TEMPLATE  7
#endif
#ifndef PID_PAGECOUNT
#define PID_PAGECOUNT 14
#endif
#ifndef VT_I4
#define VT_I4 3
#endif

/* Properties a package fills with Windows' build number itself (AppSearch +
 * RegLocator on "Windows NT\CurrentVersion" CurrentBuild[Number]), such as
 * Epic Games' WINDOWSBUILDNUM: compared like WindowsBuild. */
#define MAXPROPS 16
static WCHAR g_buildprops[MAXPROPS][72];
static int g_nbuildprops;

static DWORD g_nt;        /* VersionNT of this PC, e.g. 600 or 601 */
static DWORD g_build;     /* WindowsBuild, e.g. 6002 or 7601 */
static BOOL  g_vista;
static BOOL  g_gui;       /* no console: report problems in a message box */

static int fail(int code, const WCHAR *what)
{
    if (g_gui) MessageBoxW(NULL, what, L"VistaExt - installing a package", MB_ICONERROR);
    return code;
}

/* ---- condition rewriting --------------------------------------------------- */
static BOOL is_word(WCHAR c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static void put(WCHAR *out, SIZE_T *o, SIZE_T cch, const WCHAR *s, SIZE_T n)
{
    while (n-- && *o + 1 < cch) out[(*o)++] = *s++;
}

/* Is the comparison at in[pos] negated, by a NOT right before it or on a
 * bracketed group around it? Then it must stay as it is: lowering
 * "NOT (VersionNT >= 1000)" would turn a pass on this PC into a fail. */
static BOOL negated(const WCHAR *in, SIZE_T pos)
{
    BOOL stack[32], pending = FALSE;
    int depth = 0, over = 0;
    SIZE_T i = 0;
    while (i < pos) {
        WCHAR c = in[i];
        if (c == '"') {                                 /* quoted text */
            for (i++; i < pos && in[i] != '"'; i++) ;
            i++;
            pending = FALSE;
        } else if (c == '(') {
            BOOL cur = depth ? stack[depth - 1] : FALSE;
            if (depth < 32) stack[depth++] = cur != pending; else over++;
            pending = FALSE;
            i++;
        } else if (c == ')') {
            if (over) over--; else if (depth) depth--;
            pending = FALSE;
            i++;
        } else if (is_word(c)) {
            SIZE_T st = i;
            while (i < pos && is_word(in[i])) i++;
            if (i - st == 3 && !vx_wcsnicmp(in + st, L"NOT", 3)) pending = !pending;
            else pending = FALSE;
        } else i++;
    }
    return (depth ? stack[depth - 1] : FALSE) != pending;
}

/* Rewrites "<prop> >= N", "<prop> > N" and "<prop> = N" for VersionNT,
 * VersionNT64 and WindowsBuild when N is newer than this PC, unless the
 * comparison is negated. Returns the number of changes. */
static int rewrite(const WCHAR *in, WCHAR *out, SIZE_T cch)
{
    const WCHAR *props[3 + MAXPROPS] = { L"VersionNT64", L"VersionNT", L"WindowsBuild" };
    int np = 3;
    SIZE_T i = 0, o = 0, n = vx_wcslen(in);
    int changes = 0;
    for (np = 3; np < 3 + g_nbuildprops; np++) props[np] = g_buildprops[np - 3];
    while (i < n) {
        int p, matched = 0;
        if ((!i || !is_word(in[i - 1])) && !negated(in, i)) {
            for (p = 0; p < np && !matched; p++) {
                SIZE_T pl = vx_wcslen(props[p]), j, k;
                DWORD num = 0, limit = p >= 2 ? g_build : g_nt;
                WCHAR numtxt[16];
                const WCHAR *op;
                if (i + pl > n || vx_wcsnicmp(in + i, props[p], pl) || (i + pl < n && is_word(in[i + pl]))) continue;
                j = i + pl;
                while (j < n && in[j] == ' ') j++;
                if (j + 1 < n && in[j] == '>' && in[j + 1] == '=') { op = L">="; j += 2; }
                else if (j < n && in[j] == '>' && (j + 1 >= n || in[j + 1] != '<')) { op = L">"; j++; }
                else if (j < n && in[j] == '=') { op = L"="; j++; }
                else continue;
                while (j < n && in[j] == ' ') j++;
                k = j;
                while (k < n && in[k] >= '0' && in[k] <= '9' && k - j < 9) num = num * 10 + (in[k++] - '0');
                if (k == j || (k < n && is_word(in[k]))) continue;
                if (num < limit || (num == limit && op[0] != '>') || (num == limit && op[1] == '=')) continue;
                /* newer than this PC: require this PC's version instead */
                put(out, &o, cch, in + i, pl);
                put(out, &o, cch, L" >= ", 4);
                wsprintfW(numtxt, L"%u", limit);
                put(out, &o, cch, numtxt, vx_wcslen(numtxt));
                i = k;
                matched = 1;
                changes++;
            }
        }
        if (!matched) put(out, &o, cch, in + i++, 1);
    }
    out[o] = 0;
    return changes;
}

/* ---- MSI helpers ------------------------------------------------------------ */
static BOOL has_table(MSIHANDLE db, const WCHAR *t)
{
    return MsiDatabaseIsTablePersistentW(db, t) == MSICONDITION_TRUE;
}

static UINT exec(MSIHANDLE db, const WCHAR *sql, MSIHANDLE params)
{
    MSIHANDLE v;
    UINT r = MsiDatabaseOpenViewW(db, sql, &v);
    if (r) return r;
    r = MsiViewExecute(v, params);
    MsiViewClose(v);
    MsiCloseHandle(v);
    return r;
}

static BOOL get_str(MSIHANDLE rec, UINT f, WCHAR *buf, DWORD cch)
{
    DWORD n = cch;
    buf[0] = 0;
    return MsiRecordGetStringW(rec, f, buf, &n) == ERROR_SUCCESS;
}

static int column_index(MSIHANDLE view, const WCHAR *name)
{
    MSIHANDLE cols;
    UINT i, n;
    int idx = 0;
    WCHAR buf[72];
    if (MsiViewGetColumnInfo(view, MSICOLINFO_NAMES, &cols)) return 0;
    n = MsiRecordGetFieldCount(cols);
    for (i = 1; i <= n && !idx; i++)
        if (get_str(cols, i, buf, 72) && !vx_wcsicmp(buf, name)) idx = (int)i;
    MsiCloseHandle(cols);
    return idx;
}

static BOOL ends_with_i(const WCHAR *s, const WCHAR *suf)
{
    SIZE_T a = vx_wcslen(s), b = vx_wcslen(suf);
    return a >= b && !vx_wcsicmp(s + a - b, suf);
}

static void find_build_props(MSIHANDLE db)
{
    MSIHANDLE v, rec;
    WCHAR prop[72], key[260], name[72];
    g_nbuildprops = 0;
    if (!has_table(db, L"AppSearch") || !has_table(db, L"RegLocator")) return;
    if (MsiDatabaseOpenViewW(db, L"SELECT `AppSearch`.`Property`, `RegLocator`.`Key`, `RegLocator`.`Name` "
                                 L"FROM `AppSearch`, `RegLocator` WHERE `AppSearch`.`Signature_` = `RegLocator`.`Signature_`", &v))
        return;
    if (!MsiViewExecute(v, 0)) {
        while (g_nbuildprops < MAXPROPS && !MsiViewFetch(v, &rec)) {
            if (get_str(rec, 1, prop, 72) && get_str(rec, 2, key, 260) && get_str(rec, 3, name, 72) &&
                ends_with_i(key, L"Windows NT\\CurrentVersion") &&
                (!vx_wcsicmp(name, L"CurrentBuild") || !vx_wcsicmp(name, L"CurrentBuildNumber"))) {
                vx_wcscpyn(g_buildprops[g_nbuildprops++], prop, 72);
                con_print(L"  %s holds the Windows build number: its checks are lowered too\r\n", prop);
            }
            MsiCloseHandle(rec);
        }
    }
    MsiViewClose(v);
    MsiCloseHandle(v);
}

/* LaunchCondition: the condition is the table's key, so rows are replaced. */
static int fix_launch_conditions(MSIHANDLE db)
{
    struct { WCHAR old[1024], neu[1024], desc[1024]; } *rows;
    MSIHANDLE v, rec;
    int n = 0, i, total = 0;
    if (!has_table(db, L"LaunchCondition")) return 0;
    rows = HeapAlloc(GetProcessHeap(), 0, sizeof(*rows) * 64);
    if (!rows) return 0;
    if (!MsiDatabaseOpenViewW(db, L"SELECT `Condition`, `Description` FROM `LaunchCondition`", &v)) {
        MsiViewExecute(v, 0);
        while (n < 64 && !MsiViewFetch(v, &rec)) {
            /* a condition too long to read whole is left alone: a cut-off
             * copy would not match the row to replace */
            if (get_str(rec, 1, rows[n].old, 1024) && get_str(rec, 2, rows[n].desc, 1024) &&
                rewrite(rows[n].old, rows[n].neu, 1024)) n++;
            MsiCloseHandle(rec);
        }
        MsiViewClose(v);
        MsiCloseHandle(v);
    }
    for (i = 0; i < n; i++) {
        MSIHANDLE p = MsiCreateRecord(2);
        MsiRecordSetStringW(p, 1, rows[i].old);
        if (!exec(db, L"DELETE FROM `LaunchCondition` WHERE `Condition` = ?", p)) {
            MsiRecordSetStringW(p, 1, rows[i].neu);
            MsiRecordSetStringW(p, 2, rows[i].desc);
            if (!exec(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES (?, ?)", p)) {
                con_print(L"  install check: \"%s\"\r\n            now: \"%s\"\r\n", rows[i].old, rows[i].neu);
                total++;
            }
        }
        MsiCloseHandle(p);
    }
    HeapFree(GetProcessHeap(), 0, rows);
    return total;
}

/* Tables whose Condition column is not part of the key: update in place. */
static int fix_conditions_in(MSIHANDLE db, const WCHAR *table)
{
    WCHAR sql[128], *old, *neu;
    MSIHANDLE v, rec;
    int idx, total = 0;
    if (!has_table(db, table)) return 0;
    wsprintfW(sql, L"SELECT * FROM `%s`", table);
    if (MsiDatabaseOpenViewW(db, sql, &v)) return 0;
    old = HeapAlloc(GetProcessHeap(), 0, 2 * 4096 * sizeof(WCHAR));
    neu = old + 4096;
    if (old && (idx = column_index(v, L"Condition")) && !MsiViewExecute(v, 0)) {
        while (!MsiViewFetch(v, &rec)) {
            if (get_str(rec, (UINT)idx, old, 4096) && rewrite(old, neu, 4096)) {
                MsiRecordSetStringW(rec, (UINT)idx, neu);
                if (!MsiViewModify(v, MSIMODIFY_UPDATE, rec)) total++;
            }
            MsiCloseHandle(rec);
        }
    }
    if (old) HeapFree(GetProcessHeap(), 0, old);
    MsiViewClose(v);
    MsiCloseHandle(v);
    if (total) con_print(L"  %s: %d condition(s) lowered to this Windows version\r\n", table, total);
    return total;
}

/* Windows Installer 5.0 additions that 4.5 (Vista) cannot handle. */
static int drop_msi5(MSIHANDLE db)
{
    static const struct { const WCHAR *table, *what; } t5[] = {
        { L"MsiLockPermissionsEx", L"extended file/registry permissions (SDDL)" },
        { L"MsiServiceConfig", L"extra service settings (delayed start etc.)" },
        { L"MsiServiceConfigFailureActions", L"service recovery actions" },
        { L"MsiShortcutProperty", L"shortcut properties (taskbar app IDs)" },
    };
    WCHAR sql[128];
    int i, n = 0;
    for (i = 0; i < 4; i++) {
        if (!has_table(db, t5[i].table)) continue;
        wsprintfW(sql, L"DROP TABLE `%s`", t5[i].table);
        if (!exec(db, sql, 0)) {
            con_print(L"  removed %s (Windows Installer 5.0 only): %s\r\n", t5[i].table, t5[i].what);
            n++;
        }
    }
    if (has_table(db, L"InstallExecuteSequence"))   /* the 5.0 action that reads MsiServiceConfig */
        exec(db, L"DELETE FROM `InstallExecuteSequence` WHERE `Action` = 'MsiConfigureServices'", 0);
    return n;
}

static int fix_summary(MSIHANDLE db, BOOL *arm)
{
    MSIHANDLE si;
    UINT type;
    INT ival = 0;
    FILETIME ft;
    WCHAR tmpl[128];
    DWORD n = 128;
    BOOL lower;
    *arm = FALSE;
    /* Read-only first. Windows Installer rewrites the summary stream on commit
     * once it has been opened for writing, even if nothing was set, and then
     * drops properties (seen on Windows 7: the schema became 0). So it is
     * opened for writing only when it really changes. */
    if (MsiGetSummaryInformationW(db, NULL, 0, &si)) return 0;
    if (!MsiSummaryInfoGetPropertyW(si, PID_TEMPLATE, &type, &ival, &ft, tmpl, &n)) {
        WCHAR *p;
        for (p = tmpl; *p; p++) if (!vx_wcsnicmp(p, L"Arm", 3)) *arm = TRUE;
    }
    n = 0;
    lower = g_vista && !MsiSummaryInfoGetPropertyW(si, PID_PAGECOUNT, &type, &ival, &ft, NULL, &n) && ival > 405;
    MsiCloseHandle(si);
    if (!lower || MsiGetSummaryInformationW(db, NULL, 1, &si)) return 0;
    con_print(L"  package asks for Windows Installer %d.%d; Vista has 4.5, marked as 4.5\r\n", ival / 100, ival % 100 / 10);
    MsiSummaryInfoSetPropertyW(si, PID_PAGECOUNT, VT_I4, 405, NULL, NULL);
    MsiSummaryInfoPersist(si);
    MsiCloseHandle(si);
    return 1;
}

/* Custom action DLLs/EXEs in the Binary table. Patched ones are collected
 * first and written back after the scan, so rows are not changed while the
 * view is being read. */
#define MAXBIN 64
static WCHAR names[MAXBIN][80], files[MAXBIN][MAX_PATH];
static int g_nbin;

/* The temp files must exist until the package is saved: Windows Installer
 * may read a stream's file only when it commits. */
static void cleanup_binaries(void)
{
    int i;
    for (i = 0; i < g_nbin; i++) DeleteFileW(files[i]);
    g_nbin = 0;
}

static int fix_binaries(MSIHANDLE db)
{
    MSIHANDLE v, rec;
    WCHAR tmpdir[MAX_PATH], tmp[MAX_PATH];
    int n = 0, i, total = 0;
    if (!has_table(db, L"Binary")) return 0;
    GetTempPathW(MAX_PATH, tmpdir);
    if (MsiDatabaseOpenViewW(db, L"SELECT `Name`, `Data` FROM `Binary`", &v)) return 0;
    if (!MsiViewExecute(v, 0)) {
        while (n < MAXBIN && !MsiViewFetch(v, &rec)) {
            DWORD size = (DWORD)MsiRecordDataSize(rec, 2), got;
            BYTE *buf = size >= 64 ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
            get_str(rec, 1, names[n], 80);
            if (buf) {
                got = size;
                if (!MsiRecordReadStream(rec, 2, (char *)buf, &got) && got == size && buf[0] == 'M' && buf[1] == 'Z' &&
                    GetTempFileNameW(tmpdir, L"vxb", 0, tmp)) {
                    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    DWORD put;
                    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, buf, size, &put, NULL) && put == size;
                    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                    if (ok && vxp_process_file(tmp, VXP_NOBACKUP | VXP_QUIET, NULL, NULL, NULL) == 1) {
                        vx_wcscpyn(files[n], tmp, MAX_PATH);
                        n++;
                    } else DeleteFileW(tmp);
                }
                HeapFree(GetProcessHeap(), 0, buf);
            }
            MsiCloseHandle(rec);
        }
    }
    MsiViewClose(v);
    MsiCloseHandle(v);
    g_nbin = n;

    for (i = 0; i < n; i++) {
        MSIHANDLE p = MsiCreateRecord(2), one;
        UINT r = 1;
        MsiRecordSetStringW(p, 1, names[i]);
        /* 1st choice: update the row in place through a one-row view */
        if (!MsiDatabaseOpenViewW(db, L"SELECT * FROM `Binary` WHERE `Name` = ?", &one)) {
            if (!MsiViewExecute(one, p) && !MsiViewFetch(one, &rec)) {
                r = MsiRecordSetStreamW(rec, 2, files[i]);
                if (!r) r = MsiViewModify(one, MSIMODIFY_UPDATE, rec);
                MsiCloseHandle(rec);
            }
            MsiViewClose(one);
            MsiCloseHandle(one);
        }
        if (r) {   /* 2nd choice: delete and insert */
            r = exec(db, L"DELETE FROM `Binary` WHERE `Name` = ?", p);
            if (!r) r = MsiRecordSetStreamW(p, 2, files[i]);
            if (!r) r = exec(db, L"INSERT INTO `Binary` (`Name`, `Data`) VALUES (?, ?)", p);
        }
        if (!r) { con_print(L"  custom action %s: patched to use VistaExt\r\n", names[i]); total++; }
        else con_print(L"  custom action %s: could not update it (error %u)\r\n", names[i], r);
        MsiCloseHandle(p);
    }
    return total;
}

static void usage(void)
{
    con_write(
L"VistaExt msifix - install modern .msi packages on Windows Vista and 7\r\n"
L"\r\n"
L"  msifix [options] <package.msi> [fixed.msi]\r\n"
L"\r\n"
L"  /install     fix a copy and install it straight away (the copy is\r\n"
L"               kept in ProgramData\\VistaExt\\Packages for repairs)\r\n"
L"  /all         also lower version checks inside the install steps,\r\n"
L"               components and features (not just the up-front check)\r\n"
L"  /nobinaries  leave custom action DLLs unchanged\r\n"
L"  /keepchainer keep an embedded chainer (extra installers run after this one)\r\n"
L"\r\n"
L"The original package is never changed. Without a second name the fixed\r\n"
L"copy is saved next to it as <name>.fixed.msi.\r\n");
}

int wmain(int argc, WCHAR **argv)
{
    BOOL install = FALSE, all = FALSE, bins = TRUE, arm = FALSE, chainer = TRUE;
    const WCHAR *in = NULL, *outarg = NULL;
    WCHAR src[MAX_PATH], out[MAX_PATH + 16];

    MSIHANDLE db;
    UINT r;
    int i, changes = 0;

    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL console = (o && o != INVALID_HANDLE_VALUE) || AttachConsole(ATTACH_PARENT_PROCESS);
    for (i = 1; i < argc; i++) {
        if (!vx_wcsicmp(argv[i], L"/install")) install = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/all")) all = TRUE;
        else if (!vx_wcsicmp(argv[i], L"/nobinaries")) bins = FALSE;
        else if (!vx_wcsicmp(argv[i], L"/keepchainer")) chainer = FALSE;
        else if (argv[i][0] == '/') { usage(); return 2; }
        else if (!in) in = argv[i];
        else outarg = argv[i];
    }
    if (!in) { usage(); return 2; }
    {   /* msifix is a windowed program, so double-clicking an .msi (or the
         * right-click entry) shows no console window; problems are shown in a
         * message box instead. From a command prompt it writes to that
         * prompt's console (or wherever its output is redirected). */
        if (!console && install) g_gui = TRUE;
    }

    /* this build's Windows (VersionNT 600 = Vista, 601 = 7, 602 = 8) */
    g_nt = VX_OS;
    g_vista = VX_VISTA;
    {   /* the real build number (6001 on Vista SP1, 6002 on SP2, 7601 on 7 SP1) */
        void (WINAPI *nums)(DWORD *, DWORD *, DWORD *) =
            (void (WINAPI *)(DWORD *, DWORD *, DWORD *))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
        DWORD a = 0, b = 0, build = 0;
        if (nums) nums(&a, &b, &build);
        g_build = (build & 0xFFFF) && a == 6 && b == VX_OS % 100 ? (build & 0xFFFF) : VX_VISTA ? 6002 : VX_WIN7 ? 7601 : 9200;
    }

    if (!GetFullPathNameW(in, MAX_PATH, src, NULL)) { con_print(L"%s: bad path\r\n", in); return 2; }
    if (outarg) GetFullPathNameW(outarg, MAX_PATH, out, NULL);
    else if (install) {
        /* Windows Installer needs the package again for repairs, updates and
         * some uninstalls, so it is kept in ProgramData, not in TEMP */
        const WCHAR *base = src + vx_wcslen(src);
        while (base > src && base[-1] != '\\') base--;
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, out))) GetTempPathW(MAX_PATH, out);
        vx_wcscatn(out, L"\\VistaExt", MAX_PATH);
        CreateDirectoryW(out, NULL);
        vx_wcscatn(out, L"\\Packages", MAX_PATH);
        if (!CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            GetTempPathW(MAX_PATH, out);
            vx_wcscatn(out, L"VistaExt", MAX_PATH);
            CreateDirectoryW(out, NULL);
        }
        vx_wcscatn(out, L"\\", MAX_PATH);
        vx_wcscatn(out, base, MAX_PATH);
    } else {
        SIZE_T n;
        vx_wcscpyn(out, src, MAX_PATH);
        n = vx_wcslen(out);
        if (n > 4 && !vx_wcsicmp(out + n - 4, L".msi")) out[n - 4] = 0;
        vx_wcscatn(out, L".fixed.msi", MAX_PATH + 16);
    }

    con_print(L"Fixing %s\r\n  for %s (VersionNT %u)\r\n", src, g_vista ? L"Windows Vista" : L"Windows 7", g_nt);
    if (!CopyFileW(src, out, FALSE)) { con_print(L"Cannot create %s (error %d)\r\n", out, (int)GetLastError()); return fail(2, L"A working copy of the package could not be made."); }
    SetFileAttributesW(out, FILE_ATTRIBUTE_NORMAL);
    if ((r = MsiOpenDatabaseW(out, (LPCWSTR)MSIDBOPEN_TRANSACT, &db))) {
        con_print(L"Not a Windows Installer package, or it is damaged (error %u)\r\n", r);
        DeleteFileW(out);
        return fail(2, L"This is not a Windows Installer package, or it is damaged.");
    }
    changes += fix_summary(db, &arm);
    if (arm) {
        con_print(L"This package is for ARM processors and cannot run on this PC.\r\n");
        MsiCloseHandle(db);
        DeleteFileW(out);
        return 2;
    }
    if (g_vista) changes += drop_msi5(db);
    find_build_props(db);
    {
        int lowered = fix_launch_conditions(db);
        changes += lowered;
        /* An embedded chainer (Windows Installer 4.5) runs more installers in
         * the same transaction after this one; when one of them fails, all
         * is rolled back. In a package made for newer Windows they are made
         * for it too (Epic Games: EOS and GameInput), so the chainer goes and
         * the program itself installs. */
        if (lowered && chainer && has_table(db, L"MsiEmbeddedChainer") &&
            !exec(db, L"DROP TABLE `MsiEmbeddedChainer`", 0)) {
            con_print(L"  removed the embedded chainer (it installs extra packages made for newer Windows;\r\n"
                      L"  use /keepchainer to keep it)\r\n");
            changes++;
        }
    }
    if (all) {
        changes += fix_conditions_in(db, L"InstallExecuteSequence");
        changes += fix_conditions_in(db, L"InstallUISequence");
        changes += fix_conditions_in(db, L"Component");
        changes += fix_conditions_in(db, L"Condition");
    }
    if (bins) changes += fix_binaries(db);
    r = MsiDatabaseCommit(db);
    MsiCloseHandle(db);
    cleanup_binaries();
    if (r) { con_print(L"Could not save the fixed package (error %u)\r\n", r); return fail(2, L"The fixed copy of the package could not be saved."); }
    con_print(changes ? L"%d change(s). Fixed package: %s\r\n" : L"%d changes needed. Copy: %s\r\n", changes, out);
    if (install && !changes) {
        /* nothing to fix: the original package is installed as it is (this
         * is also what a double-click on any .msi does once VistaExt is set
         * up, so ordinary packages behave exactly as before) */
        DeleteFileW(out);
        vx_wcscpyn(out, src, MAX_PATH);
    }

    if (install) {
        WCHAR cmd[MAX_PATH * 2];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        vx_memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        wsprintfW(cmd, L"msiexec.exe /i \"%s\"", out);
        con_print(L"Starting the installer...\r\n");
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            con_print(L"Could not start msiexec (error %d)\r\n", (int)GetLastError());
            return fail(2, L"Windows Installer (msiexec) could not be started.");
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0 || code == 3010)
            con_print(L"Installed. If the program does not start, right-click it and choose\r\n"
                      L"\"VistaExt compatibility...\" (or run vxpatch on its folder).\r\n");
        else con_print(L"msiexec finished with code %u\r\n", code);
        if (g_gui) {
            /* msiexec showed its own messages; 1602 = cancelled by the user */
            if (code && code != 3010 && code != 1602 && code != 1641) {
                WCHAR m[160];
                wsprintfW(m, L"The installation did not finish (Windows Installer code %u).", code);
                fail(0, m);
            }
        } else {   /* run in a console it owns: keep it open so the result can be read */
            DWORD list[2], n;
            if (GetConsoleProcessList(list, 2) == 1) {
                WCHAR c[4];
                con_print(L"\r\nPress Enter to close.");
                ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), c, 4, &n, NULL);
            }
        }
        return (int)code;
    }
    return 0;
}

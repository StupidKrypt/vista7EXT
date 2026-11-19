/* msitest create <pkg.msi> <custom-action.exe> : build a "modern" test package
 * msitest dump <pkg.msi> <out-binary>           : print what msifix should change */
#include "../src/common/vx.h"
#include "../src/common/console.h"
#include <msi.h>
#include <msiquery.h>

static UINT sql(MSIHANDLE db, const WCHAR *q, MSIHANDLE rec)
{
    MSIHANDLE v;
    UINT r = MsiDatabaseOpenViewW(db, q, &v);
    if (r) { con_print(L"open view failed %u: %s\r\n", r, q); return r; }
    r = MsiViewExecute(v, rec);
    if (r) con_print(L"execute failed %u: %s\r\n", r, q);
    MsiViewClose(v);
    MsiCloseHandle(v);
    return r;
}

static int create(const WCHAR *path, const WCHAR *ca)
{
    MSIHANDLE db, si, rec;
    UINT r = MsiOpenDatabaseW(path, (LPCWSTR)MSIDBOPEN_CREATE, &db);
    if (r) { con_print(L"create failed %u\r\n", r); return 1; }
    sql(db, L"CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` CHAR(0) NOT NULL PRIMARY KEY `Property`)", 0);
    sql(db, L"CREATE TABLE `LaunchCondition` (`Condition` CHAR(255) NOT NULL, `Description` CHAR(255) NOT NULL PRIMARY KEY `Condition`)", 0);
    sql(db, L"CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)", 0);
    sql(db, L"CREATE TABLE `MsiServiceConfig` (`MsiServiceConfig` CHAR(72) NOT NULL, `Name` CHAR(255) NOT NULL PRIMARY KEY `MsiServiceConfig`)", 0);
    sql(db, L"CREATE TABLE `Binary` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)", 0);
    sql(db, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductName', 'Modern App')", 0);
    sql(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES ('Installed OR VersionNT >= 603', 'This app needs Windows 8.1 or later')", 0);
    sql(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES ('VersionNT64', 'This app needs 64-bit Windows')", 0);
    sql(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES ('WindowsBuild>9000 AND VersionNT>600', 'Needs a newer build')", 0);
    sql(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES ('VersionNT >= 501', 'Needs XP (already met)')", 0);
    sql(db, L"INSERT INTO `LaunchCondition` (`Condition`, `Description`) VALUES ('NOT (VersionNT >= 1000)', 'Not for Windows 10 (true on Vista/7, must stay)')", 0);
    sql(db, L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('MsiConfigureServices', 'VersionNT >= 600', 5800)", 0);
    sql(db, L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('InstallFiles', 'VersionNT >= 1000', 4000)", 0);
    sql(db, L"INSERT INTO `MsiServiceConfig` (`MsiServiceConfig`, `Name`) VALUES ('svc', 'ModernService')", 0);
    rec = MsiCreateRecord(1);
    MsiRecordSetStreamW(rec, 1, ca);
    sql(db, L"INSERT INTO `Binary` (`Name`, `Data`) VALUES ('ModernCA', ?)", rec);
    MsiCloseHandle(rec);
    MsiGetSummaryInformationW(db, NULL, 10, &si);
    MsiSummaryInfoSetPropertyW(si, 7 /* PID_TEMPLATE */, 30 /* VT_LPSTR */, 0, NULL, L"x64;1033");
    MsiSummaryInfoSetPropertyW(si, 14 /* PID_PAGECOUNT */, 3 /* VT_I4 */, 500, NULL, NULL);
    MsiSummaryInfoSetPropertyW(si, 15 /* PID_WORDCOUNT */, 3, 2, NULL, NULL);
    MsiSummaryInfoSetPropertyW(si, 9 /* PID_REVNUMBER */, 30, 0, NULL, L"{11111111-2222-3333-4444-555555555555}");
    MsiSummaryInfoPersist(si);
    MsiCloseHandle(si);
    r = MsiDatabaseCommit(db);
    MsiCloseHandle(db);
    con_print(L"created %s (commit %u)\r\n", path, r);
    return r != 0;
}

static void dump_query(MSIHANDLE db, const WCHAR *q, int cols)
{
    MSIHANDLE v, rec;
    if (MsiDatabaseOpenViewW(db, q, &v) || MsiViewExecute(v, 0)) { con_print(L"  (no table)\r\n"); return; }
    while (!MsiViewFetch(v, &rec)) {
        int i;
        con_print(L" ");
        for (i = 1; i <= cols; i++) {
            WCHAR b[300];
            DWORD n = 300;
            b[0] = 0;
            MsiRecordGetStringW(rec, i, b, &n);
            con_print(L" [%s]", b);
        }
        con_print(L"\r\n");
        MsiCloseHandle(rec);
    }
    MsiViewClose(v);
    MsiCloseHandle(v);
}

static int dump(const WCHAR *path, const WCHAR *binout)
{
    MSIHANDLE db, si, v, rec;
    UINT type;
    INT ival = 0;
    FILETIME ft;
    DWORD n = 0;
    if (MsiOpenDatabaseW(path, (LPCWSTR)MSIDBOPEN_READONLY, &db)) { con_print(L"open failed\r\n"); return 1; }
    MsiGetSummaryInformationW(db, NULL, 0, &si);
    MsiSummaryInfoGetPropertyW(si, 14, &type, &ival, &ft, NULL, &n);
    con_print(L"schema=%d\r\n", ival);
    {
        WCHAR t[100] = L"";
        DWORD tn = 100;
        MsiSummaryInfoGetPropertyW(si, 7 /* PID_TEMPLATE */, &type, &ival, &ft, t, &tn);
        con_print(L"template=%s\r\n", t);
        tn = 100; t[0] = 0;
        MsiSummaryInfoGetPropertyW(si, 9 /* PID_REVNUMBER */, &type, &ival, &ft, t, &tn);
        con_print(L"package code=%s\r\n", t);
    }
    MsiCloseHandle(si);
    con_print(L"MsiServiceConfig table: %s\r\n",
              MsiDatabaseIsTablePersistentW(db, L"MsiServiceConfig") == MSICONDITION_TRUE ? L"present" : L"absent");
    con_print(L"LaunchCondition:\r\n");
    dump_query(db, L"SELECT `Condition`, `Description` FROM `LaunchCondition`", 2);
    con_print(L"InstallExecuteSequence:\r\n");
    dump_query(db, L"SELECT `Action`, `Condition` FROM `InstallExecuteSequence`", 2);
    if (!MsiDatabaseOpenViewW(db, L"SELECT `Data` FROM `Binary` WHERE `Name` = 'ModernCA'", &v) && !MsiViewExecute(v, 0) &&
        !MsiViewFetch(v, &rec)) {
        DWORD size = (DWORD)MsiRecordDataSize(rec, 1), got = size, put;
        char *buf = HeapAlloc(GetProcessHeap(), 0, size);
        HANDLE h = CreateFileW(binout, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        MsiRecordReadStream(rec, 1, buf, &got);
        WriteFile(h, buf, got, &put, NULL);
        CloseHandle(h);
        con_print(L"Binary ModernCA: %u bytes saved\r\n", got);
        MsiCloseHandle(rec);
    }
    MsiCloseHandle(db);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc == 4 && !vx_wcsicmp(argv[1], L"create")) return create(argv[2], argv[3]);
    if (argc == 4 && !vx_wcsicmp(argv[1], L"dump")) return dump(argv[2], argv[3]);
    con_print(L"usage: msitest create|dump <msi> <file>\r\n");
    return 2;
}

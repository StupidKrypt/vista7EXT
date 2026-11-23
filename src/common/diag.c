/* The diagnostics folder (see diag.h). */
#include "vx.h"
#include "diag.h"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#ifndef PROTECTED_DACL_SECURITY_INFORMATION
#define PROTECTED_DACL_SECURITY_INFORMATION 0x80000000L   /* winnt.h; missing from Wine's headers */
#endif

static const char g_readme[] =
    "VistaExt Diagnostics\r\n"
    "====================\r\n"
    "\r\n"
    "VistaExt writes what goes wrong on this PC into this folder, as it happens:\r\n"
    "\r\n"
    "  events.log       Windows' own error reports (a program crashed, hung, or\r\n"
    "                   could not start), whether VistaExt runs that program,\r\n"
    "                   and whether a VistaExt file was involved\r\n"
    "  decisions.log    which programs VistaExt set up, and which it left\r\n"
    "                   alone because they run on this Windows by themselves\r\n"
    "  report-<date>\\   a full snapshot, made with \"Save diagnostics\" in the\r\n"
    "                   VistaExt window (Start menu) or the tray icon's menu;\r\n"
    "                   it also has each user's crash log\r\n"
    "\r\n"
    "To report a problem: use the program until it goes wrong, click\r\n"
    "\"Save diagnostics\", then copy this whole folder (for example to a USB\r\n"
    "stick) and send it. Every user can read and copy it; only VistaExt (as\r\n"
    "administrator) writes to it.\r\n"
    "\r\n"
    "Privacy: the snapshot contains the names and paths of programs on this PC,\r\n"
    "Windows' error log, and crash dumps (memory of crashed programs, which can\r\n"
    "contain what you were doing in them, such as web pages). Look through it,\r\n"
    "and delete what you do not want to share, before sending it.\r\n"
    "\r\n"
    "Uninstalling VistaExt keeps this folder. Delete it when you no longer need it.\r\n";

/* Owner Administrators; SYSTEM and Administrators full control, Users read
 * and list (inherited by everything inside); protected: nothing inherited
 * from ProgramData, where every user may create files. */
#define DIAG_SDDL L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"

/* A regular file with one name: not a link a user planted. */
static BOOL plain_file(HANDLE f)
{
    BY_HANDLE_FILE_INFORMATION fi;
    return GetFileInformationByHandle(f, &fi) && fi.nNumberOfLinks == 1 &&
           !(fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}

/* Make <d> VistaExt's: created with the protected list; one that is already
 * there (perhaps made by a user before VistaExt was installed) is taken over
 * and given that list; a link (junction) is removed first. */
static BOOL secure_dir(const WCHAR *d)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    SECURITY_ATTRIBUTES sa;
    PSID owner = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE, ok = FALSE;
    DWORD a = GetFileAttributesW(d);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(DIAG_SDDL, SDDL_REVISION_1, &sd, NULL)) return FALSE;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        RemoveDirectoryW(d);                      /* removes the link, never what it points to */
        a = GetFileAttributesW(d);
    }
    if (a == INVALID_FILE_ATTRIBUTES) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
        ok = CreateDirectoryW(d, &sa);
    } else if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT) &&
               GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
               GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
        ok = SetNamedSecurityInfoW((LPWSTR)d, SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   owner, NULL, dacl, NULL) == ERROR_SUCCESS;
    }
    LocalFree(sd);
    return ok;
}

static void write_new(const WCHAR *path, const char *text, DWORD len)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD w;
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, "\xEF\xBB\xBF", 3, &w, NULL);
    WriteFile(f, text, len, &w, NULL);
    CloseHandle(f);
}

BOOL vx_diag_dir(WCHAR *out)
{
    static volatile LONG secured;        /* once per process: then it stays VistaExt's */
    WCHAR readme[MAX_PATH + 20];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, out))) return FALSE;
    vx_wcscatn(out, L"\\VistaExt Diagnostics", MAX_PATH);
    if (!secured) {
        if (!secure_dir(out)) return FALSE;
        InterlockedExchange(&secured, 1);
    }
    vx_wcscpyn(readme, out, MAX_PATH + 20);
    vx_wcscatn(readme, L"\\README.txt", MAX_PATH + 20);
    if (GetFileAttributesW(readme) == INVALID_FILE_ATTRIBUTES) write_new(readme, g_readme, sizeof(g_readme) - 1);
    return TRUE;
}

/* append; with <only_plain>, a file that is a link (or has several names)
 * is not written through */
static void append(const WCHAR *path, const WCHAR *text, BOOL only_plain)
{
    WCHAR old[MAX_PATH + 90];
    char *u8;
    int len;
    HANDLE f;
    DWORD w, flags = only_plain ? FILE_FLAG_OPEN_REPARSE_POINT : 0;
    len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (len <= 1 || !(u8 = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)len))) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, len, NULL, NULL);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, flags, NULL);
    if (f != INVALID_HANDLE_VALUE && only_plain && !plain_file(f)) { CloseHandle(f); f = INVALID_HANDLE_VALUE; }
    if (f != INVALID_HANDLE_VALUE && GetFileSize(f, NULL) > 2u * 1024 * 1024) {   /* keep one older file */
        CloseHandle(f);
        vx_wcscpyn(old, path, MAX_PATH + 90);
        vx_wcscatn(old, L".old", MAX_PATH + 90);
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, flags, NULL);
    }
    if (f != INVALID_HANDLE_VALUE) {
        if (!GetFileSize(f, NULL)) WriteFile(f, "\xEF\xBB\xBF", 3, &w, NULL);
        WriteFile(f, u8, (DWORD)len - 1, &w, NULL);
        CloseHandle(f);
    }
    HeapFree(GetProcessHeap(), 0, u8);
}

void vx_diag_append(const WCHAR *name, const WCHAR *text)
{
    WCHAR path[MAX_PATH + 80];
    if (!vx_diag_dir(path)) return;
    vx_wcscatn(path, L"\\", MAX_PATH + 80);
    vx_wcscatn(path, name, MAX_PATH + 80);
    append(path, text, TRUE);
}

void vx_append_utf8(const WCHAR *path, const WCHAR *text) { append(path, text, FALSE); }

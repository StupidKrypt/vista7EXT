/* spawntest - a "setup" that needs VistaExt (it imports a Windows 8 function
 * from kernel32) and, like a real setup, unpacks a helper program into Temp
 * and starts it at once.
 *   spawntest <program> [/shell]
 * copies <program> to %TEMP%\vxchild-<n>.exe and starts it (CreateProcess,
 * or ShellExecuteEx with /shell), waits, and returns its exit code. The
 * helper needs VistaExt too: it only starts if VistaExt's service registered
 * it in time, which VistaExt asks for right before it starts. */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"
#include <shellapi.h>

int wmain(int argc, WCHAR **argv)
{
    WCHAR tmp[MAX_PATH], child[MAX_PATH + 40], cmd[MAX_PATH + 50];
    FILETIME ft;
    DWORD code = 99;
    HANDLE proc = NULL;
    if (argc < 2) { con_print(L"usage: spawntest <program> [/shell]\r\n"); return 2; }
    GetSystemTimePreciseAsFileTime(&ft);            /* Windows 8: this program needs VistaExt itself */
    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(child, L"%svxchild-%u.exe", tmp, GetTickCount() ^ ft.dwLowDateTime);
    if (!CopyFileW(argv[1], child, FALSE)) { con_print(L"cannot copy %s (error %u)\r\n", argv[1], GetLastError()); return 3; }
    if (argc > 2 && !vx_wcsicmp(argv[2], L"/shell")) {
        SHELLEXECUTEINFOW se;
        vx_memset(&se, 0, sizeof(se));
        se.cbSize = sizeof(se);
        se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        se.lpFile = child;
        se.nShow = SW_HIDE;
        if (ShellExecuteExW(&se)) proc = se.hProcess;
        else con_print(L"ShellExecuteEx failed (error %u)\r\n", GetLastError());
    } else {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        vx_memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        wsprintfW(cmd, L"\"%s\"", child);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); proc = pi.hProcess; }
        else con_print(L"CreateProcess failed (error %u)\r\n", GetLastError());
    }
    if (proc) {
        WaitForSingleObject(proc, 60000);
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
    }
    con_print(L"helper %s exited 0x%08X\r\n", child, code);
    return (int)code;
}

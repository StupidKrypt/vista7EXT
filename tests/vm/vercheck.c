/* vercheck - a setup that runs on Vista/7 as it is (it imports nothing they
 * lack) but refuses them, the way Spotify's setup does: VerifyVersionInfo
 * must say Windows 10 or later.
 *   vercheck           0: told Windows 10+, 1: refused this Windows
 *   vercheck /driver   tries to install a driver that Windows loads while it
 *                      starts (a service entry only: the file does not exist).
 *                      0: created (and removed again), 5: refused */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

int wmain(int argc, WCHAR **argv)
{
    OSVERSIONINFOEXW want;
    BOOL ten;
    vx_memset(&want, 0, sizeof(want));
    want.dwOSVersionInfoSize = sizeof(want);
    want.dwMajorVersion = 10;
    ten = VerifyVersionInfoW(&want, VER_MAJORVERSION, VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL));
    con_print(ten ? L"Windows 10 or later: installing\r\n" : L"This program requires Windows 10 or later.\r\n");
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/driver")) {
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE), s;
        DWORD e;
        if (!m) { con_print(L"no service manager (error %u)\r\n", GetLastError()); return 9; }
        s = CreateServiceW(m, L"vxtestdrv", L"VistaExt test driver", SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                           SERVICE_SYSTEM_START, SERVICE_ERROR_IGNORE, L"\\SystemRoot\\System32\\drivers\\vxtestdrv-none.sys",
                           NULL, NULL, NULL, NULL, NULL);
        e = GetLastError();
        if (s) { DeleteService(s); CloseServiceHandle(s); CloseServiceHandle(m); con_print(L"driver service created\r\n"); return 0; }
        CloseServiceHandle(m);
        con_print(L"driver service refused (error %u)\r\n", e);
        return (int)e;
    }
    return ten ? 0 : 1;
}

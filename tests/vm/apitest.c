/* apitest [/wait:<seconds>]
 * Imports newer functions through API-set names (api-ms-win-core-synch-l1-2-0,
 * api-ms-win-core-sysinfo-l1-2-0, and Windows 10's SetThreadDescription from
 * api-ms-win-core-processthreads-l1-1-3, which Vista, 7 and 8 all lack, so it
 * needs VistaExt on each of them) instead of kernel32, the way programs built
 * with Microsoft's "OneCore" libraries do. vm.sh links it against import
 * libraries that name those DLLs. /wait keeps it running (a program that is
 * in use, for the uninstall test). */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

int wmain(int argc, WCHAR **argv)
{
    FILETIME ft;
    volatile LONG v = 0;
    LONG zero = 0;
    BOOL r;
    DWORD secs = 0, i;

    HRESULT hr;
    GetSystemTimePreciseAsFileTime(&ft);
    WakeByAddressAll((PVOID)&v);
    r = WaitOnAddress(&v, &zero, sizeof(zero), 10);
    if (ft.dwHighDateTime < 0x01D00000 || r || GetLastError() != ERROR_TIMEOUT) {
        con_print(L"apitest: FAIL (time %08x, wait %d, error %u)\r\n", ft.dwHighDateTime, r, GetLastError());
        return 1;
    }
    if ((hr = SetThreadDescription(GetCurrentThread(), L"apitest")) != S_OK) {
        con_print(L"apitest: FAIL (SetThreadDescription 0x%08x)\r\n", (UINT)hr);
        return 1;
    }
    con_print(L"apitest: functions imported through API sets work\r\n");
    if (argc > 1 && !vx_wcsnicmp(argv[1], L"/wait:", 6)) {
        for (i = 6; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) secs = secs * 10 + (argv[1][i] - '0');
        Sleep(secs * 1000);
    }
    return 0;
}

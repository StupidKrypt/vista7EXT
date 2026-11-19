/* nofault <seconds> <program> [arguments]
 * Runs a program with Windows' error pop-ups switched off, waits for it at
 * most <seconds>, prints its exit code and returns it.
 * In an unattended test, the loader's "entry point not found" box or a crash
 * dialog would wait forever for someone to click OK. The error mode set here
 * is inherited by the program, so it fails with an exit code instead
 * (0xC0000139 = entry point not found, 0xC0000135 = DLL not found). */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

/* skip one command-line argument (quoted or not) and the spaces after it */
static WCHAR *skip_arg(WCHAR *p)
{
    BOOL q = FALSE;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') q = !q;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int wmain(int argc, WCHAR **argv)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *cmd;
    DWORD secs = 0, rc = 0, i;
    if (argc < 3) {
        con_print(L"usage: nofault <seconds> <program> [arguments]\r\n");
        return 2;
    }
    for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) secs = secs * 10 + (argv[1][i] - '0');
    cmd = skip_arg(skip_arg(GetCommandLineW()));

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    vx_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    /* redirected output (a file) must be inheritable to reach the program */
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        con_print(L"[nofault: could not start, error %u]\r\n", GetLastError());
        return 9009;
    }
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, secs ? secs * 1000 : INFINITE) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, WAIT_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 5000);
        con_print(L"[nofault: still running after %u s, stopped]\r\n", secs);
    }
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    con_print(L"[exit 0x%08X]\r\n", rc);
    return (int)rc;
}

/* Entry point and output for the VistaExt console tools (no CRT). */
#include "vx.h"
#include <shellapi.h>
#include <stdarg.h>

int wmain(int argc, WCHAR **argv);

void con_write(const WCHAR *s)
{
    static HANDLE conout;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {         /* a windowed tool attached to its caller's console */
        if (!conout) conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          NULL, OPEN_EXISTING, 0, NULL);
        h = conout;
    }
    DWORD mode, n, len = (DWORD)vx_wcslen(s);
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, len, &n, NULL);
    } else {
        char buf[4096];
        int b = WideCharToMultiByte(CP_UTF8, 0, s, (int)len, buf, sizeof(buf), NULL, NULL);
        if (b > 0) WriteFile(h, buf, (DWORD)b, &n, NULL);
    }
}

void con_print(const WCHAR *fmt, ...)
{
    WCHAR buf[1100];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    con_write(buf);
}

/* log callback for patchlib */
void con_log(void *ctx, const WCHAR *line) { con_write(line); con_write(L"\r\n"); }

void mainCRTStartup(void)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    ExitProcess(argv ? (UINT)wmain(argc, argv) : 2);
}

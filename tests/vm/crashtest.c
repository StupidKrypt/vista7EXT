/* crashtest - a windowed program that needs VistaExt (imports a Windows 8
 * function from kernel32), runs for a few seconds and then stops itself the
 * way Chromium does on a failed check: a formatted "FATAL" line on the stack,
 * then a breakpoint. VistaExt's crash window should then explain it.
 *   crashtest [/av]    /av: a memory access violation instead
 *   crashtest /helper  starts itself as a helper process (as multi-process
 *                      programs do), the helper crashes, this one ends
 *                      normally: no crash window should appear */
#include "../../src/common/vx.h"

static void __attribute__((noinline)) stop(void)
{
    volatile char msg[160];
    static const char text[] = "[1:2:1118/120000.000:FATAL:crashtest.cc:42] Check failed: VistaExt crash window test.";
    int i;
    for (i = 0; text[i] && i < 159; i++) msg[i] = text[i];
    msg[i] = 0;
    __asm__ volatile("int3");
    (void)msg[0];
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cl, int show)
{
    volatile LONG v = 0;
    LONG zero = 0;
    OSVERSIONINFOW vi;
    vi.dwOSVersionInfoSize = sizeof(vi);
    /* restarted from the crash window told Windows 10: say so and end normally */
    if (GetVersionExW(&vi) && vi.dwMajorVersion == 10) {
        WCHAR p[MAX_PATH];
        SIZE_T n = GetModuleFileNameW(NULL, p, MAX_PATH);
        HANDLE f;
        while (n && p[n - 1] != '\\') n--;
        p[n] = 0;
        lstrcatW(p, L"restarted-as-10.txt");
        f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return 0;
    }
    {   /* /helper: start a copy of itself that crashes (with a .NET-style UTF-16
         * message), wait for it, then end normally */
        const WCHAR *c = GetCommandLineW();
        for (; *c; c++) if (c[0] == '/' && c[1] == 'h' && c[2] == 'e') break;
        if (*c) {
            WCHAR exe[MAX_PATH];
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            if (CreateProcessW(exe, (WCHAR[]){ L"crashtest.exe /wide" }, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 20000);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
            return 0;
        }
    }
    WaitOnAddress(&v, &zero, sizeof(zero), 4000);      /* needs VistaExt; also the few seconds */
    {   /* /wide: the reason as UTF-16 text, then a fail-fast */
        const WCHAR *c = GetCommandLineW();
        for (; *c; c++) if (c[0] == '/' && c[1] == 'w' && c[2] == 'i') {
            volatile WCHAR msg[96];
            static const WCHAR text[] = L"Unhandled exception: VistaExt crash window test (UTF-16).";
            int i;
            for (i = 0; text[i]; i++) msg[i] = text[i];
            msg[i] = 0;
            __asm__ volatile("int3");
            (void)msg[0];
        }
    }
    { const WCHAR *c = GetCommandLineW(); for (; *c; c++) if (c[0] == '/' && c[1] == 'a' && c[2] == 'v') *(volatile int *)0 = 1; }
    stop();
    return 0;
}

void WinMainCRTStartup(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
}

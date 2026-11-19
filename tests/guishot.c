/* guishot <vxcenter.exe> <program to open> <out.bmp> [/onscreen]
 * Starts VistaExt Center hidden (so nothing appears on screen), lists its
 * controls, captures the window with PrintWindow and closes it.
 * /onscreen (test VMs): shows it normally and copies it from the screen;
 * real Windows does not draw themed controls into an off-screen capture. */
#include "../src/common/vx.h"
#include "../src/common/console.h"

static BOOL CALLBACK child(HWND h, LPARAM lp)
{
    WCHAR cls[64], text[300];
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, text, 300);
    if (text[0] && vx_wcsicmp(cls, L"Edit") ) con_print(L"  [%s] %s\r\n", cls, text);
    else if (!vx_wcsicmp(cls, L"Edit")) {
        /* another process's edit text needs WM_GETTEXT, not GetWindowText */
        int n = (int)SendMessageW(h, WM_GETTEXT, 300, (LPARAM)text);
        con_print(L"  [Edit] %d chars%s%s\r\n", n, n && n < 120 ? L": " : L"", n && n < 120 ? text : L"");
    }
    return TRUE;
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR cmd[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HWND w = NULL;
    RECT rc;
    int i, cw, ch, x = 0, y = 0;
    BOOL onscreen = argc > 4 && !vx_wcsicmp(argv[4], L"/onscreen");
    if (argc < 4) return 2;
    vx_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    wsprintfW(cmd, L"\"%s\" \"%s\"", argv[1], argv[2]);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { con_print(L"start failed\r\n"); return 1; }
    WaitForInputIdle(pi.hProcess, 10000);
    for (i = 0; i < 100 && !(w = FindWindowW(L"VistaExtCenter", NULL)); i++) Sleep(100);
    if (!w) { con_print(L"window not found\r\n"); TerminateProcess(pi.hProcess, 1); return 1; }
    /* show it off-screen, without a taskbar button, so PrintWindow has pixels
     * but nothing appears on the desktop */
    if (onscreen) {
        SetWindowPos(w, HWND_TOPMOST, 20, 20, 0, 0, SWP_NOSIZE);
        ShowWindow(w, SW_SHOWNORMAL);
        SetForegroundWindow(w);
    } else {
        SetWindowLongW(w, GWL_EXSTYLE, GetWindowLongW(w, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        SetWindowPos(w, NULL, -5000, -5000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(w, SW_SHOWNOACTIVATE);
    }
    Sleep(2500);
    GetWindowTextW(w, cmd, 200);
    con_print(L"window: %s\r\n", cmd);
    EnumChildWindows(w, child, 0);
    if (onscreen) {
        GetWindowRect(w, &rc);
        x = rc.left; y = rc.top; cw = rc.right - rc.left; ch = rc.bottom - rc.top;
    } else {
        GetClientRect(w, &rc);
        cw = rc.right; ch = rc.bottom;
    }
    {
        HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
        BITMAPINFO bi;
        void *bits;
        HBITMAP bmp;
        vx_memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = ch;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(mdc, bmp);
        if (onscreen) BitBlt(mdc, 0, 0, cw, ch, sdc, x, y, SRCCOPY);
        else PrintWindow(w, mdc, 1 /* PW_CLIENTONLY */);
        /* GDI leaves the 4th byte 0, themed parts set it to 255: viewers
         * would take it as transparency and lose all plain text */
        for (i = 0; i < cw * ch; i++) ((DWORD *)bits)[i] |= 0xFF000000;
        {
            BITMAPFILEHEADER fh;
            DWORD sz = (DWORD)cw * ch * 4, put;
            HANDLE f = CreateFileW(argv[3], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            vx_memset(&fh, 0, sizeof(fh));
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
            fh.bfSize = fh.bfOffBits + sz;
            WriteFile(f, &fh, sizeof(fh), &put, NULL);
            WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &put, NULL);
            WriteFile(f, bits, sz, &put, NULL);
            CloseHandle(f);
        }
        con_print(L"captured %dx%d\r\n", cw, ch);
    }
    PostMessageW(w, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    return 0;
}

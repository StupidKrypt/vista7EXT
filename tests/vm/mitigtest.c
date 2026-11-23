/* mitigtest - what this Windows does with process-creation mitigation
 * policies (PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY) it does not know:
 * for each mask, whether UpdateProcThreadAttribute accepts it and whether
 * CreateProcess then starts a child with it. No VistaExt involved (Windows'
 * own kernel32). Research for kernelvx's UpdateProcThreadAttribute.
 *   mitigtest            the table
 *   mitigtest /child     (the child: exits 0 at once) */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

#define ATTR_MITIGATION 0x00020007

static void try_mask(const WCHAR *what, const ULONGLONG *mask, SIZE_T size)
{
    SIZE_T sz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 20];
    BOOL up, cp = FALSE;
    DWORD eu = 0, ec = 0, code = 999;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    if (!(list = HeapAlloc(GetProcessHeap(), 0, sz)) || !InitializeProcThreadAttributeList(list, 1, 0, &sz)) {
        con_print(L"%-40s  (no attribute list)\r\n", what);
        return;
    }
    up = UpdateProcThreadAttribute(list, 0, ATTR_MITIGATION, (PVOID)mask, size, NULL, NULL);
    if (!up) eu = GetLastError();
    if (up) {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wsprintfW(cmd, L"\"%s\" /child", exe);
        vx_memset(&si, 0, sizeof(si));
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = list;
        cp = CreateProcessW(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
        if (!cp) ec = GetLastError();
        else {
            WaitForSingleObject(pi.hProcess, 10000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    con_print(L"%-40s  size %u  Update: %s (%u)  CreateProcess: %s (%u)  child exit %u\r\n", what, (unsigned)size,
              up ? L"ok" : L"FAILS", eu, !up ? L"-" : cp ? L"ok" : L"FAILS", ec, code);
    DeleteProcThreadAttributeList(list);
    HeapFree(GetProcessHeap(), 0, list);
}

int wmain(int argc, WCHAR **argv)
{
    static const ULONGLONG none = 0, dep = 0x1, w7 = 0x7, w8 = 0x333333307ULL,
        w81 = 0x333333307ULL | (0x3ULL << 36) | (0x3ULL << 40),          /* + dynamic code, CFG (8.1) */
        w10 = 0x333333307ULL | (0x3ULL << 36) | (0x3ULL << 40) | (0x3ULL << 44) | (0x3ULL << 48) | (0x3ULL << 52),
        w8only_force_reloc = 0x1ULL << 8, w81only_cfg = 0x1ULL << 40, w10only_font = 0x1ULL << 48;
    static const ULONGLONG two[2] = { 0x1ULL << 8, 0x1ULL << 4 };    /* Windows 10's 16-byte form */
    DWORD w = 0;
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/child")) return 0;
    if (argc > 1 && !vx_wcsicmp(argv[1], L"/w10")) {
        /* what Chromium told Windows 10 asks for its children: Windows 8,
         * 8.1 and 10 policies in one 8-byte mask. Exit 0: the child started. */
        static const ULONGLONG m = (0x1ULL << 8) | (0x1ULL << 16) | (0x1ULL << 36) | (0x1ULL << 40) | (0x1ULL << 48);
        SIZE_T sz = 0;
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        STARTUPINFOEXW si;
        PROCESS_INFORMATION pi;
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 20];
        InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
        if (!(list = HeapAlloc(GetProcessHeap(), 0, sz)) || !InitializeProcThreadAttributeList(list, 1, 0, &sz)) return 3;
        if (!UpdateProcThreadAttribute(list, 0, ATTR_MITIGATION, (PVOID)&m, sizeof(m), NULL, NULL)) {
            con_print(L"UpdateProcThreadAttribute failed (%u)\r\n", GetLastError());
            return 2;
        }
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wsprintfW(cmd, L"\"%s\" /child", exe);
        vx_memset(&si, 0, sizeof(si));
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = list;
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
            con_print(L"CreateProcess failed (%u)\r\n", GetLastError());
            return 1;
        }
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        con_print(L"the child started\r\n");
        return 0;
    }
    con_print(L"Windows %u.%u build %u\r\n", (unsigned)(GetVersion() & 0xFF), (unsigned)((GetVersion() >> 8) & 0xFF),
              (unsigned)(GetVersion() >> 16));
    (void)w;
    try_mask(L"nothing (0)", &none, sizeof(ULONGLONG));
    try_mask(L"DEP, 4 bytes (Windows 7's size)", &dep, sizeof(DWORD));
    try_mask(L"Windows 7's bits (DEP, ATL, SEHOP)", &w7, sizeof(ULONGLONG));
    try_mask(L"one Windows 8 bit (force relocation)", &w8only_force_reloc, sizeof(ULONGLONG));
    try_mask(L"all Windows 8 bits", &w8, sizeof(ULONGLONG));
    try_mask(L"one Windows 8.1 bit (CFG)", &w81only_cfg, sizeof(ULONGLONG));
    try_mask(L"Windows 8 + 8.1 bits", &w81, sizeof(ULONGLONG));
    try_mask(L"one Windows 10 bit (font disable)", &w10only_font, sizeof(ULONGLONG));
    try_mask(L"Windows 8 + 8.1 + 10 bits", &w10, sizeof(ULONGLONG));
    try_mask(L"16 bytes (Windows 10's two masks)", two, sizeof(two));
    {   /* each 2-bit field from bit 8 on, alone: 1 = always on, 2 = always off
         * (3 is no valid value) */
        static ULONGLONG v;
        WCHAR t[80];
        unsigned bit, val;
        for (bit = 8; bit <= 60; bit += 4)
            for (val = 1; val <= 2; val++) {
                v = (ULONGLONG)val << bit;
                wsprintfW(t, L"field at bit %u = %u", bit, val);
                try_mask(t, &v, sizeof(ULONGLONG));
            }
    }
    return 0;
}

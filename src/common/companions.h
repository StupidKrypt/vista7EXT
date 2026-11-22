/* The Windows DLLs VistaExt stands in for, and its companion for each: one
 * list for vxload (in-memory mode), vxpatch (file mode) and kernelvx
 * (GetProcAddress). Each user defines
 *     VX_COMPANION(dll, companion, numbers)
 * and expands VX_COMPANIONS. "numbers": the companion exports the original's
 * export numbers too (tools/gen.py, third_party/winexports/<dll>.ords), so
 * imports by number can be switched. kernelvx can't: it stands in for both
 * kernel32 and kernelbase, whose numbers differ.
 *
 * Per Windows (build.sh: -DVX_OS): Vista has no bcryptprimitives.dll at all;
 * VistaExt installs its own, which programs then use directly. Windows 7 has
 * one without ProcessPrng, which bcryptprimvx adds (on 8, the generator
 * forwards it to Windows where Windows 8 has it).
 *
 * A companion's name is never longer than its DLL's (the in-memory mode
 * renames imports in place): dxgi -> dxvx, winhttp -> winhtvx, userenv ->
 * uenvvx. These four keep no export numbers (no list of them is kept). */
#ifndef VX_COMPANIONS_H
#define VX_COMPANIONS_H

#if VX_OS >= 601
#define VX_COMPANION_BCRYPT VX_COMPANION(bcryptprimitives, bcryptprimvx, 1)
#else
#define VX_COMPANION_BCRYPT
#endif

#define VX_COMPANIONS \
    VX_COMPANION(kernel32,   kernelvx, 0) \
    VX_COMPANION(kernelbase, kernelvx, 0) \
    VX_COMPANION(user32,     uservx,   1) \
    VX_COMPANION(advapi32,   advapivx, 1) \
    VX_COMPANION(shell32,    shellvx,  1) \
    VX_COMPANION_BCRYPT \
    VX_COMPANION(ntdll,      ntvx,     1) \
    VX_COMPANION(ws2_32,     ws2vx,    1) \
    VX_COMPANION(cfgmgr32,   cfgmgrvx, 1) \
    VX_COMPANION(ole32,      olevx,    1) \
    VX_COMPANION(version,    versvx,   1) \
    VX_COMPANION(dxgi,       dxvx,     0) \
    VX_COMPANION(dnsapi,     dnsvx,    0) \
    VX_COMPANION(winhttp,    winhtvx,  0) \
    VX_COMPANION(userenv,    uenvvx,   0)

#endif

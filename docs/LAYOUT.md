# How VistaExt is split: per Windows, per architecture

VistaExt is built as **four targets** from one source tree:

| Target | Output | Installed on |
|---|---|---|
| Vista x86 | `out/vista-x86` | 32-bit Vista: `System32`. 64-bit Vista: `SysWOW64` (for 32-bit programs) |
| Vista x64 | `out/vista-x64` | 64-bit Vista: `System32` |
| Windows 7 x86 | `out/win7-x86` | 32-bit Windows 7: `System32`. 64-bit Windows 7: `SysWOW64` |
| Windows 7 x64 | `out/win7-x64` | 64-bit Windows 7: `System32` |

**One installer for all four.** `VistaExtSetup.exe` (and `install.cmd` in the zip) carries every build. `install.cmd` is the only place that chooses: it reads the Windows version once, at install time, and installs `<os>\<arch>`. On 64-bit Windows it also installs that Windows' x86 build into `SysWOW64`. It records the choice as `HKLM\SOFTWARE\VistaExt\Build` (for example `vista-x64`).

Nothing in the installed DLLs asks which Windows it runs on: every per-Windows difference is decided at build time.

## Why one installer, not two

Splitting the installer would bring no benefit here:
- **Detection is one registry read** (`CurrentVersion` 6.0 or 6.1), already needed for the "is this Vista or 7 at all?" check.
- **Same prerequisites.** Both builds need the same ones and the same installer mechanism (a batch script, wrapped by a small setup program).
- **No compromises in shared code.** Keeping them together needs none. The only cost is size: identical files are stored once (`tools/mkpayload.py`), so the setup file grew from 20.4 MB to 23.6 MB for four builds instead of two.

Two installers would only add a way to download the wrong one.

## Why architectures are build configs, not separate versions

- **64-bit Windows needs both architectures.** Its 32-bit programs run under WOW64 and need the x86 build in `SysWOW64`. An "x64-only" product could not exist; it would always have to ship the x86 build too.
- **The architecture difference is tiny.** About 10 `#ifdef _WIN64` blocks and 2 pointer-size branches:
  - PEB/TEB offsets (`kernelvx`, `ntvx`, `vxtest`);
  - the stack pointer in the crash recorder;
  - DEP defaults (always on for 64-bit programs);
  - 64-bit division in the precise clock (`MulDiv` on x86, which has no 64-bit divide without a runtime library);
  - which WebView2 runtime folder to read;
  - x64-only growable function tables (`ntvx`);
  - choosing `System32`, `SysWOW64` or `Sysnative` and the 32/64-bit registry view (`patchlib`, `vxsetup`).

  The build adds the per-architecture flags: target triple, entry-point decoration, `--high-entropy-va`, and stdcall name decoration in `gen.py`. There is no inline assembly and there are no drivers.
- **Splitting the code would only duplicate it.** Both architectures share over 95% of it.

## What is shared, what is per Windows

### Shared (one implementation, both Windows)

- **Everything from Windows 8 and later**, which neither Vista nor 7 has. That is about 165 functions:
  - `WaitOnAddress`, the precise clock;
  - DPI awareness, the packaged-app and power functions;
  - `RtlGetDeviceFamilyInfoEnum`, `FlsGetValue2`, …;
  - `combase.dll`, `shcore.dll`;
  - the 264 API-set forwarders and their `vxs-` copies for the in-memory mode.
- **Behaviour fixes on top of functions both have:**
  - `CreateFileMapping*` names;
  - the version functions and the per-program version setting;
  - `GetProcAddress`'s companion lookup;
  - `CreateProcessW` extra arguments;
  - `InitializeCriticalSectionEx` flags;
  - `GetFileInformationByHandleEx` classes;
  - `EventWriteTransfer`, `SHCreateItemInKnownFolder`, `SHGetKnownFolderIDList`.
- **The tools:** `vxload` (in-memory mode), `vxpatch`, `vxauto` (the service, the on/off switch, the global version), `vxtray` (the tray icon), `vxcenter`, `vxwatch`, `msifix`, and the setup wizard (`VistaExtSetup.exe`, one 32-bit program for all four builds).
  - **`vxpatch` and `vxauto`:** they compare a program against *this PC's* DLLs, which is data, not a version check.
  - **`msifix`:** built per Windows (see below).
- **`LoadLibraryEx` search flags:** the emulation stays in both builds and steps aside when Windows has them. That depends on update KB2533623 (available for Vista and 7), not on the Windows version. `AddDllDirectory`, `RemoveDllDirectory` and `SetDefaultDllDirectories` are the only functions that still look for Windows' own version at run time (`VX_NATIVE_UPDATE` in `src/common/vx.h`).
- **The table of companions** (`src/common/companions.h`) is one list used by `vxload`, `vxpatch` and `kernelvx`. It differs only in its `bcryptprimitives` entry (below).

### Diverging: Windows 7 has it, Vista doesn't (64 functions)

On Vista these are VistaExt's code. On Windows 7 they are not compiled in (`#if !VX_WIN7`), and the Windows 7 build's export table forwards those names straight to Windows (`tools/gen.py`). No wrapper, no run-time lookup.

| Area | Functions | Files |
|---|---|---|
| processor groups, NUMA | `GetActiveProcessorCount`, `GetLogicalProcessorInformationEx`, `Get/SetThreadGroupAffinity`, `GetNuma*Ex`, … (14) | `kernelvx/sysinfo.c` |
| threads, power requests, errors | `SetThreadErrorMode`, `Power*Request`, `SetWaitableTimerEx`, `RaiseFailFastException`, `CreateRemoteThreadEx`, … | `kernelvx/thread.c`, `misc.c` |
| locks | `TryAcquireSRWLockExclusive/Shared` | `kernelvx/synch.c` |
| touch, gestures, display config, popups | 19 functions | `uservx/uservx.c` |
| taskbar app IDs, known folders | 5 functions | `shellvx/shellvx.c` |
| others | `NtOpenKeyEx`, `RtlQueryPerformance*`, `EventWriteEx`, `TraceSetInformation`, `CM_Get_DevNode_PropertyW`, `CoGetApartmentType`, … | `ntvx`, `advapivx`, `cfgmgrvx`, `olevx` |

The Windows 7 build's companions are correspondingly smaller:

| Companion (x64) | Vista build | Windows 7 build |
|---|---|---|
| `kernelvx` | 150 functions | 119 |
| `uservx` | 71 | 52 |
| `shellvx` | 7 | 2 |
| `ntvx` | 8 | 4 |

### Incompatible: different per Windows, chosen at build time

| What | Vista build | Windows 7 build |
|---|---|---|
| `kernelbase.dll` | VistaExt's (Vista has none), forwarding to `kernelvx` | not built: Windows 7 has its own |
| `bcryptprimitives` | VistaExt's own `bcryptprimitives.dll` (Vista has none), used directly | `bcryptprimvx.dll`, adding `ProcessPrng` to Windows 7's |
| child-process attributes (`UpdateProcThreadAttribute`) | only parent process and handle list exist | adds processor groups, NUMA, UMS, mitigation policy; reduces 64-bit mitigation masks to Windows 7's 32-bit one |
| `msifix` | lowers packages to Windows Installer 4.5, removes 5.0-only tables | keeps Installer 5.0 |
| header version `vxpatch` writes | 6.0 | 6.1 |
| forwarded names | only names Vista's DLLs have (or that VistaExt implements) | only names Windows 7's DLLs have |
| export numbers kept | Vista's (per architecture) | Windows 7's (per architecture) |
| API-set targets (`gen.py kernel_host`) | decided against Vista's export lists | against Windows 7's |

How it's selected: `build.sh` compiles each target with `-DVX_OS=600` (Vista) or `-DVX_OS=601` (Windows 7). `src/common/vx.h` turns that into `VX_VISTA` / `VX_WIN7`, and `tools/gen.py --os vista|win7` generates that target's export tables from `third_party/winexports` (names and export numbers of the real Vista and 7 DLLs).

### Per architecture

Same source; `build.sh` passes the target triple, and the few `#ifdef _WIN64` blocks above do the rest. `gen.py` reads stdcall-decorated names on x86 and keeps each architecture's own export numbers. `install.cmd` relaunches itself as 64-bit when started from 32-bit, so `%ProgramFiles%` and the registry are the 64-bit ones. The tools go to `Program Files\VistaExt`, settings to `HKLM\SOFTWARE\VistaExt` in the 64-bit view (`VX_REG_VIEW`).

## Known consequences

- **In-place upgrades.** A PC upgraded from Vista to 7 in place still has the Vista build. Running the setup again installs the Windows 7 build over it; a leftover Vista-only file (`kernelbase.dll`) can't exist on 7, because Windows 7's own one prevents it. `HKLM\SOFTWARE\VistaExt\Build` shows which build is installed.
- **Upgrading from the earlier combined build.** Its `bcryptprimvx.dll` stays on a Vista PC until uninstall. Nothing uses it any more, and uninstall removes it.
- **Where the Vista-only code is tested.** VistaExt's own versions of the 64 Windows 7 functions exist only in the Vista build, so only the Vista VMs (and Wine, against a fake Vista system folder) exercise them.
- **`VX_NO_NATIVE=1`** now only affects the update-provided `AddDllDirectory` group.

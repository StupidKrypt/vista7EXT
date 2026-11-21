# VistaExt

An add-on layer that lets programs built for Windows 7–11 run on **Windows Vista SP2** and **Windows 7 SP1**, with per-program control over which Windows version a program sees. Inspired by [One-Core-API](https://github.com/shorthorn-project/One-Core-API-Binaries) (which does this for XP/2003 by replacing system files), but built the other way round: **VistaExt never replaces a Windows file.**

> Status: builds for x86 and x64. The test suite passes on real **Windows Vista SP2** and **Windows 7 SP1**, 32- and 64-bit (in virtual machines), and under Wine. In those VMs, Brave runs on Windows 7 and Vista, and plays 4K YouTube video smoothly, after nothing more than installing VistaExt and copying the program into Program Files. Firefox opens its window but **does not load pages yet**: its sandboxed child processes can't start (see KNOWN_RISKS.md §8b). Discord's Electron shell starts; Epic Games installs but does not start without a GPU. **Read [KNOWN_RISKS.md](KNOWN_RISKS.md) before using it**: it lists what is verified, the blind spots, the conflicts with other software and the backward-compatibility risks.

## Installing

Run **`VistaExtSetup.exe`** (one file, 32-bit, for all four builds). It is a short wizard:

1. **Warning.** VistaExt is experimental and could damage the Windows installation. *Accept* or *Decline*; Decline changes nothing.
2. **Backup.** "Create a System Restore point before installing" is checked by default; *Open Windows Backup...* starts Windows' own Backup and Restore for a full backup. If the restore point fails (System Restore turned off, no space), setup says why and asks whether to continue without it.
3. **Install.** The build for this Windows and architecture, the background service, the tray icon and its start with Windows.
4. **Finish.** "Start VistaExt's tray icon now" (checked).

`/quiet` installs without screens (`/restorepoint` adds the restore point), `/noauto` leaves the automatic mode out, `/uninstall` removes VistaExt (Programs and Features runs this).

**Using it.** After that, nothing is needed per program. Setup programs you download and programs you install are made compatible by themselves, the way Windows' own compatibility system works. Double-click a setup as on newer Windows: it goes through the usual UAC prompt and installs into its usual folders, with its own entry in Programs and Features.

The only visible part is **VistaExt's icon in the notification area**. Click it for:
- **on/off** for the whole system;
- **the Windows version programs are told**: Windows 7 SP1, 8, 8.1, 10 22H2 (10.0.19045), 11 23H2 (10.0.22631), this Windows' own version, or a custom version;
- **"Setup programs are told it too"**;
- **settings** (VistaExt Center);
- **the log**;
- **"Start with Windows"**;
- **Exit**.

**Turning it off.** *Off* takes VistaExt out of Windows **without a restart**. Programs that are running keep it until they are closed. A changed program that is running is put back as soon as it is closed.

**Safe Mode.** VistaExt does nothing in Safe Mode, and Windows does not need it to start. If it causes trouble, start in Safe Mode and choose **Start menu > Turn off VistaExt (also in Safe Mode)**; from a command prompt as administrator: `"%ProgramFiles%\VistaExt\vxauto.exe" /disable`. VistaExt then stays off after the next normal start, until it is turned on again.

**Logs.** Every change VistaExt makes is logged in `%ProgramData%\VistaExt\VistaExt.log` (*View log* in the tray menu); setup in `setup.log` next to it.

## How it works

| Part | What it does |
|---|---|
| **Companion DLLs** `kernelvx`, `uservx`, `advapivx`, `shellvx`, `bcryptprimvx`, `ntvx`, `ws2vx`, `cfgmgrvx`, `olevx`, `versvx` | Export everything the matching Windows DLL (kernel32, user32, advapi32, shell32, bcryptprimitives, ntdll, ws2_32, cfgmgr32, ole32, version) has, as forwarders, plus the newer functions. The forwarder lists come from the real Vista and 7 DLLs (`third_party/winexports`, names only). `ws2vx` also keeps Winsock's classic export numbers, which programs linked with `ws2_32.lib` import by number. |
| **New DLLs** `kernelbase`, `combase`, `shcore`, `bcryptprimitives` | DLLs that Vista doesn't have at all. On Windows 7 the installer keeps Windows' own copies. |
| **262 API-set DLLs** `api-ms-win-*` | Forwarders generated from One-Core-API's API-set lists, pointed at Vista/7 DLLs or the companions. The UCRT sets (`api-ms-win-crt-*`) are left to Microsoft's KB2999226. Every VistaExt DLL carries a version resource and an ownership marker, so the installer and uninstaller only ever replace or delete their own files. |
| **vxpatch** (library + CLI) | Reads a program's imports, compares them with *this PC's* system DLLs and, only where needed, renames e.g. `kernel32.dll` → `kernelvx.dll` in the import table. It also lowers the "needs Windows 10.0" version in a program's header (programs built for Windows 10 have it; Vista and 7 refuse to start them otherwise). The names are the same length or shorter, so the file layout doesn't change. Handles delay-loaded imports, bound imports and the PE checksum. A program that imports one DLL through several import tables gets all of them switched. Imports through API-set names (`api-ms-win-*`) are checked too, and pointed at a companion when this Windows' copy lacks the functions. The original is kept as `<file>.vxorig` and every patched file is recorded, so uninstalling undoes them all. If a program has updated itself since it was patched, undo leaves the new version alone instead of putting the old one back. |
| **Windows version** | **Global** (the tray icon; `HKLM\SOFTWARE\VistaExt` `Version`, default Windows 10): what every program VistaExt runs is told. **Per program or folder** (VistaExt Center, the crash window; `...\Layers`) overrides it, including "this Windows' own version" (`REAL`). Presets: Vista SP2, 7 SP1, 8, 8.1, 10 22H2 (19045), 11 23H2 (22631); custom: `VER:10.0.26100[.sp]`. The program sees **one** version everywhere: `kernelvx` writes it into the PEB (`RtlGetVersion`, and `GetVersionEx` inside Windows' own DLLs) and answers `GetVersion(Ex)` and `VerifyVersionInfo`; `ntvx` answers `RtlGetNtVersionNumbers`; `versvx` gives it as kernel32's and ntdll's file version; in the in-memory mode `vxload` answers the values of `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` (`CurrentVersion`, `CurrentBuild(Number)`, `CurrentMajor/MinorVersionNumber`, `ProductName`, `EditionID`, `DisplayVersion`, `ReleaseId`, `UBR`, `CSDVersion`, `BuildLab(Ex)`) the way that Windows shows them, also when Windows' own DLLs read them for the program (shlwapi, .NET); and `olevx` makes WMI's `Win32_OperatingSystem` (`Version`, `BuildNumber`, `Caption`, service pack) agree. Choose Windows 11 only for programs that ask for it: it changes the number programs see, not how fast they run. |
| **versvx** | Used only by programs with a version setting: reports the chosen Windows version as the *file* version of kernel32, kernelbase and ntdll (Chromium checks that, not just `GetVersionEx`). |
| **Service** `vxauto` ("VistaExt automatic mode", LocalSystem, no window) | **Installed programs:** it watches Program Files, Program Files (x86) and every user's AppData\Local and AppData\Roaming, and after an installer or updater has been quiet for 20 s does what VistaExt Center's Apply does, if something in the folder needs VistaExt at all (with the Chromium options for Chromium/Electron programs). **Setup programs:** a single .exe saved to a user's Downloads or Desktop, or written to their Temp folder or Internet Explorer's download cache, is looked at 2 s later: if it cannot run on this Windows by itself (missing functions, or a header asking for a newer Windows) it is registered for the in-memory mode, so a double-click starts it like on newer Windows. A *setup program* (recognised like Windows' installer detection: name, version information, installer engine) that runs as it is is registered too, so that it is told the chosen version (Spotify's setup refuses anything below Windows 10) - but not one signed by Microsoft (runtimes and updates choose what to install by the real Windows) and not a driver package. **Helpers:** a program VistaExt runs asks the service (`\\.\pipe\VistaExt`, `PREPARE`) before it starts another program (CreateProcess, ShellExecute); a setup's helper that it unpacked a moment before, or a just-installed program it starts, gets the same. **Removed programs:** their registration, backup and setting are removed too. It leaves alone: Windows' folders and program names, programs the user undid ("OFF"), game libraries and anti-cheat folders, links, and files that keep being put back. In users' folders it works as that user. It also carries out the on/off switch and the global version for the tray icon (administrators only). Log: `%ProgramData%\VistaExt\VistaExt.log`. |
| **Tray icon** `vxtray.exe` | The one visible part (one per signed-in user; started for every user by `HKLM\...\Run`, "Start with Windows" turns it off per user). Asks the service through its pipe; for a standard user, or without the service, it starts `vxauto.exe` with administrator rights (UAC prompt). |
| **On/off switch** | Off: Image File Execution Options of registered programs are removed (the list stays); patched program files are put back (a running one when it is closed); the Explorer entries and the .msi handler go; VistaExt's files move from System32/SysWOW64 into a subfolder `VistaExt.off`, where no program finds them (a file a running program has loaded can be moved too, so no restart). On reverses each step. Each step can be repeated, and the service finishes an interrupted switch when it starts. |
| **Driver safeguard** `advapivx` | A program told another Windows version may not create a driver that Windows loads while starting (boot or system start), or set one to start that way: a driver made for that other Windows could keep this one from starting. The call fails with "access denied", the user gets a message, and the program sees the real version next time. |
| **Setup** `VistaExtSetup.exe` | The wizard above: a stub with the whole release appended (`tools/mkpayload.py`), unpacked only after Accept. It installs or uninstalls, makes the restore point (`SRSetRestorePointW`), checks for Microsoft's C runtime and Vista's Platform Update, logs to `%ProgramData%\VistaExt\setup.log` and starts the tray icon as the signed-in user (through Explorer), not as administrator. Uninstalling removes everything it added: files, service, tray icon and its autostart, Start menu entries, Explorer entries, settings (also each signed-in user's own), logs; patched programs are put back. |
| **In-memory mode** `vxload.dll` (default) | Instead of rewriting a program's files, VistaExt registers the program in Image File Execution Options. Windows then loads `vxload.dll`, an Application Verifier provider that imports only ntdll, before the program's imports are resolved. vxload renames the import names in memory, for the program and every DLL it loads from outside the Windows folder: `kernel32.dll` → `kernelvx.dll`, ..., and `api-ms-win-X` → `vxs-ms-win-X`, VistaExt's own copy of the API set, which Windows' API-set table ignores. That copy is used only if it exports every imported function. Windows' own DLLs are never touched. Windows' standard verifier (`verifier.dll`) loads first and would hook about a hundred functions (heap, locks, waits) even with its checks off, which made heap calls up to 35× slower. vxload empties its hook lists before Windows applies them, so programs run at normal speed (see `KNOWN_RISKS.md` §8). A program's file is still changed in one case: when its header asks for a newer Windows (subsystem 10.0), because Windows refuses to start such a file before any DLL is loaded. `HKLM\SOFTWARE\VistaExt\Mode = file` (or `vxpatch /file`) uses the old way. In Safe Mode vxload does nothing at all. |
| **Crash window** `vxwatch.exe` | For a windowed program running with VistaExt, kernelvx starts `vxwatch.exe`, which waits for the program. If the program ends with a crash code, the window shows the error in plain words, where it happened, and the program's own last message (Chromium's `FATAL` lines). It also lists the Windows versions, so the program can be restarted told another one. The choice is saved for the user (HKCU), so it needs no administrator rights. A "don't show again" box turns it off per program. |
| **VistaExt Center** | Win32 GUI: pick a program or folder, see what it needs, choose the Windows version and options, Apply/Undo/Run. The work runs in the background, so big folders don't freeze the window, and "Run program" starts the program as the normal user (through Explorer), not as administrator. Adds Explorer right-click entries ("VistaExt compatibility...", "Run with VistaExt" for any .exe), makes double-clicking an `.msi` go through `msifix /install` (the previous command is saved and put back on uninstall), and a Start menu shortcut. |
| **msifix** | Copies an `.msi`, lowers `VersionNT`/`WindowsBuild` install checks to this PC, and on Vista re-marks schema 500 → 405 and removes the Windows Installer 5.0-only tables and action. Checks that are negated (`NOT (VersionNT >= 1000)`) are left alone. Properties a package fills with the build number itself (AppSearch on `Windows NT\CurrentVersion` → `CurrentBuild`, like Epic Games' `WINDOWSBUILDNUM`) are treated like `WindowsBuild`. When it lowers a check, it also removes an embedded chainer (extra installers run in the same transaction, made for newer Windows too, whose failure would undo the whole install; `/keepchainer` keeps it). It also patches custom-action DLLs stored inside the package. `/install` runs `msiexec` on the copy, which it keeps in `ProgramData\VistaExt\Packages` because Windows Installer needs it again for repairs. |
| **WebView2** | `vx-webview2-setup.cmd` copies and patches an extracted *Fixed Version* runtime and registers it. `kernelvx` then sets `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` and the browser arguments for patched programs. |

### Vista and Windows 7

Each Windows has its own build, chosen when building, not by checks at run time; one installer picks the right one ([docs/LAYOUT.md](docs/LAYOUT.md)). `install.cmd` checks for 6.0/6.1, never overwrites a Windows DLL (only its own, recognised by the marker), and records what it added. `uninstall.cmd` first puts every patched program back; if one is running it stops without removing anything, so that program keeps working. It then deletes only files that still carry the marker, so a file Windows Update has replaced since is kept.

**Nothing of VistaExt's is needed for Windows to start.** No Windows process loads it, the service is not one Windows depends on, and in Safe Mode it does nothing.

## Building (on Linux)

Needs `clang`, `lld`, `llvm-nm`, `llvm-windres`, `llvm-dlltool`, `python3` and Wine's headers and import libraries (`/usr/include/wine`, `/usr/lib/wine/*-windows`). No CRT is linked.

```bash
./build.sh
```

```bash
./package.sh
```

Output: four builds, `out/vista-x86`, `out/vista-x64`, `out/win7-x86` and `out/win7-x64`, and the release `out/dist/VistaExt.zip` plus `out/dist/VistaExtSetup.exe`, which carry all four. The installer picks the build for the Windows and architecture it runs on, and on 64-bit Windows also that Windows' x86 build for 32-bit programs. Everything that differs between Vista and 7 is chosen at build time, not checked at run time: [docs/LAYOUT.md](docs/LAYOUT.md) lists what is shared, what is per Windows and what is per architecture, and why.

`tools/gen.py` writes each DLL's `.def`, per build. It turns every compiled `vx_Name` function into the export `Name`. The name lists come from Wine's builds of the DLLs, One-Core-API's API-set specs (`third_party/oca-apisets`), and the real Vista and 7 DLLs' names and export numbers (`third_party/winexports`). Each build forwards only names its Windows has, and keeps that Windows' export numbers, so programs importing by number get the same function as without VistaExt.

## Tests

### Under Wine

```bash
tests/run_wine.sh
```

```bash
tests/run_msi.sh
```

`run_wine.sh` builds a fake "Vista" system folder (Wine's kernel32/user32 with every VistaExt function removed) and a test program that imports 14 newer functions. For both architectures it checks that:

- the unpatched program fails to load
- `vxpatch` patches it
- all checks pass with VistaExt's own code (`VX_NO_NATIVE=1`) and with the system-first path
- the Windows 10, 7 and folder-wide 8.1 settings are reported correctly
- the WebView2 runtime folder reaches the program
- `/undo` restores the original

`run_msi.sh` builds a package that requires Windows 8.1, 10 and Installer 5.0, then checks `msifix` as on Vista and as on 7.

Wine is not Vista: its fake "Vista" lacks every function VistaExt wraps, and Wine's `msi.dll` drops replaced streams, so the virtual-machine tests below are the real check.

### On real Windows Vista and 7 (virtual machines)

`tests/vm/vm.sh` installs Windows unattended from your own installation disc into a QEMU/KVM virtual machine, once per Windows version. Each test run then boots a throw-away copy with a test CD, and Windows runs the whole cycle by itself: `install.cmd`, the tests, `uninstall.cmd`, then it shuts down. The results are copied back and printed. The VMs have no network, run as a 30-day trial without a product key, and log on as Administrator with a blank password: they are for testing only.

```bash
tests/vm/vm.sh install vista64 <Vista SP2 x64 DVD>.iso "Windows Vista ULTIMATE"
```

```bash
tests/vm/vm.sh test vista64
```

The install takes about 10 minutes with KVM; a test run takes about 3. Use `x86` as a fourth argument for a 32-bit disc. The edition is the image name on the disc (`Windows 7 ULTIMATE`, `Windows Vista BUSINESS`, ...). Needs `qemu-system-x86_64` with KVM, `genisoimage`, `mtools`, `dosfstools` and ImageMagick; run `./build.sh`, `./package.sh` and `tests/run_wine.sh` first. Results are in `out/vm/<name>/results`, including a picture of VistaExt Center (`center.png`). While a VM runs, its screen is on VNC at `127.0.0.1` (the port is printed).

In each VM, `tests/vm/run.cmd` checks:

- `install.cmd` adds the DLLs (both System32 and SysWOW64 on 64-bit), the tools, the Start menu shortcut and the Explorer entries; on Windows 7 it leaves Windows' own `kernelbase.dll` alone; running it again over an existing install works and leaves a file Windows has replaced alone
- for 64- and 32-bit programs: Windows refuses the unpatched test program ("entry point not found"); after `vxpatch` it runs and passes every check, both with VistaExt's own code and with Windows' functions first
- the Windows 10, 7 and folder-wide 8.1 settings, as seen through `GetVersionEx`, `RtlGetVersion` (the PEB) and `VerifyVersionInfo`
- the WebView2 runtime folder reaches the program, and `/undo` restores the original
- the tray icon, its autostart and the defaults; the global version (Windows 11, a custom one, 8.1, a program's own setting, "this Windows' own") agreeing everywhere, including the registry (directly and through Windows' shlwapi) and WMI
- setups saved to Downloads: one that needs VistaExt, one that only checks the version (as Spotify's does), a driver package, one signed by Microsoft, one named like a Windows program; helpers started from Temp (CreateProcess, ShellExecute); a setup told another version that tries to install a boot driver
- the on/off switch: everything off and back on, a running patched program put back when it is closed, an interrupted switch finished by the service
- Safe Mode: VistaExt does nothing there, "Turn off VistaExt" works there, and it stays off after a normal start
- after the program "updates itself", undo keeps the new version, and patching it again renews the backup
- a program that imports through API-set names runs with VistaExt's API-set DLLs, and still runs after `vxpatch` when the API-set DLL is missing
- `msifix` with Windows Installer itself: the version checks, the 4.5 schema and 5.0 tables on Vista, the package's other properties, and a patched custom action that, read back out of the fixed package, runs
- VistaExt Center opens and shows what the program needs
- `uninstall.cmd` stops and removes nothing while a patched program is running; once it is closed, it restores the patched programs and removes every file, setting and Explorer entry it added

Results so far: Vista SP2 x64 199/199, Vista SP2 x86 163/163, Windows 7 SP1 x64 197/197, Windows 7 SP1 x86 161/161. That includes the in-memory mode, the crash window, the performance measurements, the global version through every API, the registry and WMI, setups in Downloads and their helpers, the on/off switch and Safe Mode (Windows restarts twice). The 64-bit VMs have Microsoft's C runtime update installed, as users have it.

### Real programs

```bash
tests/vm/vm.sh apps win7x64
```

Boots a VM with network access and a CD made from `out/apps` (programs you downloaded and unpacked yourself; see `tests/vm/apps.cmd` for the layout). It installs Microsoft's C runtime update (and on Vista KB971512) from `out/apps/dl/prereq/*.msu`, restarting once if needed, then installs VistaExt with `VistaExtSetup.exe /quiet`. Each program is started twice: patched with plain `vxpatch` from `C:\Apps`, and copied into Program Files with no step at all, so only the automatic mode acts. It takes a screenshot after a minute and records whether the program still runs; at the end it uninstalls with `VistaExtSetup.exe /uninstall /quiet` and checks that everything is gone. `VX_APPS="brave discord"` limits the run. Each start goes through `tests/vm/vxdbg.exe`, a small debugger that reports crashes (module + offset), exit codes and debug output, and `VX_TRACE=1` makes VistaExt report each replacement function a program calls and every function it looks up with `GetProcAddress`. `tools/appcheck.py <system folder> <program folder>` lists what a program would still lack, from a copy of a Vista/7 `System32` with VistaExt's DLLs added.

Microsoft Edge comes as Microsoft's offline MSI for businesses (`MicrosoftEdgeEnterpriseX64.msi` / `X86.msi` from Microsoft's Edge download page). `tools/unpack-omaha-msi.sh <msi> out/apps/x/edge-x64` (and `edge-x86`) unpacks it into the folder layout Edge's own setup installs, without running anything: the MSI holds Edge Update's installer, whose payload is LZMA with 7-Zip's BCJ2 filter (`tools/bcj2dec.c` undoes it), which holds Edge's setup, which holds the browser. `VX_APPS=setups tests/vm/vm.sh apps <vm>` installs VistaExt with its wizard on the VM's keyboard, then runs the real Spotify, Discord and Steam setups from Downloads (from `out/apps/dl`) and uninstalls them from Programs and Features; results in KNOWN_RISKS.md §11. `VX_APPS=edge tests/vm/vm.sh apps <vm>` then starts Edge on its own pages (settings, favorites, history: its WebUI 2.0 interface). In the test VMs only, Edge's policies switch off its first-run pages, sign-in, sync and default-browser question.

### Keeping test VMs ready

```bash
tests/vm/vm.sh prepare win7x64
```

Once per VM: installs Microsoft's updates from `out/apps/dl/prereq` and keeps the disk as `prepared.qcow2`. Every later run starts from it, so no run has to install the updates and restart first (`VX_VM_CLEAN=1` starts from the bare `base.qcow2`). The disk of the last run stays as `run.qcow2` until the next run (`VX_VM_KEEP=0` deletes it), for a look afterwards.

### Performance

`tests/vm/vxbench.c` runs as part of `vm.sh test` (or alone: `VX_VM_ONLY=bench tests/vm/vm.sh test <vm>`). It measures the same operations on plain Windows, in file mode and in in-memory mode: process start, heap, locks, threads, `LoadLibrary`, `GetProcAddress`, shared memory, and VistaExt's own `WaitOnAddress` (next to the earlier design, in the same run) and precise clock. `KNOWN_RISKS.md` §8 has the numbers.

### Video playback

```bash
tests/vm/vm.sh video win7x64
```

Brave and Firefox (from `out/apps`; `VX_VIDEO_APPS="brave firefox edge"` picks others), in in-memory and in file mode, each open `tests/vm/video.html` with nothing but its URL on the command line. The page comes from `tests/vm/videoserve.py` on the host. It plays a 4K YouTube video through YouTube's public embed API, laid out at 3840x2160 so that YouTube itself picks 2160p if the browser keeps up. It measures the quality played, video seconds per wall-clock second, stalls and the page's own frame rate, and sends the results back. Nothing in it is specific to one browser: any browser, in a VM or on a real PC, can open the same page. The VM records the CPU load, and any crash VistaExt's crash window reports. Results: `out/vm/<name>/results/video`.

The test CD also works on its own, for another VM program or a spare PC: `tests/vm/vm.sh testcd` writes `out/vm/test.iso`; insert it and run `\vxvm\run.cmd` as administrator. It installs and removes VistaExt, and puts the results in a folder on the desktop.

## Known limits

- **See [KNOWN_RISKS.md](KNOWN_RISKS.md)** for the full list: blind spots, integration conflicts, backward-compatibility and security risks, and what each test does and does not cover.
- **Coverage is what's listed.** About 290 functions are added. Big frameworks (recent Chromium/Electron, .NET 8+, Qt 6) may need more; `vxpatch /check` and VistaExt Center list what's still missing.
- **Imports by ordinal** (some `shell32` imports) are left unpatched and reported.
- **Extra start-up options** reach programs that read their command line through `GetCommandLine` in their own code, which includes Chromium, Electron and statically linked programs. Programs built with the shared C runtime (`ucrtbase.dll`, `msvcrt.dll`) get `argv` from the runtime, which reads the command line itself, so they don't see the extra options.
- **Some features are stubbed:** touch, pointer and gesture input; per-monitor DPI (Vista has one DPI); display configuration APIs; Windows Runtime classes (`RoGetActivationFactory` returns "class not registered"); `CreatePseudoConsole`; process mitigation policies. Programs are told the feature isn't available.
- **WebView2:** Microsoft's newest runtimes need Windows 10. VistaExt can point programs at a Fixed Version runtime, but whether runtime 109 starts on Vista has not been tested, and programs built with a newer WebView2 SDK may insist on a newer runtime. Supplying the runtime is up to you (it isn't redistributed here).
- **Custom actions inside MSI packages:** `msifix` patches the DLLs and programs stored in the package; this is verified with Windows Installer on Vista and 7. Script custom actions are not changed, and neither are the program files the package installs: run VistaExt Center on the installed program afterwards.
- **Signed programs** lose their signature validity after patching; programs that verify their own signature may refuse to run.

## License

GPL-2.0-or-later (see `LICENSE.txt`). The API-set name lists come from One-Core-API-Source (GPL-2.0).

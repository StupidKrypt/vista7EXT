# VistaExt: blind spots, conflicts and risks

This document says where VistaExt rests on assumptions that have not been proven, what it can break, and what has and has not been tested. Read it before you install VistaExt on a PC you depend on.

**In short:** VistaExt works by changing other programs' files and by telling programs they run on a newer Windows. Both are effective, but both are guesses about how a program will behave. Each real program tried so far has found at least one wrong guess. Treat every new program as untested until you have seen it run.

---

## 1. What has been verified, and how

| Claim | Evidence | Not covered |
|---|---|---|
| The DLLs, vxpatch, version settings, WebView2 folder, msifix and uninstall work on Vista SP2, 7 SP1 (x86 and x64) and 8 (x64) | Test suite in VMs (Beta 1.3): Vista x64 253/253, Vista x86 214/214, 7 x64 251/251, 7 x86 212/212, 8 x64 246/246; updating from Beta 1 / Beta 1.1 and uninstalling with a restart pass on all five (file mode, in-memory mode, crash window). Also Wine (file mode only: Wine has no Application Verifier). | Only freshly installed Windows without later updates. Windows 8 x86 and 8.1 not tested. Not tested on real hardware, with antivirus, or on non-English Windows. |
| `VistaExtSetup.exe /quiet`, its Programs and Features entry, the automatic-mode service and `/uninstall` | VM program run (see section 7) | The interactive (non-quiet) dialogs were not tested; neither was uninstalling from the Control Panel. |
| Automatic mode makes a program copied into Program Files work with no user step | VM program run (see section 7) | Real installers (MSI, Inno, NSIS, Squirrel), updates, several users, and logged-off users |
| Chromium/Electron programs (Brave, Discord) | VM program run (see section 7) | GPU drivers (the VM has none for Direct3D 11), sign-in, video, sandbox |
| `.msi` double-click → msifix | Unit test of msifix and the Epic Games package | The double-click flow itself (Explorer → msifix GUI mode) |

## 2. Assumptions that turned out wrong (so similar ones are likely)

These were found by running real programs. They show the *kind* of failure to expect from programs that have not been tried.

1. **"Telling a program it runs on Windows 10 is harmless."** It is not. The program then skips its own workarounds for older Windows. Chromium names its shared-memory sections only on Windows < 8.1, because older Windows ignores the access list of unnamed sections. Told "Windows 10", it skipped that and crashed in its own safety check (Brave, Discord). Fixed in `kernelvx` by naming such sections. **Every other "if older than Windows X" workaround inside a program is skipped the same way.** VistaExt cannot know them all.
   The same happened again one layer further. Told "Windows 10", Chromium asks `CreateProcess` for Windows 10-only attributes (job list `0x2000D`, component filter `0x2001A`). Windows 7 refused them, the GPU process never started, and Brave and Discord quit with "GPU process isn't usable". Fixed: `kernelvx` leaves out attributes this Windows lacks and reduces the mitigation mask to Windows 7's. **The child processes then run without those extra restrictions**, which is a security trade-off. After both fixes, Brave runs on Windows 7.
2. **"If the imports resolve, the program starts."** Not true. Brave checked kernel32's *file version* as well (fixed with `versvx`). Epic Games shipped its own copy of an API-set DLL that Windows 7 couldn't use. A DLL unloaded while VistaExt code was still in use (fixed by pinning).
3. **"An MSI's version checks are in `LaunchCondition`."** Epic Games reads the build number from the registry into its own property. msifix now handles that pattern, but other private patterns will exist.
4. **"Patching as SYSTEM is safe because only admins write to Program Files."** The automatic mode also watches every user's `AppData\Local`, which the user controls. A planted hard link could have made the SYSTEM service overwrite a file the user may not change, which is a privilege escalation. Fixed: the service now works there as that user, and vxpatch refuses hard-linked files and backups. The parser that reads those files still runs inside the SYSTEM process (see section 6).
5. **"It passed the test suite on Vista, so the tools work on Vista."** They had a latent crash. The tools are built without stack probes, so any function with a stack frame over 4 KB can skip Windows' stack guard page. Whether that crashes depends on how much stack an earlier call happened to touch. The suite passed. Then, on the same Vista VM:
   - `vxpatch` crashed silently;
   - the setup crashed (0xC0000005);
   - the automatic-mode service died at start.

   The service's watcher thread did the same on Windows 7, so automatic mode never saw a change. Fixed: the programs now commit 256 KB of stack up front. The build also refuses any DLL with a frame of 4 KB or more, since DLLs run on other programs' threads. **Passing tests did not show this; only a different sequence of calls did.**
6. **"If it builds and passes under Wine, it loads on Vista."** The build uses Wine's import libraries, which place some functions where Windows 8+ has them. The security fix made the tools bind `OpenThreadToken` to kernel32. Vista only has it in advapi32, so `vxpatch`, `msifix` and `vxauto` stopped loading on Vista, while the Wine tests and Windows 7 passed. Caught only by the Vista VMs. The build now checks every program's imports against the real Vista and 7 export lists (`tools/vistacheck.py`).
7. **"A program folder in Program Files is one program."** `Program Files (x86)\Steam` contains every installed Steam game. Fixed: the automatic mode now skips game libraries and folders with an anti-cheat system. *This change is in the tested build, but no game or anti-cheat folder was part of any test.*

## 3. Blind spots

- **"Any modern program" is not achievable by this design.** VistaExt implements about 290 functions (Windows 10 has tens of thousands). A program that needs a function it lacks still fails, whether at start-up (missing import) or later (a stub says "not supported" and the program doesn't cope). Frameworks with known large gaps:
  - Qt 6
  - .NET 8+
  - WinUI / Windows App SDK
  - anything UWP or WinRT
  - DirectX 12
  - Direct3D 11.1+ features
  - per-monitor DPI
  - modern touch input
- **Vista needs Microsoft updates first.** Without KB971512/KB971644 (Direct3D 11, DirectWrite, Direct2D) Chromium and Electron cannot start. Without KB2999226/KB3118401 (Universal C Runtime), many programs, including Firefox, can't start. VistaExt does not ship these; setup only warns.
- **No GPU in testing.** Chromium runs with `--disable-gpu` under the Chromium setting. Whether a real Direct3D 11 driver on Vista/7 works with current Chromium is unknown. Epic Games needs a D3D11 adapter and stopped there in the VM.
- **Network security is the operating system's.** Programs that use Windows' TLS (WinHTTP/WinINet/SChannel) need TLS 1.2. Vista has none. Windows 7 needs KB3140245 and a registry setting. Steam's bootstrapper failed to connect for this reason. Old root certificates cause the same symptom. Chromium and Firefox bring their own TLS, but still use Windows' certificate store (Chromium) or their own (Firefox).
- **Watched folders only.** The automatic mode watches:
  - `Program Files`
  - `Program Files (x86)`
  - each user's `AppData\Local` and `AppData\Roaming` (programs installed per user)
  - each user's Downloads and Desktop, for setup programs only (single files, not folders)

  It finds the profiles in Windows' profile list (so a profile outside `C:\Users` is found), and a signed-in user's real folders (a Downloads folder moved to another drive, for example). A user who signs in for the first time after the service started is watched from that sign-in. Programs elsewhere are not watched and need the right-click entry. Examples:
  - other drives
  - `C:\Games`
  - portable apps on the Desktop or in Downloads
  - `ProgramData`
- **Missed changes.** If Windows' change notification overflows (thousands of files at once), the service checks every program folder in that place that was created or changed in the last hour. An update deep inside an older folder that doesn't change the folder's own time can still be missed until the next change or `vxauto /scan`.
- **Patching takes time, and a program started before it finishes fails.** On Windows 7 Brave was done about 15 s after the copy. On Vista, with the disk busy, it took over 8 minutes (`chrome.dll` alone is 280 MB and was read whole, more than once). Files are now mapped instead of read, so only the parts VistaExt looks at come from the disk: checking a 200 MB program took 6–13 s instead of 18–46 s on the slow test PC (§8c), using 14–17 MB of memory instead of 220 MB. Brave itself has not been re-measured on Vista.
- **The first start after an update can fail.** An updater that starts the new version straight away (Discord's Squirrel, Chromium's updater) can start it before the 20-second quiet period ends. That first start fails; the next one works.
- **Programs in a logged-off user's AppData** are retried for about 10 minutes and then left until the next change.
- **Extra start-up options** (the `ARGS:` setting, the Chromium option) do not reach programs that use the shared C runtime's `argv`.
- **Only English Windows was tested.** Paths with non-ASCII characters and long paths (> 260 characters) are not handled specially.

## 4. Integration conflicts

| With | What can happen | What VistaExt does |
|---|---|---|
| **Code signatures** | In file mode, every patched file loses its valid Authenticode signature. In the default in-memory mode only files whose header asks for Windows 10 do. Signature checks fail in programs that check themselves, in AppLocker/SRP publisher rules, and in some updaters. | Nothing. Use Undo for such a program. |
| **Anti-cheat and DRM** | A changed game file can be treated as cheating (account bans) or tampering. | The automatic mode now skips game libraries and anti-cheat folders. Manual patching is still possible; don't do it to online games. |
| **Antivirus** | A service that rewrites other programs' executables behaves like a file infector. Antivirus may quarantine `vxauto.exe` or the patched files, or block the writes. | Untested. Add an exclusion if needed. |
| **Self-repairing programs / Windows Installer repair** | They put the original file back. The automatic mode patches it again. | After 3 times in an hour it stops and logs it. |
| **Windows Installer patches and cached packages** | msifix installs a *modified* package with the same package code. Later `.msp` patches, repairs that fall back to the original source, or a "different package with the same code" check can fail. | The fixed copy is kept in `ProgramData\VistaExt\Packages`, so repairs use it. Updates via `.msp` are untested. |
| **`.msi` file association** | VistaExt changes what double-clicking an `.msi` does, so every package goes through msifix (now a windowed program: no console window). A package msifix doesn't change installs as usual. | The previous command is saved and restored when VistaExt is switched off or uninstalled. If another program changes the association while VistaExt is installed, VistaExt puts *its* saved value back. |
| **DLLs Windows lacks** (`combase`, `shcore`, `d3d12`, Vista's `kernelbase` and `bcryptprimitives`, the `api-ms-win-*` API sets) | **Fixed in 1.3.** Up to 1.2 VistaExt put them into System32 under their Windows names, where **every** program found them, also those VistaExt does not run: a browser that runs on Vista by itself (Supermium) then loaded VistaExt's `bcryptprimitives.dll` in its sandboxed tab processes, whose `ProcessPrng` ends the process when `RtlGenRandom` fails there - every tab crashed. And any program that loaded `kernelvx` through one of them was told the global Windows version. Since 1.3 they have names of VistaExt's own (`combvx`, `shcrvx`, `d3dvx`, `bcryptprimvx`, `vxs-ms-*`/`vxe-ms-*`; Vista has no `kernelbase` of VistaExt's at all, `kernelvx` stands in for it). Programs VistaExt runs get them in place of the Windows name - imports (`vxload`, `vxpatch`) and `LoadLibrary`/`GetModuleHandle` (`kernelvx`), always asking Windows first; other programs find exactly what Windows has (VM test: a probe program's view before and after installing is compared). An update removes the earlier version's files. `kernelvx` also leaves a process VistaExt does not run unchanged even if it gets loaded there. **Windows' own DLLs inside a program VistaExt runs** take the newer Windows' paths when the program is told a newer version, and then load API sets this Windows lacks (Windows 7's shell, told Windows 10, loads `api-ms-win-core-url-l1-1-0` and refuses to start programs without it - found in the VMs after the rename). On Windows 7 and 8, `vxload` therefore gives each program it runs its own copy of Windows' API-set map (`PEB->ApiSetMap`) with an entry for each API set Windows lacks and VistaExt has, pointing at VistaExt's `vxs-ms-*` copy: Windows' entries are never overridden, no file is shadowed, and the programs it starts get Windows' map. (A `PATH` entry was tried first and dropped: children started by Windows' own code, such as a browser opened from a link, inherited it.) Vista has no API-set map, and its own DLLs use no API sets. Windows 7 and 8 RTM both use the map's version 2 (checked in their `apisetschema.dll`: 35 and 356 sets); 8.1's version 4 is left alone (no 8.1 copy to check it against). Non-API-set names (`combase`, `shcore`, `d3d12`) are not offered to Windows' own DLLs this way. | Verified in the VMs; see `probe.exe` in `tests/vm` (`/ldr`, `/child`). |
| **`d3d12.dll`** (Direct3D 12, which no Windows before 10 has; VistaExt's is `d3dvx.dll`, given only to programs VistaExt runs) | Programs that import it directly (Qt 6's rendering layer: OBS) can now start. Every function says "not supported" (`D3D12CreateDevice` returns `DXGI_ERROR_UNSUPPORTED`), as on a PC whose graphics driver has no Direct3D 12, and such programs use Direct3D 11. A program that only checks whether `d3d12.dll` loads, and then assumes Direct3D 12 works, would be misled. | A program's own `d3d12.dll` (Direct3D 12 on Windows 7 for some games) is found first and not affected. Its export numbers come from Wine's copy and agree with what Qt imports; not compared with a Windows 10 copy. |
| **Microsoft's C runtime update (KB2999226/KB3118401)** | It installs some `api-ms-win-core-*` DLLs with the same names as VistaExt's. | VistaExt never overwrites a file without its marker. If the update replaces VistaExt's copy, Windows' copy wins, and uninstall keeps it. Tested in the VM program run on 7 and Vista. |
| **The version setting (PEB)** | It changes what *Windows' own DLLs inside that program* see too (C runtime, .NET, comctl32, Direct3D...). They may take newer code paths. | Every program VistaExt runs is now told the global version (Windows 10 by default), not only Chromium programs; "this Windows' own version" in the tray menu, or per program in VistaExt Center, turns it off. Programs VistaExt does not run are never told anything. |
| **`GetProcAddress`** | In a patched program, looking up a newer function by name now finds VistaExt's version. A program that used to fall back cleanly when the function was missing now uses a stub. | By design (otherwise many programs fail). It is the main backward-compatibility risk, see below. |
| **Undo vs. the automatic mode** | Undo puts the originals back and saves "OFF" for that program, so the service leaves it alone. Applying in VistaExt Center replaces "OFF". | |

### Notifications delivered through a hidden window

Windows 8 offers power-setting, suspend/resume and device notifications as callbacks (`PowerSettingRegisterNotification`, `RegisterSuspendResumeNotification`, `CM_Register_Notification`). Vista and 7 deliver the same events only to windows. So the first callback registration of each kind gives the program **one hidden window on a thread of VistaExt's**, which receives what Windows sends and calls the program's callbacks on that thread (Windows 8 calls them on a thread of its own, too). Before November 2026 these registrations succeeded and never called back.

- Windows sends a power setting's current value right after it is registered, so the first callback comes at once, as on Windows 8.
- Unregistering runs on the window's thread: once it returns, no callback of that registration is running or still to come.
- Registering from inside a DLL's `DllMain` waits for that thread to start and can deadlock. Windows' own function does not have this limit.
- Device-instance notifications (`CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE`) have no counterpart on Vista or 7: that filter fails with `CR_CALL_NOT_IMPLEMENTED`. Device interfaces and device handles work.
- Windows 7's `powrprof.dll` has `PowerSettingRegisterNotification` (undocumented there) but refuses the callback form. Windows' function is asked first; VistaExt's window takes only what Windows refused.
- Suspend and resume reach a top-level window directly (Windows broadcasts them). A message-only or child window registered for them gets them passed on by VistaExt.

### Power requests on Vista

`PowerSetRequest` and `PowerClearRequest` are counted per request and type, and one helper thread holds the combined state for the process (Vista's `SetThreadExecutionState` belongs to the calling thread and is a single state). A request whose handle is closed without being cleared stays in force until that handle value is used again for a new request, or the process ends; Windows 7 ends it at once.

### Windows 8: process protections for child processes

Measured in the VMs with `tests/vm/mitigtest` (no VistaExt involved): Windows 7 refuses any mitigation-policy mask that is not 4 bytes in `UpdateProcThreadAttribute` itself (error 24) and knows DEP, ATL thunks and SEHOP. **Windows 8 (build 9200) accepts any 8-byte mask there and checks it only in `CreateProcess`**, which fails with `ERROR_INVALID_PARAMETER` for a policy it does not know: it accepts bits 0-2 and the 2-bit fields at bits 8-32, and refuses every field from bit 36 on (8.1's dynamic-code and CFG policies, Windows 10's) - the same for 64- and 32-bit processes. Up to Beta 1.3 VistaExt reduced the mask only when `UpdateProcThreadAttribute` failed, which on Windows 8 it never does, so a program told Windows 10 that asks for Windows 10's protections could not start any child. Now the Windows 8 build passes Windows a copy reduced to the measured fields (lenient case only: programs run without sandbox, as before). 8.1 accepts every Windows 8 field too, but its own additions are then left out (not measured: no 8.1 VM).

### Windows 8: not verified

- **32-bit Windows 8** (not WOW64): its ntdll system-call stubs have a form (inline `sysenter`) that `vxload`'s stub check does not know, so the hook that rewrites a DLL's imports as soon as it is mapped is not installed there (traced as "unknown code, left alone"). DLLs a program loads at start then miss VistaExt on 32-bit Windows 8, as on Windows 8 before 1.1. No 32-bit Windows 8 to test on.
- **Windows 8.1**: its API-set map (version 4), its mitigation fields, and its ntdll stubs are not checked against real files.

### Diagnostics

- **Where:** `%ProgramData%\VistaExt Diagnostics`, with a protected access list (SYSTEM and Administrators write, Users read). The service writes there as SYSTEM, so the folder must not be one users can write to: a planted hard link or junction would make the service write elsewhere. A folder of that name that exists already is taken over (owner Administrators, protected list); a junction of that name is removed; a log file there that is a link or has several names is not written to. If the folder cannot be secured (a user's own access list denies administrators), nothing is written there.
- **What:** the logs name programs and paths; the snapshot adds Windows' error log and crash dumps, which can contain what a program had in memory. Nothing is sent anywhere: the user copies the folder.
- **Crash logs of programs VistaExt runs** are written by the crash window as that user, into the user's own `%LOCALAPPDATA%\VistaExt`; the snapshot (as administrator) copies every user's.
- **Event log watching** needs the Windows Event Log service; if it cannot subscribe, the service says so in its log and works on without it.

## 5. Backward-compatibility risks (things that worked before VistaExt)

1. **Programs that run by themselves are left alone (since 1.3).** Up to 1.2 the service set up a whole program folder as soon as *any* file in it lacked something - also a DLL loaded on demand (delay-loaded imports, which programs check first) or an optional part made for newer Windows - and gave it the recommended settings (Chromium: no sandbox, no GPU, told Windows 10). A program that ran fine without VistaExt could break. Now a program is set up only if it **cannot start on this Windows by itself** (`vxp_runs_natively`: its header, and every function it and the DLLs it loads at start from its own folder import, checked against Windows' own DLLs and API sets, not VistaExt's files). Programs next to it that run by themselves stay as they are. An update looks again at what earlier versions set up (`vxauto /review`) and puts back every program that runs by itself, except those the user chose (right-click, the VistaExt window, `vxpatch`). Each decision is written to `decisions.log` in the diagnostics folder. A program that starts by itself but loads a DLL needing VistaExt later (with `LoadLibrary`) is not detected: use "Open with VistaExt compatibility mode" for it.
2. **The version DLL is switched in every file of a program with a version setting.** `versvx` changes the file version of kernel32, kernelbase and ntdll only. It does nothing for other files, but it is still a different DLL in that program.
3. **System-wide new DLLs**: none since 1.3 (see the table above).
4. **Uninstalling** puts every recorded file back. Two exceptions:
   - A file a program updated since it was patched is kept as the new version.
   - Programs installed while VistaExt was present stop working, as they need it.

   Patched files that were never recorded (for example in a copy of the folder made after patching) stay patched, along with their `.vxorig` backups.
5. **Upgrading from earlier VistaExt builds** was tested only with the same build installed twice. The saved-settings format gained the `OFF` value. Older builds don't know it and treat such a program as having no setting.

## 6. Security

- **The automatic mode runs as LocalSystem.** It reads and parses executables from places users can write. In users' own folders, files are now read and written as that user. Program Files folders with weak permissions (some installers leave them writable by everyone) are still handled as SYSTEM. Hard-linked files are refused there. A link swapped in at exactly the right moment (a race) is not fully excluded.
- **The PE parser was not fuzzed.** A malformed executable that crashes or corrupts the parser does so inside a SYSTEM process. To remove that risk, do the parsing in a separate process with the user's rights.
- **Named shared memory:** the shared-memory fix gives sections random 128-bit names with an empty access list. Other processes can't open them, and a name already taken is never reused.

## 7. Last real-program run (VMs, with network)

Windows 7 SP1 x64 and Vista SP2 x64. Fresh installs plus Microsoft's C runtime update (KB3118401), and on Vista also KB971512 (Direct3D 11/DirectWrite). VistaExt was installed with `VistaExtSetup.exe /quiet`.

"Auto" means: the program was copied into Program Files and the test did nothing else. The automatic-mode service had to notice it, choose settings and patch it. "Plain" means `vxpatch` without settings, from a folder the service doesn't watch.

| Program | Windows 7 | Vista | Notes |
|---|---|---|---|
| **Brave** (Chromium) | auto: **runs**, new-tab page shown. Plain: stops at the Windows-version check. | auto: **runs**, new-tab page shown. Plain: same as 7. | Runs with `--no-sandbox --disable-gpu`; no sandbox means less security. Browsing, sign-in, video and extensions were not tested. |
| **Discord** (Electron) | auto: Electron starts (5 processes) | auto: Electron starts (5 processes) | The test copies only the app folder. Discord downloads its core module through its own updater on first install, which the test doesn't do, so the app shows "Cannot find module 'discord_desktop_core'". **The full app is unverified.** |
| **Firefox** | window opens, **no page loads** | window opens, **no page loads** | Its sandboxed child processes can't start (§8b). The earlier "runs" only checked the process. |
| **Epic Games Launcher** | installs from the msifix package; the service patches 99 files; the launcher doesn't stay running | installs; patched; crashes at start | Earlier diagnosis: no Direct3D 11 adapter in the VM (no GPU). Not re-diagnosed with the final build. |
| **Steam** | bootstrapper runs (plain and auto) | runs | Earlier run: can't connect (TLS 1.2 / root certificates). Not a VistaExt problem, but it is what users will see. |
| **Setup / uninstall** | `/quiet` install, Programs and Features entry, service, `/uninstall /quiet` removes everything | same | The interactive dialogs were not tested. |

Firefox, Epic and Steam were last run on the build *before* the final two changes (process attributes, service disk priority). Brave and Discord were run on the final build. The basic suites were rerun on the final build (see the README for the totals).

## 8. In-memory mode (now the default) and the crash window

**What it fixes.**
- Program files are no longer rewritten. The exception is a header that asks for Windows 10, see below.
- Signatures stay valid in most cases, and updaters and repairs put back files that need nothing.
- Undo is a registry change. Programs on read-only media can run (setups on a CD).

**Verified in the VMs (Vista and 7, x86 and x64):**
- A program and its DLLs, both those loaded at start and those loaded later with LoadLibrary, have their imports redirected with the files unchanged.
- A program of the same name elsewhere is left alone.
- API-set imports go to VistaExt's copy, and `/undo` removes everything.
- The crash window appears, shows the program's fatal message, and "Restart with this setting" restarts the program told Windows 10.

**Real programs in memory mode (Windows 7 x64 and Vista x64, final build):**
- Brave runs (7–8 processes, no crashes), and Discord's Electron shell runs.
- Firefox's window opens, but it loads no page (its child processes can't start; found by the video test, see §8b).
- Steam's bootstrapper runs. Epic still stops for lack of a GPU, as it did in file mode.
- All of these were made to work by the automatic mode alone, without rewriting the files' imports.
- Uninstalling first stops any waiting crash windows. Otherwise their open file kept the VistaExt folder from being removed, which the suite caught.

**Found by those runs:**
- **A wrong conclusion of mine, corrected:** Firefox's content processes crashed and its window stayed blank. I first blamed the verifier and made Gecko programs fall back to file mode. Testing both modes showed the real cause: my fix for Brave's child processes, which reported Windows 10-only process attributes as applied when they weren't. Firefox's sandbox relies on them, and its children stopped when they found them missing. That handling now applies only to processes running with `--no-sandbox` (what VistaExt gives Chromium/Electron programs). The fallback was removed. (Later found with the video test: Firefox then opens its window in both modes but still loads no page, see §8b.) **Other programs that sandbox their children without that switch get Windows 7's own behaviour, which means no Windows 10-only hardening.**
- **Anything else that writes `GlobalFlag` for the same program name switches VistaExt off.** That includes gflags, debugging tools and other compatibility tools. The test harness did exactly this (to get loader diagnostics) and silently turned memory mode off for every program it started.

**Its own risks, not verified:**
- **Application Verifier is switched on for the program, and it was expensive. Measured, then fixed.** `GlobalFlag 0x100` makes Windows load its own `verifier.dll` before `vxload.dll`. `VerifierFlags = 0x80000000` (the value VxKex uses) turns its *checks* off, but **not its hooks**: `verifier.dll` still routed about a hundred functions through itself (heap, critical sections, waits, TLS, files, registry). Measured with `tests/vm/vxbench.c` on Windows 7 x64:

  | | plain Windows | in-memory, before | in-memory, now |
  |---|---|---|---|
  | HeapAlloc + HeapFree | 22 ns | 777 ns | 23 ns |
  | critical section enter + leave | 3 ns | 18 ns | 3 ns |
  | process start + exit | 2.4 ms | 6.7 ms | 4.2 ms |

  Vista x64 and both 32-bit systems showed the same: heap operations 5–35× slower before, the same as plain Windows now. **The fix:** when `vxload.dll` starts, it finds the provider descriptor `verifier.dll` handed to Windows and empties its hook lists, before Windows applies them. It only does this when the descriptor is found exactly once and looks as expected, and only when `VerifierFlags` is VistaExt's own 0x80000000, so a real verifier session someone set up is left alone. If it can't recognise the structures, it changes nothing: the program then runs at the old, slow speed but still works. If `verifier.dll` added the page-heap flag on its own, that flag is removed too. `VX_KEEP_VERIFIER=1` turns the fix off for comparison. **Risk:** this depends on the layout of `verifier.dll`'s descriptor, which is undocumented. It was checked on Vista SP2 and 7 SP1 (x86 and x64). A Windows update to `verifier.dll` could make the check fail, which is safe, just slow again. Programs that look for a debugger or verifier (some games, DRM, anti-cheat) may still react to `verifier.dll` being loaded.
- **Image File Execution Options go by file name.** Registering `setup.exe` or `launcher.exe` switches on the verifier for *every* program of that name. vxload then does nothing in the others, but `verifier.dll` is still loaded into them. Another tool's IFEO values for the same name are kept and put back on undo.
- **The header is still patched for programs that say "Windows 10".** Windows refuses such a file at process creation, before any DLL can help, so these files still lose their signature. Many current programs are like this (Discord.exe, Brave's helpers).
- **Only processes of a registered program are covered.** A program's DLL loaded into another process (a shell extension in Explorer, a plug-in of another program) is not redirected there. File mode handled that.
- **A program started through an 8.3 short path** (`C:\PROGRA~1\...`) is not recognised. Registrations are kept by long path, and vxload can't convert paths before kernel32 exists.
- **When the redirect happens:** the in-memory rewrite relies on Windows announcing each DLL before resolving its imports. That holds on the tested Vista SP2 and 7 SP1 builds. A later update to ntdll could change it.

**Crash window:**
- It appears only for windowed programs, and only for a program's main process. A process started by the same program (the helpers of any multi-process program: browser renderers, Electron helpers, Firefox content processes, ...) is skipped; this used to be decided by Chromium's and Firefox's own command-line switches and is now one generic rule. A program that restarts itself (the new instance is started by the old one) gets no crash window after that restart.
- The summary shows the program's own last words if it formatted them on the stack before stopping: any ANSI or UTF-16 text near the stack containing "fatal", "check failed", "assert", "panic", "unhandled", "abort" or "terminate" (Chromium, C/C++ asserts, Rust, .NET-style messages). Programs that write their reason only to a log file show just the error code and module.
- It turns off Windows Error Reporting's "has stopped working" box for those programs, so no crash report goes to Microsoft.
- It can't help when Windows refuses to start the program at all (a missing DLL or function), because the program never runs. Windows shows its own message then.
- The version picked there is saved per user (HKCU) and takes priority over the setting made in VistaExt Center for all users. Uninstalling VistaExt can't remove other users' HKCU entries; they're harmless leftovers.
- `VX_WATCH_TEST_CHOICE` is a test hook: when set, the window answers itself without showing.

## 8b. Performance and stability pass (measured with `tests/vm/vxbench.c` and real 4K video)

**Rewritten, with the reason:**
- **`verifier.dll`'s hooks are switched off in in-memory mode** (see §8): heap calls had been up to 35× slower than plain Windows; now equal.
- **`WaitOnAddress` / `WakeByAddress*`.** Rust's standard library builds its locks, condition variables and thread parking on these, and Firefox has a lot of Rust. The old version had one condition variable per hash bucket, so every wake woke *all* waiters of the bucket, and `WakeByAddressSingle` woke more than one. Now each waiter sleeps on its own record: a wake wakes exactly the ones asked for, and a wake with nobody waiting takes no lock at all. Tested: one of two sleepers wakes on `WakeByAddressSingle`, the other on `WakeByAddressAll` (Wine and all four VMs). Measured against the old design in the same run (`vxbench`, 2 virtual CPUs):
  - ping-pong between two threads: faster on Vista x86 in every run (76–338 ns against 174–828 ns), mixed on Windows 7 x86;
  - a Rust-style mutex: the same within a few ns;
  - one address with 8 sleepers, each release waking one: the new design is often *slower* there, about 17 µs per release on Vista x86, against 1.5–156 µs for the old one, varying from run to run. That is the cost of waking exactly one thread per release, which is what Windows 8 does. The old design woke all eight, which happened to drain a queue faster in this test. Programs written for Windows 8 expect the one-thread behaviour.
- **`GetSystemTimePreciseAsFileTime`** used to return the normal clock, which moves in 15.6 ms steps. Where the performance counter is cheap, it now interpolates between ticks, giving about 11 µs resolution, and stays within a tick of the system clock. Where the counter is slow (HPET or ACPI timers, and the test VMs at about 6 µs per read), it keeps the old cheap behaviour, because programs call this in tight loops. It decides once per process.
- **`GetProcAddress`** asked the loader (which takes its lock) for every module beyond the 16th, and looked every name up in two DLLs. Now the module's name comes from its own export table (no lock), and only names VistaExt implements itself get a second lookup.
- **`vxload`** opened and mapped VistaExt's copy of an API set from disk for *every* DLL that imports it. Now each copy is mapped once per process and searched by binary search.
- **The crash recorder** ran `GetModuleHandleEx` and `GetModuleFileName` (the loader lock) on every first-chance exception, including those programs handle themselves on purpose (Java, .NET, WebAssembly). That could deadlock if the exception came while another thread held the loader lock. It now uses `VirtualQuery` and the mapped file name, no lock and little stack, one thread at a time.
- **`GetDpiForWindow` and friends** created and released a screen DC on every call (Chromium asks on every layout). The system DPI is now read once per process; it can't change during a logon on Vista/7.
- **Shared-memory names** (`CreateFileMapping` with a DACL) called the system's random generator, a device call, every time. Now it is called once per process, with a counter.

**Bugs found by the new tests:**
- **`vxpatch /auto <folder>` saved garbage options** (`ARGS:::::...`) for Chromium/Electron folders. A pointer into a local variable outlived it. So in file mode Brave got no `--no-sandbox` and stopped with "GPU process isn't usable". Fixed, and `run.cmd` now checks the saved setting. The automatic mode was not affected (separate code).
- The crash window used Chromium's and Firefox's own switches to skip helper processes. It now uses one generic rule (a process started by the same program is a helper), and its summary picks up common fatal messages of any runtime (ANSI or UTF-16). See the crash-window notes in §8.
- Every crash the window reports is now also written to `%LOCALAPPDATA%\VistaExt\crashes.log`.
- **VistaExt's API-set DLLs in System32 don't help where Microsoft's C runtime update put its own.** KB3118401 (which VistaExt needs) installs its own `api-ms-win-core-synch-l1-2-0.dll`, among others. It forwards `WaitOnAddress`, `WakeByAddress*` and the synchronization barrier to `kernel32`, which Vista/7 doesn't have. VistaExt never replaces a Windows file, so a program that imports these through that API-set name and hasn't been made compatible crashes at start (an access violation in the VMs). Every earlier suite run started from a VM *without* that update, so the tests never saw this. Programs handled by the automatic mode or `vxpatch` are not affected: their import goes to VistaExt's copy in memory, or to `kernelvx` on disk, and the suite checks both. The suite now records whether Windows' own copy was there first and expects that behaviour. Rust programs (Rust ≥ 1.78 imports `WaitOnAddress` this way) need the automatic mode or `vxpatch` on Vista/7.

**4K video (YouTube "Big Buck Bunny 60fps 4K", 180 s, browser-independent test page, 8 virtual CPUs, no GPU):**

| | Windows 7 x64 | Vista x64 |
|---|---|---|
| Brave, in-memory mode | 2160p, speed 1.00, 0 stalls, page 60 fps, CPU ~20% | 2160p, speed 1.00, 0 stalls, page 60 fps, CPU ~23% |
| Brave, file mode | 2160p, speed 1.00, 0 stalls, page 60 fps, CPU ~23% | 2160p, speed 1.00, 0 stalls, page 60 fps, CPU ~21% |
| Firefox (both modes) | **window opens, no page loads** | **window opens, no page loads** |

"Speed" is video seconds per wall-clock second. YouTube chose 2160p itself (the player is laid out at 3840×2160) after starting at 1080p, and never stepped down. No crashes. The page can't see dropped frames inside YouTube's player (it's another site); stalls, speed and the page's own frame rate are what any page can measure.

**Firefox does not work under VistaExt yet; this corrects my earlier "Firefox runs".** Its window opens, but it never loads a page. Firefox runs every page, and its GPU, network and utility work, in sandboxed child processes, and none of them start. Its own error output (now captured by the test) says why. Told it runs on Windows 10, Firefox asks for Windows 10-only protections for its children, and Windows rejects the child process: `ERROR_INVALID_PARAMETER` on Vista, `ERROR_NOT_SUPPORTED` on Windows 7. Earlier tests only checked that a `firefox.exe` process was running, which it is.
- With `VX_LENIENT_CHILDREN=1` (VistaExt leaves out the protections this Windows lacks, as it does for `--no-sandbox` programs), Windows 7 starts the children (3–8 processes), but they then end abnormally ("IPC close with reason=AbnormalShutdown"). Firefox's crash reporter doesn't work there, so no reason was recorded. On Vista the children still fail to start, at a later step of the same launch.
- On Windows 7, Firefox in file mode also crashed twice at the same place, `ole32.dll+0xa936`. That is Windows' COM code starting an outgoing call on a thread without a COM context. Firefox imports `CoIncrementMTAUsage` and `RoGetAgileReference` from VistaExt's `combase.dll`, so VistaExt's versions are suspects, but this isn't confirmed.
- Not tried yet: telling Firefox the real Windows version (it may then choose Windows 7-compatible protections, if its code still has them), and finding why the children end on Windows 7.

**Not covered:** real GPUs (the VMs have none, so decoding is in software); dropped-frame counts inside the player; 32-bit browsers; other video sites.

## 8c. Slow-PC pass (measured with `tests/vm/perf.cmd`)

The test PC: one processor, 1 GB of memory, a disk limited to 20 MB/s and 100 operations per second (`tests/vm/vm.sh perf <vm>`). Each figure is from the same VM image before and after; two rounds each, on all four builds. Run-to-run noise was about ±20% for anything involving the disk or starting programs.

| | before | after |
|---|---|---|
| Service, private memory | 10.3–12.3 MB | 3.5–4.6 MB |
| Service, threads | 13–17 | 5–8 |
| Service, wake-ups in 10 idle minutes | about 600 (once a second) | 0–14 |
| Tray icon, wake-ups in 10 idle minutes | about 200 (every 3 s) | 0 |
| Checking a 200 MB program (`vxpatch /check`) | 18–46 s, 220 MB in memory | 6–13 s, 14–17 MB |
| Looking at a 200 MB unsigned setup (`vxauto /prepare`) | 16–36 s | 3–17 s (one read of the file for Windows' catalog check) |
| `vxauto /scan` of Program Files | 130–260 ms | 6–71 ms |
| A program VistaExt runs, start to end | 4.0–5.5 ms (plain Windows: 2.2–3.1 ms) | unchanged |
| Install (`install.cmd`) | 38–76 s | unchanged (within noise) |

**What changed:**
- The service waits for something to happen (a file change, a sign-in, a setting) instead of looking every second; one thread watches up to 63 folders, with 64 KB buffers instead of 512 KB.
- The tray icon waits for VistaExt's settings to change instead of reading them every 3 seconds, and makes changes in a second thread (its window keeps answering).
- Program files are mapped, not read: only the parts looked at come from the disk.
- A program's own signature is read from its signature block (a few KB); Windows' `CryptQueryObject` on the file read all of it.
- The API-set DLLs carry only the imports they use (5 KB instead of 6 KB each); the setup is 14.3 MB instead of 16.0 MB.

**Kept, with the reason:**
- **The crash window is one small process per windowed program VistaExt runs** (0.7–1.4 MB private each). One shared watcher would save that, but it would be one more thing that must be running, and a program's watcher must outlive the program.
- **`CreateFileMapping` with an access list costs about 15 µs** instead of under 1 µs: VistaExt names such sections so that Vista/7 apply the list (see the comment in `kernelvx/misc.c`). There is no cheaper way on Vista/7.
- **The service starts with Windows, not delayed.** A delayed start was tried: it broke the switch and setup handling for the first two minutes after signing in, while the service's own start costs 10–70 ms.
- **A 200 MB unsigned setup is still read once** for the check against Windows' catalogs (Microsoft's own files can be signed only there).

**Conflicts found with the runtimes and updates of `deps.cmd` installed** (both older than this pass; found by its test runs):
- **.NET's optimizer got crash windows.** After .NET 4.8 is installed, Windows runs `mscorsvw.exe` (it compiles .NET code in the background). It got `kernelvx.dll` loaded through VistaExt's API-set DLLs in the system folder, and `kernelvx` started a crash watcher for it, as for any windowed program. Fixed: the watcher is started only for programs VistaExt runs (their own imports name VistaExt's DLLs, or `vxload` is in the process).
- **Windows Search's filter host keeps VistaExt's files in use** (see "New system DLLs" in §4): uninstalling then needs a restart, and says so.
- **A delayed service start broke the first minutes after signing in** (see above): reverted before release.

## 10. Tray icon, on/off switch, setup programs

**How safe it is for Windows itself.** Nothing of VistaExt's takes part in starting Windows:
- No Windows process loads its DLLs. The in-memory loader is registered only by program name, and never under a name Windows has a program of (checked in the Windows folder, System32, SysWOW64, wbem, oobe, sysprep, and Windows' programs in Program Files).
- The service is not one Windows depends on.
- In Safe Mode the loader does nothing.
- If VistaExt's files are missing or broken, only programs registered for it fail to start.

So a crash or a damaged install of VistaExt cannot keep Windows from starting. Two things can still harm Windows: what *programs* do because they are told another version (below), and VistaExt's files in System32 that unpatched programs can see (section 4).

**Setup programs that run by themselves are watched.** Many setups run on Vista/7 but unpack and start a helper that does not (Discord's setup runs on Vista with KB2533623; the `Update.exe` it unpacks into AppData and starts at once needs Windows 7's functions). Beta 1.3 left such a setup completely alone, so its helper started without VistaExt ("The procedure entry point TryAcquireSRWLockExclusive could not be located"). Now a recognised setup (or a new helper of one) that runs by itself is registered **only to watch** (`Memory` value data `watch`): `vxload` rewrites nothing in it, tells it no version and loads no `kernelvx`; it only hooks `NtCreateUserProcess` (every process start ends there, also those made by Windows' own DLLs) and asks the service about each program right before it starts. A helper that cannot run by itself is set up, one that can is watched the same way. A setup VistaExt never saw (run from a USB stick, or downloaded before VistaExt was installed) is not watched: right-click it, "Open with VistaExt compatibility mode". On 32-bit Windows 8 the hook cannot be installed (see "Windows 8: not verified").

**"Runs by itself" is judged from the program file at start (same family as the Discord error).** What the service can see before a program starts is its header, its import tables and the DLLs those load at start. Three ways a program can still need VistaExt later, and what VistaExt does about each:
- *It starts another program that needs VistaExt* (a setup's helper): setups are watched (above). Other programs' new `.exe` files in Program Files and AppData are seen by the folder watch when written; one started within a moment of being written can start before the service has looked at it.
- *Its main code is a DLL it loads by name* (`firefox.exe` → `xul.dll`, `brave.exe`/`chrome.exe` → `<version>\chrome.dll`, `msedge.exe` → `msedge.dll`): that DLL is judged too. Evidence it is the main code: its name is written in the program file (not in an import table), it is in the program's folder or one folder below, it is the largest such DLL and larger than the program file. Optional DLLs Chromium programs name (`dxcompiler.dll`, `vulkan-1.dll`, `d3dcompiler_47.dll`) are smaller and never count, so a native browser (Supermium) is not changed for them. 1.3's review may have put such programs back; the 1.3.1 review (rules 3) clears `AutoScanned`, so every program folder is looked at again once.
- *It calls a function Windows lacks only later* (a delay-loaded import it does not check, a DLL it loads on demand): cannot be seen beforehand. When it fails, Windows logs it (Application Error 0xC0000135/0xC0000139/0xC06D007E/0xC06D007F, or an "Application Popup" naming a DLL): the service writes it to `decisions.log` and the tray icon shows a tip to use "Open with VistaExt compatibility mode". It does not register the program on its own: the event does not say whether VistaExt has what is missing. On Vista the Application Error event has no full path, so the tip names the program only.
- Elevated helpers started with ShellExecute "runas" are created by Windows' AppInfo service, not by the setup, so a watched setup cannot ask about them first (a program that asks for elevation in its manifest is asked about: Windows tries CreateProcess first).

**1.2's setup registrations.** 1.2 told every recognised setup another version by default. The 1.3.1 review turns those that run by themselves into watch-only registrations while "setup programs are told it too" is off (1.3's review removed them instead).

**Setup programs told another version.** Off by default since 1.3 (a setup that runs by itself must not change because VistaExt is on). When turned on in the tray menu ("Setup programs are told it too"), a setup program you download is told the chosen version even if it would run without VistaExt (that is what makes Spotify's setup install on Windows 7). Such a setup may then install files made for that newer Windows. VistaExt limits this:
- **Excluded:** setups signed by Microsoft (C runtimes, .NET, updates) and driver packages see the real version. A driver package is recognised by "driver" in its name or description, so one that doesn't say so is not recognised.
- **Boot drivers refused:** a program told another version may not create or reconfigure a driver Windows loads while starting. This applies only to calls through the program's own `CreateService`/`ChangeServiceConfig`. Not covered:
  - a helper such as `sc.exe` (it sees the real version, since it is Windows' own);
  - writing the service's registry keys directly;
  - `SetupAPI` from inside the setup's process.
- **Running it again:** a setup that was refused a boot driver gets the real version the next time.

With the rule off (the default), only setups that cannot run at all without VistaExt are changed.

**Recognising setups is a guess.** It goes by name (`setup`, `install`, `update`, `upgrade`, `bootstrap`), version information, or an installer engine in the manifest (NSIS, Inno Setup, InstallShield, Squirrel, WiX). A setup with none of these runs as it is and may refuse this Windows. A program that isn't a setup but has such a name (an "updater" tool) is told the version too.

**Where setups are noticed.** In Downloads and Desktop of each user, in their Temp folder, and in Internet Explorer's download cache, 2 s after the file was written:
- **Too quick:** a setup double-clicked within those 2 s fails the first time.
- **Other places:** setups on a USB stick, a network share, another drive or a DVD are not noticed. Copy them to Downloads first, or use the right-click entry "Open with VistaExt compatibility mode".
- **Helpers of setups:** handled before they start, when the setup starts them with `CreateProcess` or `ShellExecute` through its own imports. Not when Windows' own DLLs start them for it (for example through COM or a scheduled task).
- **Helpers of a told setup:** they are told the version too, if their file was written in the last hour (so it is part of the install). An older program the setup starts, such as a browser to show a web page, is not.

**One version everywhere, with gaps.** The registry values are answered in the in-memory mode only (the default). Not covered:
- enumerating the values (`RegEnumValue`);
- reading them in another process;
- `SystemInfo`-style tools.

WMI is answered for synchronous queries from the program, and for those Windows' own DLLs make for it in the in-memory mode. Not for asynchronous queries, or for objects passed to another thread's apartment. In file mode, `RtlGetNtVersionNumbers`, the registry and WMI agree only when the program's own imports were switched for another reason. `KUSER_SHARED_DATA` (read directly by a few anti-cheat and copy-protection systems) always shows the real version.

**The switch.**
- **Off is not instant for running programs:** they keep what they loaded until they are closed, but programs they start from then on start without VistaExt (a browser's new tab process, for example, fails). A changed program that is running is put back as soon as it is closed; until then it still needs VistaExt's DLLs, which are already moved aside, so if it is restarted before the service has put it back it fails like on Windows without VistaExt.
- **Something keeps a file open:** a system file that can't be moved (antivirus holding it) is moved at the next restart, and the tray icon says so.
- **Programs registered while off:** the list is kept, and they are switched on with the rest.

**Who may switch.** The service accepts changes (on/off, the version, the setup rule) from members of Administrators, also while UAC runs their programs without administrator rights, so the tray icon needs no UAC prompt for them. Any program running as an administrator can therefore switch VistaExt off or change the version without a prompt. A standard user gets a UAC prompt (the tray icon starts `vxauto.exe` with administrator rights).

**UAC prompts were not tested.** The test VMs log on as the built-in Administrator, which Windows never prompts. VistaExt's registration works the same whether a setup is started elevated or not (Windows reads it when the program starts), but the prompt itself was not seen in a test.

**In-memory redirection inside programs.** In programs VistaExt runs (and only there, in memory; files on disk are not changed), `vxload` changes two Windows functions:
- **`NtQueryValueKey` in ntdll:** the first 5 bytes of its code become a jump to VistaExt's function, which runs a copy of the original. Windows' DLLs keep their own pointers to ntdll's functions, so redirecting only import and export entries reached nothing (tested). The code is changed only if it is exactly one of the known Vista/7 system-call stubs (x64, x86, 32-bit on 64-bit); otherwise it is left alone and only the registry values stay real.
- **`CoCreateInstance(Ex)` and `CoSetProxyBlanket` in ole32:** through its export table and the import tables of DLLs already loaded.

Windows' verifier was meant to apply such hooks for VistaExt, but it applied none on Vista or 7. A program that checks its own ntdll code for changes (some anti-cheat and copy-protection systems) will see the jump; such folders are left alone by the automatic mode.

## 11. Real setup programs (VMs, with network; this build)

VistaExt installed with its own wizard, driven on the VM's keyboard. On all four systems, Accept, the restore point ("Before installing VistaExt" was created), Install and Finish worked, and Finish started the tray icon. Then SpotifySetup.exe, DiscordSetup.exe and SteamSetup.exe were saved to Downloads and started the way a double-click does.

| Setup | What VistaExt did | Result | What is missing |
|---|---|---|---|
| **Steam** (NSIS) | registered it as a setup program (told Windows 10) | **Installs** on all four systems, into Program Files with its Start menu shortcut and Programs and Features entry; **uninstalls** from Programs and Features | Steam then says "Steam needs to be online to update": its update download uses Windows' own HTTPS, which these VMs can't do with Steam's servers (no TLS 1.2 update or current root certificates). Not VistaExt's part. |
| **Spotify** (64-bit only) | registered it as a setup program; on Vista it also needs functions Vista lacks | Its own "requires Windows 10" check **passes**; then "Automatic download failed (error code 57)" while downloading the app | The same HTTPS problem as Steam: Spotify's setup only downloads the app. Not tested past this point. |
| **Discord** (Squirrel) | registered it (it imports a function Vista/7 lack) | Stops at "Install .NET 4.5" | Discord's installer needs Microsoft .NET Framework 4.5 or later, which Vista and 7 don't include. Not tested past this point. |

So none of the three was seen *running* on Vista/7 from its own setup in this test. What stopped Spotify and Discord, and Steam's first update, is missing system prerequisites (TLS 1.2 for Windows' HTTPS, current root certificates, .NET 4.5+), not functions VistaExt adds. Discord's program itself (copied from its package) starts with VistaExt (section 7).

**Uninstalling while those setups were still open** left VistaExt's DLLs in System32: a running program had them loaded. Fixed after this run: the uninstaller now has Windows delete such files at the next restart and says so (Windows' own mechanism for files in use).

## 9. Further design ideas

- Patch the "needs Windows 10" header without changing the file, through a launcher that creates the process from a corrected image. That works, but it is the technique malware uses, and antivirus software reacts to it.
- Per-path IFEO filtering (`UseFilter`, Windows 7 and later) would stop the verifier being switched on for other programs of the same name. It wasn't used because Vista doesn't support it.

VistaExt - run programs made for newer Windows on Windows Vista and 7
=====================================================================

Needs: Windows Vista SP2 or Windows 7 SP1, 32- or 64-bit.
Install first, from Microsoft (Windows Update or catalog.update.microsoft.com):
  - Universal C Runtime: KB3118401 (or KB2999226)  - Vista and 7
  - Platform Update: KB971644 / KB971512           - Vista only; without
    it browsers, Discord, Epic Games and other Chromium apps cannot start
Windows 7 also needs KB3140245 for TLS 1.2 (programs that use Windows'
own internet functions, such as Steam's installer).

Before you start: read KNOWN_RISKS.md. VistaExt changes program files;
try it on a test PC or VM first.

INSTALL
  Run VistaExtSetup.exe and answer Yes. That's all.
  It adds files to Windows (never replaces a Windows file), adds VistaExt
  to Programs and Features, and starts the automatic mode.
  Silent install: VistaExtSetup.exe /quiet
  Without the automatic mode: VistaExtSetup.exe /noauto

  The Start menu gets one folder, "VistaExt", with two entries:
    VistaExt            turn VistaExt on or off for the whole PC (also in
                        Safe Mode), and settings for one program; it also
                        starts VistaExt's icon next to the clock
    Uninstall VistaExt  removes it (see UNINSTALL)

UPDATE
  Run the new VistaExtSetup.exe. It sees the installed version and offers
  "Update VistaExt" (or Uninstall). Updating keeps your settings, the
  Windows version programs are told, whether VistaExt is on or off, and
  the programs it made compatible. A file a running program uses is
  swapped for the new one too (the program keeps the old copy until it is
  closed; the old copy is deleted when Windows restarts), so old and new
  files are never mixed. Restart Windows after an update if programs were
  running.

USING PROGRAMS
  Automatic mode: install programs as usual. A few seconds after an
  installer or updater finishes, VistaExt makes the new program work
  (it watches Program Files and every user's AppData\Local). Nothing to
  click. What it did is in %ProgramData%\VistaExt\auto.log.
  Games and programs with an anti-cheat system are NOT changed
  automatically (changed game files can get an account banned).

  Any program, or its shortcut (desktop, Start menu): right-click it and
  choose "Open with VistaExt compatibility mode". VistaExt sets it up
  (recommended settings, or the ones you saved) and starts it, as you,
  like a double-click. It asks for administrator rights once (the shield),
  and after that the program also starts with a plain double-click.

  Installer packages (.msi): just double-click them. VistaExt checks the
  package and, if it refuses this Windows, installs a fixed copy (kept in
  ProgramData\VistaExt\Packages - Windows needs it for repairs).

  VistaExt does not change programs' files any more (only in rare cases
  a few bytes of the header): it redirects what they need from Windows while
  they start. Installed programs keep their digital signatures.

  If a program VistaExt runs crashes, a window explains what happened and
  lets you restart it told another Windows version (Windows 10 is often
  the one that works for new programs, but some need an older one).
  Each crash is also written to %LOCALAPPDATA%\VistaExt\crashes.log.

  If a program still does not start: right-click it and choose "VistaExt
  compatibility..." (or right-click its folder). The VistaExt window shows what
  is missing and lets you pick the Windows version the program is told it
  runs on, and the Chromium/Electron options. Apply, then "Run program".
  "Undo all changes" puts the original files back, and the automatic mode
  then leaves that program alone.

  To turn the automatic mode off: set HKLM\SOFTWARE\VistaExt\Auto
  (DWORD) to 0, or remove VistaExt and install it with /noauto.

WEBVIEW2 PROGRAMS
  Programs built on Microsoft Edge WebView2 need its runtime. Get the
  "Fixed Version" WebView2 runtime from Microsoft (version 109 is the last
  one made for Windows 7), extract it, then in an administrator command
  prompt run:
     "%ProgramFiles%\VistaExt\vx-webview2-setup.cmd" "<extracted folder>" x64
  (or x86). Then tick "Uses Microsoft WebView2" for the program.

COMMAND LINE
  vxpatch.exe /check <program or folder>     report only
  vxpatch.exe <program or folder>            make compatible
  vxpatch.exe /auto <program or folder>      ... with the recommended settings
  vxpatch.exe /winver:WIN10 <program>        set the reported Windows version
  vxpatch.exe /undo <program or folder>      put the originals back
  vxpatch.exe /file <program or folder>      change the files instead of
                                             redirecting in memory
  msifix.exe /install <package.msi>          fix and install a package
  vxauto.exe /scan                           make all installed programs work now

PROGRAMS THAT ALREADY WORK
  VistaExt changes only programs that cannot start on this Windows without
  it. A program that runs by itself (for example a browser built for
  Vista) is left exactly as it is, and VistaExt puts no DLL with a Windows
  name into the system folder. To give a program VistaExt anyway,
  right-click it: "Open with VistaExt compatibility mode".

DIAGNOSTICS (when something goes wrong)
  VistaExt writes errors as they happen into
     %ProgramData%\VistaExt Diagnostics
  (type that into Explorer's address bar; "Save diagnostics" opens it)
  events.log      every program crash or "could not start" Windows reports
  decisions.log   which programs VistaExt set up, and which it left alone
  Crashes of programs VistaExt runs go to each user's
  %LOCALAPPDATA%\VistaExt (crashes.log, crash-details.log).
  After testing, click "Save diagnostics" in the VistaExt window (Start
  menu) or the tray icon's menu: it adds a full snapshot (report-<date>)
  with every user's crash logs, and opens the folder. Then copy the whole
  folder and send it. Crash dumps in it can contain what a program had
  open (web pages): look through it first.

UNINSTALL
  Start menu -> VistaExt -> Uninstall VistaExt, or Programs and Features
  -> VistaExt -> Uninstall (or VistaExtSetup.exe /uninstall; /quiet for no
  questions, /restorepoint to make the restore point then too).
  It lists the programs that use VistaExt and warns you first: programs
  you installed are NOT removed, but those made for newer Windows will no
  longer start without VistaExt. Then it:
    1. creates a System Restore point "Before removing VistaExt",
    2. turns VistaExt's compatibility layer off,
    3. removes every file VistaExt installed (DLLs, tools, service, tray
       icon, Start menu folder, Explorer entries, settings, logs) and puts
       back every program file it changed.
  Close the programs VistaExt changed first. If one is still running,
  VistaExt stays off but is not removed, and you are told to close it.

GOOD TO KNOW
  * VistaExt adds about 290 functions from Windows 7, 8 and 10. Programs
    that need more than that still will not start; the VistaExt window lists
    what is missing.
  * Some features cannot exist on Vista/7 (touch input, Windows Store /
    UWP apps, DirectX 12). Programs asking for them are told they are not
    available, which most programs handle.
  * On Windows 7, VistaExt uses Windows' own version of a function
    whenever Windows has one.
  * A program whose header says "needs Windows 10" still gets that header
    changed on disk (Windows refuses to start it otherwise), which breaks
    its digital signature. Everything else happens in memory.

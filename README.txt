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

USING PROGRAMS
  Automatic mode: install programs as usual. A few seconds after an
  installer or updater finishes, VistaExt makes the new program work
  (it watches Program Files and every user's AppData\Local). Nothing to
  click. What it did is in %ProgramData%\VistaExt\auto.log.
  Games and programs with an anti-cheat system are NOT changed
  automatically (changed game files can get an account banned).

  Setup programs: right-click a setup .exe and choose "Run with VistaExt"
  if it refuses to start.

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
  compatibility..." (or right-click its folder). VistaExt Center shows what
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

UNINSTALL
  Programs and Features -> VistaExt -> Uninstall
  (or VistaExtSetup.exe /uninstall, /quiet for no questions).
  Close the programs VistaExt changed first. It puts every one of them back
  the way it was; if one is still running, it removes nothing (so that
  program keeps working) and tells you to close it.

GOOD TO KNOW
  * VistaExt adds about 290 functions from Windows 7, 8 and 10. Programs
    that need more than that still will not start; VistaExt Center lists
    what is missing.
  * Some features cannot exist on Vista/7 (touch input, Windows Store /
    UWP apps, DirectX 12). Programs asking for them are told they are not
    available, which most programs handle.
  * On Windows 7, VistaExt uses Windows' own version of a function
    whenever Windows has one.
  * A program whose header says "needs Windows 10" still gets that header
    changed on disk (Windows refuses to start it otherwise), which breaks
    its digital signature. Everything else happens in memory.

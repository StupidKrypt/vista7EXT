@echo off
rem VistaExt with real programs (tests/vm/vm.sh apps). Runs at logon from the
rem apps CD (%1 = its drive). Does what a user does:
rem   - installs Microsoft's C runtime update (\prereq\*.msu on the CD)
rem   - installs VistaExt with VistaExtSetup.exe /quiet
rem   - runs each program twice, asking the host for a screenshot after a
rem     minute ("SHOT <name>" on COM1) and recording whether it still runs:
rem       plain - copied to C:\Apps (not watched), patched with plain vxpatch
rem       auto  - copied into Program Files and NOT touched by this script:
rem               the automatic mode (vxauto service) must make it work
rem   - uninstalls VistaExt with VistaExtSetup.exe /uninstall /quiet
rem CD layout: \apps\brave, \apps\firefox (Firefox's "core" folder),
rem \apps\edge-x64 and \apps\edge-x86 (see :edge),
rem \apps\discord (Update.exe + app\), \apps\steam, \apps\epic\EpicInstaller.msi
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "R="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
if not defined R (
    (echo VXAPPS no results disk)>COM1
    shutdown /s /t 5 /f
    exit /b 1
)
set "LOG=%R%\results.txt"
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" /f >nul 2>&1
set "T=%SystemDrive%\vxt"
set "VX=%ProgramFiles%\VistaExt"
set "APPS=%SystemDrive%\Apps"
set "ALOG=%ProgramData%\VistaExt\VistaExt.log"
rem VistaExt reports what the programs ask it for (seen by vxdbg)
set VX_TRACE=1
rem after the restart that Microsoft's updates may need, the run continues
set "BOOTMARK=%SystemDrive%\vxt-restarted.marker"
if not exist "%BOOTMARK%" (> "%LOG%" echo VistaExt with real programs) else (call :say "--- after the restart for the updates ---")
set "K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
for /f "tokens=2*" %%a in ('reg query "%K%" /v ProductName 2^>nul ^| find "REG_"') do set "PN=%%b"
for /f "tokens=2*" %%a in ('reg query "%K%" /v CSDVersion 2^>nul ^| find "REG_"') do set "CSD=%%b"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
call :say "Windows: !PN! !CSD!, %OSA%"
call :say "Started: %DATE% %TIME%"

if exist "%T%" rd /s /q "%T%"
xcopy "%CDR%\vxvm" "%T%\" /e /i /q /y >nul
attrib -r "%T%\*" /s /d >nul
set "B=%T%\bin\%OSA%"

call :say "### network"
nslookup discord.com > "%R%\network.txt" 2>&1
find /c "Address" "%R%\network.txt" >> "%LOG%"

call :say "### Microsoft updates from the CD"
set "OSV=6.0"
ver | find "6.1." >nul && set "OSV=6.1"
set "NEEDBOOT="
for %%m in ("%CDR%\prereq\windows!OSV!-*-%OSA%_*.msu") do (
    start "" /wait wusa "%%~fm" /quiet /norestart
    set "RC=!errorlevel!"
    call :say "%%~nxm: wusa exit !RC! - 0 installed, 3010 installed/restart, 2359302 already there"
    if "!RC!"=="3010" set "NEEDBOOT=1"
)
if defined NEEDBOOT if not exist "%BOOTMARK%" (
    > "%BOOTMARK%" echo restarted
    call :say "restarting Windows to finish the updates"
    shutdown /r /t 5 /f
    exit /b 0
)
if exist "%SystemRoot%\System32\d3d11.dll" (call :say "Direct3D 11 d3d11.dll: present") else (call :say "Direct3D 11 d3d11.dll: MISSING")
if exist "%SystemRoot%\System32\ucrtbase.dll" (call :say "C runtime ucrtbase.dll: present") else (call :say "C runtime ucrtbase.dll: MISSING")

set "WANT= "
for /f "usebackq delims=" %%a in ("%CDR%\vxvm\apps.txt") do set "WANT=!WANT!%%a "
if not "!WANT: setups =!"=="!WANT!" (
    call :wizard
) else (
    call :say "### install VistaExt: VistaExtSetup.exe /quiet"
    start "" /wait "%T%\VistaExtSetup.exe" /quiet
    call :say "setup exit !errorlevel!"
)
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\install.txt" >nul 2>&1
find "file(s) added" "%R%\install.txt" >> "%LOG%" 2>nul
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt" /v UninstallString >nul 2>&1 && (call :say "Programs and Features entry: yes") || (call :say "Programs and Features entry: MISSING")
sc query VistaExtAuto | find "RUNNING" >nul && (call :say "automatic mode service: running") || (call :say "automatic mode service: NOT running")
if exist "%VX%\vxpatch.exe" (call :say "VistaExt files: installed") else (call :say "VistaExt files: MISSING")

rem ------------------------------------------------------------- programs
rem (only those listed in \vxvm\apps.txt)
call :say "Programs: !WANT!"
if not "!WANT: setups =!"=="!WANT!" call :setups
if not "!WANT: brave =!"=="!WANT!" if exist "%CDR%\apps\brave\brave.exe" call :app brave "brave.exe" "--no-first-run"
if not "!WANT: discord =!"=="!WANT!" if exist "%CDR%\apps\discord\app\Discord.exe" call :app discord "app\Discord.exe" ""
if not "!WANT: firefox =!"=="!WANT!" if exist "%CDR%\apps\firefox\firefox.exe" call :app firefox "firefox.exe" ""
if not "!WANT: epic =!"=="!WANT!" if exist "%CDR%\apps\epic\EpicInstaller.msi" call :epic
if not "!WANT: steam =!"=="!WANT!" if exist "%CDR%\apps\steam\Steam.exe" call :app steam "Steam.exe" ""
if not "!WANT: edge =!"=="!WANT!" if exist "%CDR%\apps\edge-%OSA%\msedge.exe" call :edge

rem ---------------------------------------------------------------- the end
wevtutil qe Application /c:80 /rd:true /f:text > "%R%\events.txt" 2>&1
copy /y "%ALOG%" "%R%\auto.log" >nul 2>&1

call :say "### uninstall: VistaExtSetup.exe /uninstall /quiet"
copy /y "%VX%\VistaExtSetup.exe" "%T%\uninst-check.exe" >nul 2>&1 || call :say "VistaExtSetup.exe was not copied into %VX%"
start "" /wait "%VX%\VistaExtSetup.exe" /uninstall /quiet
call :say "uninstall exit !errorlevel!"
rem the uninstaller runs from a copy in %TEMP%; give it time to finish
for /l %%w in (1,1,30) do (
    if not exist "%VX%\vxpatch.exe" goto :uninst_done
    ping -n 3 127.0.0.1 >nul
)
:uninst_done
ping -n 4 127.0.0.1 >nul
if exist "%VX%\vxpatch.exe" (call :say "after uninstall: files LEFT") else (call :say "after uninstall: files removed")
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt" >nul 2>&1 && (call :say "after uninstall: Programs and Features entry LEFT") || (call :say "after uninstall: Programs and Features entry removed")
sc query VistaExtAuto >nul 2>&1 && (call :say "after uninstall: service LEFT") || (call :say "after uninstall: service removed")
if exist "%SystemRoot%\System32\kernelvx.dll" (call :say "after uninstall: kernelvx.dll LEFT") else (call :say "after uninstall: system DLLs removed")
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\uninstall.txt" >nul 2>&1
call :say "Finished: %DATE% %TIME%"
call :say "RESULT: programs tried, see above"
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

rem =========================================================================
rem VistaExt's own setup wizard, on the VM's keyboard as a user would click:
rem Accept the warning, keep "create a restore point", Install, Finish (which
rem starts the tray icon). A picture of each page.
:wizard
call :say "### install VistaExt with its setup wizard (keyboard, as a user would)"
start "" "%T%\VistaExtSetup.exe"
ping -n 9 127.0.0.1 >nul
2>nul >COM1 echo SHOT wizard-1-warning
ping -n 4 127.0.0.1 >nul
2>nul >COM1 echo KEYS alt-a
ping -n 4 127.0.0.1 >nul
2>nul >COM1 echo SHOT wizard-2-backup
ping -n 4 127.0.0.1 >nul
2>nul >COM1 echo KEYS alt-i
ping -n 6 127.0.0.1 >nul
2>nul >COM1 echo SHOT wizard-3-restorepoint
for /l %%w in (1,1,120) do (
    reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt" /v UninstallString >nul 2>&1 && goto :wiz_installed
    ping -n 6 127.0.0.1 >nul
)
call :say "wizard: NOT installed within 12 minutes"
:wiz_installed
ping -n 16 127.0.0.1 >nul
2>nul >COM1 echo SHOT wizard-4-finish
ping -n 4 127.0.0.1 >nul
2>nul >COM1 echo KEYS alt-f
ping -n 12 127.0.0.1 >nul
tasklist /fi "imagename eq vxtray.exe" 2>nul | find /i "vxtray.exe" >nul && (call :say "wizard: Finish started the tray icon") || (call :say "wizard: tray icon NOT running after Finish")
tasklist /fi "imagename eq VistaExtSetup.exe" 2>nul | find /i "VistaExtSetup.exe" >nul && (call :say "wizard: setup still open") || (call :say "wizard: setup closed")
2>nul >COM1 echo SHOT wizard-5-tray
wmic /namespace:\\root\default path SystemRestore get Description > "%T%\rp.txt" 2>&1
type "%T%\rp.txt" >> "%LOG%"
find /i "Before installing VistaExt" "%T%\rp.txt" >nul && (call :say "wizard: restore point 'Before installing VistaExt' exists") || (call :say "wizard: NO restore point found")
goto :eof

rem Real setup programs, as a user gets them: saved to Downloads (as a browser
rem does), then double-clicked (Explorer starts them). Nothing else is done
rem for them. Each runs, installs where it installs, and usually starts the
rem program; then it is uninstalled the way Programs and Features does.
:setups
set "DL=%USERPROFILE%\Downloads"
if not exist "%DL%" mkdir "%DL%"
call :say "### setup programs saved to Downloads"
for %%s in ("%CDR%\setups\*.exe") do copy /y "%%~fs" "%DL%\" >nul
ping -n 16 127.0.0.1 >nul
for %%s in ("%DL%\*.exe") do (
    reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%%~fs" > "%T%\m.txt" 2>&1
    find /i "setup" "%T%\m.txt" >nul && (call :say "%%~nxs: registered by VistaExt as a setup program") || (
        find /i "REG_SZ" "%T%\m.txt" >nul && (call :say "%%~nxs: registered by VistaExt") || (call :say "%%~nxs: left alone by VistaExt"))
)
if exist "%DL%\SpotifySetup.exe" if "%OSA%"=="x64" call :setup_run spotify SpotifySetup.exe "%APPDATA%\Spotify\Spotify.exe" 0
if exist "%DL%\DiscordSetup.exe" call :setup_run discord DiscordSetup.exe "%LOCALAPPDATA%\Discord\Update.exe" 0
set "STEAMDIR=%ProgramFiles%\Steam"
if "%OSA%"=="x64" set "STEAMDIR=%ProgramFiles(x86)%\Steam"
if exist "%DL%\SteamSetup.exe" call :setup_run steam SteamSetup.exe "!STEAMDIR!\Steam.exe" 4
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\VistaExt-setups.log" >nul 2>&1
call :uninstall_app spotify Spotify
call :uninstall_app discord Discord
call :uninstall_app steam Steam
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\VistaExt.log" >nul 2>&1
goto :eof

rem setup_run <tag> <setup in Downloads> <the installed program> <Enter presses>
:setup_run
set "TAG=%~1"
set "SET=%~2"
set "INST=%~3"
call :say "### !TAG!: double-click !SET! in Downloads"
explorer.exe "%DL%\!SET!"
ping -n 31 127.0.0.1 >nul
2>nul >COM1 echo SHOT !TAG!-setup-1
rem a setup with pages (Steam's): Next, Next, Install, Finish
for /l %%k in (1,1,%~4) do (
    2>nul >COM1 echo KEYS ret
    ping -n 21 127.0.0.1 >nul
    2>nul >COM1 echo SHOT !TAG!-setup-page%%k
)
rem installing and (for most) downloading the program: up to 8 minutes
for /l %%w in (1,1,48) do (
    if exist "!INST!" goto :setup_installed
    ping -n 11 127.0.0.1 >nul
)
call :say "!TAG!: NOT installed after 8 minutes (!INST! missing)"
2>nul >COM1 echo SHOT !TAG!-setup-end
goto :setup_report
:setup_installed
call :say "!TAG!: installed (!INST!)"
rem the setup (or updater) usually starts the program: give it time to update and open
ping -n 121 127.0.0.1 >nul
2>nul >COM1 echo SHOT !TAG!-running
:setup_report
for /f "tokens=2*" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\!TAG!" /v DisplayName 2^>nul ^| find "REG_"') do call :say "!TAG!: Programs and Features entry (user): %%b"
for /f "tokens=2*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\!TAG!" /v DisplayName 2^>nul ^| find "REG_"') do call :say "!TAG!: Programs and Features entry: %%b"
for /f "tokens=2*" %%a in ('reg query "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall\!TAG!" /v DisplayName 2^>nul ^| find "REG_"') do call :say "!TAG!: Programs and Features entry: %%b"
tasklist /v > "%T%\tasks.txt" 2>&1
findstr /i /c:"!TAG!" "%T%\tasks.txt" > "%T%\t2.txt"
for /f %%c in ('find /c /v "" ^< "%T%\t2.txt"') do call :say "!TAG!: %%c process(es) running"
type "%T%\t2.txt" >> "%LOG%"
findstr /i /c:"!TAG!" "%ProgramData%\VistaExt\VistaExt.log" >> "%LOG%" 2>nul
dir /s /b "%ProgramData%\Microsoft\Windows\Start Menu\Programs\*!TAG!*.lnk" "%APPDATA%\Microsoft\Windows\Start Menu\Programs\*!TAG!*.lnk" >> "%LOG%" 2>nul
goto :eof

rem uninstall_app <tag> <name in Programs and Features>: its UninstallString,
rem as Programs and Features runs it (Enter for its questions), then: gone?
:uninstall_app
set "TAG=%~1"
set "US="
for %%k in ("HKCU\Software" "HKLM\SOFTWARE" "HKLM\SOFTWARE\Wow6432Node") do (
    for /f "tokens=2*" %%a in ('reg query "%%~k\Microsoft\Windows\CurrentVersion\Uninstall\%~2" /v UninstallString 2^>nul ^| find "REG_"') do set "US=%%b"
)
if not defined US (call :say "!TAG!: no uninstall entry" & goto :eof)
call :say "### !TAG!: uninstall from Programs and Features: !US!"
taskkill /f /t /im "!TAG!.exe" >nul 2>&1
ping -n 4 127.0.0.1 >nul
start "" cmd /c "!US!"
for /l %%k in (1,1,4) do (
    ping -n 16 127.0.0.1 >nul
    2>nul >COM1 echo SHOT !TAG!-uninstall-%%k
    2>nul >COM1 echo KEYS ret
)
ping -n 61 127.0.0.1 >nul
set "LEFT="
for %%k in ("HKCU\Software" "HKLM\SOFTWARE" "HKLM\SOFTWARE\Wow6432Node") do reg query "%%~k\Microsoft\Windows\CurrentVersion\Uninstall\%~2" >nul 2>&1 && set "LEFT=1"
if defined LEFT (call :say "!TAG!: Programs and Features entry still there") else (call :say "!TAG!: Programs and Features entry removed")
for %%d in ("%APPDATA%\!TAG!" "%LOCALAPPDATA%\!TAG!" "%ProgramFiles(x86)%\!TAG!" "%ProgramFiles%\!TAG!") do if exist "%%~d" (
    dir /s /b /a-d "%%~d" 2>nul | find /c /v "" > "%T%\n.txt"
    for /f %%n in (%T%\n.txt) do call :say "!TAG!: %%~d still has %%n file(s)"
)
reg query "HKLM\SOFTWARE\VistaExt\Memory" 2>nul | find /i "!TAG!" > "%T%\m.txt"
for /f %%c in ('find /c /v "" ^< "%T%\m.txt"') do call :say "!TAG!: VistaExt registrations left right after: %%c"
goto :eof

:epic
rem installed into Program Files: the automatic mode handles it from there
call :say "### epic: msifix on the installer package"
"%VX%\msifix.exe" "%CDR%\apps\epic\EpicInstaller.msi" "%T%\EpicInstaller.fixed.msi" > "%R%\epic-msifix.txt" 2>&1
type "%R%\epic-msifix.txt" >> "%LOG%"
call :say "### epic: original package, for comparison"
start "" /wait msiexec /i "%CDR%\apps\epic\EpicInstaller.msi" /qn /l*v "%T%\epic-orig.log"
call :say "msiexec original: exit !errorlevel!"
call :say "### epic: fixed package"
start "" /wait msiexec /i "%T%\EpicInstaller.fixed.msi" /qn /l*v "%T%\epic-fixed.log"
call :say "msiexec fixed: exit !errorlevel!"
copy /y "%T%\epic-fixed.log" "%R%\epic-msiexec.log" >nul
set "EG="
for %%d in ("%ProgramFiles(x86)%\Epic Games" "%ProgramFiles%\Epic Games") do if exist "%%~d\Launcher\Portal\Binaries\Win64\EpicGamesLauncher.exe" set "EG=%%~d"
if not defined EG (
    call :say "epic: not installed"
    goto :eof
)
call :say "epic: installed in !EG!"
call :wait_auto "!EG!"
call :launch "epic-auto" "!EG!\Launcher\Portal\Binaries\Win64\EpicGamesLauncher.exe" ""
goto :eof

rem Microsoft Edge (\apps\edge-x64 or -x86, unpacked from Microsoft's offline
rem MSI by tools/unpack-omaha-msi.sh). Its own pages - settings, favorites,
rem history, downloads - are its WebUI 2.0 interface (see :pages). Edge's policies (only in these
rem test VMs, with the owner's agreement) skip its first-run pages, sign-in,
rem sync and default-browser question.
:edge
set "EP=HKLM\SOFTWARE\Policies\Microsoft\Edge"
reg add "%EP%" /v HideFirstRunExperience /t REG_DWORD /d 1 /f >nul
reg add "%EP%" /v DefaultBrowserSettingEnabled /t REG_DWORD /d 0 /f >nul
reg add "%EP%" /v BrowserSignin /t REG_DWORD /d 0 /f >nul
reg add "%EP%" /v SyncDisabled /t REG_DWORD /d 1 /f >nul
call :say "Edge policies: no first-run experience, sign-in, sync or default-browser question"
call :app edge-%OSA% "msedge.exe" "--no-first-run"
call :pages "edge-%OSA%" "%ProgramFiles%\VXT-edge-%OSA%\msedge.exe" "settings favorites history downloads" "edge://"
goto :eof

rem pages <tag> <browser> "<page> ..." <prefix>: the browser's own pages, which it
rem does not open from its command line: typed into its address bar on the
rem VM's keyboard (KEYS/TYPE on COM1, done by vm.sh), each with a screenshot
:pages
set "TAG=%~1"
set "PROG=%~2"
set "IMG=%~nx2"
call :say "### !TAG!: its own pages: %~3"
start "" /b "%B%\vxdbg.exe" 360 "!PROG!" --no-first-run > "%R%\!TAG!-pages-run.txt" 2>&1
ping -n 46 127.0.0.1 >nul
for %%p in (%~3) do (
    2>nul >COM1 echo KEYS ctrl-l
    ping -n 3 127.0.0.1 >nul
    2>nul >COM1 echo TYPE %~4%%p
    ping -n 4 127.0.0.1 >nul
    2>nul >COM1 echo KEYS ret
    ping -n 16 127.0.0.1 >nul
    2>nul >COM1 echo SHOT !TAG!-%%p
    ping -n 6 127.0.0.1 >nul
)
tasklist /fi "imagename eq !IMG!" 2>nul | find /i "!IMG!" > "%T%\running.txt"
for /f %%c in ('find /c /v "" ^< "%T%\running.txt"') do set "NP=%%c"
if "!NP!"=="0" (call :say "!TAG! pages: NOT running at the end") else (call :say "!TAG! pages: running at the end, !NP! process(es)")
taskkill /f /t /im "!IMG!" >nul 2>&1
ping -n 4 127.0.0.1 >nul
findstr /c:"exception 0x" /c:"exited 0x" /c:"[vxdbg:" "%R%\!TAG!-pages-run.txt" > "%T%\summary.txt" 2>nul
for /f "usebackq delims=" %%l in ("%T%\summary.txt") do 2>nul >COM1 echo   %%l
goto :eof

rem app <name> <program in its folder> <arguments>
:app
set "N=%~1"
set "EXE=%~2"
set "XA=%~3"
set "SRC=%CDR%\apps\!N!"
set "AD=%APPS%\!N!"
xcopy "!SRC!" "!AD!\" /e /i /q /y >nul
call :say "### !N!: vxpatch /check"
"%VX%\vxpatch.exe" /check /q "!AD!" > "%R%\!N!-check.txt" 2>&1
find "file(s) checked" "%R%\!N!-check.txt" >> "%LOG%"
call :say "### !N!: plain vxpatch"
"%VX%\vxpatch.exe" /q "!AD!" > "%R%\!N!-patch.txt" 2>&1
find "file(s) checked" "%R%\!N!-patch.txt" >> "%LOG%"
call :launch "!N!-plain" "!AD!\!EXE!" "!XA!"
rem what users get: copied into Program Files, then only the automatic mode acts
set "PD=%ProgramFiles%\VXT-!N!"
call :say "### !N!: copied into !PD!, automatic mode only"
xcopy "!SRC!" "!PD!\" /e /i /q /y >nul
call :wait_auto "!PD!"
call :launch "!N!-auto" "!PD!\!EXE!" "!XA!"
goto :eof

rem wait_auto <folder>: until the automatic mode reports the folder (10 min max)
:wait_auto
set "WF=%~1"
for /l %%w in (1,1,120) do (
    findstr /i /c:"!WF!: " "%ALOG%" > "%T%\autohit.txt" 2>nul
    find "made compatible" "%T%\autohit.txt" >nul 2>&1 && goto :auto_done
    ping -n 6 127.0.0.1 >nul
)
call :say "auto: NOTHING logged for !WF! within 10 minutes"
goto :eof
:auto_done
for /f "usebackq delims=" %%l in ("%T%\autohit.txt") do call :say "auto: %%l"
goto :eof

rem launch <tag> <program> <arguments>: run it for 100 s at most, under
rem vxdbg, which reports crashes (where), exit codes and debug output
:launch
set "TAG=%~1"
set "PROG=%~2"
set "IMG=%~nx2"
call :say "### !TAG!: start !IMG!"
rem the loader explains what it cannot resolve ("loader snaps", seen by vxdbg)
set "IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\!IMG!"
rem (added to VistaExt's own flag 0x100 if the program is registered for the
rem in-memory mode: replacing it would switch VistaExt off)
set "GF=2"
reg query "!IFEO!" /v VistaExt >nul 2>&1 && set "GF=0x102"
reg add "!IFEO!" /v GlobalFlag /t REG_DWORD /d !GF! /f >nul 2>&1
start "" /b "%B%\vxdbg.exe" 100 "!PROG!" %~3 > "%R%\!TAG!-run.txt" 2>&1
ping -n 61 127.0.0.1 >nul
2>nul >COM1 echo SHOT !TAG!
ping -n 9 127.0.0.1 >nul
tasklist /fi "imagename eq !IMG!" 2>nul | find /i "!IMG!" > "%T%\running.txt"
for /f %%c in ('find /c /v "" ^< "%T%\running.txt"') do set "NP=%%c"
if "!NP!"=="0" (call :say "!TAG!: NOT running after a minute") else (call :say "!TAG!: running after a minute, !NP! process(es)")
for /l %%w in (1,1,45) do (
    find "[vxdbg:" "%R%\!TAG!-run.txt" >nul 2>&1 && goto :launched
    ping -n 2 127.0.0.1 >nul
)
:launched
rem the full output (with the loader's messages) stays in <tag>-run.txt
findstr /v /c:"LdrpUpdateLoadCount" /c:"LdrpLoadDll" /c:"LdrpFindOrMapDll" /c:"LdrpSnap" /c:"LdrpMapDll" /c:"LdrpResolve" /c:"LdrpGetProcedureAddress" /c:"LdrGetProcedureAddressEx" /c:"LdrpSearchPath" /c:"LdrpApplyFileNameRedirection" /c:"LdrpCheckForLoadedDll" /c:"LdrpFindKnownDll" /c:"LdrpCallInitRoutine" /c:"LdrLoadDll" /c:"LdrpProcessWork" /c:"LdrpInitializeProcess" /c:"LdrpRunInitializeRoutines" /c:"LdrpLoadImportModule" /c:"LdrpMapViewOfSection" /c:"LdrpHandleOneNewFormatImportDescriptor" /c:"LdrpHandleOneOldFormatImportDescriptor" /c:"LdrpWalkImportDescriptor" "%R%\!TAG!-run.txt" >> "%LOG%" 2>nul
findstr /c:"exception 0x" /c:"exited 0x" /c:"[vxdbg:" "%R%\!TAG!-run.txt" > "%T%\summary.txt" 2>nul
for /f "usebackq delims=" %%l in ("%T%\summary.txt") do 2>nul >COM1 echo   %%l
taskkill /f /t /im "!IMG!" >nul 2>&1
reg query "!IFEO!" /v VistaExt >nul 2>&1 && (reg add "!IFEO!" /v GlobalFlag /t REG_DWORD /d 0x100 /f >nul 2>&1) || (reg delete "!IFEO!" /f >nul 2>&1)
ping -n 4 127.0.0.1 >nul
goto :eof

:say
>>"%LOG%" echo %~1
2>nul >COM1 echo %~1
echo %~1
goto :eof

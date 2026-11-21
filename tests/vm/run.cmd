@echo off
rem VistaExt test suite for a real Windows Vista / 7 VM (tests/vm/vm.sh test).
rem Started at logon from the test CD (%1 = its drive). It installs VistaExt
rem from the release folder with install.cmd, tests it, uninstalls it with
rem uninstall.cmd, writes the results to the results disk and shuts down.
rem Every program runs through nofault.exe, so a failure is an exit code,
rem never a pop-up waiting for a click.
rem By hand (any Vista/7 PC or VM): run \vxvm\run.cmd from the test CD as
rem administrator. The results then go to a folder on the desktop. Do this
rem on a test system: it installs and removes VistaExt.
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "SELF=%~f0"
set "R="
set "BYHAND="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
if not defined R (
    set "BYHAND=1"
    set "R=%USERPROFILE%\Desktop\VistaExt test results"
)
if defined BYHAND (
    set "ADMIN="
    net session >nul 2>&1 && set "ADMIN=1"
    if not defined ADMIN fltmc >nul 2>&1 && set "ADMIN=1"
    if not defined ADMIN (
        echo Right-click run.cmd and choose "Run as administrator".
        pause
        exit /b 1
    )
    echo This installs VistaExt, tests it and uninstalls it again.
    echo Results: !R!
    pause
    if not exist "!R!" mkdir "!R!"
)
set "LOG=%R%\results.txt"
set "T=%SystemDrive%\vxt"
set "VX=%ProgramFiles%\VistaExt"
set "SYS=%windir%\System32"
set "WOW=%windir%\SysWOW64"
set "LAYERS=HKLM\SOFTWARE\VistaExt\Layers"
set "IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
rem the Safe Mode part restarts Windows twice; these carry the run across
set "SAFESTATE=%SystemDrive%\vxt-safe.state"
set "SAFEAFTER=%SystemDrive%\vxt-safe.after"
set PASS=0
set FAIL=0
if exist "%SAFESTATE%" for /f "usebackq tokens=1,2" %%a in ("%SAFESTATE%") do (set "PASS=%%a" & set "FAIL=%%b")
if not exist "%SAFESTATE%" > "%LOG%" echo VistaExt test on real Windows

set "CV=?"
set "BLD=?"
set "PN=?"
set "CSD="
set "K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
for /f "tokens=2*" %%a in ('reg query "%K%" /v ProductName 2^>nul ^| find "REG_"') do set "PN=%%b"
for /f "tokens=2*" %%a in ('reg query "%K%" /v CSDVersion 2^>nul ^| find "REG_"') do set "CSD=%%b"
for /f "tokens=2*" %%a in ('reg query "%K%" /v CurrentVersion 2^>nul ^| find "REG_"') do set "CV=%%b"
for /f "tokens=2*" %%a in ('reg query "%K%" /v CurrentBuildNumber 2^>nul ^| find "REG_"') do set "BLD=%%b"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
set "REALVER=!CV!.!BLD!"
if exist "%SAFESTATE%" (
    set "B=%T%\bin\%OSA%"
    set "NF=!B!\nofault.exe"
    if "%~2"=="safemode" goto :safe_in
    if exist "%SAFEAFTER%" goto :safe_after
    if exist "%SystemDrive%\vxt-uninst.after" goto :uninst_after
)
call :say "Windows: !PN! !CSD!, version !CV!, build !BLD!, %OSA%"
call :say "Started: %DATE% %TIME%"

if exist "%T%" rd /s /q "%T%"
xcopy "%CDR%\vxvm" "%T%\" /e /i /q /y >nul
attrib -r "%T%\*" /s /d >nul
set "B=%T%\bin\%OSA%"
set "NF=%B%\nofault.exe"

rem ---------------------------------------------------------- before install
rem Microsoft's C runtime update (KB3118401, which VistaExt needs) brings its
rem own api-ms-win-core-synch-l1-2-0.dll. VistaExt never replaces a Windows
rem file, so where it is, programs need patching for that API set.
set "OWNSYS="
set "OWNWOW="
if exist "%SYS%\api-ms-win-core-synch-l1-2-0.dll" set "OWNSYS=1"
if "%OSA%"=="x64" if exist "%WOW%\api-ms-win-core-synch-l1-2-0.dll" set "OWNWOW=1"
if defined OWNSYS call :say "Windows' own api-ms-win-core-synch-l1-2-0.dll is here already (a Microsoft update)"
call :say "### before VistaExt: a program that imports through API sets"
call :run 60 "%B%\apitest.exe"
call :nonzero "API-set program cannot start without VistaExt" !RC!

rem ---------------------------------------------------------------- install
call :say "### install.cmd"
cmd /c ""%T%\VistaExt\install.cmd"" < nul > "%R%\install.txt" 2>&1
call :expect "install.cmd succeeds" 0 !errorlevel!
type "%R%\install.txt" | find "file(s) added" >> "%LOG%"
call :exists "kernelvx.dll added to System32" "%SYS%\kernelvx.dll"
call :exists "uservx.dll added to System32" "%SYS%\uservx.dll"
call :exists "API-set DLL added: api-ms-win-core-synch-l1-2-0" "%SYS%\api-ms-win-core-synch-l1-2-0.dll"
if "%OSA%"=="x64" call :exists "32-bit kernelvx.dll added to SysWOW64" "%WOW%\kernelvx.dll"
if "%OSA%"=="x64" call :exists "32-bit API-set DLL added to SysWOW64" "%WOW%\api-ms-win-core-synch-l1-2-0.dll"
call :exists "tools installed: vxpatch, msifix, VistaExt Center" "%VX%\vxcenter.exe"
call :exists "Start menu shortcut" "%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs\VistaExt Center.lnk"
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt\command" >nul 2>&1
call :expect "Explorer right-click entry for programs" 0 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\Msi.Package\shell\VistaExtInstall\command" >nul 2>&1
call :expect "Explorer right-click entry for .msi packages" 0 !errorlevel!
findstr /i /l /c:"System32\kernelbase.dll" "%VX%\installed.txt" >nul
set "KB=!errorlevel!"
if "%CV%"=="6.0" call :expect "Vista: kernelbase.dll added (Vista has none)" 0 !KB!
if "%CV%"=="6.1" call :expect "Windows 7: its own kernelbase.dll left alone" 1 !KB!
rem one installer, four builds: it must pick this Windows' and architecture's
set "WANTB=win7-%OSA%"
if "%CV%"=="6.0" set "WANTB=vista-%OSA%"
reg query "HKLM\SOFTWARE\VistaExt" /v Build 2>nul | find "!WANTB!" >nul
call :expect "the installer picked the build for this Windows (!WANTB!)" 0 !errorlevel!
if "%CV%"=="6.0" call :absent "Vista build: no bcryptprimvx.dll (Vista uses VistaExt's bcryptprimitives.dll)" "%SYS%\bcryptprimvx.dll"
if "%CV%"=="6.0" call :exists "Vista build: VistaExt's bcryptprimitives.dll added" "%SYS%\bcryptprimitives.dll"
if "%CV%"=="6.1" call :exists "Windows 7 build: bcryptprimvx.dll added" "%SYS%\bcryptprimvx.dll"
findstr /m /l /c:"VistaExt-owned-file" "%SYS%\kernelvx.dll" >nul 2>&1
call :expect "VistaExt's DLLs carry the ownership marker" 0 !errorlevel!
findstr /m /l /c:"VistaExt-owned-file" "%SYS%\kernel32.dll" >nul 2>&1
call :expect "Windows' own DLLs do not" 1 !errorlevel!

call :say "### the tray icon, its autostart and the defaults"
call :exists "the tray icon program is installed" "%VX%\vxtray.exe"
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v VistaExt 2>nul | find /i "vxtray.exe" >nul
call :expect "the tray icon starts with Windows (Run entry)" 0 !errorlevel!
set "SM=%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs"
call :exists "Start menu: the tray icon" "%SM%\VistaExt tray icon.lnk"
call :exists "Start menu: turn VistaExt off, also in Safe Mode" "%SM%\Turn off VistaExt (also in Safe Mode).lnk"
reg query "HKLM\SOFTWARE\VistaExt" /v Version 2>nul | find "WIN10" >nul
call :expect "programs are told Windows 10 by default" 0 !errorlevel!
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x1" >nul
call :expect "VistaExt is on after installing" 0 !errorlevel!
sc query VistaExtAuto | find "RUNNING" >nul
call :expect "VistaExt's service runs (hidden, no window)" 0 !errorlevel!
sc qc VistaExtAuto | find "DELAYED" >nul
call :expect "the service starts with Windows, not delayed (programs started at sign-in need it)" 1 !errorlevel!
start "" "%VX%\vxtray.exe" /showmenu
ping -n 5 127.0.0.1 >nul
tasklist /fi "imagename eq vxtray.exe" 2>nul | find /i "vxtray.exe" >nul
call :expect "the tray icon runs" 0 !errorlevel!
2>nul >COM1 echo SHOT tray-menu
ping -n 4 127.0.0.1 >nul
2>nul >COM1 echo KEYS esc
ping -n 2 127.0.0.1 >nul
start "" "%VX%\vxtray.exe"
ping -n 3 127.0.0.1 >nul
tasklist /fi "imagename eq vxtray.exe" 2>nul | find /c /i "vxtray.exe" | find "1" >nul
call :expect "only one tray icon per user, also when started again" 0 !errorlevel!
rem the tests below give programs their own versions; the global setting has its own part
call :run 60 "%VX%\vxauto.exe" /setver REAL

call :say "### a VistaExt file that Windows replaced (as Windows Update can) is left alone"
set "REPL=%SYS%\api-ms-win-core-winrt-l1-1-0.dll"
copy /y "%SYS%\version.dll" "%REPL%" >nul
cmd /c ""%T%\VistaExt\install.cmd"" < nul > "%R%\install2.txt" 2>&1
call :expect "install.cmd again, over an existing install, succeeds" 0 !errorlevel!
fc /b "%REPL%" "%SYS%\version.dll" >nul
call :expect "reinstalling does not overwrite the Windows file" 0 !errorlevel!

rem ------------------------------------------------ recommended settings
call :say "### vxpatch /auto on a Chromium-style folder saves the Chromium options"
set "FC=%T%\fakechrome"
mkdir "%FC%" 2>nul
copy /y "%B%\vxtest.exe" "%FC%\app.exe" >nul
copy /y "%B%\memdll.dll" "%FC%\chrome_elf.dll" >nul
call :run 120 "%VX%\vxpatch.exe" /file /auto /q "%FC%"
reg query "%LAYERS%" /v "%FC%\\" 2>nul | find "ARGS:--no-sandbox --disable-gpu" >nul
call :expect "... the saved setting has the options, intact" 0 !errorlevel!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%FC%"
reg delete "%LAYERS%" /v "%FC%\\" /f >nul 2>&1
rem a renamed Chromium (Edge: msedge_elf.dll): only V8's and ICU's data
rem files, in a version folder below the program
set "FE=%T%\fakeedge"
mkdir "%FE%\1.0.0.0" 2>nul
copy /y "%B%\vxtest.exe" "%FE%\browser.exe" >nul
copy /y "%B%\memdll.dll" "%FE%\1.0.0.0\browser_elf.dll" >nul
> "%FE%\1.0.0.0\icudtl.dat" echo x
> "%FE%\1.0.0.0\v8_context_snapshot.bin" echo x
call :run 120 "%VX%\vxpatch.exe" /file /auto /q "%FE%"
reg query "%LAYERS%" /v "%FE%\\" 2>nul | find "ARGS:--no-sandbox --disable-gpu" >nul
call :expect "... a renamed Chromium (only V8/ICU data files) is recognized too" 0 !errorlevel!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%FE%"
reg delete "%LAYERS%" /v "%FE%\\" /f >nul 2>&1

if exist "%T%\only-diag.txt" goto :diag
if exist "%T%\only-watch.txt" goto :watchdiag
if exist "%T%\only-uninst.txt" goto :uninst
rem --------------------------------------------------------- performance
call :bench
if exist "%T%\only-bench.txt" goto :finish

rem ------------------------------------------- programs importing API sets
call :apisettests x64
call :apisettests x86

rem ------------------------------------------------- programs, per architecture
call :archtests x64
call :archtests x86

rem ---------------------------------------- in-memory mode (vxload.dll)
call :memtests x64
call :memtests x86

rem -------------------------------------------------------------------- msi
call :msitests

rem ------------------------------------- the global version, setups, the switch
call :globaltests
call :setuptests
call :switchtests

rem ------------------------------------------------- Safe Mode (two restarts)
call :safe_prepare
:safe_done

rem ------------------------------------------------------- VistaExt Center
call :say "### VistaExt Center window (picture: center.png)"
>>"%LOG%" 2>&1 "%NF%" 60 "%B%\guishot.exe" "%VX%\vxcenter.exe" "%T%\%OSA%\app\vxtest.exe" "%R%\center.bmp" /onscreen
call :expect "VistaExt Center opens and lists the program's needs" 0 !errorlevel!

rem -------------------------------------------------------------- uninstall
call :say "### uninstall.cmd while a patched program is running"
set "BUSY=%T%\busy"
mkdir "%BUSY%" 2>nul
copy /y "%B%\apitest.exe" "%BUSY%\vxbusy.exe" >nul
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll" api-ms-win-core-synch-l1-2-0.dll.vxtest
call :run 120 "%VX%\vxpatch.exe" /file /q "%BUSY%\vxbusy.exe"
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll.vxtest" api-ms-win-core-synch-l1-2-0.dll
call :run 120 "%VX%\vxpatch.exe" /file /q "%T%\%OSA%\app\vxtest.exe"
start "" "%BUSY%\vxbusy.exe" /wait:300
ping -n 4 127.0.0.1 >nul
cmd /c ""%VX%\uninstall.cmd"" < nul > "%R%\uninstall-busy.txt" 2>&1
call :expect "uninstall.cmd stops while a patched program is running" 1 !errorlevel!
call :exists "... and removes nothing: the DLLs stay" "%SYS%\kernelvx.dll"
call :exists "... and the tools stay" "%VX%\vxpatch.exe"
fc /b "%T%\%OSA%\app\vxtest.exe" "%T%\bin\%OSA%\vxtest.exe" >nul
call :expect "... and the other patched programs stay patched, so they keep working" 1 !errorlevel!
taskkill /f /im vxbusy.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul

call :say "### uninstall.cmd (with programs still patched, switched off and on once)"
set "APP=%T%\%OSA%\app\vxtest.exe"
call :run 120 "%VX%\vxpatch.exe" /file /q "%APP%"
rem Windows Search's filter host can have VistaExt's API-set DLLs loaded (with
rem the deps baseline's updates, a component it loads imports two of them; see
rem KNOWN_RISKS "New system DLLs"). uninstall.cmd then rightly leaves them for
rem a restart. Stopped here (a test VM) so that removal in this session is checked.
net stop WSearch >nul 2>&1
taskkill /f /im SearchFilterHost.exe >nul 2>&1
taskkill /f /im SearchProtocolHost.exe >nul 2>&1
cmd /c ""%VX%\uninstall.cmd"" < nul > "%R%\uninstall.txt" 2>&1
call :expect "uninstall.cmd succeeds" 0 !errorlevel!
if exist "%SYS%\kernelvx.dll" (
    rem who still has it loaded (diagnosis for the check below)
    >>"%LOG%" echo   processes with kernelvx.dll loaded:
    tasklist /m kernelvx.dll >> "%LOG%" 2>&1
)
call :absent "kernelvx.dll removed from System32" "%SYS%\kernelvx.dll"
if defined OWNSYS (call :exists "Windows' own API-set DLL kept" "%SYS%\api-ms-win-core-synch-l1-2-0.dll") else (call :absent "API-set DLLs removed" "%SYS%\api-ms-win-core-synch-l1-2-0.dll")
if "%OSA%"=="x64" call :absent "32-bit kernelvx.dll removed from SysWOW64" "%WOW%\kernelvx.dll"
if "%CV%"=="6.0" call :absent "Vista: added kernelbase.dll removed" "%SYS%\kernelbase.dll"
if "%CV%"=="6.1" call :exists "Windows 7: its own kernelbase.dll still there" "%SYS%\kernelbase.dll"
call :absent "VistaExt folder removed" "%VX%"
if exist "%VX%" (>>"%LOG%" echo   left in %VX%:& dir /b /a "%VX%" >>"%LOG%" 2>&1)
reg query "HKLM\SOFTWARE\VistaExt" >nul 2>&1
call :expect "VistaExt settings removed" 1 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt" >nul 2>&1
call :expect "Explorer entries removed" 1 !errorlevel!
fc /b "%APP%" "%T%\bin\%OSA%\vxtest.exe" >nul
call :expect "uninstall puts patched programs back" 0 !errorlevel!
fc /b "%BUSY%\vxbusy.exe" "%B%\apitest.exe" >nul
call :expect "... including the one that was running before" 0 !errorlevel!
call :exists "the file Windows replaced is kept" "%REPL%"
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v VistaExt >nul 2>&1
call :expect "the tray icon's autostart is removed" 1 !errorlevel!
tasklist /fi "imagename eq vxtray.exe" 2>nul | find /i "vxtray.exe" >nul
call :expect "the tray icon is closed" 1 !errorlevel!
sc query VistaExtAuto >nul 2>&1
call :expect "the service is removed (sc: no such service)" 1060 !errorlevel!
reg query "HKCU\Software\VistaExt" >nul 2>&1
call :expect "the user's own VistaExt settings are removed" 1 !errorlevel!
call :absent "no VistaExt.off folder is left in System32" "%SYS%\VistaExt.off"
call :absent "Start menu: tray icon shortcut removed" "%SM%\VistaExt tray icon.lnk"
call :absent "Start menu: 'Turn off' shortcut removed" "%SM%\Turn off VistaExt (also in Safe Mode).lnk"
call :absent "the change log is removed" "%ProgramData%\VistaExt\VistaExt.log"
reg query "%IFEO%\vxtest.exe" /v VistaExt >nul 2>&1
call :expect "no Image File Execution Options entry of VistaExt's is left" 1 !errorlevel!
del /f /q "%REPL%" >nul 2>&1
call :run 60 "%B%\apitest.exe"
call :nonzero "API-set program cannot start again once VistaExt is removed" !RC!

rem ---------------------------------------------------------------- the end
:finish
wevtutil qe Application /c:40 /rd:true /f:text > "%R%\events.txt" 2>&1
call :say "Finished: %DATE% %TIME%"
call :say "RESULT: !PASS! passed, !FAIL! failed"
echo done> "%R%\done.marker"
if defined BYHAND (
    echo.
    echo Results: %R%\results.txt
    pause
    exit /b 0
)
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

rem =========================================================================
:archtests
set "A=%~1"
if "%OSA%"=="x86" if "%A%"=="x64" goto :eof
set "W=%T%\%A%"
set "APP=%W%\app\vxtest.exe"
set "ORIG=%T%\bin\%A%\vxtest.exe"
mkdir "%W%\app" "%W%\wv2rt" 2>nul
copy /y "%ORIG%" "%APP%" >nul

call :say "### %A%: unpatched program"
call :run 60 "%APP%"
call :expect "%A% unpatched program is refused by Windows - entry point not found" -1073741511 !RC!

call :say "### %A%: vxpatch /check, then vxpatch"
call :run 120 "%VX%\vxpatch.exe" /check "%APP%"
call :run 120 "%VX%\vxpatch.exe" /file "%APP%"
call :expect "%A% vxpatch makes the program compatible" 0 !RC!

call :say "### %A%: patched program, VistaExt's own code"
set VX_NO_NATIVE=1
call :run 60 "%APP%"
set VX_NO_NATIVE=
call :expect "%A% patched program runs, all checks pass - VistaExt's own code" 0 !RC!
call :say "### %A%: patched program, Windows' own functions first"
call :run 60 "%APP%"
call :expect "%A% patched program runs, all checks pass - Windows' functions first" 0 !RC!

call :say "### %A%: Windows version setting"
call :run 60 "%VX%\vxpatch.exe" /q /file /winver:WIN11 "%APP%"
call :run 60 "%APP%" /expect:10.0.22631
call :expect "%A% set to Windows 11 with vxpatch /winver: versions and kernel32's file version agree" 0 !RC!
reg add "%LAYERS%" /v "%APP%" /d "WIN10 ARGS:--hello-from-vistaext" /f >nul
call :run 60 "%APP%" /expect:10.0.19045
call :expect "%A% set to Windows 10: GetVersionEx, RtlGetVersion and VerifyVersionInfo agree" 0 !RC!
reg add "%LAYERS%" /v "%APP%" /d "WIN7" /f >nul
call :run 60 "%APP%" /expect:6.1.7601
call :expect "%A% set to Windows 7" 0 !RC!
reg delete "%LAYERS%" /v "%APP%" /f >nul
reg add "%LAYERS%" /v "%W%\app\\" /d "WIN81" /f >nul
call :run 60 "%APP%" /expect:6.3.9600
call :expect "%A% folder set to Windows 8.1 applies to the program inside" 0 !RC!
reg delete "%LAYERS%" /v "%W%\app\\" /f >nul

call :say "### %A%: WebView2 runtime folder"
copy /y "%ORIG%" "%W%\wv2rt\msedgewebview2.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /q /wv2runtime:%A% "%W%\wv2rt"
"%NF%" 60 "%APP%" > "%W%\wv2.txt" 2>&1
type "%W%\wv2.txt" | find "WEBVIEW2" >> "%LOG%"
find /i "wv2rt" "%W%\wv2.txt" >nul
call :expect "%A% patched program is given the WebView2 runtime folder" 0 !errorlevel!
reg delete "HKLM\SOFTWARE\VistaExt\WebView2" /f >nul 2>&1

call :say "### %A%: vxpatch /undo"
call :run 120 "%VX%\vxpatch.exe" /undo "%APP%"
fc /b "%APP%" "%ORIG%" >nul
call :expect "%A% /undo puts the original file back" 0 !errorlevel!

rem A program that updates itself replaces the patched file. Undo must not
rem put the old version back, and patching the new one must renew the backup.
call :say "### %A%: undo and patch after the program updated itself"
call :run 120 "%VX%\vxpatch.exe" /file /q "%APP%"
copy /b /y "%ORIG%" + "%T%\run.cmd" "%W%\new1.exe" >nul
copy /y "%W%\new1.exe" "%APP%" >nul
call :run 120 "%VX%\vxpatch.exe" /undo "%APP%"
fc /b "%APP%" "%W%\new1.exe" >nul
call :expect "%A% /undo keeps the updated program, no old version put back" 0 !errorlevel!
call :absent "%A% ... and deletes the outdated backup" "%APP%.vxorig"
call :run 120 "%VX%\vxpatch.exe" /file /q "%APP%"
copy /b /y "%ORIG%" + "%T%\run.cmd" + "%T%\run.cmd" "%W%\new2.exe" >nul
copy /y "%W%\new2.exe" "%APP%" >nul
call :run 120 "%VX%\vxpatch.exe" /file "%APP%"
call :run 120 "%VX%\vxpatch.exe" /undo "%APP%"
fc /b "%APP%" "%W%\new2.exe" >nul
call :expect "%A% patching an updated program renews its backup" 0 !errorlevel!
copy /y "%ORIG%" "%APP%" >nul
goto :eof

rem -------------------------------------------------------------------------
:apisettests
set "A=%~1"
if "%OSA%"=="x86" if "%A%"=="x64" goto :eof
set "AD=%SYS%"
if "%OSA%"=="x64" if "%A%"=="x86" set "AD=%WOW%"
set "W=%T%\%A%\api"
mkdir "%W%" 2>nul
copy /y "%T%\bin\%A%\apitest.exe" "%W%\apitest.exe" >nul
set "OWN=%OWNSYS%"
if "%OSA%"=="x64" if "%A%"=="x86" set "OWN=%OWNWOW%"
call :say "### %A%: program importing through API sets"
if defined OWN goto :apiset_own
call :run 60 "%W%\apitest.exe"
call :expect "%A% API-set program runs with VistaExt's API-set DLLs" 0 !RC!
call :say "### %A%: the same when this Windows' copy of the API-set DLL lacks the functions"
ren "%AD%\api-ms-win-core-synch-l1-2-0.dll" api-ms-win-core-synch-l1-2-0.dll.vxtest
call :run 60 "%W%\apitest.exe"
call :nonzero "%A% without that API-set DLL the program cannot start" !RC!
call :run 120 "%VX%\vxpatch.exe" /file "%W%\apitest.exe"
call :expect "%A% vxpatch points the API-set import at VistaExt" 0 !RC!
call :run 60 "%W%\apitest.exe"
call :expect "%A% ... and then the program runs" 0 !RC!
ren "%AD%\api-ms-win-core-synch-l1-2-0.dll.vxtest" api-ms-win-core-synch-l1-2-0.dll
call :run 120 "%VX%\vxpatch.exe" /undo "%W%\apitest.exe"
fc /b "%W%\apitest.exe" "%T%\bin\%A%\apitest.exe" >nul
call :expect "%A% /undo restores it" 0 !errorlevel!
goto :eof

:apiset_own
rem Windows' own file stays: unchanged, the program can't start; patched, it runs
call :run 60 "%W%\apitest.exe"
call :nonzero "%A% Windows' own API-set DLL is kept, so the unchanged program cannot start" !RC!
call :run 120 "%VX%\vxpatch.exe" /file "%W%\apitest.exe"
call :expect "%A% vxpatch points the API-set import at VistaExt" 0 !RC!
call :run 60 "%W%\apitest.exe"
call :expect "%A% ... and then the program runs" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo "%W%\apitest.exe"
fc /b "%W%\apitest.exe" "%T%\bin\%A%\apitest.exe" >nul
call :expect "%A% /undo restores it" 0 !errorlevel!
goto :eof

rem -------------------------------------------------------------------------
:msitests
set "M=%T%\msi"
mkdir "%M%" 2>nul
call :say "### msifix on a package that wants Windows 8.1 and Installer 5.0"
call :run 60 "%B%\msitest.exe" create "%M%\modern.msi" "%T%\bin\%OSA%\vxtest.exe"
"%NF%" 120 "%VX%\msifix.exe" "%M%\modern.msi" "%M%\fixed.msi" > "%M%\fix.txt" 2>&1
set "RC=!errorlevel!"
type "%M%\fix.txt" >> "%LOG%"
call :expect "msifix makes a fixed copy" 0 !RC!
"%NF%" 60 "%B%\msitest.exe" dump "%M%\fixed.msi" "%M%\ca.exe" > "%M%\dump.txt" 2>&1
type "%M%\dump.txt" >> "%LOG%"
set "D=%M%\dump.txt"
call :has "64-bit check kept" "%D%" "[VersionNT64]"
call :has "XP check untouched" "%D%" "[VersionNT >= 501]"
call :has "negated check left as it is" "%D%" "[NOT (VersionNT >= 1000)]"
call :has "package platform kept" "%D%" "template=x64;1033"
call :has "package code kept" "%D%" "package code={11111111-2222-3333-4444-555555555555}"
call :has "install-step condition untouched without /all" "%D%" "[InstallFiles] [VersionNT >= 1000]"
call :has "custom action patched" "%M%\fix.txt" "custom action ModernCA: patched to use VistaExt"
if "%CV%"=="6.0" (
    call :has "Vista: Windows 8.1 check lowered" "%D%" "[Installed OR VersionNT >= 600]"
    call :has "Vista: build check lowered" "%D%" "WindowsBuild >= 6002 AND VersionNT >= 600"
    call :has "Vista: schema lowered to 405" "%D%" "schema=405"
    call :has "Vista: Installer 5.0 table removed" "%D%" "MsiServiceConfig table: absent"
    call :hasnot "Vista: Installer 5.0 action removed" "%D%" "[MsiConfigureServices]"
)
if "%CV%"=="6.1" (
    call :has "Windows 7: Windows 8.1 check lowered" "%D%" "[Installed OR VersionNT >= 601]"
    call :has "Windows 7: build check lowered" "%D%" "WindowsBuild >= 7601 AND VersionNT>600"
    call :has "Windows 7: schema 500 kept" "%D%" "schema=500"
    call :has "Windows 7: Installer 5.0 table kept" "%D%" "MsiServiceConfig table: present"
)
rem The real test of the write-back (Wine cannot do this): the custom
rem action taken out of the fixed package must be the patched program.
call :say "### custom action from the fixed package"
call :run 60 "%M%\ca.exe"
call :expect "custom action stored in the fixed package is patched and runs" 0 !RC!
"%NF%" 60 "%B%\msitest.exe" dump "%M%\modern.msi" "%M%\caorig.exe" > "%M%\dumporig.txt" 2>&1
call :has "original package untouched" "%M%\dumporig.txt" "schema=500"
goto :eof

rem -------------------------------------------------------------------------
:memtests
set "A=%~1"
if "%OSA%"=="x86" if "%A%"=="x64" goto :eof
set "AD=%SYS%"
if "%OSA%"=="x64" if "%A%"=="x86" set "AD=%WOW%"
set "W=%T%\%A%\mem"
set "IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
mkdir "%W%" "%T%\%A%\other" 2>nul
for %%f in (vxtest.exe memhost.exe memdll.dll apitest.exe crashtest.exe) do copy /y "%T%\bin\%A%\%%f" "%W%\" >nul
copy /y "%T%\bin\%A%\memdll.dll" "%W%\memdll2.dll" >nul
copy /y "%T%\bin\%A%\vxtest.exe" "%T%\%A%\other\vxtest.exe" >nul
call :say "### %A%: in-memory mode"
call :run 60 "%W%\memhost.exe"
call :expect "%A% memory: before, the program with a Windows 8 DLL is refused" -1073741511 !RC!
call :run 120 "%VX%\vxpatch.exe" /memory "%W%"
call :expect "%A% memory: vxpatch /memory registers the folder" 0 !RC!
fc /b "%W%\vxtest.exe" "%T%\bin\%A%\vxtest.exe" >nul
call :expect "%A% memory: the program file is not changed" 0 !errorlevel!
fc /b "%W%\memdll.dll" "%T%\bin\%A%\memdll.dll" >nul
call :expect "%A% memory: the DLL file is not changed" 0 !errorlevel!
call :run 60 "%W%\vxtest.exe"
call :expect "%A% memory: the program runs, all checks pass" 0 !RC!
call :run 60 "%W%\memhost.exe"
call :expect "%A% memory: a DLL loaded at start and one loaded later are redirected too" 0 !RC!
call :run 60 "%T%\%A%\other\vxtest.exe"
call :expect "%A% memory: a program of the same name elsewhere is left alone" -1073741511 !RC!
rem API sets: without VistaExt's api-ms file in the system folder, the
rem import goes to VistaExt's vxs- copy, which Windows' API-set table ignores
ren "%AD%\api-ms-win-core-synch-l1-2-0.dll" api-ms-win-core-synch-l1-2-0.dll.vxtest
call :run 60 "%W%\apitest.exe"
call :expect "%A% memory: API-set imports go to VistaExt's own copy" 0 !RC!
ren "%AD%\api-ms-win-core-synch-l1-2-0.dll.vxtest" api-ms-win-core-synch-l1-2-0.dll
rem crash window: a windowed program that stops itself after 4 s
call :say "### %A%: crash window (picture: shots\crash-%A%.png)"
start "" "%W%\crashtest.exe"
ping -n 12 127.0.0.1 >nul
tasklist /fi "imagename eq vxwatch.exe" 2>nul | find /i "vxwatch.exe" >nul
call :expect "%A% crash window appears after the program crashed" 0 !errorlevel!
tasklist /fi "imagename eq crashtest.exe" 2>nul | find /i "crashtest.exe" >nul
call :expect "%A% ... and the program itself is gone (no Windows error box holding it)" 1 !errorlevel!
2>nul >COM1 echo SHOT crash-%A%
ping -n 6 127.0.0.1 >nul
taskkill /f /im vxwatch.exe >nul 2>&1
rem a helper process (started by the program itself) that crashes gets no
rem window of its own: the main process is the one users see
rem watchers of other windowed programs still running are counted first:
rem only a new one would be the helper's
set "N0=0"
for /f %%n in ('tasklist /fi "imagename eq vxwatch.exe" 2^>nul ^| find /c /i "vxwatch.exe"') do set "N0=%%n"
start "" "%W%\crashtest.exe" /helper
ping -n 6 127.0.0.1 >nul
rem the program waits for its crashed helper (Windows Error Reporting may hold
rem it for a while); its own watcher runs, silently, until it ends: so wait for
rem the program to end before looking for a crash window
for /l %%w in (1,1,30) do (tasklist /fi "imagename eq crashtest.exe" 2>nul | find /i "crashtest.exe" >nul && ping -n 2 127.0.0.1 >nul)
ping -n 4 127.0.0.1 >nul
set "N1=0"
for /f %%n in ('tasklist /fi "imagename eq vxwatch.exe" 2^>nul ^| find /c /i "vxwatch.exe"') do set "N1=%%n"
set "HW=1"
if !N1! GTR !N0! (
    set "HW=0"
    rem a new watcher is still there: whose, and what is on screen
    2>nul >COM1 echo SHOT helper-%A%
    ping -n 6 127.0.0.1 >nul
    >>"%LOG%" echo   watchers before !N0!, after !N1!; still running:
    wmic process where "name='vxwatch.exe' or name='crashtest.exe' or name='WerFault.exe'" get ProcessId,ParentProcessId,CommandLine /format:list 2>nul | find "=" >> "%LOG%"
    wmic process get Name,ProcessId /format:csv 2>nul | find "," >> "%LOG%"
) else if !N0! GTR 0 (
    >>"%LOG%" echo   ^(!N0! watcher^(s^) of other windowed programs still running were there before^)
    wmic process get Name,ProcessId,ParentProcessId /format:csv 2>nul | find "," >> "%LOG%"
)
call :expect "%A% no crash window for a crashed helper process of a program" 1 !HW!
taskkill /f /im vxwatch.exe >nul 2>&1
taskkill /f /im crashtest.exe >nul 2>&1
rem "Restart with this setting": answered by the test hook, Windows 10
set VX_WATCH_TEST_CHOICE=WIN10
start "" "%W%\crashtest.exe"
set VX_WATCH_TEST_CHOICE=
for /l %%w in (1,1,20) do if not exist "%W%\restarted-as-10.txt" ping -n 2 127.0.0.1 >nul
call :exists "%A% crash window restarts the program told the chosen version (Windows 10)" "%W%\restarted-as-10.txt"
reg delete "HKCU\SOFTWARE\VistaExt\Layers" /v "%W%\crashtest.exe" /f >nul 2>&1
call :run 120 "%VX%\vxpatch.exe" /undo "%W%"
call :expect "%A% memory: /undo succeeds" 0 !RC!
call :run 60 "%W%\memhost.exe"
call :expect "%A% memory: after /undo it is refused again" -1073741511 !RC!
reg query "%IFEO%\memhost.exe" >nul 2>&1
call :expect "%A% memory: /undo removes the Image File Execution Options entry" 1 !errorlevel!
goto :eof

rem ---------------------------------------------------------------------------
:bench
rem VistaExt's cost: the same measurements on plain Windows, in file mode and
rem in in-memory mode (numbers only: nothing here passes or fails)
set "W=%T%\bench"
mkdir "%W%\native" "%W%\file" "%W%\mem" 2>nul
copy /y "%B%\vxbench-native.exe" "%W%\native\" >nul
for %%d in (native file mem) do copy /y "%B%\benchdll.dll" "%W%\%%d\" >nul
for %%d in (file mem) do copy /y "%B%\vxbench.exe" "%W%\%%d\" >nul
call :say "### performance: plain Windows (no VistaExt)"
call :run 600 "%W%\native\vxbench-native.exe"
call :run 120 "%VX%\vxpatch.exe" /file /q "%W%\file"
call :say "### performance: file mode"
call :run 600 "%W%\file\vxbench.exe"
call :run 120 "%VX%\vxpatch.exe" /memory /q "%W%\mem"
call :say "### performance: in-memory mode"
call :run 600 "%W%\mem\vxbench.exe"
call :say "### performance: in-memory mode, verifier.dll left as Windows sets it up (VX_KEEP_VERIFIER=1)"
set VX_KEEP_VERIFIER=1
call :run 600 "%W%\mem\vxbench.exe"
set VX_KEEP_VERIFIER=
call :run 120 "%VX%\vxpatch.exe" /undo /q "%W%\file"
call :run 120 "%VX%\vxpatch.exe" /undo /q "%W%\mem"
goto :eof

rem =========================================================================
:globaltests
call :say "### the Windows version programs are told (the tray icon's setting)"
set "GV=%T%\global"
mkdir "%GV%" 2>nul
copy /y "%B%\vxtest.exe" "%GV%\vxtest.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%GV%"
call :run 60 "%VX%\vxauto.exe" /pipe STATUS
call :expect "the service answers the tray icon (status)" 0 !RC!
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN11
call :expect "the service takes a new global version, as the tray icon asks" 0 !RC!
call :run 60 "%GV%\vxtest.exe" /expectall:10.0.22631
call :expect "global Windows 11: GetVersionEx, RtlGetVersion, RtlGetNtVersionNumbers, VerifyVersionInfo, kernel32's file version, registry and WMI agree" 0 !RC!
call :run 60 "%VX%\vxauto.exe" /pipe SETVER VER:10.0.26100
call :run 60 "%GV%\vxtest.exe" /expectall:10.0.26100
call :expect "global custom version 10.0.26100: everything agrees" 0 !RC!
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN81
call :run 60 "%GV%\vxtest.exe" /expectall:6.3.9600
call :expect "global Windows 8.1: everything agrees, the registry as Windows 8.1 has it" 0 !RC!
reg add "%LAYERS%" /v "%GV%\vxtest.exe" /d "WIN10" /f >nul
call :run 60 "%GV%\vxtest.exe" /expectall:10.0.19045
call :expect "a program's own setting (Windows 10) wins over the global one" 0 !RC!
reg add "%LAYERS%" /v "%GV%\vxtest.exe" /d "REAL" /f >nul
call :run 60 "%GV%\vxtest.exe" /expectall:!REALVER!
call :expect "a program set to this Windows' own version sees the real one, everywhere" 0 !RC!
reg delete "%LAYERS%" /v "%GV%\vxtest.exe" /f >nul
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN99
call :expect "a version VistaExt does not know is refused" 1 !RC!
call :run 60 "%VX%\vxauto.exe" /pipe SETVER REAL
call :run 120 "%VX%\vxpatch.exe" /undo /q "%GV%"
goto :eof

:setuptests
call :say "### a setup saved to Downloads, double-clicked, and the helper it unpacks to Temp"
set "DL=%USERPROFILE%\Downloads"
if not exist "%DL%" mkdir "%DL%"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN10
copy /y "%B%\spawntest.exe" "%DL%\vxsetuptest.exe" >nul
copy /y "%B%\vxbench-native.exe" "%DL%\vxplain.exe" >nul
copy /y "%B%\spawntest.exe" "%DL%\notepad.exe" >nul
copy /y "%B%\vercheck.exe" "%DL%\vxver-setup.exe" >nul
copy /y "%B%\vercheck.exe" "%DL%\vxver.exe" >nul
copy /y "%B%\vercheck.exe" "%DL%\vxdriver-setup.exe" >nul
copy /y "%SYS%\wusa.exe" "%DL%\ms-setup.exe" >nul
for /l %%w in (1,1,20) do (reg query "%IFEO%\vxsetuptest.exe" /v VistaExt >nul 2>&1 || ping -n 2 127.0.0.1 >nul)
ping -n 6 127.0.0.1 >nul
reg query "%IFEO%\vxsetuptest.exe" /v VistaExt >nul 2>&1
call :expect "the service registers a new setup in Downloads that cannot run without VistaExt" 0 !errorlevel!
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe"
call :expect "it runs like on newer Windows, and the helper it unpacks to Temp and starts at once runs too (CreateProcess)" 0 !RC!
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe" /shell
call :expect "... also when the helper is started through ShellExecute" 0 !RC!
start "" /wait "%DL%\vxsetuptest.exe" "%B%\vxtest.exe"
call :expect "... and when the setup is started with cmd's START, as a shortcut would" 0 !errorlevel!
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vercheck.exe"
call :expect "a new helper of a setup, which runs as it is but wants Windows 10, is told it too" 0 !RC!
reg query "%IFEO%\vxplain.exe" >nul 2>&1
call :expect "a program that runs on this Windows as it is (not a setup) is left alone" 1 !errorlevel!
call :run 60 "%DL%\vxver-setup.exe"
call :expect "a setup that runs as it is but refuses this Windows (like Spotify's) is told the chosen version" 0 !RC!
call :run 60 "%DL%\vxver.exe"
call :expect "the same program not named or described as a setup is left alone: it refuses, as without VistaExt" 1 !RC!
reg query "%IFEO%\vxdriver-setup.exe" /v VistaExt >nul 2>&1
call :expect "a driver package sees the real Windows (not registered)" 1 !errorlevel!
reg query "%IFEO%\ms-setup.exe" /v VistaExt >nul 2>&1
call :expect "a setup signed by Microsoft sees the real Windows (not registered)" 1 !errorlevel!
set VX_NO_DIALOGS=1
call :run 60 "%DL%\vxver-setup.exe" /driver
set VX_NO_DIALOGS=
call :expect "a setup told another version may not install a driver that Windows loads while starting" 5 !RC!
sc query vxtestdrv >nul 2>&1
call :expect "... no such driver service exists" 1060 !errorlevel!
call :run 60 "%DL%\vxver-setup.exe"
call :expect "... and when run again it sees this Windows' real version (and refuses it)" 1 !RC!
reg delete "HKCU\Software\VistaExt\Layers" /v "%DL%\vxver-setup.exe" /f >nul 2>&1
call :run 60 "%VX%\vxauto.exe" /pipe SETUPS 0
copy /y "%B%\vercheck.exe" "%DL%\vxver2-setup.exe" >nul
ping -n 8 127.0.0.1 >nul
reg query "%IFEO%\vxver2-setup.exe" /v VistaExt >nul 2>&1
call :expect "with the tray icon's 'setup programs' turned off, such a setup is left alone" 1 !errorlevel!
call :run 60 "%VX%\vxauto.exe" /pipe SETUPS 1
reg query "%IFEO%\notepad.exe" /v VistaExt >nul 2>&1
call :expect "a setup named like a Windows program (notepad.exe) is never registered by name" 1 !errorlevel!
find /i "Windows has a program of the same name" "%ProgramData%\VistaExt\VistaExt.log" >nul
call :expect "... and the log says why" 0 !errorlevel!
del /f /q "%DL%\vxsetuptest.exe" "%DL%\vxplain.exe" "%DL%\notepad.exe" "%DL%\vxver*.exe" "%DL%\vxdriver-setup.exe" "%DL%\ms-setup.exe" "%TEMP%\vxchild-*.exe" >nul 2>&1
call :say "    (removed; waiting for VistaExt to forget it)"
for /l %%w in (1,1,30) do (reg query "%IFEO%\vxsetuptest.exe" /v VistaExt >nul 2>&1 && ping -n 3 127.0.0.1 >nul)
reg query "%IFEO%\vxsetuptest.exe" /v VistaExt >nul 2>&1
call :expect "a deleted setup's registration is removed too" 1 !errorlevel!
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\VistaExt-setups.log" >nul 2>&1
call :run 60 "%VX%\vxauto.exe" /pipe SETVER REAL
goto :eof

:switchtests
call :say "### switching VistaExt off and on (the tray icon's switch)"
set "SW=%T%\switch"
mkdir "%SW%\mem" "%SW%\file" "%SW%\busy" 2>nul
copy /y "%B%\vxtest.exe" "%SW%\mem\vxtest.exe" >nul
copy /y "%B%\vxtest.exe" "%SW%\file\vxfile.exe" >nul
copy /y "%B%\apitest.exe" "%SW%\busy\vxbusy2.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%SW%\mem"
call :run 120 "%VX%\vxpatch.exe" /file /q "%SW%\file\vxfile.exe"
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll" api-ms-win-core-synch-l1-2-0.dll.vxtest
call :run 120 "%VX%\vxpatch.exe" /file /q "%SW%\busy\vxbusy2.exe"
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll.vxtest" api-ms-win-core-synch-l1-2-0.dll
start "" "%SW%\busy\vxbusy2.exe" /wait:300
ping -n 4 127.0.0.1 >nul
call :run 180 "%VX%\vxauto.exe" /pipe DISABLE
call :expect "the service switches VistaExt off" 0 !RC!
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x0" >nul
call :expect "off: the setting says so" 0 !errorlevel!
call :absent "off: kernelvx.dll is out of System32" "%SYS%\kernelvx.dll"
call :absent "off: vxload.dll is out of System32" "%SYS%\vxload.dll"
call :exists "off: ... kept aside in System32\VistaExt.off" "%SYS%\VistaExt.off\kernelvx.dll"
if "%OSA%"=="x64" call :absent "off: 32-bit kernelvx.dll is out of SysWOW64" "%WOW%\kernelvx.dll"
reg query "%IFEO%\vxtest.exe" /v VerifierDlls >nul 2>&1
call :expect "off: the program's Image File Execution Options entry is gone" 1 !errorlevel!
fc /b "%SW%\file\vxfile.exe" "%B%\vxtest.exe" >nul
call :expect "off: a patched program file is put back" 0 !errorlevel!
call :run 60 "%SW%\mem\vxtest.exe"
call :expect "off: a program that needs VistaExt is refused, exactly as on Windows without it" -1073741511 !RC!
call :run 60 "%B%\apitest.exe"
call :nonzero "off: the API-set program cannot start, as before installing" !RC!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt" >nul 2>&1
call :expect "off: Explorer's VistaExt entries are gone" 1 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\Msi.Package\shell\Open\command" /ve 2>nul | find /i "msifix" >nul
call :expect "off: double-clicking an .msi is Windows' own again" 1 !errorlevel!
tasklist /fi "imagename eq vxbusy2.exe" 2>nul | find /i "vxbusy2.exe" >nul
call :expect "off: a changed program that was running keeps running" 0 !errorlevel!
"%VX%\vxauto.exe" /pipe STATUS 2>nul | find "waiting=1" >nul
call :expect "off: ... and is put back once it is closed (waiting)" 0 !errorlevel!
taskkill /f /im vxbusy2.exe >nul 2>&1
for /l %%w in (1,1,30) do (fc /b "%SW%\busy\vxbusy2.exe" "%B%\apitest.exe" >nul 2>&1 || ping -n 3 127.0.0.1 >nul)
fc /b "%SW%\busy\vxbusy2.exe" "%B%\apitest.exe" >nul
call :expect "off: ... closed: its original is back" 0 !errorlevel!
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\VistaExt-switch.log" >nul 2>&1
call :run 180 "%VX%\vxauto.exe" /pipe ENABLE
call :expect "the service switches VistaExt on again" 0 !RC!
call :exists "on: kernelvx.dll is back" "%SYS%\kernelvx.dll"
call :exists "on: vxload.dll is back" "%SYS%\vxload.dll"
call :absent "on: nothing left aside" "%SYS%\VistaExt.off\kernelvx.dll"
reg query "%IFEO%\vxtest.exe" /v VerifierDlls >nul 2>&1
call :expect "on: the Image File Execution Options entry is back" 0 !errorlevel!
fc /b "%SW%\file\vxfile.exe" "%B%\vxtest.exe" >nul
call :expect "on: the program file is patched again" 1 !errorlevel!
call :run 60 "%SW%\mem\vxtest.exe"
call :expect "on: the in-memory program runs again" 0 !RC!
call :run 60 "%SW%\file\vxfile.exe"
call :expect "on: the patched program runs again" 0 !RC!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt" >nul 2>&1
call :expect "on: Explorer's entries are back" 0 !errorlevel!
call :say "### a switch interrupted midway (power cut) is finished when the service starts"
reg add "HKLM\SOFTWARE\VistaExt" /v Enabled /t REG_DWORD /d 0 /f >nul
net stop VistaExtAuto >nul 2>&1
net start VistaExtAuto >nul 2>&1
ping -n 4 127.0.0.1 >nul
call :absent "set to off, then the service started: VistaExt's files are moved aside" "%SYS%\kernelvx.dll"
reg add "HKLM\SOFTWARE\VistaExt" /v Enabled /t REG_DWORD /d 1 /f >nul
net stop VistaExtAuto >nul 2>&1
net start VistaExtAuto >nul 2>&1
ping -n 4 127.0.0.1 >nul
call :exists "set to on, then the service started: they are back" "%SYS%\kernelvx.dll"
call :run 60 "%SW%\mem\vxtest.exe"
call :expect "... and programs run again" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%SW%\mem"
call :run 120 "%VX%\vxpatch.exe" /undo /q "%SW%\file"
call :run 120 "%VX%\vxpatch.exe" /undo /q "%SW%\busy"
goto :eof

rem ---- Safe Mode: Windows starts in it with VistaExt on; VistaExt does
rem ---- nothing there, and its Start menu switch turns it off. Then Windows
rem ---- starts normally, VistaExt stays off, and is turned on again.
:safe_prepare
call :say "### Safe Mode (Windows restarts twice)"
set "SF=%T%\safe"
mkdir "%SF%" 2>nul
copy /y "%B%\vxtest.exe" "%SF%\vxtest.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%SF%"
call :run 60 "%SF%\vxtest.exe"
call :expect "before: the program runs with VistaExt" 0 !RC!
> "%SAFESTATE%" echo !PASS! !FAIL!
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce" /v "*vxsafe" /d "cmd /c \"%SELF%\" %CDR% safemode" /f >nul
bcdedit /set {current} safeboot minimal >nul
call :say "    restarting into Safe Mode"
shutdown /r /t 3 /f
exit

:safe_in
call :say "--- in Safe Mode ---"
set "SF=%T%\safe"
call :say "SAFEBOOT_OPTION=%SAFEBOOT_OPTION%"
if /i "%SAFEBOOT_OPTION%"=="MINIMAL" (call :pass "Windows started in Safe Mode, with VistaExt installed and on") else (call :fail "Windows started in Safe Mode")
call :run 60 "%SF%\vxtest.exe"
call :expect "Safe Mode: VistaExt does nothing (the program is refused as without it)" -1073741511 !RC!
sc query VistaExtAuto 2>nul | find "RUNNING" >nul
call :expect "Safe Mode: VistaExt's service does not run" 1 !errorlevel!
call :run 180 "%VX%\vxauto.exe" /disable
call :expect "Safe Mode: 'Turn off VistaExt' from the Start menu works without the service" 0 !RC!
call :absent "Safe Mode: VistaExt's files are out of System32" "%SYS%\kernelvx.dll"
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x0" >nul
call :expect "Safe Mode: switched off for the next normal start too" 0 !errorlevel!
bcdedit /deletevalue {current} safeboot >nul
> "%SAFESTATE%" echo !PASS! !FAIL!
> "%SAFEAFTER%" echo after
call :say "    restarting normally"
shutdown /r /t 3 /f
exit

:safe_after
call :say "--- normal start after Safe Mode ---"
set "SF=%T%\safe"
set "SM=%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs"
del /f /q "%SAFESTATE%" "%SAFEAFTER%" >nul 2>&1
for /l %%w in (1,1,30) do (sc query VistaExtAuto 2>nul | find "RUNNING" >nul || ping -n 3 127.0.0.1 >nul)
sc query VistaExtAuto 2>nul | find "RUNNING" >nul
call :expect "Windows starts normally again; VistaExt's service runs" 0 !errorlevel!
call :absent "VistaExt stays off" "%SYS%\kernelvx.dll"
call :run 60 "%SF%\vxtest.exe"
call :expect "still off: the program is refused as without VistaExt" -1073741511 !RC!
call :run 180 "%VX%\vxauto.exe" /pipe ENABLE
call :expect "turned on again from the tray icon's switch" 0 !RC!
call :run 60 "%SF%\vxtest.exe"
call :expect "... and the program runs again" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%SF%"
set "REPL=%SYS%\api-ms-win-core-winrt-l1-1-0.dll"
set "OWNSYS="
if exist "%SYS%\api-ms-win-core-synch-l1-2-0.dll" findstr /m /l /c:"VistaExt-owned-file" "%SYS%\api-ms-win-core-synch-l1-2-0.dll" >nul 2>&1 || set "OWNSYS=1"
goto :safe_done

rem ---- uninstalling with VistaExtSetup.exe while a program VistaExt runs is open
:uninst
call :say "### uninstall (VistaExtSetup.exe) while a program VistaExt runs is open"
set "UN=%T%\uninst"
mkdir "%UN%" 2>nul
copy /y "%B%\apitest.exe" "%UN%\vxbusy3.exe" >nul
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll" api-ms-win-core-synch-l1-2-0.dll.vxtest
call :run 120 "%VX%\vxpatch.exe" /memory /q "%UN%"
ren "%SYS%\api-ms-win-core-synch-l1-2-0.dll.vxtest" api-ms-win-core-synch-l1-2-0.dll
start "" "%UN%\vxbusy3.exe" /wait:600
ping -n 5 127.0.0.1 >nul
tasklist /fi "imagename eq vxbusy3.exe" 2>nul | find /i "vxbusy3.exe" >nul
call :expect "a program VistaExt runs is open" 0 !errorlevel!
copy /y "%T%\VistaExtSetup.exe" "%VX%\VistaExtSetup.exe" >nul
start "" /wait "%VX%\VistaExtSetup.exe" /uninstall /quiet
for /l %%w in (1,1,30) do (if exist "%VX%\vxpatch.exe" ping -n 3 127.0.0.1 >nul)
ping -n 6 127.0.0.1 >nul
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\uninst-setup.log" >nul 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager" /v PendingFileRenameOperations > "%R%\uninst-pending.txt" 2>&1
find /i "System32\kernelvx.dll" "%R%\uninst-pending.txt" >nul
call :expect "a VistaExt file the open program uses is deleted when Windows restarts" 0 !errorlevel!
call :absent "the service is gone" "%VX%\vxauto.exe"
reg query "HKLM\SOFTWARE\VistaExt" >nul 2>&1
call :expect "settings are gone" 1 !errorlevel!
> "%SAFESTATE%" echo !PASS! !FAIL!
> "%SystemDrive%\vxt-uninst.after" echo after
call :say "    restarting"
shutdown /r /t 3 /f
exit

:uninst_after
call :say "--- after the restart ---"
del /f /q "%SAFESTATE%" "%SystemDrive%\vxt-uninst.after" >nul 2>&1
call :absent "kernelvx.dll is gone from System32" "%SYS%\kernelvx.dll"
call :absent "vxload.dll is gone" "%SYS%\vxload.dll"
if "%OSA%"=="x64" call :absent "32-bit kernelvx.dll is gone from SysWOW64" "%WOW%\kernelvx.dll"
dir /b "%SYS%\api-ms-win-core-winrt-l1-1-0.dll" "%SYS%\vxs-*" > "%T%\left.txt" 2>nul
for /f %%c in ('find /c /v "" ^< "%T%\left.txt"') do call :say "    VistaExt API-set files left: %%c"
call :absent "VistaExt's folder in Program Files is gone" "%VX%"
call :absent "the log folder is gone" "%ProgramData%\VistaExt"
goto :finish

rem ---- diagnostics (VX_VM_ONLY=diag): the in-memory loader's own report
:watchdiag
rem the crash watcher with 32-bit programs: which processes exist when
call :say "### diagnostics: crash watchers of 32-bit programs"
set "WD=%T%\watchdiag"
mkdir "%WD%" 2>nul
copy /y "%T%\bin\x86\crashtest.exe" "%WD%\" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%WD%"
call :say "--- a program that keeps running, then is ended (taskkill /f)"
start "" "%WD%\crashtest.exe" /stay
ping -n 6 127.0.0.1 >nul
call :procs "running"
taskkill /f /im crashtest.exe >nul 2>&1
ping -n 6 127.0.0.1 >nul
call :procs "5 s after it was ended"
taskkill /f /im vxwatch.exe >nul 2>&1
call :say "--- a program whose helper crashes"
start "" "%WD%\crashtest.exe" /helper
ping -n 3 127.0.0.1 >nul
call :procs "2 s"
ping -n 7 127.0.0.1 >nul
call :procs "8 s"
ping -n 23 127.0.0.1 >nul
call :procs "30 s"
ping -n 16 127.0.0.1 >nul
call :procs "45 s"
2>nul >COM1 echo SHOT watchdiag
ping -n 6 127.0.0.1 >nul
copy /y "%LOCALAPPDATA%\VistaExt\crashes.log" "%R%\crashes.log" >nul 2>&1
goto :finish

:procs
call :say "  processes (%~1):"
wmic process where "name='vxwatch.exe' or name='crashtest.exe' or name='WerFault.exe'" get ProcessId,ParentProcessId,CommandLine /format:list 2>nul | find "=" >> "%LOG%"
goto :eof

:diag
call :say "### diagnostics: vxload under vxdbg, VX_TRACE=1"
set "DG=%T%\diag"
mkdir "%DG%" 2>nul
copy /y "%B%\vxtest.exe" "%DG%\vxtest.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%DG%"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN11
set VX_TRACE=1
"%NF%" 120 "%B%\vxdbg.exe" 60 "%DG%\vxtest.exe" /expectall:10.0.22631 > "%R%\diag-vxtest.txt" 2>&1
set "DL=%USERPROFILE%\Downloads"
if not exist "%DL%" mkdir "%DL%"
copy /y "%B%\spawntest.exe" "%DL%\vxsetuptest.exe" >nul
ping -n 12 127.0.0.1 >nul
reg query "HKLM\SOFTWARE\VistaExt\Memory" > "%R%\diag-memory.txt" 2>&1
"%NF%" 120 "%B%\vxdbg.exe" 60 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe" > "%R%\diag-spawn.txt" 2>&1
set VX_TRACE=
reg query "HKLM\SOFTWARE\VistaExt\Memory" >> "%R%\diag-memory.txt" 2>&1
reg query "%IFEO%" /s > "%R%\diag-ifeo.txt" 2>&1
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\diag-VistaExt.log" >nul 2>&1
goto :finish

rem ------------------------------------------------------------- helpers
:run
rem run <seconds> <program> [arguments]: output to the log, exit code in RC
>>"%LOG%" 2>&1 "%NF%" %*
set "RC=%errorlevel%"
goto :eof

:say
>>"%LOG%" echo %~1
2>nul >COM1 echo %~1
echo %~1
goto :eof

:expect
rem expect <name> <wanted exit code> <exit code>
if "%~2"=="%~3" goto :pass
set /a FAIL+=1
call :say "FAIL: %~1 [exit %~3, wanted %~2]"
goto :eof

:nonzero
rem nonzero <name> <exit code>: the program must have failed
if not "%~2"=="0" goto :pass
goto :fail

:exists
if exist "%~2" goto :pass
goto :fail

:absent
if not exist "%~2" goto :pass
goto :fail

:has
findstr /l /c:"%~3" "%~2" >nul && goto :pass
goto :fail

:hasnot
findstr /l /c:"%~3" "%~2" >nul || goto :pass
goto :fail

:pass
set /a PASS+=1
call :say "PASS: %~1"
goto :eof

:fail
set /a FAIL+=1
call :say "FAIL: %~1"
goto :eof

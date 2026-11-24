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
rem VistaExt's diagnostics folder (src/common/diag.h)
set "DIAG=%ProgramData%\VistaExt Diagnostics"
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
if exist "%T%\only-upgrade.txt" goto :upgrade
if exist "%T%\only-mitig.txt" (
    call :say "### mitigtest (no VistaExt): x64/x86 as this Windows has them"
    "%B%\mitigtest.exe" >> "%LOG%" 2>&1
    if "%OSA%"=="x64" "%T%\bin\x86\mitigtest.exe" >> "%LOG%" 2>&1
    goto :finish
)

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
rem what a program finds on this Windows: compared again with VistaExt installed
"%B%\probe.exe" > "%R%\probe-before.txt" 2>&1

rem ---------------------------------------------------------------- install
call :say "### install.cmd"
cmd /c ""%T%\VistaExt\install.cmd"" < nul > "%R%\install.txt" 2>&1
call :expect "install.cmd succeeds" 0 !errorlevel!
type "%R%\install.txt" | find "file(s) added" >> "%LOG%"
call :exists "kernelvx.dll added to System32" "%SYS%\kernelvx.dll"
call :exists "uservx.dll added to System32" "%SYS%\uservx.dll"
call :exists "VistaExt's API sets added under its own names: vxs-ms-win-core-synch-l1-2-0" "%SYS%\vxs-ms-win-core-synch-l1-2-0.dll"
if "%OSA%"=="x64" call :exists "32-bit kernelvx.dll added to SysWOW64" "%WOW%\kernelvx.dll"
if "%OSA%"=="x64" call :exists "32-bit API sets added to SysWOW64 (vxs-)" "%WOW%\vxs-ms-win-core-synch-l1-2-0.dll"
call :say "### programs VistaExt does not run see exactly what Windows has"
rem no file with a Windows name that is VistaExt's, in either system folder
set "WN=0"
for %%d in ("%SYS%" "%WOW%") do if exist "%%~d" (
    for %%f in ("%%~d\api-ms-win-*.dll" "%%~d\ext-ms-win-*.dll" "%%~d\combase.dll" "%%~d\shcore.dll" "%%~d\d3d12.dll" "%%~d\kernelbase.dll" "%%~d\bcryptprimitives.dll") do (
        findstr /m /l /c:"VistaExt-owned-file" "%%~f" >nul 2>&1 && (set /a WN+=1 & >>"%LOG%" echo   VistaExt's, with a Windows name: %%~f)
    )
)
call :expect "no VistaExt file with a Windows name (api-ms-win-*, combase, shcore, d3d12, kernelbase, bcryptprimitives)" 0 !WN!
"%B%\probe.exe" > "%R%\probe-after.txt" 2>&1
call :expect "a program VistaExt does not run loads none of VistaExt's DLLs by a Windows name" 0 !errorlevel!
"%B%\probe.exe" /ldr api-ms-win-core-url-l1-1-0.dll >nul 2>&1
set "URLRC=!errorlevel!"
if "%CV%"=="6.2" (call :expect "Windows 8 has api-ms-win-core-url-l1-1-0 itself" 0 !URLRC!) else (call :expect "... nor, through Windows' loader, an API set this Windows lacks (api-ms-win-core-url-l1-1-0)" 1 !URLRC!)
fc "%R%\probe-before.txt" "%R%\probe-after.txt" >nul
call :expect "... and finds exactly the DLLs it found before VistaExt was installed" 0 !errorlevel!
if "%OSA%"=="x64" (
    "%T%\bin\x86\probe.exe" > "%R%\probe-after-x86.txt" 2>&1
    call :expect "the same for a 32-bit program" 0 !errorlevel!
)
call :exists "tools installed: vxpatch, msifix, VistaExt Center" "%VX%\vxcenter.exe"
set "SM=%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs"
call :startmenu
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt\command" >nul 2>&1
call :expect "Explorer right-click entry for programs" 0 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\Msi.Package\shell\VistaExtInstall\command" >nul 2>&1
call :expect "Explorer right-click entry for .msi packages" 0 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExtRun" /ve 2>nul | find "Open with VistaExt compatibility mode" >nul
call :expect "right-click a program: 'Open with VistaExt compatibility mode'" 0 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\lnkfile\shell\VistaExtRun\command" /ve 2>nul | find /i "vxcenter.exe" >nul
call :expect "right-click a shortcut: the same entry" 0 !errorlevel!
findstr /i /l /c:"System32\kernelbase.dll" "%VX%\installed.txt" >nul
set "KB=!errorlevel!"
if "%CV%"=="6.0" call :expect "Vista: no kernelbase.dll added (kernelvx stands in for it, for programs VistaExt runs)" 1 !KB!
if "%CV%"=="6.1" call :expect "Windows 7: its own kernelbase.dll left alone" 1 !KB!
if "%CV%"=="6.2" call :expect "Windows 8: its own kernelbase.dll left alone" 1 !KB!
rem one installer, four builds: it must pick this Windows' and architecture's
set "WANTB=win7-%OSA%"
if "%CV%"=="6.0" set "WANTB=vista-%OSA%"
if "%CV%"=="6.2" set "WANTB=win8-%OSA%"
if "%CV%"=="6.3" set "WANTB=win8-%OSA%"
reg query "HKLM\SOFTWARE\VistaExt" /v Build 2>nul | find "!WANTB!" >nul
call :expect "the installer picked the build for this Windows (!WANTB!)" 0 !errorlevel!
if "%CV%"=="6.0" call :exists "Vista build: VistaExt's bcryptprimitives as bcryptprimvx.dll" "%SYS%\bcryptprimvx.dll"
if "%CV%"=="6.0" call :absent "Vista build: no bcryptprimitives.dll (Vista has none; programs VistaExt does not run must not find one)" "%SYS%\bcryptprimitives.dll"
if "%CV%"=="6.1" call :exists "Windows 7 build: bcryptprimvx.dll added" "%SYS%\bcryptprimvx.dll"
if "%CV%"=="6.2" call :exists "Windows 8 build: bcryptprimvx.dll added" "%SYS%\bcryptprimvx.dll"
findstr /m /l /c:"VistaExt-owned-file" "%SYS%\kernelvx.dll" >nul 2>&1
call :expect "VistaExt's DLLs carry the ownership marker" 0 !errorlevel!
findstr /m /l /c:"VistaExt-owned-file" "%SYS%\kernel32.dll" >nul 2>&1
call :expect "Windows' own DLLs do not" 1 !errorlevel!

call :say "### the tray icon, its autostart and the defaults"
call :exists "the tray icon program is installed" "%VX%\vxtray.exe"
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v VistaExt 2>nul | find /i "vxtray.exe" >nul
call :expect "the tray icon starts with Windows (Run entry)" 0 !errorlevel!
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
rem (a contract none of Vista, 7 and 8.0 has: VistaExt's own file on each)
set "REPL=%SYS%\api-ms-win-core-winrt-string-l1-1-1.dll"
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
if exist "%T%\only-shellx.txt" goto :shellx
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
call :nativetests
call :diagtests
call :switchtests

rem ------------------------------------------------- Safe Mode (two restarts)
call :safe_prepare
:safe_done

rem ------------------------------------------------------- VistaExt Center
call :say "### VistaExt Center window (picture: center.png)"
>>"%LOG%" 2>&1 "%NF%" 60 "%B%\guishot.exe" "%VX%\vxcenter.exe" "%T%\%OSA%\app\vxtest.exe" "%R%\center.bmp" /onscreen
call :expect "VistaExt Center opens and lists the program's needs" 0 !errorlevel!

call :say "### right-click a program's shortcut: 'Open with VistaExt compatibility mode'"
set "LR=%T%\lnkrun"
mkdir "%LR%" 2>nul
copy /y "%B%\vxtest.exe" "%LR%\vxlnk.exe" >nul
>"%T%\mklnk.vbs" echo Set s = CreateObject("WScript.Shell").CreateShortcut(WScript.Arguments(0))
>>"%T%\mklnk.vbs" echo s.TargetPath = WScript.Arguments(1)
>>"%T%\mklnk.vbs" echo s.Arguments = "/wait:90"
>>"%T%\mklnk.vbs" echo s.WorkingDirectory = WScript.Arguments(2)
>>"%T%\mklnk.vbs" echo s.Save
cscript //nologo "%T%\mklnk.vbs" "%LR%\VxLnk.lnk" "%LR%\vxlnk.exe" "%LR%" >>"%LOG%" 2>&1
call :run 60 "%LR%\vxlnk.exe"
call :nonzero "before: the program cannot start without VistaExt's mode" !RC!
rem the command Explorer runs for the entry, %%1 = the shortcut
set "LCMD="
for /f "tokens=2*" %%a in ('reg query "HKLM\SOFTWARE\Classes\lnkfile\shell\VistaExtRun\command" /ve 2^>nul ^| find "REG_"') do set "LCMD=%%b"
set "LCMD=!LCMD:%%1=%LR%\VxLnk.lnk!"
>>"%LOG%" echo   command: !LCMD!
cmd /c "!LCMD!"
call :expect "the entry's command succeeds" 0 !errorlevel!
set "LUP="
for /l %%w in (1,1,15) do if not defined LUP (tasklist /fi "imagename eq vxlnk.exe" 2>nul | find /i "vxlnk.exe" >nul && set "LUP=1" || ping -n 2 127.0.0.1 >nul)
if defined LUP (call :pass "the shortcut's program starts with VistaExt and its own arguments (/wait keeps it open)") else (call :fail "the shortcut's program starts with VistaExt and its own arguments")
taskkill /f /im vxlnk.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
call :run 60 "%LR%\vxlnk.exe"
call :expect "after: a plain double-click runs it too (it stays set up)" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%LR%"

rem -------------------------------------------------------------- uninstall
call :say "### uninstall.cmd while a patched program is running"
set "BUSY=%T%\busy"
mkdir "%BUSY%" 2>nul
rem the test program: it needs VistaExt on Vista, 7 and 8 alike (/wait keeps it open)
copy /y "%B%\vxtest.exe" "%BUSY%\vxbusy.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /file /q "%BUSY%\vxbusy.exe"
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
call :absent "VistaExt's API sets removed (vxs-)" "%SYS%\vxs-ms-win-core-synch-l1-2-0.dll"
if "%OSA%"=="x64" call :absent "32-bit kernelvx.dll removed from SysWOW64" "%WOW%\kernelvx.dll"
if "%CV%"=="6.0" call :absent "Vista: added kernelbase.dll removed" "%SYS%\kernelbase.dll"
if "%CV%"=="6.1" call :exists "Windows 7: its own kernelbase.dll still there" "%SYS%\kernelbase.dll"
call :absent "VistaExt folder removed" "%VX%"
if exist "%VX%" (>>"%LOG%" echo   left in %VX%:& dir /b /a "%VX%" >>"%LOG%" 2>&1)
reg query "HKLM\SOFTWARE\VistaExt" >nul 2>&1
call :expect "VistaExt settings removed" 1 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt" >nul 2>&1
call :expect "Explorer entries removed" 1 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\lnkfile\shell\VistaExtRun" >nul 2>&1
call :expect "... also the shortcut entry" 1 !errorlevel!
fc /b "%APP%" "%T%\bin\%OSA%\vxtest.exe" >nul
call :expect "uninstall puts patched programs back" 0 !errorlevel!
fc /b "%BUSY%\vxbusy.exe" "%B%\vxtest.exe" >nul
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
call :absent "Start menu: the VistaExt folder is removed" "%SM%\VistaExt"
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
set "W=%T%\%A%\api"
mkdir "%W%" 2>nul
copy /y "%T%\bin\%A%\apitest.exe" "%W%\apitest.exe" >nul
call :say "### %A%: program importing through API sets"
call :run 60 "%W%\apitest.exe"
call :nonzero "%A% unchanged it cannot start, exactly as without VistaExt (VistaExt's API sets are only for programs it runs)" !RC!
call :run 120 "%VX%\vxpatch.exe" /memory /q "%W%\apitest.exe"
call :run 60 "%W%\apitest.exe"
call :expect "%A% in memory: it gets VistaExt's API sets, and runs" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%W%\apitest.exe"
call :run 60 "%W%\apitest.exe"
call :nonzero "%A% undone: as before" !RC!
call :run 120 "%VX%\vxpatch.exe" /file "%W%\apitest.exe"
call :expect "%A% file mode: vxpatch points the API-set imports at VistaExt" 0 !RC!
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
rem API sets: the import goes to VistaExt's vxs- copy (no api-ms file of
rem VistaExt's is in the system folder; Windows' API-set table ignores vxs-)
call :run 60 "%W%\apitest.exe"
call :expect "%A% memory: API-set imports go to VistaExt's own copy" 0 !RC!
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
rem setups that run as they are are left alone by default; these tests turn
rem the tray icon's "setup programs are told it too" on, and off again at the end
call :run 60 "%VX%\vxauto.exe" /pipe SETUPS 1
set "DL=%USERPROFILE%\Downloads"
if not exist "%DL%" mkdir "%DL%"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN10
copy /y "%B%\spawntest.exe" "%DL%\vxsetuptest.exe" >nul
copy /y "%B%\vxbench-native.exe" "%DL%\vxplain.exe" >nul
copy /y "%B%\vxtest.exe" "%DL%\notepad.exe" >nul
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
for /l %%w in (1,1,20) do (reg query "%IFEO%\vxver2-setup.exe" /v VistaExt >nul 2>&1 || ping -n 2 127.0.0.1 >nul)
call :run 60 "%DL%\vxver2-setup.exe"
call :expect "with 'setup programs are told it too' off (the default), such a setup is only watched: it sees this Windows' real version (it refuses, as without VistaExt)" 1 !RC!
call :say "### a setup that runs by itself and unpacks a helper that does not (Discord's on Vista)"
copy /y "%B%\spawnnative.exe" "%DL%\vxnative-setup.exe" >nul
for /l %%w in (1,1,20) do (reg query "%IFEO%\vxnative-setup.exe" /v VistaExt >nul 2>&1 || ping -n 2 127.0.0.1 >nul)
ping -n 3 127.0.0.1 >nul
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%DL%\vxnative-setup.exe" 2>nul | find "watch" >nul
call :expect "the service registers it only to watch what it starts" 0 !errorlevel!
call :run 60 "%DL%\vxnative-setup.exe"
call :expect "... and it runs unchanged" 2 !RC!
call :run 90 "%DL%\vxnative-setup.exe" "%B%\vxtest.exe" /squirrel
call :expect "the helper it unpacks into AppData\Local and starts at once runs too" 0 !RC!
call :run 90 "%DL%\vxnative-setup.exe" "%B%\probe.exe" /squirrel
call :expect "... and a helper that runs by itself is left alone (it finds none of VistaExt's DLLs)" 0 !RC!
del /f /q "%DL%\vxnative-setup.exe" >nul 2>&1
rd /s /q "%LOCALAPPDATA%\VxSquirrelTemp" >nul 2>&1
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
copy /y "%B%\vxtest.exe" "%SW%\busy\vxbusy2.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%SW%\mem"
call :run 120 "%VX%\vxpatch.exe" /file /q "%SW%\file\vxfile.exe"
call :run 120 "%VX%\vxpatch.exe" /file /q "%SW%\busy\vxbusy2.exe"
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
reg query "HKLM\SOFTWARE\Classes\lnkfile\shell\VistaExtRun" >nul 2>&1
call :expect "off: ... also on shortcuts" 1 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\Msi.Package\shell\Open\command" /ve 2>nul | find /i "msifix" >nul
call :expect "off: double-clicking an .msi is Windows' own again" 1 !errorlevel!
tasklist /fi "imagename eq vxbusy2.exe" 2>nul | find /i "vxbusy2.exe" >nul
call :expect "off: a changed program that was running keeps running" 0 !errorlevel!
"%VX%\vxauto.exe" /pipe STATUS 2>nul | find "waiting=1" >nul
call :expect "off: ... and is put back once it is closed (waiting)" 0 !errorlevel!
taskkill /f /im vxbusy2.exe >nul 2>&1
for /l %%w in (1,1,30) do (fc /b "%SW%\busy\vxbusy2.exe" "%B%\vxtest.exe" >nul 2>&1 || ping -n 3 127.0.0.1 >nul)
fc /b "%SW%\busy\vxbusy2.exe" "%B%\vxtest.exe" >nul
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
reg query "HKLM\SOFTWARE\Classes\lnkfile\shell\VistaExtRun" >nul 2>&1
call :expect "on: ... also on shortcuts" 0 !errorlevel!
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
call :expect "Safe Mode: turning VistaExt off (the VistaExt window's switch) works without the service" 0 !RC!
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
set "REPL=%SYS%\api-ms-win-core-winrt-string-l1-1-1.dll"
set "OWNSYS="
if exist "%SYS%\api-ms-win-core-synch-l1-2-0.dll" findstr /m /l /c:"VistaExt-owned-file" "%SYS%\api-ms-win-core-synch-l1-2-0.dll" >nul 2>&1 || set "OWNSYS=1"
goto :safe_done

rem ---- uninstalling with VistaExtSetup.exe while a program VistaExt runs is open
:uninst
call :say "### uninstall (VistaExtSetup.exe) while a program VistaExt runs is open"
set "UN=%T%\uninst"
mkdir "%UN%" 2>nul
copy /y "%B%\vxtest.exe" "%UN%\vxbusy3.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%UN%"
start "" "%UN%\vxbusy3.exe" /wait:600
ping -n 5 127.0.0.1 >nul
tasklist /fi "imagename eq vxbusy3.exe" 2>nul | find /i "vxbusy3.exe" >nul
call :expect "a program VistaExt runs is open" 0 !errorlevel!
copy /y "%T%\VistaExtSetup.exe" "%VX%\VistaExtSetup.exe" >nul
start "" /wait "%VX%\VistaExtSetup.exe" /uninstall /quiet
call :wait_uninstaller
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\uninst-setup.log" >nul 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager" /v PendingFileRenameOperations > "%R%\uninst-pending.txt" 2>&1
rem (switched off first: the file is in VistaExt.off by then)
findstr /i /r /c:"System32\\kernelvx\.dll" /c:"System32\\VistaExt\.off\\kernelvx\.dll" "%R%\uninst-pending.txt" >nul
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
dir /b "%SYS%\api-ms-win-core-winrt-string-l1-1-1.dll" "%SYS%\vxs-*" > "%T%\left.txt" 2>nul
for /f %%c in ('find /c /v "" ^< "%T%\left.txt"') do call :say "    VistaExt API-set files left: %%c"
call :absent "VistaExt's folder in Program Files is gone" "%VX%"
call :absent "the log folder is gone" "%ProgramData%\VistaExt"
call :absent "no VistaExt.off folder is left" "%SYS%\VistaExt.off"
if "%OSA%"=="x64" call :absent "no VistaExt.off folder is left in SysWOW64" "%WOW%\VistaExt.off"
call :absent "no old copy an update set aside is left" "%SYS%\*.vxold*"
goto :finish

rem ---- a program that runs on this Windows by itself is never changed
:nativetests
call :say "### a program VistaExt runs without sandbox asks for Windows 10's process protections for its child"
set "MT=%T%\vxmitig"
mkdir "%MT%" 2>nul
copy /y "%B%\mitigtest.exe" "%MT%\vxmitig.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q /winver:WIN10 "%MT%\vxmitig.exe"
call :run 60 "%MT%\vxmitig.exe" /w10 --no-sandbox
call :expect "the child starts (the protections this Windows knows are kept, the others left out)" 0 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%MT%\vxmitig.exe"
rd /s /q "%MT%" >nul 2>&1
call :say "### a program VistaExt runs: Windows' own DLLs in it find the API sets the told Windows has; its children not"
set "VP=%T%\vxpathtest"
mkdir "%VP%" 2>nul
copy /y "%B%\probe.exe" "%VP%\vxpprobe.exe" >nul
copy /y "%B%\probe.exe" "%VP%\vxpchild.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q /winver:WIN10 "%VP%\vxpprobe.exe"
rem (Windows 10's processthreads-l1-1-3, which neither 7 nor 8 has; checked
rem in their apisetschema.dll)
call :run 60 "%VP%\vxpprobe.exe" /child api-ms-win-core-processthreads-l1-1-3.dll
rem Windows 7 and 8: VistaExt adds its API sets to the program's own API-set
rem map (Vista has none, and its own DLLs use no API sets)
if "%CV%"=="6.0" (call :expect "Vista: neither (Vista has no API-set map)" 1 !RC!) else call :expect "the program loads an API set Windows lacks through Windows' loader; a program it starts does not" 11 !RC!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%VP%\vxpprobe.exe"
rd /s /q "%VP%" >nul 2>&1
call :say "### a small starter that loads its big main DLL by name (firefox.exe and xul.dll)"
set "NM=%SystemDrive%\VxNamedTest"
mkdir "%NM%\1.0" 2>nul
rem the starter: a program that runs by itself, with the DLL's name in it
> "%NM%\name.txt" echo vxbigcore.dll
copy /y /b "%B%\probe.exe" + "%NM%\name.txt" "%NM%\vxstarter.exe" >nul
rem its main code, larger than it, in a version folder, needing functions this Windows lacks
copy /y /b "%B%\memdll.dll" + "%B%\probe.exe" + "%B%\probe.exe" + "%B%\probe.exe" "%NM%\1.0\vxbigcore.dll" >nul
call :run 60 "%VX%\vxpatch.exe" /native "%NM%\vxstarter.exe"
call :expect "the starter cannot run by itself: its main DLL cannot load" 1 !RC!
rem the same DLL smaller than the starter: an optional part (Chromium's dxcompiler.dll), not counted
copy /y /b "%B%\probe.exe" + "%B%\probe.exe" + "%B%\probe.exe" + "%B%\probe.exe" + "%B%\probe.exe" + "%NM%\name.txt" "%NM%\vxstarter.exe" >nul
copy /y /b "%B%\memdll.dll" "%NM%\1.0\vxbigcore.dll" >nul
call :run 60 "%VX%\vxpatch.exe" /native "%NM%\vxstarter.exe"
call :expect "... a smaller optional DLL it names does not make it need VistaExt" 0 !RC!
rd /s /q "%NM%" >nul 2>&1
call :say "### the service leaves a program that runs by itself alone, whatever else is in its folder"
set "NP=%ProgramFiles%\VxNativeTest"
set "NN=%ProgramFiles%\VxNeedsTest"
mkdir "%NP%" "%NN%" 2>nul
rem a program that runs as it is, next to a DLL it does not load at start
rem that needs functions this Windows lacks (an optional part)
copy /y "%B%\probe.exe" "%NP%\vxnative.exe" >nul
copy /y "%B%\memdll.dll" "%NP%\optional.dll" >nul
rem a program that loads such a DLL at start: it cannot run by itself
copy /y "%B%\memhost.exe" "%NN%\vxneeds.exe" >nul
copy /y "%B%\memdll.dll" "%NN%\memdll.dll" >nul
copy /y "%B%\memdll.dll" "%NN%\memdll2.dll" >nul
rem (memdll imports WaitOnAddress from kernel32: even Windows 8 has it only in
rem kernelbase, so on every Windows it cannot start by itself)
for /l %%w in (1,1,40) do (find /i "VxNeedsTest" "%DIAG%\decisions.log" >nul 2>&1 || ping -n 3 127.0.0.1 >nul)
for /l %%w in (1,1,20) do (find /i "VxNativeTest" "%DIAG%\decisions.log" >nul 2>&1 || ping -n 3 127.0.0.1 >nul)
ping -n 6 127.0.0.1 >nul
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%NN%\vxneeds.exe" >nul 2>&1
call :expect "a program whose DLL needs VistaExt at start is set up" 0 !errorlevel!
call :run 60 "%NN%\vxneeds.exe"
call :expect "... and runs" 0 !RC!
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%NP%\vxnative.exe" >nul 2>&1
call :expect "a program that runs by itself is not set up, although a DLL next to it needs VistaExt" 1 !errorlevel!
reg query "%IFEO%\vxnative.exe" >nul 2>&1
call :expect "... no Image File Execution Options entry for it" 1 !errorlevel!
reg query "%LAYERS%" /v "%NP%\\" >nul 2>&1
call :expect "... and no settings saved for its folder" 1 !errorlevel!
call :run 60 "%NP%\vxnative.exe"
call :expect "... it runs exactly as without VistaExt (none of VistaExt's DLLs)" 0 !RC!
find /i "VxNativeTest: its programs run on this Windows by themselves" "%DIAG%\decisions.log" >nul 2>&1
call :expect "... and the diagnostics folder's decisions.log says why" 0 !errorlevel!
find /i "vxneeds.exe: cannot start on this Windows by itself" "%DIAG%\decisions.log" >nul 2>&1
call :expect "decisions.log says why the other one was set up" 0 !errorlevel!
call :say "### looking again: a program an earlier version set up that runs by itself is put back"
copy /y "%B%\probe.exe" "%T%\vxreview.exe" >nul
reg add "HKLM\SOFTWARE\VistaExt\Memory" /v "%T%\vxreview.exe" /d "" /f >nul
mkdir "%T%\vxuserdir" 2>nul
copy /y "%B%\probe.exe" "%T%\vxuserdir\vxuser.exe" >nul
rem (a version setting makes vxpatch register it although nothing is missing)
call :run 120 "%VX%\vxpatch.exe" /memory /q /winver:WIN10 "%T%\vxuserdir\vxuser.exe"
call :run 300 "%VX%\vxauto.exe" /review
call :expect "vxauto /review succeeds" 0 !RC!
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%T%\vxreview.exe" >nul 2>&1
call :expect "an automatic registration of a program that runs by itself is taken back" 1 !errorlevel!
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%T%\vxuserdir\vxuser.exe" 2>nul | find "user" >nul
call :expect "one the user chose (vxpatch, right-click) stays" 0 !errorlevel!
call :run 120 "%VX%\vxpatch.exe" /undo /q "%T%\vxuserdir\vxuser.exe"
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%NN%\vxneeds.exe" >nul 2>&1
call :expect "one that cannot run by itself stays" 0 !errorlevel!
rd /s /q "%NP%" >nul 2>&1
rd /s /q "%NN%" >nul 2>&1
del /f /q "%T%\vxreview.exe" >nul 2>&1
rd /s /q "%T%\vxuserdir" >nul 2>&1
goto :eof

rem ---- the diagnostics folder: errors are written there as they happen
:diagtests
call :say "### diagnostics folder (%DIAG%)"
call :exists "it exists, with its README" "%DIAG%\README.txt"
for /f "delims=" %%a in ('"%VX%\vxauto.exe" /diagdir 2^>nul') do set "DD=%%a"
if /i "!DD!"=="%DIAG%" (call :pass "vxauto /diagdir names it") else (call :fail "vxauto /diagdir names it [!DD!]")
rem only administrators may write there (a folder every user could write to
rem would let one plant a link the service writes through)
icacls "%DIAG%" > "%R%\diag-acl.txt" 2>&1
find /i "BUILTIN\Users:(OI)(CI)(RX)" "%R%\diag-acl.txt" >nul
call :expect "every user may read it" 0 !errorlevel!
findstr /i /c:"Users:(OI)(CI)(M)" /c:"Users:(OI)(CI)(F)" /c:"Users:(OI)(CI)(W)" /c:"Users:(M)" /c:"Users:(F)" /c:"Users:(W)" /c:"Users:(CI)(AD)" /c:"Users:(CI)(WD)" "%R%\diag-acl.txt" >nul
call :expect "... and no user may change it (nothing inherited from ProgramData)" 1 !errorlevel!
rem a crash of a program VistaExt does not run: Windows reports it to its
rem event log (no "stopped working" window in this test VM), the service
rem writes it to events.log
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting" /v DontShowUI /t REG_DWORD /d 1 /f >nul
copy /y "%B%\probe.exe" "%T%\vxcrashme.exe" >nul
start "" /wait "%T%\vxcrashme.exe" /crash
for /l %%w in (1,1,30) do (find /i "vxcrashme.exe" "%DIAG%\events.log" >nul 2>&1 || ping -n 2 127.0.0.1 >nul)
find /i "vxcrashme.exe" "%DIAG%\events.log" >nul 2>&1
call :expect "a crash of any program is written to events.log" 0 !errorlevel!
find /i "not a program VistaExt runs" "%DIAG%\events.log" >nul 2>&1
call :expect "... saying that VistaExt does not run it" 0 !errorlevel!
call :exists "crashes of programs VistaExt runs are in the user's crash-details.log" "%LOCALAPPDATA%\VistaExt\crash-details.log"
call :say "### Save diagnostics (vx-diagnostics.cmd)"
cmd /c ""%VX%\vx-diagnostics.cmd" /quiet" > "%R%\diag-run.txt" 2>&1
call :expect "it runs" 0 !errorlevel!
set "REP="
for /d %%d in ("%DIAG%\report-*") do set "REP=%%~d"
if defined REP (call :pass "a report folder is made (!REP!)") else (call :fail "a report folder is made")
if defined REP (
    for %%f in (system.txt vistaext.txt programs.txt files.txt processes.txt events-application.txt events-system.txt) do call :exists "report: %%f" "!REP!\%%f"
    set "CDL="
    for %%f in ("!REP!\logs\crash-details-*.log") do set "CDL=%%~f"
    if defined CDL (call :pass "report: each user's crash details are copied") else (call :fail "report: each user's crash details are copied")
    find /i "without VistaExt:" "!REP!\programs.txt" >nul 2>&1
    call :expect "report: programs.txt says for each program whether it would start without VistaExt" 0 !errorlevel!
    xcopy "%DIAG%" "%R%\diagnostics\" /e /i /q /y >nul 2>&1
)
del /f /q "%T%\vxcrashme.exe" >nul 2>&1
goto :eof

:shellx
rem ---- (VX_VM_ONLY=shellx) what makes ShellExecuteEx of a setup's helper fail
call :say "### experiment: ShellExecuteEx of a helper from a setup VistaExt runs"
set "DL=%USERPROFILE%\Downloads"
if not exist "%DL%" mkdir "%DL%"
call :run 60 "%VX%\vxauto.exe" /pipe SETUPS 1
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN10
copy /y "%B%\spawntest.exe" "%DL%\vxsetuptest.exe" >nul
for /l %%w in (1,1,20) do (reg query "%IFEO%\vxsetuptest.exe" /v VistaExt >nul 2>&1 || ping -n 2 127.0.0.1 >nul)
ping -n 6 127.0.0.1 >nul
call :say "--- A: helper needs VistaExt, CreateProcess"
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe"
call :say "--- B: helper needs VistaExt, ShellExecute"
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe" /shell
call :say "--- C: helper runs by itself (probe), ShellExecute"
call :run 90 "%DL%\vxsetuptest.exe" "%B%\probe.exe" /shell
call :say "--- D: B again, setup told this Windows' own version"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER REAL
call :run 90 "%DL%\vxsetuptest.exe" "%B%\vxtest.exe" /shell
call :say "--- E: B with the service stopped (no registration of the helper)"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN10
net stop VistaExtAuto >nul 2>&1
call :run 90 "%DL%\vxsetuptest.exe" "%B%\probe.exe" /shell
net start VistaExtAuto >nul 2>&1
call :say "--- F: plain cmd START of a registered helper copy"
copy /y "%B%\vxtest.exe" "%TEMP%\vxchild-manual.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%TEMP%\vxchild-manual.exe"
start "" /wait "%TEMP%\vxchild-manual.exe"
call :say "    START exit !errorlevel!"
call :say "--- H: B under the debugger with the loader's messages (GlobalFlag 0x102)"
set "SIF=%IFEO%\vxsetuptest.exe"
reg query "%SIF%" > "%R%\shellx-sif.txt" 2>&1
reg add "%SIF%" /v GlobalFlag /t REG_DWORD /d 0x102 /f >nul 2>&1
"%NF%" 120 "%B%\vxdbg.exe" 60 "%DL%\vxsetuptest.exe" "%B%\probe.exe" /shell > "%R%\shellx-dbg.txt" 2>&1
reg add "%SIF%" /v GlobalFlag /t REG_DWORD /d 0x100 /f >nul 2>&1
call :say "--- G: the setup not registered (unchanged spawntest), probe helper, ShellExecute"
call :run 120 "%VX%\vxpatch.exe" /undo /q "%DL%\vxsetuptest.exe"
reg add "HKLM\SOFTWARE\VistaExt\Layers" /v "%DL%\vxsetuptest.exe" /d OFF /f >nul
copy /y "%B%\vxtest.exe" "%DL%\vxplainhost.exe" >nul
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\shellx-VistaExt.log" >nul 2>&1
reg query "%IFEO%" /s > "%R%\shellx-ifeo.txt" 2>&1
goto :finish

rem ---- the Start menu: one folder "VistaExt" with exactly two entries
:startmenu
call :exists "Start menu: VistaExt (the window with the on/off switch)" "%SM%\VistaExt\VistaExt.lnk"
call :exists "Start menu: Uninstall VistaExt" "%SM%\VistaExt\Uninstall VistaExt.lnk"
set "NLNK=0"
for %%f in ("%SM%\VistaExt\*") do set /a NLNK+=1
call :expect "Start menu: nothing else in the VistaExt folder" 2 !NLNK!
call :absent "Start menu: no shortcut of an earlier version (VistaExt Center)" "%SM%\VistaExt Center.lnk"
call :absent "Start menu: ... (tray icon)" "%SM%\VistaExt tray icon.lnk"
call :absent "Start menu: ... (Turn off)" "%SM%\Turn off VistaExt (also in Safe Mode).lnk"
goto :eof

rem ---- updating (VX_VM_ONLY=upgrade): an earlier release installed, programs
rem ---- made compatible with it and one of them running, then this release
rem ---- over it (also once while switched off), then the new uninstaller
:upgrade
set "SM=%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs"
set "ARPK=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt"
set "OSB=win7"
if "%CV%"=="6.0" set "OSB=vista"
if "%CV%"=="6.2" set "OSB=win8"
if "%CV%"=="6.3" set "OSB=win8"
set "NEWK=%T%\VistaExt\!OSB!\%OSA%\kernelvx.dll"
set "UP=%T%\upgrade"
mkdir "%UP%\file" "%UP%\mem" "%UP%\busy" 2>nul
call :say "### update: the earlier release installed and used"
start "" /wait "%T%\VistaExtSetup-old.exe" /quiet
call :expect "the earlier release installs" 0 !errorlevel!
set "OLDV=?"
for /f "tokens=2*" %%a in ('reg query "%ARPK%" /v DisplayVersion 2^>nul ^| find "REG_"') do set "OLDV=%%b"
call :say "    installed: VistaExt !OLDV!"
copy /y "%B%\vxtest.exe" "%UP%\file\vxup.exe" >nul
copy /y "%B%\vxtest.exe" "%UP%\mem\vxupmem.exe" >nul
copy /y "%B%\vxtest.exe" "%UP%\busy\vxupbusy.exe" >nul
call :run 120 "%VX%\vxpatch.exe" /file /q "%UP%\file\vxup.exe"
call :run 120 "%VX%\vxpatch.exe" /memory /q "%UP%\mem"
call :run 120 "%VX%\vxpatch.exe" /file /q "%UP%\busy\vxupbusy.exe"
call :run 60 "%UP%\file\vxup.exe"
rem (the test program's own checks are this release's: against the earlier
rem release's DLLs some fail, so here only that it starts and runs counts -
rem its exit code is then the number of failed checks, not a loader error)
call :ran "earlier release: a patched program starts and runs" !RC!
call :run 60 "%UP%\mem\vxupmem.exe"
call :ran "earlier release: an in-memory program starts and runs" !RC!
call :run 60 "%VX%\vxauto.exe" /setver WIN81
rem what earlier versions did: a program that runs by itself set up automatically
copy /y "%B%\probe.exe" "%UP%\vxupnative.exe" >nul
reg add "HKLM\SOFTWARE\VistaExt\Memory" /v "%UP%\vxupnative.exe" /d "" /f >nul
start "" "%UP%\busy\vxupbusy.exe" /wait:900
ping -n 5 127.0.0.1 >nul

call :say "### update: the new setup sees the installed version (picture: setup-existing.png)"
rem this release's version, from its install.cmd ("1.2.0")
set "NEWV=?"
for /f "tokens=7" %%a in ('findstr /c:"/v DisplayVersion /d" "%T%\VistaExt\install.cmd"') do set "NEWV=%%~a"
call :say "    this release: VistaExt !NEWV!"
set "WANTUPD=Update VistaExt to !NEWV!"
echo !OLDV!| find "!NEWV!" >nul && set "WANTUPD=Reinstall VistaExt !NEWV!"
>>"%LOG%" 2>&1 "%NF%" 60 "%B%\guishot.exe" "%T%\VistaExtSetup.exe" - "%R%\setup-existing.bmp" /onscreen /class:VistaExtSetup
call :expect "the setup window opens" 0 !errorlevel!
call :has "it offers: !WANTUPD!" "%LOG%" "!WANTUPD!"
call :has "it offers: Uninstall VistaExt" "%LOG%" "ninstall VistaExt"

call :say "### update to this release while a program patched by the earlier one runs"
start "" /wait "%T%\VistaExtSetup.exe" /quiet
call :expect "the update succeeds" 0 !errorlevel!
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\upgrade-setup.log" >nul 2>&1
reg query "%ARPK%" /v DisplayVersion 2>nul | find "!NEWV!" >nul
call :expect "Programs and Features shows the new version" 0 !errorlevel!
fc /b "%SYS%\kernelvx.dll" "!NEWK!" >nul
call :expect "the new kernelvx.dll is in place" 0 !errorlevel!
call :has "the file the running program had loaded was set aside, not left old" "%R%\upgrade-setup.log" "old copy deleted at restart"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager" /v PendingFileRenameOperations 2>nul | find /i ".vxold" >nul
call :expect "... and that old copy is deleted when Windows restarts" 0 !errorlevel!
call :hasnot "no file failed" "%R%\upgrade-setup.log" "could not"
tasklist /fi "imagename eq vxupbusy.exe" 2>nul | find /i "vxupbusy.exe" >nul
call :expect "the running program kept running" 0 !errorlevel!
set "WN=0"
for %%d in ("%SYS%" "%WOW%") do if exist "%%~d" (
    for %%f in ("%%~d\api-ms-win-*.dll" "%%~d\ext-ms-win-*.dll" "%%~d\combase.dll" "%%~d\shcore.dll" "%%~d\d3d12.dll" "%%~d\kernelbase.dll" "%%~d\bcryptprimitives.dll") do (
        findstr /m /l /c:"VistaExt-owned-file" "%%~f" >nul 2>&1 && (set /a WN+=1 & >>"%LOG%" echo   still there: %%~f)
    )
)
call :expect "the update removed the earlier version's files with Windows names (or set them aside for the restart)" 0 !WN!
"%B%\probe.exe" > "%R%\probe-upgraded.txt" 2>&1
call :expect "a program VistaExt does not run loads none of VistaExt's DLLs by a Windows name" 0 !errorlevel!
reg query "HKLM\SOFTWARE\VistaExt\Memory" /v "%UP%\vxupnative.exe" >nul 2>&1
call :expect "the earlier automatic set-up of a program that runs by itself is taken back" 1 !errorlevel!
reg query "HKLM\SOFTWARE\VistaExt" /v Version 2>nul | find "WIN81" >nul
call :expect "settings kept: the Windows version programs are told" 0 !errorlevel!
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x1" >nul
call :expect "VistaExt is still on" 0 !errorlevel!
call :startmenu
call :run 60 "%UP%\file\vxup.exe"
call :expect "a program the earlier release patched still runs" 0 !RC!
call :run 60 "%UP%\mem\vxupmem.exe"
call :expect "an in-memory program the earlier release registered still runs" 0 !RC!
for /l %%w in (1,1,20) do (sc query VistaExtAuto 2>nul | find "RUNNING" >nul || ping -n 2 127.0.0.1 >nul)
sc query VistaExtAuto 2>nul | find "RUNNING" >nul
call :expect "the service runs again" 0 !errorlevel!
fc /b "%VX%\VistaExtSetup.exe" "%T%\VistaExtSetup.exe" >nul
call :expect "the uninstaller is the new one" 0 !errorlevel!
taskkill /f /im vxupbusy.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
call :run 60 "%UP%\busy\vxupbusy.exe"
call :expect "the program that was running starts again with the new version" 0 !RC!

call :say "### update while VistaExt is switched off: it stays off"
call :run 180 "%VX%\vxauto.exe" /disable
call :absent "off: kernelvx.dll is out of System32" "%SYS%\kernelvx.dll"
start "" /wait "%T%\VistaExtSetup.exe" /quiet
call :expect "setup over a switched-off VistaExt succeeds" 0 !errorlevel!
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\upgrade-off-setup.log" >nul 2>&1
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x0" >nul
call :expect "VistaExt stays off" 0 !errorlevel!
call :absent "... its files stay out of System32" "%SYS%\kernelvx.dll"
fc /b "%SYS%\VistaExt.off\kernelvx.dll" "!NEWK!" >nul
call :expect "... the new ones wait in VistaExt.off" 0 !errorlevel!
reg query "HKLM\SOFTWARE\Classes\exefile\shell\VistaExt" >nul 2>&1
call :expect "... no Explorer entries while off" 1 !errorlevel!
call :run 60 "%UP%\file\vxup.exe"
call :nonzero "... a program that needs VistaExt is refused, as without it" !RC!
call :run 180 "%VX%\vxauto.exe" /enable
call :expect "turned on again" 0 !RC!
fc /b "%SYS%\kernelvx.dll" "!NEWK!" >nul
call :expect "... the new kernelvx.dll is back in System32" 0 !errorlevel!
call :run 60 "%UP%\file\vxup.exe"
call :expect "... the patched program runs again" 0 !RC!
call :run 60 "%UP%\mem\vxupmem.exe"
call :expect "... the in-memory program runs again" 0 !RC!

call :say "### Uninstall VistaExt (Start menu entry): restore point, off, removed"
set "LNKT="
for /f "delims=" %%a in ('findstr /m /l /c:"VistaExtSetup.exe" "%SM%\VistaExt\Uninstall VistaExt.lnk"') do set "LNKT=1"
if defined LNKT (call :pass "the Uninstall entry starts VistaExtSetup.exe") else (call :fail "the Uninstall entry starts VistaExtSetup.exe")
net stop WSearch >nul 2>&1
taskkill /f /im SearchFilterHost.exe >nul 2>&1
taskkill /f /im SearchProtocolHost.exe >nul 2>&1
start "" /wait "%VX%\VistaExtSetup.exe" /uninstall /quiet /restorepoint
call :wait_uninstaller
copy /y "%ProgramData%\VistaExt\setup.log" "%R%\upgrade-uninstall.log" >nul 2>&1
call :has "it turned VistaExt off first" "%R%\upgrade-uninstall.log" "Switching VistaExt off"
fc /b "%UP%\file\vxup.exe" "%B%\vxtest.exe" >nul
call :expect "the patched program is put back as it was" 0 !errorlevel!
call :exists "... and stays installed" "%UP%\file\vxup.exe"
reg query "%IFEO%\vxupmem.exe" /v VistaExt >nul 2>&1
call :expect "the in-memory registration is removed" 1 !errorlevel!
call :absent "kernelvx.dll is removed" "%SYS%\kernelvx.dll"
if exist "%SYS%\VistaExt.off" (
    >>"%LOG%" echo   in VistaExt.off until the restart:
    dir /a "%SYS%\VistaExt.off" >>"%LOG%" 2>&1
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager" /v PendingFileRenameOperations >>"%LOG%" 2>&1
    tasklist /m kernelvx.dll >>"%LOG%" 2>&1
)
rem (a file a running Windows process has loaded goes at the restart, with
rem the folder: checked after the restart)
if exist "%VX%" (>>"%LOG%" echo   in %VX% until the restart:& dir /b /a "%VX%" >>"%LOG%" 2>&1)
call :absent "the Start menu folder is removed" "%SM%\VistaExt"
reg query "HKLM\SOFTWARE\VistaExt" >nul 2>&1
call :expect "settings are removed" 1 !errorlevel!
reg query "%ARPK%" >nul 2>&1
call :expect "the Programs and Features entry is removed" 1 !errorlevel!
wmic /namespace:\\root\default path SystemRestore get Description 2>nul | find "Before removing VistaExt" >nul
if errorlevel 1 (call :say "    info: no restore point listed (System Restore may be off in this VM)") else (call :say "    info: restore point 'Before removing VistaExt' created")
rem what was in use goes at the restart: restart and check (as :uninst_after)
> "%SAFESTATE%" echo !PASS! !FAIL!
> "%SystemDrive%\vxt-uninst.after" echo after
call :say "    restarting"
shutdown /r /t 3 /f
exit

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
copy /y "%B%\memhost.exe" "%DG%\memhost.exe" >nul
copy /y "%B%\memdll.dll" "%DG%\memdll.dll" >nul
copy /y "%B%\memdll.dll" "%DG%\memdll2.dll" >nul
call :run 120 "%VX%\vxpatch.exe" /memory /q "%DG%"
call :run 60 "%VX%\vxauto.exe" /pipe SETVER WIN11
set VX_TRACE=1
"%NF%" 120 "%B%\vxdbg.exe" 60 "%DG%\vxtest.exe" /expectall:10.0.22631 > "%R%\diag-vxtest.txt" 2>&1
rem a program with a DLL it loads at start and one it loads later, with the
rem loader's own messages (GlobalFlag 2, added to VistaExt's 0x100)
set "MIF=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\memhost.exe"
reg query "%MIF%" > "%R%\diag-memhost-ifeo.txt" 2>&1
reg add "%MIF%" /v GlobalFlag /t REG_DWORD /d 0x102 /f >nul 2>&1
"%NF%" 120 "%B%\vxdbg.exe" 60 "%DG%\memhost.exe" > "%R%\diag-memhost.txt" 2>&1
reg add "%MIF%" /v GlobalFlag /t REG_DWORD /d 0x100 /f >nul 2>&1
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

:wait_uninstaller
rem The uninstaller goes on from a copy in Temp (VistaExtUninstall.<n>.exe)
rem and ends after closing the restore point and scheduling what was in
rem use for the restart: wait for it to end (at most 10 minutes).
ping -n 4 127.0.0.1 >nul
for /l %%w in (1,1,200) do (tasklist /fi "imagename eq VistaExtUninstall*" 2>nul | find /i "VistaExtUninstall" >nul && ping -n 4 127.0.0.1 >nul)
tasklist /fi "imagename eq VistaExtUninstall*" 2>nul | find /i "VistaExtUninstall" >nul
call :expect "the uninstaller has finished" 1 !errorlevel!
goto :eof

:ran
rem ran <name> <exit code>: the program started and ended by itself (0 to
rem 999: not a loader error or crash code, which are large or negative)
if %~2 GEQ 0 if %~2 LSS 1000 goto :pass
set /a FAIL+=1
call :say "FAIL: %~1 [exit %~2]"
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

@echo off
rem VistaExt installer for Windows Vista (SP2), Windows 7 (SP1), Windows 8 and 8.1.
rem Right-click this file and choose "Run as administrator".
rem
rem The release folder has one build per Windows and architecture:
rem vista\x86, vista\x64, win7\x86, win7\x64, win8\x86, win8\x64 (8.1 uses the
rem Windows 8 build: it has everything 8 has). This picks the one for this
rem Windows (and on 64-bit Windows also that Windows' x86 build, for the
rem 32-bit programs in SysWOW64). It is the only place that chooses.
rem
rem It never replaces a Windows file: a DLL that already exists is only
rem replaced if it is VistaExt's own (it contains the "VistaExt-owned-file"
rem marker). Everything it adds is listed in
rem "%ProgramFiles%\VistaExt\installed.txt" so uninstall.cmd can remove it.
rem
rem Updating: running it over an earlier install replaces VistaExt's files
rem and keeps the settings, the programs made compatible and the on/off
rem state. A file a running program has loaded cannot be overwritten, but it
rem can be renamed: it is renamed to <name>.vxoldN and the new one takes its
rem place, so no install ever mixes old and new files. The running program
rem keeps the old copy until it is closed; the old copies are deleted when
rem Windows restarts (VistaExtSetup.exe schedules that from the log) or by
rem the next install.
setlocal enableextensions enabledelayedexpansion
rem Options: /quiet (no questions or pauses: VistaExtSetup.exe uses it),
rem /force (also on Windows other than Vista to 8.1), /noauto (no automatic mode).
set "QUIET="
set "FORCE="
set "NOAUTO="
for %%a in (%*) do (
    if /i "%%~a"=="/quiet" set "QUIET=1"
    if /i "%%~a"=="/force" set "FORCE=1"
    if /i "%%~a"=="/noauto" set "NOAUTO=1"
)

rem A 32-bit command prompt on 64-bit Windows sees SysWOW64 as System32:
rem restart in the 64-bit one.
if defined PROCESSOR_ARCHITEW6432 (
    "%windir%\Sysnative\cmd.exe" /c ""%~f0" %*"
    exit /b !errorlevel!
)
rem Administrator check: "net session" needs the Server service, so fltmc
rem (which also needs administrator rights) is tried as well.
set "ADMIN="
net session >nul 2>&1 && set "ADMIN=1"
if not defined ADMIN fltmc >nul 2>&1 && set "ADMIN=1"
if not defined ADMIN (
    echo Please right-click install.cmd and choose "Run as administrator".
    if not defined QUIET pause
    exit /b 1
)

set "WINVER=unknown"
for /f "tokens=3" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentVersion 2^>nul ^| find "CurrentVersion"') do set "WINVER=%%a"
rem Windows 10 and 11 still say 6.3 there; they have CurrentMajorVersionNumber
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentMajorVersionNumber >nul 2>&1 && set "WINVER=10"
set "KNOWN="
for %%v in (6.0 6.1 6.2 6.3) do if "!WINVER!"=="%%v" set "KNOWN=1"
if not defined KNOWN if not defined FORCE (
    echo VistaExt is for Windows Vista, 7, 8 and 8.1. This is Windows !WINVER!.
    echo Use "install.cmd /force" to install anyway.
    if not defined QUIET pause
    exit /b 1
)

rem the build for this Windows (another Windows, only with /force: the newest)
set "OSB=win8"
if "!WINVER!"=="6.0" set "OSB=vista"
if "!WINVER!"=="6.1" set "OSB=win7"
if "!OSB!"=="win8" if not exist "%~dp0win8" if not defined KNOWN set "OSB=win7"
set "HERE=%~dp0"
set "SRC=%~dp0!OSB!"
set "VX=%ProgramFiles%\VistaExt"
set "LIST=%VX%\installed.txt"
set "NATIVE=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "NATIVE=x64"
if not exist "%VX%" mkdir "%VX%"
if not exist "%LIST%" type nul > "%LIST%"
set ADDED=0
set KEPT=0
set FAILED=0
set INUSE=0
rem switched off (an update while VistaExt is off): its system files wait in
rem VistaExt.off; the new ones go there too, and it stays off
set "ISOFF="
reg query "HKLM\SOFTWARE\VistaExt" /v Enabled 2>nul | find "0x0" >nul && set "ISOFF=1"
rem old copies an earlier update had to set aside (deleted if nothing has them loaded now)
for %%d in ("%windir%\System32" "%windir%\SysWOW64" "%windir%\System32\VistaExt.off" "%windir%\SysWOW64\VistaExt.off" "%VX%") do (
    if exist "%%~d\*.vxold*" del /f /q "%%~d\*.vxold*" >nul 2>&1
)

echo Installing VistaExt for Windows !WINVER! (build: !OSB! %NATIVE%)...
if not exist "!SRC!\%NATIVE%\kernelvx.dll" (
    echo The !OSB! %NATIVE% build is missing from this folder.
    if not defined QUIET pause
    exit /b 1
)
rem crash windows waiting for a program (vxwatch.exe) would keep their file in use
taskkill /f /im vxwatch.exe >nul 2>&1
call :install_dir "%SRC%\%NATIVE%" "%windir%\System32"
call :install_dir "%SRC%\%NATIVE%\apisets" "%windir%\System32"
if "%NATIVE%"=="x64" (
    call :install_dir "%SRC%\x86" "%windir%\SysWOW64"
    call :install_dir "%SRC%\x86\apisets" "%windir%\SysWOW64"
)
rem the crash window, started by kernelvx from the program's own system folder
call :install_file "%SRC%\%NATIVE%\vxwatch.exe" "%windir%\System32\vxwatch.exe"
if "%NATIVE%"=="x64" call :install_file "%SRC%\x86\vxwatch.exe" "%windir%\SysWOW64\vxwatch.exe"

echo Installing the tools to %VX% ...
rem a running service holds vxauto.exe, a tray icon vxtray.exe: stop them before updating
if exist "%VX%\vxauto.exe" "%VX%\vxauto.exe" /uninstall >nul 2>&1
taskkill /f /im vxtray.exe >nul 2>&1
for %%f in (vxcenter.exe vxpatch.exe msifix.exe vxauto.exe vxtray.exe) do call :install_tool "%SRC%\%NATIVE%\%%f" "%VX%\%%f"
rem which build is installed (this cmd is the 64-bit one on 64-bit Windows,
rem so this is the registry's 64-bit view, where VistaExt keeps its settings)
reg add "HKLM\SOFTWARE\VistaExt" /v Build /d "!OSB!-%NATIVE%" /f >nul
rem switched on (an update keeps "off" if the user switched it off); the
rem Windows version programs are told: Windows 10 unless the user chose
rem another before (the VistaExt window and the tray icon change both)
if not defined ISOFF reg add "HKLM\SOFTWARE\VistaExt" /v Enabled /t REG_DWORD /d 1 /f >nul
reg query "HKLM\SOFTWARE\VistaExt" /v Version >nul 2>&1 || reg add "HKLM\SOFTWARE\VistaExt" /v Version /d WIN10 /f >nul
if exist "%HERE%VistaExtSetup.exe" call :install_tool "%HERE%VistaExtSetup.exe" "%VX%\VistaExtSetup.exe"
call :install_tool "%HERE%webview2\vx-webview2-setup.cmd" "%VX%\vx-webview2-setup.cmd"
call :install_tool "%HERE%uninstall.cmd" "%VX%\uninstall.cmd"
if exist "%HERE%README.txt" call :install_tool "%HERE%README.txt" "%VX%\README.txt"
start "" /wait "%VX%\vxcenter.exe" /register

rem Add/Remove Programs (Programs and Features)
set "ARP=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt"
set "UNINST=cmd.exe /c \"\"%VX%\uninstall.cmd\"\""
if exist "%VX%\VistaExtSetup.exe" set "UNINST=\"%VX%\VistaExtSetup.exe\" /uninstall"
reg add "%ARP%" /v DisplayName /d "VistaExt" /f >nul
reg add "%ARP%" /v DisplayVersion /d "1.2.0 (Beta 1.2)" /f >nul
reg add "%ARP%" /v Publisher /d "VistaExt project" /f >nul
reg add "%ARP%" /v DisplayIcon /d "%VX%\vxcenter.exe" /f >nul
reg add "%ARP%" /v InstallLocation /d "%VX%" /f >nul
reg add "%ARP%" /v UninstallString /d "!UNINST!" /f >nul
reg add "%ARP%" /v NoModify /t REG_DWORD /d 1 /f >nul
reg add "%ARP%" /v NoRepair /t REG_DWORD /d 1 /f >nul

rem automatic mode: newly installed and updated programs are made compatible
rem by a background service (vxauto); /noauto leaves it out
if not defined NOAUTO "%VX%\vxauto.exe" /install

rem the tray icon starts with Windows for every user ("Start with Windows"
rem in its menu turns that off for one user)
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v VistaExt /d "\"%VX%\vxtray.exe\" /autostart" /f >nul

echo.
echo   !ADDED! file(s) added or updated, !KEPT! already present on this Windows (left alone), !FAILED! failed.
if not "!FAILED!"=="0" echo   Some files could not be written. Close all programs (or restart) and run install.cmd again.
if not "!INUSE!"=="0" echo   !INUSE! file(s) were in use by running programs: they use the new version once restarted. Restart Windows to finish.
if defined ISOFF echo   VistaExt is switched off, as it was before: turn it on in VistaExt ^(Start menu^).
echo.
echo Done. Open "VistaExt" from the Start menu, or right-click a program
echo and choose "VistaExt compatibility...".
if not defined QUIET pause
exit /b 0

rem ---------------------------------------------------------------------------
rem install_dir <source folder> <system folder>
:install_dir
if not exist "%~1" goto :eof
for %%f in ("%~1\*.dll") do call :install_file "%%~f" "%~2\%%~nxf"
goto :eof

rem install_file <source> <target>
rem  The file is copied if Windows does not have it, or if the one there is
rem  VistaExt's own (an earlier install). A Windows file of the same name, also
rem  one that Windows Update put there after an earlier VistaExt install, is
rem  left alone.
:install_file
set "NAME=%~nx1"
set "OURS=0"
set "DEST=%~2"
rem switched off: VistaExt's copy is in VistaExt.off (unless one is still in
rem the system folder: in use when it was switched off)
if defined ISOFF if not exist "%~2" (
    if not exist "%~dp2VistaExt.off" mkdir "%~dp2VistaExt.off"
    set "DEST=%~dp2VistaExt.off\%~nx2"
)
if not exist "!DEST!" set "OURS=1"
if exist "!DEST!" findstr /m /l /c:"VistaExt-owned-file" "!DEST!" >nul 2>&1 && set "OURS=1"
if "!OURS!"=="0" (
    set /a KEPT+=1
    goto :eof
)
rem Copied next to the target first, then renamed into place: a rename on the
rem same disk is all or nothing, so a full disk or a power cut never leaves a
rem half-written DLL in the system folder (Windows' own programs can load the
rem API-set ones). (copy's and move's own results: they do not reset
rem ERRORLEVEL on success.)
call :put "%~1" "!DEST!" || goto :eof
findstr /i /x /c:"%~2" "%LIST%" >nul 2>&1 || >>"%LIST%" echo %~2
set /a ADDED+=1
goto :eof

rem install_tool <source> <target>: one of VistaExt's own files in its folder
:install_tool
set "NAME=%~nx1"
if not exist "%~1" goto :eof
call :put "%~1" "%~2"
goto :eof

rem put <source> <target>: copied next to the target, then renamed into
rem place. A target a running program has loaded cannot be replaced, but it
rem can be renamed: it becomes <target>.vxoldN (deleted at restart) and the
rem new file takes its place. Errorlevel 1 if the target was not replaced.
:put
if exist "%~2.vxnew" del /f /q "%~2.vxnew" >nul 2>&1
copy /y "%~1" "%~2.vxnew" >nul 2>&1 || (
    if exist "%~2.vxnew" del /f /q "%~2.vxnew" >nul 2>&1
    echo   could not copy !NAME! ^(disk full?^)
    set /a FAILED+=1
    exit /b 1
)
move /y "%~2.vxnew" "%~2" >nul 2>&1 && exit /b 0
set "ASIDE="
for /l %%i in (1,1,20) do if not defined ASIDE (
    if exist "%~2.vxold%%i" del /f /q "%~2.vxold%%i" >nul 2>&1
    if not exist "%~2.vxold%%i" ren "%~2" "%~nx2.vxold%%i" >nul 2>&1 && set "ASIDE=%~2.vxold%%i"
)
if defined ASIDE move /y "%~2.vxnew" "%~2" >nul 2>&1 && (
    echo   in use, replaced; old copy deleted at restart: !ASIDE!
    set /a INUSE+=1
    exit /b 0
)
rem could not be set aside either: the old file stays complete and in place
if defined ASIDE if not exist "%~2" ren "!ASIDE!" "%~nx2" >nul 2>&1
del /f /q "%~2.vxnew" >nul 2>&1
echo   could not replace !NAME! ^(in use^)
set /a FAILED+=1
exit /b 1

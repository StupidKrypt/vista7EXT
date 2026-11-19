@echo off
rem VistaExt installer for Windows Vista (SP2) and Windows 7 (SP1).
rem Right-click this file and choose "Run as administrator".
rem
rem The release folder has one build per Windows and architecture:
rem vista\x86, vista\x64, win7\x86, win7\x64. This picks the one for this
rem Windows (and on 64-bit Windows also that Windows' x86 build, for the
rem 32-bit programs in SysWOW64). It is the only place that chooses.
rem
rem It never replaces a Windows file: a DLL that already exists is only
rem replaced if it is VistaExt's own (it contains the "VistaExt-owned-file"
rem marker). Everything it adds is listed in
rem "%ProgramFiles%\VistaExt\installed.txt" so uninstall.cmd can remove it.
setlocal enableextensions enabledelayedexpansion
rem Options: /quiet (no questions or pauses: VistaExtSetup.exe uses it),
rem /force (also on Windows other than Vista/7), /noauto (no automatic mode).
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
if not "!WINVER!"=="6.0" if not "!WINVER!"=="6.1" if not defined FORCE (
    echo VistaExt is for Windows Vista and Windows 7. This is Windows !WINVER!.
    echo Use "install.cmd /force" to install anyway.
    if not defined QUIET pause
    exit /b 1
)

rem the build for this Windows (another Windows, only with /force: Windows 7's)
set "OSB=win7"
if "!WINVER!"=="6.0" set "OSB=vista"
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
for %%f in (vxcenter.exe vxpatch.exe msifix.exe vxauto.exe vxtray.exe) do copy /y "%SRC%\%NATIVE%\%%f" "%VX%\" >nul
rem which build is installed (this cmd is the 64-bit one on 64-bit Windows,
rem so this is the registry's 64-bit view, where VistaExt keeps its settings)
reg add "HKLM\SOFTWARE\VistaExt" /v Build /d "!OSB!-%NATIVE%" /f >nul
rem switched on; the Windows version programs are told: Windows 10 unless
rem the user chose another before (the tray icon changes it)
reg add "HKLM\SOFTWARE\VistaExt" /v Enabled /t REG_DWORD /d 1 /f >nul
reg query "HKLM\SOFTWARE\VistaExt" /v Version >nul 2>&1 || reg add "HKLM\SOFTWARE\VistaExt" /v Version /d WIN10 /f >nul
if exist "%HERE%VistaExtSetup.exe" copy /y "%HERE%VistaExtSetup.exe" "%VX%\" >nul
copy /y "%HERE%webview2\vx-webview2-setup.cmd" "%VX%\" >nul
copy /y "%HERE%uninstall.cmd" "%VX%\" >nul
copy /y "%HERE%README.txt" "%VX%\" >nul 2>&1
start "" /wait "%VX%\vxcenter.exe" /register

rem Add/Remove Programs (Programs and Features)
set "ARP=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt"
set "UNINST=cmd.exe /c \"\"%VX%\uninstall.cmd\"\""
if exist "%VX%\VistaExtSetup.exe" set "UNINST=\"%VX%\VistaExtSetup.exe\" /uninstall"
reg add "%ARP%" /v DisplayName /d "VistaExt" /f >nul
reg add "%ARP%" /v DisplayVersion /d "1.0.0" /f >nul
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
if not "!FAILED!"=="0" echo   Some files were in use. Close all programs (or restart) and run install.cmd again.
echo.
echo Done. Start "VistaExt Center" from the Start menu, or right-click a program
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
if not exist "%~2" set "OURS=1"
if exist "%~2" findstr /m /l /c:"VistaExt-owned-file" "%~2" >nul 2>&1 && set "OURS=1"
if "!OURS!"=="0" (
    set /a KEPT+=1
    goto :eof
)
rem (test copy's own result: it does not reset ERRORLEVEL on success)
copy /y "%~1" "%~2" >nul 2>&1 || (
    echo   could not copy !NAME! ^(in use?^)
    set /a FAILED+=1
    goto :eof
)
findstr /i /x /c:"%~2" "%LIST%" >nul 2>&1 || >>"%LIST%" echo %~2
set /a ADDED+=1
goto :eof

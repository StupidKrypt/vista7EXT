@echo off
rem VistaExt uninstaller. Right-click and choose "Run as administrator".
rem 1. Puts back every program file VistaExt patched.
rem 2. Removes the service, the tray icon and its autostart, the Explorer menu
rem    entries, the Start menu shortcuts and every setting (also each signed-in
rem    user's own).
rem 3. Deletes the system files VistaExt added (installed.txt), but only those
rem    that are still VistaExt's (marker "VistaExt-owned-file"): a file that
rem    Windows Update has replaced since is kept.
setlocal enableextensions enabledelayedexpansion
rem Option: /quiet (no questions or pauses: VistaExtSetup.exe /uninstall uses it)
set "QUIET="
for %%a in (%*) do if /i "%%~a"=="/quiet" set "QUIET=1"
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
    echo Please right-click uninstall.cmd and choose "Run as administrator".
    if not defined QUIET pause
    exit /b 1
)
set "VX=%ProgramFiles%\VistaExt"
set "LIST=%VX%\installed.txt"
echo This removes VistaExt and puts every patched program back the way it was.
echo Close all programs that were made compatible with VistaExt first.
if not defined QUIET pause

rem Patched programs need VistaExt's DLLs: if one cannot be put back (it is
rem running), stop here and remove nothing, so it keeps working.
rem automatic mode first: it would make the restored programs compatible again
if exist "%VX%\vxauto.exe" "%VX%\vxauto.exe" /uninstall >nul 2>&1
if exist "%VX%\vxpatch.exe" (
    "%VX%\vxpatch.exe" /undoall
    if errorlevel 1 (
        echo.
        echo Some programs could not be put back because they are running.
        echo Close them ^(or restart Windows^) and run uninstall.cmd again.
        echo Nothing has been removed.
        if exist "%VX%\vxauto.exe" "%VX%\vxauto.exe" /install >nul 2>&1
        if not defined QUIET pause
        exit /b 1
    )
)
if exist "%VX%\vxcenter.exe" start "" /wait "%VX%\vxcenter.exe" /unregister
rem the tray icon, in every session, and its autostart
taskkill /f /im vxtray.exe >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v VistaExt /f >nul 2>&1
reg delete "HKLM\SOFTWARE\VistaExt" /f >nul 2>&1
rem each user's own settings (crash window choices, tray icon autostart):
rem every user profile that is loaded now
for /f "delims=" %%k in ('reg query HKU 2^>nul') do reg delete "%%k\Software\VistaExt" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt" /f >nul 2>&1

rem crash windows waiting for a program (vxwatch.exe) would keep their file in use
taskkill /f /im vxwatch.exe >nul 2>&1
set LEFT=0
if exist "%LIST%" (
    for /f "usebackq delims=" %%f in ("%LIST%") do (
        rem switched off: VistaExt's copy waits in VistaExt.off next to it
        if exist "%%~dpfVistaExt.off\%%~nxf" del /f /q "%%~dpfVistaExt.off\%%~nxf" >nul 2>&1
        if exist "%%f" (
            findstr /m /l /c:"VistaExt-owned-file" "%%f" >nul 2>&1
            if errorlevel 1 (
                echo   kept, replaced by Windows since: %%f
            ) else (
                del /f /q "%%f" >nul 2>&1
                if exist "%%f" (
                    echo   still in use: %%f
                    set /a LEFT+=1
                )
            )
        )
    )
)
for %%d in ("%windir%\System32\VistaExt.off" "%windir%\SysWOW64\VistaExt.off") do if exist %%d rd %%d >nul 2>&1
rem the log of what VistaExt changed (setup.log, with this removal, stays
rem until Windows restarts: VistaExtSetup.exe removes it then)
if exist "%ProgramData%\VistaExt\VistaExt.log" del /f /q "%ProgramData%\VistaExt\VistaExt.log" >nul 2>&1
if exist "%ProgramData%\VistaExt\auto.log" del /f /q "%ProgramData%\VistaExt\auto.log" >nul 2>&1
rem each user's crash log (%LOCALAPPDATA%\VistaExt\crashes.log)
for /d %%u in ("%SystemDrive%\Users\*") do if exist "%%~u\AppData\Local\VistaExt\crashes.log" (
    del /f /q "%%~u\AppData\Local\VistaExt\crashes.log" >nul 2>&1
    rd "%%~u\AppData\Local\VistaExt" >nul 2>&1
)
rem the patched WebView2 runtime copy, if vx-webview2-setup made one
if exist "%VX%\WebView2" rd /s /q "%VX%\WebView2" >nul 2>&1
for %%f in (vxcenter.exe vxpatch.exe msifix.exe vxauto.exe vxtray.exe VistaExtSetup.exe vx-webview2-setup.cmd README.txt) do (
    if exist "%VX%\%%f" del /f /q "%VX%\%%f" >nul 2>&1
)
if "!LEFT!"=="0" (
    del /f /q "%LIST%" >nul 2>&1
    echo VistaExt has been removed.
) else (
    echo !LEFT! file^(s^) are in use. Restart Windows and run uninstall.cmd again.
)
if not defined QUIET pause
rem When running from the installed copy, delete this script and the (now
rem empty) folder last: "(goto)" ends the script before cmd reads further.
rem (installed.txt and this script stay if anything was in use, for the retry)
if "!LEFT!"=="0" if /i "%~dp0"=="%VX%\" (goto) 2>nul & del /f /q "%~f0" & rd "%VX%"

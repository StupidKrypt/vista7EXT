@echo off
rem VistaExt: save a diagnostics snapshot ("Save diagnostics" in the VistaExt
rem window and the tray icon run this as administrator).
rem
rem It goes into the diagnostics folder, %ProgramData%\VistaExt Diagnostics
rem (src/common/diag.h: every user may read it, only administrators write),
rem as report-<date>-<time>, next to what VistaExt writes there by itself
rem (events.log, decisions.log). Copy the whole folder to send it. Nothing is
rem sent anywhere by this script.
rem
rem   system.txt            Windows, processor, memory, graphics
rem   updates.txt           installed Windows updates
rem   vistaext.txt          VistaExt's version, state and settings
rem   programs.txt          every program VistaExt runs or changed: who chose
rem                         it, whether it would start without VistaExt, and
rem                         what VistaExt finds for it now
rem   files.txt             VistaExt's files in the system folders, and any
rem                         VistaExt file with a Windows name (should be none)
rem   processes.txt         running programs, and which have VistaExt loaded
rem   events-*.txt          Windows' error log, last 14 days
rem   logs\                 VistaExt's logs, each user's crash logs
rem   wer\                  Windows Error Reporting's reports (last 14 days)
rem   dumps\                crash dumps of the last 14 days: Windows' own and
rem                         those of Chromium/Electron programs (Crashpad)
setlocal enableextensions enabledelayedexpansion
rem the 64-bit command prompt on 64-bit Windows (System32, registry view)
if defined PROCESSOR_ARCHITEW6432 (
    "%windir%\Sysnative\cmd.exe" /c ""%~f0" %*"
    exit /b !errorlevel!
)
set "VX=%ProgramFiles%\VistaExt"
rem the folder and a new report folder in it, from VistaExt itself (Windows'
rem own folder API, and the folder's protection: see diag.h)
set "D="
set "R="
if exist "%VX%\vxauto.exe" (
    for /f "delims=" %%a in ('"%VX%\vxauto.exe" /diagdir 2^>nul') do set "D=%%a"
    for /f "delims=" %%a in ('"%VX%\vxauto.exe" /diagdir report 2^>nul') do set "R=%%a"
)
if not defined R (
    echo VistaExt's diagnostics folder cannot be used. Run this as administrator,
    echo with VistaExt installed.
    if /i not "%~1"=="/quiet" pause
    exit /b 1
)
mkdir "%R%\logs" "%R%\wer" "%R%\dumps" >nul 2>&1
echo Saving VistaExt diagnostics to:
echo   %R%
echo This takes a minute.

echo   - Windows and this PC
> "%R%\system.txt" (
    echo VistaExt diagnostics, %DATE% %TIME%
    ver
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v ProductName
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CSDVersion
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber
    echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE%
    echo PROCESSOR_IDENTIFIER=%PROCESSOR_IDENTIFIER%
    echo NUMBER_OF_PROCESSORS=%NUMBER_OF_PROCESSORS%
    wmic os get Caption,Version,OSArchitecture,TotalVisibleMemorySize,FreePhysicalMemory,OSLanguage /value
    wmic computersystem get Manufacturer,Model /value
    wmic cpu get Name /value
    wmic path Win32_VideoController get Name,DriverVersion,DriverDate /value
) 2>&1
wmic qfe get HotFixID,InstalledOn > "%R%\updates.txt" 2>&1

echo   - VistaExt's state and settings
> "%R%\vistaext.txt" (
    reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VistaExt" /v DisplayVersion
    if exist "%VX%\vxauto.exe" "%VX%\vxauto.exe" /status
    sc query VistaExtAuto
    reg query "HKLM\SOFTWARE\VistaExt" /s
    echo.
    echo Each user's own settings:
    for /f "delims=" %%k in ('reg query HKU 2^>nul') do reg query "%%k\Software\VistaExt" /s 2>nul
    echo.
    echo Image File Execution Options naming vxload.dll:
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options" /s /f vxload.dll /d
    reg query "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\Image File Execution Options" /s /f vxload.dll /d
) 2>&1

echo   - the programs VistaExt runs
if exist "%VX%\vxauto.exe" "%VX%\vxauto.exe" /report "%R%\programs.txt" >nul 2>&1

echo   - VistaExt's files
> "%R%\files.txt" (
    echo Files VistaExt installed ^(installed.txt^), and whether they are there:
    if exist "%VX%\installed.txt" for /f "usebackq delims=" %%f in ("%VX%\installed.txt") do (
        if exist "%%f" (echo   present  %%f) else if exist "%%~dpfVistaExt.off\%%~nxf" (echo   off      %%f) else echo   MISSING  %%f
    )
    echo.
    echo Files with a Windows name that carry VistaExt's marker ^(this version puts none there^):
    for %%d in ("%windir%\System32" "%windir%\SysWOW64") do if exist "%%~d" (
        for %%f in ("%%~d\api-ms-win-*.dll" "%%~d\ext-ms-win-*.dll" "%%~d\combase.dll" "%%~d\shcore.dll" "%%~d\d3d12.dll" "%%~d\kernelbase.dll" "%%~d\bcryptprimitives.dll") do (
            findstr /m /l /c:"VistaExt-owned-file" "%%~f" >nul 2>&1 && echo   %%~f
        )
    )
    echo.
    echo Left over from updates ^(deleted at the next restart^):
    for %%d in ("%windir%\System32" "%windir%\SysWOW64" "%VX%") do if exist "%%~d\*.vxold*" dir /b "%%~d\*.vxold*"
) 2>&1

echo   - running programs
> "%R%\processes.txt" (
    tasklist /v
    echo.
    echo With VistaExt's in-memory loader:
    tasklist /m vxload.dll
    echo.
    echo With VistaExt's kernelvx.dll:
    tasklist /m kernelvx.dll
) 2>&1

echo   - Windows' error log (last 14 days)
wevtutil qe Application /q:"*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" /f:text /rd:true > "%R%\events-application.txt" 2>&1
wevtutil qe System /q:"*[System[(Level=1 or Level=2 or Provider[@Name='Application Popup']) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" /f:text /rd:true > "%R%\events-system.txt" 2>&1

echo   - logs
for %%f in ("%ProgramData%\VistaExt\*.log") do copy /y "%%~f" "%R%\logs\" >nul 2>&1
for /d %%u in ("%SystemDrive%\Users\*") do (
    if exist "%%~u\AppData\Local\VistaExt\crashes.log" copy /y "%%~u\AppData\Local\VistaExt\crashes.log" "%R%\logs\crashes-%%~nxu.log" >nul 2>&1
    if exist "%%~u\AppData\Local\VistaExt\crash-details.log" copy /y "%%~u\AppData\Local\VistaExt\crash-details.log" "%R%\logs\crash-details-%%~nxu.log" >nul 2>&1
)

echo   - error reports and crash dumps (last 14 days)
set "RC=/S /MAXAGE:14 /R:0 /W:0 /NP /NJH /NJS /NFL /NDL"
rem minidumps only: Windows' heap dumps (.hdmp) can be hundreds of MB.
rem Chromium and Electron programs keep their own crash dumps (Crashpad).
robocopy "%ProgramData%\Microsoft\Windows\WER" "%R%\wer\all-users" *.wer *.txt *.mdmp %RC% >nul 2>&1
for /d %%u in ("%SystemDrive%\Users\*") do (
    if exist "%%~u\AppData\Local\Microsoft\Windows\WER" robocopy "%%~u\AppData\Local\Microsoft\Windows\WER" "%R%\wer\%%~nxu" *.wer *.txt *.mdmp %RC% >nul 2>&1
    if exist "%%~u\AppData\Local\CrashDumps" robocopy "%%~u\AppData\Local\CrashDumps" "%R%\dumps\%%~nxu\windows" *.dmp %RC% >nul 2>&1
    for %%r in ("%%~u\AppData\Local" "%%~u\AppData\Roaming") do if exist "%%~r" (
        for /f "delims=" %%c in ('dir /s /b /ad "%%~r\Crashpad" 2^>nul') do (
            set "CP=%%c"
            set "NAME=!CP:%%~r\=!"
            set "NAME=!NAME:\=_!"
            robocopy "%%c" "%R%\dumps\%%~nxu\!NAME!" *.dmp %RC% >nul 2>&1
        )
    )
)
for /f %%n in ('dir /s /b /a-d "%R%\dumps" 2^>nul ^| find /c /v ""') do echo     %%n crash dump^(s^)

echo.
echo Done. The diagnostics folder:
echo   %D%
echo Copy that whole folder to send it. See README.txt in it about what it contains.
if /i not "%~1"=="/quiet" (
    start "" explorer.exe "%D%"
    pause
)
exit /b 0

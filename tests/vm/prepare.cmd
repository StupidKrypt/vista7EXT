@echo off
rem Prepares a test VM once (tests/vm/vm.sh prepare): installs Microsoft's
rem updates from the CD (\prereq\*.msu: the C runtime, and Vista's Platform
rem Update), restarting if they ask for it, then shuts down. The disk is kept
rem as prepared.qcow2 and later runs start from it, so they skip this.
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "R="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
set "LOG=%R%\results.txt"
set "MARK=%SystemDrive%\vxt-prepare-restarted.marker"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
set "OSV=6.0"
ver | find "6.1." >nul && set "OSV=6.1"
set "NEEDBOOT="
for %%m in ("%CDR%\prereq\windows!OSV!-*-%OSA%_*.msu") do (
    start "" /wait wusa "%%~fm" /quiet /norestart
    set "RC=!errorlevel!"
    call :say "%%~nxm: wusa exit !RC! - 0 installed, 3010 installed/restart, 2359302 already there"
    if "!RC!"=="3010" set "NEEDBOOT=1"
)
if defined NEEDBOOT if not exist "%MARK%" (
    > "%MARK%" echo restarted
    call :say "restarting Windows to finish the updates"
    shutdown /r /t 5 /f
    exit /b 0
)
del /f /q "%MARK%" >nul 2>&1
call :say "PREPARED ok"
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

:say
if defined R >>"%LOG%" echo %~1
2>nul >COM1 echo %~1
goto :eof

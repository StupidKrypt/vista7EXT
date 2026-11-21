@echo off
rem VistaExt's cost on a slow PC (tests/vm/vm.sh perf <vm>; the VM gets one
rem processor, 1 GB of memory and a 20 MB/s disk). Started at logon from the
rem perf CD (%1 = its drive). Measures, with tests/vm/procstat.exe and
rem vxbench.exe; nothing here passes or fails. Lines start with PERF/BENCH.
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "R="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
if not defined R (
    (echo VXPERF no results disk)>COM1
    shutdown /s /t 5 /f
    exit /b 1
)
set "LOG=%R%\results.txt"
set "T=%SystemDrive%\vxt"
set "VX=%ProgramFiles%\VistaExt"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
> "%LOG%" echo VistaExt on a slow PC
if exist "%T%" rd /s /q "%T%"
xcopy "%CDR%\vxvm" "%T%\" /e /i /q /y >nul
attrib -r "%T%\*" /s /d >nul
set "B=%T%\bin\%OSA%"
set "PS=%B%\procstat.exe"
set "P=%T%\perf"
mkdir "%P%\mem" "%P%\native" "%P%\win" "%P%\big" 2>nul
ver >> "%LOG%"
wmic computersystem get NumberOfLogicalProcessors,TotalPhysicalMemory /format:list | find "=" >> "%LOG%"

call :say "### installing (install.cmd)"
"%PS%" run cmd /c "%T%\VistaExt\install.cmd" /quiet >> "%LOG%" 2>&1
if not exist "%VX%\vxauto.exe" (
    call :say "RESULT: FAILED - VistaExt did not install, nothing measured"
    (echo VXTEST done)>COM1
    shutdown /s /t 3 /f
    exit /b 1
)
if exist "%CDR%\vxvm\only-big.txt" goto :big
if exist "%CDR%\vxvm\only-bench.txt" goto :bench
call :say "### the service's first scan of installed programs"
for /l %%w in (1,1,180) do (
    find "scan finished" "%ProgramData%\VistaExt\VistaExt.log" >nul 2>&1 && goto :scanned
    ping -n 6 127.0.0.1 >nul
)
:scanned
findstr /c:"scanning installed programs" /c:"scan finished" "%ProgramData%\VistaExt\VistaExt.log" >> "%LOG%" 2>nul

call :say "### the service: memory, start time"
"%PS%" show vxauto.exe >> "%LOG%"
net stop VistaExtAuto >nul 2>&1
"%PS%" service VistaExtAuto \\.\pipe\VistaExt >> "%LOG%"

call :say "### the tray icon: start time, memory"
taskkill /f /im vxtray.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%PS%" ready "%VX%\vxtray.exe" >> "%LOG%"
ping -n 6 127.0.0.1 >nul
"%PS%" show vxtray.exe >> "%LOG%"

call :say "### idle: 10 minutes, nothing happening"
ping -n 61 127.0.0.1 >nul
"%PS%" idle 600 vxauto.exe vxtray.exe >> "%LOG%"

call :say "### the crash window's cost: 3 windowed programs VistaExt runs"
copy /y "%B%\crashtest.exe" "%P%\win\" >nul
"%VX%\vxpatch.exe" /memory /q "%P%\win" >nul
for /l %%i in (1,1,3) do start "" "%P%\win\crashtest.exe" /stay
ping -n 16 127.0.0.1 >nul
"%PS%" show crashtest.exe vxwatch.exe >> "%LOG%"
taskkill /f /im crashtest.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
taskkill /f /im vxwatch.exe >nul 2>&1

:bench
call :say "### per call and program start: plain Windows, VistaExt (real version), VistaExt (told Windows 10)"
copy /y "%B%\vxbench-native.exe" "%P%\native\" >nul
for %%d in (native mem) do copy /y "%B%\benchdll.dll" "%P%\%%d\" >nul
copy /y "%B%\vxbench.exe" "%P%\mem\" >nul
"%VX%\vxpatch.exe" /memory /q "%P%\mem" >nul
call :say "--- plain Windows"
"%P%\native\vxbench-native.exe" >> "%LOG%" 2>&1
"%VX%\vxauto.exe" /setver REAL >nul
call :say "--- VistaExt, real version"
"%P%\mem\vxbench.exe" >> "%LOG%" 2>&1
"%VX%\vxauto.exe" /setver WIN10 >nul
call :say "--- VistaExt, told Windows 10"
"%P%\mem\vxbench.exe" >> "%LOG%" 2>&1

:big
call :say "### a 200 MB program file: checking it (vxpatch /check)"
fsutil file createnew "%P%\pad.bin" 209715200 >nul
copy /b "%B%\vxtest.exe" + "%P%\pad.bin" "%P%\big\big.exe" >nul
copy /b "%B%\vercheck.exe" + "%P%\pad.bin" "%P%\big\big-setup.exe" >nul
del "%P%\pad.bin"
rem let Windows finish writing the 400 MB just made (20 MB/s): not part of what is measured
ping -n 31 127.0.0.1 >nul
"%PS%" run "%VX%\vxpatch.exe" /check /q "%P%\big\big.exe" >> "%LOG%" 2>&1
call :say "### a 200 MB setup program that runs as it is (setup rule, signature check)"
"%PS%" run "%VX%\vxauto.exe" /prepare "%P%\big\big-setup.exe" >> "%LOG%" 2>&1
call :say "### scanning Program Files (vxauto /scan)"
"%PS%" run "%VX%\vxauto.exe" /scan >> "%LOG%" 2>&1

call :say "### slow steps the service logged"
findstr /c:" took " "%ProgramData%\VistaExt\VistaExt.log" >> "%LOG%" 2>nul
copy /y "%ProgramData%\VistaExt\VistaExt.log" "%R%\VistaExt.log" >nul 2>&1
call :say "Finished: %DATE% %TIME%"
call :say "RESULT: measured, see above"
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

:say
>>"%LOG%" echo %~1
>>"%LOG%" echo    [%TIME%]
2>nul >COM1 echo %~1
goto :eof

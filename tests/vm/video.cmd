@echo off
rem Video playback with real browsers (tests/vm/vm.sh video). Runs at logon
rem from the video CD (%1 = its drive):
rem   - installs Microsoft's C runtime update (\prereq\*.msu), restarting once
rem   - installs VistaExt with VistaExtSetup.exe /quiet
rem   - Brave and Firefox, each in in-memory mode (copied into Program Files,
rem     made compatible by the automatic mode) and in file mode (vxpatch
rem     /file): starts the browser with nothing but the test page's URL on
rem     its command line (any browser takes that). The page (video.html,
rem     served by the host) plays a 4K YouTube video, measures it and sends
rem     the results back. Meanwhile this script records the CPU load.
rem     Nothing here is specific to one browser.
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "R="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
if not defined R (
    (echo VXVIDEO no results disk)>COM1
    shutdown /s /t 5 /f
    exit /b 1
)
set "LOG=%R%\results.txt"
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" /f >nul 2>&1
set "T=%SystemDrive%\vxt"
set "VX=%ProgramFiles%\VistaExt"
set "ALOG=%ProgramData%\VistaExt\VistaExt.log"
set "BOOTMARK=%SystemDrive%\vxt-restarted.marker"
rem how long the page plays the video (s), and the page's address
set /a PLAYSEC=180
set "URL="
for /f "usebackq delims=" %%u in ("%R%\video-url.txt") do set "URL=%%u"
if not exist "%BOOTMARK%" (> "%LOG%" echo VistaExt video playback test) else (call :say "--- after the restart for the updates ---")
set "K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
for /f "tokens=2*" %%a in ('reg query "%K%" /v ProductName 2^>nul ^| find "REG_"') do set "PN=%%b"
for /f "tokens=2*" %%a in ('reg query "%K%" /v CSDVersion 2^>nul ^| find "REG_"') do set "CSD=%%b"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
call :say "Windows: !PN! !CSD!, %OSA%, %NUMBER_OF_PROCESSORS% processors"
call :say "Started: %DATE% %TIME%"
if exist "%T%" rd /s /q "%T%"
xcopy "%CDR%\vxvm" "%T%\" /e /i /q /y >nul
attrib -r "%T%\*" /s /d >nul

call :say "### Microsoft updates from the CD"
set "OSV=6.0"
ver | find "6.1." >nul && set "OSV=6.1"
set "NEEDBOOT="
for %%m in ("%CDR%\prereq\windows!OSV!-*-%OSA%_*.msu") do (
    start "" /wait wusa "%%~fm" /quiet /norestart
    set "RC=!errorlevel!"
    call :say "%%~nxm: wusa exit !RC!"
    if "!RC!"=="3010" set "NEEDBOOT=1"
)
if defined NEEDBOOT if not exist "%BOOTMARK%" (
    > "%BOOTMARK%" echo restarted
    call :say "restarting Windows to finish the updates"
    shutdown /r /t 5 /f
    exit /b 0
)

call :say "### install VistaExt: VistaExtSetup.exe /quiet"
start "" /wait "%T%\VistaExtSetup.exe" /quiet
call :say "setup exit !errorlevel!"

call :say "test page: !URL!"

rem which browsers: \vxvm\video-apps.txt (default: brave firefox). Each is
rem the folder \apps\<name> on the CD, or \apps\<name>-x64 / -x86 for this
rem Windows; its program is in X_<name>.
set "WANT= brave firefox "
if exist "%CDR%\vxvm\video-apps.txt" (
    set "WANT= "
    for /f "usebackq delims=" %%a in ("%CDR%\vxvm\video-apps.txt") do set "WANT=!WANT!%%a "
)
call :say "browsers:!WANT!"
set "X_brave=brave.exe"
set "X_firefox=firefox.exe"
set "X_edge=msedge.exe"

rem in-memory mode: what users get (automatic mode in Program Files);
rem file mode in C:\Apps, for comparison
for %%b in (!WANT!) do (
    set "SRC=%CDR%\apps\%%b"
    if exist "%CDR%\apps\%%b-%OSA%" set "SRC=%CDR%\apps\%%b-%OSA%"
    xcopy "!SRC!" "%ProgramFiles%\VXT-%%b\" /e /i /q /y >nul
    xcopy "!SRC!" "%SystemDrive%\Apps\%%b\" /e /i /q /y >nul
    "%VX%\vxpatch.exe" /file /auto /q "%SystemDrive%\Apps\%%b" > "%R%\%%b-filepatch.txt" 2>&1
)
for %%b in (!WANT!) do call :wait_auto "%ProgramFiles%\VXT-%%b"

rem Firefox asks on its first start to agree to Mozilla's Terms of Use, and
rem then whether it should be the default browser, and loads nothing until
rem these are answered. In these throwaway test VMs (only here, with the
rem owner's agreement) Firefox's own enterprise policies answer them: terms
rem accepted, no default-browser question (Firefox is not made the default),
rem no first-run page.
for %%d in ("%ProgramFiles%\VXT-firefox" "%SystemDrive%\Apps\firefox") do if exist "%%~d" (
    mkdir "%%~d\distribution" 2>nul
    > "%%~d\distribution\policies.json" echo {"policies": {"SkipTermsOfUse": true, "DontCheckDefaultBrowser": true, "OverrideFirstRunPage": ""}}
)
rem Edge: the same through its policies (also only in these test VMs, with
rem the owner's agreement): no first-run pages, sign-in, sync or
rem default-browser question
if not "!WANT: edge =!"=="!WANT!" call :edge_policies

rem an experiment: VistaExt leaves out child-process attributes this Windows
rem lacks for every program (VX_LENIENT_CHILDREN), not only --no-sandbox ones
if exist "%CDR%\vxvm\lenient.txt" (
    set VX_LENIENT_CHILDREN=1
    call :say "VX_LENIENT_CHILDREN=1"
)
for %%b in (!WANT!) do call :vtest %%b memory "%ProgramFiles%\VXT-%%b\!X_%%b!"
for %%b in (!WANT!) do call :vtest %%b file "%SystemDrive%\Apps\%%b\!X_%%b!"

wevtutil qe Application /c:60 /rd:true /f:text > "%R%\events.txt" 2>&1
copy /y "%ALOG%" "%R%\auto.log" >nul 2>&1
call :say "Finished: %DATE% %TIME%"
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

rem =========================================================================
:edge_policies
set "EP=HKLM\SOFTWARE\Policies\Microsoft\Edge"
reg add "%EP%" /v HideFirstRunExperience /t REG_DWORD /d 1 /f >nul
reg add "%EP%" /v DefaultBrowserSettingEnabled /t REG_DWORD /d 0 /f >nul
reg add "%EP%" /v BrowserSignin /t REG_DWORD /d 0 /f >nul
reg add "%EP%" /v SyncDisabled /t REG_DWORD /d 1 /f >nul
call :say "Edge policies: no first-run experience, sign-in, sync or default-browser question"
goto :eof

rem =========================================================================
rem vtest <browser> <mode> <program>
:vtest
set "BR=%~1"
set "MO=%~2"
set "PROG=%~3"
set "IMG=%~nx3"
call :say "### video: !BR!, !MO! mode"
call :close
del /f /q "%LOCALAPPDATA%\VistaExt\crashes.log" >nul 2>&1
rem a first start of its own, closed again: what a browser shows only on
rem its very first start (welcome pages) does not get in the way
start "" "!PROG!"
ping -n 21 127.0.0.1 >nul
call :close
rem the browser's own error output (any program's stdout/stderr) is kept
> "%T%\withlog.cmd" echo @"%%~1" "%%~2" ^> "%%~3" 2^>^&1
start "" /b cmd /c call "%T%\withlog.cmd" "!PROG!" "!URL!?b=!BR!&m=!MO!&s=%PLAYSEC%" "%R%\out-!BR!-!MO!.txt"
start "" /b typeperf "\Processor(_Total)\%% Processor Time" -si 5 -sc 44 -o "%R%\cpu-!BR!-!MO!.csv" -y >nul 2>&1
ping -n 91 127.0.0.1 >nul
2>nul >COM1 echo SHOT video-!BR!-!MO!-90s
ping -n 111 127.0.0.1 >nul
2>nul >COM1 echo SHOT video-!BR!-!MO!-end
tasklist /fi "imagename eq !IMG!" 2>nul | find /i "!IMG!" > "%T%\running.txt"
for /f %%c in ('find /c /v "" ^< "%T%\running.txt"') do set "NP=%%c"
if "!NP!"=="0" (call :say "!BR! !MO!: NOT running at the end") else (call :say "!BR! !MO!: still running at the end, !NP! process(es)")
rem vxwatch.exe waits quietly next to every windowed program; only a crash
rem makes it show its window and add a line to its crash log
set "CL=%LOCALAPPDATA%\VistaExt\crashes.log"
if exist "!CL!" (
    call :say "!BR! !MO!: CRASHED - VistaExt's crash log:"
    type "!CL!" >> "%LOG%"
    type "!CL!" > COM1 2>nul
    2>nul >COM1 echo SHOT video-!BR!-!MO!-crashwindow
    del /f /q "!CL!" >nul 2>&1
) else (
    call :say "!BR! !MO!: no crash"
)
ping -n 6 127.0.0.1 >nul
call :close
ping -n 4 127.0.0.1 >nul
if exist "!CL!" (
    call :say "!BR! !MO!: CRASHED while closing - VistaExt's crash log:"
    type "!CL!" >> "%LOG%"
    2>nul >COM1 echo SHOT video-!BR!-!MO!-crash-closing
    del /f /q "!CL!" >nul 2>&1
)
taskkill /f /im typeperf.exe >nul 2>&1
taskkill /f /im vxwatch.exe >nul 2>&1
goto :eof

rem close the browser the way a user does (its windows get "close"), and
rem only if it does not go away, end it
:close
taskkill /im "!IMG!" >nul 2>&1
for /l %%w in (1,1,10) do (
    tasklist /fi "imagename eq !IMG!" 2>nul | find /i "!IMG!" >nul || goto :eof
    ping -n 2 127.0.0.1 >nul
)
taskkill /f /t /im "!IMG!" >nul 2>&1
ping -n 3 127.0.0.1 >nul
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

:say
>>"%LOG%" echo %~1
2>nul >COM1 echo %~1
echo %~1
goto :eof

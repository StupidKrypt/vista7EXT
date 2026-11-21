@echo off
rem Test VM dependencies (tests/vm/vm.sh deps <vm>). Runs at logon from the
rem deps CD (%1 = its drive), restarting Windows as often as the updates
rem need (it continues where it stopped: %SystemDrive%\vxdeps\stage).
rem   \vxvm\only-survey.txt on the CD: only record the state, change nothing.
rem Results on the results disk: survey-before.txt, install.txt,
rem survey-after.txt; every installer's own log in logs\.
setlocal enableextensions enabledelayedexpansion
set "CDR=%~1"
if "%CDR%"=="" set "CDR=%~d0"
set "R="
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxresults.marker set "R=%%i:"
mode COM1:115200,n,8,1 >nul 2>&1
if not defined R (
    (echo VXDEPS no results disk)>COM1
    shutdown /s /t 5 /f
    exit /b 1
)
set "LOG=%R%\install.txt"
set "W=%SystemDrive%\vxdeps"
if not exist "%W%" mkdir "%W%"
if not exist "%R%\logs" mkdir "%R%\logs"
set "OSA=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "OSA=x64"
set "OSV=6.0"
ver | find "6.1." >nul && set "OSV=6.1"
set "B=%CDR%\vxvm\bin\%OSA%"

if exist "%CDR%\vxvm\only-survey.txt" (
    call :survey "%R%\survey-before.txt"
    call :say "SURVEY ok"
    goto :done
)

rem ------------------------------------------------------------ installing
set "P=%CDR%\deps"
set "DONE=%W%\done.txt"
if not exist "%P%\plan.txt" (
    call :say "no plan.txt on the CD"
    goto :done
)
del /f /q "%W%\restarting" >nul 2>&1
if not exist "%DONE%" (
    > "%LOG%" echo Dependencies for the test VM: Windows !OSV! %OSA%
    call :survey "%R%\survey-before.txt"
    type nul > "%DONE%"
) else (call :say "--- continuing after a restart ---")
for /f "usebackq tokens=1-5 delims=|" %%a in ("%P%\plan.txt") do (
    findstr /x /c:"%%a" "%DONE%" >nul || call :step "%%a" "%%b" "%%c" "%%d" "%%e"
    if exist "%W%\restarting" goto :eof
)
call :say "### after installing"
call :survey "%R%\survey-after.txt"
for %%l in ("%TEMP%\dd_*.log" "%TEMP%\Microsoft .NET Framework*.htm" "%TEMP%\*vcredist*.log" "%SystemRoot%\Logs\DirectX.log" "%SystemRoot%\IE11_main.log") do copy /y "%%~fl" "%R%\logs\" >nul 2>&1
if exist "%W%\failed.txt" (
    for /f %%c in ('find /c /v "" ^< "%W%\failed.txt"') do call :say "DEPS INCOMPLETE: %%c step(s) failed"
    type "%W%\failed.txt" >> "%LOG%"
) else call :say "DEPS ok"
goto :done

rem step <id> <kind> <file> <arguments> <description>
:step
set "ID=%~1"
set "KIND=%~2"
set "F=%P%\%~3"
set "ARGS=%~4"
if "!ARGS!"=="-" set "ARGS="
call :say "### !ID!: %~5"
set "RC="
if "!KIND!"=="restart" (
    >>"%DONE%" echo !ID!
    > "%W%\restarting" echo !ID!
    call :say "restarting Windows"
    shutdown /r /t 5 /f
    goto :eof
)
if "!KIND!"=="cab" (
    dism /online /add-package /packagepath:"!F!" /quiet /norestart > "%W%\dism.txt" 2>&1
    set "RC=!errorlevel!"
)
if "!KIND!"=="msu" (
    start "" /wait wusa "!F!" /quiet /norestart
    set "RC=!errorlevel!"
)
if "!KIND!"=="exe" (
    start "" /wait "!F!" !ARGS!
    set "RC=!errorlevel!"
    rem the newest Visual C++ 2015-2022 refusing this Windows: the fallback runs
    if "!ID:~0,5!"=="vc14-" if not "!RC!"=="0" if not "!RC!"=="3010" if not "!RC!"=="1638" > "%W%\!ID:~5!.vc14failed" echo !RC!
)
if "!KIND!"=="fallback" (
    set "A=!ID:~6!"
    if not exist "%W%\!A!.vc14failed" (
        call :say "not needed: the newest one installed"
        >>"%DONE%" echo !ID!
        goto :eof
    )
    start "" /wait "!F!" !ARGS!
    set "RC=!errorlevel!"
)
if "!KIND!"=="dx" (
    if exist "%W%\dx" rd /s /q "%W%\dx"
    start "" /wait "!F!" /Q /T:"%W%\dx"
    start "" /wait "%W%\dx\DXSETUP.exe" /silent
    set "RC=!errorlevel!"
)
if "!KIND!"=="netfx3" (
    dism /online /enable-feature /featurename:NetFx3 /norestart > "%W%\dism.txt" 2>&1
    set "RC=!errorlevel!"
    find /i "enabled" "%W%\dism.txt" >> "%LOG%"
)
if "!KIND!"=="roots" (
    set /a NC=0, NF=0
    for %%c in ("%P%\roots\*.crt") do (
        certutil -addstore -f Root "%%~fc" >nul 2>&1 && (set /a NC+=1) || (set /a NF+=1)
    )
    call :say "trusted roots imported: !NC!, failed: !NF!"
    certutil -addstore -f Disallowed "%P%\disallowed\disallowedcert.stl" > "%W%\dis.txt" 2>&1
    set "RC=!errorlevel!"
    call :say "Microsoft's disallowed list imported: exit !RC!"
    if not "!NF!"=="0" set "RC=1"
)
if "!KIND!"=="tls" (
    call :tls
    set "RC=0"
)
call :meaning !RC!
rem the newest Visual C++ 2015-2022 refusing this Windows is expected: an
rem older one follows (the fallback step); every other failure counts
if "!MEANING!"=="failed" if "!ID:~0,5!"=="vc14-" set "MEANING=refused this Windows: the older one follows"
if "!MEANING!"=="failed" >>"%W%\failed.txt" echo !ID!
call :say "!ID!: exit !RC! - !MEANING!"
>>"%DONE%" echo !ID!
goto :eof

:meaning
set "MEANING=failed"
if "%~1"=="0" set "MEANING=installed"
if "%~1"=="3010" set "MEANING=installed, restart needed"
if "%~1"=="2359302" set "MEANING=already installed"
if "%~1"=="-2145124329" set "MEANING=not applicable to this Windows"
if "%~1"=="1638" set "MEANING=a same or newer version is already installed"
if "%~1"=="1641" set "MEANING=installed, restart started by the installer"
if "%~1"=="-2146498530" set "MEANING=not applicable to this Windows"
if "%~1"=="5100" set "MEANING=refused: the system does not meet its requirements"
if "%~1"=="-2145124330" set "MEANING=another installation is in progress"
goto :eof

rem TLS 1.1/1.2 on: SChannel (the system), WinHTTP's default (KB3140245),
rem Internet Explorer / WinINet (all users, as a policy), .NET 2.0-4.x
:tls
set "SC=HKLM\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols"
for %%v in ("TLS 1.1" "TLS 1.2") do for %%r in (Client Server) do (
    reg add "%SC%\%%~v\%%r" /v Enabled /t REG_DWORD /d 1 /f >nul
    reg add "%SC%\%%~v\%%r" /v DisabledByDefault /t REG_DWORD /d 0 /f >nul
)
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Internet Settings\WinHttp" /v DefaultSecureProtocols /t REG_DWORD /d 0xA80 /f >nul
if "%OSA%"=="x64" reg add "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Internet Settings\WinHttp" /v DefaultSecureProtocols /t REG_DWORD /d 0xA80 /f >nul
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\CurrentVersion\Internet Settings" /v SecureProtocols /t REG_DWORD /d 0xA80 /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings" /v SecureProtocols /t REG_DWORD /d 0xA80 /f >nul
for %%n in (v2.0.50727 v4.0.30319) do (
    reg add "HKLM\SOFTWARE\Microsoft\.NETFramework\%%n" /v SchUseStrongCrypto /t REG_DWORD /d 1 /f >nul
    reg add "HKLM\SOFTWARE\Microsoft\.NETFramework\%%n" /v SystemDefaultTlsVersions /t REG_DWORD /d 1 /f >nul
    if "%OSA%"=="x64" reg add "HKLM\SOFTWARE\Wow6432Node\Microsoft\.NETFramework\%%n" /v SchUseStrongCrypto /t REG_DWORD /d 1 /f >nul
    if "%OSA%"=="x64" reg add "HKLM\SOFTWARE\Wow6432Node\Microsoft\.NETFramework\%%n" /v SystemDefaultTlsVersions /t REG_DWORD /d 1 /f >nul
)
call :say "TLS 1.1 and 1.2: SChannel client and server on; WinHTTP default 0xA80 (TLS 1.0-1.2); Internet Explorer 0xA80; .NET strong crypto and system default TLS"
goto :eof

rem =========================================================================
rem survey <file>: what this Windows has (nothing is changed)
:survey
set "S=%~1"
> "%S%" echo ===== %DATE% %TIME%
>>"%S%" ver
>>"%S%" echo ===== operating system
wmic os get Caption,Version,BuildNumber,CSDVersion,ServicePackMajorVersion,OSArchitecture,OSLanguage /format:list 2>nul | find "=" >> "%S%"
>>"%S%" echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE%
>>"%S%" echo ===== installed updates
wmic qfe get HotFixID,Description,InstalledOn 2>nul | more >> "%S%"
>>"%S%" echo ===== installed programs (Programs and Features)
for %%k in ("HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall") do (
    for /f "tokens=2*" %%a in ('reg query "%%~k" /s /v DisplayName 2^>nul ^| find "REG_SZ"') do >>"%S%" echo   %%b
)
>>"%S%" echo ===== .NET Framework (version / release per key)
reg query "HKLM\SOFTWARE\Microsoft\NET Framework Setup\NDP" /s /v Version 2>nul | find /v "End of search" >> "%S%"
reg query "HKLM\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full" /v Release 2>nul | find "Release" >> "%S%"
>>"%S%" echo ===== Visual C++ runtimes (system folders)
for %%f in (msvcr80.dll msvcr90.dll msvcr100.dll msvcr110.dll msvcr120.dll vcruntime140.dll msvcp140.dll ucrtbase.dll) do (
    if exist "%SystemRoot%\System32\%%f" (>>"%S%" echo   System32\%%f) else (>>"%S%" echo   System32\%%f: missing)
    if "%OSA%"=="x64" if exist "%SystemRoot%\SysWOW64\%%f" (>>"%S%" echo   SysWOW64\%%f) else if "%OSA%"=="x64" (>>"%S%" echo   SysWOW64\%%f: missing)
)
dir /b "%SystemRoot%\WinSxS\*microsoft.vc80.crt*" "%SystemRoot%\WinSxS\*microsoft.vc90.crt*" 2>nul >> "%S%"
>>"%S%" echo ===== DirectX and graphics
reg query "HKLM\SOFTWARE\Microsoft\DirectX" /v Version 2>nul | find "Version" >> "%S%"
for %%f in (d3d11.dll dxgi.dll d2d1.dll dwrite.dll d3dx9_43.dll xinput1_3.dll d3dcompiler_43.dll d3dcompiler_47.dll xaudio2_7.dll) do (
    if exist "%SystemRoot%\System32\%%f" (>>"%S%" echo   %%f) else (>>"%S%" echo   %%f: missing)
)
>>"%S%" echo ===== Internet Explorer
reg query "HKLM\SOFTWARE\Microsoft\Internet Explorer" /v Version 2>nul | find "REG_" >> "%S%"
reg query "HKLM\SOFTWARE\Microsoft\Internet Explorer" /v svcVersion 2>nul | find "REG_" >> "%S%"
>>"%S%" echo ===== fonts
if exist "%SystemRoot%\Fonts\seguisym.ttf" (>>"%S%" echo   Segoe UI Symbol: present) else (>>"%S%" echo   Segoe UI Symbol: missing)
>>"%S%" echo ===== TLS settings
reg query "HKLM\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols" /s 2>nul >> "%S%"
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Internet Settings\WinHttp" /v DefaultSecureProtocols 2>nul | find "REG_" >> "%S%"
reg query "HKLM\SOFTWARE\Microsoft\.NETFramework\v4.0.30319" /v SchUseStrongCrypto 2>nul | find "REG_" >> "%S%"
reg query "HKLM\SOFTWARE\Microsoft\.NETFramework\v4.0.30319" /v SystemDefaultTlsVersions 2>nul | find "REG_" >> "%S%"
>>"%S%" echo ===== trusted root certificates
certutil -store Root > "%W%\roots.txt" 2>&1
find /c "================ Certificate" "%W%\roots.txt" >> "%S%"
>>"%S%" echo ===== HTTPS: native (WinHTTP, WinINet)
"%B%\nettest.exe" https://www.microsoft.com/ https://www.google.com/ https://discord.com/ https://store.steampowered.com/ https://download.scdn.co/SpotifySetup.exe >> "%S%" 2>&1
>>"%S%" echo ===== HTTPS: .NET
call :netcheck "%S%"
goto :eof

rem netcheck <file>: an HTTPS download from a .NET program, compiled here with
rem the newest .NET compiler this Windows has (no compiler: no .NET 2.0+)
:netcheck
set "CSC="
for %%v in (v2.0.50727 v3.5 v4.0.30319) do (
    if exist "%SystemRoot%\Microsoft.NET\Framework\%%v\csc.exe" set "CSC=%SystemRoot%\Microsoft.NET\Framework\%%v\csc.exe"
)
if not defined CSC (>>"%~1" echo   no .NET compiler found & goto :eof)
>>"%~1" echo   compiler: !CSC!
> "%W%\nettest.cs" echo using System; using System.Net;
>>"%W%\nettest.cs" echo class T { static int Main(string[] a) { int bad = 0; foreach (string u in a) { try { new WebClient().DownloadString(u); Console.WriteLine("  .NET     " + u + ": ok"); } catch (Exception e) { bad++; Console.WriteLine("  .NET     " + u + ": FAILED, " + e.GetBaseException().Message); } } Console.WriteLine("  runtime " + Environment.Version); return bad; } }
"!CSC!" /nologo /out:"%W%\nettest-net.exe" "%W%\nettest.cs" > "%W%\csc.txt" 2>&1 || (type "%W%\csc.txt" >> "%~1" & goto :eof)
"%W%\nettest-net.exe" https://www.microsoft.com/ https://www.google.com/ https://discord.com/ >> "%~1" 2>&1
goto :eof

:say
>>"%LOG%" echo %~1
2>nul >COM1 echo %~1
echo %~1
goto :eof

:done
(echo VXTEST done)>COM1
shutdown /s /t 3 /f
exit /b 0

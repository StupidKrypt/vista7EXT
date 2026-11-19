@echo off
rem VistaExt: set up a Microsoft Edge WebView2 runtime for WebView2 programs.
rem
rem   vx-webview2-setup.cmd "<extracted Fixed Version runtime folder>" [x86|x64]
rem
rem WebView2 programs need the WebView2 runtime. Microsoft's installer does
rem not run on Vista, and version 109 was the last for Windows 7. Instead use
rem a "Fixed Version" runtime (a plain folder, downloaded from Microsoft and
rem extracted with: expand <file>.cab -F:* <folder>). This script copies it to
rem Program Files, patches it with VistaExt and tells VistaExt where it is.
rem Then, in VistaExt Center, tick "Uses Microsoft WebView2" for the program.
setlocal
set "ADMIN="
net session >nul 2>&1 && set "ADMIN=1"
if not defined ADMIN fltmc >nul 2>&1 && set "ADMIN=1"
if not defined ADMIN (echo Please run this from an administrator command prompt. & exit /b 1)
if "%~1"=="" (findstr /b "rem" "%~f0" & exit /b 1)
set "SRC=%~f1"
set "ARCH=%~2"
if "%ARCH%"=="" echo %SRC% | find /i "x64" >nul && set ARCH=x64
if "%ARCH%"=="" echo %SRC% | find /i "x86" >nul && set ARCH=x86
if "%ARCH%"=="" (echo Add x86 or x64 after the folder name. & exit /b 1)
if not exist "%SRC%\msedgewebview2.exe" (echo "%SRC%" does not contain msedgewebview2.exe & exit /b 1)

set "VX=%ProgramFiles%\VistaExt"
if not exist "%VX%\vxpatch.exe" (echo VistaExt is not installed. Run install.cmd first. & exit /b 1)
set "DEST=%VX%\WebView2\%ARCH%"
echo Copying the runtime to %DEST% ...
xcopy "%SRC%" "%DEST%\" /e /i /q /y >nul || (echo Copy failed. & exit /b 1)

echo Patching the runtime for this Windows version ...
"%VX%\vxpatch.exe" /q /wv2runtime:%ARCH% "%DEST%"
if errorlevel 2 exit /b 1
echo.
echo Done. In VistaExt Center, choose the WebView2 program and tick
echo "Uses Microsoft WebView2", then click Apply.
endlocal

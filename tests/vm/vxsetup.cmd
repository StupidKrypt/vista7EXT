@echo off
rem One-time preparation of the disposable test VM (run by autounattend.xml at
rem the first logon, from the setup CD; %1 is that CD's drive).
rem Makes the VM run unattended: permanent automatic logon, no error pop-ups,
rem no sleep, and a logon hook that runs a test CD's vxvm\run.cmd if one is
rem inserted. Then it shuts down; vm.sh keeps the disk as the clean base.
mode COM1:115200,n,8,1 >nul 2>&1
>COM1 echo VXSETUP start

set "WL=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
reg add "%WL%" /v AutoAdminLogon /d 1 /f >nul
reg add "%WL%" /v DefaultUserName /d Administrator /f >nul
reg add "%WL%" /v DefaultPassword /d "" /f >nul
reg delete "%WL%" /v AutoLogonCount /f >nul 2>&1

rem no "has stopped working" / "entry point not found" boxes waiting for a click
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting" /v DontShowUI /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting" /v Disabled /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Windows" /v ErrorMode /t REG_DWORD /d 2 /f >nul
sc config PcaSvc start= disabled >nul 2>&1

rem never sleep, no screen saver, no welcome windows
powercfg -change -monitor-timeout-ac 0 >nul 2>&1
powercfg -change -standby-timeout-ac 0 >nul 2>&1
powercfg -change -hibernate-timeout-ac 0 >nul 2>&1
powercfg -h off >nul 2>&1
reg add "HKCU\Control Panel\Desktop" /v ScreenSaveActive /d 0 /f >nul
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v WindowsWelcomeCenter /f >nul 2>&1
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v StartMenuInit /t REG_DWORD /d 1 /f >nul

copy /y "%~1\vxboot.cmd" "%SystemDrive%\vxboot.cmd" >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v vxboot /d "%SystemDrive%\vxboot.cmd" /f >nul
echo prepared> "%SystemDrive%\vxsetup.done"

>COM1 echo VXSETUP done
shutdown /s /t 5 /f

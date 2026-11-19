@echo off
rem Runs at every logon of the test VM: if a test CD is inserted, run it.
mode COM1:115200,n,8,1 >nul 2>&1
>COM1 echo VXBOOT logon
for %%i in (D E F G H I J K L M N O P) do if exist %%i:\vxvm\run.cmd (
    call %%i:\vxvm\run.cmd %%i:
    goto :eof
)
>COM1 echo VXBOOT no test CD

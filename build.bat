@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set "WDK_VER=10.0.26100.0"
set WDK_INC=C:\Program Files (x86)\Windows Kits\10\Include\%WDK_VER%
set WDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\%WDK_VER%

echo [BUILDING DRIVER]
set CFLAGS=/nologo /W3 /WX- /Od /Oy /D_AMD64_ /D_WIN64 /D_KERNEL_MODE /D_WIN32_WINNT=0x0A00 /Gy /Zc:threadSafeInit- /GF /Zp8 /GS- /Z7 /I"%WDK_INC%\km" /I"%WDK_INC%\shared" /I"%WDK_INC%\um" /I"%WDK_INC%\km\crt" /kernel
set LDFLAGS=/nologo /driver /subsystem:native /base:0x10000 /entry:DriverEntry /nodefaultlib /dynamicbase /nxcompat /osversion:10.0 "%WDK_LIB%\km\x64\ntoskrnl.lib" "%WDK_LIB%\km\x64\hal.lib" "%WDK_LIB%\km\x64\wmilib.lib" "%WDK_LIB%\km\x64\Netio.lib"

cl.exe /c %CFLAGS% src\main.c src\memory.c src\apc.c src\aes_gcm.c src\payload.c
link.exe %LDFLAGS% /out:RainHack.sys main.obj memory.obj apc.obj aes_gcm.obj payload.obj


@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist cheat\ext mkdir cheat\ext

echo [DOWNLOADING MINHOOK]
powershell -Command "if (-not (Test-Path 'cheat\ext\minhook')) { Invoke-WebRequest -Uri 'https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.zip' -OutFile 'minhook.zip'; Expand-Archive -Path 'minhook.zip' -DestinationPath 'cheat\ext'; Rename-Item -Path 'cheat\ext\minhook-master' -NewName 'minhook'; Remove-Item 'minhook.zip' }"

echo [BUILDING INJECTOR]
cd injector
taskkill /F /IM RainHack_Injector_V2.exe >nul 2>&1
cl.exe /nologo /EHsc /std:c++17 /O2 main.cpp injector.cpp User32.lib Kernel32.lib Advapi32.lib Comdlg32.lib /link /SUBSYSTEM:WINDOWS /OUT:RainHack_Injector_V2.exe
cd ..

echo [BUILDING CHEAT DLL]
cd cheat
set MINHOOK_SRC=ext\minhook\src\buffer.c ext\minhook\src\hook.c ext\minhook\src\trampoline.c ext\minhook\src\hde\hde64.c ext\minhook\src\hde\hde32.c
set IMGUI_SRC=ext\imgui\imgui.cpp ext\imgui\imgui_draw.cpp ext\imgui\imgui_tables.cpp ext\imgui\imgui_widgets.cpp ext\imgui\backends\imgui_impl_dx11.cpp ext\imgui\backends\imgui_impl_win32.cpp
set SOURCES=main.cpp features\esp\esp.cpp features\aim\aim.cpp features\Menu\Menu.cpp features\world\world.cpp features\misc\misc.cpp features\combat\combat.cpp %MINHOOK_SRC% %IMGUI_SRC%

cl.exe /nologo /LD /EHa /std:c++17 /O2 /MT /DNDEBUG /D_USRDLL /D_WINDLL /I ext\minhook\include /I ext\imgui /I ext\imgui\backends %SOURCES% User32.lib Kernel32.lib d3d11.lib dxgi.lib /link /OUT:cheat.dll
cd ..

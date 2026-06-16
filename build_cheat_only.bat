@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

echo [BUILDING CHEAT DLL - FREE BUILD: ESP ONLY]
cd cheat

del /q world.obj  2>nul
del /q misc.obj   2>nul
del /q combat.obj 2>nul
del /q farm.obj   2>nul

if exist features\font\resources.rc (
    echo [RC] compiling features\font\resources.rc
    rc.exe /nologo /fo features\font\resources.res features\font\resources.rc
    if errorlevel 1 (
        echo [ERROR] rc.exe failed
        cd ..
        exit /b 1
    )
)

set MINHOOK_SRC=ext\minhook\src\buffer.c ext\minhook\src\hook.c ext\minhook\src\trampoline.c ext\minhook\src\hde\hde64.c ext\minhook\src\hde\hde32.c
set IMGUI_SRC=ext\imgui\imgui.cpp ext\imgui\imgui_draw.cpp ext\imgui\imgui_tables.cpp ext\imgui\imgui_widgets.cpp ext\imgui\backends\imgui_impl_dx11.cpp ext\imgui\backends\imgui_impl_win32.cpp
set SOURCES=main.cpp features\esp\esp.cpp features\aim\aim.cpp features\Menu\Menu.cpp %MINHOOK_SRC% %IMGUI_SRC%

set RESFILE=
if exist features\font\resources.res set RESFILE=features\font\resources.res

cl.exe /nologo /LD /EHa /std:c++17 /O2 /MT /GS- /Gy /DNDEBUG /D_USRDLL /D_WINDLL ^
    /I ext\minhook\include /I ext\imgui /I ext\imgui\backends /I features\font ^
    %SOURCES% ^
    User32.lib Kernel32.lib d3d11.lib dxgi.lib ^
    /link /OUT:cheat.dll /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF %RESFILE%

if errorlevel 1 (
    echo [ERROR] cl.exe failed
    cd ..
    exit /b 1
)

echo [OK] cheat.dll built
cd ..

if exist "cheat\cheat.dll" (
    if not exist "Loader\rc" mkdir "Loader\rc" 2>nul
    copy /y "cheat\cheat.dll" "Loader\rc\cheat.dll" >nul
    if errorlevel 1 (
        echo [WARN] failed to sync cheat.dll to Loader\rc\
    ) else (
        echo [SYNC] cheat\cheat.dll -^> Loader\rc\cheat.dll
    )
)
endlocal

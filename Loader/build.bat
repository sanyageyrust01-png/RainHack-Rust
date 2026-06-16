@echo off
setlocal enabledelayedexpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "RC_DIR=%~dp0rc"
set "OUT_RES=%~dp0Loader\resources"
set "ENC_PY=%~dp0Loader\tools\encrypt_blob.py"

if not exist "!RC_DIR!\cheat.dll" (
    echo [ERR] missing "!RC_DIR!\cheat.dll"
    exit /b 2
)
if not exist "!RC_DIR!\RainHack.sys" (
    echo [ERR] missing "!RC_DIR!\RainHack.sys"
    exit /b 2
)
if not exist "!RC_DIR!\kdmapper.exe" (
    echo [ERR] missing "!RC_DIR!\kdmapper.exe"
    exit /b 2
)
if not exist "!ENC_PY!" (
    echo [ERR] missing "!ENC_PY!"
    exit /b 2
)

where python >nul 2>nul
if errorlevel 1 (
    echo [ERR] Python not on PATH. Install Python 3 and 'pip install cryptography'.
    exit /b 3
)

echo [..] Encrypting payloads with 3-layer pipeline
python "!ENC_PY!" ^
    --cheat  "!RC_DIR!\cheat.dll" ^
    --driver "!RC_DIR!\RainHack.sys" ^
    --kdm    "!RC_DIR!\kdmapper.exe" ^
    --out-dir "!OUT_RES!"
if errorlevel 1 (
    echo [ERR] encrypt_blob.py failed
    exit /b 4
)

if not exist "!OUT_RES!\alpha.bin" (echo [ERR] alpha.bin not produced & exit /b 5)
if not exist "!OUT_RES!\beta.bin"  (echo [ERR] beta.bin not produced  & exit /b 5)
if not exist "!OUT_RES!\gamma.bin" (echo [ERR] gamma.bin not produced & exit /b 5)

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERR] vswhere.exe not found. Install Visual Studio 2022 with C++ workload.
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath"`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo [ERR] No Visual Studio installation with MSBuild found.
    exit /b 1
)

set "MSBUILD=!VSPATH!\MSBuild\Current\Bin\MSBuild.exe"
if not exist "!MSBUILD!" (
    echo [ERR] MSBuild.exe not found.
    echo        Expected: "!MSBUILD!"
    exit /b 1
)

echo [..] Building Loader ^(!CONFIG! x64^)
"!MSBUILD!" "%~dp0Loader.sln" /m /p:Configuration=!CONFIG! /p:Platform=x64 /v:m
if errorlevel 1 (
    echo [ERR] Build failed.
    exit /b 1
)

echo.
echo [OK] Output: "%~dp0build\!CONFIG!\RainHackLoader.exe"
endlocal

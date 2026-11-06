@echo off
chcp 65001 >nul
title Hekpixle - C++ Build
cd /d "%~dp0"

rem Needs: CMake + Visual Studio (or Build Tools) with "Desktop development with C++".
rem Works from a normal cmd window AND from a "Developer Command Prompt". No git/vcpkg needed.
rem First run downloads the WebView2 SDK from nuget.org (internet / VPN required once).

where cmake >nul 2>nul
if errorlevel 1 (
  echo [error] CMake is not installed. Download: https://cmake.org/download   ^(tick "Add CMake to PATH"^)
  goto fail
)

rem --- Make sure the MSVC compiler (cl.exe) is available: load it with vcvars64 if needed ---
where cl >nul 2>nul
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_vs
echo [info] Using Visual Studio at: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto no_vs

:have_cl
if exist build rmdir /s /q build

echo [1/2] Configuring...
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto fail

echo [2/2] Building...
cmake --build build
if errorlevel 1 goto fail

if not exist "build\out\Hekpixle.exe" (
  echo [error] build\out\Hekpixle.exe not found.
  goto fail
)
echo.
echo Done!  Folder: %~dp0build\out   ^(Hekpixle.exe + WebView2Loader.dll + renderer\ ...^)
echo Keep the whole folder together; the exe needs the files next to it.
start "" "%~dp0build\out"
pause
exit /b 0

:no_vs
echo [error] The C++ compiler was not found.
echo         Install "Visual Studio Build Tools" and tick "Desktop development with C++":
echo         https://visualstudio.microsoft.com/downloads/   ^(Tools for Visual Studio ^> Build Tools^)

:fail
echo.
echo Build failed. Copy the red error lines above and send them to get a fix.
pause
exit /b 1

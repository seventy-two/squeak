@echo off
rem Builds build\squeak.exe and runs the tests. Needs Visual Studio 2022
rem (or its Build Tools) with the C++ workload. Run from any prompt.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio with C++ tools not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build\obj mkdir build\obj
if not exist build\tests mkdir build\tests
set CFLAGS=/nologo /O2 /utf-8 /W3 /MP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Isrc /Ithird_party /Ithird_party\imgui /Ithird_party\imgui\backends
set CXXFLAGS=%CFLAGS% /std:c++17 /EHsc

echo Building tests...
cl %CXXFLAGS% /Fobuild\tests\ /Fe:build\tests.exe tests\tests.cpp src\protocol.cpp > build\tests.log 2>&1 || (type build\tests.log & exit /b 1)
build\tests.exe || exit /b 1

echo Building squeak.exe...
rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl %CXXFLAGS% /Fobuild\obj\ /Fe:build\squeak.exe src\main.cpp src\app.cpp src\devices.cpp src\hid.cpp src\protocol.cpp third_party\imgui\imgui*.cpp third_party\imgui\backends\*.cpp build\app.res ^
    /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib advapi32.lib hid.lib setupapi.lib > build\build.log 2>&1 || (type build\build.log & exit /b 1)
findstr /i /c:"warning" build\build.log
echo Built build\squeak.exe

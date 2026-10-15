@echo off
setlocal
set "RT_ROOT=%~dp0..\.."
set "RT_SDK=%RT_ROOT%\reshade_addon\sdk\include"
if not exist "%RT_SDK%\reshade.hpp" (
    echo Fetching the pinned ReShade SDK...
    git -C "%RT_ROOT%" submodule update --init reshade_addon/sdk
    if errorlevel 1 exit /b 1
)
where cl.exe >nul 2>nul
if errorlevel 1 (
    set "RT_VS="
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "RT_VS=%%i"
    call :setup_compiler
    if errorlevel 1 exit /b 1
)
set "RT_OUT=%RT_ROOT%\build_addon\rt_capture"
if not exist "%RT_OUT%" mkdir "%RT_OUT%"
cl.exe /nologo /std:c++20 /EHsc /O2 /W4 /WX /MT /LD /external:W0 /external:I "%RT_SDK%" "%~dp0rt_capture.cpp" /Fo"%RT_OUT%\rt_capture.obj" /link /INCREMENTAL:NO /OUT:"%RT_OUT%\wwmi_rt_capture.addon64" user32.lib psapi.lib
if errorlevel 1 exit /b 1
echo Built: %RT_OUT%\wwmi_rt_capture.addon64
exit /b 0

:setup_compiler
if not defined RT_VS (
    echo Visual Studio 2022 C++ desktop tools and a Windows SDK are required.
    exit /b 1
)
call "%RT_VS%\VC\Auxiliary\Build\vcvars64.bat"
exit /b %errorlevel%

@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Build Tools were not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo The Visual C++ x64 tools were not found.
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
set IMGUI=third_party\imgui
set LZ4=third_party\lz4
set TJ=third_party\libjpeg-turbo\sdk
set APP_SRC=src\main.cpp src\app.cpp src\app_gui.cpp src\app_window.cpp src\format_menu.cpp src\preview_renderer.cpp src\directshow_capture.cpp src\desktop_capture.cpp src\source_timing.cpp src\audio_capture.cpp src\async_sender.cpp src\async_calibrate.cpp src\field_prep.cpp src\field_prep_avx2.cpp src\groovy_sender.cpp src\latency_probe.cpp src\rate_lock.cpp
set IMGUI_SRC=%IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp %IMGUI%\backends\imgui_impl_win32.cpp %IMGUI%\backends\imgui_impl_dx11.cpp
rc /nologo /fo build\groovygrab.res src\groovygrab.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W4 /permissive- /MP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%LZ4%" /I"%IMGUI%" /I"%IMGUI%\backends" /I"%TJ%\include" /Fo:build\ /Fe:build\GroovyGrab.exe %APP_SRC% %LZ4%\lz4.c %IMGUI_SRC% /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup build\groovygrab.res strmiids.lib ole32.lib oleaut32.lib d3d11.lib dxgi.lib d3dcompiler.lib avrt.lib user32.lib gdi32.lib ws2_32.lib winmm.lib dwmapi.lib cfgmgr32.lib comctl32.lib "%TJ%\lib\turbojpeg.lib"
set BUILD_RESULT=%errorlevel%
del /q build\*.obj build\*.res 2>nul
if not "%BUILD_RESULT%"=="0" exit /b %BUILD_RESULT%
copy /y "%TJ%\bin\turbojpeg.dll" build\ >nul
exit /b %errorlevel%

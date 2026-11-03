@echo off
setlocal EnableDelayedExpansion
title INFRA VR build

rem Builds INFRA VR into one ready-to-install folder, dist\INFRA-VR\, laid out like the INFRA folder
rem it installs into (the way Half-Life 2 VR and Black Mesa VR ship):
rem   install.bat, uninstall.bat, infravr.bat (VR launcher)
rem   bin\d3d9.dll (the mod), bin\dxvk_d3d9.dll (DXVK, 32-bit), bin\openvr_api.dll (OpenVR, 32-bit)
rem   infravr\ (the mod folder: cfg\ settings, cfg\steamvr\ bindings, models\hands\ hand models,
rem   bin\ the 64-bit OpenXR helper with Khronos' loader), licenses\
rem
rem Needs Visual Studio 2022 (or newer) with "Desktop development with C++", and Windows 10/11
rem (curl and tar are built in). DXVK, OpenVR and the OpenXR loader are downloaded once into
rem build-downloads\.
rem
rem Usage:
rem   build.bat            build the folder
rem   build.bat install    build it, then run its install.bat (finds INFRA automatically)
rem The version written to infravr.log comes from the INFRAVR_VERSION environment variable
rem (default "dev").

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\dist\INFRA-VR"
set "DL=%ROOT%\build-downloads"
set "DXVK_VER=2.7.1"
set "OPENVR_VER=2.15.6"
set "OPENXR_VER=1.1.43"
if "%INFRAVR_VERSION%"=="" set "INFRAVR_VERSION=dev"

rem --- CMake: on PATH, or the copy that comes with Visual Studio
set "CMAKE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" goto :novs
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
    if not defined VS goto :novs
    set "CMAKE=!VS!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE!" goto :novs
)

echo [1/4] Building the mod (32-bit d3d9.dll)
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build-vs" -A Win32 -DINFRAVR_VERSION=%INFRAVR_VERSION% >nul || goto :fail
"%CMAKE%" --build "%ROOT%\build-vs" --config Release --target infravr -- /nologo /v:minimal || goto :fail

echo [2/4] Building the OpenXR helper (64-bit infravr_openxr.exe)
"%CMAKE%" -S "%ROOT%\helper\openxr" -B "%ROOT%\build-vs-xr" -A x64 >nul || goto :fail
"%CMAKE%" --build "%ROOT%\build-vs-xr" --config Release -- /nologo /v:minimal || goto :fail

echo [3/4] Getting DXVK %DXVK_VER%, OpenVR %OPENVR_VER% and the OpenXR loader %OPENXR_VER%
if not exist "%DL%" mkdir "%DL%"
if not exist "%DL%\dxvk_d3d9.dll" (
    curl -fsSL -o "%DL%\dxvk.tar.gz" "https://github.com/doitsujin/dxvk/releases/download/v%DXVK_VER%/dxvk-%DXVK_VER%.tar.gz" || goto :faildl
    if exist "%DL%\dxvk" rmdir /s /q "%DL%\dxvk"
    mkdir "%DL%\dxvk"
    tar -xf "%DL%\dxvk.tar.gz" -C "%DL%\dxvk" || goto :faildl
    copy /y "%DL%\dxvk\dxvk-%DXVK_VER%\x32\d3d9.dll" "%DL%\dxvk_d3d9.dll" >nul || goto :faildl
)
if not exist "%DL%\openvr_api.dll" (
    curl -fsSL -o "%DL%\openvr_api.dll" "https://raw.githubusercontent.com/ValveSoftware/openvr/v%OPENVR_VER%/bin/win32/openvr_api.dll" || goto :faildl
)
if not exist "%DL%\openxr_loader.dll" (
    curl -fsSL -o "%DL%\openxr.zip" "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VER%/openxr_loader_windows-%OPENXR_VER%.zip" || goto :faildl
    if exist "%DL%\openxr" rmdir /s /q "%DL%\openxr"
    mkdir "%DL%\openxr"
    tar -xf "%DL%\openxr.zip" -C "%DL%\openxr" || goto :faildl
    copy /y "%DL%\openxr\x64\bin\openxr_loader.dll" "%DL%\openxr_loader.dll" >nul || goto :faildl
    if not exist "%DL%\openxr_loader.dll" goto :faildl
)

echo [4/4] Putting it together in dist\INFRA-VR
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\bin" "%OUT%\infravr\bin" "%OUT%\licenses" || goto :fail
copy /y "%ROOT%\dist-src\install.bat" "%OUT%\" >nul || goto :fail
copy /y "%ROOT%\dist-src\uninstall.bat" "%OUT%\" >nul || goto :fail
copy /y "%ROOT%\dist-src\infravr.bat" "%OUT%\" >nul || goto :fail
copy /y "%ROOT%\README.md" "%OUT%\" >nul
copy /y "%ROOT%\build-vs\Release\d3d9.dll" "%OUT%\bin\" >nul || goto :fail
copy /y "%DL%\dxvk_d3d9.dll" "%OUT%\bin\" >nul || goto :fail
copy /y "%DL%\openvr_api.dll" "%OUT%\bin\" >nul || goto :fail
rem The repository's infravr\ folder is the mod folder as installed (cfg, cfg\steamvr, models).
xcopy /e /i /q /y "%ROOT%\infravr" "%OUT%\infravr" >nul || goto :fail
copy /y "%ROOT%\build-vs-xr\Release\infravr_openxr.exe" "%OUT%\infravr\bin\" >nul || goto :fail
copy /y "%DL%\openxr_loader.dll" "%OUT%\infravr\bin\" >nul || goto :fail
copy /y "%ROOT%\licenses\*.txt" "%OUT%\licenses\" >nul

echo.
echo Done: %OUT%
echo Copy that folder anywhere and run install.bat in it, or run  build.bat install
if /i "%~1"=="install" (
    echo.
    call "%OUT%\install.bat"
)
exit /b 0

:novs
echo Visual Studio 2022 with "Desktop development with C++" was not found (and cmake is not on PATH).
pause
exit /b 1

:faildl
echo Download failed - check the internet connection, or delete build-downloads\ and try again.
pause
exit /b 1

:fail
echo Build failed - see the messages above.
pause
exit /b 1

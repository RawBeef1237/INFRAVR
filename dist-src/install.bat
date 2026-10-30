@echo off
setlocal EnableDelayedExpansion
title INFRA VR installer

rem Installs INFRA VR into INFRA\bin:
rem   bin\d3d9.dll          the mod (Source loads d3d9.dll from the bin folder)
rem   bin\dxvk_d3d9.dll     DXVK 2.7.1, 32-bit (renders D3D9 through Vulkan)
rem   bin\openvr_api.dll    Valve's OpenVR runtime client, 32-bit
rem   bin\infravr\          settings + SteamVR controller bindings
rem   bin\infravr\hands\    the VR hand models (glTF .glb)
rem   bin\infravr\openxr\   the 64-bit OpenXR helper (only used with backend = openxr)
rem Usage: double-click, or  install.bat "D:\SteamLibrary\steamapps\common\infra"

set "HERE=%~dp0"
set "GAME=%~1"

if "%GAME%"=="" if exist "C:\Program Files (x86)\Steam\steamapps\common\infra\infra.exe" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\infra"
if "%GAME%"=="" (
    for /f "tokens=2*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul') do set "STEAM=%%b"
    if defined STEAM (
        set "STEAM=!STEAM:/=\!"
        if exist "!STEAM!\steamapps\common\infra\infra.exe" set "GAME=!STEAM!\steamapps\common\infra"
        if not defined GAME if exist "!STEAM!\steamapps\libraryfolders.vdf" (
            for /f "tokens=2 delims=	 " %%p in ('findstr /c:"\"path\"" "!STEAM!\steamapps\libraryfolders.vdf"') do (
                set "LIB=%%~p"
                set "LIB=!LIB:\\=\!"
                if exist "!LIB!\steamapps\common\infra\infra.exe" set "GAME=!LIB!\steamapps\common\infra"
            )
        )
    )
)
if "%GAME%"=="" (
    echo Could not find INFRA automatically.
    set /p "GAME=Paste the folder that contains infra.exe: "
)
set "GAME=%GAME:"=%"
if not exist "%GAME%\infra.exe" (
    echo infra.exe was not found in "%GAME%".
    pause
    exit /b 1
)
set "BIN=%GAME%\bin"
echo INFRA found at: %GAME%
echo.

rem --- clean up earlier INFRA VR versions (v1 files next to infra.exe, v2 OpenXR helper)
for %%f in (d3d9.dll dxvk_d3d9.dll wineopenxr.dll openxr_loader.dll infravr.ini dxvk.conf infravr_xr64.log infravr_wineopenxr.log) do (
    if exist "%GAME%\%%f" (del /q "%GAME%\%%f" & echo   removed old %%f from the INFRA folder)
)
if exist "%GAME%\infravr\infravr_xr64.exe" (rmdir /s /q "%GAME%\infravr" & echo   removed the old OpenXR helper folder)
if exist "%BIN%\infravr.ini" (del /q "%BIN%\infravr.ini" & echo   removed old bin\infravr.ini)

rem --- back up a d3d9.dll that is not ours
if exist "%BIN%\d3d9.dll" (
    findstr /m /c:"INFRA VR" "%BIN%\d3d9.dll" >nul 2>&1 || (
        copy /y "%BIN%\d3d9.dll" "%BIN%\d3d9.dll.before-infravr" >nul
        echo   backed up your existing bin\d3d9.dll to d3d9.dll.before-infravr
    )
)

for %%f in (d3d9.dll dxvk_d3d9.dll openvr_api.dll) do (
    copy /y "%HERE%bin\%%f" "%BIN%\%%f" >nul || (echo Failed to copy %%f - is INFRA still running? & pause & exit /b 1)
    echo   copied bin\%%f
)
if not exist "%BIN%\infravr" mkdir "%BIN%\infravr"
if not exist "%BIN%\infravr\actions" mkdir "%BIN%\infravr\actions"
copy /y "%HERE%bin\infravr\actions\*.json" "%BIN%\infravr\actions\" >nul && echo   copied bin\infravr\actions (SteamVR bindings)
copy /y "%HERE%bin\infravr\dxvk.conf" "%BIN%\infravr\" >nul && echo   copied bin\infravr\dxvk.conf
if not exist "%BIN%\infravr\hands" mkdir "%BIN%\infravr\hands"
copy /y "%HERE%bin\infravr\hands\*.glb" "%BIN%\infravr\hands\" >nul && echo   copied bin\infravr\hands (VR hand models)
if exist "%HERE%bin\infravr\openxr\infravr_openxr.exe" (
    if not exist "%BIN%\infravr\openxr" mkdir "%BIN%\infravr\openxr"
    copy /y "%HERE%bin\infravr\openxr\*.*" "%BIN%\infravr\openxr\" >nul && echo   copied bin\infravr\openxr ^(OpenXR helper^)
)
rem Mechanisms that crashed an earlier build are not held against this one.
if exist "%GAME%\infravr_skip.txt" (del /q "%GAME%\infravr_skip.txt" & echo   cleared infravr_skip.txt)
if exist "%BIN%\infravr\infravr.ini" (
    copy /y "%HERE%bin\infravr\infravr.ini" "%BIN%\infravr\infravr.ini.new" >nul
    echo   kept your bin\infravr\infravr.ini ^(new defaults in infravr.ini.new^)
) else (
    copy /y "%HERE%bin\infravr\infravr.ini" "%BIN%\infravr\" >nul && echo   copied bin\infravr\infravr.ini
)

echo.
echo Done.
echo  1. Start SteamVR (Quest: Link/Air Link/Virtual Desktop with SteamVR).
echo  2. In Steam set INFRA's launch options to:
echo        -windowed -novid +mat_queue_mode 0
echo  3. Start INFRA. Menus appear on a big screen in front of you;
echo     point a controller at it and pull the trigger to click.
echo Log: %GAME%\infravr.log
pause

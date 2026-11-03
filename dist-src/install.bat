@echo off
setlocal EnableDelayedExpansion
title INFRA VR installer

rem Installs INFRA VR the way Half-Life 2 VR and Black Mesa VR are laid out: the engine-level DLLs
rem in bin\, and everything else in one mod folder next to the game's own content folder.
rem   bin\d3d9.dll              the mod (Source loads d3d9.dll from the bin folder)
rem   bin\dxvk_d3d9.dll         DXVK 2.7.1, 32-bit (renders D3D9 through Vulkan)
rem   bin\openvr_api.dll        Valve's OpenVR runtime client, 32-bit
rem   infravr\cfg\             infravr.ini, dxvk.conf
rem   infravr\cfg\steamvr\     SteamVR action manifest + controller bindings
rem   infravr\models\hands\    the VR hand models (glTF .glb)
rem   infravr\bin\             the 64-bit OpenXR helper (only used with backend = openxr)
rem   infravr.bat              starts INFRA with the VR launch options
rem The mod writes its logs and crash flags into infravr\ as well.
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

set "MOD=%GAME%\infravr"

rem --- clean up earlier INFRA VR layouts (files next to infra.exe, or everything in bin\infravr\)
for %%f in (d3d9.dll dxvk_d3d9.dll wineopenxr.dll openxr_loader.dll infravr.ini dxvk.conf infravr_xr64.log infravr_wineopenxr.log infravr.log infravr_openxr.log infravr_skip.txt) do (
    if exist "%GAME%\%%f" (del /q "%GAME%\%%f" & echo   removed old %%f from the INFRA folder)
)
if exist "%GAME%\infravr_in_*.flag" del /q "%GAME%\infravr_in_*.flag"
rem An early build put only its OpenXR helper in INFRA\infravr\; that folder is the mod folder now.
if exist "%MOD%\infravr_xr64.exe" (rmdir /s /q "%MOD%" & echo   removed the old OpenXR helper folder)
if not exist "%MOD%\cfg" mkdir "%MOD%\cfg"
if exist "%BIN%\infravr.ini" (del /q "%BIN%\infravr.ini" & echo   removed old bin\infravr.ini)
if exist "%BIN%\infravr\infravr.ini" if not exist "%MOD%\cfg\infravr.ini" (
    copy /y "%BIN%\infravr\infravr.ini" "%MOD%\cfg\infravr.ini" >nul && echo   moved your settings from bin\infravr\infravr.ini to infravr\cfg\infravr.ini
)
if exist "%BIN%\infravr" (rmdir /s /q "%BIN%\infravr" & echo   removed the old bin\infravr\ folder)

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
if not exist "%MOD%\cfg\steamvr" mkdir "%MOD%\cfg\steamvr"
copy /y "%HERE%infravr\cfg\steamvr\*.json" "%MOD%\cfg\steamvr\" >nul && echo   copied infravr\cfg\steamvr (SteamVR bindings)
copy /y "%HERE%infravr\cfg\dxvk.conf" "%MOD%\cfg\" >nul && echo   copied infravr\cfg\dxvk.conf
if not exist "%MOD%\models\hands" mkdir "%MOD%\models\hands"
copy /y "%HERE%infravr\models\hands\*.*" "%MOD%\models\hands\" >nul && echo   copied infravr\models\hands (VR hand models)
if exist "%HERE%infravr\bin\infravr_openxr.exe" (
    if not exist "%MOD%\bin" mkdir "%MOD%\bin"
    copy /y "%HERE%infravr\bin\*.*" "%MOD%\bin\" >nul && echo   copied infravr\bin ^(OpenXR helper^)
)
copy /y "%HERE%infravr.bat" "%GAME%\infravr.bat" >nul && echo   copied infravr.bat (VR launcher)
rem Mechanisms that crashed an earlier build are not held against this one.
if exist "%MOD%\infravr_skip.txt" (del /q "%MOD%\infravr_skip.txt" & echo   cleared infravr\infravr_skip.txt)
if exist "%MOD%\infravr_in_*.flag" del /q "%MOD%\infravr_in_*.flag"
if exist "%MOD%\cfg\infravr.ini" (
    copy /y "%HERE%infravr\cfg\infravr.ini" "%MOD%\cfg\infravr.ini.new" >nul
    echo   kept your infravr\cfg\infravr.ini ^(new defaults in infravr.ini.new^)
) else (
    copy /y "%HERE%infravr\cfg\infravr.ini" "%MOD%\cfg\" >nul && echo   copied infravr\cfg\infravr.ini
)

rem --- 4GB patch: set the Large Address Aware flag on the 32-bit infra.exe so it can use 4 GB of
rem     memory instead of 2 GB. The original is kept as infra.exe.before-infravr-4gb (uninstall.bat
rem     puts it back). Nothing is changed if the flag is already set.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$f = Join-Path $env:GAME 'infra.exe'; try { $b = [IO.File]::ReadAllBytes($f); $pe = [BitConverter]::ToInt32($b, 60); if ($b[0] -ne 77 -or $b[1] -ne 90 -or [BitConverter]::ToUInt32($b, $pe) -ne 17744) { '  4GB patch skipped: infra.exe is not a Windows program'; exit 0 }; if ([BitConverter]::ToUInt16($b, $pe + 4) -ne 332) { '  4GB patch not needed: infra.exe is not 32-bit'; exit 0 }; $o = $pe + 22; if ($b[$o] -band 32) { '  infra.exe already has the 4GB patch'; exit 0 }; Copy-Item -LiteralPath $f -Destination ($f + '.before-infravr-4gb') -Force; $b[$o] = $b[$o] -bor 32; [IO.File]::WriteAllBytes($f, $b); '  applied the 4GB patch to infra.exe (original kept as infra.exe.before-infravr-4gb)' } catch { '  4GB patch failed: ' + $_.Exception.Message }"

echo.
echo Done.
echo  1. Start SteamVR (Quest: Link/Air Link/Virtual Desktop with SteamVR).
echo  2. Start INFRA with infravr.bat in the INFRA folder, or set INFRA's
echo     launch options in Steam to:  -windowed -novid +mat_queue_mode 0
echo  3. Menus appear on a big screen in front of you;
echo     point a controller at it and pull the trigger to click.
echo Settings: %MOD%\cfg\infravr.ini
echo Log:      %MOD%\infravr.log
pause

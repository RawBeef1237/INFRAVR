@echo off
setlocal
title INFRA VR uninstaller
set "GAME=%~1"
if "%GAME%"=="" if exist "C:\Program Files (x86)\Steam\steamapps\common\infra\infra.exe" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\infra"
if "%GAME%"=="" set /p "GAME=Paste the folder that contains infra.exe: "
set "GAME=%GAME:"=%"
if not exist "%GAME%\infra.exe" (echo infra.exe not found in "%GAME%" & pause & exit /b 1)
for %%f in (bin\d3d9.dll bin\dxvk_d3d9.dll bin\openvr_api.dll bin\infravr.ini) do (
    if exist "%GAME%\%%f" del /q "%GAME%\%%f" && echo   removed %%f
)
if exist "%GAME%\bin\infravr" rmdir /s /q "%GAME%\bin\infravr" && echo   removed bin\infravr\
if exist "%GAME%\bin\d3d9.dll.before-infravr" (
    ren "%GAME%\bin\d3d9.dll.before-infravr" d3d9.dll && echo   restored your previous bin\d3d9.dll
)
echo INFRA is back to normal (infravr.log is left in the INFRA folder).
pause

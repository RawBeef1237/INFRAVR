@echo off
rem INFRA VR launcher - starts INFRA with the launch options VR needs (like Half-Life 2 VR's hlvr.bat).
rem Start SteamVR first. Steam must be running. Add your own options below, or pass them to this file.
set "params=-windowed -novid +mat_queue_mode 0"
cd /d "%~dp0"
start "" "%~dp0infra.exe" %params% %*

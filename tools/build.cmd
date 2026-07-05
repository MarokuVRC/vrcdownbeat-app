@echo off
rem Build the app. Run tools\configure.cmd once first.
taskkill /IM BandJam.exe /F >nul 2>&1
call "%~dp0env.cmd" cmake --build "%~dp0..\build" --target BandJam %*

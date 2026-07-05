@echo off
rem Builds the standalone Release exe (static runtime - just copy it anywhere).
rem Output: build-release\BandJam_artefacts\Release\BandJam.exe

taskkill /f /im BandJam.exe >nul 2>&1

call "%~dp0env.cmd" cmake -S "%~dp0.." -B "%~dp0..\build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

call "%~dp0env.cmd" cmake --build "%~dp0..\build-release"

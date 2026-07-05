@echo off
rem One-time CMake configure (Ninja, Debug). Downloads JUCE on first run.
call "%~dp0env.cmd" cmake -S "%~dp0.." -B "%~dp0..\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug

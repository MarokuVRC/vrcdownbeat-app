@echo off
rem Sets up the MSVC x64 toolchain environment, then runs the given command.
if not defined VSCMD_VER call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
%*

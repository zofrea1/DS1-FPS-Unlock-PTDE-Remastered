@echo off
setlocal
cd /d "%~dp0"
rem "build.bat release" passes release on: both DLLs without the [Diagnostics] tools (src\common\buildcfg.h).
call "%~dp0build_dsr.bat" %1
if errorlevel 1 exit /b 1
call "%~dp0build_ptde.bat" %1
if errorlevel 1 exit /b 1
echo Built both targets.

@echo off
setlocal
cd /d "%~dp0"
call "%~dp0build_dsr.bat"
if errorlevel 1 exit /b 1
call "%~dp0build_ptde.bat"
if errorlevel 1 exit /b 1
echo Built both targets.

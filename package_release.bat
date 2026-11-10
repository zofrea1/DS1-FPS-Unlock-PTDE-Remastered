@echo off
setlocal
cd /d "%~dp0"
set VERSION=2.1.0
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

rem Release builds of both DLLs: the [Diagnostics] tools compiled out (src\common\fpsu_buildcfg.h).
call "%~dp0build.bat" release
if errorlevel 1 exit /b 1

if exist build\release rmdir /s /q build\release
mkdir build\release\remastered build\release\ptde

rem Neither DLL may import the diagnostics' thread context, thread suspension or keyboard functions, or have a
rem static TLS section (Thread Storage Directory).
cmd /c "call %VCVARS% x64 >nul && dumpbin /nologo /imports /headers build\dsr\dinput8.dll build\ptde\xinput1_3.dll" > build\release\pe-check.txt
if errorlevel 1 exit /b 1
findstr /r /c:" SuspendThread$" /c:" GetThreadContext$" /c:" SetThreadContext$" /c:" CreateToolhelp32Snapshot$" /c:" GetAsyncKeyState$" build\release\pe-check.txt
if not errorlevel 1 (
    echo A release DLL imports a diagnostics function; see build\release\pe-check.txt.
    exit /b 1
)
findstr /c:"of Thread Storage Directory" build\release\pe-check.txt | findstr /v /r /c:"^ *0 \[ *0\]"
if not errorlevel 1 (
    echo A release DLL has a TLS section; see build\release\pe-check.txt.
    exit /b 1
)

copy /y build\dsr\dinput8.dll build\release\remastered >nul
copy /y build\ptde\xinput1_3.dll build\release\ptde >nul
copy /y build\ptde\dxvk_d3d9.dll build\release\ptde >nul
copy /y third_party\dxvk\LICENSE build\release\ptde\DXVK-LICENSE.txt >nul
copy /y README.md build\release\remastered >nul
copy /y README.md build\release\ptde >nul
copy /y LICENSE build\release\remastered >nul
copy /y LICENSE build\release\ptde >nul

rem The shipped INIs leave out [Diagnostics], which the release DLLs do not have.
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File tools\release_ini.ps1 -In DSR-FPS-Unlock.ini -Out build\release\remastered\DSR-FPS-Unlock.ini
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\release_ini.ps1 -In PTDE-FPS-Unlock.ini -Out build\release\ptde\PTDE-FPS-Unlock.ini
if errorlevel 1 exit /b 1

powershell -NoProfile -Command "Compress-Archive -Path build\release\remastered\* -DestinationPath build\release\DS1-FPS-Unlock-v%VERSION%-REMASTERED-DX11.zip; Compress-Archive -Path build\release\ptde\* -DestinationPath build\release\DS1-FPS-Unlock-v%VERSION%-PTDE-DX9.zip"
if errorlevel 1 exit /b 1
echo Packaged build\release\DS1-FPS-Unlock-v%VERSION%-REMASTERED-DX11.zip and DS1-FPS-Unlock-v%VERSION%-PTDE-DX9.zip

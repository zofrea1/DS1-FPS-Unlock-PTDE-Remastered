@echo off
setlocal
cd /d "%~dp0"
if not exist build\ptde mkdir build\ptde

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

cmd /c "call %VCVARS% x86 && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /Isrc\common /Isrc\ptde src\ptde\dllmain.cpp src\ptde\xinput_proxy.cpp src\common\log.cpp src\common\diag.cpp src\ptde\settings.cpp src\ptde\dsfix.cpp src\ptde\patches.cpp src\ptde\frame.cpp src\ptde\window.cpp src\ptde\present.cpp src\ptde\d3d.cpp src\ptde\profile.cpp src\ptde\trace.cpp src\ptde\watch.cpp src\ptde\fixes.cpp src\ptde\snap.cpp src\ptde\camera.cpp src\ptde\deep.cpp src\ptde\ui.cpp src\ptde\gauge.cpp src\ptde\turn.cpp src\ptde\ghost.cpp src\ptde\sfx.cpp src\ptde\dxvk.cpp src\ptde\gamma.cpp /Fobuild\ptde\ /Febuild\ptde\xinput1_3.dll /link /DEF:src\ptde\xinput1_3.def /OPT:REF"
if errorlevel 1 exit /b 1

rem DXVK's 32-bit d3d9.dll for the optional DXVK = true (pinned release, SHA-256 checked).
rem PSModulePath is cleared so Windows PowerShell does not pick up PowerShell 7's modules.
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_dxvk.ps1 -OutDir build\ptde
if errorlevel 1 exit /b 1

echo Built build\ptde\xinput1_3.dll and build\ptde\dxvk_d3d9.dll

@echo off
setlocal
cd /d "%~dp0"
if not exist build\ptde mkdir build\ptde

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

rem The sources are listed in src\ptde\sources.txt (a build that bundles another mod uses the same list).
set RSP=build\ptde\sources.rsp
> %RSP% (for /f "usebackq eol=# delims=" %%f in ("src\ptde\sources.txt") do @echo %%f)
>> %RSP% echo src\ptde\extension_none.cpp

cmd /c "call %VCVARS% x86 && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /Isrc\common /Isrc\ptde @%RSP% /Fobuild\ptde\ /Febuild\ptde\xinput1_3.dll /link /DEF:src\ptde\xinput1_3.def /OPT:REF"
if errorlevel 1 exit /b 1

rem DXVK's 32-bit d3d9.dll for the optional DXVK = true (pinned release, SHA-256 checked).
rem PSModulePath is cleared so Windows PowerShell does not pick up PowerShell 7's modules.
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_dxvk.ps1 -OutDir build\ptde
if errorlevel 1 exit /b 1

echo Built build\ptde\xinput1_3.dll and build\ptde\dxvk_d3d9.dll

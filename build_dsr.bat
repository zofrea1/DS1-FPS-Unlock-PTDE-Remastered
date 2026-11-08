@echo off
setlocal
cd /d "%~dp0"
if not exist build\dsr mkdir build\dsr

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

rem "build_dsr.bat release" leaves the [Diagnostics] tools out (src\common\fpsu_buildcfg.h); without it they are built in.
set DIAG=
set KIND=diagnostics build
if /i "%~1"=="release" set DIAG=/DFPSU_DIAGNOSTICS=0
if /i "%~1"=="release" set KIND=release build

cmd /c "call %VCVARS% x64 && rc /nologo /Isrc\common /fo build\dsr\version.res src\dsr\version.rc && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD %DIAG% /Isrc\common /Isrc\dsr src\dsr\dllmain.cpp src\dsr\proxy.cpp src\common\log.cpp src\common\diag.cpp src\dsr\settings.cpp src\dsr\bonfire.cpp src\dsr\patches.cpp src\dsr\trace.cpp src\dsr\watch.cpp src\dsr\inputdump.cpp build\dsr\version.res /Fobuild\dsr\ /Febuild\dsr\dinput8.dll /link /OPT:REF"
if errorlevel 1 exit /b 1

echo Built build\dsr\dinput8.dll (%KIND%)

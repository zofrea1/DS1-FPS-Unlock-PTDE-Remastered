@echo off
setlocal
cd /d "%~dp0"
if not exist build\dsr mkdir build\dsr

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

cmd /c "call %VCVARS% x64 && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /Isrc\common /Isrc\dsr src\dsr\dllmain.cpp src\dsr\proxy.cpp src\common\log.cpp src\common\diag.cpp src\dsr\settings.cpp src\dsr\patches.cpp src\dsr\trace.cpp src\dsr\watch.cpp /Fobuild\dsr\ /Febuild\dsr\dinput8.dll /link /OPT:REF"
if errorlevel 1 exit /b 1

echo Built build\dsr\dinput8.dll

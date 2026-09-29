@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

cmd /c "call %VCVARS% x64 && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /Isrc src\dllmain.cpp src\proxy.cpp src\log.cpp src\settings.cpp src\patches.cpp src\trace.cpp src\watch.cpp /Fobuild\ /Febuild\dinput8.dll /link /OPT:REF"
if errorlevel 1 exit /b 1

echo Built build\dinput8.dll

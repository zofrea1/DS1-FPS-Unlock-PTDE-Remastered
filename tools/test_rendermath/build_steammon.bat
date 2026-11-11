@echo off
rem Builds steammon.exe (watches the Steam client.s threads for bursts of work).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 steammon.cpp /Fe:steammon.exe /link winmm.lib

@echo off
rem Builds steamthread.exe (follows one steam.exe thread every millisecond).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 steamthread.cpp /Fe:steamthread.exe /link winmm.lib

@echo off
rem Builds scanmon.exe (reads the primary monitor scan line, to see G-SYNC / VRR follow a game).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 scanmon.cpp /Fe:scanmon.exe

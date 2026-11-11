@echo off
rem Builds vblankmon.exe (counts the primary monitor's vertical blanks, to see G-SYNC / VRR follow a game).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 vblankmon.cpp /Fe:vblankmon.exe

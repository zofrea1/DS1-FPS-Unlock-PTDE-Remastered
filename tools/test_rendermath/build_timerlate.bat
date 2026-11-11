@echo off
rem Builds timerlate.exe (the FPS cap wait on its own, to find late timer wake-ups).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 timerlate.cpp /Fe:timerlate.exe

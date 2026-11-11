@echo off
rem Builds test_spike.exe (the profiler's spike watch against a fake game thread with known hitches).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Istub /I..\..\src\ptde /I..\..\src\common test_spike.cpp /Fe:test_spike.exe /link winmm.lib

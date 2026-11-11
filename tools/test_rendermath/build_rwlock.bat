@echo off
rem Builds test_rwlock.exe (FastReadLocks against the game's lock code). Needs ptde_image.bin as test_rmath.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Istub /I..\..\src\ptde test_rwlock.cpp /Fe:test_rwlock.exe /link /BASE:0x60000000 /FIXED /DYNAMICBASE:NO

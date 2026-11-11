@echo off
rem Builds test_rmath.exe: PTDE's original vector/matrix helpers against FastRenderMath's replacements.
rem Needs ptde_image.bin next to it: the game exe's memory image (make_image.py DARKSOULS.exe, an unpacked exe).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Istub /I..\..\src\ptde test_rmath.cpp /Fe:test_rmath.exe /link /BASE:0x60000000 /FIXED /DYNAMICBASE:NO

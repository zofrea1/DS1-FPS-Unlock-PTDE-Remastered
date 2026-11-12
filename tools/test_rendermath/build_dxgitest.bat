@echo off
rem Builds dxgitest.exe (DXVK presenting through DXGI outside the game: colours on the HDR desktop, frame rate, mode).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I..\..\src\common /I..\..\third_party\vulkan\include dxgitest.cpp ..\..\src\ptde\dxgipresent.cpp ..\..\src\common\log.cpp /Fe:dxgitest.exe

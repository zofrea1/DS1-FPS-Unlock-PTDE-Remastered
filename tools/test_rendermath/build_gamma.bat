@echo off
rem Builds test_gamma.exe (GammaRamp on a real system Direct3D 9 or DXVK device).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Istub /I..\..\src\ptde test_gamma.cpp /Fe:test_gamma.exe

@echo off
rem Builds hdrprobe.exe (what Windows composes for a Direct3D 9 / DXVK window in HDR, read through Desktop Duplication).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS hdrprobe.cpp /Fe:hdrprobe.exe

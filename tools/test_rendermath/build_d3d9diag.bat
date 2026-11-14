@echo off
rem Builds d3d9diag.exe (a game-like fullscreen Direct3D 9 / DXVK device for checking G-SYNC / VRR).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS d3d9diag.cpp /Fe:d3d9diag.exe

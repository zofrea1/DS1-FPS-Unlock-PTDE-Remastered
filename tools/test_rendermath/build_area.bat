@echo off
rem Builds test_area.exe (the area-average copy shader against its previous point-sampled version).
cd /d "%~dp0"
set FXC=
for /f "delims=" %%f in ('dir /b /s /o-n "C:\Program Files (x86)\Windows Kits\10\bin\*fxc.exe" 2^>nul ^| findstr /i "\\x64\\fxc.exe"') do if not defined FXC set FXC="%%f"
%FXC% /nologo /T vs_3_0 /E main /O3 /Vn kTestAreaVs /Fh test_area_vs.h test_area.hlsl
if errorlevel 1 exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS test_area.cpp /Fe:test_area.exe

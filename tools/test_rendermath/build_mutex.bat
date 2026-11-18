@echo off
rem Builds and runs test_mutex.exe (FastMutexes on its own: the original's return values and a stress run).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Istub /I..\..\src\ptde test_mutex.cpp /Fe:test_mutex.exe
if errorlevel 1 exit /b 1
test_mutex.exe

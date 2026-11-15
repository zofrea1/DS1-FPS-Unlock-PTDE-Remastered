@echo off
rem Builds and runs test_eventwait.exe (src\common\eventwait.h against the original game's wait countdown).
set HERE=%~dp0
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
cmd /c "call %VCVARS% x86 >nul && cl /nologo /std:c++17 /O2 /EHsc /W3 "%HERE%test_eventwait.cpp" /Fo"%HERE%\" /Fe"%HERE%test_eventwait.exe"" || exit /b 1
"%HERE%test_eventwait.exe"

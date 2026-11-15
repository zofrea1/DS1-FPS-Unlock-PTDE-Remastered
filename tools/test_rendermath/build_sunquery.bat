@echo off
rem Builds test_sunquery.exe, sunquery_dll\dinput8.dll and sunquery_game\game.dll (FixSun's DSfix 3.0 check; see
rem test_sunquery.cpp).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if not exist sunquery_dll mkdir sunquery_dll
cl /nologo /O2 /EHsc /W3 /LD sunquery_dll.cpp /Fosunquery_dll\ /Fe:sunquery_dll\dinput8.dll /link d3d9.lib
if errorlevel 1 exit /b 1
if not exist sunquery_game mkdir sunquery_game
cl /nologo /O2 /EHsc /W3 /LD /DPLAIN sunquery_dll.cpp /Fosunquery_game\ /Fe:sunquery_game\game.dll /link d3d9.lib
if errorlevel 1 exit /b 1
rem resolution.cpp is compiled whole; the aspect/layout calls it makes elsewhere are never reached here.
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I..\..\src\common /I..\..\src\ptde test_sunquery.cpp ..\..\src\common\log.cpp /Fe:test_sunquery.exe /link d3d9.lib user32.lib /FORCE:UNRESOLVED

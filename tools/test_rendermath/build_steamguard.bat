@echo off
rem Builds test_steamguard.exe and its stand-in steam_api.dll (fake_steam_api.cpp), then runs nothing.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if not exist sg mkdir sg
cl /nologo /std:c++17 /O2 /EHsc /W3 /LD fake_steam_api.cpp /Fosg\ /Fe:sg\steam_api.dll /link winmm.lib
cl /nologo /std:c++17 /O2 /EHsc /W3 /DFPSU_DIAGNOSTICS=1 /I..\..\src\ptde /I..\..\src\common test_steamguard.cpp ..\..\src\ptde\steamguard.cpp ..\..\src\common\log.cpp /Fosg\ /Fe:sg\test_steamguard.exe /link winmm.lib

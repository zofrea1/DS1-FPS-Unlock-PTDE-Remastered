@echo off
rem Builds wlantime.exe (times the Wi-Fi API calls the Steam client makes every 5 s).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /std:c++17 /O2 /EHsc /W3 wlantime.cpp /Fe:wlantime.exe

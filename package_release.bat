@echo off
setlocal
cd /d "%~dp0"
if exist build\release rmdir /s /q build\release
mkdir build\release\remastered build\release\ptde
copy /y build\dsr\dinput8.dll build\release\remastered >nul
copy /y DSR-FPS-Unlock.ini build\release\remastered >nul
copy /y build\ptde\xinput1_3.dll build\release\ptde >nul
copy /y PTDE-FPS-Unlock.ini build\release\ptde >nul
copy /y build\ptde\dxvk_d3d9.dll build\release\ptde >nul
copy /y third_party\dxvk\LICENSE build\release\ptde\DXVK-LICENSE.txt >nul
copy /y README.md build\release\remastered >nul
copy /y README.md build\release\ptde >nul
copy /y LICENSE build\release\remastered >nul
copy /y LICENSE build\release\ptde >nul
powershell -NoProfile -Command "Compress-Archive -Path build\release\remastered\* -DestinationPath build\release\DS1-FPS-Unlock-v1.4.0-REMASTERED-DX11.zip; Compress-Archive -Path build\release\ptde\* -DestinationPath build\release\DS1-FPS-Unlock-v1.4.0-PTDE-DX9.zip"

@echo off
rem Compiles the DLL's own shaders (src\ptde\shaders\*.hlsl) into headers with their bytecode. The headers are
rem committed, so a normal build needs no shader compiler; run this after changing a shader.
setlocal
cd /d "%~dp0.."
set FXC=
for /f "delims=" %%f in ('dir /b /s /o-n "C:\Program Files (x86)\Windows Kits\10\bin\*fxc.exe" 2^>nul ^| findstr /i "\\x64\\fxc.exe"') do if not defined FXC set FXC="%%f"
if not defined FXC (
    echo fxc.exe not found ^(Windows SDK^).
    exit /b 1
)
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kDofCompositePs /Fh src\ptde\shaders\dof_composite_ps.h src\ptde\shaders\dof_composite.hlsl
if errorlevel 1 exit /b 1
echo Built src\ptde\shaders\dof_composite_ps.h

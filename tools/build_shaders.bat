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
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kHdrColAdjPs /Fh src\ptde\shaders\hdr_coladj_ps.h src\ptde\shaders\hdr_coladj.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kAreaReducePs /Fh src\ptde\shaders\area_reduce_ps.h src\ptde\shaders\area_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kDofNearRatePs /Fh src\ptde\shaders\dof_near_rate_ps.h src\ptde\shaders\dof_near_rate.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kPresentLutPs /Fh src\ptde\shaders\present_lut_ps.h src\ptde\shaders\present_lut.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_3_0 /E main_vs /O3 /Vn kDofHrVs /Fh src\ptde\shaders\dof_hr_vs.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_down /O3 /Vn kDofHrDownPs /Fh src\ptde\shaders\dof_hr_down_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_blur /O3 /Vn kDofHrBlurPs /Fh src\ptde\shaders\dof_hr_blur_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_composite /O3 /Vn kDofHrCompositePs /Fh src\ptde\shaders\dof_hr_composite_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kEffectsCompositePs /Fh src\ptde\shaders\effects_composite_ps.h src\ptde\shaders\effects_composite.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_3_0 /E main_vs /O3 /Vn kPresentReduceVs /Fh src\ptde\shaders\present_reduce_vs.h src\ptde\shaders\present_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_ps /O3 /Vn kPresentReducePs /Fh src\ptde\shaders\present_reduce_ps.h src\ptde\shaders\present_reduce.hlsl
if errorlevel 1 exit /b 1
echo Built src\ptde\shaders\dof_composite_ps.h, hdr_coladj_ps.h, area_reduce_ps.h, dof_near_rate_ps.h and present_reduce_vs/ps.h

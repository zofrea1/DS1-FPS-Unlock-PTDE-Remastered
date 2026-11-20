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
%FXC% /nologo /T ps_3_0 /E main /O3 /D SIDE_ALPHA=1 /Vn kDofCompositeSidePs /Fh src\ptde\shaders\dof_composite_side_ps.h src\ptde\shaders\dof_composite.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kHdrColAdjPs /Fh src\ptde\shaders\hdr_coladj_ps.h src\ptde\shaders\hdr_coladj.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kAreaReducePs /Fh src\ptde\shaders\area_reduce_ps.h src\ptde\shaders\area_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /D SIDE_ALPHA=1 /Vn kAreaReduceSidePs /Fh src\ptde\shaders\area_reduce_side_ps.h src\ptde\shaders\area_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_h /O3 /Vn kAreaReduce2HPs /Fh src\ptde\shaders\area_reduce2_h_ps.h src\ptde\shaders\area_reduce2.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_v /O3 /Vn kAreaReduce2VPs /Fh src\ptde\shaders\area_reduce2_v_ps.h src\ptde\shaders\area_reduce2.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kDofNearRatePs /Fh src\ptde\shaders\dof_near_rate_ps.h src\ptde\shaders\dof_near_rate.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_h /O3 /Vn kDofNearRate2HPs /Fh src\ptde\shaders\dof_near_rate2_h_ps.h src\ptde\shaders\dof_near_rate2.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_v /O3 /Vn kDofNearRate2VPs /Fh src\ptde\shaders\dof_near_rate2_v_ps.h src\ptde\shaders\dof_near_rate2.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kCopyRgbPs /Fh src\ptde\shaders\copy_rgb_ps.h src\ptde\shaders\copy_rgb.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kPresentLutPs /Fh src\ptde\shaders\present_lut_ps.h src\ptde\shaders\present_lut.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_3_0 /E main_vs /O3 /Vn kDofHrVs /Fh src\ptde\shaders\dof_hr_vs.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_down /O3 /Vn kDofHrDownPs /Fh src\ptde\shaders\dof_hr_down_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_down /O3 /D SIDE_ALPHA=1 /Vn kDofHrDownSidePs /Fh src\ptde\shaders\dof_hr_down_side_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_blur /O3 /Vn kDofHrBlurPs /Fh src\ptde\shaders\dof_hr_blur_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_composite /O3 /Vn kDofHrCompositePs /Fh src\ptde\shaders\dof_hr_composite_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_composite /O3 /D SIDE_ALPHA=1 /Vn kDofHrCompositeSidePs /Fh src\ptde\shaders\dof_hr_composite_side_ps.h src\ptde\shaders\dof_hr.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main /O3 /Vn kEffectsCompositePs /Fh src\ptde\shaders\effects_composite_ps.h src\ptde\shaders\effects_composite.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_3_0 /E main_vs /O3 /Vn kPresentReduceVs /Fh src\ptde\shaders\present_reduce_vs.h src\ptde\shaders\present_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_ps /O3 /Vn kPresentReducePs /Fh src\ptde\shaders\present_reduce_ps.h src\ptde\shaders\present_reduce.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_4_0 /E main_vs /O3 /Vn kDxgiScrgbVs /Fh src\ptde\shaders\dxgi_scrgb_vs.h src\ptde\shaders\dxgi_scrgb.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_4_0 /E main_ps /O3 /Vn kDxgiScrgbPs /Fh src\ptde\shaders\dxgi_scrgb_ps.h src\ptde\shaders\dxgi_scrgb.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_4_0 /E main_ps_pq /O3 /Vn kDxgiHdr10Ps /Fh src\ptde\shaders\dxgi_hdr10_ps.h src\ptde\shaders\dxgi_scrgb.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T vs_3_0 /E main_vs /O3 /Vn kOcclBoxVs /Fh src\ptde\shaders\occl_box_vs.h src\ptde\shaders\occl_box.hlsl
if errorlevel 1 exit /b 1
%FXC% /nologo /T ps_3_0 /E main_ps /O3 /Vn kOcclBoxPs /Fh src\ptde\shaders\occl_box_ps.h src\ptde\shaders\occl_box.hlsl
if errorlevel 1 exit /b 1
echo Built src\ptde\shaders\dof_composite_ps.h, hdr_coladj_ps.h, area_reduce_ps.h, dof_near_rate_ps.h and present_reduce_vs/ps.h

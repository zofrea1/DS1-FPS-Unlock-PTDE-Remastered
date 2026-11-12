# run_dxgitest.ps1 MODE SIZE SECONDS FPS: one dxgitest.exe run with its output and the module log printed.
param([string]$Mode = 'd3d9', [string]$Size = 'window', [string]$Seconds = '3', [string]$Fps = '0')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$dll = Join-Path $here '..\..\build\ptde\dxvk_d3d9.dll'
$arg = switch ($Mode) { 'dxvk' { "dxvk:$dll" } 'dxgi' { "dxgi:$dll" } default { 'd3d9' } }
Remove-Item -ErrorAction SilentlyContinue dxgitest.log, out_run.txt, err_run.txt
$p = Start-Process -FilePath (Join-Path $here 'dxgitest.exe') -ArgumentList $arg, $Size, $Seconds, $Fps -NoNewWindow -PassThru `
    -RedirectStandardOutput out_run.txt -RedirectStandardError err_run.txt
if (-not $p.WaitForExit(90000)) { 'TIMEOUT'; $p.Kill() }
"exit=$($p.ExitCode)"
Get-Content out_run.txt
Get-Content err_run.txt | Select-Object -Last 5
if (Test-Path dxgitest.log) { '--- module log'; Get-Content dxgitest.log }

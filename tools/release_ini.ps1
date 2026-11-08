# Writes the INI shipped in a release zip: the repository's INI without its [Diagnostics] section, which release
# builds do not include (src\common\buildcfg.h). [Diagnostics] is the last section of both INIs.
param([Parameter(Mandatory = $true)][string]$In, [Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'

$text = [IO.File]::ReadAllText((Resolve-Path $In))
$at = $text.IndexOf("`n[Diagnostics]")
if ($at -lt 0) { throw "$In has no [Diagnostics] section" }
$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$kept = $text.Substring(0, $at).TrimEnd() + $nl
[IO.File]::WriteAllText((Join-Path (Get-Location) $Out), $kept, (New-Object Text.UTF8Encoding $false))
Write-Host "Wrote $Out ($In without [Diagnostics])"

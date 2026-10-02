# Puts DXVK's 32-bit d3d9.dll in build\ptde as dxvk_d3d9.dll (the PTDE mod's optional DXVK = true).
# Downloads the pinned release from GitHub once and checks both the archive and the DLL against
# their SHA-256, so every build ships the same file. DXVK's license is third_party\dxvk\LICENSE.
param([string]$OutDir = "build\ptde")
$ErrorActionPreference = 'Stop'

$Version = '3.1.1'
$ArchiveSha256 = '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608'
$DllSha256 = '265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa'
$Url = "https://github.com/doitsujin/dxvk/releases/download/v$Version/dxvk-$Version.tar.gz"

# Not Get-FileHash: it can fail to load when Windows PowerShell is started from PowerShell 7.
function Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead((Resolve-Path $Path))
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
    }
}

$Out = Join-Path $OutDir 'dxvk_d3d9.dll'
if ((Test-Path $Out) -and (Sha256 $Out) -eq $DllSha256) {
    exit 0
}

$Work = 'build\dxvk'
New-Item -ItemType Directory -Force $Work, $OutDir | Out-Null
$Archive = Join-Path $Work "dxvk-$Version.tar.gz"
if (-not (Test-Path $Archive) -or (Sha256 $Archive) -ne $ArchiveSha256) {
    Write-Host "Downloading DXVK $Version"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $Archive -UseBasicParsing
    if ((Sha256 $Archive) -ne $ArchiveSha256) {
        throw "dxvk-$Version.tar.gz does not have the expected SHA-256"
    }
}

& "$env:SystemRoot\System32\tar.exe" -xzf $Archive -C $Work "dxvk-$Version/x32/d3d9.dll"
if ($LASTEXITCODE -ne 0) {
    throw "Could not extract x32/d3d9.dll from dxvk-$Version.tar.gz"
}
$Dll = Join-Path $Work "dxvk-$Version\x32\d3d9.dll"
if ((Sha256 $Dll) -ne $DllSha256) {
    throw "DXVK's x32/d3d9.dll does not have the expected SHA-256"
}
Copy-Item $Dll $Out -Force
Write-Host "DXVK $Version x32/d3d9.dll -> $Out"

# Downloads and pins the DirectX Shader Compiler used to build every native shader.
# The version, archive name and SHA-256 below are the single source of truth; both
# tools/build.ps1 and .incrementalTesting/core/ci.ps1 go through this script so a build can never pick up
# a different DXC from PATH or from a developer machine's Visual Studio install.
param(
    [string]$Version = '1.9.2609',
    [string]$ArchiveName = 'dxc_2026_09_29.zip',
    [string]$Sha256 = 'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1',
    [string]$DownloadUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip',
    [switch]$Force,
    [switch]$Quiet,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'

function Write-Info([string]$text) { if (-not $Quiet) { Write-Host $text } }

$root = Split-Path $PSScriptRoot -Parent
$dxcRoot = Join-Path $root '.tools\dxc'
$installDir = Join-Path $dxcRoot $Version
$exePath = Join-Path $installDir 'bin\x64\dxc.exe'
$manifestPath = Join-Path $installDir 'dxc.json'

function Test-Install {
    if (-not (Test-Path -LiteralPath $exePath)) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $installDir 'bin\x64\dxcompiler.dll'))) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $installDir 'bin\x64\dxil.dll'))) { return $false }
    if (-not (Test-Path -LiteralPath $manifestPath)) { return $false }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        return ($manifest.version -eq $Version -and $manifest.sha256 -eq $Sha256)
    } catch { return $false }
}

if ($VerifyOnly) {
    if (Test-Install) {
        Write-Output $exePath
        exit 0
    }
    Write-Error "DXC $Version is not installed at $installDir (run tools/bootstrap-dxc.ps1)."
    exit 1
}

if ($Force -and (Test-Path -LiteralPath $installDir)) {
    Remove-Item -LiteralPath $installDir -Recurse -Force
}

if (Test-Install) {
    Write-Info "DXC $Version already installed: $exePath"
    Write-Output $exePath
    exit 0
}

# Portable PowerShell 5.1 needs TLS 1.2 explicitly for the GitHub release host.
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls11
} catch { }

$downloadDir = Join-Path $root '.tools\downloads'
New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
$archivePath = Join-Path $downloadDir $ArchiveName

if (-not (Test-Path -LiteralPath $archivePath)) {
    Write-Info "Downloading DXC $Version ($ArchiveName, ~31 MB)..."
    $progress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $archivePath -UseBasicParsing -TimeoutSec 600
    } finally {
        $ProgressPreference = $progress
    }
}

$actual = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $Sha256) {
    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    throw "DXC archive checksum mismatch: expected $Sha256, got $actual. The partial archive was removed; retry."
}

New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $archivePath).Path)
try {
    foreach ($entry in $archive.Entries) {
        # Only the shipped x64 compiler, headers and licences are needed.
        if ([string]::IsNullOrEmpty($entry.Name)) { continue }
        $relative = $entry.FullName.Replace('/', '\')
        $destination = Join-Path $installDir $relative
        $parent = Split-Path $destination -Parent
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
    }
} finally {
    $archive.Dispose()
}

if (-not (Test-Path -LiteralPath $exePath)) { throw "DXC extraction did not produce $exePath." }

$compilerVersion = ''
try {
    # DXC rejects -V; --version is the supported banner query and prints the DXIL and
    # dxcompiler build ids we record as evidence.
    $compilerVersion = (& $exePath --version 2>&1 | Out-String).Trim()
} catch {
    $compilerVersion = "dxc --version failed: $($_.Exception.Message)"
}
if (-not $compilerVersion) { $compilerVersion = "dxc $Version" }

$manifest = [pscustomobject]@{
    version = $Version
    archive = $ArchiveName
    downloadUrl = $DownloadUrl
    sha256 = $Sha256
    dxc = $exePath
    dxcVersionText = $compilerVersion
    dxcompilerSha256 = (Get-FileHash -LiteralPath (Join-Path $installDir 'bin\x64\dxcompiler.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    dxilSha256 = (Get-FileHash -LiteralPath (Join-Path $installDir 'bin\x64\dxil.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    installedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Info "DXC $Version installed: $exePath"
Write-Output $exePath

# Clean-state continuous integration for NativeGPU.
#
# This is the single command referenced by .github/workflows/windows.yml and is meant
# to be replayable on a developer machine:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1
#
# It builds from a clean tree, runs every test layer, captures the G1 evidence bundle,
# packages a runnable output and writes reports/ci-<commit>.json. Failure logs are kept
# under reports/ci-failure-<commit>-*.log so a red run can be inspected later.
param(
    [switch]$Clean,
    [switch]$AllowUnsupportedGpu,
    [switch]$SkipPackage,
    [string]$Frames = '240'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')

$startedUtc = (Get-Date).ToUniversalTime()
$commit = Get-GitCommit
$commitShort = if ($commit -eq 'no-git') { 'no-git' } else { $commit.Substring(0, [Math]::Min(12, $commit.Length)) }
$context = New-ReportContext -Mode 'ci'
$logDirectory = Join-Path $context.directory 'logs'
New-Item -ItemType Directory -Force -Path $logDirectory,'reports' | Out-Null
$steps = [System.Collections.Generic.List[object]]::new()
$overall = $true
$failureLog = $null

function Write-Log([string]$name, [scriptblock]$action) {
    $logPath = Join-Path $logDirectory (($name -replace '[^A-Za-z0-9.-]+', '-') + '.log')
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $action *>&1 | Tee-Object -FilePath $logPath
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($null -eq $code) { $code = 0 }
    return [pscustomobject]@{ exitCode = [int]$code; log = $logPath }
}

function Invoke-CiStep {
    param(
        [string]$Name,
        [scriptblock]$Action,
        [switch]$AllowedToFail
    )
    Write-Host ""
    Write-Host "== $Name" -ForegroundColor Cyan
    $started = Get-Date
    $ok = $true
    $message = ''
    $log = ''
    try {
        $result = Write-Log $Name $Action
        $log = $result.log
        if ($result.exitCode -ne 0) { throw "exit code $($result.exitCode)" }
    } catch {
        $ok = $false
        $message = $_.Exception.Message
        if ($AllowedToFail) {
            Write-Host "ALLOWED FAILURE: $message" -ForegroundColor Yellow
        } else {
            $script:overall = $false
            $script:failureLog = $log
            Write-Host "FAILED: $message" -ForegroundColor Red
        }
    }
    $steps.Add([pscustomobject]@{
        name = $Name
        ok = $ok
        allowedToFail = [bool]$AllowedToFail
        durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        error = $message
        log = $log
    })
}

if ($Clean) {
    Write-Host "Cleaning build/ and outputs/ (reports/ is preserved as evidence)..." -ForegroundColor Cyan
    foreach ($directory in 'build','outputs') {
        if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    }
}

Write-Host "NativeGPU CI $($context.version) @ $commitShort (dirty=$($context.dirty))" -ForegroundColor Cyan

Invoke-CiStep 'Toolchain bootstrap' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'bootstrap.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'C++ toolchain bootstrap failed.' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'bootstrap-dxc.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'DXC bootstrap failed.' }
}

Invoke-CiStep 'Verify (build, tests, smoke, probe, hot reload, MCP)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'tools/verify.ps1 reported a failure.' }
}

# Hardware-dependent evidence. A CI runner without a D3D12 GPU may legitimately report
# unsupported capabilities; it must never report an effect as implemented.
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'rt-shadows.ps1')) {
    Invoke-CiStep 'DXR shadow evidence (on/off + screenshots)' -AllowedToFail:([bool]$AllowUnsupportedGpu) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'rt-shadows.ps1') -Frames $Frames
        if ($LASTEXITCODE -ne 0) { throw 'tools/rt-shadows.ps1 reported a failure.' }
    }
} else {
    $steps.Add([pscustomobject]@{ name = 'DXR shadow evidence (on/off + screenshots)'; ok = $false; allowedToFail = $true; durationMs = 0; error = 'script not present in this work package'; log = '' })
    Write-Host "SKIPPED: tools/rt-shadows.ps1 not present in this work package." -ForegroundColor Yellow
}

if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'g1-reliability.ps1')) {
    Invoke-CiStep 'G1 reliability suite' -AllowedToFail:([bool]$AllowUnsupportedGpu) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'g1-reliability.ps1') -Frames $Frames
        if ($LASTEXITCODE -ne 0) { throw 'tools/g1-reliability.ps1 reported a failure.' }
    }
} else {
    $steps.Add([pscustomobject]@{ name = 'G1 reliability suite'; ok = $false; allowedToFail = $true; durationMs = 0; error = 'script not present in this work package'; log = '' })
    Write-Host "SKIPPED: tools/g1-reliability.ps1 not present in this work package." -ForegroundColor Yellow
}

if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'mesh-bindless.ps1')) {
    Invoke-CiStep 'Mesh shader and bindless evidence' -AllowedToFail:([bool]$AllowUnsupportedGpu) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'mesh-bindless.ps1') -Frames $Frames
        if ($LASTEXITCODE -ne 0) { throw 'tools/mesh-bindless.ps1 reported a failure.' }
    }
} else {
    $steps.Add([pscustomobject]@{ name = 'Mesh shader and bindless evidence'; ok = $false; allowedToFail = $true; durationMs = 0; error = 'script not present in this work package'; log = '' })
    Write-Host "SKIPPED: tools/mesh-bindless.ps1 not present in this work package." -ForegroundColor Yellow
}

if (-not $SkipPackage) {
    Invoke-CiStep 'Package' {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Package step failed.' }
    }
}

# Collect the newest report of each kind from reports/latest (written by the steps above
# or by verify.ps1).
$latest = Join-Path $root 'reports\latest'
function Read-LatestJson([string]$name) {
    $path = Join-Path $latest $name
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try { return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json) } catch { return $null }
}

$probe = Read-LatestJson 'verify-probe.json'
$rt = Read-LatestJson 'rt-shadows-evidence.json'
$reliability = Read-LatestJson 'g1-reliability.json'
$mesh = Read-LatestJson 'mesh-bindless-evidence.json'

$g1 = [pscustomobject]@{
    capabilities = if ($probe) { $probe.capabilities } else { $null }
    materialTable = if ($probe) { $probe.probes.materialTable } else { $null }
    accelerationStructures = if ($probe) { $probe.probes.raytracingAccelerationStructures } else { $null }
    raytracingShadows = if ($rt) { $rt.summary } elseif ($probe) { $probe.probes.raytracingShadows } else { $null }
    reliability = if ($reliability) { $reliability.summary } else { $null }
    meshShaders = if ($mesh) { $mesh.meshShader } elseif ($probe) { $probe.probes.meshShader } else { $null }
    bindless = if ($mesh) { $mesh.bindless } elseif ($probe) { $probe.probes.bindless } else { $null }
    implemented = if ($probe) { $probe.capabilities.implemented } else { $null }
}

$packageInfo = $null
if (-not $SkipPackage) {
    $packagePath = Get-ChildItem -LiteralPath 'outputs' -Filter '*.zip' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($packagePath) {
        $packageInfo = [pscustomobject]@{
            name = $packagePath.Name
            path = (Get-RelativePath $packagePath.FullName)
            bytes = $packagePath.Length
            sha256 = (Get-FileHash -LiteralPath $packagePath.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

$durationSeconds = ((Get-Date).ToUniversalTime() - $startedUtc).TotalSeconds
# The build happened inside the Verify step; re-read its identity so staleBuild reflects
# the binary that actually produced the evidence.
$context.build = Get-BuildIdentity
$report = [pscustomobject]@{
    schema = 'nativegpu.ci/2'
    version = $context.version
    commit = $commit
    commitShort = $commitShort
    worktreeDirty = $context.dirty
    build = $context.build
    staleBuild = [bool]($context.build.commit -ne $commit -or $context.build.dirty -eq $true)
    node = $context.node
    powershell = $context.powershell
    os = $context.os
    adapter = $context.adapter
    dxc = $context.dxc
    sceneSha256 = $context.sceneHash
    startedAtUtc = $startedUtc.ToString('o')
    finishedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    durationSeconds = [Math]::Round($durationSeconds, 1)
    allowUnsupportedGpu = [bool]$AllowUnsupportedGpu
    success = $overall
    steps = $steps
    g1 = $g1
    package = $packageInfo
    evidenceDirectory = (Get-RelativePath $context.directory)
}

$ciPath = Join-Path $context.directory 'ci.json'
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ciPath -Encoding utf8
Register-Report -Context $context -Path $ciPath -LegacyAliases @("ci-$commitShort.json") | Out-Null

if (-not $overall -and $failureLog -and (Test-Path -LiteralPath $failureLog)) {
    $failureName = 'ci-failure-' + $commitShort + '-' + (Split-Path $failureLog -Leaf)
    $failureTarget = Join-Path $context.directory $failureName
    Copy-Item -LiteralPath $failureLog -Destination $failureTarget -Force
    Register-Report -Context $context -Path $failureTarget -LegacyAliases @($failureName) | Out-Null
    Write-Host "Failure log preserved: reports/$failureName" -ForegroundColor Red
}

$runPath = Complete-Run -Context $context -Success $overall
Write-Host ""
Write-Host "CI summary ($($context.version) @ $commitShort)" -ForegroundColor Cyan
$steps | Format-Table name, ok, allowedToFail, durationMs, error -AutoSize | Out-String | Write-Host
Write-Host "Report:   $ciPath"
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $overall) { exit 1 }

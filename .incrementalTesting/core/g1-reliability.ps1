# G1 reliability evidence: device removal (injected fault), renderer lifecycle stress,
# illegal-state diagnostics, corrupt package detection, DXC failure, capture availability
# and the fixed-camera performance baseline.
#
# Exit code 0 means every check passed; the evidence is archived under
# reports/<version>/<commit12>/<UTC stamp>/g1-reliability.json.
param(
    [int]$Frames = 240,
    [int]$LifecycleCycles = 50,
    [string]$Camera = '9,5.5,10,-2,0.8,0',
    [string]$RenderSize = '1280x720',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')
$context = New-ReportContext -Mode 'g1-reliability'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

if (-not $SkipBuild -or -not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Invoke-Native {
    param([string[]]$Arguments, [string]$Tag, [int[]]$AllowedExit = @(0))
    $report = Join-Path $stage "$Tag.json"
    $full = $Arguments + @('--report', (Get-RelativePath $report))
    $process = Start-Process -FilePath $exe -ArgumentList $full -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    $data = $null
    if (Test-Path -LiteralPath $report) {
        try { $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json } catch { $data = $null }
    }
    return [pscustomobject]@{
        exitCode = $process.ExitCode
        report = $report
        data = $data
        allowed = ($AllowedExit -contains $process.ExitCode)
        tag = $Tag
    }
}

Write-Host "G1 reliability suite ($LifecycleCycles lifecycle cycles, $RenderSize)" -ForegroundColor Cyan

# --- 1. device removal (injected fault) ---------------------------------------
$sceneHashBefore = (Get-FileHash -LiteralPath '.screen\courtyard.scene.json' -Algorithm SHA256).Hash.ToLowerInvariant()
$removalFrame = 40
$removal = Invoke-Native -Tag 'device-removal' -Arguments @(
    '--device-removal-after', "$removalFrame", '--frames', '120', '--render-size', $RenderSize,
    '--rt-shadows', 'on')
Add-Check 'device removal run produced a valid report' ($null -ne $removal.data) "exit=$($removal.exitCode)"
if ($removal.data) {
    $dr = $removal.data.deviceRemoval
    Add-Check 'device removal detected' ([bool]$dr.detected) "mode=$($dr.mode)"
    Add-Check 'error text names DXGI_ERROR_DEVICE_REMOVED' ((("" + $dr.error).IndexOf('DEVICE_REMOVED') -ge 0)) "$($dr.error)"
    Add-Check 'renderer rebuilt after the removal' ([bool]$dr.recovered) "recoveries=$($dr.recoveries)"
    Add-Check 'frames rendered after recovery' ([int]$removal.data.frames -gt [int]$dr.detectedAtFrame) `
        "frames=$($removal.data.frames) detectedAt=$($dr.detectedAtFrame)"
    Add-Check 'removal run reported success after recovery' ([bool]$removal.data.success) "exit=$($removal.exitCode)"
    Add-Check 'report still valid JSON after the fault' ($removal.exitCode -eq 0) "exit=$($removal.exitCode)"
    Add-Check 'device removal is labelled as injected, not hardware' ($dr.mode -eq 'injected-fault') "$($dr.mode)"
}
$sceneHashAfter = (Get-FileHash -LiteralPath '.screen\courtyard.scene.json' -Algorithm SHA256).Hash.ToLowerInvariant()
Add-Check 'scene file untouched by the device-removal run' ($sceneHashBefore -eq $sceneHashAfter) ''

# --- 2. renderer lifecycle stress ---------------------------------------------
$lifecycle = Invoke-Native -Tag 'lifecycle' -Arguments @(
    '--lifecycle', "$LifecycleCycles", '--frames', '40', '--render-size', '960x540')
Add-Check 'lifecycle run produced a valid report' ($null -ne $lifecycle.data) "exit=$($lifecycle.exitCode)"
$lifecycleLeak = 0
$vramSpread = 0
$lifecycleEntries = @()
if ($lifecycle.data) {
    $lifecycleEntries = @($lifecycle.data.lifecycle.entries)
    Add-Check 'all requested lifecycle cycles completed' ([int]$lifecycle.data.lifecycle.completed -eq $LifecycleCycles) `
        "completed=$($lifecycle.data.lifecycle.completed)"
    $objects = @($lifecycleEntries | ForEach-Object { [long]$_.liveObjectsAfterShutdown })
    $lifecycleLeak = ($objects | Measure-Object -Maximum).Maximum
    Add-Check 'no D3D12 objects survive shutdown' ($lifecycleLeak -eq 0) "maxLive=$lifecycleLeak"
    $trend = @($objects | Select-Object -Last 5)
    Add-Check 'object count does not grow across cycles' (($trend | Measure-Object -Maximum).Maximum -eq ($objects[0])) `
        "first=$($objects[0]) last=$($objects[-1])"
    $memory = @($lifecycleEntries | ForEach-Object { [long]$_.videoMemoryBytes })
    $vramSpread = ($memory | Measure-Object -Maximum).Maximum - ($memory | Measure-Object -Minimum).Minimum
    Add-Check 'video memory does not keep growing' ($vramSpread -le 16777216) "spread=$vramSpread bytes"
    $created = @($lifecycleEntries | ForEach-Object { [long]$_.objectsCreated })
    Add-Check 'each cycle recreates the full renderer' ($created[-1] -gt $created[0]) `
        "first=$($created[0]) last=$($created[-1])"
}

# --- 3. illegal state and error inputs ----------------------------------------
$brokenScene = Join-Path $stage 'broken.scene.json'
Set-Content -LiteralPath $brokenScene -Encoding utf8 -Value '{ "projectId": "not-a-uuid", "entities": [ }'
$faults = Invoke-Native -Tag 'fault-test' -Arguments @(
    '--fault-test', '--frames', '40', '--render-size', '960x540', '--mesh-shaders', 'off',
    '--fault-scene', (Get-RelativePath $brokenScene))
Add-Check 'fault-test run produced a valid report' ($null -ne $faults.data) "exit=$($faults.exitCode)"
if ($faults.data) {
    $cases = @($faults.data.faultTests)
    $pipelineCase = $cases | Where-Object { $_.case -eq 'invalid-pipeline' }
    $sceneCase = $cases | Where-Object { $_.case -eq 'invalid-scene' }
    Add-Check 'invalid pipeline reports NG-E-PIPE-COMPILE' `
        ($pipelineCase -and $pipelineCase.ok -and $pipelineCase.actualCode -eq 'NG-E-PIPE-COMPILE') `
        "actual=$($pipelineCase.actualCode)"
    Add-Check 'invalid scene is rejected with its code' ($sceneCase -and $sceneCase.ok) `
        "actual=$($sceneCase.actualCode)"
    $items = @($faults.data.probes.diagnostics.items)
    $slot = $items | Where-Object { $_.code -eq 'NG-E-MAT-STALE' }
    $missing = $items | Where-Object { $_.code -eq 'NG-E-MAT-MISSING' }
    Add-Check 'out-of-range material slot is diagnosed' ($slot -and $slot.occurrences -gt 0) `
        "occurrences=$($slot.occurrences)"
    Add-Check 'missing material texture is diagnosed and falls back' ($missing -and $missing.occurrences -gt 0) `
        "occurrences=$($missing.occurrences)"
    Add-Check 'diagnostic events counted' ([int]$faults.data.probes.diagnostics.events -gt 0) `
        "events=$($faults.data.probes.diagnostics.events)"
}

# --- 4. DXC compile failure preserves diagnostics -----------------------------
$dxcSelfTest = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/compile-shaders.ps1') -SelfTest -Quiet
$dxcOk = $false
$dxcDetail = ''
if ($LASTEXITCODE -eq 0 -and $dxcSelfTest) {
    try {
        $parsed = $dxcSelfTest | ConvertFrom-Json
        $dxcOk = [bool]$parsed.failedAsExpected
        $dxcDetail = ($parsed.diagnosticLines -join ' | ')
    } catch { $dxcDetail = "$dxcSelfTest" }
}
Add-Check 'DXC rejects a broken shader and keeps diagnostics' $dxcOk $dxcDetail

# --- 5. corrupt package detection ---------------------------------------------
$packageRoot = Join-Path $env:TEMP ('nativegpu-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
$packageOk = $false
$packageDetail = ''
try {
    # The scene CLI writes failures to stderr and exits non-zero, so merge the streams
    # and relax the preference while the child runs.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $firstBuild = & node 'tools\scene.mjs' build '.screen\courtyard.scene.json' $packageRoot 2>&1 | ConvertFrom-Json
    } finally {
        $ErrorActionPreference = $previous
    }
    if (-not $firstBuild.ok) { throw 'first package build failed' }
    $packageDirectory = $firstBuild.result.packageDirectory
    $sceneFile = Join-Path $packageDirectory 'scene.json'
    if (-not (Test-Path -LiteralPath $sceneFile)) { throw 'package has no scene.json' }
    # Tamper: add whitespace so the content hash no longer matches the manifest.
    Add-Content -LiteralPath $sceneFile -Value ' '
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $secondBuild = & node 'tools\scene.mjs' build '.screen\courtyard.scene.json' $packageRoot 2>&1 | ConvertFrom-Json
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($secondBuild.ok) {
        $packageDetail = "tampered package was silently reused (cacheHit=$($secondBuild.result.cacheHit))"
    } elseif ($secondBuild.error.code -eq 'BUILD_CONFLICT') {
        $packageOk = $true
        $packageDetail = 'tampered package rejected with BUILD_CONFLICT'
    } else {
        $packageDetail = "unexpected error $($secondBuild.error.code)"
    }
} catch {
    $packageDetail = $_.Exception.Message
} finally {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force -ErrorAction SilentlyContinue
}
Add-Check 'corrupt package is detected instead of reused' $packageOk $packageDetail

# --- 6. scene apply/undo residue ----------------------------------------------
$residueOk = $false
$residueDetail = ''
$sceneDirectory = Join-Path $env:TEMP ('nativegpu-residue-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $sceneDirectory | Out-Null
try {
    $liveScene = Join-Path $sceneDirectory 'live.scene.json'
    Copy-Item -LiteralPath '.screen\courtyard.scene.json' -Destination $liveScene
    $revision = 0
    for ($iteration = 0; $iteration -lt 10; ++$iteration) {
        $transaction = Join-Path $sceneDirectory 'transaction.json'
        $payload = [ordered]@{
            projectId = 'a1e00000-0000-4000-8000-000000000001'
            expectedRevision = $revision
            commands = @([ordered]@{ op = 'set'; entityId = 'a1e00000-0000-4000-8000-000000000030'
                                     changes = [ordered]@{ position = @(-4, 0.6, 1) } })
        }
        # PowerShell 5.1 writes a BOM with Set-Content -Encoding utf8; the scene tool
        # rejects it, so write UTF-8 without BOM.
        [System.IO.File]::WriteAllText($transaction, ($payload | ConvertTo-Json -Depth 6),
            (New-Object System.Text.UTF8Encoding($false)))
        $apply = & node 'tools\scene.mjs' apply $liveScene $transaction | ConvertFrom-Json
        if (-not $apply.ok) { throw "apply failed at iteration $iteration" }
        $revision = [int]$apply.result.scene.revision
        $undo = & node 'tools\scene.mjs' undo $liveScene --project 'a1e00000-0000-4000-8000-000000000001' --revision $revision | ConvertFrom-Json
        if (-not $undo.ok) { throw "undo failed at iteration $iteration" }
        $revision = [int]$undo.result.scene.revision
    }
    # Sidecars are expected: <scene>.history.json and <scene>.snapshot.json. Locks,
    # pending writes and temporaries are not.
    $leftovers = @(Get-ChildItem -LiteralPath $sceneDirectory -Force | Where-Object {
        $_.Name -notlike 'live.scene.json*' -and $_.Name -ne 'transaction.json'
    })
    $residueOk = ($leftovers.Count -eq 0) -and ($revision -ge 20)
    $residueDetail = "revision=$revision leftovers=$($leftovers.Name -join ',')"
} catch {
    $residueDetail = $_.Exception.Message
} finally {
    Remove-Item -LiteralPath $sceneDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
Add-Check 'apply/undo leaves no lock or temp residue' $residueOk $residueDetail

# --- 7. capture availability --------------------------------------------------
$captureScript = Join-Path $root '.incrementalTesting/rendering/capture.ps1'
$capture = & powershell -NoProfile -ExecutionPolicy Bypass -File $captureScript -Quiet | ConvertFrom-Json
Add-Check 'capture status recorded' ($null -ne $capture) ''
$captureDetail = "renderDoc=$($capture.renderDoc.available) pix=$($capture.pix.available) dxcap=$($capture.dxcap.available)"

# --- 8. performance baseline --------------------------------------------------
$perf = Invoke-Native -Tag 'perf' -Arguments @(
    '--probe', '--no-stress', '--frames', "$Frames", '--render-size', $RenderSize,
    '--rt-shadows', 'on', '--camera', $Camera)
Add-Check 'performance run produced a valid report' ($null -ne $perf.data) "exit=$($perf.exitCode)"
$perfSummary = $null
if ($perf.data) {
    $perfSummary = [pscustomobject]@{
        resolution = $RenderSize
        frames = [int]$perf.data.frames
        gpuSamples = [int]$perf.data.gpuSamples
        cpuP50Ms = [Math]::Round([double]$perf.data.cpuP50Ms, 3)
        cpuSubmitP50Ms = [Math]::Round([double]$perf.data.cpuSubmitP50Ms, 3)
        cpuSubmitP95Ms = [Math]::Round([double]$perf.data.cpuSubmitP95Ms, 3)
        gpuP50Ms = [Math]::Round([double]$perf.data.gpuP50Ms, 3)
        gpuP95Ms = [Math]::Round([double]$perf.data.gpuP95Ms, 3)
        gpuRtP50Ms = [Math]::Round([double]$perf.data.gpuRtP50Ms, 3)
        gpuRtP95Ms = [Math]::Round([double]$perf.data.gpuRtP95Ms, 3)
        frameIntervalP95Ms = [Math]::Round([double]$perf.data.frameIntervalP95Ms, 3)
        drawCalls = [int]$perf.data.probes.reliability.drawCalls
        triangles = [int]$perf.data.probes.reliability.triangles
        materialSlots = [int]$perf.data.capabilities.materialTableSlots
        videoMemoryBytes = [long]$perf.data.probes.reliability.videoMemoryBytes
        sceneSha256 = $context.sceneHash
    }
    Add-Check 'frame times recorded' ($perfSummary.gpuP50Ms -gt 0 -and $perfSummary.cpuSubmitP50Ms -gt 0) `
        "gpuP50=$($perfSummary.gpuP50Ms) cpuSubmitP50=$($perfSummary.cpuSubmitP50Ms)"
}

$success = -not ($checks | Where-Object { -not $_.ok })
$summary = [pscustomobject]@{
    schema = 'nativegpu.g1-reliability/1'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    deviceRemoval = if ($removal.data) { $removal.data.deviceRemoval } else { $null }
    lifecycle = [pscustomobject]@{
        requested = $LifecycleCycles
        completed = if ($lifecycle.data) { [int]$lifecycle.data.lifecycle.completed } else { 0 }
        maxLiveObjectsAfterShutdown = $lifecycleLeak
        videoMemorySpreadBytes = $vramSpread
        cycles = $lifecycleEntries
    }
    faultTests = if ($faults.data) { $faults.data.faultTests } else { @() }
    diagnostics = if ($faults.data) { $faults.data.probes.diagnostics } else { $null }
    capture = $capture
    captureDetail = $captureDetail
    performance = $perfSummary
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'g1-reliability.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('g1-reliability.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("G1 reliability: {0} checks, {1} failed" -f $checks.Count, @($checks | Where-Object { -not $_.ok }).Count)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

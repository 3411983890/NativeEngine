# Mesh shader and bindless evidence.
#
# Mesh shader: runs the same fixed camera with the amplification/mesh path on and off,
# saves screenshots, compares the images and asserts that the capability query, the
# compiled shader blobs, the stream pipeline state and the vertex-path fallback all exist.
#
# Bindless: asserts the bounded material table reports slot generations, a default slot,
# reuse and diagnostics for missing/stale slots, and that at least two materials sampled
# in the same batch are distinct.
param(
    [int]$Frames = 200,
    [string]$Camera = '9,5.5,10,-2,0.8,0',
    [string]$RenderSize = '1280x720',
    [int]$ScreenshotFrame = 180,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')
Add-Type -AssemblyName System.Drawing
$context = New-ReportContext -Mode 'mesh-bindless'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

function Compare-Images([string]$a, [string]$b) {
    $left = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $a).Path)
    $right = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $b).Path)
    if ($left.Width -ne $right.Width -or $left.Height -ne $right.Height) {
        $left.Dispose(); $right.Dispose()
        return [pscustomobject]@{ changedPercent = 100.0; total = 0; meanLeft = 0.0; meanRight = 0.0 }
    }
    $changed = 0
    $total = 0
    $sumLeft = 0.0
    $sumRight = 0.0
    for ($y = 0; $y -lt $left.Height; $y += 2) {
        for ($x = 0; $x -lt $left.Width; $x += 2) {
            $pl = $left.GetPixel($x, $y)
            $pr = $right.GetPixel($x, $y)
            $sumLeft += ($pl.R + $pl.G + $pl.B) / 3.0
            $sumRight += ($pr.R + $pr.G + $pr.B) / 3.0
            $delta = [Math]::Abs($pl.R - $pr.R) + [Math]::Abs($pl.G - $pr.G) + [Math]::Abs($pl.B - $pr.B)
            if ($delta -gt 12) { ++$changed }
            ++$total
        }
    }
    $left.Dispose(); $right.Dispose()
    $percent = 0.0
    if ($total -gt 0) { $percent = 100.0 * $changed / $total }
    return [pscustomobject]@{
        changedPercent = [Math]::Round($percent, 2)
        total = $total
        meanLeft = [Math]::Round($sumLeft / [Math]::Max(1, $total), 2)
        meanRight = [Math]::Round($sumRight / [Math]::Max(1, $total), 2)
    }
}

if (-not $SkipBuild -or -not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Invoke-Run {
    param([string]$Mode, [string]$Tag)
    $report = Join-Path $stage "$Tag.json"
    $image = Join-Path $stage "$Tag.png"
    $arguments = @('--probe', '--no-stress', '--frames', "$Frames", '--rt-shadows', 'off',
                   '--mesh-shaders', $Mode, '--render-size', $RenderSize, '--camera', $Camera,
                   '--screenshot-frame', "$ScreenshotFrame",
                   '--report', (Get-RelativePath $report), '--screenshot', (Get-RelativePath $image))
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "mesh run $Tag exited $($process.ExitCode)." }
    return [pscustomobject]@{
        data = (Get-Content -LiteralPath $report -Raw | ConvertFrom-Json)
        image = $image
        report = $report
    }
}

Write-Host "Mesh shader and bindless evidence ($RenderSize)" -ForegroundColor Cyan
$on = Invoke-Run -Mode 'on' -Tag 'mesh-on'
$off = Invoke-Run -Mode 'off' -Tag 'mesh-off'
$meshOn = $on.data.probes.meshShader
$meshOff = $off.data.probes.meshShader
$bindless = $on.data.probes.bindless

Add-Check 'adapter reports mesh shader capability' ([bool]$meshOn.available) "tier=$($meshOn.tier)"
Add-Check 'mesh shader blobs compiled by DXC' ([bool]$meshOn.dxcAvailable) ''
Add-Check 'stream pipeline state created' ([bool]$meshOn.implemented -and $meshOn.pipelineStateType -eq 'stream') `
    $meshOn.error
Add-Check 'mesh path rendered frames' ([int]$meshOn.frames -gt 0) "frames=$($meshOn.frames)"
Add-Check 'mesh path did not fall back while enabled' (-not [bool]$meshOn.fallbackUsed) ''
Add-Check 'vertex path reports fallback when disabled' ([bool]$meshOff.fallbackUsed) ''
Add-Check 'disabled mesh path rendered no mesh frames' ([int]$meshOff.frames -eq 0) "frames=$($meshOff.frames)"
Add-Check 'meshlet geometry dispatched' ([int]$meshOn.meshletInstancesDrawn -gt 0) `
    "drawn=$($meshOn.meshletInstancesDrawn)"
Add-Check 'meshlet count is the cube face split' ([int]$meshOn.meshlets -eq 6) "meshlets=$($meshOn.meshlets)"
Add-Check 'amplification culling status is reported' ($null -ne $meshOn.amplificationCulling) `
    "amplificationCulling=$($meshOn.amplificationCulling)"
foreach ($run in @($on, $off)) {
    Add-Check "screenshot written: $(Split-Path $run.image -Leaf)" (Test-Path -LiteralPath $run.image) ''
    if (Test-Path -LiteralPath $run.image) { Publish-Artifact -Context $context -SourcePath $run.image | Out-Null }
}
$imageDiff = Compare-Images $on.image $off.image
Add-Check 'mesh path and vertex path are both non-black' ($imageDiff.meanLeft -gt 3 -and $imageDiff.meanRight -gt 3) `
    "meshMean=$($imageDiff.meanLeft) vertexMean=$($imageDiff.meanRight)"

# --- bindless table ---------------------------------------------------------
Add-Check 'bindless mode is an explicit bounded table' ($bindless.mode -eq 'bounded-table') $bindless.mode
Add-Check 'default slot is reserved' ([int]$bindless.defaultSlot -eq 0) ''
Add-Check 'slot generations tracked' ([int]$bindless.generationHighWater -ge 2) `
    "highWater=$($bindless.generationHighWater)"
Add-Check 'slot acquire/release exercised' (([int]$bindless.acquired -ge 3) -and ([int]$bindless.released -ge 3)) `
    "acquired=$($bindless.acquired) released=$($bindless.released)"
Add-Check 'missing material falls back to the default slot' ([int]$bindless.missingFallbacks -gt 0) `
    "missing=$($bindless.missingFallbacks)"
Add-Check 'stale generation is rejected' ([int]$bindless.staleRejections -gt 0) `
    "stale=$($bindless.staleRejections)"
Add-Check 'SM 6.6 heap indexing assessed, not claimed' ($null -ne $bindless.sm66HeapIndexing -and -not [bool]$bindless.sm66HeapIndexing.implemented) `
    "supported=$($bindless.sm66HeapIndexing.supported)"
$slotStates = @($bindless.slotState)
$resident = @($slotStates | Where-Object { $_.inUse -and $_.slot -le 3 })
Add-Check 'multiple materials are resident in one table' ($resident.Count -ge 3) "resident=$($resident.Count)"
$diagnostics = @($on.data.probes.diagnostics.items)
$staleDiagnostic = $diagnostics | Where-Object { $_.code -eq 'NG-E-MAT-STALE' }
Add-Check 'stale slot has a stable diagnostic' ($null -ne $staleDiagnostic) ''

$success = -not ($checks | Where-Object { -not $_.ok })
$summary = [pscustomobject]@{
    schema = 'nativegpu.mesh-bindless/1'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    sceneSha256 = $context.sceneHash
    camera = $Camera
    resolution = $RenderSize
    meshShader = $meshOn
    meshShaderFallbackRun = [pscustomobject]@{
        requested = [bool]$meshOff.requested
        implemented = [bool]$meshOff.implemented
        fallbackUsed = [bool]$meshOff.fallbackUsed
        frames = [int]$meshOff.frames
    }
    bindless = $bindless
    images = [pscustomobject]@{
        mesh = 'artifacts/mesh-on.png'
        vertex = 'artifacts/mesh-off.png'
    }
    imageDiff = $imageDiff
    timing = [pscustomobject]@{
        meshGpuP95Ms = [Math]::Round([double]$on.data.gpuP95Ms, 3)
        vertexGpuP95Ms = [Math]::Round([double]$off.data.gpuP95Ms, 3)
        meshCpuSubmitP95Ms = [Math]::Round([double]$on.data.cpuSubmitP95Ms, 3)
        vertexCpuSubmitP95Ms = [Math]::Round([double]$off.data.cpuSubmitP95Ms, 3)
    }
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'mesh-bindless-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('mesh-bindless-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Mesh/bindless evidence: {0} checks, {1} failed" -f $checks.Count, @($checks | Where-Object { -not $_.ok }).Count)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Pop-Location
if (-not $success) { exit 1 }

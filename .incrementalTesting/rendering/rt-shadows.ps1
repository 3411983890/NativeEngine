# DXR static shadow evidence: runs the same fixed camera with the pass on and off,
# saves screenshots (final, mask, key light), diffs the two finals, compares frame times
# and asserts that the report fields tell the truth.
#
# Exit code 0 means every check passed and the evidence was archived under
# reports/<version>/<commit12>/<UTC stamp>/.
param(
    [int]$Frames = 240,
    [string]$Camera = '9,5.5,10,-2,0.8,0',
    [string]$RenderSize = '1280x720',
    [int]$ScreenshotFrame = 200,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.InteropServices

$context = New-ReportContext -Mode 'rt-shadows'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

function Get-ImageData([string]$path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($path)
    $width = $bitmap.Width
    $height = $bitmap.Height
    $rect = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
    $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $locked.Stride
    $bytes = New-Object byte[] ($stride * $height)
    [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $bytes, 0, $bytes.Length)
    $bitmap.UnlockBits($locked)
    $bitmap.Dispose()
    return [pscustomobject]@{ bytes = $bytes; stride = $stride; width = $width; height = $height }
}

function Measure-Image([object]$image) {
    $sum = 0.0
    $min = 255
    $max = 0
    $count = 0
    for ($y = 0; $y -lt $image.height; $y += 4) {
        $row = $y * $image.stride
        for ($x = 0; $x -lt $image.width; $x += 4) {
            $index = $row + $x * 4
            $value = ($image.bytes[$index] + $image.bytes[$index + 1] + $image.bytes[$index + 2]) / 3.0
            $sum += $value
            if ($value -lt $min) { $min = $value }
            if ($value -gt $max) { $max = $value }
            ++$count
        }
    }
    if ($count -eq 0) { return [pscustomobject]@{ mean = 0.0; min = 0; max = 0 } }
    return [pscustomobject]@{ mean = $sum / $count; min = $min; max = $max }
}

function Compare-Images([object]$a, [object]$b) {
    $changed = 0
    $total = 0
    $maxDelta = 0
    for ($y = 0; $y -lt $a.height; ++$y) {
        $rowA = $y * $a.stride
        for ($x = 0; $x -lt $a.width; ++$x) {
            $index = $rowA + $x * 4
            $delta = [Math]::Abs([int]$a.bytes[$index] - [int]$b.bytes[$index]) +
                     [Math]::Abs([int]$a.bytes[$index + 1] - [int]$b.bytes[$index + 1]) +
                     [Math]::Abs([int]$a.bytes[$index + 2] - [int]$b.bytes[$index + 2])
            if ($delta -gt 12) { ++$changed }
            if ($delta -gt $maxDelta) { $maxDelta = $delta }
            ++$total
        }
    }
    $percent = 0.0
    if ($total -gt 0) { $percent = 100.0 * $changed / $total }
    return [pscustomobject]@{
        changedPixels = $changed
        totalPixels = $total
        changedPercent = [Math]::Round($percent, 2)
        maxChannelDelta = $maxDelta
    }
}

if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    $SkipBuild = $false
}
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Invoke-ShadowRun {
    param([string]$Mode, [string]$DebugView, [string]$Tag)
    $report = Join-Path $stage "$Tag.json"
    $image = Join-Path $stage "$Tag.png"
    # Start-Process joins ArgumentList without quoting, so the process only ever gets
    # relative paths: the project directory contains spaces.
    $arguments = @('--probe', '--frames', "$Frames", '--rt-shadows', $Mode,
                   '--render-size', $RenderSize, '--camera', $Camera,
                   '--debug-view', $DebugView, '--screenshot-frame', "$ScreenshotFrame",
                   '--report', (Get-RelativePath $report), '--screenshot', (Get-RelativePath $image))
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Shadow run $Tag exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if (-not $data.success) { throw "Shadow run $Tag reported success=false: $($data.error)" }
    return [pscustomobject]@{ data = $data; report = $report; image = $image }
}

Write-Host "DXR shadow evidence ($RenderSize, camera $Camera)" -ForegroundColor Cyan
$on = Invoke-ShadowRun -Mode 'on' -DebugView 'final' -Tag 'shadow-on'
$off = Invoke-ShadowRun -Mode 'off' -DebugView 'final' -Tag 'shadow-off'
$mask = Invoke-ShadowRun -Mode 'on' -DebugView 'mask' -Tag 'shadow-mask'
$key = Invoke-ShadowRun -Mode 'on' -DebugView 'key' -Tag 'shadow-key'

$onProbe = $on.data.probes.raytracingShadows
$offProbe = $off.data.probes.raytracingShadows

# --- truth checks ------------------------------------------------------------
Add-Check 'adapter supports DXR (probeAvailable)' ([bool]$onProbe.probeAvailable) `
    "tier=$($on.data.capabilities.raytracingTier)"
Add-Check 'pinned DXC available' ([bool]$onProbe.dxcAvailable) "version=$($onProbe.dxcVersion)"
Add-Check 'RTPSO created with the pass enabled' ([bool]$onProbe.rtpsoCreated) $onProbe.error
Add-Check 'shader binding table created' ([bool]$onProbe.sbtCreated) ''
Add-Check 'shadow rays dispatched every frame' ([int]$onProbe.dispatchedFrames -gt 0) `
    "dispatched=$($onProbe.dispatchedFrames)"
Add-Check 'effect implemented when enabled' ([bool]$onProbe.effectImplemented) ''
Add-Check 'no raster fallback while the pass is enabled' (-not [bool]$onProbe.fallbackUsed) ''
Add-Check 'capabilities.implemented.raytracing matches the effect' `
    ([bool]$on.data.capabilities.implemented.raytracing) ''
Add-Check 'pass disabled reports fallback' ([bool]$offProbe.fallbackUsed) ''
Add-Check 'pass disabled does not claim the effect' (-not [bool]$offProbe.effectImplemented) ''
Add-Check 'pass disabled dispatches nothing' ([int]$offProbe.dispatchedFrames -eq 0) ''
Add-Check 'pass disabled keeps capabilities.implemented.raytracing false' `
    (-not [bool]$off.data.capabilities.implemented.raytracing) ''
Add-Check 'shadow texture is allocated' ([UInt64]$onProbe.shadowTextureBytes -gt 0) `
    "bytes=$($onProbe.shadowTextureBytes)"
Add-Check 'separable GPU time for the dispatch' ([double]$onProbe.gpuRtMs -gt 0) `
    "gpuRtMs=$($onProbe.gpuRtMs)"

# --- screenshots -------------------------------------------------------------
foreach ($run in @($on, $off, $mask, $key)) {
    Add-Check "screenshot written: $(Split-Path $run.image -Leaf)" (Test-Path -LiteralPath $run.image) ''
    if (Test-Path -LiteralPath $run.image) {
        Publish-Artifact -Context $context -SourcePath $run.image | Out-Null
    }
}
$onImage = Get-ImageData $on.image
$offImage = Get-ImageData $off.image
$maskImage = Get-ImageData $mask.image
$diff = Compare-Images $onImage $offImage
$onStats = Measure-Image $onImage
$offStats = Measure-Image $offImage
$maskStats = Measure-Image $maskImage
Add-Check 'final frame is not black' ($onStats.mean -gt 5.0) ("mean=" + [Math]::Round($onStats.mean, 1))
Add-Check 'shadow mask is not uniform' ($maskStats.max - $maskStats.min -gt 100) `
    ("min=" + [Math]::Round($maskStats.min, 1) + " max=" + [Math]::Round($maskStats.max, 1))
Add-Check 'enabling the pass changes the image' ($diff.changedPercent -ge 0.5) `
    "changed=$($diff.changedPercent)%"
Add-Check 'disabling the pass reproduces the raster baseline' ($diff.maxChannelDelta -gt 0) `
    "maxDelta=$($diff.maxChannelDelta)"

# --- timing ------------------------------------------------------------------
$timing = [pscustomobject]@{
    onGpuP95Ms = [Math]::Round([double]$on.data.gpuP95Ms, 4)
    offGpuP95Ms = [Math]::Round([double]$off.data.gpuP95Ms, 4)
    onCpuP95Ms = [Math]::Round([double]$on.data.cpuSubmitP95Ms, 4)
    offCpuP95Ms = [Math]::Round([double]$off.data.cpuSubmitP95Ms, 4)
    gpuRtMs = [Math]::Round([double]$onProbe.gpuRtMs, 4)
    gpuSampleCount = [int]$on.data.gpuSamples
    resolution = $RenderSize
    frames = $Frames
    note = 'GPU P95 covers the whole command list; gpuRtMs covers only the DispatchRays window.'
}
$timing | Add-Member -NotePropertyName gpuP95DeltaMs `
    -NotePropertyValue ([Math]::Round($timing.onGpuP95Ms - $timing.offGpuP95Ms, 4))
Add-Check 'timing recorded for both modes' (($timing.onGpuP95Ms -gt 0) -and ($timing.offGpuP95Ms -gt 0)) `
    "on=$($timing.onGpuP95Ms) off=$($timing.offGpuP95Ms)"

$success = -not ($checks | Where-Object { -not $_.ok })
$images = [pscustomobject]@{
    finalOn = 'artifacts/shadow-on.png'
    finalOff = 'artifacts/shadow-off.png'
    mask = 'artifacts/shadow-mask.png'
    keyLight = 'artifacts/shadow-key.png'
    diffPercent = $diff.changedPercent
}
$summary = [pscustomobject]@{
    schema = 'nativegpu.rt-shadows/1'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    scene = '.screen/courtyard.scene.json'
    sceneSha256 = $context.sceneHash
    camera = $Camera
    resolution = $RenderSize
    screenshotFrame = $ScreenshotFrame
    adapter = $on.data.capabilities.adapter
    adapterDriver = $context.adapter.driverVersion
    dxcVersion = $onProbe.dxcVersion
    probeAvailable = [bool]$onProbe.probeAvailable
    dxcAvailable = [bool]$onProbe.dxcAvailable
    effectImplemented = [bool]$onProbe.effectImplemented
    fallbackUsed = [bool]$onProbe.fallbackUsed
    rtpsoCreated = [bool]$onProbe.rtpsoCreated
    sbtCreated = [bool]$onProbe.sbtCreated
    shadowTextureBytes = $onProbe.shadowTextureBytes
    dispatchedFrames = [int]$onProbe.dispatchedFrames
    fallbackFrames = [int]$onProbe.fallbackFrames
    dispatchFailures = [int]$onProbe.dispatchFailures
    tlasInstances = [int]$onProbe.tlasInstances
    tlasRebuilds = [int]$onProbe.tlasRebuilds
    gpuRtMs = [double]$onProbe.gpuRtMs
    mode = $onProbe.mode
    tlasPolicy = $onProbe.tlasPolicy
    error = $onProbe.error
    fallbackRun = [pscustomobject]@{
        effectImplemented = [bool]$offProbe.effectImplemented
        fallbackUsed = [bool]$offProbe.fallbackUsed
        dispatchedFrames = [int]$offProbe.dispatchedFrames
        implementedFlag = [bool]$off.data.capabilities.implemented.raytracing
    }
    timing = $timing
    images = $images
    diff = $diff
    imageStats = [pscustomobject]@{
        onMean = [Math]::Round($onStats.mean, 2)
        offMean = [Math]::Round($offStats.mean, 2)
        maskMean = [Math]::Round($maskStats.mean, 2)
    }
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'rt-shadows-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('rt-shadows-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("DXR shadow evidence: {0} checks, {1} failed" -f $checks.Count, ($checks | Where-Object { -not $_.ok }).Count)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

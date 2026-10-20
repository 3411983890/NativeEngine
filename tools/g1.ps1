# G1 release gate.
#
# Reads the newest evidence of every suite, maps it onto the five G1 conditions from
# docs/NEXT-PHASE.md 3.5, and writes reports/latest/g1.json. Exit code 0 means G1 passed.
# This script never re-runs rendering: it validates what the suites actually recorded.
param([switch]$AllowDirtyWorktree)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')

$latest = Join-Path $root 'reports\latest'
function Read-Evidence([string]$name) {
    $path = Join-Path $latest $name
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try { return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json) } catch { return $null }
}

$rt = Read-Evidence 'rt-shadows-evidence.json'
$mesh = Read-Evidence 'mesh-bindless-evidence.json'
$reliability = Read-Evidence 'g1-reliability.json'
$verify = Read-Evidence 'verify.json'

$criteria = New-Object System.Collections.Generic.List[object]
function Add-Criterion([string]$id, [string]$requirement, $ok, [string]$evidence) {
    $passed = [bool]$ok
    $criteria.Add([pscustomobject]@{ id = $id; requirement = $requirement; ok = $passed; evidence = $evidence })
    if ($passed) { Write-Host "  PASS $id $requirement" -ForegroundColor Green }
    else { Write-Host "  FAIL $id $requirement" -ForegroundColor Red }
}

Write-Host "G1 release gate" -ForegroundColor Cyan

$shadowOn = $false
$shadowOff = $false
$shadowImages = $false
if ($rt) {
    $shadowOn = [bool]$rt.effectImplemented -and (-not [bool]$rt.fallbackUsed) -and
                [bool]$rt.rtpsoCreated -and [bool]$rt.sbtCreated -and ([int]$rt.dispatchedFrames -gt 0)
    $shadowOff = [bool]$rt.fallbackRun.fallbackUsed -and (-not [bool]$rt.fallbackRun.effectImplemented)
    $shadowImages = [bool]($rt.images.finalOn) -and [bool]($rt.images.finalOff)
}
$pass1 = $shadowOn -and $shadowOff -and $shadowImages
$detail1 = 'rt-shadows-evidence.json missing'
if ($rt) { $detail1 = "rtpso=$($rt.rtpsoCreated) sbt=$($rt.sbtCreated) dispatched=$($rt.dispatchedFrames) gpuRtMs=$($rt.gpuRtMs) fallbackRun=$($rt.fallbackRun.fallbackUsed)" }
Add-Criterion 'G1.1' 'DXR static shadows have a togglable effect and a raster fallback' $pass1 $detail1

$meshOk = $false
$meshFiles = (Test-Path -LiteralPath 'shaders\mesh_shaders.hlsl') -and
             (Test-Path -LiteralPath 'build\shaders\manifest.json')
if ($mesh) {
    $meshOk = [bool]$mesh.meshShader.implemented -and ([int]$mesh.meshShader.frames -gt 0) -and
              [bool]$mesh.meshShaderFallbackRun.fallbackUsed
}
$pass2 = $meshOk -and [bool]$meshFiles
$detail2 = 'mesh-bindless-evidence.json missing'
if ($mesh) { $detail2 = "frames=$($mesh.meshShader.frames) meshlets=$($mesh.meshShader.meshlets) drawn=$($mesh.meshShader.meshletInstancesDrawn) fallback=$($mesh.meshShaderFallbackRun.fallbackUsed) culling=$($mesh.meshShader.amplificationCulling)" }
Add-Criterion 'G1.2' 'Mesh Shader sample runs with amplification + mesh + pixel and falls back to the vertex path' $pass2 $detail2

$pass3 = $false
$detail3 = 'mesh-bindless-evidence.json missing'
if ($mesh) {
    $pass3 = ($mesh.bindless.mode -eq 'bounded-table') -and
             ([int]$mesh.bindless.generationHighWater -ge 2) -and
             ([int]$mesh.bindless.missingFallbacks -gt 0) -and
             ([int]$mesh.bindless.staleRejections -gt 0) -and
             ($null -ne $mesh.bindless.sm66HeapIndexing)
    $detail3 = "mode=$($mesh.bindless.mode) generations=$($mesh.bindless.generationHighWater) missing=$($mesh.bindless.missingFallbacks) stale=$($mesh.bindless.staleRejections)"
}
Add-Criterion 'G1.3' 'Material slots report missing textures, reuse and out-of-range diagnostics' $pass3 $detail3

$removalOk = $false
$lifecycleOk = $false
$faultOk = $false
$detail4 = 'g1-reliability.json missing'
if ($reliability) {
    $removalOk = [bool]$reliability.deviceRemoval.detected -and [bool]$reliability.deviceRemoval.recovered
    $lifecycleOk = ([int]$reliability.lifecycle.completed -ge 50) -and
                   ([int]$reliability.lifecycle.maxLiveObjectsAfterShutdown -eq 0)
    $faultOk = (@($reliability.faultTests | Where-Object { $_.ok }).Count -ge 2) -and
               ([int]$reliability.diagnostics.events -gt 0)
    $detail4 = "removal=$($reliability.deviceRemoval.detected)/$($reliability.deviceRemoval.recovered) lifecycle=$($reliability.lifecycle.completed) maxLive=$($reliability.lifecycle.maxLiveObjectsAfterShutdown) faults=$(@($reliability.faultTests | Where-Object { $_.ok }).Count) events=$($reliability.diagnostics.events)"
}
$pass4 = $removalOk -and $lifecycleOk -and $faultOk
Add-Criterion 'G1.4' 'Device removal, resource release and illegal-input tests each exist and pass' $pass4 $detail4

$timingOk = $false
$perfOk = $false
$captureRecorded = $false
$detail5 = 'timing evidence missing'
if ($rt -and $reliability) {
    $timingOk = ([double]$rt.gpuRtMs -gt 0) -and ([double]$rt.timing.onGpuP95Ms -gt 0) -and
                ([double]$rt.timing.offGpuP95Ms -gt 0)
    $perfOk = ([double]$reliability.performance.cpuSubmitP50Ms -gt 0) -and
              ([double]$reliability.performance.gpuP50Ms -gt 0)
    $captureRecorded = $null -ne $reliability.capture
    $detail5 = "rtGpuMs=$($rt.gpuRtMs) gpuP50=$($reliability.performance.gpuP50Ms) cpuSubmitP50=$($reliability.performance.cpuSubmitP50Ms) capture=$($reliability.captureDetail)"
}
$docsOk = (Test-Path -LiteralPath 'docs\PERF-BASELINE.md') -and (Test-Path -LiteralPath 'docs\CAPTURE.md') -and
          (Test-Path -LiteralPath 'docs\RT-SHADOWS.md') -and (Test-Path -LiteralPath 'docs\MESH-BINDLESS.md')
$pass5 = $timingOk -and $perfOk -and $captureRecorded -and $docsOk
Add-Criterion 'G1.5' 'Screenshots, capture status and frame timings are recorded and replayable' $pass5 $detail5

$verifyOk = ($null -ne $verify) -and [bool]$verify.success
$detail0 = 'verify.json missing'
if ($verify) { $detail0 = "steps=$(@($verify.steps).Count) failed=$(@($verify.steps | Where-Object { -not $_.ok }).Count)" }
Add-Criterion 'G1.0' 'The full verify suite is green in the same working tree' $verifyOk $detail0

$commit = Get-GitCommit
$dirty = Get-GitDirty
$dirtyOk = (-not $dirty) -or $AllowDirtyWorktree
if (-not $dirtyOk) {
    Write-Host "  WARN worktree is dirty; G1 evidence is not release-grade until the tree is clean" -ForegroundColor Yellow
}
$failedCount = @($criteria | Where-Object { -not $_.ok }).Count
$passed = ($failedCount -eq 0) -and $dirtyOk

$sources = [pscustomobject]@{
    rtShadows = 'reports/latest/rt-shadows-evidence.json'
    meshBindless = 'reports/latest/mesh-bindless-evidence.json'
    reliability = 'reports/latest/g1-reliability.json'
    verify = 'reports/latest/verify.json'
}
$next = [pscustomobject]@{
    stop = 'G1 passed: stop expanding graphics'
    blocked = 'Chromium S2 starts only after explicit user confirmation'
    checklist = 'docs/CHROMIUM-START.md'
}
$summary = [pscustomobject]@{
    schema = 'nativegpu.g1/1'
    version = Get-ProjectVersion
    commit = $commit
    worktreeDirty = $dirty
    allowDirtyWorktree = [bool]$AllowDirtyWorktree
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    criteria = $criteria
    failed = $failedCount
    passed = $passed
    sources = $sources
    nextPhase = $next
}
New-Item -ItemType Directory -Force -Path $latest | Out-Null
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $latest 'g1.json') -Encoding utf8
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'reports/g1.json' -Encoding utf8

Write-Host ""
$criteria | Format-Table id, ok, requirement -AutoSize | Out-String | Write-Host
$state = 'NOT PASSED'
if ($passed) { $state = 'PASSED' }
$cleanState = 'clean'
if ($dirty) { $cleanState = 'dirty' }
Write-Host ("G1 {0} (commit {1}, {2})" -f $state, $commit.Substring(0, 12), $cleanState)
Write-Host "Gate report: reports/g1.json"
Pop-Location
if (-not $passed) { exit 1 }

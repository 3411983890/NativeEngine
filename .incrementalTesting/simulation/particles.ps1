# Particle field evidence: runs the smoke with the particle system on and off, then asserts
# the report fields and proves the pass actually changes the image.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/particles.ps1
#
# Exits non-zero on the first failed check. Every number in the evidence file comes from a
# real run; nothing is written by hand.

param(
    [int]$Frames = 240,
    [int]$LifecycleCycles = 5,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location -LiteralPath $root

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $script:checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    $status = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1} - {2}" -f $status, $name, $detail)
}

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

$exe = Join-Path $root 'build\NativeGPU.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'build\NativeGPU.exe is missing; run tools/build.ps1' }
. (Join-Path $root 'tools/native-build-state.ps1')
Assert-NativeBuildCurrent

$outDir = Join-Path $root 'reports\tmp'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$camera = '7,4,10,0,2,0'
# Emit inside the courtyard crate. With collision on the field is pushed back out of the
# box; with the collider toggle off the source stays inside the solid and the plume differs.
$scene = Get-Content -LiteralPath (Join-Path $root '.screen\courtyard.scene.json') -Raw | ConvertFrom-Json
$crate = $scene.entities | Where-Object { $_.collider -and $_.name -eq 'Crate' } | Select-Object -First 1
if (-not $crate) { throw 'Crate collider not found in the courtyard scene' }
$emitter = (($crate.position | ForEach-Object { [double]$_ }) -join ',')
$onReport = Join-Path $outDir 'particles-on.json'
$offReport = Join-Path $outDir 'particles-off.json'
$onImage = Join-Path $outDir 'particles-on.png'
$offImage = Join-Path $outDir 'particles-off.png'
$nocollideReport = Join-Path $outDir 'particles-nocollide.json'
$nocollideImage = Join-Path $outDir 'particles-nocollide.png'
$lifecycleReport = Join-Path $outDir 'particles-lifecycle.json'
if ($LifecycleCycles -lt 1 -or $LifecycleCycles -gt 100) { throw 'LifecycleCycles must be in [1,100].' }

& $exe --smoke --no-watch --frames $Frames --particles on --particle-emitter $emitter --camera $camera `
    --screenshot $onImage --screenshot-frame ($Frames - 20) --report $onReport | Out-Null
Add-Check 'particle run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

& $exe --smoke --no-watch --frames $Frames --particles off --camera $camera `
    --screenshot $offImage --screenshot-frame ($Frames - 20) --report $offReport | Out-Null
Add-Check 'baseline run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

& $exe --smoke --no-watch --frames $Frames --particles on --particle-emitter $emitter --particle-colliders off --camera $camera `
    --screenshot $nocollideImage --screenshot-frame ($Frames - 20) --report $nocollideReport | Out-Null
Add-Check 'no-collider run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

$on = Get-Content -LiteralPath $onReport -Raw | ConvertFrom-Json
$off = Get-Content -LiteralPath $offReport -Raw | ConvertFrom-Json
$nocollide = Get-Content -LiteralPath $nocollideReport -Raw | ConvertFrom-Json

$p = $on.probes.particles
Add-Check 'particles requested' ($p.requested -eq $true) "requested=$($p.requested)"
Add-Check 'particle pipeline built' ($p.available -eq $true) "available=$($p.available) error='$($p.error)'"
Add-Check 'particle pass dispatched' ($p.frames -gt 0) "frames=$($p.frames)"
Add-Check 'no dispatch failures' ($p.failures -eq 0) "failures=$($p.failures)"
Add-Check 'GPU indirect dispatch executes' ($p.indirect.active -and $p.indirect.dispatches -gt 0) "dispatches=$($p.indirect.dispatches)"
Add-Check 'indirect arguments validated asynchronously' ($p.indirect.validatedSamples -gt 0 -and $p.indirect.argumentMismatches -eq 0) "samples=$($p.indirect.validatedSamples) mismatches=$($p.indirect.argumentMismatches)"
Add-Check 'compute queue measured separately' ($p.computeTiming.available -and $p.computeTiming.samples -gt 0 -and $p.computeTiming.lastMs -gt 0) "samples=$($p.computeTiming.samples) ms=$($p.computeTiming.lastMs)"
Add-Check 'telemetry does not synchronously wait' ($p.computeTiming.blockingReadback -eq $false) 'readback only maps completed old ring slots'
Add-Check 'volumetric draws recorded' ($p.draws -gt 0) "draws=$($p.draws)"
Add-Check 'integer-atomic splat' ($p.splat -eq 'integer-atomic') "splat=$($p.splat)"
Add-Check 'field solver is the advection + projection stack' ($p.solver -eq 'buoyancy+advection+vorticity+jacobi') "solver=$($p.solver)"
Add-Check 'solver passes recorded' ($p.solverPasses -gt 0) "solverPasses=$($p.solverPasses) iterations=$($p.solverIterations)"
Add-Check 'field resolution is 96^3' ($p.volumeDim -eq 96) "volumeDim=$($p.volumeDim)"
Add-Check 'field voxel size' ([math]::Abs([double]$p.voxelMetres - (16.0 / 96.0)) -lt 0.001) "voxelMetres=$($p.voxelMetres)"
Add-Check 'capability field set only when implemented' ([bool]$on.capabilities.implemented.raytracing -eq [bool]$on.probes.raytracingShadows.effectImplemented) 'shadow contract kept while particles are on'

$baseline = $off.probes.particles
Add-Check 'baseline stays disabled' ($baseline.requested -eq $false -and $baseline.draws -eq 0) "requested=$($baseline.requested) draws=$($baseline.draws)"

Add-Check 'scene colliders reach the field' ($p.colliders -gt 0) "colliders=$($p.colliders) rejected=$($p.collidersRejected)"
Add-Check 'no collider was dropped' ($p.collidersRejected -eq 0) "rejected=$($p.collidersRejected)"
Add-Check 'collider toggle really disables them' ($nocollide.probes.particles.colliders -eq 0) "colliders=$($nocollide.probes.particles.colliders)"

$onHash = (Get-FileHash -LiteralPath $onImage).Hash
$offHash = (Get-FileHash -LiteralPath $offImage).Hash
$nocollideHash = (Get-FileHash -LiteralPath $nocollideImage).Hash
Add-Check 'particle pass changes the image' ($onHash -ne $offHash) "on=$($onHash.Substring(0,12)) off=$($offHash.Substring(0,12))"
Add-Check 'collision changes the image' ($onHash -ne $nocollideHash) "on=$($onHash.Substring(0,12)) nocollide=$($nocollideHash.Substring(0,12))"

Add-Check 'scene still renders with particles on' ($on.success -eq $true) "success=$($on.success) error='$($on.error)'"

& $exe --smoke --no-watch --particles on --lifecycle $LifecycleCycles --frames ($LifecycleCycles * 8 + 8) `
    --render-size 640x360 --report $lifecycleReport | Out-Null
$lifecycleExit = $LASTEXITCODE
$lifecycle = Get-Content -LiteralPath $lifecycleReport -Raw | ConvertFrom-Json
Add-Check 'particle resources survive renderer rebuilds' ($lifecycleExit -eq 0 -and $lifecycle.success -and $lifecycle.lifecycle.completed -eq $LifecycleCycles) "cycles=$($lifecycle.lifecycle.completed) exit=$lifecycleExit"
$leaks = @($lifecycle.lifecycle.entries | Where-Object { $_.liveObjectsAfterShutdown -ne 0 })
Add-Check 'particle shutdown releases every tracked GPU object' ($leaks.Count -eq 0 -and @($lifecycle.lifecycle.entries).Count -eq $LifecycleCycles) "leakingCycles=$($leaks.Count)"

$evidence = [pscustomobject]@{
    generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    frames = $Frames
    camera = $camera
    resolution = $on.resolution
    particles = $p
    baseline = $baseline
    noColliders = $nocollide.probes.particles
    lifecycle = $lifecycle.lifecycle
    images = [pscustomobject]@{ on = $onImage; off = $offImage; noColliders = $nocollideImage; onSha256 = $onHash; offSha256 = $offHash; noCollidersSha256 = $nocollideHash }
    checks = $checks
    ok = (@($checks | Where-Object { -not $_.ok }).Count -eq 0)
}
$evidencePath = Join-Path $root 'reports\particles-evidence.json'
$evidence | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $evidencePath -Encoding utf8

. (Join-Path $root 'tools/report-lib.ps1')
$particleContext = New-ReportContext -Mode 'particles'
$evidence | Add-Member -NotePropertyName build -NotePropertyValue $particleContext.build
$evidence | Add-Member -NotePropertyName sourceManifestHash -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $root 'build/source-inputs.json') -Algorithm SHA256).Hash
$evidence.images.on = Publish-Artifact -Context $particleContext -SourcePath $onImage -Name 'particles-on.png'
$evidence.images.off = Publish-Artifact -Context $particleContext -SourcePath $offImage -Name 'particles-off.png'
$evidence.images.noColliders = Publish-Artifact -Context $particleContext -SourcePath $nocollideImage -Name 'particles-no-colliders.png'
$archivedEvidence = Join-Path $particleContext.directory 'particles-evidence.json'
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $archivedEvidence -Encoding utf8
Register-Report -Context $particleContext -Path $archivedEvidence -LegacyAliases @('particles-evidence.json') | Out-Null

$failed = @($checks | Where-Object { -not $_.ok })
Write-Host ("{0}/{1} checks passed. Evidence: {2}" -f ($checks.Count - $failed.Count), $checks.Count, $evidencePath)
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Host ("  FAILED: {0} ({1})" -f $f.name, $f.detail) -ForegroundColor Red }
    exit 1
}
exit 0

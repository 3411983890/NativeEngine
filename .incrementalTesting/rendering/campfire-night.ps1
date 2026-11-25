# Campfire night acceptance: renders the three-view evidence set for
# .screen/campfire-night.scene.json and asserts every line of the FIELD-REALISM-GOAL
# acceptance table against a real run.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/campfire-night.ps1 [-SkipBuild]
#
# Exits non-zero on the first failed check. Every number in the evidence file comes from a
# report the renderer wrote, never from a constant in this script.

param(
    [switch]$SkipBuild,
    [int]$Frames = 240
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

$scene = '.screen\campfire-night.scene.json'
if (-not (Test-Path -LiteralPath $scene)) { throw "missing $scene" }

$validation = & node tools\scene.mjs validate $scene 2>&1 | Out-String
Add-Check 'campfire-night scene validates' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

$exe = Join-Path $root 'build\NativeGPU.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'build\NativeGPU.exe is missing; run tools/build.ps1' }

# Acceptance artifacts live under reports/ (not reports/tmp) so the three views and the
# temporal-off comparison stay with the evidence file.
$outDir = Join-Path $root 'reports\campfire-night'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Three views of the same fire. The scene key light sits at -Z, so 'front' looks at the
# fire with the moon behind it (rim lit), 'back' puts the key behind the camera, and 'side'
# is a profile that shows the plume silhouette.
$views = @(
    [pscustomobject]@{ id = 'front'; camera = '3.2,1.35,4.4,0,0.55,0' },
    [pscustomobject]@{ id = 'back'; camera = '-3.6,1.5,-4.2,0,0.6,0' },
    [pscustomobject]@{ id = 'side'; camera = '4.6,1.15,-0.7,0,0.55,0' }
)

$renders = @{}
foreach ($view in $views) {
    $image = Join-Path $outDir "campfire-night-$($view.id).png"
    $report = Join-Path $outDir "campfire-night-$($view.id).json"
    & $exe --smoke --no-watch --scene $scene --frames $Frames --particles on --fire `
        --camera $view.camera --screenshot $image --screenshot-frame $Frames `
        --report $report | Out-Null
    Add-Check "render '$($view.id)' exits cleanly" ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
    $renders[$view.id] = [pscustomobject]@{
        image = $image
        report = $report
        json = (Get-Content -LiteralPath $report -Raw | ConvertFrom-Json)
        hash = (Get-FileHash -LiteralPath $image).Hash
    }
}

# Temporal fallback: the same frame with the accumulation path disabled must report itself
# off and still produce a different (but valid) image.
$directImage = Join-Path $outDir 'campfire-night-direct.png'
$directReport = Join-Path $outDir 'campfire-night-direct.json'
& $exe --smoke --no-watch --scene $scene --frames $Frames --particles on --fire `
    --particle-temporal off --camera $views[0].camera --screenshot $directImage `
    --screenshot-frame $Frames --report $directReport | Out-Null
Add-Check 'temporal-off render exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
$direct = Get-Content -LiteralPath $directReport -Raw | ConvertFrom-Json
$directHash = (Get-FileHash -LiteralPath $directImage).Hash

$front = $renders['front']
$p = $front.json.probes.particles
$d = $direct.probes.particles
$fire = $front.json.fireLight

Add-Check 'scene loads and renders' ($front.json.success -eq $true) "success=$($front.json.success) error='$($front.json.error)'"
Add-Check 'particle field is live' ($p.available -eq $true -and $p.failures -eq 0 -and $p.draws -gt 0) "available=$($p.available) failures=$($p.failures) draws=$($p.draws)"
Add-Check 'fire preset active' ($p.firePreset -eq $true) "firePreset=$($p.firePreset)"
Add-Check 'shading is the full new model' `
    ($p.shading -eq 'blackbody+hg+multiscatter+fireglow') "shading=$($p.shading)"
Add-Check 'emission is Planck chroma + heat radiance' `
    ($p.emissionModel -eq 'planck-chroma+heat-radiance') "emissionModel=$($p.emissionModel)"
Add-Check 'deterministic trilinear splat kernel' ($p.splatKernel -eq 'trilinear2x2x2' -and $p.splat -eq 'integer-atomic') "kernel=$($p.splatKernel) splat=$($p.splat)"
Add-Check 'MacCormack second-order advection on' ($p.macCormack -eq $true) "macCormack=$($p.macCormack)"
Add-Check 'curl-noise turbulence active' ($p.turbulence -gt 0) "turbulence=$($p.turbulence)"
Add-Check 'fire field voxel is fine enough' ($p.voxelMetres -le 0.07) "voxel=$($p.voxelMetres) m extent=$($p.volumeExtent) m dim=$($p.volumeDim)"
Add-Check 'solver consumed the lease without stalling' ($p.leaseStalls -eq 0 -and $p.leasesComplete -eq $true) "stalls=$($p.leaseStalls) complete=$($p.leasesComplete) passes=$($p.solverPasses)"
Add-Check 'temporal accumulation is active' ($p.temporal.active -eq $true -and $p.temporal.accumulatedFrames -gt 0) "active=$($p.temporal.active) frames=$($p.temporal.accumulatedFrames) half=$($p.temporal.halfWidth)x$($p.temporal.halfHeight)"
Add-Check 'temporal can be switched off' ($d.temporal.requested -eq $false -and $d.temporal.active -eq $false) "requested=$($d.temporal.requested) active=$($d.temporal.active) frames=$($d.temporal.accumulatedFrames)"
Add-Check 'temporal off changes the image' ($directHash -ne $front.hash) "off=$($directHash.Substring(0,12)) on=$($front.hash.Substring(0,12))"
Add-Check 'fire drives the scene light' ($fire.applied -eq $true -and $fire.intensity -gt 0) "applied=$($fire.applied) intensity=$($fire.intensity) flicker=$($fire.flicker)"

$hashFront = $front.hash
$hashBack = $renders['back'].hash
$hashSide = $renders['side'].hash
Add-Check 'three views are distinct' `
    ($hashFront -ne $hashBack -and $hashBack -ne $hashSide -and $hashFront -ne $hashSide) `
    "front=$($hashFront.Substring(0,12)) back=$($hashBack.Substring(0,12)) side=$($hashSide.Substring(0,12))"
Add-Check 'back view also renders the field' ($renders['back'].json.probes.particles.draws -gt 0 -and $renders['side'].json.probes.particles.draws -gt 0) "back=$($renders['back'].json.probes.particles.draws) side=$($renders['side'].json.probes.particles.draws)"

$evidence = [pscustomobject]@{
    generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    scene = $scene
    frames = $Frames
    resolution = $front.json.resolution
    build = $front.json.build
    particles = $p
    fireLight = $fire
    temporalOff = $d.temporal
    views = @($views | ForEach-Object {
        [pscustomobject]@{
            id = $_.id
            camera = $_.camera
            image = $renders[$_.id].image
            sha256 = $renders[$_.id].hash
            particles = $renders[$_.id].json.probes.particles
        }
    })
    directImage = $directImage
    directSha256 = $directHash
    checks = $checks
    ok = (@($checks | Where-Object { -not $_.ok }).Count -eq 0)
}

$evidencePath = Join-Path $root 'reports\campfire-night-evidence.json'
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8

$failed = @($checks | Where-Object { -not $_.ok })
Write-Host ("{0}/{1} checks passed. Evidence: {2}" -f ($checks.Count - $failed.Count), $checks.Count, $evidencePath)
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Host ("  FAILED: {0} ({1})" -f $f.name, $f.detail) -ForegroundColor Red }
    exit 1
}
exit 0

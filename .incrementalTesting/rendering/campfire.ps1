# Campfire evidence: validates the scene, then renders a steady fire and an explosion frame.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/campfire.ps1 [-SkipBuild]
#
# Exits non-zero on the first failed check. Every number comes from a real run.

param(
    [switch]$SkipBuild,
    [int]$SteadyFrames = 300,
    [int]$BoomFrames = 322
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

$scene = '.screen\campfire.scene.json'
if (-not (Test-Path -LiteralPath $scene)) { throw "missing $scene" }

$validation = & node tools\scene.mjs validate $scene 2>&1 | Out-String
Add-Check 'campfire scene validates' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

$exe = Join-Path $root 'build\NativeGPU.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'build\NativeGPU.exe is missing; run tools/build.ps1' }

$outDir = Join-Path $root 'reports\tmp'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$camera = '5.5,3.0,8.5,0,0.9,0'
$explodeAt = 150

$steadyImage = Join-Path $outDir 'campfire-steady.png'
$steadyReport = Join-Path $outDir 'campfire-steady.json'
$boomImage = Join-Path $outDir 'campfire-boom.png'
$boomReport = Join-Path $outDir 'campfire-boom.json'
$afterImage = Join-Path $outDir 'campfire-aftermath.png'
$afterReport = Join-Path $outDir 'campfire-aftermath.json'

& $exe --smoke --no-watch --scene $scene --frames $SteadyFrames --particles on --fire `
    --camera $camera --screenshot $steadyImage --screenshot-frame $SteadyFrames `
    --report $steadyReport | Out-Null
Add-Check 'steady fire run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

& $exe --smoke --no-watch --scene $scene --frames $BoomFrames --particles on --fire `
    --explode-at $explodeAt --camera $camera --screenshot $boomImage `
    --screenshot-frame $BoomFrames --report $boomReport | Out-Null
Add-Check 'explosion run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

& $exe --smoke --no-watch --scene $scene --frames 420 --particles on --fire `
    --explode-at $explodeAt --camera $camera --screenshot $afterImage `
    --screenshot-frame 420 --report $afterReport | Out-Null
Add-Check 'aftermath run exits cleanly' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

$steady = Get-Content -LiteralPath $steadyReport -Raw | ConvertFrom-Json
$boom = Get-Content -LiteralPath $boomReport -Raw | ConvertFrom-Json
$after = Get-Content -LiteralPath $afterReport -Raw | ConvertFrom-Json
$p = $steady.probes.particles
$b = $boom.probes.particles

Add-Check 'campfire scene loads' ($steady.success -eq $true) "success=$($steady.success) error='$($steady.error)'"
Add-Check 'fire preset active' ($p.firePreset -eq $true) "firePreset=$($p.firePreset)"
Add-Check 'no explosion in the steady run' ($p.explodeAt -eq 0 -and $p.burstPeak -eq 0) "explodeAt=$($p.explodeAt) burstPeak=$($p.burstPeak)"
Add-Check 'explosion scheduled' ($b.explodeAt -eq $explodeAt) "explodeAt=$($b.explodeAt)"
Add-Check 'explosion burst fired' ($b.burstPeak -gt 0.9) "burstPeak=$($b.burstPeak)"
Add-Check 'vorticity confinement active' ($p.vorticity -eq $true -and $p.solverPasses -gt 0) "solver=$($p.solver) passes=$($p.solverPasses)"
Add-Check 'fire scene colliders uploaded' ($p.colliders -gt 0) "colliders=$($p.colliders) rejected=$($p.collidersRejected)"
Add-Check 'field channels are leased' ($p.leasesComplete -eq $true) "leasesComplete=$($p.leasesComplete)"
Add-Check 'solver consumed the lease without stalling' ($p.leaseStalls -eq 0 -and $p.leaseAcquisitions -gt 0) "stalls=$($p.leaseStalls) acquisitions=$($p.leaseAcquisitions)"
Add-Check 'density lease has a live reference' ($p.leases.density.refs -ge 1) "refs=$($p.leases.density.refs) level=$($p.leases.density.level)"
Add-Check 'explosion scorches nearby entities' ($after.scorched -gt 0 -and $after.scorchApplied -eq $true) "scorched=$($after.scorched) applied=$($after.scorchApplied)"

$steadyHash = (Get-FileHash -LiteralPath $steadyImage).Hash
$boomHash = (Get-FileHash -LiteralPath $boomImage).Hash
$afterHash = (Get-FileHash -LiteralPath $afterImage).Hash
Add-Check 'explosion changes the image' ($steadyHash -ne $boomHash) "steady=$($steadyHash.Substring(0,12)) boom=$($boomHash.Substring(0,12))"
Add-Check 'aftermath differs from the blast frame' ($afterHash -ne $boomHash) "after=$($afterHash.Substring(0,12)) boom=$($boomHash.Substring(0,12))"

$evidence = [pscustomobject]@{
    generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    scene = $scene
    camera = $camera
    explodeAt = $explodeAt
    steady = $p
    explosion = $b
    aftermath = $after
    images = [pscustomobject]@{ steady = $steadyImage; boom = $boomImage; aftermath = $afterImage; steadySha256 = $steadyHash; boomSha256 = $boomHash; aftermathSha256 = $afterHash }
    checks = $checks
    ok = (@($checks | Where-Object { -not $_.ok }).Count -eq 0)
}
$evidencePath = Join-Path $root 'reports\campfire-evidence.json'
$evidence | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $evidencePath -Encoding utf8

$failed = @($checks | Where-Object { -not $_.ok })
Write-Host ("{0}/{1} checks passed. Evidence: {2}" -f ($checks.Count - $failed.Count), $checks.Count, $evidencePath)
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Host ("  FAILED: {0} ({1})" -f $f.name, $f.detail) -ForegroundColor Red }
    exit 1
}
exit 0

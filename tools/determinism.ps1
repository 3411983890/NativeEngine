# Determinism evidence: runs the same automated session more than once, with different
# rendering settings, and proves the simulation digest does not move. This is the gate
# every later replay / fork / image-regression feature depends on, so it is asserted
# instead of assumed.
#
# Exit code 0 means every check passed and the evidence was archived under
# reports/<version>/<commit12>/<UTC stamp>/.
param(
    [int]$Frames = 240,
    [string]$RenderSize = '1280x720',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')

$context = New-ReportContext -Mode 'determinism'
$checks = New-Object System.Collections.Generic.List[object]
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { $SkipBuild = $false }
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path

# The executable is a GUI subsystem binary, so each run must be waited on explicitly.
# Only relative paths are passed: the project directory contains spaces.
function Invoke-Session {
    param([string]$Tag, [string[]]$Extra = @())
    $report = Join-Path $stage "$Tag.json"
    $log = Join-Path $stage "$Tag.log"
    $arguments = @('--smoke', '--no-watch', '--no-stress',
                   '--scene', 'assets/courtyard.scene.json',
                   '--frames', "$Frames", '--render-size', $RenderSize,
                   '--report', (Get-RelativePath $report),
                   '--hash-log', (Get-RelativePath $log)) + $Extra
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Session $Tag exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if (-not $data.success) { throw "Session $Tag reported success=false: $($data.error)" }
    return [pscustomobject]@{ data = $data; report = $report; log = $log }
}

function Read-Log([string]$path) {
    return [System.IO.File]::ReadAllText($path)
}

Write-Host "Determinism evidence ($Frames frames, $RenderSize)" -ForegroundColor Cyan
$first = Invoke-Session -Tag 'base-a'
$second = Invoke-Session -Tag 'base-b'
$small = Invoke-Session -Tag 'small-view' -Extra @('--render-size', '640x360')
$mask = Invoke-Session -Tag 'debug-mask' -Extra @('--debug-view', 'mask')
$seeded = Invoke-Session -Tag 'seed-7' -Extra @('--seed', '7')

$logA = Read-Log $first.log
$logB = Read-Log $second.log
$logSmall = Read-Log $small.log
$logMask = Read-Log $mask.log
$logSeed = Read-Log $seeded.log

$linesA = $logA -split "`n" | Where-Object { $_ -ne '' }
$bodyA = $linesA | Where-Object { -not $_.StartsWith('#') }
$bodySeed = ($logSeed -split "`n") | Where-Object { $_ -ne '' -and -not $_.StartsWith('#') }
$bodyTextA = $bodyA -join "`n"
$bodyTextSeed = $bodySeed -join "`n"
$lastLine = $bodyA[-1]
$lastParts = $lastLine -split ','
$lastTick = [int]$lastParts[1]
$lastHash = $lastParts[2]

$determinism = $first.data.determinism
$expectedTicks = $Frames * 2

Add-Check 'hash log written with a header and one line per frame' `
    ($linesA.Count -eq ($Frames + 1)) "lines=$($linesA.Count) expected=$($Frames + 1)"
Add-Check 'hash log header records the seed' ($logA.StartsWith('# nativegpu-replay v2 seed=20261020')) `
    ($linesA[0])
Add-Check 'two runs with identical input produce byte-identical digests' ($logA -ceq $logB) `
    'digest logs differ'
Add-Check 'render resolution does not change the simulation digest' ($logA -ceq $logSmall) `
    '640x360 produced a different digest sequence'
Add-Check 'composite debug view does not change the simulation digest' ($logA -ceq $logMask) `
    '--debug-view mask produced a different digest sequence'
Add-Check 'a different seed is recorded separately in the report' `
    ([long]$seeded.data.determinism.seed -eq 7) "$($seeded.data.determinism.seed)"
Add-Check 'a different seed changes the RNG fingerprint' `
    ([string]$seeded.data.determinism.rng.fingerprint -ne [string]$determinism.rng.fingerprint) `
    'seed 7 produced the same RNG fingerprint'
# No consumer draws from a stream yet, so the simulation digest is expected to stay equal
# across seeds. Recording that expectation makes the day it changes visible.
Add-Check 'digest stays seed-independent while no consumer draws from a stream' `
    ($bodyTextA -ceq $bodyTextSeed) 'the digest moved without an RNG consumer'
Add-Check 'fixed 120 Hz step advances two ticks per smoke frame' ($lastTick -eq $expectedTicks) `
    "ticks=$lastTick expected=$expectedTicks"
Add-Check 'report ticks match the last logged tick' ([int]$determinism.ticks -eq $lastTick) `
    "report=$($determinism.ticks) log=$lastTick"
Add-Check 'report final hash matches the last logged hash' ([string]$determinism.finalHash -eq $lastHash) `
    "report=$($determinism.finalHash) log=$lastHash"
Add-Check 'report declares the fixed 120 Hz step' ([int]$determinism.fixedStepHz -eq 120) `
    "$($determinism.fixedStepHz)"
Add-Check 'run seed is echoed in the report' ([long]$determinism.seed -eq 20261020) `
    "$($determinism.seed)"
Add-Check 'report records the hash log path' (-not [string]::IsNullOrEmpty([string]$determinism.hashLog)) `
    'hashLog is empty'
Add-Check 'two named RNG streams are registered' (@($determinism.rng.streams).Count -eq 2) `
    "count=$(@($determinism.rng.streams).Count)"
Add-Check 'RNG fingerprint is a 16 digit hex value' ([string]$determinism.rng.fingerprint -match '^[0-9a-f]{16}$') `
    "$($determinism.rng.fingerprint)"
Add-Check 'stream registrations are ordered gameplay, cosmetic' `
    (([string]$determinism.rng.streams[0].name -eq 'gameplay') -and ([string]$determinism.rng.streams[1].name -eq 'cosmetic')) `
    "names=$($determinism.rng.streams.name -join ',')"

# Static gate: the simulation must not reach for process-global randomness or wall clock
# time. Those are the two ways a digest silently stops reproducing.
$forbidden = @{
    'std::rand' = 'rand\s*\('
    'srand' = 'srand\s*\('
    'std::random_device' = 'random_device'
    'wall clock' = '(std::time|time\s*\(\s*nullptr|GetTickCount|QueryPerformanceCounter)'
}
$simFiles = Get-ChildItem -Path 'engine\native' -File -Include '*.cpp', '*.hpp' -Recurse
$violations = New-Object System.Collections.Generic.List[string]
foreach ($file in $simFiles) {
    # Comments may name the forbidden APIs; only code lines count.
    $text = (Get-Content -LiteralPath $file.FullName -Raw) -split "`n" |
        Where-Object { $_.Trim() -notmatch '^(//|\*|/\*)' } |
        Out-String
    foreach ($name in $forbidden.Keys) {
        if ($text -match $forbidden[$name]) {
            $violations.Add("$($file.Name): $name")
        }
    }
}
Add-Check 'simulation sources contain no global RNG or wall-clock calls' ($violations.Count -eq 0) `
    ($violations -join '; ')
Add-Check 'the simulation owns a determinism header' (Test-Path -LiteralPath 'engine\native\determinism.hpp') `
    'determinism.hpp missing'

$failed = @($checks | Where-Object { -not $_.ok }).Count
$success = $failed -eq 0

$summary = [pscustomobject]@{
    schema = 'nativegpu.determinism/1'
    frames = $Frames
    renderSize = $RenderSize
    seed = 20261020
    ticks = $lastTick
    expectedTicks = $expectedTicks
    finalHash = $lastHash
    rngFingerprint = [string]$determinism.rng.fingerprint
    streams = @($determinism.rng.streams | ForEach-Object { [pscustomobject]@{ name = $_.name; counter = $_.counter } })
    identicalRuns = ($logA -ceq $logB)
    resolutionIndependent = ($logA -ceq $logSmall)
    debugViewIndependent = ($logA -ceq $logMask)
    seedSensitive = ([string]$seeded.data.determinism.rng.fingerprint -ne [string]$determinism.rng.fingerprint)
    staticGateViolations = @($violations)
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'determinism-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('determinism-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Determinism evidence: {0} checks, {1} failed" -f $checks.Count, $failed)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

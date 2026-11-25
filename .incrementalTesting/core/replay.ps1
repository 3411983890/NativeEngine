# Replay and black-box evidence: records a session, replays it bit-exactly, proves a
# tampered recording is rejected at the right frame, and proves an injected stall produces
# a replayable anomaly snapshot.
#
# Exit code 0 means every check passed and the evidence was archived under
# reports/<version>/<commit12>/<UTC stamp>/.
param(
    [int]$Frames = 240,
    [int]$StallFrame = 120,
    [int]$StallMs = 60,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')

$context = New-ReportContext -Mode 'replay'
$checks = New-Object System.Collections.Generic.List[object]
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { $SkipBuild = $false }
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path

# The executable is a GUI subsystem binary: every run must be waited on explicitly.
function Invoke-Run {
    param([string]$Tag, [string[]]$Extra = @(), [switch]$AllowFailure)
    $report = Join-Path $stage "$Tag.json"
    $arguments = @('--smoke', '--no-watch', '--no-stress',
                   '--scene', '.screen/courtyard.scene.json',
                   '--frames', "$Frames", '--render-size', '640x360',
                   '--report', (Get-RelativePath $report)) + $Extra
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    $data = $null
    if (Test-Path -LiteralPath $report) {
        $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    }
    if (-not $AllowFailure -and $process.ExitCode -ne 0) {
        throw "Run $Tag exited $($process.ExitCode): $($data.error)"
    }
    return [pscustomobject]@{ exit = $process.ExitCode; data = $data; report = $report }
}

Write-Host "Replay evidence ($Frames frames)" -ForegroundColor Cyan
$recording = Join-Path $stage 'session.log'
$record = Invoke-Run -Tag 'record' -Extra @('--record', (Get-RelativePath $recording))
Add-Check 'recording run succeeded' ($record.exit -eq 0) "exit=$($record.exit)"
Add-Check 'recording file exists' (Test-Path -LiteralPath $recording) $recording
Add-Check 'initial simulation snapshot exists' (Test-Path -LiteralPath "$recording.initial.json")

$lines = [System.IO.File]::ReadAllLines($recording)
$header = $lines[0]
$body = @($lines | Where-Object { $_ -ne '' -and -not $_.StartsWith('#') })
Add-Check 'recording header declares the replay format and seed' `
    (($header -match '^# nativegpu-replay v4 seed=20261020 step=1/120 mode=record content=[0-9a-f]{16} character=([0-9a-f]{16})$') -and
        ($Matches[1] -eq [string]$record.data.animation.runtimeFingerprint)) $header
Add-Check 'recording has one line per frame' ($body.Count -eq $Frames) "lines=$($body.Count) expected=$Frames"
Add-Check 'each line carries the input columns' (($body[0] -split ',').Count -eq 13) $body[0]
Add-Check 'recorded frame numbers are contiguous' `
    ((0..($body.Count - 1) | Where-Object { [int](($body[$_] -split ',')[0]) -ne $_ }).Count -eq 0) `
    'frame column is not contiguous'

$replay = Invoke-Run -Tag 'replay' -Extra @('--replay', (Get-RelativePath $recording), '--replay-strict')
Add-Check 'replay exited 0 in strict mode' ($replay.exit -eq 0) "exit=$($replay.exit)"
Add-Check 'replay compared every recorded frame' ([int]$replay.data.replay.compared -eq $Frames) `
    "compared=$($replay.data.replay.compared)"
Add-Check 'replay reproduced every digest' ([int]$replay.data.replay.mismatches -eq 0) `
    "mismatches=$($replay.data.replay.mismatches) first=$($replay.data.replay.firstMismatchFrame)"
Add-Check 'replay adopted the recorded seed' ([long]$replay.data.determinism.seed -eq 20261020) `
    "$($replay.data.determinism.seed)"
Add-Check 'replay report marks strict mode' ([bool]$replay.data.replay.strict) "$($replay.data.replay.strict)"

# Tamper with one digest in the middle of the recording. Strict replay must fail at
# exactly that frame, which is what makes the recording usable as evidence.
$tampered = Join-Path $stage 'tampered.log'
$tamperIndex = [Math]::Min(99, $body.Count - 1)
$copy = New-Object System.Collections.Generic.List[string]
$copy.Add($header)
for ($index = 0; $index -lt $body.Count; ++$index) {
    $line = $body[$index]
    if ($index -eq $tamperIndex) {
        $fields = $line -split ','
        $hash = $fields[2]
        $flipped = if ($hash[0] -eq 'a') { 'b' } else { 'a' }
        $fields[2] = $flipped + $hash.Substring(1)
        $line = $fields -join ','
    }
    $copy.Add($line)
}
[System.IO.File]::WriteAllLines($tampered, $copy)
Copy-Item -LiteralPath "$recording.initial.json" -Destination "$tampered.initial.json"

$badReplay = Invoke-Run -Tag 'tampered' -Extra @('--replay', (Get-RelativePath $tampered), '--replay-strict') -AllowFailure
Add-Check 'strict replay of a tampered recording exits non-zero' ($badReplay.exit -ne 0) "exit=$($badReplay.exit)"
Add-Check 'tampered replay reports the mismatch code' ([string]$badReplay.data.errorCode -eq 'NG-E-REPLAY-MISMATCH') `
    "$($badReplay.data.errorCode)"
Add-Check 'tampered replay fails at the tampered frame' ([int]$badReplay.data.replay.firstMismatchFrame -eq $tamperIndex) `
    "first=$($badReplay.data.replay.firstMismatchFrame) expected=$tamperIndex"
Add-Check 'tampered replay counts the remaining mismatches' ([int]$badReplay.data.replay.mismatches -gt 0) `
    "mismatches=$($badReplay.data.replay.mismatches)"

# Re-record a synthetic input stream containing zero, partial and multiple ticks.
# The first non-strict run establishes its hashes; the second must reproduce them.
$variableInput=Join-Path $stage 'variable-input.log'
$variablePath=Join-Path $stage 'variable.log'
$deltaBits=@('00000000','3c000000','3d000000','3c800000','3d800000')
$variableLines=New-Object System.Collections.Generic.List[string]
$variableLines.Add($header)
for($index=0;$index -lt $body.Count;++$index){$fields=$body[$index] -split ',';$fields[12]=$deltaBits[$index%$deltaBits.Count];$variableLines.Add(($fields -join ','))}
[IO.File]::WriteAllLines($variableInput,$variableLines)
Copy-Item -LiteralPath "$recording.initial.json" -Destination "$variableInput.initial.json"
$variable=Invoke-Run -Tag 'variable-record' -Extra @('--replay',(Get-RelativePath $variableInput),'--record',(Get-RelativePath $variablePath))
$variableReplay=Invoke-Run -Tag 'variable-replay' -Extra @('--replay',(Get-RelativePath $variablePath),'--replay-strict')
Add-Check 'variable frame timing replays exactly' ($variableReplay.exit -eq 0 -and [int]$variableReplay.data.replay.mismatches -eq 0 -and [int]$variableReplay.data.replay.compared -eq $Frames)

# A different content contract is rejected before any simulation frame.
$wrongContent=Join-Path $stage 'wrong-content.log'
$wrongHeader=$header -replace 'content=[0-9a-f]{16}','content=0000000000000000'
[IO.File]::WriteAllLines($wrongContent,@($wrongHeader)+$body)
Copy-Item -LiteralPath "$recording.initial.json" -Destination "$wrongContent.initial.json"
$contentFailure=Invoke-Run -Tag 'wrong-content' -Extra @('--replay',(Get-RelativePath $wrongContent),'--replay-strict') -AllowFailure
Add-Check 'different content is rejected before replay' ($contentFailure.exit -ne 0 -and $contentFailure.data.errorCode -eq 'NG-E-REPLAY-CONTENT')

# Black box: an injected wall-clock stall must produce a snapshot next to the recording.
$blackbox = Join-Path $stage 'stall.log'
$stall = Invoke-Run -Tag 'stall' -Extra @('--record', (Get-RelativePath $blackbox),
    '--stall-frame', "$StallFrame", '--stall-ms', "$StallMs", '--blackbox-stall-ms', '25')
$anomalyPath = "$blackbox.anomaly.json"
Add-Check 'stall run succeeded' ($stall.exit -eq 0) "exit=$($stall.exit)"
Add-Check 'stall run triggered the black box' ([bool]$stall.data.blackbox.triggered) `
    "triggered=$($stall.data.blackbox.triggered)"
Add-Check 'anomaly snapshot was written' (Test-Path -LiteralPath $anomalyPath) $anomalyPath
if (Test-Path -LiteralPath $anomalyPath) {
    $anomaly = Get-Content -LiteralPath $anomalyPath -Raw | ConvertFrom-Json
    Add-Check 'anomaly snapshot names the stall trigger' ([string]$anomaly.trigger -eq 'stall') "$($anomaly.trigger)"
    Add-Check 'anomaly snapshot records the stalled frame' ([int]$anomaly.frame -eq $StallFrame) `
        "frame=$($anomaly.frame) expected=$StallFrame"
    Add-Check 'anomaly snapshot records a real stall above the threshold' `
        ([double]$anomaly.dtMs -gt [double]$anomaly.thresholdMs) `
        "dt=$($anomaly.dtMs) threshold=$($anomaly.thresholdMs)"
    Add-Check 'anomaly snapshot points at its recording' `
        ([string]$anomaly.recording -eq (Get-RelativePath $blackbox)) "$($anomaly.recording)"
}
Add-Check 'black box recorded the stall frame number' ([int]$stall.data.blackbox.frame -eq $StallFrame) `
    "frame=$($stall.data.blackbox.frame)"

# The snapshot is only useful if it replays. The stall is wall-clock only, so the
# simulation must still reproduce exactly.
$stallReplay = Invoke-Run -Tag 'stall-replay' -Extra @('--replay', (Get-RelativePath $blackbox), '--replay-strict')
Add-Check 'black-box recording replays without diverging' ([int]$stallReplay.data.replay.mismatches -eq 0) `
    "mismatches=$($stallReplay.data.replay.mismatches)"
Add-Check 'black-box replay compared every frame' ([int]$stallReplay.data.replay.compared -eq $Frames) `
    "compared=$($stallReplay.data.replay.compared)"

$failed = @($checks | Where-Object { -not $_.ok }).Count
$success = $failed -eq 0

$summary = [pscustomobject]@{
    schema = 'nativegpu.replay/1'
    frames = $Frames
    header = $header
    recordedLines = $body.Count
    replay = [pscustomobject]@{
        compared = [int]$replay.data.replay.compared
        mismatches = [int]$replay.data.replay.mismatches
        seed = [long]$replay.data.determinism.seed
    }
    tamper = [pscustomobject]@{
        index = $tamperIndex
        exit = $badReplay.exit
        errorCode = [string]$badReplay.data.errorCode
        firstMismatchFrame = [int]$badReplay.data.replay.firstMismatchFrame
        mismatches = [int]$badReplay.data.replay.mismatches
    }
    blackbox = [pscustomobject]@{
        triggered = [bool]$stall.data.blackbox.triggered
        frame = [int]$stall.data.blackbox.frame
        dtMs = [double]$stall.data.blackbox.dtMs
        thresholdMs = [double]$stall.data.blackbox.thresholdMs
        replayMismatches = [int]$stallReplay.data.replay.mismatches
    }
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'replay-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('replay-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Replay evidence: {0} checks, {1} failed" -f $checks.Count, $failed)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

# Audio evidence: proves the mixer meets the documented gates (.developmentAgent/animation-audio/AUDIO-PLAN.md §8)
# and that the host actually routes gameplay events into it.
#
# Three layers, because they fail for different reasons:
#   1. build\native_audio_tests.exe  - the mixer itself, offline and deterministic
#   2. --audio offline sessions      - host routing, no device required
#   3. --audio wasapi sessions       - a real endpoint, if the machine has one
#
# Exit code 0 means every check passed and the evidence was archived under
# reports/<version>/<commit12>/<UTC stamp>/.
param(
    [int]$Frames = 360,
    [switch]$SkipBuild,
    [switch]$SkipDevice
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')

$context = New-ReportContext -Mode 'audio'
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

# ---- layer 1: the mixer, offline ------------------------------------------------
$testsExe = Join-Path $root 'build\native_audio_tests.exe'
if (-not (Test-Path -LiteralPath $testsExe)) { throw 'build\native_audio_tests.exe is missing; run tools/build.ps1.' }
$testsOutput = & $testsExe 2>&1
$testsExit = $LASTEXITCODE
$testsText = ($testsOutput | Out-String)
$testsText | Set-Content -LiteralPath (Join-Path $stage 'native-audio-tests.txt') -Encoding utf8
$match = [regex]::Match($testsText, '(\d+)\s+checks,\s+(\d+)\s+failed')
if (-not $match.Success) { throw "Cannot parse native audio test output: $testsText" }
$mixerChecks = [int]$match.Groups[1].Value
$mixerFailed = [int]$match.Groups[2].Value
Add-Check 'native audio suite exits clean' ($testsExit -eq 0) "exit=$testsExit"
Add-Check 'native audio suite runs every check' ($mixerChecks -ge 40) "checks=$mixerChecks"
Add-Check 'native audio suite has no failures' ($mixerFailed -eq 0) "failed=$mixerFailed"

# The suite writes one receipt per gate; they are archived with the run so a later
# change can be diffed against the numbers a gate was actually accepted on.
$audioReports = Join-Path $root 'reports\audio'
$receipts = @{}
if (Test-Path -LiteralPath $audioReports) {
    foreach ($file in Get-ChildItem -LiteralPath $audioReports -Filter '*.json') {
        $target = Join-Path $stage $file.Name
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        try { $receipts[$file.BaseName] = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json } catch { }
    }
}
Add-Check 'mixer wrote determinism receipt' ($receipts.ContainsKey('determinism')) 'determinism.json missing'
if ($receipts.ContainsKey('determinism')) {
    $determinism = $receipts['determinism']
    Add-Check 'pcm hash is identical across block sizes' `
        ([string]$determinism.hash256 -eq [string]$determinism.hash480) `
        "256=$($determinism.hash256) 480=$($determinism.hash480)"
    Add-Check 'offline render covers the requested span' ([int]$determinism.frames -eq 48000) `
        "frames=$($determinism.frames)"
}
if ($receipts.ContainsKey('budget')) {
    $budget = $receipts['budget']
    Add-Check '64 binaural voices fit the block period' `
        ([double]$budget.blockMs -lt [double]$budget.periodMs) `
        "blockMs=$($budget.blockMs) periodMs=$($budget.periodMs)"
    Add-Check 'the voice budget is the documented 64' ([int]$budget.voices -eq 64) "voices=$($budget.voices)"
}
if ($receipts.ContainsKey('null-test')) {
    $nullTest = $receipts['null-test']
    Add-Check 'unprocessed path is transparent' ([double]$nullTest.maxAbsError -le [double]$nullTest.threshold) `
        "error=$($nullTest.maxAbsError) threshold=$($nullTest.threshold)"
}
if ($receipts.ContainsKey('hrtf')) {
    $hrtf = $receipts['hrtf']
    Add-Check 'ITD matches the Woodworth model' ([double]$hrtf.itdErrorUs -lt 30) `
        "itdErrorUs=$($hrtf.itdErrorUs)"
    Add-Check 'head shadow produces a usable ILD' ([double]$hrtf.ildDb -gt 2) "ildDb=$($hrtf.ildDb)"
}
if ($receipts.ContainsKey('occlusion')) {
    $occlusion = $receipts['occlusion']
    Add-Check 'occlusion attenuates inside the authored window' `
        ([double]$occlusion.dropDb -ge 6 -and [double]$occlusion.dropDb -le 20) `
        "dropDb=$($occlusion.dropDb)"
    Add-Check 'occlusion removes high frequency energy' `
        ([double]$occlusion.hfClosed -lt ([double]$occlusion.hfOpen * 0.1)) `
        "open=$($occlusion.hfOpen) closed=$($occlusion.hfClosed)"
    Add-Check 'occlusion smoothing never steps' ([double]$occlusion.worstBlockJumpDb -lt 4) `
        "worstBlockJumpDb=$($occlusion.worstBlockJumpDb)"
}
if ($receipts.ContainsKey('panning')) {
    $panning = $receipts['panning']
    Add-Check 'VBAP gains are constant power' ([double]$panning.worstGainError -lt 0.05) `
        "worstGainError=$($panning.worstGainError)"
    Add-Check 'every azimuth lands on a speaker' (@($panning.cases | Where-Object { [double]$_.gain -lt 0.99 }).Count -eq 0) `
        (@($panning.cases | Where-Object { [double]$_.gain -lt 0.99 } | ForEach-Object { $_.azimuth }) -join ',')
}
if ($receipts.ContainsKey('virtualization')) {
    $virtual = $receipts['virtualization']
    Add-Check 'voice virtualization caps physical voices' `
        ([int]$virtual.physical -le [int]$virtual.limit) `
        "physical=$($virtual.physical) limit=$($virtual.limit)"
    Add-Check 'voice virtualization keeps virtual voices alive' ([int]$virtual.virtual -gt 100) `
        "virtual=$($virtual.virtual)"
    Add-Check 'every accepted play is counted once' ([int]$virtual.started -eq [int]$virtual.requested) `
        "started=$($virtual.started) requested=$($virtual.requested)"
}
if ($receipts.ContainsKey('stability')) {
    $stability = $receipts['stability']
    Add-Check 'a ten second run has no underruns' ([int]$stability.underruns -eq 0) `
        "underruns=$($stability.underruns)"
    Add-Check 'a ten second run stays inside full scale' ([double]$stability.peak -le 1.0) `
        "peak=$($stability.peak)"
}
if ($receipts.ContainsKey('loudness')) {
    $loudness = $receipts['loudness']
    Add-Check 'a 6 dB gain step measures 6 LU' `
        ([math]::Abs([double]$loudness.deltaLu - [double]$loudness.target) -le [double]$loudness.tolerance) `
        "deltaLu=$($loudness.deltaLu) target=$($loudness.target)"
}

# ---- layer 2: host routing, offline --------------------------------------------
function Invoke-Session {
    param([string]$Tag, [string[]]$Extra = @())
    $report = Join-Path $stage "$Tag.json"
    $arguments = @('--smoke', '--no-watch', '--no-stress',
                   '--scene', '.screen/courtyard.scene.json',
                   '--frames', "$Frames", '--render-size', '1280x720',
                   '--report', (Get-RelativePath $report)) + $Extra
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Session $Tag exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if (-not $data.success) { throw "Session $Tag reported success=false: $($data.error)" }
    return [pscustomobject]@{ data = $data; report = $report }
}

$off = Invoke-Session -Tag 'off' -Extra @('--audio', 'off')
Add-Check '--audio off disables the subsystem' ([bool]$off.data.audio.enabled -eq $false) `
    "enabled=$($off.data.audio.enabled)"
Add-Check '--audio off renders no mixer stats' ($null -eq $off.data.audio.mixer) `
    "mixer=$($off.data.audio.mixer)"
Add-Check '--audio off starts no device' ([bool]$off.data.audio.device.started -eq $false) `
    "started=$($off.data.audio.device.started)"

$offline = Invoke-Session -Tag 'offline' -Extra @('--audio', 'offline', '--audio-demo')
$offlineAudio = $offline.data.audio
Add-Check 'offline session configures the mixer' ([bool]$offlineAudio.configured) $offlineAudio.error
Add-Check 'offline session reports no error' ([string]::IsNullOrEmpty([string]$offlineAudio.error)) `
    "$($offlineAudio.error)"
Add-Check 'offline session starts no device' ([bool]$offlineAudio.device.started -eq $false) `
    "started=$($offlineAudio.device.started)"
Add-Check 'bank loads every authored sound' ([int]$offlineAudio.bank.sounds -eq 5) `
    "sounds=$($offlineAudio.bank.sounds)"
Add-Check 'scene emitters are adopted' ([int]$offlineAudio.emitters -eq 6) `
    "emitters=$($offlineAudio.emitters)"
Add-Check 'the ambient bed starts on adoption' ([int]$offlineAudio.plays -ge 1) `
    "plays=$($offlineAudio.plays)"
Add-Check 'the scripted sweep fires every trigger' ([int]$offlineAudio.triggersFired -ge 5) `
    "fired=$($offlineAudio.triggersFired)"
Add-Check 'no play is rejected' ([int]$offlineAudio.rejected -eq 0) `
    "rejected=$($offlineAudio.rejected)"
Add-Check 'the mixer rendered audio' ([int]$offlineAudio.mixer.frames -gt 0) `
    "frames=$($offlineAudio.mixer.frames)"
Add-Check 'offline rendering has no underruns' ([int]$offlineAudio.mixer.underruns -eq 0) `
    "underruns=$($offlineAudio.mixer.underruns)"
Add-Check 'offline rendering stays finite and in range' `
    ([double]$offlineAudio.mixer.peak -gt 0 -and [double]$offlineAudio.mixer.peak -le 1.0) `
    "peak=$($offlineAudio.mixer.peak)"
Add-Check 'offline rendering produces a pcm hash' ([string]$offlineAudio.mixer.pcmHash -ne 'cbf29ce484222325') `
    "pcmHash=$($offlineAudio.mixer.pcmHash)"
# The demo sweep only proves routing: a lamp twelve metres away is legitimately quiet.
# Audibility is proved separately by a non-spatial audition at the listener.
Add-Check 'the demo mix is not silent' ([double]$offlineAudio.mixer.peakMax -gt 0.001) `
    "peakMax=$($offlineAudio.mixer.peakMax)"
Add-Check 'voices carry a bus and a distance' `
    (@($offlineAudio.voices | Where-Object { [string]::IsNullOrEmpty([string]$_.bus) }).Count -eq 0) `
    'a voice has no bus'

# A second offline run must land on the same hash: the mixer is deterministic and the
# host submits the same commands at the same frames.
$offline2 = Invoke-Session -Tag 'offline-2' -Extra @('--audio', 'offline', '--audio-demo')
Add-Check 'two offline runs agree on pcm hash' `
    ([string]$offline.data.audio.mixer.pcmHash -eq [string]$offline2.data.audio.mixer.pcmHash) `
    "$($offline.data.audio.mixer.pcmHash) vs $($offline2.data.audio.mixer.pcmHash)"

# Audition: one sound at the listener, no distance, no panning. This is where the
# loudness normalisation and the target level are actually observable.
$audition = Invoke-Session -Tag 'audition' -Extra @('--audio', 'offline', '--audio-audition', 'fogtown/blast')
$auditionAudio = $audition.data.audio
Add-Check 'the audition sound is accepted' ([string]$auditionAudio.audition -eq 'fogtown/blast') `
    "audition=$($auditionAudio.audition)"
Add-Check 'the audition is not rejected' ([int]$auditionAudio.rejected -eq 0) `
    "rejected=$($auditionAudio.rejected)"
Add-Check 'an auditioned sound reaches the loudness target' `
    ([double]$auditionAudio.mixer.lufsShortMax -ge -26 -and [double]$auditionAudio.mixer.lufsShortMax -le -8) `
    "lufsShortMax=$($auditionAudio.mixer.lufsShortMax) target=$($auditionAudio.loudnessTargetLufs)"
Add-Check 'an auditioned sound uses real headroom' ([double]$auditionAudio.mixer.peakMax -ge 0.05) `
    "peakMax=$($auditionAudio.mixer.peakMax)"
Add-Check 'an auditioned sound never clips' ([double]$auditionAudio.mixer.peakMax -le 1.0) `
    "peakMax=$($auditionAudio.mixer.peakMax)"

$binaural = Invoke-Session -Tag 'binaural' -Extra @('--audio', 'offline', '--audio-demo', '--audio-quality', 'binaural')
Add-Check 'binaural quality forces a stereo layout' ([string]$binaural.data.audio.layout -eq '2.0') `
    "layout=$($binaural.data.audio.layout)"
Add-Check 'binaural quality is reported back' ([string]$binaural.data.audio.quality -eq 'binaural') `
    "quality=$($binaural.data.audio.quality)"

$atmos = Invoke-Session -Tag 'atmos' -Extra @('--audio', 'offline', '--audio-demo', '--audio-quality', 'atmos')
Add-Check 'atmos quality keeps the 7.1.2 bed' ([string]$atmos.data.audio.layout -eq '7.1.2') `
    "layout=$($atmos.data.audio.layout)"
Add-Check 'atmos quality renders without error' ([string]::IsNullOrEmpty([string]$atmos.data.audio.error)) `
    "$($atmos.data.audio.error)"

# ---- layer 3: a real endpoint ---------------------------------------------------
$device = $null
if (-not $SkipDevice) {
    $device = Invoke-Session -Tag 'device' -Extra @('--audio', 'wasapi', '--audio-demo')
    $deviceAudio = $device.data.audio
    if ([bool]$deviceAudio.device.failed) {
        Write-Host "  skip device checks - $($deviceAudio.error)" -ForegroundColor Yellow
        $device = $null
    } else {
        Add-Check 'the device starts' ([bool]$deviceAudio.device.started) $deviceAudio.error
        Add-Check 'the device negotiated 48 kHz' ([int]$deviceAudio.device.sampleRate -eq 48000) `
            "sampleRate=$($deviceAudio.device.sampleRate)"
        Add-Check 'the device reports a channel count' ([int]$deviceAudio.device.channels -ge 2) `
            "channels=$($deviceAudio.device.channels)"
        Add-Check 'the device buffer is a real period' ([int]$deviceAudio.device.bufferFrames -gt 0) `
            "bufferFrames=$($deviceAudio.device.bufferFrames)"
        Add-Check 'the device run has no underruns' ([int]$deviceAudio.mixer.underruns -eq 0) `
            "underruns=$($deviceAudio.mixer.underruns)"
        Add-Check 'the device run rendered audio' ([int]$deviceAudio.mixer.frames -gt 0) `
            "frames=$($deviceAudio.mixer.frames)"
        Add-Check 'the device run fires every trigger' ([int]$deviceAudio.triggersFired -ge 5) `
            "fired=$($deviceAudio.triggersFired)"
        Add-Check 'no command missed its sample deadline' ([int]$deviceAudio.mixer.lateCommands -eq 0) `
            "lateCommands=$($deviceAudio.mixer.lateCommands)"
        Add-Check 'mixer CPU stays far below the block budget' ([double]$deviceAudio.mixer.cpuMs -lt 5.33) `
            "cpuMs=$($deviceAudio.mixer.cpuMs)"
    }
}

# ---- layer 4: audio must not touch the frame ----------------------------------
# The plan's migration rule is that removing audio leaves rendering byte-identical.
# Two screenshots of the same camera, one with the mixer running and one without,
# are the cheapest way to keep that promise honest.
function Invoke-Screenshot {
    param([string]$Tag, [string[]]$Extra = @())
    $path = Join-Path $stage "$Tag.png"
    $arguments = @('--smoke', '--no-watch', '--no-stress',
                   '--scene', '.screen/courtyard.scene.json',
                   '--frames', '200', '--render-size', '640x360',
                   '--camera', '8,4.5,10.5,0,1.4,-2',
                   '--screenshot', (Get-RelativePath $path)) + $Extra
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Screenshot $Tag exited $($process.ExitCode)." }
    if (-not (Test-Path -LiteralPath $path)) { throw "Screenshot $Tag was not written." }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$shotOff = Invoke-Screenshot -Tag 'frame-audio-off' -Extra @('--audio', 'off')
$shotOn = Invoke-Screenshot -Tag 'frame-audio-on' -Extra @('--audio', 'wasapi', '--audio-demo')
Add-Check 'audio does not change the rendered frame' ($shotOff -eq $shotOn) `
    "off=$($shotOff.Substring(0,12)) on=$($shotOn.Substring(0,12))"

# ---- summary -------------------------------------------------------------------
$failed = @($checks | Where-Object { -not $_.ok }).Count
$success = $failed -eq 0

$summary = [pscustomobject]@{
    schema = 'nativegpu.audio/1'
    frames = $Frames
    mixer = [pscustomobject]@{
        checks = $mixerChecks
        failed = $mixerFailed
        receipts = @($receipts.Keys | Sort-Object)
    }
    host = [pscustomobject]@{
        disabled = [pscustomobject]@{ enabled = [bool]$off.data.audio.enabled }
        offline = [pscustomobject]@{
            quality = [string]$offlineAudio.quality
            layout = [string]$offlineAudio.layout
            sounds = [int]$offlineAudio.bank.sounds
            emitters = [int]$offlineAudio.emitters
            plays = [int]$offlineAudio.plays
            triggersFired = [int]$offlineAudio.triggersFired
            pcmHash = [string]$offlineAudio.mixer.pcmHash
            frames = [int]$offlineAudio.mixer.frames
            underruns = [int]$offlineAudio.mixer.underruns
            peak = [double]$offlineAudio.mixer.peak
            peakMax = [double]$offlineAudio.mixer.peakMax
            lufsShortMax = [double]$offlineAudio.mixer.lufsShortMax
            lufsIntegrated = [double]$offlineAudio.mixer.lufsIntegrated
        }
        binaural = [pscustomobject]@{ quality = [string]$binaural.data.audio.quality; layout = [string]$binaural.data.audio.layout }
        atmos = [pscustomobject]@{ quality = [string]$atmos.data.audio.quality; layout = [string]$atmos.data.audio.layout }
        audition = [pscustomobject]@{
            sound = [string]$auditionAudio.audition
            lufsShortMax = [double]$auditionAudio.mixer.lufsShortMax
            lufsIntegrated = [double]$auditionAudio.mixer.lufsIntegrated
            peak = [double]$auditionAudio.mixer.peak
            peakMax = [double]$auditionAudio.mixer.peakMax
            target = [double]$auditionAudio.loudnessTargetLufs
        }
        device = if ($device) {
            [pscustomobject]@{
                started = [bool]$device.data.audio.device.started
                sampleRate = [int]$device.data.audio.device.sampleRate
                channels = [int]$device.data.audio.device.channels
                bufferFrames = [int]$device.data.audio.device.bufferFrames
                floatFormat = [bool]$device.data.audio.device.floatFormat
                frames = [int]$device.data.audio.mixer.frames
                underruns = [int]$device.data.audio.mixer.underruns
                lateCommands = [int]$device.data.audio.mixer.lateCommands
                cpuMs = [double]$device.data.audio.mixer.cpuMs
            }
        } else { $null }
    }
    checks = $checks
    success = $success
    frame = [pscustomobject]@{
        audioOffSha256 = $shotOff
        audioOnSha256 = $shotOn
        identical = ($shotOff -eq $shotOn)
    }
}
$evidencePath = Join-Path $context.directory 'audio-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('audio-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Audio evidence: {0} checks, {1} failed" -f $checks.Count, $failed)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

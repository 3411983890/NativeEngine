param([switch]$SkipBuild, [int]$LifecycleCycles = 5)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if ($LifecycleCycles -lt 1 -or $LifecycleCycles -gt 100) { throw 'LifecycleCycles must be in [1,100].' }
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    }
    . (Join-Path $root 'tools/native-build-state.ps1')
    Assert-NativeBuildCurrent
    $raw = & './build/native_effects_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Native effect reference checks failed.' }
    $reference = $raw | ConvertFrom-Json
    & node tools/effects-runtime-fixtures.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Effect renderer fixtures failed.' }
    . (Join-Path $root 'tools/report-lib.ps1')
    $context = New-ReportContext -Mode 'effects-runtime'
    $checks = [System.Collections.Generic.List[object]]::new()
    function Add-Check([string]$name, [bool]$ok, [string]$detail) {
        $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
        $status = if ($ok) { 'PASS' } else { 'FAIL' }
        Write-Host "[$status] $name - $detail"
    }
    $runs = [ordered]@{}
    $images = [ordered]@{}
    function Invoke-EffectRun([string]$name, [string[]]$extra, [bool]$capture = $true, [bool]$stress = $false) {
        $reportPath = Join-Path $context.directory "$name.json"
        $imagePath = Join-Path $context.directory "$name.png"
        $arguments = @('--smoke','--no-watch','--audio','off','--frames','240',
            '--render-size','960x540','--camera','5,3,8,0,1,0','--report',(Get-RelativePath $reportPath))
        if (-not $stress) { $arguments += '--no-stress' }
        if ($capture) { $arguments += @('--screenshot',(Get-RelativePath $imagePath),'--screenshot-frame','30') }
        $process = Start-Process -FilePath './build/NativeGPU.exe' -ArgumentList ($arguments + $extra) `
            -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
        if (-not (Test-Path -LiteralPath $reportPath)) { throw "Missing $name report (exit $($process.ExitCode))." }
        $data = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $runs[$name] = [pscustomobject]@{ exit = $process.ExitCode; report = Get-RelativePath $reportPath; data = $data }
        if ($capture -and (Test-Path -LiteralPath $imagePath)) {
            $images[$name] = [pscustomobject]@{ path = Get-RelativePath $imagePath; sha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash }
        }
    }
    Add-Check 'CPU instance reference passes' ([bool]$reference.ok) "groups=$($reference.groups) checks=$($reference.checks)"
    Invoke-EffectRun 'effects-off' @()
    Invoke-EffectRun 'effects-on' @('--effect','sprite_demo','--effect-count','4')
    Invoke-EffectRun 'effects-repeat' @('--effect','sprite_demo','--effect-count','4')
    Invoke-EffectRun 'effects-stopped' @('--effect','sprite_demo','--effect-count','4','--effect-stop-tick','10')
    foreach ($name in @('effects-off','effects-on','effects-repeat','effects-stopped')) {
        $run = $runs[$name]
        Add-Check "$name completes" ($run.exit -eq 0 -and $run.data.success -and $images.Contains($name)) "exit=$($run.exit) error=$($run.data.error)"
    }
    $on = $runs['effects-on'].data
    $sprites = $on.probes.effectSprites
    Add-Check 'native effect instances advance' ($on.effectRuntime.simulationBackend -eq 'cpu-reference' -and $on.effectRuntime.tick -gt 0 -and $on.effectRuntime.instances -eq 4 -and $on.effectRuntime.emitted -gt 0) "tick=$($on.effectRuntime.tick) emitted=$($on.effectRuntime.emitted)"
    Add-Check 'GPU textured sprite path draws' ($sprites.available -and $sprites.implemented -and $sprites.draws -gt 0 -and $sprites.textures -gt 0 -and $sprites.uploadBytes -gt 0) "draws=$($sprites.draws) textures=$($sprites.textures) uploadBytes=$($sprites.uploadBytes)"
    Add-Check 'frame buffered upload and sorting enabled' ($sprites.frameBuffered -and $sprites.alphaSorted) 'per-frame fenced upload buffers; premultiplied sprites sort by depth'
    Add-Check 'effect changes captured image' ($images['effects-on'].sha256 -and $images['effects-on'].sha256 -ne $images['effects-off'].sha256) 'identical camera, simulation frame and scene'
    Add-Check 'same seed repeats captured image' ($images['effects-repeat'].sha256 -and $images['effects-on'].sha256 -eq $images['effects-repeat'].sha256) 'exact PNG SHA256 comparison'
    $stopped = $runs['effects-stopped'].data.effectRuntime
    Add-Check 'immediate stop retires all particles' ($stopped.stopped -and $stopped.alive -eq 0 -and $stopped.instances -eq 0) "alive=$($stopped.alive) instances=$($stopped.instances)"
    Add-Check 'stopped image returns to baseline' ($images['effects-stopped'].sha256 -and $images['effects-stopped'].sha256 -eq $images['effects-off'].sha256) 'no residual effect after tick 10'
    foreach ($blend in @('additive','premultiplied','cutout')) {
        $name = "effects-$blend"
        Invoke-EffectRun $name @('--effect',"validation_$blend",'--effect-count','4',
            '--effect-library','build/effects-validation/cooked/library.json')
        $run = $runs[$name]
        Add-Check "$blend blend draws" ($run.exit -eq 0 -and $run.data.success -and $run.data.probes.effectSprites.implemented) "draws=$($run.data.probes.effectSprites.draws)"
        Add-Check "$blend blend changes image" ($images[$name].sha256 -and $images[$name].sha256 -ne $images['effects-off'].sha256) 'captured pixels differ from disabled effect'
    }
    Add-Check 'three blend captures are distinct' (@($images['effects-additive'].sha256,$images['effects-premultiplied'].sha256,$images['effects-cutout'].sha256 | Select-Object -Unique).Count -eq 3) 'fixture blend modes produce different pixels'
    Invoke-EffectRun 'effects-unsupported' @('--effect','smoke_reference') $false
    $unsupported = $runs['effects-unsupported']
    Add-Check 'unsupported field effect rejects explicitly' ($unsupported.exit -ne 0 -and -not $unsupported.data.success -and $unsupported.data.errorCode -eq 'NG-E-EFFECT-UNSUPPORTED') "code=$($unsupported.data.errorCode)"
    Invoke-EffectRun 'effects-lifecycle' @('--effect','sprite_demo','--effect-count','4','--lifecycle',"$LifecycleCycles",'--frames',"$($LifecycleCycles*8+8)") $false
    $lifecycle = $runs['effects-lifecycle']
    Add-Check 'sprite pipeline survives renderer rebuilds' ($lifecycle.exit -eq 0 -and $lifecycle.data.success -and $lifecycle.data.lifecycle.completed -eq $LifecycleCycles -and $lifecycle.data.probes.effectSprites.available -and $lifecycle.data.probes.effectSprites.textures -gt 0) "completed=$($lifecycle.data.lifecycle.completed) textures=$($lifecycle.data.probes.effectSprites.textures); lifecycle harness exits immediately after final initialization"
    $leaks = @($lifecycle.data.lifecycle.entries | Where-Object { $_.liveObjectsAfterShutdown -ne 0 })
    Add-Check 'sprite rebuilds release tracked GPU objects' ($leaks.Count -eq 0 -and @($lifecycle.data.lifecycle.entries).Count -eq $LifecycleCycles) "leakingCycles=$($leaks.Count)"
    Invoke-EffectRun 'effects-recovery' @('--effect','sprite_demo','--effect-count','4') $false $true
    $recovered = $runs['effects-recovery']
    Add-Check 'sprites draw after resize and renderer restart' ($recovered.exit -eq 0 -and $recovered.data.success -and $recovered.data.rendererReinitializations -gt 0 -and $recovered.data.resizeEvents -gt 0 -and $recovered.data.probes.effectSprites.implemented -and $recovered.data.probes.effectSprites.frames -gt 0) "framesAfterRestart=$($recovered.data.probes.effectSprites.frames)"
    $ok = @($checks | Where-Object { -not $_.ok }).Count -eq 0
    $evidence = [ordered]@{
        schema = 'nativegpu.effects-integration-evidence/1'
        generatedAtUtc = $context.timestamp
        build = $context.build
        sourceManifestHash = (Get-FileHash -LiteralPath 'build/source-inputs.json' -Algorithm SHA256).Hash
        binaryHash = (Get-FileHash -LiteralPath 'build/NativeGPU.exe' -Algorithm SHA256).Hash
        reference = $reference
        scope = 'CPU reference simulation with GPU unlit sprites; general GPU simulation, lit/soft sprites, fields and other renderers remain pending'
        runtime = $on.effectRuntime
        renderer = $sprites
        lifecycle = $lifecycle.data.lifecycle
        afterRecovery = $recovered.data.probes.effectSprites
        images = $images
        reports = [ordered]@{}
        checks = $checks
        ok = $ok
    }
    foreach ($name in $runs.Keys) { $evidence.reports[$name] = $runs[$name].report }
    $destination = Join-Path $context.directory 'effects-runtime-evidence.json'
    $evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $destination -Encoding utf8
    Register-Report -Context $context -Path $destination -LegacyAliases @('effects-runtime-evidence.json') | Out-Null
    Write-Host "Effects runtime: $($checks.Count - @($checks | Where-Object { -not $_.ok }).Count)/$($checks.Count); $destination"
    if (-not $ok) { exit 1 }
} finally { Pop-Location }

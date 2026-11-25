param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
$verificationLock = $null
try {
. (Join-Path $root 'tools/report-lib.ps1')
$context = New-ReportContext -Mode 'verify'
$steps = [System.Collections.Generic.List[object]]::new()
$overall = $true
$nativeProvenance = $null
$verificationScriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash

function Invoke-Step {
    param([string]$Name, [scriptblock]$Action, [switch]$AllowedToFail)
    Write-Host ""
    Write-Host "== $Name" -ForegroundColor Cyan
    $started = Get-Date
    $ok = $true
    $message = ''
    try {
        & $Action
    } catch {
        $ok = $false
        $message = $_.Exception.Message
        if (-not $AllowedToFail) { $script:overall = $false }
        Write-Host "FAILED: $message" -ForegroundColor Red
    }
    $steps.Add([pscustomobject]@{
        name = $Name
        ok = $ok
        durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        error = $message
    })
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw 'Node.js 22 or newer is required on PATH.' }
$nodeVersion = (& $node.Source --version).Trim()
. (Join-Path $root 'tools/native-build-state.ps1')
if ($SkipBuild) { $verificationLock = Enter-NativeOutputLock; Assert-NativeBuildCurrent }

Invoke-Step 'JavaScript tests' {
    $testFiles = Get-ChildItem -LiteralPath 'tests' -Filter '*.test.mjs' | ForEach-Object { $_.FullName }
    if ($testFiles.Count -eq 0) { throw 'No JavaScript tests found.' }
    & $node.Source --test @testFiles
    if ($LASTEXITCODE -ne 0) { throw 'JavaScript tests failed.' }
}

if (-not $SkipBuild) {
    Invoke-Step 'Native build' {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    }
    $verificationLock = Enter-NativeOutputLock
    Assert-NativeBuildCurrent
}

Invoke-Step 'Gameplay runtime' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/gameplay.ps1') -SkipBuild; if ($LASTEXITCODE -ne 0) { throw 'Gameplay verification failed.' } }

Invoke-Step 'Animation system' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/animation.ps1') -SkipBuild; if ($LASTEXITCODE -ne 0) { throw 'Animation evidence failed.' } }

Invoke-Step 'CPU field contracts' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/fields.ps1') -SkipBuild; if ($LASTEXITCODE -ne 0) { throw 'Field reference evidence failed.' } }

Invoke-Step 'Independent smoke solver' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/smoke-gpu.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Smoke CPU/GPU evidence failed.' } }
Invoke-Step 'GPU particle smoke sources' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/smoke-particles.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Particle smoke source evidence failed.' } }
Invoke-Step 'GPU smoke instance service' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/smoke-service.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Smoke instance service evidence failed.' } }
Invoke-Step 'GPU smoke density field publication' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/smoke-fields.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Smoke density field evidence failed.' } }
Invoke-Step 'GPU smoke automatic sharing' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/smoke-sharing.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Smoke automatic sharing evidence failed.' } }
Invoke-Step 'GPU particles and smoke joint WF' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/fields-joint-gpu.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Joint visual field evidence failed.' } }
Invoke-Step 'GPU production services joint WF' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/fields-services-gpu.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Joint production service field evidence failed.' } }
Invoke-Step 'GPU effect field coordinator' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/field-coordinator-gpu.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Effect field coordinator evidence failed.' } }
Invoke-Step 'Smoke medium renderer' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/smoke-host.ps1'); if ($LASTEXITCODE -ne 0) { throw 'Smoke optical host evidence failed.' } }

Invoke-Step 'Native effect instances and sprite contracts' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/effects-runtime.ps1') -SkipBuild; if ($LASTEXITCODE -ne 0) { throw 'Native effect evidence failed.' } }

Invoke-Step 'Particle indirect and lifecycle contracts' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/particles.ps1') -SkipBuild; if ($LASTEXITCODE -ne 0) { throw 'Particle runtime evidence failed.' } }

Invoke-Step 'Shader compile self-test' {
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/compile-shaders.ps1') -SelfTest -Quiet
    if ($LASTEXITCODE -ne 0) { throw 'DXC negative-path self-test failed.' }
    $parsed = $output | ConvertFrom-Json
    if (-not $parsed.failedAsExpected) { throw 'DXC accepted a broken shader.' }
}

Invoke-Step 'Native tests' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/test-native.ps1') -SkipBuild -IncludeGpu
    if ($LASTEXITCODE -ne 0) { throw 'Native regression suite failed.' }
}

Invoke-Step 'Physics destruction evidence' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/simulation/physics.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Physics destruction evidence failed.' }
}

Invoke-Step 'Offline asset tools' {
    & $node.Source 'tools\offline\make-fixtures.mjs' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'glTF fixtures failed to generate.' }
    & $node.Source 'tools\offline\gltf.mjs' export --in 'tools\offline\fixtures\quad.gltf' --out 'build\offline\gltf.mesh.json' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'glTF export failed.' }
    $sdf = & $node.Source 'tools\offline\sdf-bake.mjs' self-test
    if ($LASTEXITCODE -ne 0) { throw 'SDF self-test failed.' }
    if (-not ($sdf | Where-Object { $_ -match '^self-test: PASS' })) { throw 'SDF self-test did not report PASS.' }
    & $node.Source 'tools\offline\sdf-bake.mjs' bake --out 'build\offline\sdf' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'SDF bake failed.' }
    & $node.Source 'tools\offline\sdf-bake.mjs' verify --volume 'build\offline\sdf\sdf.volume.json' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'SDF volume verification failed.' }
}

Invoke-Step 'Scene design lint' {
    $lint = & $node.Source 'tools\scene.mjs' lint '.screen\courtyard.scene.json' --json
    if ($LASTEXITCODE -ne 0) { throw 'Courtyard scene has lint errors.' }
    $parsed = $lint | ConvertFrom-Json
    if (-not $parsed.ok -or -not $parsed.result.ok) { throw 'Courtyard lint returned ok=false.' }
}

Invoke-Step 'Scene query CLI' {
    $query = Join-Path $env:TEMP ('nativegpu-query-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        '{"where":[{"field":"kind","op":"eq","value":"wall"}]}' | Set-Content -LiteralPath $query -Encoding ascii
        $result = & $node.Source 'tools\scene.mjs' query '.screen\courtyard.scene.json' --file $query
        if ($LASTEXITCODE -ne 0) { throw 'Scene query failed.' }
        $parsed = $result | ConvertFrom-Json
        if (-not $parsed.ok -or [int]$parsed.result.count -lt 1) { throw 'Scene query returned no wall entities.' }
    } finally {
        if (Test-Path -LiteralPath $query) { Remove-Item -LiteralPath $query -Force }
    }
}

Invoke-Step 'Scene contract validation' {
    $output = & $node.Source 'tools\scene.mjs' validate '.screen\courtyard.scene.json'
    $parsed = $output | ConvertFrom-Json
    if (-not $parsed.ok -or -not $parsed.result.valid) { throw 'Scene validation returned an error.' }
}

$smokeReport = Join-Path $context.directory 'verify-native-smoke.json'
Invoke-Step 'Native smoke' {
    $relative = Get-RelativePath $smokeReport
    $process = Start-Process -FilePath '.\build\NativeGPU.exe' `
        -ArgumentList @('--smoke','--frames','240','--report',$relative) `
        -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Native smoke exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $smokeReport -Raw | ConvertFrom-Json
    if (-not $data.success) { throw 'Native smoke report success=false.' }
    if (-not $data.capabilities.initialized) { throw 'Renderer did not initialize.' }
    if ($data.version -ne $context.version) { throw "Smoke report version $($data.version) does not match project version $($context.version)." }
}

$probeReport = Join-Path $context.directory 'verify-probe.json'
Invoke-Step 'Native S1 probe' {
    $relative = Get-RelativePath $probeReport
    $process = Start-Process -FilePath '.\build\NativeGPU.exe' `
        -ArgumentList @('--probe','--frames','240','--report',$relative) `
        -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Native probe exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $probeReport -Raw | ConvertFrom-Json
    if (-not $data.success) { throw 'Native probe report success=false.' }
    if (-not $data.probes.materialTable.implemented) { throw 'Material table probe did not report implemented=true.' }
    if (-not $data.probes.sceneLights.implemented) { throw 'Scene lights probe did not report implemented=true.' }
    $tier = $data.capabilities.raytracingTier
    if ($null -ne $tier -and "$tier" -ne 'unsupported' -and -not $data.probes.raytracingAccelerationStructures.built) {
        throw "DXR acceleration structures were available but not built: $($data.probes.raytracingAccelerationStructures.error)"
    }
    # The probe run has the shadow pass enabled by default, so the effect must be real.
    $shadows = $data.probes.raytracingShadows
    if ($null -ne $tier -and "$tier" -ne 'unsupported' -and $shadows.dxcAvailable) {
        if (-not $shadows.rtpsoCreated) { throw "DXR shadow RTPSO was not created: $($shadows.error)" }
        if (-not $shadows.sbtCreated) { throw 'DXR shadow shader binding table was not created.' }
        if (-not $shadows.effectImplemented) { throw 'DXR shadow pass did not report effectImplemented=true.' }
        if ($shadows.dispatchedFrames -lt 1) { throw 'DXR shadow pass dispatched no frames.' }
        if (-not $data.capabilities.implemented.raytracing) { throw 'capabilities.implemented.raytracing stayed false while the pass ran.' }
    }
}

Invoke-Step 'DXR shadow evidence' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/rt-shadows.ps1') -SkipBuild
    if ($LASTEXITCODE -ne 0) { throw 'DXR shadow evidence checks failed.' }
    $evidence = Join-Path 'reports\latest' 'rt-shadows-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'DXR shadow evidence report is missing.' }
    $parsed = Get-Content -LiteralPath $evidence -Raw | ConvertFrom-Json
    if (-not $parsed.success) { throw 'DXR shadow evidence reported success=false.' }
    # The evidence script writes its own versioned run; carry the summary into this run.
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'rt-shadows-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'rt-shadows-evidence.json') | Out-Null
}

$hotReloadReport = Join-Path $context.directory 'verify-hot-reload.json'
Invoke-Step 'Mesh shader and bindless evidence' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/mesh-bindless.ps1') -SkipBuild
    if ($LASTEXITCODE -ne 0) { throw 'Mesh shader and bindless evidence checks failed.' }
    $evidence = Join-Path 'reports\latest' 'mesh-bindless-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Mesh/bindless evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'mesh-bindless-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'mesh-bindless-evidence.json') | Out-Null
}

Invoke-Step 'G1 reliability suite' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/g1-reliability.ps1') -SkipBuild -LifecycleCycles 50
    if ($LASTEXITCODE -ne 0) { throw 'G1 reliability suite failed.' }
    $evidence = Join-Path 'reports\latest' 'g1-reliability.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'G1 reliability report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'g1-reliability.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'g1-reliability.json') | Out-Null
}

Invoke-Step 'Determinism evidence' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/determinism.ps1') -SkipBuild -Frames 240
    if ($LASTEXITCODE -ne 0) { throw 'Determinism evidence suite failed.' }
    $evidence = Join-Path 'reports\latest' 'determinism-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Determinism evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'determinism-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'determinism-evidence.json') | Out-Null
}

Invoke-Step 'Frame budget evidence' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/budget.ps1') -SkipBuild -Frames 240
    if ($LASTEXITCODE -ne 0) { throw 'Frame budget evidence suite failed.' }
    $evidence = Join-Path 'reports\latest' 'budget-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Budget evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'budget-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'budget-evidence.json') | Out-Null
}

Invoke-Step 'Audio mixer and host routing' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/audio.ps1') -SkipBuild -Frames 240
    if ($LASTEXITCODE -ne 0) { throw 'Audio evidence suite failed.' }
    $evidence = Join-Path 'reports\latest' 'audio-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Audio evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'audio-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'audio-evidence.json') | Out-Null
}

Invoke-Step 'Replay and black box' {    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/core/replay.ps1') -SkipBuild -Frames 240
    if ($LASTEXITCODE -ne 0) { throw 'Replay evidence suite failed.' }
    $evidence = Join-Path 'reports\latest' 'replay-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Replay evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'replay-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'replay-evidence.json') | Out-Null
}

Invoke-Step 'Image error contracts' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/error-contract.ps1') -SkipBuild
    if ($LASTEXITCODE -ne 0) { throw 'Image error contracts failed.' }
    $evidence = Join-Path 'reports\latest' 'error-contract-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Error contract evidence is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'error-contract-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'error-contract-evidence.json') | Out-Null
}

Invoke-Step 'Image regression gate' {
    # Baselines live under the gitignored reports/ tree, so a fresh checkout has none.
    # Bootstrapping is explicit and reported instead of silently passing: the first run
    # accepts the current frame, the second one compares against it.
    $baselinePath = Join-Path 'reports\image-baselines' 'courtyard-default.png'
    if (-not (Test-Path -LiteralPath $baselinePath)) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/image-regression.ps1') -SkipBuild -Accept
        if ($LASTEXITCODE -ne 0) { throw 'Image regression baseline bootstrap failed.' }
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/image-regression.ps1') -SkipBuild
    if ($LASTEXITCODE -ne 0) { throw 'Image regression gate failed.' }
    $evidence = Join-Path 'reports\latest' 'image-regression-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Image regression evidence is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'image-regression-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'image-regression-evidence.json') | Out-Null
}

Invoke-Step 'Compute queue and heap indexing' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '.incrementalTesting/rendering/compute.ps1') -SkipBuild -Frames 240
    if ($LASTEXITCODE -ne 0) { throw 'Compute evidence suite failed.' }
    $evidence = Join-Path 'reports\latest' 'compute-evidence.json'
    if (-not (Test-Path -LiteralPath $evidence)) { throw 'Compute evidence report is missing.' }
    Copy-Item -LiteralPath $evidence -Destination (Join-Path $context.directory 'compute-evidence.json') -Force
    Register-Report -Context $context -Path (Join-Path $context.directory 'compute-evidence.json') | Out-Null
}

Invoke-Step 'Live scene hot reload' {
    $directory = Join-Path $env:TEMP ('nativegpu-verify-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory | Out-Null
    try {
        $scene = Join-Path $directory 'live.scene.json'
        $report = Join-Path $directory 'report.json'
        Copy-Item -LiteralPath '.screen\courtyard.scene.json' -Destination $scene
        $process = Start-Process -FilePath '.\build\NativeGPU.exe' `
            -ArgumentList @('--smoke','--watch','--frames','420','--scene',$scene,'--report',$report) `
            -WorkingDirectory $root -WindowStyle Hidden -PassThru
        Start-Sleep -Milliseconds 1200
        $apply = & $node.Source 'tools\scene.mjs' apply $scene 'ai\example-transaction.json' | ConvertFrom-Json
        if (-not $apply.ok) { throw 'Scene transaction failed during hot reload test.' }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Hot reload process exited $($process.ExitCode)." }
        $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        if (-not $data.success) { throw 'Hot reload smoke report success=false.' }
        if ($data.hotReloads -lt 1) { throw 'The native host did not report a hot reload.' }
        if ($data.revision -lt 1) { throw 'The running host did not adopt the new revision.' }
        Copy-Item -LiteralPath $report -Destination $hotReloadReport -Force
    } finally {
        Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Invoke-Step 'MCP protocol smoke' {
    $messages = @(
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"verify","version":"1"}}}',
        '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"scene_validate","arguments":{"scenePath":".screen/courtyard.scene.json"}}}'
    ) -join "`n"
    $stderrPath = Join-Path $env:TEMP ('nativegpu-mcp-' + [guid]::NewGuid().ToString('N') + '.log')
    try {
        $output = $messages | & $node.Source 'ai\mcp-server.mjs' '--quiet' 2> $stderrPath
    } finally {
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
    $responses = @($output | ForEach-Object { $_ | ConvertFrom-Json })
    if ($responses.Count -ne 3) { throw "Expected 3 MCP responses, got $($responses.Count)." }
    if ($responses[0].result.serverInfo.name -ne 'nativegpu-scene') { throw 'Unexpected MCP server identity.' }
    if ($responses[1].result.tools.Count -lt 8) { throw 'MCP tool catalog is incomplete.' }
    $validated = $responses[2].result.content[0].text | ConvertFrom-Json
    if (-not $validated.valid) { throw 'MCP scene validation failed.' }
}

Invoke-Step 'Native provenance after verification' {
    Assert-NativeBuildCurrent
    if ((Get-FileHash -LiteralPath (Join-Path $root '.incrementalTesting/core/verify.ps1') -Algorithm SHA256).Hash -ne $verificationScriptHash) { throw 'Verification script changed during this run.' }
    $sourceSnapshot = Join-Path $context.directory 'native-source-inputs.json'
    $stateSnapshot = Join-Path $context.directory 'native-build-state.json'
    Copy-Item -LiteralPath 'build/source-inputs.json' -Destination $sourceSnapshot
    Copy-Item -LiteralPath 'build/last-build-state.json' -Destination $stateSnapshot
    $state = Get-Content -LiteralPath $stateSnapshot -Raw | ConvertFrom-Json
    $sourceHash = (Get-FileHash -LiteralPath $sourceSnapshot -Algorithm SHA256).Hash
    if ($state.state -ne 'verified' -or $sourceHash -ne $state.sourceManifestHash) { throw 'Build provenance changed while archiving the verification snapshot.' }
    $script:nativeProvenance = [ordered]@{
        sourceInputs = Get-RelativePath $sourceSnapshot
        buildState = Get-RelativePath $stateSnapshot
        sourceManifestHash = $sourceHash
        verificationScriptHash = $verificationScriptHash
        capturedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    }
}

foreach ($pair in @(
    @{ path = $smokeReport; aliases = @('verify-native-smoke.json') },
    @{ path = $probeReport; aliases = @('verify-probe.json') },
    @{ path = $hotReloadReport; aliases = @('verify-hot-reload.json') }
)) {
    if (Test-Path -LiteralPath $pair.path) {
        Register-Report -Context $context -Path $pair.path -LegacyAliases $pair.aliases | Out-Null
    }
}

$adapterFromProbe = $null
if (Test-Path -LiteralPath $probeReport) {
    try {
        $probeData = Get-Content -LiteralPath $probeReport -Raw | ConvertFrom-Json
        $adapterFromProbe = $probeData.capabilities.adapter
    } catch { }
}
# The build ran inside this script, so the build identity must be re-read before the
# manifest decides whether this evidence came from the current commit.
$context.build = Get-BuildIdentity

$summary = [pscustomobject]@{
    schema = 'nativegpu.verify/2'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    build = $context.build
    nativeProvenance = $nativeProvenance
    node = $nodeVersion
    adapter = if ($adapterFromProbe) { $adapterFromProbe } else { $context.adapter.name }
    dxc = $context.dxc.version
    dxcAvailable = $context.dxc.available
    sceneSha256 = $context.sceneHash
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    success = $overall
    steps = $steps
}
$summaryPath = Join-Path $context.directory 'verify.json'
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Register-Report -Context $context -Path $summaryPath -LegacyAliases @('verify.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $overall

Write-Host ""
Write-Host "Verification summary" -ForegroundColor Cyan
$steps | Format-Table name, ok, durationMs, error -AutoSize | Out-String | Write-Host
Write-Host "Evidence:  $(Get-RelativePath $context.directory)"
Write-Host "Manifest:  $(Get-RelativePath $runPath)"
Write-Host "Alias:     reports/verify.json"
if (-not $overall) { exit 1 }
} finally {
    Exit-NativeOutputLock $verificationLock
    Pop-Location
}

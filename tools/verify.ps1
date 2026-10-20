param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')
$context = New-ReportContext -Mode 'verify'
$steps = [System.Collections.Generic.List[object]]::new()
$overall = $true

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

Invoke-Step 'JavaScript tests' {
    $testFiles = Get-ChildItem -LiteralPath 'tests' -Filter '*.test.mjs' | ForEach-Object { $_.FullName }
    if ($testFiles.Count -eq 0) { throw 'No JavaScript tests found.' }
    & $node.Source --test @testFiles
    if ($LASTEXITCODE -ne 0) { throw 'JavaScript tests failed.' }
}

if (-not $SkipBuild) {
    Invoke-Step 'Native build' {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    }
}

Invoke-Step 'Shader compile self-test' {
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'compile-shaders.ps1') -SelfTest -Quiet
    if ($LASTEXITCODE -ne 0) { throw 'DXC negative-path self-test failed.' }
    $parsed = $output | ConvertFrom-Json
    if (-not $parsed.failedAsExpected) { throw 'DXC accepted a broken shader.' }
}

Invoke-Step 'Native tests' {
    & '.\build\native_simulation_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'native_simulation_tests failed.' }
    & '.\build\native_scene_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'native_scene_tests failed.' }
}

Invoke-Step 'Scene contract validation' {
    $output = & $node.Source 'tools\scene.mjs' validate 'assets\courtyard.scene.json'
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
}

$hotReloadReport = Join-Path $context.directory 'verify-hot-reload.json'
Invoke-Step 'Live scene hot reload' {
    $directory = Join-Path $env:TEMP ('nativegpu-verify-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory | Out-Null
    try {
        $scene = Join-Path $directory 'live.scene.json'
        $report = Join-Path $directory 'report.json'
        Copy-Item -LiteralPath 'assets\courtyard.scene.json' -Destination $scene
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
        '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"scene_validate","arguments":{"scenePath":"assets/courtyard.scene.json"}}}'
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

$summary = [pscustomobject]@{
    schema = 'nativegpu.verify/2'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    build = $context.build
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
Pop-Location
if (-not $overall) { exit 1 }

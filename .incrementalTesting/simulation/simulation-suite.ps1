param([switch]$SkipBuild,[string]$OutputDirectory='build')
# Unified simulation evidence entry (plan A–G, stage G integration gate).
#
# This runs every CPU authority reference and every asset contract test that exists
# today and writes one versioned report. GPU cloth, particle and shared-read field
# paths have separate incremental gates and are outside this CPU suite.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1') -OutputDirectory $OutputDirectory
        if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    }
    . (Join-Path $root 'tools/native-build-state.ps1')
    Assert-NativeBuildCurrent -Directory $OutputDirectory
    . (Join-Path $root 'tools/report-lib.ps1')
    $context = New-ReportContext -Mode 'simulation-suite'
    $checks = [System.Collections.Generic.List[object]]::new()
    function Add-Check([string]$name, [bool]$ok, [string]$detail) {
        $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
        $status = if ($ok) { 'PASS' } else { 'FAIL' }
        Write-Host "[$status] $name - $detail"
    }
    $references = [ordered]@{}
    $executables = [ordered]@{
        effects = 'native_effects_tests.exe'
        fields  = 'native_fields_tests.exe'
        cloth   = 'native_cloth_tests.exe'
        water   = 'native_water_tests.exe'
        coupling = 'native_field_adapters_tests.exe'
    }
    foreach ($name in $executables.Keys) {
        $path = Join-Path $OutputDirectory $executables[$name]
        if (-not (Test-Path -LiteralPath $path)) {
            Add-Check "$name reference present" $false "missing $path"
            continue
        }
        $raw = & (Resolve-Path -LiteralPath $path)
        $exit = $LASTEXITCODE
        $parsed = $null
        try { $parsed = ($raw | ConvertFrom-Json) } catch { }
        $references[$name] = [pscustomobject]@{ exit = $exit; output = $parsed }
        $ok = $exit -eq 0 -and $null -ne $parsed -and $parsed.ok
        Add-Check "$name reference" ([bool]$ok) "exit=$exit groups=$($parsed.groups) checks=$($parsed.checks)"
    }
    # Asset contract tests are part of the same gate: a solver that passes its numeric
    # fixture while the cooker publishes an unusable asset is not a usable system.
    $jsTests = @('tests/effects.test.mjs', 'tests/cloth.test.mjs', 'tests/water.test.mjs')
    $tap = & node --test --test-reporter=tap @jsTests 2>&1
    $tapText = ($tap | Out-String)
    $exit = $LASTEXITCODE
    $pass = 0; $fail = 0
    if ($tapText -match '# pass (\d+)') { $pass = [int]$Matches[1] }
    if ($tapText -match '# fail (\d+)') { $fail = [int]$Matches[1] }
    Add-Check 'asset contract tests' ($exit -eq 0 -and $fail -eq 0 -and $pass -gt 0) "pass=$pass fail=$fail"
    $reportPath = Join-Path $context.directory 'asset-contracts.tap.txt'
    Set-Content -LiteralPath $reportPath -Value $tapText -Encoding utf8
    $ok = @($checks | Where-Object { -not $_.ok }).Count -eq 0
    Assert-NativeBuildCurrent -Directory $OutputDirectory
    $evidence = [ordered]@{
        schema = 'nativegpu.simulation-suite-evidence/1'
        generatedAtUtc = $context.timestamp
        build = $context.build
        sourceManifestHash = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'source-inputs.json') -Algorithm SHA256).Hash
        buildStateHash = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'last-build-state.json') -Algorithm SHA256).Hash
        scope = 'CPU references, cloth/water field adapters and asset contracts; GPU cloth/particle/shared-read field paths have separate incremental gates. GPU water/field-write solvers, editor panels and the joint rain-camp scene remain pending'
        references = $references
        assetContracts = [pscustomobject]@{ pass = $pass; fail = $fail; report = 'asset-contracts.tap.txt' }
        checks = $checks
        ok = $ok
    }
    $destination = Join-Path $context.directory 'simulation-suite-evidence.json'
    $evidence | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $destination -Encoding utf8
    Register-Report -Context $context -Path $destination -LegacyAliases @('simulation-suite-evidence.json') | Out-Null
    Write-Host "Simulation suite: $($checks.Count - @($checks | Where-Object { -not $_.ok }).Count)/$($checks.Count); $destination"
    if (-not $ok) { exit 1 }
} finally { Pop-Location }

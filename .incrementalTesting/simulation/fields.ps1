param([switch]$SkipBuild,[string]$OutputDirectory='build')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build-fields.ps1') -OutputDirectory $OutputDirectory
        if ($LASTEXITCODE -ne 0) { throw 'Field reference build failed.' }
    }
    $manifest = Get-Content -LiteralPath (Join-Path $OutputDirectory 'fields-source-inputs.json') -Raw | ConvertFrom-Json
    foreach ($source in $manifest.sources.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath $source.Name -Algorithm SHA256).Hash -ne $source.Value) {
            throw "Stale field binary; rebuild after changing $($source.Name)"
        }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'native_fields_tests.exe') -Algorithm SHA256).Hash -ne $manifest.binaryHash) {
        throw 'Field binary does not match build provenance.'
    }
    $raw = & (Resolve-Path -LiteralPath (Join-Path $OutputDirectory 'native_fields_tests.exe'))
    if ($LASTEXITCODE -ne 0) { throw 'Field reference checks failed.' }
    $result = $raw | ConvertFrom-Json
    if (-not $result.ok) { throw 'Field reference checks reported failure.' }
    . (Join-Path $root 'tools/report-lib.ps1')
    $context = New-ReportContext -Mode 'fields-reference'
    $directory = Join-Path $context.directory 'simulation'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $report = Join-Path $directory 'fields-reference-evidence.json'
    [ordered]@{
        timestampUtc = $context.timestamp
        commit = $context.commit
        dirty = $context.dirty
        build = $manifest
        results = $result
        scope = 'CPU regional fields, rational clock, atomic multi-channel publication, LEF/WF projection, per-source representation and observability reference; GPU and host integration pending'
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $report -Encoding utf8
    Register-Report -Context $context -Path $report | Out-Null
    Write-Host "Fields reference: $($result.groups) groups, $($result.checks) checks; evidence $report"
} finally { Pop-Location }

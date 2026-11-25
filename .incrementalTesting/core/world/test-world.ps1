param(
    [switch]$SkipNative,
    [switch]$SkipBuild
)

# Incremental entry for the world contract: JS authoring rules plus the native compiler and
# runtime. It builds only build/world-host and runs only world tests — never the full suite.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$root = Split-Path $root -Parent
$root = Split-Path $root -Parent
Push-Location $root
try {
    Write-Host 'world: contract tests'
    & node --test '.incrementalTesting/core/world/world-contract.test.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'World contract tests failed.' }

    if ($SkipNative) { return }

    $native = Join-Path $root 'build/world-host/native_world_tests.exe'
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build-world.ps1') -Force
        if ($LASTEXITCODE -ne 0) { throw 'World host build failed.' }
    }
    if (-not (Test-Path -LiteralPath $native)) {
        throw 'Missing build/world-host/native_world_tests.exe; run without -SkipBuild.'
    }

    Write-Host 'world: native compiler and runtime'
    & $native
    if ($LASTEXITCODE -ne 0) { throw 'Native world tests failed.' }
} finally {
    Pop-Location
}

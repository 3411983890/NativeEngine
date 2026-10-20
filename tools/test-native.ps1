$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    & '.\build\native_simulation_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'native_simulation_tests failed.' }
    & '.\build\native_scene_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'native_scene_tests failed.' }
    & '.\build\budget_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'budget_tests failed.' }
    & '.\build\rhi_conformance_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'rhi_conformance_tests failed.' }
} finally {
    Pop-Location
}

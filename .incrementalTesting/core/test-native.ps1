param([switch]$SkipBuild,[switch]$IncludeGpu,[string]$OutputDirectory='build')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1') -OutputDirectory $OutputDirectory
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    }
    . (Join-Path $root 'tools/native-build-state.ps1')
    Assert-NativeBuildCurrent -Directory $OutputDirectory
    & (Join-Path $OutputDirectory 'native_gameplay_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_gameplay_tests failed.' }
    & (Join-Path $OutputDirectory 'native_gameplay_integration_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_gameplay_integration_tests failed.' }
    & (Join-Path $OutputDirectory 'native_simulation_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_simulation_tests failed.' }
    & (Join-Path $OutputDirectory 'native_physics_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_physics_tests failed.' }
    & (Join-Path $OutputDirectory 'native_broadphase_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_broadphase_tests failed.' }
    & (Join-Path $OutputDirectory 'native_joints_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_joints_tests failed.' }
    & (Join-Path $OutputDirectory 'native_fracture_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_fracture_tests failed.' }
    & (Join-Path $OutputDirectory 'native_destruction_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_destruction_tests failed.' }
    & (Join-Path $OutputDirectory 'native_animation_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_animation_tests failed.' }
    & (Join-Path $OutputDirectory 'native_scene_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_scene_tests failed.' }
    & (Join-Path $OutputDirectory 'native_audio_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_audio_tests failed.' }
    & (Join-Path $OutputDirectory 'renderer_math_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'renderer_math_tests failed.' }
    & (Join-Path $OutputDirectory 'native_fogtown_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_fogtown_tests failed.' }
    & (Join-Path $OutputDirectory 'budget_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'budget_tests failed.' }
    & (Join-Path $OutputDirectory 'rhi_conformance_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'rhi_conformance_tests failed.' }
    & (Join-Path $OutputDirectory 'native_fields_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_fields_tests failed.' }
    & (Join-Path $OutputDirectory 'native_effects_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_effects_tests failed.' }
    & (Join-Path $OutputDirectory 'native_vector_source_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_vector_source_tests failed.' }
    & (Join-Path $OutputDirectory 'native_cloth_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_cloth_tests failed.' }
    & (Join-Path $OutputDirectory 'native_water_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_water_tests failed.' }
    & (Join-Path $OutputDirectory 'native_field_adapters_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'native_field_adapters_tests failed.' }
    if($IncludeGpu) {
        & (Join-Path $OutputDirectory 'native_cloth_draw_tests.exe')
        if($LASTEXITCODE -ne 0) {throw 'native_cloth_draw_tests failed.'}
        & (Join-Path $OutputDirectory 'native_cloth_gpu_tests.exe') --library build/cloth/library.json
        if($LASTEXITCODE -ne 0) {throw 'native_cloth_gpu_tests failed.'}
        & (Join-Path $OutputDirectory 'native_effect_gpu_tests.exe')
        if($LASTEXITCODE -ne 0) {throw 'native_effect_gpu_tests failed.'}
        & (Join-Path $OutputDirectory 'native_effect_gpu_runtime_tests.exe')
        if($LASTEXITCODE -ne 0) {throw 'native_effect_gpu_runtime_tests failed.'}
        & (Join-Path $OutputDirectory 'native_effect_field_gpu_tests.exe')
        if($LASTEXITCODE -ne 0) {throw 'native_effect_field_gpu_tests failed.'}
        & (Join-Path $OutputDirectory 'native_effect_gpu_draw_tests.exe')
        if($LASTEXITCODE -ne 0) {throw 'native_effect_gpu_draw_tests failed.'}
    }
    Assert-NativeBuildCurrent -Directory $OutputDirectory
} finally {
    Pop-Location
}

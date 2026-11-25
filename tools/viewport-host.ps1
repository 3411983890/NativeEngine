param(
    [switch]$SkipBuild,
    [switch]$Probe,
    [string]$Screenshot = '',
    [int]$Frames = 0,
    [int]$Instances = 1,
    [string]$Species = 'oak_broadleaf',
    [string]$Parent = '0'
)

# Builds (and optionally runs) the native viewport host used by the editor's C path.
#
# The host is a separate executable on purpose: an N-API addon would put a D3D12 device
# inside the Electron main process, and a device lost would take the whole editor with it.
# A separate process can crash and be restarted without losing the page.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $directory = Join-Path $root 'build/viewport-host'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe = Join-Path $directory 'nativegpu_viewport_host.exe'
    $compiler = Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) { throw 'Run tools/bootstrap.ps1 once to install the LLVM toolchain.' }
    if (-not (Test-Path -LiteralPath 'build/generated/nativegpu_shaders.h')) {
        throw 'Missing build/generated/nativegpu_shaders.h: run tools/compile-shaders.ps1 -Source shaders/vegetation.hlsl first.'
    }

    if ($Probe) {
        # Which swap-chain descriptions this machine accepts is a load-bearing fact (see the
        # header of host/viewport_host.cpp): flip model rejects _SRGB, and the bitblt model is
        # rejected outright on some drivers.
        $probe = Join-Path $directory 'probe_swapchain.exe'
        & $compiler -std=c++17 -O1 -DNOMINMAX -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -static `
            tests/probe_swapchain.cpp -ld3d12 -ldxgi -luser32 -lgdi32 -luuid -o $probe
        if ($LASTEXITCODE -ne 0) { throw 'Swap-chain probe compile failed.' }
        & $probe
        return
    }

    if (-not $SkipBuild) {
        $sources = @(
            'host/viewport_host.cpp', 'host/main.cpp', 'host/scene_loader.cpp', 'host/texture_asset.cpp', 'host/budget.cpp',
            'renderer/d3d12_renderer.cpp', 'renderer/lighting_quality.cpp', 'renderer/rhi.cpp', 'renderer/rhi_d3d12.cpp',
            'renderer/rt_shadows.cpp', 'renderer/mesh_bindless.cpp', 'renderer/particles.cpp', 'renderer/compute_pass.cpp',
            'renderer/effect_sprites.cpp', 'renderer/gpu_effect_sprites.cpp', 'renderer/gpu_effect_bridge.cpp', 'renderer/gpu_field_bridge.cpp',
            'renderer/water_draw.cpp', 'renderer/water_ripple.cpp', 'renderer/waterfall_draw.cpp', 'engine/native/water/body.cpp', 'engine/native/water/asset.cpp',
            # game.cpp references the footprint and coupling entry points, so the host links
            # the same water units the main build does. Without them the link fails on
            # updateFootprint/coupleBox even though nothing in this mode uses water.
            'engine/native/water/coupling.cpp', 'engine/native/water/transfer.cpp',
            'renderer/vegetation_draw.cpp', 'renderer/hzb.cpp', 'renderer/matrix_math.cpp', 'renderer/png_writer.cpp',
            # World mode: the runtime, the bridge and the terrain/instance draw path.
            'renderer/world_draw.cpp', 'host/world_bridge.cpp',
            'engine/native/world/world_path.cpp', 'engine/native/world/world_terrain.cpp',
            'engine/native/world/world_scatter.cpp', 'engine/native/world/world_settlement.cpp',
            'engine/native/world/world_chunk.cpp', 'engine/native/world/world_compiler.cpp',
            'engine/native/world/world_runtime.cpp',
            'renderer/hair_draw.cpp', 'engine/native/hair/gpu_pack.cpp',
            'engine/native/game.cpp', 'engine/native/physics.cpp', 'engine/native/animation.cpp', 'engine/native/animation_features.cpp',
            'engine/native/animation_asset.cpp', 'engine/native/determinism.cpp', 'engine/native/geometry_class.cpp', 'engine/native/material_handle.cpp',
            'engine/native/vegetation/asset.cpp', 'engine/native/vegetation/world.cpp', 'engine/native/assets/package.cpp', 'engine/native/assets/sha256.cpp',
            'engine/native/effects/gpu_pool.cpp', 'engine/native/effects/gpu_runtime.cpp', 'engine/native/effects/runtime.cpp', 'engine/native/effects/asset.cpp',
            'engine/native/fields/gpu_vector.cpp', 'engine/native/fields/gpu_storage.cpp', 'engine/native/fields/world.cpp',
            'host/effect_fields.cpp', 'engine/native/fields/vector_source.cpp',
            'host/simulation_fields.cpp', 'host/cloth_preview.cpp', 'renderer/gpu_cloth_draw.cpp', 'renderer/gpu_cloth_bridge.cpp',
            'engine/native/cloth/batch_gpu.cpp', 'engine/native/cloth/asset.cpp', 'engine/native/cloth/solver_gpu.cpp',
            'renderer/gpu_smoke_bridge.cpp', 'engine/native/smoke/solver.cpp', 'engine/native/smoke/gpu.cpp', 'engine/native/smoke/rate_view.cpp',
            'engine/native/smoke/particle_source.cpp', 'engine/native/smoke/service.cpp', 'engine/native/smoke/sharing.cpp',
            'engine/native/effects/field_deposits.cpp', 'engine/native/fields/gpu_local.cpp', 'engine/native/fields/local.cpp',
            'engine/native/fields/gpu_world.cpp', 'engine/native/fields/gpu_sharing.cpp', 'engine/native/fields/effect_services.cpp',
            'engine/audio/audio.cpp', 'engine/audio/audio_asset.cpp', 'engine/audio/audio_device.cpp',
            'build/gameplay/nativegpu-gameplay.a'
        )
        $flags = @('-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Wpedantic', '-DNOMINMAX', '-DWINVER=0x0A00', '-DUNICODE', '-D_UNICODE',
                   '-D_WIN32_WINNT=0x0A00', '-DNATIVEGPU_SCENE_LIBRARY', '-Ibuild/generated', '-static')
        $libs = @('-ld3d12', '-ldxgi', '-ld3dcompiler', '-lbcrypt', '-lole32', '-luuid', '-luser32', '-lgdi32', '-lcomdlg32', '-lshell32', '-lwinmm')
        $arguments = $flags + $sources + $libs + @('-o', $exe)
        $log = Join-Path $directory 'compile.log'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { & $compiler @arguments *> $log; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
        if ($code -ne 0) { Get-Content -LiteralPath $log -Tail 40; throw 'Viewport host compile failed.' }
        Write-Host "Built $exe"
    }

    if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe" }
    if ($Screenshot -or $Frames -gt 0) {
        $runArgs = @('--species', $Species, '--instances', "$Instances", '--parent', $Parent)
        if ($Screenshot) { $runArgs += @('--screenshot', $Screenshot) }
        if ($Frames -gt 0) { $runArgs += @('--frames', "$Frames") }
        & $exe @runArgs
        if ($LASTEXITCODE -ne 0) { throw "Viewport host exited with $LASTEXITCODE" }
    }
} finally { Pop-Location }

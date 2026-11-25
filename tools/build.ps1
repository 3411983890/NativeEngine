param([switch]$DebugBuild, [switch]$SkipShaders, [string]$OutputDirectory='build')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
$buildMutex = $null
$buildMutexHeld = $false
$buildStatePath = $null
try {
    $outputPath = [IO.Path]::GetFullPath($OutputDirectory)
    $mutexHasher = [Security.Cryptography.SHA256]::Create()
    try { $mutexKey = ([BitConverter]::ToString($mutexHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($outputPath.ToLowerInvariant())))).Replace('-','') } finally { $mutexHasher.Dispose() }
    $buildMutex = New-Object Threading.Mutex($false,('Local\NativeGPU-build-'+$mutexKey))
    try { $buildMutexHeld = $buildMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $buildMutexHeld = $true }
    if (-not $buildMutexHeld) { throw 'A native build is already running for this output directory.' }
    $compiler = Join-Path $root '.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) {
        & "$PSScriptRoot\bootstrap.ps1"
        if ($LASTEXITCODE -ne 0) { throw 'Toolchain setup failed.' }
    }
    New-Item -ItemType Directory -Force -Path 'build','reports','build\generated' | Out-Null
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $buildStatePath = Join-Path $outputPath 'last-build-state.json'
    [ordered]@{ state='building'; processId=$PID; startedUtc=(Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $buildStatePath -Encoding utf8
    $builtExecutables = [System.Collections.Generic.List[string]]::new()
    & node 'tools/offline/animation-fixture.mjs' 'build/animation/mannequin.gltf'
    if ($LASTEXITCODE -ne 0) { throw 'Animation fixture generation failed.' }
    & node 'tools/offline/animation.mjs' import --in 'build/animation/mannequin.gltf' --out 'build/animation/mannequin'
    if ($LASTEXITCODE -ne 0) { throw 'Animation import failed.' }
    & node 'tools/effects-fixtures.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'Effect texture fixture generation failed.' }
    & node 'tools/effects.mjs' cook --in 'assets/effects' --out 'build/effects'
    if ($LASTEXITCODE -ne 0) { throw 'Effect library cook failed.' }
    & node 'tools/effects-field-fixtures.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'Velocity field fixture cook failed.' }
    & node 'tools/cloth.mjs' cook --in 'assets/cloth' --out 'build/cloth'
    if ($LASTEXITCODE -ne 0) { throw 'Cloth library cook failed.' }
    & node 'tools/cloth-gpu-fixtures.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'GPU cloth fixture cook failed.' }
    & node 'tools/cloth-gpu-fixtures.mjs' 'build/cloth-draw'
    if ($LASTEXITCODE -ne 0) { throw 'GPU cloth draw fixture cook failed.' }
    & node 'tools/cloth-grouped-fixtures.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'GPU cloth grouped fixture cook failed.' }
    & node 'tools/water.mjs' cook --in 'assets/water' --out 'build/water'
    if ($LASTEXITCODE -ne 0) { throw 'Water library cook failed.' }

    # Vegetation and groom content. The cookers are deterministic and content-addressed,
    # so this step is a no-op when the sources have not changed; the native tests then
    # read the exact bytes the runtime loads.
    & node 'tools/vegetation-fixtures.mjs' 'build/vegetation/source'
    if ($LASTEXITCODE -ne 0) { throw 'Vegetation fixture generation failed.' }
    # Vegetation textures are authored in two places: the oak photo set under
    # assets/source/vegetation and the generated needle/diagnostic atlases under
    # assets/vegetation/textures/generated. The cook takes one directory, so the
    # build stages a merged copy first; the cook itself is still content-addressed.
    New-Item -ItemType Directory -Force -Path 'build\vegetation\textures-source' | Out-Null
    Copy-Item 'assets/source/vegetation/oak-imagegen-v1/complete/*.png' 'build/vegetation/textures-source/' -Force
    Copy-Item 'assets/vegetation/textures/generated/*.png' 'build/vegetation/textures-source/' -Force
    & node 'tools/vegetation-textures.mjs' cook --in 'build/vegetation/textures-source' --out 'build/vegetation/textures' --roles 'assets/vegetation/textures/roles.json'
    if ($LASTEXITCODE -ne 0) { throw 'Vegetation texture cook failed.' }
    & node 'tools/vegetation.mjs' library --in 'build/vegetation/source' --out 'build/vegetation/cooked' --textures 'build/vegetation/textures'
    if ($LASTEXITCODE -ne 0) { throw 'Vegetation species cook failed.' }
    & node 'tools/vegetation.mjs' regions --in 'build/vegetation/source' --out 'build/vegetation/cooked' --species 'build/vegetation/cooked'
    if ($LASTEXITCODE -ne 0) { throw 'Vegetation region cook failed.' }
    & node 'tools/groom-fixtures.mjs' 'build/groom/source'
    if ($LASTEXITCODE -ne 0) { throw 'Groom fixture generation failed.' }
    & node 'tools/groom.mjs' library --in 'build/groom/source' --out 'build/groom/cooked'
    if ($LASTEXITCODE -ne 0) { throw 'Groom library cook failed.' }

    $version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
    $commit = 'unknown'
    $dirty = '1'
    try {
        $head = & git rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $head) { $commit = "$head".Trim() }
        $status = & git status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0 -and -not $status) { $dirty = '0' }
    } catch { }
    $versionHeader = @(
        '#pragma once',
        '// Generated by tools/build.ps1. Do not edit.',
        "#define NATIVEGPU_VERSION `"$version`"",
        "#define NATIVEGPU_BUILD_COMMIT `"$commit`"",
        "#define NATIVEGPU_BUILD_DIRTY $dirty"
    )
    Set-Content -LiteralPath 'build\generated\nativegpu_version.h' -Encoding ascii -Value $versionHeader

    # Headers define shared C++ and GPU layouts. Reject a mixed build if another editor
    # changes an input while different translation units or shader blobs are compiling.
    $buildInputHashes=@{}
    $buildInputs=Get-ChildItem -LiteralPath 'host','renderer','engine/native','engine/audio','third_party','shaders','tests' -Recurse -File |
        Where-Object {$_.Extension -in @('.cpp','.c','.h','.hpp','.hlsl','.hlsli','.json')}
    foreach($inputFile in $buildInputs) {
        $buildInputHashes[$inputFile.FullName]=(Get-FileHash -LiteralPath $inputFile.FullName -Algorithm SHA256).Hash
    }
    foreach ($buildTool in @('tools/build.ps1','tools/native-build-state.ps1')) {
        $buildToolPath = Join-Path $root $buildTool
        $buildInputHashes[$buildToolPath] = (Get-FileHash -LiteralPath $buildToolPath -Algorithm SHA256).Hash
    }

    $fingerprintText=($buildInputHashes.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Name.Substring($root.Length)+':'+$_.Value }) -join "`n"
    $sha=[System.Security.Cryptography.SHA256]::Create()
    try {$buildFingerprint=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fingerprintText+" debug=$DebugBuild")))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
    Add-Content -LiteralPath 'build/generated/nativegpu_version.h' -Encoding ascii -Value "#define NATIVEGPU_BUILD_FINGERPRINT `"$buildFingerprint`""

    if (-not $SkipShaders) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'compile-shaders.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Shader build failed.' }
    }
    foreach($inputFile in (Get-ChildItem -LiteralPath 'build/generated' -Filter '*.h' -File)) {
        $buildInputHashes[$inputFile.FullName]=(Get-FileHash -LiteralPath $inputFile.FullName -Algorithm SHA256).Hash
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-gameplay.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Gameplay build failed.' }

    # Only build/generated is on the include path. The repository root must never be an
    # include directory: the filesystem is case-insensitive, so `-I.` makes libc++'s
    # <version> header resolve to the repository VERSION file.
    $flags = @('-std=c++17','-Wall','-Wextra','-Wpedantic','-DUNICODE','-D_UNICODE','-DNOMINMAX','-Ibuild/generated','-static')
    if ($DebugBuild) { $flags += @('-O0','-g') } else { $flags += '-O2' }

    # Every compile writes a log; only the tail is echoed on failure so CI output stays
    # readable while the full diagnostics remain on disk. PowerShell 5.1 turns native
    # stderr into error records, so the preference is relaxed around the child process.
    function Invoke-Compile {
        param([string]$Name, [string[]]$Arguments)
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('renderer/effect_sprites.cpp','renderer/gpu_effect_sprites.cpp','renderer/gpu_effect_bridge.cpp','renderer/gpu_field_bridge.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','-lbcrypt') }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('host/effect_fields.cpp','engine/native/fields/vector_source.cpp') }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('host/simulation_fields.cpp','host/cloth_preview.cpp','renderer/gpu_cloth_draw.cpp','renderer/gpu_cloth_bridge.cpp','engine/native/cloth/batch_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/cloth/solver_gpu.cpp') }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('engine/native/water/body.cpp','engine/native/water/asset.cpp','renderer/water_draw.cpp','renderer/water_ripple.cpp','renderer/waterfall_draw.cpp','engine/native/water/coupling.cpp','engine/native/water/transfer.cpp') }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('renderer/vegetation_draw.cpp','renderer/hzb.cpp','engine/native/vegetation/asset.cpp','engine/native/vegetation/world.cpp','engine/native/assets/package.cpp','renderer/hair_draw.cpp','engine/native/hair/gpu_pack.cpp','engine/native/groom/asset.cpp','engine/native/groom/solver.cpp') }
        if ($Arguments -contains 'engine/native/animation.cpp') { $Arguments += 'engine/native/animation_features.cpp' }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('renderer/gpu_smoke_bridge.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/smoke/sharing.cpp') }
        if ($Arguments -contains 'host/main.cpp') { $Arguments += @('engine/native/effects/field_deposits.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/effect_services.cpp') }
        if ($Arguments -contains 'engine/native/effects/gpu_pool.cpp') { $Arguments += @('engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp') }
        if (($Arguments -contains 'host/scene_loader.cpp') -and ($Arguments -notcontains 'engine/native/assets/sha256.cpp')) {
            $Arguments += @('engine/native/assets/sha256.cpp','-lbcrypt')
        }
        if ($Arguments -contains 'engine/native/game.cpp' -or $Arguments -contains 'host/scene_loader.cpp') {
            $Arguments += @('build/gameplay/nativegpu-gameplay.a')
            if ($Arguments -notcontains 'engine/native/determinism.cpp') { $Arguments += 'engine/native/determinism.cpp' }
        }
        $logPath = Join-Path $OutputDirectory ('compile-' + ($Name -replace '[^A-Za-z0-9]+', '-') + '.log')
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $compiler @flags @Arguments *> $logPath
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($code -ne 0) {
            Write-Host "Compile of $Name failed (exit $code); last 80 lines of $logPath`:" -ForegroundColor Red
            Get-Content -LiteralPath $logPath -Tail 80
            throw "$Name build failed."
        }
        $binaryOption = [Array]::IndexOf($Arguments,'-o')
        if ($binaryOption -ge 0) { $builtExecutables.Add([IO.Path]::GetFullPath($Arguments[$binaryOption+1])) }
    }

    Invoke-Compile 'nativegpu' @('host/main.cpp','host/scene_loader.cpp','host/texture_asset.cpp','host/budget.cpp','renderer/d3d12_renderer.cpp','renderer/lighting_quality.cpp','renderer/rhi.cpp','renderer/rhi_d3d12.cpp','renderer/rt_shadows.cpp','renderer/mesh_bindless.cpp','renderer/particles.cpp','renderer/compute_pass.cpp','renderer/matrix_math.cpp','renderer/png_writer.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','engine/native/geometry_class.cpp','engine/native/material_handle.cpp','engine/audio/audio.cpp','engine/audio/audio_asset.cpp','engine/audio/audio_device.cpp','-municode','-mwindows','-o',(Join-Path $OutputDirectory 'NativeGPU.exe'),'-ld3d12','-ldxgi','-ld3dcompiler','-luser32','-lgdi32','-lcomdlg32','-lshell32','-lole32','-luuid','-lwinmm')
    Invoke-Compile 'simulation-tests' @('tests/native_simulation.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_features.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','engine/native/geometry_class.cpp','engine/native/material_handle.cpp','engine/native/water/body.cpp','engine/native/water/asset.cpp','engine/native/water/coupling.cpp','engine/native/water/transfer.cpp','engine/native/assets/sha256.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_simulation_tests.exe'))
    Invoke-Compile 'physics-tests' @('tests/native_physics.cpp','engine/native/physics.cpp','-o',(Join-Path $OutputDirectory 'native_physics_tests.exe'))
    Invoke-Compile 'broadphase-tests' @('tests/native_broadphase.cpp','engine/native/physics.cpp','-o',(Join-Path $OutputDirectory 'native_broadphase_tests.exe'))
    Invoke-Compile 'frustum-tests' @('tests/native_frustum.cpp','-o',(Join-Path $OutputDirectory 'native_frustum_tests.exe'))
    Invoke-Compile 'batching-tests' @('tests/native_batching.cpp','-o',(Join-Path $OutputDirectory 'native_batching_tests.exe'))
    Invoke-Compile 'joints-tests' @('tests/native_joints.cpp','engine/native/physics.cpp','-o',(Join-Path $OutputDirectory 'native_joints_tests.exe'))
    Invoke-Compile 'fracture-tests' @('tests/native_fracture.cpp','-o',(Join-Path $OutputDirectory 'native_fracture_tests.exe'))
    Invoke-Compile 'effects-tests' @('tests/native_effects.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_effects_tests.exe'))
    Invoke-Compile 'vector-source-tests' @('tests/native_vector_source.cpp','engine/native/fields/vector_source.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_vector_source_tests.exe'))
    Invoke-Compile 'effects-gpu-tests' @('tests/native_effect_gpu.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_gpu_tests.exe'))
    Invoke-Compile 'effects-gpu-runtime-tests' @('tests/native_effect_gpu_runtime.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_gpu_runtime_tests.exe'))
    Invoke-Compile 'effects-fields-tests' @('tests/native_effect_field_gpu.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_field_gpu_tests.exe'))
    Invoke-Compile 'effects-gpu-draw-tests' @('tests/native_effect_gpu_draw.cpp','renderer/gpu_effect_sprites.cpp','renderer/matrix_math.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_gpu_draw_tests.exe'))
    Invoke-Compile 'cloth-tests' @('tests/native_cloth.cpp','engine/native/cloth/asset.cpp','engine/native/cloth/solver_cpu.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_cloth_tests.exe'))
    Invoke-Compile 'cloth-gpu-tests' @('tests/native_cloth_gpu.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/cloth/solver_cpu.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_cloth_gpu_tests.exe'))
    Invoke-Compile 'cloth-draw-tests' @('tests/native_cloth_draw.cpp','host/cloth_preview.cpp','renderer/gpu_cloth_draw.cpp','renderer/matrix_math.cpp','renderer/png_writer.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_cloth_draw_tests.exe'))
    Invoke-Compile 'cloth-grouped-tests' @('tests/native_cloth_grouped.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_cloth_grouped_tests.exe'))
    Invoke-Compile 'cloth-batch-tests' @('tests/native_cloth_batch.cpp','engine/native/cloth/batch_gpu.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_cloth_batch_tests.exe'))
    Invoke-Compile 'shared-wind-tests' @('tests/native_shared_wind.cpp','host/simulation_fields.cpp','engine/native/fields/vector_source.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_shared_wind_tests.exe'))
    Invoke-Compile 'fields-local-gpu-tests' @('tests/native_field_local_gpu.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_field_local_gpu_tests.exe'))
    Invoke-Compile 'fields-world-gpu-tests' @('tests/native_field_world_gpu.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_field_world_gpu_tests.exe'))
    Invoke-Compile 'fields-joint-gpu-tests' @('tests/native_field_joint_gpu.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_field_joint_gpu_tests.exe'))
    Invoke-Compile 'fields-services-gpu-tests' @('tests/native_field_services_gpu.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/smoke/sharing.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/field_deposits.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_field_services_gpu_tests.exe'))
    Invoke-Compile 'fields-coordinator-gpu-tests' @('tests/native_field_coordinator_gpu.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/smoke/sharing.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/field_deposits.cpp','engine/native/fields/effect_services.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_field_coordinator_gpu_tests.exe'))
    Invoke-Compile 'effects-deposits-tests' @('tests/native_effect_deposits.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_deposits_tests.exe'))
    Invoke-Compile 'effects-deposit-service-tests' @('tests/native_effect_deposit_service.cpp','engine/native/effects/field_deposits.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_effect_deposit_service_tests.exe'))
    Invoke-Compile 'smoke-gpu-tests' @('tests/native_smoke.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_smoke_tests.exe'))
    Invoke-Compile 'smoke-particles-tests' @('tests/native_smoke_particles.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_smoke_particles_tests.exe'))
    Invoke-Compile 'smoke-service-tests' @('tests/native_smoke_service.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_smoke_service_tests.exe'))
    Invoke-Compile 'smoke-fields-tests' @('tests/native_smoke_fields.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_smoke_fields_tests.exe'))
    Invoke-Compile 'smoke-sharing-tests' @('tests/native_smoke_sharing.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/smoke/sharing.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp','-ffp-contract=off','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_smoke_sharing_tests.exe'))
    Invoke-Compile 'water-tests' @('tests/native_water.cpp','engine/native/water/asset.cpp','engine/native/water/body.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_water_tests.exe'))
    Invoke-Compile 'field-adapters-tests' @('tests/native_field_adapters.cpp','engine/native/simulation/field_adapters.cpp','engine/native/fields/world.cpp','engine/native/cloth/solver_cpu.cpp','engine/native/cloth/asset.cpp','engine/native/water/body.cpp','engine/native/water/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_field_adapters_tests.exe'))
    Invoke-Compile 'destruction-tests' @('tests/native_destruction.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','engine/native/geometry_class.cpp','engine/native/material_handle.cpp','-o',(Join-Path $OutputDirectory 'native_destruction_tests.exe'))
    Invoke-Compile 'animation-tests' @('tests/native_animation.cpp','engine/native/animation.cpp','engine/native/animation_asset.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/determinism.cpp','-o',(Join-Path $OutputDirectory 'native_animation_tests.exe'))
    Invoke-Compile 'budget-tests' @('tests/budget_tests.cpp','host/budget.cpp','-o',(Join-Path $OutputDirectory 'budget_tests.exe'))
    Invoke-Compile 'rhi-tests' @('tests/rhi_conformance.cpp','renderer/rhi.cpp','renderer/matrix_math.cpp','-o',(Join-Path $OutputDirectory 'rhi_conformance_tests.exe'))
    Invoke-Compile 'vegetation-tests' @('tests/native_vegetation.cpp','engine/native/vegetation/asset.cpp','engine/native/vegetation/world.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp','renderer/hzb.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_vegetation_tests.exe'))
    Invoke-Compile 'groom-tests' @('tests/native_groom.cpp','engine/native/groom/asset.cpp','engine/native/groom/solver.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp','-lbcrypt','-o',(Join-Path $OutputDirectory 'native_groom_tests.exe'))
    Invoke-Compile 'hair-draw-tests' @('tests/native_hair_draw.cpp','renderer/hair_draw.cpp','renderer/png_writer.cpp','renderer/matrix_math.cpp','engine/native/hair/gpu_pack.cpp','engine/native/assets/sha256.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_hair_draw_tests.exe'))
    Invoke-Compile 'vegetation-draw-tests' @('tests/native_vegetation_draw.cpp','renderer/vegetation_draw.cpp','renderer/png_writer.cpp','renderer/matrix_math.cpp','engine/native/vegetation/asset.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp','-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid','-o',(Join-Path $OutputDirectory 'native_vegetation_draw_tests.exe'))
    Invoke-Compile 'scene-tests' @('tests/native_scene.cpp','host/scene_loader.cpp','-o',(Join-Path $OutputDirectory 'native_scene_tests.exe'))
    Invoke-Compile 'audio-tests' @('tests/native_audio.cpp','engine/audio/audio.cpp','engine/audio/audio_asset.cpp','-o',(Join-Path $OutputDirectory 'native_audio_tests.exe'))
    Invoke-Compile 'renderer-math-tests' @('tests/renderer_math.cpp','renderer/matrix_math.cpp','-o',(Join-Path $OutputDirectory 'renderer_math_tests.exe'))
    Invoke-Compile 'gameplay-tests' @('tests/native_gameplay.cpp','engine/native/determinism.cpp','build/gameplay/nativegpu-gameplay.a','-o',(Join-Path $OutputDirectory 'native_gameplay_tests.exe'))
    Invoke-Compile 'gameplay-integration-tests' @('tests/native_gameplay_integration.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','-o',(Join-Path $OutputDirectory 'native_gameplay_integration_tests.exe'))
    Invoke-Compile 'fogtown-tests' @('tests/native_fogtown.cpp','host/scene_loader.cpp','engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','-o',(Join-Path $OutputDirectory 'native_fogtown_tests.exe'))
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-fields.ps1') -OutputDirectory $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Field reference build failed.' }
    $builtExecutables.Add([IO.Path]::GetFullPath((Join-Path $OutputDirectory 'native_fields_tests.exe')))
    $changedInputs=@($buildInputHashes.Keys | Where-Object {
        -not (Test-Path -LiteralPath $_) -or
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash -ne $buildInputHashes[$_]
    })
    if($changedInputs.Count) {throw "Source inputs changed during compilation; rerun the build: $($changedInputs -join ', ')"}
    $buildInputHashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'source-inputs.json') -Encoding utf8
    $executableHashes = [ordered]@{}
    foreach ($binary in $builtExecutables) { $executableHashes[$binary] = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash }
    [ordered]@{ state='verified'; completedUtc=(Get-Date).ToUniversalTime().ToString('o'); sourceManifestHash=(Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'source-inputs.json') -Algorithm SHA256).Hash; executables=$executableHashes } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $buildStatePath -Encoding utf8
    Write-Host "Built $OutputDirectory\NativeGPU.exe and native tests (version $version, commit $commit, dirty $dirty)."
} catch {
    if ($buildStatePath) {
        [ordered]@{ state='failed'; completedUtc=(Get-Date).ToUniversalTime().ToString('o'); error=$_.Exception.Message } |
            ConvertTo-Json | Set-Content -LiteralPath $buildStatePath -Encoding utf8
    }
    throw
} finally {
    if ($buildMutexHeld) { $buildMutex.ReleaseMutex() }
    if ($buildMutex) { $buildMutex.Dispose() }
    Pop-Location
}

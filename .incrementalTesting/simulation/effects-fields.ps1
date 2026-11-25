param([switch]$SkipBuild,[string]$Adapter='AMD')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/effects-fields'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_effect_field_gpu_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        & node tools/effects-field-fixtures.mjs
        if($LASTEXITCODE -ne 0){throw 'Velocity field asset cook failed.'}
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source @('shaders/effect_pool.hlsl','shaders/field_query.hlsl') -OutputDirectory 'build/effects-fields'
        if($LASTEXITCODE -ne 0){throw 'Velocity field shader compilation failed.'}
        $sources=@('tests/native_effect_field_gpu.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp',
            'engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp')
        $inputs=$sources+@('tests/d3d12_test_rig.hpp','engine/native/effects/gpu_pool.hpp','engine/native/effects/gpu_runtime.hpp','engine/native/effects/runtime.hpp',
            'engine/native/fields/gpu_vector.hpp','engine/native/fields/gpu_storage.hpp','engine/native/fields/world.hpp','engine/native/vec3.hpp','engine/native/determinism.hpp','engine/native/assets/sha256.hpp','third_party/json.hpp',
            'shaders/effect_pool.hlsl','shaders/field_query.hlsl','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','shaders/manifest.json','build/effects-fields/generated/nativegpu_shaders.h',
            'assets/effects/wind_drift.effect.json','assets/effects/textures/reference_glow.albedo.ngtex','engine/effects/asset.mjs','tools/effects.mjs','tools/effects-field-fixtures.mjs','tests/effects.test.mjs',
            '.incrementalTesting/simulation/effects-fields.ps1','tools/compile-shaders.ps1')
        $inputs+=@(Get-ChildItem -LiteralPath 'build/effects-fields/cooked' -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})
        $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/effects-fields/generated')+$sources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        & $compiler @arguments *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 35;throw 'Velocity field GPU test compilation failed.'}
        foreach($file in $inputs){if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hashes[$file]){throw "Velocity field input changed during build: $file"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    function Assert-FieldInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale velocity field input: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Velocity field binary differs from the build manifest.'}
    }
    Assert-FieldInputs
    & node --test tests/effects.test.mjs *> (Join-Path $directory 'authoring.log')
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'authoring.log') -Tail 30;throw 'Velocity field authoring regression failed.'}
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw 'Velocity field GPU regression failed.'}
    $result=$raw | ConvertFrom-Json
    if(-not $result.ok){throw 'Velocity field report is not successful.'}
    Assert-FieldInputs
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'effects-fields'
    $report=Join-Path $context.directory 'effects-fields-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;results=$result;authoring='tests/effects.test.mjs';regression='incremental: velocity field consumer and effect asset contract';
        scope='GPU read-only wind/flow velocity modules over immutable CPU-authoritative or visual field snapshots; explicit field time, complete vectors, bounded metre/Q8 mapping and real fence retention; host field producer and field writes pending'} |
        ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $report -Encoding utf8
    foreach($file in @($exe,(Join-Path $directory 'compile.log'),(Join-Path $directory 'authoring.log'),(Join-Path $directory 'shaders/manifest.json'))){Copy-Item -LiteralPath $file -Destination $context.directory}
    $snapshot=Join-Path $context.directory 'source-snapshot'
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $snapshot $entry.Name;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
    Assert-FieldInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-fields-evidence.json') | Out-Null
    Write-Host "GPU effect fields: $($result.groups) groups / $($result.checks) checks / $($result.cpuVectorComparisons) CPU vector comparisons on $($result.adapter); $report"
} finally {Pop-Location}

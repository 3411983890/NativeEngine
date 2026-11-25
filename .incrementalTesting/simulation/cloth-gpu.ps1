param([switch]$SkipBuild,[string]$Adapter='AMD')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/cloth-gpu'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $gpuExe=Join-Path $directory 'native_cloth_gpu_tests.exe'
    $cpuExe=Join-Path $directory 'native_cloth_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        $common=@('engine/native/cloth/asset.cpp','engine/native/cloth/solver_cpu.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp')
        $gpuSources=@('tests/native_cloth_gpu.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp')+$common
        $cpuSources=@('tests/native_cloth.cpp')+$common
        $inputs=@($gpuSources+$cpuSources+@('tests/d3d12_test_rig.hpp','engine/native/cloth/asset.hpp','engine/native/cloth/solver_cpu.hpp','engine/native/cloth/solver_gpu.hpp','engine/native/cloth/triangle_math.hpp',
            'engine/native/fields/gpu_vector.hpp','engine/native/fields/gpu_storage.hpp','engine/native/fields/world.hpp','engine/native/vec3.hpp','engine/native/determinism.hpp','engine/native/assets/sha256.hpp','third_party/json.hpp',
            'shaders/cloth.hlsl','shaders/cloth_constraints.hlsli','shaders/cloth_dynamics.hlsli','shaders/cloth_grouped.hlsli','shaders/cloth_normals.hlsli','shaders/cloth_batch_bindings.hlsli','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','shaders/manifest.json',
            'engine/cloth/asset.mjs','engine/cloth/topology.mjs','engine/assets/canonical.mjs','tools/cloth.mjs','tools/cloth-gpu-fixtures.mjs','tests/cloth.test.mjs',
            '.incrementalTesting/simulation/cloth-gpu.ps1','tools/compile-shaders.ps1','tools/report-lib.ps1','VERSION') | Sort-Object -Unique)
        $inputs+=@(Get-ChildItem -LiteralPath assets/cloth -Filter '*.cloth.json' -File | ForEach-Object {'assets/cloth/'+$_.Name})
        $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        & node tools/cloth.mjs cook --in assets/cloth --out build/cloth-gpu/cooked *> (Join-Path $directory 'cook.log')
        if($LASTEXITCODE -ne 0){throw 'Triangular cloth cook failed.'}
        & node tools/cloth-gpu-fixtures.mjs *> (Join-Path $directory 'fixtures.log')
        if($LASTEXITCODE -ne 0){throw 'GPU/legacy cloth fixtures failed.'}
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source @('shaders/cloth.hlsl') -OutputDirectory 'build/cloth-gpu'
        if($LASTEXITCODE -ne 0){throw 'GPU cloth shader compilation failed.'}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/cloth-gpu/generated')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try {& $compiler @arguments @gpuSources -o $gpuExe -ld3d12 -ldxgi -ld3dcompiler -lbcrypt -lole32 -luuid *> (Join-Path $directory 'gpu-test-compile.log');$gpuCode=$LASTEXITCODE
            & $compiler @arguments @cpuSources -o $cpuExe -lbcrypt *> (Join-Path $directory 'cpu-compile.log');$cpuCode=$LASTEXITCODE
        } finally {$ErrorActionPreference=$previous}
        if($gpuCode -ne 0 -or $cpuCode -ne 0){Get-Content (Join-Path $directory 'gpu-test-compile.log'),(Join-Path $directory 'cpu-compile.log') -Tail 30;throw 'Cloth native build failed.'}
        foreach($file in $inputs){if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hashes[$file]){throw "Cloth input changed during build: $file"}}
        $outputs=@('build/cloth-gpu/generated/nativegpu_shaders.h','build/cloth-gpu/shaders/manifest.json')
        foreach($folder in @('cooked','fixtures','legacy','source','legacy-source')){
            $outputs+=@(Get-ChildItem -LiteralPath (Join-Path $directory $folder) -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})
        }
        foreach($file in $outputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;gpuBinaryHash=(Get-FileHash -LiteralPath $gpuExe -Algorithm SHA256).Hash;cpuBinaryHash=(Get-FileHash -LiteralPath $cpuExe -Algorithm SHA256).Hash} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    function Assert-ClothInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale cloth input: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $gpuExe -Algorithm SHA256).Hash -ne $manifest.gpuBinaryHash -or (Get-FileHash -LiteralPath $cpuExe -Algorithm SHA256).Hash -ne $manifest.cpuBinaryHash){throw 'Cloth executable differs from its build manifest.'}
    }
    Assert-ClothInputs
    & node --test tests/cloth.test.mjs *> (Join-Path $directory 'authoring.log')
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'authoring.log') -Tail 35;throw 'Cloth authoring regression failed.'}
    $cpuRaw=& $cpuExe --library build/cloth-gpu/cooked/library.json
    if($LASTEXITCODE -ne 0){throw 'Triangular CPU cloth regression failed.'}
    $legacyRaw=& $cpuExe --library build/cloth-gpu/legacy/library.json
    if($LASTEXITCODE -ne 0){throw 'Legacy CPU cloth regression failed.'}
    Assert-ClothInputs
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $gpuRaw=& $gpuExe @arguments
    if($LASTEXITCODE -ne 0){throw 'GPU cloth regression failed.'}
    $cpu=$cpuRaw | ConvertFrom-Json;$legacy=$legacyRaw | ConvertFrom-Json;$gpu=$gpuRaw | ConvertFrom-Json
    if(-not $cpu.ok -or -not $legacy.ok -or -not $gpu.ok){throw 'Cloth report is not successful.'}
    Assert-ClothInputs
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'cloth-gpu'
    $report=Join-Path $context.directory 'cloth-gpu-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;cpu=$cpu;legacyCpu=$legacy;gpu=$gpu;authoring='tests/cloth.test.mjs';regression='incremental: cloth asset/cook, triangular CPU/GPU solver and legacy CPU compatibility';scope='Colored triangular membrane/dihedral XPBD, normals/tangents, static contacts, read-only shared wind, actual fence retention; host drawing, bones, self collision and CCD remain pending'} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $report -Encoding utf8
    foreach($file in @($gpuExe,$cpuExe,$manifestPath,(Join-Path $directory 'gpu-test-compile.log'),(Join-Path $directory 'cpu-compile.log'),(Join-Path $directory 'authoring.log'),(Join-Path $directory 'cook.log'),(Join-Path $directory 'fixtures.log'))){Copy-Item -LiteralPath $file -Destination $context.directory}
    $snapshot=Join-Path $context.directory 'source-snapshot'
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $snapshot $entry.Name;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination
        if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw "Cloth frozen input differs: $($entry.Name)"}}
    Assert-ClothInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('cloth-gpu-evidence.json') | Out-Null
    Write-Host "Cloth GPU: $($gpu.groups) groups / $($gpu.checks) checks; CPU triangle/legacy: $($cpu.checks)/$($legacy.checks); $report"
} finally {Pop-Location}

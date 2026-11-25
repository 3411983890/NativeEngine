param([switch]$SkipBuild,[string]$Adapter='')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/fields-gpu'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_field_gpu_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    if(-not $SkipBuild){
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source 'shaders/field_query.hlsl' -OutputDirectory 'build/fields-gpu'
        if($LASTEXITCODE -ne 0){throw 'Field GPU shader compilation failed.'}
        $inputs=@('engine/native/fields/gpu_storage.hpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.hpp','engine/native/fields/world.cpp','engine/native/fields/local.hpp','engine/native/fields/local.cpp','engine/native/determinism.hpp','engine/native/determinism.cpp','tests/native_field_gpu.cpp','third_party/json.hpp','build/fields-gpu/generated/nativegpu_shaders.h','shaders/field_query.hlsl','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','shaders/manifest.json','.incrementalTesting/simulation/fields-gpu.ps1','tools/compile-shaders.ps1')
        $hashes=[ordered]@{}
        foreach($inputFile in $inputs){$hashes[$inputFile]=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash}
        $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
        & $compiler -std=c++17 -Wall -Wextra -Wpedantic -O2 -static -DNOMINMAX -Ibuild/fields-gpu/generated tests/native_field_gpu.cpp engine/native/fields/gpu_storage.cpp engine/native/fields/world.cpp engine/native/fields/local.cpp engine/native/determinism.cpp -o $exe -ld3d12 -ldxgi -ld3dcompiler -lole32 -luuid *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 45;throw 'Field GPU native compilation failed.'}
        foreach($inputFile in $inputs){if((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash -ne $hashes[$inputFile]){throw "GPU field input changed during compilation: $inputFile"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale GPU field binary: $($entry.Name)"}}
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'GPU field binary differs from build manifest.'}
    $arguments=@()
    if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw 'GPU field checks failed.'}
    $result=$raw | ConvertFrom-Json
    if(-not $result.ok){throw 'GPU field report is not successful.'}
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'fields-gpu'
    $report=Join-Path $context.directory 'fields-gpu-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;results=$result;scope='Real D3D12 immutable sparse read snapshots; CPU projection, lattice and Q8 trilinear sampling with budgeted halos; renderer and GPU solver writers pending'} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding utf8
    Register-Report -Context $context -Path $report -LegacyAliases @('fields-gpu-evidence.json') | Out-Null
    Write-Host "GPU fields: $($result.groups) groups, $($result.checks) checks on $($result.adapter); $report"
} finally {Pop-Location}

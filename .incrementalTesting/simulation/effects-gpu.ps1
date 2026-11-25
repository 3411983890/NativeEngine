param([switch]$SkipBuild,[string]$Adapter='',[switch]$RuntimeOnly)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/effects-gpu'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $targets=@([ordered]@{name='pool';source='tests/native_effect_gpu.cpp';extra=@();binary='native_effect_gpu_tests.exe'},
        [ordered]@{name='runtime';source='tests/native_effect_gpu_runtime.cpp';extra=@('engine/native/effects/gpu_runtime.cpp');binary='native_effect_gpu_runtime_tests.exe'})
    if($RuntimeOnly){$targets=@($targets | Where-Object {$_.name -eq 'runtime'})}
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    $fieldSources=@('engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp')
    if(-not $SkipBuild) {
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source 'shaders/effect_pool.hlsl' -OutputDirectory 'build/effects-gpu'
        if($LASTEXITCODE -ne 0){throw 'GPU particle shader compilation failed.'}
    }
    $results=[ordered]@{}
    $builds=[ordered]@{}
    foreach($target in $targets){
      $exe=Join-Path $directory $target.binary
      $manifestPath=Join-Path $directory ($target.name+'-inputs.json')
      $compileLog=Join-Path $directory ($target.name+'-compile.log')
      if(-not $SkipBuild){
        $inputs=@('engine/native/effects/gpu_pool.hpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.hpp','engine/native/smoke/effect_config.hpp','engine/native/smoke/configuration.hpp','engine/native/smoke/solver.hpp',
            'engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.hpp','engine/native/assets/sha256.cpp',
            'engine/native/vec3.hpp','engine/native/determinism.hpp','engine/native/determinism.cpp',$target.source,'tests/d3d12_test_rig.hpp',
            'third_party/json.hpp','shaders/effect_pool.hlsl','shaders/manifest.json','build/effects-gpu/generated/nativegpu_shaders.h','.incrementalTesting/simulation/effects-gpu.ps1','tools/compile-shaders.ps1')
        $inputs+=@($target.extra)
        $inputs+=$fieldSources+@('engine/native/fields/gpu_vector.hpp','engine/native/fields/gpu_storage.hpp','engine/native/fields/world.hpp','shaders/field_sampling.hlsli','shaders/field_constants.hlsli')
        if($target.name -eq 'runtime'){$inputs+=@('engine/native/effects/gpu_runtime.hpp')}
        $hashes=[ordered]@{}
        foreach($inputFile in $inputs){$hashes[$inputFile]=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash}
        $compileArguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/effects-gpu/generated',$target.source,
            'engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp')+@($target.extra)+$fieldSources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        & $compiler @compileArguments *> $compileLog
        if($LASTEXITCODE -ne 0){Get-Content $compileLog -Tail 30;throw "GPU particle $($target.name) compilation failed."}
        foreach($inputFile in $inputs){if((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash -ne $hashes[$inputFile]){throw "GPU particle input changed during build: $inputFile"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
      }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale GPU particle input: $($entry.Name)"}}
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'GPU particle binary differs from build manifest.'}
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw "GPU particle $($target.name) checks failed."}
    $raw | Set-Content -LiteralPath (Join-Path $directory ($target.name+'-results.json')) -Encoding utf8
    $result=$raw | ConvertFrom-Json
    if(-not $result.ok){throw 'GPU particle report is not successful.'}
    foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "GPU particle input changed during regression: $($entry.Name)"}}
    $builds[$target.name]=$manifest
    $results[$target.name]=$result
    }
    foreach($name in $builds.Keys){
        foreach($entry in $builds[$name].sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "GPU particle input changed between targets: $($entry.Name)"}}
    }
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'effects-gpu'
    $report=Join-Path $context.directory 'effects-gpu-evidence.json'
    [ordered]@{timestamp=$context.timestamp;builds=$builds;results=$results;regression='incremental: effect pool/runtime only';shaderSource='shaders/effect_pool.hlsl';
        scope='Real D3D12 sprite SoA pool and visual asset/instance lifecycle with shared GPU admission ledgers and bounded async telemetry; drawing and read-only velocity fields have separate incremental tests; GPU reload, events and collision pending'} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $report -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $directory 'shaders/manifest.json') -Destination (Join-Path $context.directory 'effects-shaders.json')
    foreach($target in $targets){Copy-Item -LiteralPath (Join-Path $directory $target.binary) -Destination $context.directory;Copy-Item -LiteralPath (Join-Path $directory ($target.name+'-compile.log')) -Destination $context.directory;Copy-Item -LiteralPath (Join-Path $directory ($target.name+'-results.json')) -Destination $context.directory}
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-gpu-evidence.json') | Out-Null
    foreach($name in $results.Keys){$result=$results[$name];Write-Host "GPU effects ${name}: $($result.groups) groups, $($result.checks) checks on $($result.adapter)"}
    Write-Host $report
} finally {Pop-Location}

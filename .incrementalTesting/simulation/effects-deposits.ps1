param([switch]$SkipBuild,[string]$Adapter='AMD')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/effects-deposits';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_effect_deposits_tests.exe';$manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        $sources=@('tests/native_effect_deposits.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/world.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp')
        & node tools/effect-deposit-fixtures.mjs | Out-Null
        if($LASTEXITCODE -ne 0){throw 'Field deposit fixture cook failed.'}
        $inputs=$sources+@('tools/effect-deposit-fixtures.mjs','engine/effects/asset.mjs','engine/assets/canonical.mjs','tests/effect-deposits.test.mjs','build/effects-deposits/fixture.effect.json','build/effects-deposits/fixture.cooked.json','.incrementalTesting/simulation/effects-deposits.ps1','tools/compile-shaders.ps1','tools/report-lib.ps1','shaders/manifest.json','shaders/field_world.hlsl','shaders/field_query.hlsl','shaders/field_local.hlsl','shaders/effect_pool.hlsl','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','VERSION')
        $queue=[System.Collections.Generic.Queue[string]]::new();foreach($file in $inputs){$queue.Enqueue($file)}
        $seen=@{};$hashes=[ordered]@{}
        while($queue.Count){$file=$queue.Dequeue();if($seen.ContainsKey($file)){continue};$seen[$file]=$true;$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if([IO.Path]::GetExtension($file) -notin @('.cpp','.hpp','.h')){continue}
            foreach($match in [regex]::Matches((Get-Content -LiteralPath $file -Raw -Encoding UTF8),'#include\s+"([^"]+)"')){
                $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $root $file)) $match.Groups[1].Value))
                if($absolute.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $absolute)){$queue.Enqueue($absolute.Substring($root.Length+1).Replace('\','/'))}
            }
        }
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source @('shaders/field_world.hlsl','shaders/field_query.hlsl','shaders/field_local.hlsl','shaders/effect_pool.hlsl') -OutputDirectory build/effects-deposits
        if($LASTEXITCODE -ne 0){throw 'Effect field deposits shaders failed.'}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/effects-deposits/generated')+$sources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @arguments *> (Join-Path $directory 'compile.log');$code=$LASTEXITCODE}finally{$ErrorActionPreference=$previous}
        if($code -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 30;throw 'Effect field deposits build failed.'}
        foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Effect field deposits input changed during build: $($entry.Key)"}}
        foreach($file in @('build/effects-deposits/generated/nativegpu_shaders.h','build/effects-deposits/shaders/manifest.json')){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    function Assert-DepositInputs {foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale Effect field deposits input: $($entry.Name)"}};if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Effect field deposits binary mismatch.'}}
    Assert-DepositInputs
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments 2> (Join-Path $directory 'runtime.stderr.log')
    if($LASTEXITCODE -ne 0){throw 'Effect field deposits numeric/lifetime tests failed.'}
    $result=$raw | ConvertFrom-Json;if(-not $result.ok -or -not $result.debugLayer -or $result.debugErrors -ne 0){throw 'Effect field deposits hardware validation report failed.'}
    Assert-DepositInputs
    . tools/report-lib.ps1;$context=New-ReportContext -Mode 'effects-deposits';$report=Join-Path $context.directory 'effects-deposits-evidence.json'
    [ordered]@{schema='nativegpu.effects-deposits-report/1';ok=$true;build=$manifest;results=$result;fullRegressionRun=$false;scope='Incremental authenticated fieldDeposit asset, GPU instance post-spawn writer contract, real LEF/WF integration, paused/retired source lifecycle and driver failure isolation. Test coordinator only; renderer driver/asset lifecycle integration pending'} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $report -Encoding UTF8
    $raw | Set-Content -LiteralPath (Join-Path $context.directory 'runtime.stdout.log') -Encoding UTF8
    Copy-Item -LiteralPath $exe,$manifestPath,(Join-Path $directory 'compile.log'),(Join-Path $directory 'runtime.stderr.log') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Effect field deposits frozen source mismatch.'}}
    Assert-DepositInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-deposits-evidence.json') | Out-Null
    Write-Host "Effect field deposits: $($result.groups) groups / $($result.checks) checks / real GPU pool / LEF / WF checks; $report"
}finally{Pop-Location}

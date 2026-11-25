param([switch]$SkipBuild,[switch]$SkipDrawBuild,[string]$Adapter='AMD')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/cloth-batch';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $drawArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root '.incrementalTesting/simulation/cloth-draw.ps1'),'-Adapter',$Adapter,'-Receipt','build/cloth-batch/draw-receipt.json');if($SkipBuild -or $SkipDrawBuild){$drawArguments+='-SkipBuild'}
    & powershell @drawArguments
    if($LASTEXITCODE -ne 0){throw 'Affected cloth solve/draw regressions failed.'}
    $receipt=Get-Content build/cloth-batch/draw-receipt.json -Raw -Encoding UTF8 | ConvertFrom-Json
    if((Get-FileHash -LiteralPath $receipt.report -Algorithm SHA256).Hash -ne $receipt.sha256){throw 'Cloth draw receipt mismatch.'}
    $draw=Get-Content -LiteralPath $receipt.report -Raw -Encoding UTF8 | ConvertFrom-Json
    $exe=Join-Path $directory 'native_cloth_batch_tests.exe';$manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        $hashes=[ordered]@{};foreach($entry in $draw.build.sources.PSObject.Properties){$hashes[$entry.Name]=$entry.Value}
        foreach($file in @('tests/native_cloth_batch.cpp','engine/native/cloth/batch_gpu.cpp','engine/native/cloth/batch_gpu.hpp','tools/cloth-grouped-fixtures.mjs','.incrementalTesting/simulation/cloth-batch.ps1')){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        & node tools/cloth-grouped-fixtures.mjs *> (Join-Path $directory 'fixtures.log')
        if($LASTEXITCODE -ne 0){throw 'Cloth batch boundary fixtures failed.'}
        foreach($folder in @('fixtures','cooked','source')){foreach($file in (Get-ChildItem -LiteralPath (Join-Path $root ('build/cloth-grouped/'+$folder)) -Recurse -File)){$relative=$file.FullName.Substring($root.Length+1).Replace('\','/');$hashes[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}}
        $sources=@('tests/native_cloth_batch.cpp','engine/native/cloth/batch_gpu.cpp','engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp')
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/cloth-draw/generated')+$sources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @arguments *> (Join-Path $directory 'compile.log');$code=$LASTEXITCODE}finally{$ErrorActionPreference=$previous}
        if($code -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 30;throw 'Cloth batch build failed.'}
        foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Cloth batch input changed during build: $($entry.Key)"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    function Assert-BatchInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale cloth batch input: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Cloth batch binary differs from manifest.'}
    }
    Assert-BatchInputs
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw 'Cloth batch equivalence/lifetime checks failed.'}
    $result=$raw | ConvertFrom-Json;if(-not $result.ok){throw 'Cloth batch report not successful.'}
    Assert-BatchInputs
    . (Join-Path $root 'tools/report-lib.ps1');$context=New-ReportContext -Mode 'cloth-batch';$report=Join-Path $context.directory 'cloth-batch-evidence.json'
    [ordered]@{schema='nativegpu.cloth-batch-report/1';timestamp=$context.timestamp;ok=$true;build=$manifest;results=$result;affectedDrawReport=$receipt.report;affectedDrawHash=$receipt.sha256;affectedSolver=$draw.solver;affectedDraw=$draw.draw;scope='Incremental cloth batching and affected GPU solver/draw; not full regression or combined A-G performance acceptance'} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $report -Encoding UTF8
    Copy-Item -LiteralPath $exe,$manifestPath,(Join-Path $directory 'compile.log'),(Join-Path $directory 'fixtures.log') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Frozen cloth batch input mismatch.'}}
    Assert-BatchInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('cloth-batch-evidence.json') | Out-Null
    Write-Host "Cloth batch: $($result.groups) groups / $($result.checks) checks / $($result.vertexComparisons) exact comparisons; $report"
}finally{Pop-Location}

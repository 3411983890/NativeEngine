param([switch]$SkipBuild,[string]$Adapter='AMD',[string]$Receipt='')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/shared-wind';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_shared_wind_tests.exe';$manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        & node tools/simulation-fields-fixtures.mjs *> (Join-Path $directory 'fixtures.log')
        if($LASTEXITCODE -ne 0){throw 'Shared wind fixtures failed.'}
        $sources=@('tests/native_shared_wind.cpp','host/simulation_fields.cpp','engine/native/fields/vector_source.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp')
        $inputs=$sources+@('.incrementalTesting/simulation/shared-wind.ps1','tools/simulation-fields-fixtures.mjs','build/simulation-fields/fixed.field.json','build/simulation-fields/authority.field.json','build/simulation-fields/flow.field.json')
        $queue=[System.Collections.Generic.Queue[string]]::new();foreach($file in $inputs){$queue.Enqueue($file)}
        $seen=@{};$hashes=[ordered]@{}
        while($queue.Count){$file=$queue.Dequeue();if($seen.ContainsKey($file)){continue};$seen[$file]=$true;$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if([IO.Path]::GetExtension($file) -notin @('.cpp','.hpp','.h')){continue}
            foreach($match in [regex]::Matches((Get-Content -LiteralPath $file -Raw -Encoding UTF8),'#include\s+"([^"]+)"')){
                $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $root $file)) $match.Groups[1].Value))
                if($absolute.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $absolute)){$queue.Enqueue($absolute.Substring($root.Length+1).Replace('\','/'))}
            }
        }
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX')+$sources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @arguments *> (Join-Path $directory 'compile.log');$code=$LASTEXITCODE}finally{$ErrorActionPreference=$previous}
        if($code -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 30;throw 'Shared wind source build failed.'}
        foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Shared wind input changed during build: $($entry.Key)"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    function Assert-WindInputs {foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale shared wind input: $($entry.Name)"}};if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Shared wind binary mismatch.'}}
    Assert-WindInputs
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw 'Shared wind source clock/lease tests failed.'}
    $result=$raw | ConvertFrom-Json;if(-not $result.ok){throw 'Shared wind report failed.'}
    Assert-WindInputs
    . tools/report-lib.ps1;$context=New-ReportContext -Mode 'shared-wind';$report=Join-Path $context.directory 'shared-wind-evidence.json'
    [ordered]@{schema='nativegpu.shared-wind-report/1';ok=$true;build=$manifest;results=$result;fullRegressionRun=$false;scope='Incremental source subscriptions, duplicate-frame reuse, monotone clocks, real GPU reader snapshots, recovery and failed-publication retry'} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding UTF8
    Copy-Item -LiteralPath $exe,$manifestPath,(Join-Path $directory 'compile.log'),(Join-Path $directory 'fixtures.log') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Shared wind frozen source mismatch.'}}
    Assert-WindInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('shared-wind-evidence.json') | Out-Null
    if($Receipt){[ordered]@{report=$report;sha256=(Get-FileHash -LiteralPath $report -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content -LiteralPath $Receipt -Encoding UTF8}
    Write-Host "Shared wind: $($result.checks) checks; $report"
}finally{Pop-Location}

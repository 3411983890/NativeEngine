param([switch]$SkipBuild,[string]$Adapter='')
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/smoke-particles';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_smoke_particles_tests.exe';$manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        $sources=@('tests/native_smoke_particles.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/effects/gpu_pool.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp')
        $inputs=$sources+@('shaders/smoke.hlsl','shaders/smoke_particles.hlsl','shaders/effect_pool.hlsl','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','shaders/manifest.json','.incrementalTesting/simulation/smoke-particles.ps1','tools/compile-shaders.ps1','tools/report-lib.ps1','VERSION')
        $queue=[System.Collections.Generic.Queue[string]]::new();foreach($file in $inputs){$queue.Enqueue($file)}
        $seen=@{};$hashes=[ordered]@{}
        while($queue.Count){$file=$queue.Dequeue();if($seen.ContainsKey($file)){continue};$seen[$file]=$true;$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if([IO.Path]::GetExtension($file) -notin @('.cpp','.hpp','.h')){continue}
            foreach($match in [regex]::Matches((Get-Content -LiteralPath $file -Raw -Encoding UTF8),'#include\s+"([^"]+)"')){
                $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $root $file)) $match.Groups[1].Value))
                if($absolute.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $absolute)){$queue.Enqueue($absolute.Substring($root.Length+1).Replace('\','/'))}
            }
        }
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source @('shaders/smoke.hlsl','shaders/smoke_particles.hlsl','shaders/effect_pool.hlsl') -OutputDirectory build/smoke-particles
        if($LASTEXITCODE -ne 0){throw 'Smoke shader compilation failed.'}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-ffp-contract=off','-Ibuild/smoke-particles/generated')+$sources+@('-o',$exe,'-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @arguments *> (Join-Path $directory 'compile.log');$code=$LASTEXITCODE}finally{$ErrorActionPreference=$previous}
        if($code -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 35;throw 'Particle smoke source build failed.'}
        foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Smoke input changed during compilation: $($entry.Key)"}}
        foreach($file in @('build/smoke-particles/generated/nativegpu_shaders.h','build/smoke-particles/shaders/manifest.json')){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale smoke input: $($entry.Name)"}}
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Smoke binary does not match its manifest.'}
    . tools/report-lib.ps1;$context=New-ReportContext -Mode 'smoke-particles'
    Copy-Item -LiteralPath $exe,$manifestPath,(Join-Path $directory 'compile.log') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Smoke source changed before freeze.'}}
    $frozenExe=Join-Path $context.directory 'native_smoke_particles_tests.exe';if((Get-FileHash -LiteralPath $frozenExe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Frozen smoke binary mismatch.'}
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{$raw=& $frozenExe @arguments 2> (Join-Path $context.directory 'runtime.stderr.log');$code=$LASTEXITCODE}finally{$ErrorActionPreference=$previous}
    $raw | Set-Content -LiteralPath (Join-Path $context.directory 'runtime.stdout.log') -Encoding UTF8
    $result=$null;if($code -eq 0){$result=$raw | ConvertFrom-Json}
    $ok=$code -eq 0 -and $result.ok -and $result.debugLayer -and $result.debugErrors -eq 0
    $changes=@();foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$changes+=$entry.Name}}
    $report=Join-Path $context.directory 'smoke-particles-evidence.json'
    [ordered]@{schema='nativegpu.smoke-particles-report/1';ok=[bool]$ok;build=$manifest;results=$result;exit=$code;buildSnapshotFrozenBeforeRun=$true;sourceChangesAfterFreeze=$changes;fullRegressionRun=$false;scope='Incremental GPU alive-pool to immutable physical smoke source rates, CPU/GPU history comparison, local/world transforms, known-zero source, outside-grid losses, overflow/rate/channel/grid/timeline/byte/frame rejection, multi-substep integration and actual queue producer-consumer lifetimes. No renderer/WF/froxel or full D/A-G acceptance.'} | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $report -Encoding UTF8
    Register-Report -Context $context -Path $report -LegacyAliases @('smoke-particles-evidence.json') | Out-Null
    if(-not $ok){Get-Content (Join-Path $context.directory 'runtime.stderr.log') -Tail 40;throw "Particle smoke source validation failed; frozen report: $report"}
    Write-Host "Particle smoke source: $($result.groups) groups / $($result.checks) checks / $($result.comparisons) CPU-GPU scalar comparisons; $report"
}finally{Pop-Location}

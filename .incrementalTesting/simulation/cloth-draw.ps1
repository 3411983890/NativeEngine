param([switch]$SkipBuild,[string]$Adapter='AMD',[string]$Receipt='')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/cloth-draw';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $drawExe=Join-Path $directory 'native_cloth_draw_tests.exe';$solverExe=Join-Path $directory 'native_cloth_gpu_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json';$compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        $common=@('engine/native/cloth/solver_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp','engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp')
        $drawSources=@('tests/native_cloth_draw.cpp','host/cloth_preview.cpp','renderer/gpu_cloth_draw.cpp','renderer/matrix_math.cpp','renderer/png_writer.cpp')+$common
        $solverSources=@('tests/native_cloth_gpu.cpp','engine/native/cloth/solver_cpu.cpp')+$common
        $inputs=@($drawSources+$solverSources+@('shaders/cloth.hlsl','shaders/cloth_constraints.hlsli','shaders/cloth_dynamics.hlsli','shaders/cloth_grouped.hlsli','shaders/cloth_normals.hlsli','shaders/cloth_batch_bindings.hlsli','shaders/cloth_draw.hlsl','shaders/field_sampling.hlsli','shaders/field_constants.hlsli','shaders/manifest.json',
            '.incrementalTesting/simulation/cloth-draw.ps1','tools/compile-shaders.ps1','tools/cloth.mjs','tools/cloth-gpu-fixtures.mjs','tools/report-lib.ps1','engine/cloth/asset.mjs','engine/cloth/topology.mjs','engine/assets/canonical.mjs','VERSION') | Sort-Object -Unique)
        # Follow quoted C++ headers rather than hashing unrelated source trees.
        $queue=[System.Collections.Generic.Queue[string]]::new();foreach($file in $inputs){$queue.Enqueue($file)}
        $seen=@{};$hashes=[ordered]@{}
        while($queue.Count){$file=$queue.Dequeue();if($seen.ContainsKey($file)){continue};$seen[$file]=$true;$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if([IO.Path]::GetExtension($file) -notin @('.cpp','.hpp','.h')){continue}
            foreach($match in [regex]::Matches((Get-Content -LiteralPath $file -Raw -Encoding UTF8),'#include\s+"([^"]+)"')){
                $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $root $file)) $match.Groups[1].Value))
                if($absolute.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $absolute)){$queue.Enqueue($absolute.Substring($root.Length+1).Replace('\','/'))}
            }
        }
        foreach($file in (Get-ChildItem assets/cloth -Filter '*.cloth.json' -File)){$relative='assets/cloth/'+$file.Name;$hashes[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
        & node tools/cloth.mjs cook --in assets/cloth --out build/cloth-draw/cooked *> (Join-Path $directory 'cook.log');if($LASTEXITCODE -ne 0){throw 'Cloth draw asset cook failed.'}
        & node tools/cloth-gpu-fixtures.mjs build/cloth-draw *> (Join-Path $directory 'fixtures.log');if($LASTEXITCODE -ne 0){throw 'Cloth draw fixture cook failed.'}
        & (Join-Path $root 'tools/compile-shaders.ps1') -RequireDxc -Quiet -Source @('shaders/cloth.hlsl','shaders/cloth_draw.hlsl') -OutputDirectory build/cloth-draw
        if($LASTEXITCODE -ne 0){throw 'Cloth draw shader build failed.'}
        $flags=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX','-Ibuild/cloth-draw/generated');$libs=@('-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid')
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @flags @drawSources -o $drawExe @libs *> (Join-Path $directory 'compile.log');$drawCode=$LASTEXITCODE
            & $compiler @flags @solverSources -o $solverExe @libs *> (Join-Path $directory 'solver-compile.log');$solverCode=$LASTEXITCODE
        }finally{$ErrorActionPreference=$previous}
        if($drawCode -ne 0 -or $solverCode -ne 0){Get-Content (Join-Path $directory 'compile.log'),(Join-Path $directory 'solver-compile.log') -Tail 30;throw 'Cloth draw native build failed.'}
        foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Cloth draw source changed during build: $($entry.Key)"}}
        $outputs=@('build/cloth-draw/generated/nativegpu_shaders.h','build/cloth-draw/shaders/manifest.json')
        foreach($folder in @('cooked','fixtures')){$outputs+=@(Get-ChildItem (Join-Path $directory $folder) -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})}
        foreach($file in $outputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash $compiler -Algorithm SHA256).Hash;drawBinaryHash=(Get-FileHash $drawExe -Algorithm SHA256).Hash;solverBinaryHash=(Get-FileHash $solverExe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    function Assert-ClothDrawInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale cloth draw input: $($entry.Name)"}}
        if((Get-FileHash $drawExe -Algorithm SHA256).Hash -ne $manifest.drawBinaryHash -or (Get-FileHash $solverExe -Algorithm SHA256).Hash -ne $manifest.solverBinaryHash){throw 'Cloth draw binary differs from its manifest.'}
    }
    Assert-ClothDrawInputs
    $adapterArgs=@();if($Adapter){$adapterArgs=@('--adapter',$Adapter)}
    $solverRaw=& $solverExe --library build/cloth-draw/cooked/library.json --fixtures build/cloth-draw/fixtures/library.json @adapterArgs
    if($LASTEXITCODE -ne 0){throw 'Affected GPU cloth solve regression failed.'}
    $drawRaw=& $drawExe @adapterArgs
    if($LASTEXITCODE -ne 0){throw 'GPU cloth actual draw regression failed.'}
    $solver=$solverRaw | ConvertFrom-Json;$draw=$drawRaw | ConvertFrom-Json
    if(-not $solver.ok -or -not $draw.ok){throw 'Cloth draw evidence not successful.'}
    Assert-ClothDrawInputs
    . (Join-Path $root 'tools/report-lib.ps1');$context=New-ReportContext -Mode 'cloth-draw';$report=Join-Path $context.directory 'cloth-draw-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;solver=$solver;draw=$draw;scope='Incremental GPU cloth solve, actual MRT pixels/VS/PS/depth, two-sided normal/UV/tangent, real draw fence lifetime and host preview clock; no full regression or host run'} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $report -Encoding UTF8
    foreach($file in @($drawExe,$solverExe,$manifestPath,(Join-Path $directory 'compile.log'),(Join-Path $directory 'solver-compile.log'),(Join-Path $directory 'cook.log'),(Join-Path $directory 'fixtures.log'))){Copy-Item -LiteralPath $file -Destination $context.directory}
    Get-ChildItem -LiteralPath $directory -Filter '*.png' -File | Copy-Item -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Frozen cloth draw input mismatch.'}}
    Assert-ClothDrawInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('cloth-draw-evidence.json') | Out-Null
    if($Receipt){[ordered]@{report=$report;sha256=(Get-FileHash -LiteralPath $report -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content -LiteralPath $Receipt -Encoding UTF8}
    Write-Host "Cloth draw: $($draw.groups) groups/$($draw.checks) checks; affected solve $($solver.groups)/$($solver.checks); $report"
}finally{Pop-Location}

param([switch]$SkipBuild,[string]$OutputDirectory='build/effects-host')
$ErrorActionPreference='Stop'
if(-not(Get-Command Get-FileHash -ErrorAction SilentlyContinue)) {
    # npm/cmd may preserve a PowerShell 7 module path while launching Windows
    # PowerShell. Resolve its own bundled utility module before hashing inputs.
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
}
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $directory=Join-Path $root "$OutputDirectory"
    $manifestPath=Join-Path $directory 'host-inputs.json'
    $exe=Join-Path $directory 'NativeGPU.exe'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        New-Item -ItemType Directory -Force -Path $directory,(Join-Path $directory 'generated') | Out-Null
        # This builds the host dependency graph only. It does not build or run
        # the native/JS test suite, cook unrelated assets, or invoke verify.
        $inputs=Get-ChildItem -LiteralPath host,renderer,engine/native,engine/audio,third_party,shaders -Recurse -File |
            Where-Object {$_.Extension -in @('.cpp','.c','.h','.hpp','.hlsl','.hlsli')}
        $hashes=[ordered]@{}
        foreach($file in $inputs){$hashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
        foreach($file in @('VERSION','shaders/manifest.json','tools/build-effects-host.ps1','tools/compile-shaders.ps1','tools/build-gameplay.ps1')){
            $absolute=Join-Path $root $file;$hashes[$absolute]=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
        }
        $shaderCurrent=$false;$shaderToolHash=(Get-FileHash tools/compile-shaders.ps1 -Algorithm SHA256).Hash
        if((Test-Path "$OutputDirectory/shaders/manifest.json") -and (Test-Path "$OutputDirectory/generated/nativegpu_shaders.h") -and (Test-Path "$OutputDirectory/shaders/tool.sha256")){
            $shaderCache=Get-Content "$OutputDirectory/shaders/manifest.json" -Raw | ConvertFrom-Json
            $shaderCurrent=($shaderCache.sourceInputs -and -not $shaderCache.sourceSelection -and (Get-Content "$OutputDirectory/shaders/tool.sha256" -Raw).Trim() -eq $shaderToolHash)
            if($shaderCurrent){foreach($entry in $shaderCache.sourceInputs.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$shaderCurrent=$false;break}}}
        }
        if(-not $shaderCurrent){
            & (Join-Path $PSScriptRoot 'compile-shaders.ps1') -OutputDirectory "$OutputDirectory" -RequireDxc -Quiet
            if($LASTEXITCODE -ne 0){throw 'Host shader dependencies failed to compile.'}
            Set-Content -LiteralPath "$OutputDirectory/shaders/tool.sha256" -Value $shaderToolHash -Encoding ascii
        }
        $gameplayCurrent=$false
        if((Test-Path "$OutputDirectory/gameplay/inputs.json") -and (Test-Path "$OutputDirectory/gameplay/nativegpu-gameplay.a")){
            $gameplayInputs=Get-Content "$OutputDirectory/gameplay/inputs.json" -Raw | ConvertFrom-Json
            $gameplayCurrent=$true
            foreach($entry in $gameplayInputs.PSObject.Properties){if(-not(Test-Path -LiteralPath $entry.Name) -or (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$gameplayCurrent=$false;break}}
        }
        if(-not $gameplayCurrent){& (Join-Path $PSScriptRoot 'build-gameplay.ps1') -OutputDirectory "$OutputDirectory/gameplay";if($LASTEXITCODE -ne 0){throw 'Host gameplay dependency failed to compile.'}}
        $version=(Get-Content VERSION -Raw).Trim();$commit=(& git rev-parse HEAD).Trim()
        $signature=($hashes.GetEnumerator() | ForEach-Object {$_.Key+':'+$_.Value}) -join "`n"
        $hasher=[Security.Cryptography.SHA256]::Create()
        try{$fingerprint=([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($signature)))).Replace('-','').ToLowerInvariant()}finally{$hasher.Dispose()}
        @('#pragma once',"#define NATIVEGPU_VERSION `"$version`"","#define NATIVEGPU_BUILD_COMMIT `"$commit`"",'#define NATIVEGPU_BUILD_DIRTY 1',"#define NATIVEGPU_BUILD_FINGERPRINT `"$fingerprint`"") |
            Set-Content -LiteralPath (Join-Path $directory 'generated/nativegpu_version.h') -Encoding ascii
        foreach($file in @("$OutputDirectory/generated/nativegpu_shaders.h","$OutputDirectory/generated/nativegpu_version.h","$OutputDirectory/gameplay/nativegpu-gameplay.a")){
            $absolute=Join-Path $root $file;$hashes[$absolute]=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
        }
        $sources=@('host/main.cpp','host/scene_loader.cpp','host/texture_asset.cpp','host/budget.cpp','renderer/d3d12_renderer.cpp','renderer/lighting_quality.cpp','renderer/rhi.cpp','renderer/rhi_d3d12.cpp',
            'renderer/rt_shadows.cpp','renderer/mesh_bindless.cpp','renderer/particles.cpp','renderer/compute_pass.cpp','renderer/effect_sprites.cpp','renderer/gpu_effect_sprites.cpp','renderer/gpu_effect_bridge.cpp','renderer/gpu_field_bridge.cpp','renderer/matrix_math.cpp','renderer/png_writer.cpp',
            'engine/native/game.cpp','engine/native/physics.cpp','engine/native/animation.cpp','engine/native/animation_features.cpp','engine/native/animation_asset.cpp','engine/native/determinism.cpp','engine/native/geometry_class.cpp','engine/native/material_handle.cpp',
            'engine/native/effects/gpu_pool.cpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/fields/gpu_storage.cpp','engine/native/assets/sha256.cpp','engine/audio/audio.cpp','engine/audio/audio_asset.cpp','engine/audio/audio_device.cpp')
        $sources+=@('engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/world.cpp')
        $sources+=@('host/effect_fields.cpp','engine/native/fields/vector_source.cpp')
        $sources+=@('host/simulation_fields.cpp','host/cloth_preview.cpp','renderer/gpu_cloth_draw.cpp','renderer/gpu_cloth_bridge.cpp','engine/native/cloth/batch_gpu.cpp','engine/native/cloth/asset.cpp','engine/native/cloth/solver_gpu.cpp')
        $sources+=@('renderer/gpu_smoke_bridge.cpp','engine/native/smoke/solver.cpp','engine/native/smoke/gpu.cpp','engine/native/smoke/rate_view.cpp','engine/native/smoke/particle_source.cpp','engine/native/smoke/service.cpp','engine/native/smoke/sharing.cpp')
        $sources+=@('engine/native/effects/field_deposits.cpp','engine/native/fields/gpu_local.cpp','engine/native/fields/local.cpp','engine/native/fields/gpu_world.cpp','engine/native/fields/gpu_sharing.cpp')
        $sources+=@('engine/native/fields/effect_services.cpp')
        $sources+=@('renderer/vegetation_draw.cpp','renderer/hzb.cpp','engine/native/vegetation/asset.cpp','engine/native/vegetation/world.cpp','engine/native/assets/package.cpp')
        $sources+=@('renderer/hair_draw.cpp','engine/native/hair/gpu_pack.cpp')
        $sources+=@('engine/native/water/body.cpp','engine/native/water/asset.cpp','engine/native/water/coupling.cpp','renderer/water_draw.cpp','renderer/water_ripple.cpp','renderer/waterfall_draw.cpp','engine/native/water/transfer.cpp')
        $flags=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-DNOMINMAX','-DUNICODE','-D_UNICODE',"-I$OutputDirectory/generated")
        $compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash
        $cacheKey=$compilerHash+($flags -join ' ')
        $objects=@();$compiled=0;$reused=0;$objectDirectory=Join-Path $directory 'objects'
        New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
        Set-Content -LiteralPath (Join-Path $directory 'compile.log') -Value '' -Encoding utf8
        foreach($source in ($sources | Select-Object -Unique)){
            $sourceFlags=$flags;$unitCacheKey=$cacheKey;if($source.StartsWith('engine/native/smoke/')){$sourceFlags=$flags+@('-ffp-contract=off');$unitCacheKey+=' smoke-strict'}
            $object=Join-Path $objectDirectory (($source -replace '[/\\]','_')+'.o');$dependency=$object+'.d';$cacheFile=$object+'.json';$current=$false
            if((Test-Path -LiteralPath $object) -and (Test-Path -LiteralPath $cacheFile)){
                $cache=Get-Content -LiteralPath $cacheFile -Raw | ConvertFrom-Json
                $current=$cache.key -eq $unitCacheKey -and (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash -eq $cache.objectHash
                if($current){foreach($entry in $cache.sources.PSObject.Properties){if(-not(Test-Path -LiteralPath $entry.Name) -or (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$current=$false;break}}}
            }
            if(-not $current){
                $previousPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
                try{& $compiler @sourceFlags -MMD -MF $dependency -MT object -c $source -o $object *> ($object+'.log');$compileExit=$LASTEXITCODE}finally{$ErrorActionPreference=$previousPreference}
                if($compileExit -ne 0){Get-Content ($object+'.log') -Tail 60;throw "Effect host compilation failed: $source"}
                $dependencyText=((Get-Content -LiteralPath $dependency -Raw) -replace '\\\r?\n',' ') -replace '^object:\s*',''
                $objectInputs=[ordered]@{}
                foreach($item in ($dependencyText.Trim() -split '(?<!\\)\s+')){
                    $item=$item -replace '\\ ', ' ' -replace '\\:', ':'
                    $absolute=if([IO.Path]::IsPathRooted($item)){[IO.Path]::GetFullPath($item)}else{[IO.Path]::GetFullPath((Join-Path $root $item))}
                    $objectInputs[$absolute]=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
                }
                [ordered]@{key=$unitCacheKey;sources=$objectInputs;objectHash=(Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $cacheFile -Encoding utf8
                Add-Content -LiteralPath (Join-Path $directory 'compile.log') -Value (Get-Content ($object+'.log') -Raw)
                ++$compiled
            }else{++$reused}
            $objects+=$object
        }
        $pendingExe=Join-Path $directory ('NativeGPU.pending-'+[guid]::NewGuid().ToString('N')+'.exe')
        $linkArguments=@('-static')+$objects+@("$OutputDirectory/gameplay/nativegpu-gameplay.a",'-municode','-mwindows','-o',$pendingExe,'-ld3d12','-ldxgi','-ld3dcompiler','-luser32','-lgdi32','-lcomdlg32','-lshell32','-lole32','-luuid','-lwinmm','-lbcrypt')
        $previousPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& $compiler @linkArguments *> (Join-Path $directory 'link.log');$linkExit=$LASTEXITCODE}finally{$ErrorActionPreference=$previousPreference}
        if($linkExit -ne 0){Get-Content (Join-Path $directory 'link.log') -Tail 60;throw 'Effect host link failed.'}
        foreach($inputFile in $hashes.Keys){if((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash -ne $hashes[$inputFile]){throw "Host input changed during compilation: $inputFile"}}
        # A concurrent edit must not overwrite the last checked executable.
        $pendingManifest=Join-Path $directory ('host-inputs.pending-'+[guid]::NewGuid().ToString('N')+'.json')
        [ordered]@{scope='host executable and required dependencies only; no regression executed';compiledUnits=$compiled;reusedUnits=$reused;sources=$hashes;binaryHash=(Get-FileHash -LiteralPath $pendingExe -Algorithm SHA256).Hash;compilerHash=$compilerHash} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $pendingManifest -Encoding utf8
        Move-Item -LiteralPath $pendingExe -Destination $exe -Force
        Move-Item -LiteralPath $pendingManifest -Destination $manifestPath -Force
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach($inputFile in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $inputFile.Name -Algorithm SHA256).Hash -ne $inputFile.Value){throw "Stale effect host input: $($inputFile.Name)"}}
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Effect host executable does not match its manifest.'}
    Write-Host "Effect host current: $exe (no tests run)"
}finally{Pop-Location}

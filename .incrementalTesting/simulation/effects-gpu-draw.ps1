param([switch]$SkipBuild,[switch]$WithHost,[string]$Adapter='AMD')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/effects-draw'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_effect_gpu_draw_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        & (Join-Path $root 'tools/compile-shaders.ps1') -Source @('shaders/effect_pool.hlsl','shaders/gpu_effect_sprites.hlsl') -OutputDirectory 'build/effects-draw' -RequireDxc -Quiet
        if($LASTEXITCODE -ne 0){throw 'GPU sprite shader compilation failed.'}
        $inputs=@('tests/native_effect_gpu_draw.cpp','tests/d3d12_test_rig.hpp','renderer/gpu_effect_sprites.cpp','renderer/gpu_effect_sprites.hpp','renderer/matrix_math.cpp','renderer/matrix_math.h',
            'engine/native/effects/gpu_pool.cpp','engine/native/effects/gpu_pool.hpp','engine/native/effects/gpu_runtime.cpp','engine/native/effects/gpu_runtime.hpp','engine/native/effects/runtime.cpp','engine/native/effects/runtime.hpp',
            'engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/assets/sha256.hpp','engine/native/determinism.cpp','engine/native/determinism.hpp','engine/native/vec3.hpp','third_party/json.hpp',
            'shaders/effect_pool.hlsl','shaders/gpu_effect_sprites.hlsl','shaders/manifest.json','build/effects-draw/generated/nativegpu_shaders.h','.incrementalTesting/simulation/effects-gpu-draw.ps1','tools/compile-shaders.ps1')
        $inputs+=@('engine/native/fields/gpu_vector.cpp','engine/native/fields/gpu_vector.hpp','engine/native/fields/gpu_storage.cpp','engine/native/fields/gpu_storage.hpp','engine/native/fields/world.cpp','engine/native/fields/world.hpp','shaders/field_sampling.hlsli','shaders/field_constants.hlsli')
        $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        & $compiler -std=c++17 -Wall -Wextra -Wpedantic -O2 -static -DNOMINMAX -Ibuild/effects-draw/generated tests/native_effect_gpu_draw.cpp renderer/gpu_effect_sprites.cpp renderer/matrix_math.cpp engine/native/effects/gpu_pool.cpp engine/native/effects/gpu_runtime.cpp engine/native/effects/runtime.cpp engine/native/effects/asset.cpp engine/native/assets/sha256.cpp engine/native/determinism.cpp engine/native/fields/gpu_vector.cpp engine/native/fields/gpu_storage.cpp engine/native/fields/world.cpp -o $exe -ld3d12 -ldxgi -ld3dcompiler -lbcrypt -lole32 -luuid *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 35;throw 'GPU sprite test build failed.'}
        foreach($file in $inputs){if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hashes[$file]){throw "GPU sprite input changed during compilation: $file"}}
        [ordered]@{sources=$hashes;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    function Assert-DrawInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale GPU sprite input: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'GPU sprite test binary changed.'}
    }
    Assert-DrawInputs
    $arguments=@();if($Adapter){$arguments=@('--adapter',$Adapter)}
    $raw=& $exe @arguments
    if($LASTEXITCODE -ne 0){throw 'GPU sprite draw regression failed.'}
    $result=$raw | ConvertFrom-Json
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'effects-gpu-draw'
    $checks=[System.Collections.Generic.List[object]]::new()
    function Add-Check([string]$name,[bool]$ok,[string]$detail){$checks.Add([ordered]@{name=$name;ok=$ok;detail=$detail});if(-not $ok){Write-Host "FAIL: $name ($detail)"}}
    Add-Check 'GPU gather/sort/actual pixel regression' ([bool]$result.ok) "$($result.groups) groups / $($result.checks) checks"
    $runs=[ordered]@{}
    if($WithHost){
        & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild:$SkipBuild
        if($LASTEXITCODE -ne 0){throw 'Effect host dependencies failed.'}
        & node tools/effects-runtime-fixtures.mjs
        if($LASTEXITCODE -ne 0){throw 'Effect blend fixtures failed.'}
        & node tools/effects.mjs cook --in assets/effects --out build/effects-draw/host-cooked *> (Join-Path $directory 'host-cook.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'host-cook.log') -Tail 20;throw 'Scoped host effect cook failed.'}
        $hostAssetInputs=[ordered]@{}
        $hostAssetFiles=@(Get-ChildItem -LiteralPath assets/effects,build/effects-draw/host-cooked,build/effects-validation/cooked -Recurse -File | Where-Object {$_.Extension -in @('.json','.ngtex')})
        $catalogue=Get-Content -LiteralPath build/effects-draw/host-cooked/library.json -Raw | ConvertFrom-Json
        foreach($entry in $catalogue.dependencies.PSObject.Properties){$hostAssetFiles+=Get-Item -LiteralPath (Join-Path 'assets' $entry.Name)}
        foreach($file in $hostAssetFiles){$relative=$file.FullName.Substring($root.Length+1);$hostAssetInputs[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
        function Assert-HostAssets {foreach($entry in $hostAssetInputs.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Host effect input changed during regression: $($entry.Key)"}}}
        function Run-Host([string]$name,[string[]]$extra,[bool]$stress=$false,[bool]$capture=$true){
            $reportPath=Join-Path $context.directory ($name+'.json');$imagePath=Join-Path $context.directory ($name+'.png')
            $args=@('--smoke','--validation','--no-watch','--audio','off','--frames','240','--render-size','640x360','--camera','5,3,8,0,1,0','--report',(Get-RelativePath $reportPath))+$extra
            if($extra -notcontains '--effect-library'){$args+=@('--effect-library','build/effects-draw/host-cooked/library.json')}
            if(-not $stress){$args+='--no-stress'}
            if($capture){$args+=@('--screenshot',(Get-RelativePath $imagePath),'--screenshot-frame','30')}
            $process=Start-Process -FilePath (Join-Path $root 'build/effects-host/NativeGPU.exe') -ArgumentList $args -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
            if(-not(Test-Path -LiteralPath $reportPath)){throw "Missing $name report, exit=$($process.ExitCode)"}
            $data=Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            $imageHash=if(Test-Path -LiteralPath $imagePath){(Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash}else{''}
            $runs[$name]=[ordered]@{exit=$process.ExitCode;report=(Get-RelativePath $reportPath);image=(Get-RelativePath $imagePath);imageHash=$imageHash;data=$data}
        }
        Run-Host 'off' @()
        Run-Host 'cpu' @('--effect','sprite_demo','--effect-count','4')
        Run-Host 'gpu' @('--effect','sprite_demo','--effect-backend','gpu','--effect-count','4')
        Run-Host 'stopped' @('--effect','sprite_demo','--effect-backend','gpu','--effect-count','4','--effect-stop-tick','10')
        foreach($blend in @('premultiplied','cutout')){Run-Host $blend @('--effect',"validation_$blend",'--effect-backend','gpu','--effect-count','4','--effect-library','build/effects-validation/cooked/library.json')}
        Run-Host 'recovery' @('--effect','sprite_demo','--effect-backend','gpu','--effect-count','4') $true $false
        Run-Host 'unsupported' @('--effect','smoke_reference','--effect-backend','gpu') $false $false
        Run-Host 'unbound-wind' @('--effect','wind_drift','--effect-backend','gpu') $false $false
        foreach($name in @('off','cpu','gpu','stopped','premultiplied','cutout','recovery')){
            Add-Check "$name host completes" ($runs[$name].exit -eq 0 -and $runs[$name].data.success) $runs[$name].data.error
            $validation=$runs[$name].data.probes.renderTargets
            Add-Check "$name actual validation layer has no errors" ($validation.validationLayer -and $validation.validationErrors -eq 0) "enabled=$($validation.validationLayer) errors=$($validation.validationErrors) warnings=$($validation.validationWarnings)"
        }
        $gpu=$runs['gpu'].data.probes.gpuEffects;$cpu=$runs['cpu'].data
        Add-Check 'host GPU simulation and indirect rendering active' ($runs['gpu'].data.effectRuntime.simulationBackend -eq 'gpu-visual' -and $gpu.available -and $gpu.frames -eq 240 -and $gpu.drawIndirects -eq 720 -and $gpu.gatherIndirects -gt 0 -and $gpu.sortIndirects -gt 0 -and $gpu.emitted -gt 0 -and $gpu.faults -eq 0) "frames=$($gpu.frames) draws=$($gpu.drawIndirects) emitted=$($gpu.emitted)"
        Add-Check 'CPU fallback still draws' ($cpu.effectRuntime.simulationBackend -eq 'cpu-reference' -and $cpu.probes.effectSprites.frames -eq 240 -and -not $cpu.probes.gpuEffects.available) 'explicit default CPU reference path'
        Add-Check 'GPU simulation bypasses CPU sprite upload' ($runs['gpu'].data.probes.effectSprites.frames -eq 0 -and $gpu.globalAlphaSorted -and -not $gpu.synchronousParticleReadback) 'direct GPU data gather and indirect draws'
        Add-Check 'GPU effect work has fenced timing and enters frame budget' ($gpu.includedInFrameGpuTime -and $gpu.timingSamples -gt 0 -and $gpu.simulateSortGpuMs -ge 0 -and $gpu.drawGpuMs -ge 0) "samples=$($gpu.timingSamples) simulation/sort=$($gpu.simulateSortGpuMs) draw=$($gpu.drawGpuMs)"
        foreach($name in @('gpu','premultiplied','cutout')){Add-Check "$name changes captured pixels" ([bool]$runs[$name].imageHash -and $runs[$name].imageHash -ne $runs['off'].imageHash) 'same camera/frame/scene'}
        Add-Check 'GPU immediate stop returns to baseline pixels' ($runs['stopped'].imageHash -eq $runs['off'].imageHash -and $runs['stopped'].data.effectRuntime.stopped -and $runs['stopped'].data.effectRuntime.alive -eq 0 -and $runs['stopped'].data.probes.gpuEffects.instances -eq 0) 'no residual sprites or instance handles'
        $recovery=$runs['recovery'].data
        Add-Check 'GPU effects resume after resize/recreation' ($recovery.rendererReinitializations -gt 0 -and $recovery.resizeEvents -gt 0 -and $recovery.probes.gpuEffects.frames -gt 0 -and $recovery.probes.gpuEffects.emitted -gt 0) "post-rebuild frames=$($recovery.probes.gpuEffects.frames)"
        Add-Check 'unsupported GPU field effect rejected' ($runs['unsupported'].exit -ne 0 -and -not $runs['unsupported'].data.success -and $runs['unsupported'].data.errorCode -eq 'NG-E-EFFECT-UNSUPPORTED') $runs['unsupported'].data.errorCode
        Add-Check 'unbound velocity field rejected by host' ($runs['unbound-wind'].exit -ne 0 -and -not $runs['unbound-wind'].data.success -and $runs['unbound-wind'].data.errorCode -eq 'NG-E-EFFECT-UNSUPPORTED') $runs['unbound-wind'].data.errorCode
        & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild
        if($LASTEXITCODE -ne 0){throw 'Host inputs changed during GPU sprite regression.'}
        Assert-HostAssets
        Copy-Item -LiteralPath build/effects-host/host-inputs.json,build/effects-host/NativeGPU.exe -Destination $context.directory
        $hostBuild=Get-Content -LiteralPath build/effects-host/host-inputs.json -Raw | ConvertFrom-Json
        foreach($entry in $hostBuild.sources.PSObject.Properties){$relative=$entry.Name.Substring($root.Length+1);$destination=Join-Path (Join-Path $context.directory 'host-source-snapshot') $relative;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
        foreach($entry in $hostAssetInputs.GetEnumerator()){$destination=Join-Path (Join-Path $context.directory 'host-asset-snapshot') $entry.Key;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Key -Destination $destination}
        $hostAssetInputs | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $context.directory 'host-asset-inputs.json') -Encoding utf8
        Assert-HostAssets
        & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild
        if($LASTEXITCODE -ne 0){throw 'Host inputs changed during evidence snapshot.'}
    }
    Assert-DrawInputs
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0
    $evidence=[ordered]@{schema='nativegpu.effect-gpu-draw-integration/1';ok=$ok;timestamp=$context.timestamp;fullRegressionRun=$false;scope='incremental GPU sprite gather/sort/draw and optional affected host paths only';build=$manifest;native=$result;checks=$checks;runs=$runs}
    $report=Join-Path $context.directory 'effects-gpu-draw-evidence.json'
    $evidence | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath $report -Encoding utf8
    Copy-Item -LiteralPath $exe,(Join-Path $directory 'compile.log'),(Join-Path $directory 'shaders/manifest.json') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path (Join-Path $context.directory 'inputs') $entry.Name;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-gpu-draw-evidence.json') | Out-Null
    Write-Host "GPU sprite drawing: $($result.groups) groups / $($result.checks) checks; integration checks $($checks.Count); $report"
    if(-not $ok){exit 1}
}finally{Pop-Location}

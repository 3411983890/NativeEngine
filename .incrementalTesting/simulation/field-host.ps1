param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/field-host';New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_vector_source_tests.exe';$manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        & node tools/field-host-fixtures.mjs
        if($LASTEXITCODE -ne 0){throw 'Field host fixtures failed.'}
        $sources=@('tests/native_vector_source.cpp','engine/native/fields/vector_source.cpp','engine/native/fields/world.cpp','engine/native/determinism.cpp','engine/native/assets/sha256.cpp')
        $inputs=$sources+@('engine/native/fields/vector_source.hpp','engine/native/fields/world.hpp','engine/native/vec3.hpp','engine/native/simulation/clock.hpp','engine/native/determinism.hpp','engine/native/assets/sha256.hpp','third_party/json.hpp',
            'assets/fields/gust_wind.field.json','assets/effects/wind_drift.effect.json','assets/effects/sprite_demo.effect.json','assets/effects/textures/reference_glow.albedo.ngtex','.screen/courtyard.scene.json','engine/effects/asset.mjs','tools/effects.mjs','tools/field-host-fixtures.mjs','.incrementalTesting/simulation/field-host.ps1')
        $inputs+=@(Get-ChildItem -LiteralPath build/field-host/cooked,build/field-host/effects-source -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1)})
        $inputs+=@(Get-ChildItem -LiteralPath build/field-host -Filter '*.field.json' -File | ForEach-Object {$_.FullName.Substring($root.Length+1)})
        $hashes=[ordered]@{};foreach($inputFile in $inputs){$hashes[$inputFile]=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX')+$sources+@('-o',$exe,'-lbcrypt')
        & $compiler @arguments *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 35;throw 'Vector source test compilation failed.'}
        foreach($inputFile in $inputs){if((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash -ne $hashes[$inputFile]){throw "Field source input changed during compilation: $inputFile"}}
        [ordered]@{sources=$hashes;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 | ConvertFrom-Json
    function Assert-Inputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale field host source: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Vector source test binary changed.'}
    }
    Assert-Inputs
    $raw=& $exe;if($LASTEXITCODE -ne 0){throw 'Vector source reference failed.'};$reference=$raw | ConvertFrom-Json
    & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild:$SkipBuild
    if($LASTEXITCODE -ne 0){throw 'Field host build failed.'}
    . (Join-Path $root 'tools/report-lib.ps1');$context=New-ReportContext -Mode 'field-host'
    $checks=[System.Collections.Generic.List[object]]::new();$runs=[ordered]@{}
    function Check([string]$name,[bool]$ok,[string]$detail){$checks.Add([ordered]@{name=$name;ok=$ok;detail=$detail});if(-not $ok){Write-Host "FAIL: $name ($detail)"}}
    function Run-Host([string]$name,[string[]]$extra,[bool]$stress=$false,[bool]$capture=$true){
        $report=Join-Path $context.directory ($name+'.json');$image=Join-Path $context.directory ($name+'.png')
        $arguments=@('--smoke','--validation','--no-watch','--audio','off','--frames','240','--render-size','640x360','--camera','5,3,8,0,1,0','--report',(Get-RelativePath $report),'--effect-library','build/field-host/cooked/library.json')+$extra
        if(-not $stress){$arguments+='--no-stress'};if($capture){$arguments+=@('--screenshot',(Get-RelativePath $image),'--screenshot-frame','90')}
        $p=Start-Process -FilePath (Join-Path $root 'build/effects-host/NativeGPU.exe') -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
        if(-not(Test-Path -LiteralPath $report)){throw "Missing field host $name report: exit=$($p.ExitCode)"}
        $data=Get-Content -LiteralPath $report -Raw -Encoding utf8 | ConvertFrom-Json
        $runs[$name]=[ordered]@{exit=$p.ExitCode;data=$data;imageHash=$(if(Test-Path -LiteralPath $image){(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash}else{''})}
    }
    Run-Host 'off' @()
    Run-Host 'default-cpu' @('--effect','sprite_demo','--effect-count','4')
    Run-Host 'default-gpu' @('--effect','sprite_demo','--effect-count','4','--effect-backend','gpu')
    $wind=@('--effect','wind_drift','--effect-count','4','--effect-backend','gpu')
    Run-Host 'zero' ($wind+@('--effect-field','build/field-host/zero.field.json'))
    Run-Host 'gust' ($wind+@('--effect-field','assets/fields/gust_wind.field.json'))
    $unicodeFiles=@(Get-ChildItem -LiteralPath build/field-host -Filter '*.field.json' -File | Where-Object {$_.Name -match '[^\x00-\x7f]'})
    if($unicodeFiles.Count -ne 1){throw 'Expected exactly one Unicode field fixture.'}
    Run-Host 'unicode' ($wind+@('--effect-field',(Get-RelativePath $unicodeFiles[0].FullName))) $false $false
    Run-Host 'stopped' ($wind+@('--effect-field','assets/fields/gust_wind.field.json','--effect-stop-tick','10'))
    Run-Host 'recovery' ($wind+@('--effect-field','assets/fields/gust_wind.field.json')) $true $false
    Run-Host 'flow' @('--effect','flow_drift','--effect-count','4','--effect-backend','gpu','--effect-field','build/field-host/flow.field.json')
    Run-Host 'reactivate' @('--effect','wind_pulse','--effect-backend','gpu','--effect-repeat-ticks','16','--effect-field','assets/fields/gust_wind.field.json') $false $false
    Run-Host 'unbound' $wind $false $false
    Run-Host 'mismatch' ($wind+@('--effect-field','build/field-host/mismatch.field.json')) $false $false
    Run-Host 'invalid' ($wind+@('--effect-field','build/field-host/invalid.field.json')) $false $false
    Run-Host 'authority' ($wind+@('--effect-field','build/field-host/authority.field.json')) $false $false
    Check 'CPU authored vector source reference' ([bool]$reference.ok) "$($reference.groups) groups / $($reference.checks) checks"
    foreach($name in @('off','default-cpu','default-gpu','zero','gust','unicode','stopped','recovery','flow','reactivate')){
        Check "$name completes" ($runs[$name].exit -eq 0 -and $runs[$name].data.success) $runs[$name].data.error
        $validation=$runs[$name].data.probes.renderTargets
        Check "$name actual debug layer is error free" ($validation.validationLayer -and $validation.validationErrors -eq 0) "warnings=$($validation.validationWarnings)"
    }
    Check 'default CPU backend needs no field resources' ($runs.'default-cpu'.data.probes.effectSprites.frames -eq 240 -and -not $runs.'default-cpu'.data.probes.gpuEffects.fields.available) 'existing CPU reference and GPU sprite path'
    Check 'default GPU backend needs no field resources' ($runs.'default-gpu'.data.probes.gpuEffects.frames -eq 240 -and -not $runs.'default-gpu'.data.probes.gpuEffects.fields.available) 'existing GPU sprite path with no World allocation'
    $gpu=$runs.gust.data.probes.gpuEffects;$field=$gpu.fields;$source=$runs.gust.data.effectField
    Check 'GPU consumes live bounded field snapshots' ($gpu.frames -eq 240 -and $gpu.drawIndirects -eq 720 -and $gpu.faults -eq 0 -and $field.available -and $field.currentPages -eq 3 -and $field.leases -eq 1 -and $field.publishedSnapshots -gt 400 -and $field.bindingsCreated -gt 400 -and $field.rejectedSnapshots -eq 0 -and $field.rejectedBindings -eq 0) "pages=$($field.currentPages) snapshots=$($field.publishedSnapshots) bindings=$($field.bindingsCreated)"
    Check 'field storage and immutable metadata obey configured budgets' ($field.defaultBytes+$field.uploadBytes -le 16*1024*1024 -and $field.metadataBytes -le 2*1024*1024 -and $field.snapshots -le 16 -and $field.metadataBindings -le 32) 'actual aligned allocations, including outstanding versions'
    Check 'field clock and source publication are explicit' ($source.requested -and $source.tick -eq $source.sourceTick -and $field.worldTick -eq $source.tick -and $source.sourcePublications -eq $source.tick+1 -and -not $source.readback -and -not $field.fieldReadback -and -not $source.gameAuthorityIntegrated) "sourceTick=$($source.sourceTick) visual producer, separate from Game authority"
    Check 'GPU field path bypasses CPU particle upload' ($runs.gust.data.probes.effectSprites.frames -eq 0 -and -not $gpu.synchronousParticleReadback) 'GPU gather/draw consumes the updated SoA'
    Check 'field publication does not amplify resource creation warnings' ($runs.gust.data.probes.renderTargets.validationWarnings -le $runs.'default-gpu'.data.probes.renderTargets.validationWarnings) "field=$($runs.gust.data.probes.renderTargets.validationWarnings) base=$($runs.'default-gpu'.data.probes.renderTargets.validationWarnings)"
    Check 'wind source changes captured particle motion' ($runs.gust.imageHash -ne $runs.zero.imageHash -and [bool]$runs.gust.imageHash -and [bool]$runs.zero.imageHash) 'same effect/seed/camera/frame; only source velocity differs'
    Check 'wind effect changes baseline pixels' ($runs.gust.imageHash -ne $runs.off.imageHash) 'actual render output'
    Check 'Unicode field asset path preserves source data and timeline' ($runs.unicode.data.probes.gpuEffects.fields.bindingsCreated -gt 400 -and $runs.unicode.data.effectField.worldDigest -eq $source.worldDigest) 'UTF-8 CLI path opens the same authored track through the Windows filesystem'
    Check 'visual field cannot change Game authority digest' ($runs.gust.data.determinism.finalHash -eq $runs.zero.data.determinism.finalHash -and $runs.zero.data.determinism.finalHash -eq $runs.off.data.determinism.finalHash) $runs.gust.data.determinism.finalHash
    $stopped=$runs.stopped.data.probes.gpuEffects.fields
    Check 'stop releases spatial leases, pages and metadata' ($runs.stopped.imageHash -eq $runs.off.imageHash -and $stopped.leases -eq 0 -and $stopped.snapshots -eq 0 -and $stopped.defaultBytes -eq 0 -and $stopped.uploadBytes -eq 0 -and $stopped.metadataBytes -eq 0 -and $runs.stopped.data.probes.gpuEffects.instances -eq 0) 'after actual queued work retires, no residual visual pages'
    Check 'no consumers leaves no CPU field allocation' ($stopped.cpuPages -eq 0 -and $stopped.cpuRetiredPages -eq 0 -and $stopped.cpuBytes -eq 0 -and $stopped.cpuLeases -eq 0 -and $stopped.cpuSources -eq 0) 'source parameters stay available; published field pages and writer lease retire'
    $recovery=$runs.recovery.data
    Check 'field bindings rebuild while CPU source clock continues' ($recovery.rendererReinitializations -gt 0 -and $recovery.resizeEvents -gt 0 -and $recovery.effectField.epochs -gt 1 -and $recovery.effectField.tick -gt $recovery.probes.gpuEffects.tick -and $recovery.probes.gpuEffects.fields.publishedSnapshots -gt 0 -and $recovery.probes.gpuEffects.frames -gt 0) "field=$($recovery.effectField.tick) rebuilt-particle=$($recovery.probes.gpuEffects.tick) epochs=$($recovery.effectField.epochs)"
    Check 'separate water velocity track drives actual GPU rendering' ($runs.flow.data.probes.gpuEffects.fields.available -and $runs.flow.data.probes.gpuEffects.fields.bindingsCreated -gt 400 -and $runs.flow.imageHash -ne $runs.off.imageHash -and $runs.flow.data.probes.gpuEffects.faults -eq 0) 'authored 3D vector source, not a water body solver'
    $repeat=$runs.reactivate.data
    Check 'field producer repeatedly reacquires after natural retirement' ($repeat.effectField.sourcePublications -gt 50 -and $repeat.probes.gpuEffects.emitted -ge 100 -and $repeat.probes.gpuEffects.fields.rejectedSnapshots -eq 0 -and $repeat.probes.gpuEffects.fields.rejectedBindings -eq 0 -and $repeat.probes.gpuEffects.faults -eq 0) "publications=$($repeat.effectField.sourcePublications) emitted=$($repeat.probes.gpuEffects.emitted)"
    foreach($name in @('unbound','mismatch')){Check "$name explicitly rejected" ($runs[$name].exit -ne 0 -and -not $runs[$name].data.success -and $runs[$name].data.errorCode -eq 'NG-E-EFFECT-UNSUPPORTED') $runs[$name].data.errorCode}
    Check 'invalid field source explicitly rejected' ($runs.invalid.exit -ne 0 -and -not $runs.invalid.data.success -and $runs.invalid.data.errorCode -eq 'NG-E-FIELD-SOURCE') $runs.invalid.data.errorCode
    Check 'preview driver cannot impersonate Game CPU authority' ($runs.authority.exit -ne 0 -and -not $runs.authority.data.success -and $runs.authority.data.errorCode -eq 'NG-E-FIELD-SOURCE') $runs.authority.data.errorCode
    Assert-Inputs
    & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild
    if($LASTEXITCODE -ne 0){throw 'Host input changed during field regression.'}
    $hostBuild=Get-Content -LiteralPath build/effects-host/host-inputs.json -Raw -Encoding utf8 | ConvertFrom-Json
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0
    $report=Join-Path $context.directory 'field-host-evidence.json'
    [ordered]@{schema='nativegpu.field-host-evidence/1';timestamp=$context.timestamp;ok=$ok;fullRegressionRun=$false;scope='Authored CPU vector source, generic renderer field lease/publish/tick callbacks, GPU particle rendering and affected host lifetime/error paths';build=$manifest;hostBuild=$hostBuild;reference=$reference;checks=$checks;runs=$runs} | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath $report -Encoding utf8
    Copy-Item -LiteralPath $exe,(Join-Path $directory 'compile.log'),build/effects-host/NativeGPU.exe -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path (Join-Path $context.directory 'source-snapshot') $entry.Name;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
    foreach($entry in $hostBuild.sources.PSObject.Properties){$destination=Join-Path (Join-Path $context.directory 'host-source-snapshot') $entry.Name.Substring($root.Length+1);New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
    Assert-Inputs
    & (Join-Path $root 'tools/build-effects-host.ps1') -SkipBuild
    if($LASTEXITCODE -ne 0){throw 'Host input changed while freezing field evidence.'}
    Register-Report -Context $context -Path $report -LegacyAliases @('field-host-evidence.json') | Out-Null
    Write-Host "Field host: $($checks.Count) checks; CPU $($reference.groups) groups / $($reference.checks) checks; $report"
    if(-not $ok){exit 1}
}finally{Pop-Location}

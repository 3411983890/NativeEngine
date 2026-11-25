param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    . (Join-Path $root 'tools/report-lib.ps1');$context=New-ReportContext -Mode 'cloth-host'
    $buildArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'tools/build-effects-host.ps1'),'-OutputDirectory','build/cloth-host');if($SkipBuild){$buildArguments+='-SkipBuild'}
    & powershell @buildArguments
    if($LASTEXITCODE -ne 0){throw 'Cloth host dependency build failed.'}
    # Freeze the compiled dependency snapshot before longer-running cases. Shared
    # workspace edits after this point do not change the executable under test.
    $directory=Join-Path $root 'build/cloth-host'
    $hostBuild=Get-Content build/cloth-host/host-inputs.json -Raw -Encoding UTF8 | ConvertFrom-Json
    Copy-Item -LiteralPath (Join-Path $directory 'NativeGPU.exe'),(Join-Path $directory 'host-inputs.json'),(Join-Path $directory 'compile.log'),(Join-Path $directory 'link.log') -Destination $context.directory
    $frozenExe=Join-Path $context.directory 'NativeGPU.exe'
    if((Get-FileHash -LiteralPath $frozenExe -Algorithm SHA256).Hash -ne $hostBuild.binaryHash){throw 'Frozen host binary mismatch.'}
    foreach($entry in $hostBuild.sources.PSObject.Properties){$destination=Join-Path $context.directory ('host-source-snapshot/'+$entry.Name.Substring($root.Length+1));New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Host source changed before the build snapshot was frozen.'}}
    & node tools/cloth-host-fixtures.mjs
    if($LASTEXITCODE -ne 0){throw 'Cloth host fixtures failed.'}
    & node tools/cloth-grouped-fixtures.mjs
    if($LASTEXITCODE -ne 0){throw 'Cloth grouped host fixtures failed.'}
    $directory=Join-Path $root 'build/cloth-host';$unicodeName=[string][char]0x5E03+[char]0x6599+[char]0x5E93;$unicodeDirectory=Join-Path $directory $unicodeName
    New-Item -ItemType Directory -Force -Path $unicodeDirectory | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $directory 'cooked') -File | Copy-Item -Destination $unicodeDirectory
    $inputs=@('.screen/courtyard.scene.json','assets/effects/sprite_demo.effect.json','.incrementalTesting/simulation/cloth-host.ps1','tools/cloth-host-fixtures.mjs','tools/cloth-grouped-fixtures.mjs','tools/cloth.mjs','tools/effects.mjs','engine/cloth/asset.mjs','engine/cloth/topology.mjs','engine/effects/asset.mjs','engine/assets/canonical.mjs')
    $inputs+=@(Get-ChildItem assets/cloth -Filter '*.cloth.json' -File | ForEach-Object {'assets/cloth/'+$_.Name})
    $inputs+='build/cloth-host/preview.scene.json'
    foreach($folder in @('cooked','effects','effects-source',$unicodeName)){$inputs+=@(Get-ChildItem -LiteralPath (Join-Path $directory $folder) -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})}
    $inputs+=@(Get-ChildItem -LiteralPath build/cloth-grouped/fixtures -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})
    $effectLibrary=Get-Content build/cloth-host/effects/library.json -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($entry in $effectLibrary.dependencies.PSObject.Properties){$inputs+='assets/'+$entry.Name}
    $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
    function Assert-PreviewInputs {foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Cloth host input changed: $($entry.Key)"}}}
    $checks=[System.Collections.Generic.List[object]]::new();$runs=[ordered]@{}
    function Check([string]$name,[bool]$ok,[string]$detail){$checks.Add([ordered]@{name=$name;ok=$ok;detail=$detail});if(-not $ok){Write-Host "FAIL: $name ($detail)"}}
    function Run-Preview([string]$name,[string[]]$extra,[bool]$stress=$false,[bool]$capture=$true){
        $report=Join-Path $context.directory ($name+'.json');$image=Join-Path $context.directory ($name+'.png')
        $arguments=@('--smoke','--validation','--no-watch','--audio','off','--frames','240','--render-size','640x360','--scene','build/cloth-host/preview.scene.json','--camera','2,2.2,4,0,1.3,0','--cloth-library','build/cloth-host/cooked/library.json','--report',(Get-RelativePath $report))+$extra
        if(-not $stress){$arguments+='--no-stress'};if($capture){$arguments+=@('--screenshot',(Get-RelativePath $image),'--screenshot-frame',$(if($stress){'210'}else{'90'}))}
        $process=Start-Process -FilePath $frozenExe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
        if(-not(Test-Path -LiteralPath $report)){throw "Missing cloth host report: $name, exit=$($process.ExitCode)"}
        $data=Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
        $runs[$name]=[ordered]@{exit=$process.ExitCode;data=$data;imageHash=$(if(Test-Path -LiteralPath $image){(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash}else{''})}
        Write-Host "Cloth host run: $name exit=$($process.ExitCode)"
    }
    $cloth=@('--cloth','flag_banner')
    Run-Preview 'off' @()
    Run-Preview 'calm' ($cloth+@('--cloth-wind','0,0,0'))
    Run-Preview 'wind' $cloth
    Run-Preview 'dispatch' ($cloth+@('--cloth-execution','dispatch'))
    Run-Preview 'group-2048' @('--cloth','group_limit','--cloth-count','4','--cloth-library','build/cloth-grouped/fixtures/library.json','--cloth-execution','grouped')
    Run-Preview 'multi' ($cloth+@('--cloth-count','4'))
    Run-Preview 'normal' ($cloth+@('--cloth-debug','1','--cloth-execution','grouped'))
    Run-Preview 'paused' ($cloth+@('--cloth-pause-tick','10'))
    Run-Preview 'resumed' ($cloth+@('--cloth-pause-tick','10','--cloth-resume-tick','40'))
    Run-Preview 'teleport' ($cloth+@('--cloth-teleport-tick','30','--cloth-teleport-origin','2,1,0'))
    Run-Preview 'reset' ($cloth+@('--cloth-reset-tick','100'))
    Run-Preview 'recovery' ($cloth+@('--cloth-teleport-tick','30','--cloth-teleport-origin','2,1,0','--cloth-pause-tick','10','--cloth-resume-tick','40')) $true
    Run-Preview 'mixed-gpu' ($cloth+@('--cloth-count','2','--effect','sprite_demo','--effect-backend','gpu','--effect-library','build/cloth-host/effects/library.json'))
    Run-Preview 'unicode' ($cloth+@('--cloth-library',('build/cloth-host/'+$unicodeName+'/library.json')))
    Run-Preview 'skinned' @('--cloth','cape_cloak') $false $false
    Run-Preview 'missing' @('--cloth','not_in_library') $false $false
    Run-Preview 'bad-pause' ($cloth+@('--cloth-pause-tick','30','--cloth-resume-tick','10')) $false $false
    Run-Preview 'bad-debug' ($cloth+@('--cloth-debug','not-a-number')) $false $false
    Run-Preview 'bad-vector' ($cloth+@('--cloth-wind','0,0,4junk')) $false $false
    Run-Preview 'non-finite' ($cloth+@('--cloth-wind','nan,0,0')) $false $false
    Run-Preview 'bad-execution' ($cloth+@('--cloth-execution','unknown')) $false $false
    $normalRuns=@('off','calm','wind','dispatch','group-2048','multi','normal','paused','resumed','teleport','reset','recovery','mixed-gpu','unicode')
    foreach($name in $normalRuns){$run=$runs[$name];Check "$name host completes" ($run.exit -eq 0 -and $run.data.success) $run.data.error
        Check "$name actual validation layer" ($run.data.probes.renderTargets.validationLayer -and $run.data.probes.renderTargets.validationErrors -eq 0) "errors=$($run.data.probes.renderTargets.validationErrors), warnings=$($run.data.probes.renderTargets.validationWarnings)"}
    Check 'off allocates no cloth instance' (-not $runs.off.data.probes.gpuCloth.available -and $runs.off.data.probes.gpuCloth.simulationBytes -eq 0) 'default rendering remains off'
    foreach($name in @('calm','wind','dispatch','multi','normal','paused','resumed','teleport','reset','mixed-gpu','unicode')){
        $s=$runs[$name].data.probes.gpuCloth;Check "$name actual GPU mesh draw" ($s.available -and $s.frames -eq 240 -and $s.draws -eq 240*$s.instances -and $s.triangles -eq 240*$s.instances*494 -and $s.totalAccountedBytes -eq ($s.simulationBytes+$s.batchAccountedBytes) -and $s.totalAccountedBytes -le $s.bufferBudgetBytes -and -not $s.cpuVertexUpload -and -not $s.synchronousVertexReadback) "instances=$($s.instances) draws=$($s.draws) triangles=$($s.triangles)"
        Check "$name does not change game authority" ([bool]$runs.off.data.determinism.finalHash -and $runs[$name].data.determinism.finalHash -eq $runs.off.data.determinism.finalHash) 'same fixed tick input and scene'
    }
    $wind=$runs.wind.data.probes.gpuCloth;Check 'simulation/draw timing is fenced and in frame' ($wind.includedInFrameGpuTime -and $wind.timingSamples -gt 0 -and $wind.simulationGpuMs -ge 0 -and $wind.drawGpuMs -ge 0) "samples=$($wind.timingSamples), solve=$($wind.simulationGpuMs)ms, draw=$($wind.drawGpuMs)ms"
    foreach($name in @('calm','wind','multi','normal','teleport')){Check "$name changes baseline pixels" ([bool]$runs[$name].imageHash -and $runs[$name].imageHash -ne $runs.off.imageHash) 'identical scene/camera/capture frame'}
    Check 'wind changes cloth pixels' ($runs.wind.imageHash -ne $runs.calm.imageHash) 'zero versus nonzero normal pressure'
    Check 'automatic grouped and dispatch modes have identical captured pixels' ($runs.wind.data.probes.gpuCloth.groupedInstances -eq 1 -and $runs.dispatch.data.probes.gpuCloth.dispatchInstances -eq 1 -and $runs.wind.imageHash -eq $runs.dispatch.imageHash) 'strict IEEE arithmetic, same game/physics clock and mesh'
    $dense=$runs.'group-2048'.data.probes.gpuCloth
    foreach($name in @('calm','wind','multi','normal','paused','resumed','teleport','reset','group-2048','mixed-gpu','unicode')){
        $s=$runs[$name].data.probes.gpuCloth;Check "$name uses bounded batch service" ($s.batchAvailable -and $s.batchDispatches -gt 0 -and $s.batchedSteps -eq $s.batchDispatches*$s.instances -and $s.batchFallbackSteps -eq 0 -and $s.batchAccountedBytes -eq ($s.batchMetadataAllocationBytes+$s.batchDescriptorEstimateBytes) -and $s.batchDescriptorSlots -eq $s.instances*32*12 -and $s.batchActiveSlots -le 32 -and $s.batchHeldResourceReferences -le 32*$s.instances*12 -and $s.totalAccountedBytes -le $s.bufferBudgetBytes) "dispatches=$($s.batchDispatches) steps=$($s.batchedSteps) accounted=$($s.totalAccountedBytes)"
    }
    Check 'dispatch mode allocates no batch service' (-not $runs.dispatch.data.probes.gpuCloth.batchAvailable -and $runs.dispatch.data.probes.gpuCloth.batchAccountedBytes -eq 0) 'general backend retains its execution mode'
    Check 'batch dispatches counted once per tick' ($dense.batchDispatches -eq $dense.tick -and $dense.dispatches -eq ($dense.tick+2*$dense.instances)) 'one shared dispatch plus two initialization dispatches per actor'
    Check '2048 vertex host load draws four full grouped meshes' ($dense.groupedInstances -eq 4 -and $dense.groupThreads -eq 256 -and $dense.sharedBytesPerGroup -eq 30720 -and $dense.draws -eq 960 -and $dense.triangles -eq 4*930*240 -and $dense.simulationBytes -le $dense.bufferBudgetBytes) '4x512 vertices, no topology truncation'
    Check '2048 vertex cloth is visual only' ($runs.'group-2048'.data.determinism.finalHash -eq $runs.off.data.determinism.finalHash) 'unchanged game authority'
    Check 'pause freezes GPU simulation clock' ($runs.paused.data.clothRuntime.paused -and $runs.paused.data.probes.gpuCloth.tick -eq 9 -and $runs.paused.data.clothRuntime.pausedGameTicks -eq ($runs.paused.data.clothRuntime.gameAge-9)) 'game clock continues, render continues'
    Check 'resume preserves separate clocks' (-not $runs.resumed.data.clothRuntime.paused -and $runs.resumed.data.clothRuntime.pausedGameTicks -eq 30 -and $runs.resumed.data.probes.gpuCloth.tick -eq ($runs.resumed.data.clothRuntime.gameAge-30)) 'pause interval [10,40)'
    Check 'teleport occurs once and keeps age' ($runs.teleport.data.clothRuntime.teleports -eq 1 -and $runs.teleport.data.probes.gpuCloth.tick -eq $runs.teleport.data.clothRuntime.gameAge -and $runs.teleport.imageHash -ne $runs.wind.imageHash) 'new origin changes actual captured geometry'
    Check 'reset keeps game age and restarts solver clock' ($runs.reset.data.clothRuntime.resets -eq 1 -and $runs.reset.data.probes.gpuCloth.tick -eq ($runs.reset.data.clothRuntime.gameAge-99)) 'reset occurs before simulation tick 100'
    $recovery=$runs.recovery.data;Check 'resize/recreation keeps driver controls' ($recovery.rendererReinitializations -gt 0 -and $recovery.resizeEvents -gt 0 -and $recovery.clothRuntime.epochs -ge 2 -and $recovery.probes.gpuCloth.frames -gt 0 -and $recovery.probes.gpuCloth.frames -lt 240 -and $recovery.clothRuntime.teleports -eq 1 -and $recovery.clothRuntime.pausedGameTicks -eq 30) 'visual restarts, game age and already consumed events persist'
    Check 'cloth and GPU particles draw together' ($runs.'mixed-gpu'.data.probes.gpuEffects.frames -eq 240 -and $runs.'mixed-gpu'.data.probes.gpuEffects.drawIndirects -eq 720 -and $runs.'mixed-gpu'.data.probes.gpuCloth.frames -eq 240) 'opaque cloth precedes transparent GPU sprites'
    Check 'unicode library matches ASCII pixels' ($runs.unicode.imageHash -eq $runs.wind.imageHash -and $runs.unicode.data.clothRuntime.assetHash -eq $runs.wind.data.clothRuntime.assetHash) 'same authenticated payload through UTF-8 path'
    foreach($name in @('skinned','missing','bad-pause','bad-debug','bad-vector','non-finite','bad-execution')){Check "$name explicitly rejected" ($runs[$name].exit -ne 0 -and -not $runs[$name].data.success) $runs[$name].data.error}
    Check 'bone support not silently approximated' ($runs.skinned.data.errorCode -eq 'NG-E-CLOTH-UNSUPPORTED') $runs.skinned.data.errorCode
    Assert-PreviewInputs
    $sourceChangesAfterFreeze=@();foreach($entry in $hostBuild.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$sourceChangesAfterFreeze+=$entry.Name}}
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0;$report=Join-Path $context.directory 'cloth-host-evidence.json'
    [ordered]@{schema='nativegpu.cloth-host-evidence/1';timestamp=$context.timestamp;ok=$ok;fullRegressionRun=$false;scope='Incremental real host cloth draw, lifecycle, authority separation, errors and coexistence with existing GPU sprites';inputs=$hashes;hostBuild=$hostBuild;buildSnapshotFrozenBeforeRuns=$true;sourceChangesAfterFreeze=$sourceChangesAfterFreeze;checks=$checks;runs=$runs} | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $report -Encoding UTF8
    foreach($entry in $hashes.GetEnumerator()){$destination=Join-Path $context.directory ('asset-source-snapshot/'+$entry.Key);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Key -Destination $destination;if((Get-FileHash $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Frozen host asset mismatch.'}}
    Assert-PreviewInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('cloth-host-evidence.json') | Out-Null
    Write-Host "Cloth host: $($checks.Count - @($checks | Where-Object {-not $_.ok}).Count)/$($checks.Count); $report"
    if(-not $ok){exit 1}
}finally{Pop-Location}

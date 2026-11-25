param([switch]$SkipBuild,[switch]$LifecycleOnly,[switch]$AdmissionOnly,[switch]$TransactionOnly,[string]$ReferenceReport='',[string]$HostBuildDirectory='build/effects-deposit-host')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    . tools/report-lib.ps1;$context=New-ReportContext -Mode 'effects-deposit-host'
    if(([int][bool]$LifecycleOnly+[int][bool]$AdmissionOnly+[int][bool]$TransactionOnly) -gt 1){throw 'Choose one incremental host scope.'}
    $buildArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory',$HostBuildDirectory);if($SkipBuild){$buildArguments+='-SkipBuild'}
    & powershell @buildArguments;if($LASTEXITCODE -ne 0){throw 'Deposit host dependency build failed.'}
    $directory=Join-Path $root 'build/effects-deposit-host';$hostDirectory=Join-Path $root $HostBuildDirectory;$hostBuild=Get-Content (Join-Path $hostDirectory 'host-inputs.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Copy-Item -LiteralPath (Join-Path $hostDirectory 'NativeGPU.exe'),(Join-Path $hostDirectory 'host-inputs.json'),(Join-Path $hostDirectory 'compile.log'),(Join-Path $hostDirectory 'link.log') -Destination $context.directory
    $exe=Join-Path $context.directory 'NativeGPU.exe';if((Get-FileHash $exe -Algorithm SHA256).Hash -ne $hostBuild.binaryHash){throw 'Frozen deposit host binary mismatch.'}
    foreach($inputFile in $hostBuild.sources.PSObject.Properties){$destination=Join-Path $context.directory ('host-source-snapshot/'+$inputFile.Name.Substring($root.Length+1));New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $inputFile.Name -Destination $destination;if((Get-FileHash $destination -Algorithm SHA256).Hash -ne $inputFile.Value){throw 'Host source changed before freeze.'}}
    & node tools/effect-deposit-host-fixtures.mjs;if($LASTEXITCODE -ne 0){throw 'Deposit host fixtures failed.'}
    $inputs=@('.screen/courtyard.scene.json','assets/effects/sprite_demo.effect.json','.incrementalTesting/simulation/effects-deposit-host.ps1','tools/effect-deposit-host-fixtures.mjs','tools/effects.mjs','engine/effects/asset.mjs','engine/assets/canonical.mjs')
    foreach($folder in @('effects-source','effects-cooked','base-source','base-cooked','bad-source','bad-cooked','mixed-source','mixed-cooked','admission-source','admission-cooked',([string][char]0x6C89+[char]0x79EF+[char]0x5E93))){$inputs+=@(Get-ChildItem -LiteralPath (Join-Path $directory $folder) -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})}
    $inputs+='build/effects-deposit-host/preview.scene.json';$library=Get-Content build/effects-deposit-host/effects-cooked/library.json -Raw -Encoding UTF8 | ConvertFrom-Json;foreach($entry in $library.dependencies.PSObject.Properties){$inputs+='assets/'+$entry.Name}
    $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
    $checks=[System.Collections.Generic.List[object]]::new();$runs=[ordered]@{}
    function Check([string]$name,[bool]$ok,[string]$detail){$checks.Add([ordered]@{name=$name;ok=$ok;detail=$detail});if(-not $ok){Write-Host "FAIL: $name ($detail)"}}
    function Run([string]$name,[string]$effect,[string]$library='effects-cooked',[string[]]$extra=@(),[bool]$stress=$false,[bool]$capture=$true){
        $report=Join-Path $context.directory ($name+'.json');$image=Join-Path $context.directory ($name+'.png');$arguments=@('--smoke','--validation','--no-watch','--audio','off','--frames','240','--render-size','640x360','--scene','build/effects-deposit-host/preview.scene.json','--camera','2,2.2,4,0,1.3,0','--effect-backend','gpu','--effect-origin','0,1,0','--effect-count','4','--effect',$effect,'--effect-library',('build/effects-deposit-host/'+$library+'/library.json'),'--report',(Get-RelativePath $report))+$extra
        if(-not $stress){$arguments+='--no-stress'};if($capture){$arguments+=@('--screenshot',(Get-RelativePath $image),'--screenshot-frame',$(if($AdmissionOnly){'60'}elseif($stress){'210'}else{'90'}))}
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
        if(-not(Test-Path -LiteralPath $report)){throw "Missing deposit host report: $name, exit=$($process.ExitCode)"};$data=Get-Content $report -Raw -Encoding UTF8 | ConvertFrom-Json
        $runs[$name]=[ordered]@{exit=$process.ExitCode;data=$data;imageHash=$(if(Test-Path -LiteralPath $image){(Get-FileHash $image -Algorithm SHA256).Hash}else{''})};Write-Host "Deposit host run: $name exit=$($process.ExitCode)"
    }
    if($TransactionOnly){
        Run 'transaction-world' 'deposit_world'
        Run 'transaction-stopped' 'deposit_world' 'effects-cooked' @('--effect-stop-tick','20')
        foreach($name in @('transaction-world','transaction-stopped')){
            $data=$runs[$name].data;$g=$data.probes.gpuEffects;$v=$data.probes.renderTargets
            Check "$name successful actual draw" ($runs[$name].exit -eq 0 -and $data.success -and $g.frames -eq 240 -and $g.drawIndirects -gt 0 -and -not $g.deposits.failed) $data.error
            Check "$name actual debug layer has no errors" ($v.validationLayer -and $v.validationErrors -eq 0) "errors=$($v.validationErrors) warnings=$($v.validationWarnings)"
        }
        $g=$runs.'transaction-world'.data.probes.gpuEffects;$d=$g.deposits
        Check 'normal transactional publication preserves instance cadence' ($d.instances -eq 4 -and $d.reservations -eq 4 -and $d.shared -eq 4 -and $d.worldSources -eq 4 -and $d.rewrites -eq 4*$g.tick -and $d.worldCommits -eq $g.tick-2 -and $d.worldDispatches -eq 2*$d.worldCommits -and $d.deferredCommits -eq 0 -and $d.deferredCandidates -eq 0 -and $d.pendingPublications -eq 0 -and $d.overduePublications -eq 0 -and @($d.publicationBacklog).Count -eq 0) "tick=$($g.tick) commits=$($d.worldCommits)"
        Check 'transactional field buffers remain within partitions' ($d.allocationBytes -eq $d.localBytes+$d.worldBytes -and $d.allocationBytes -le 128*1024*1024 -and -not $d.fieldValueReadback) "bytes=$($d.allocationBytes)"
        $d=$runs.'transaction-stopped'.data.probes.gpuEffects.deposits
        Check 'last writer withdrawal acknowledges without allocating an empty payload' ($d.instances -eq 0 -and $d.reservations -eq 0 -and $d.pendingRetirements -eq 0 -and $d.allocationBytes -eq 0 -and $d.worldSources -eq 0 -and $d.worldLeases -eq 0 -and $d.worldSnapshots -eq 0 -and $d.pendingPublications -eq 0 -and $d.overduePublications -eq 0 -and @($d.publicationBacklog).Count -eq 0 -and $d.worldCommits -eq 19 -and $d.worldDispatches -eq 2*($d.worldCommits-1)) "commits=$($d.worldCommits) dispatches=$($d.worldDispatches) bytes=$($d.allocationBytes)"
        Check 'transactional lifecycle preserves CPU authority' ([bool]$runs.'transaction-world'.data.determinism.finalHash -and $runs.'transaction-world'.data.determinism.finalHash -eq $runs.'transaction-stopped'.data.determinism.finalHash) 'same scene/input'
    }elseif($AdmissionOnly){
        Run 'admission-base' 'deposit_base' 'base-cooked'
        Run 'admission-unused-bad' 'deposit_base' 'mixed-cooked'
        Run 'admission-capacity' 'deposit_admission' 'admission-cooked' @('--effect-count','64')
        Run 'admission-selected-bad' 'deposit_bad_grid' 'mixed-cooked' @() $false $false
        foreach($name in @('admission-base','admission-unused-bad','admission-capacity')){
            $data=$runs[$name].data;$g=$data.probes.gpuEffects;$v=$data.probes.renderTargets
            Check "$name successful actual draw" ($runs[$name].exit -eq 0 -and $data.success -and $g.frames -eq 240 -and $g.drawIndirects -gt 0 -and -not $g.deposits.failed) $data.error
            Check "$name actual debug layer has no errors" ($v.validationLayer -and $v.validationErrors -eq 0) "errors=$($v.validationErrors) warnings=$($v.validationWarnings)"
            Check "$name preserves CPU authority" ($data.determinism.finalHash -eq $runs.'admission-base'.data.determinism.finalHash) 'same game scene/input'
        }
        $unused=$runs.'admission-unused-bad'.data.probes.gpuEffects.deposits
        Check 'unused unsupported writer allocates no fields or reservations' ($unused.available -and $unused.instances -eq 0 -and $unused.reservations -eq 0 -and $unused.allocationBytes -eq 0 -and $unused.admissionRejected -eq 0) 'mixed library selects a non-writing sprite'
        Check 'unused unsupported writer preserves sprite pixels' ([bool]$runs.'admission-base'.imageHash -and $runs.'admission-base'.imageHash -eq $runs.'admission-unused-bad'.imageHash) 'same selected asset and frame'
        $cap=$runs.'admission-capacity'.data;$d=$cap.probes.gpuEffects.deposits
        Check '64 requested writers admit 32 and reject 32 before recording' ($cap.effectRuntime.instances -eq 32 -and $cap.effectRuntime.rejectedInstances -eq 32 -and $d.reservations -eq 32 -and $d.admissionRejected -eq 32 -and $d.instances -eq 32) "instances=$($d.instances) rejected=$($d.admissionRejected)"
        Check 'accepted sources keep drawing and publishing under capacity rejection' ($d.shared -eq 32 -and $d.rewrites -eq 32*$d.frames -and $d.worldSources -eq 32 -and $d.lastAdmissionError -like '*count*' -and $d.allocationBytes -le 128*1024*1024 -and -not $d.failed) "shared=$($d.shared) bytes=$($d.allocationBytes)"
        $bad=$runs.'admission-selected-bad';$d=$bad.data.probes.gpuEffects.deposits
        Check 'selected unsupported writer rejects spawn without poisoning recorder' ($bad.exit -ne 0 -and -not $bad.data.success -and $bad.data.error -like '*ratio*' -and $d.available -and -not $d.failed -and $d.admissionRejected -eq 1 -and $d.reservations -eq 0 -and $d.allocationBytes -eq 0) $bad.data.error
    }elseif(-not $LifecycleOnly){
    Run 'base' 'deposit_base' 'base-cooked'
    Run 'world' 'deposit_world'
    Run 'local' 'deposit_local'
    Run 'stopped' 'deposit_world' 'effects-cooked' @('--effect-stop-tick','20')
    Run 'repeated' 'deposit_pulse' 'effects-cooked' @('--effect-repeat-ticks','16')
    $unicode=[string][char]0x6C89+[char]0x79EF+[char]0x5E93;Run 'unicode' 'deposit_world' $unicode
    Run 'recovery' 'deposit_world' 'effects-cooked' @() $true
    Run 'lifecycle' 'deposit_world' 'effects-cooked' @('--lifecycle','2') $false $false
    Run 'bad-grid' 'deposit_bad_grid' 'bad-cooked' @() $false $false
    foreach($name in @('base','world','local','stopped','repeated','unicode','recovery','lifecycle')){Check "$name successful host run" ($runs[$name].exit -eq 0 -and $runs[$name].data.success) $runs[$name].data.error}
    foreach($name in @('base','world','local','stopped','repeated','unicode')){
        $data=$runs[$name].data;$g=$data.probes.gpuEffects;Check "$name actual GPU sprite draw" ($g.available -and $g.frames -eq 240 -and $g.drawIndirects -gt 0 -and -not $g.synchronousParticleReadback) "draws=$($g.drawIndirects)"
        Check "$name preserves CPU authority" ([bool]$runs.base.data.determinism.finalHash -and $data.determinism.finalHash -eq $runs.base.data.determinism.finalHash) 'same game input/scene'
    }
    Check 'base does not configure a deposit service' (-not $runs.base.data.probes.gpuEffects.deposits.available -and $runs.base.data.probes.gpuEffects.deposits.allocationBytes -eq 0) 'non-writing library'
    $world=$runs.world.data.probes.gpuEffects;$d=$world.deposits
    Check 'world source maps all four actual instances' ($d.available -and -not $d.failed -and $d.instances -eq 4 -and $d.shared -eq 4 -and $d.worldSources -eq 4 -and $d.worldLeases -eq 4 -and $d.worldPages -gt 0) "sources=$($d.worldSources) shared=$($d.shared)"
    Check 'write cadence follows fixed instance ticks' ($d.frames -eq $world.tick -and $d.runtimeCallbacks -eq $world.tick -and $d.rewrites -eq 4*$world.tick -and $d.worldCommits -eq $world.tick-2 -and $d.worldDispatches -eq 2*$d.worldCommits) "tick=$($world.tick) commits=$($d.worldCommits)"
    Check 'field writes use bounded partitioned memory and no value readback' ($d.allocationBytes -eq $d.localBytes+$d.worldBytes -and $d.allocationBytes -le 128*1024*1024 -and -not $d.fieldValueReadback -and $d.includedInEffectsGpuTime) "bytes=$($d.allocationBytes)"
    $local=$runs.local.data.probes.gpuEffects.deposits;Check 'local-only runs no WF and creates no projection' ($local.available -and $local.instances -eq 4 -and $local.localInstances -eq 4 -and $local.localBytes -gt 0 -and $local.localProjections -eq 0 -and $local.worldBytes -eq 0 -and $local.worldPages -eq 0 -and $local.worldDispatches -eq 0) 'direct sprites use private particle state'
    Check 'world/local deposits preserve direct sprite pixels' ([bool]$runs.base.imageHash -and $runs.base.imageHash -eq $runs.world.imageHash -and $runs.base.imageHash -eq $runs.local.imageHash) 'same emitters, random streams, camera and frame'
    $stopped=$runs.stopped.data.probes.gpuEffects.deposits;Check 'stopped sources release all field allocations' ($stopped.instances -eq 0 -and $stopped.pendingRetirements -eq 0 -and $stopped.allocationBytes -eq 0 -and $stopped.localInstances -eq 0 -and $stopped.worldSources -eq 0 -and $stopped.worldLeases -eq 0 -and $stopped.worldPages -eq 0 -and $stopped.worldSnapshots -eq 0 -and $stopped.worldCommits -eq 19) "commits=$($stopped.worldCommits) bytes=$($stopped.allocationBytes)"
    $repeated=$runs.repeated.data.probes.gpuEffects.deposits;Check 'repeated short sources remain bounded' (-not $repeated.failed -and $repeated.instances -le 4 -and $repeated.pendingRetirements -eq 0 -and $repeated.worldSources -le 4 -and $repeated.worldSnapshots -le 8 -and $repeated.allocationBytes -le 128*1024*1024) "instances=$($repeated.instances) snapshots=$($repeated.worldSnapshots)"
    Check 'unicode payload produces identical pixels' ($runs.unicode.imageHash -eq $runs.world.imageHash) 'UTF-8 library path'
    Check 'recovery recreates writing service' ($runs.recovery.data.rendererReinitializations -gt 0 -and $runs.recovery.data.resizeEvents -gt 0 -and $runs.recovery.data.probes.gpuEffects.deposits.frames -gt 0 -and -not $runs.recovery.data.probes.gpuEffects.deposits.failed) 'new renderer fence/source identity domain'
    $lifecycle=$runs.lifecycle.data.lifecycle;Check 'lifecycle releases tracked GPU objects' ($lifecycle.completed -eq 2 -and @($lifecycle.entries | Where-Object {$_.liveObjectsAfterShutdown -ne 0}).Count -eq 0) 'two renderer shutdown/rebuild cycles'
    Check 'unsupported grid rejected explicitly' ($runs.'bad-grid'.exit -ne 0 -and -not $runs.'bad-grid'.data.success -and $runs.'bad-grid'.data.error -like '*ratio*') $runs.'bad-grid'.data.error
    }else{
        if(-not $ReferenceReport -or -not(Test-Path -LiteralPath $ReferenceReport)){throw 'Lifecycle delta requires its frozen functional reference report.'}
        Run 'base-lifecycle' 'deposit_base' 'base-cooked' @('--lifecycle','2') $false $false
        Run 'lifecycle' 'deposit_world' 'effects-cooked' @('--lifecycle','2') $false $false
        foreach($name in @('base-lifecycle','lifecycle')){
            $data=$runs[$name].data;$life=$data.lifecycle;$validation=$data.probes.renderTargets
            Check "$name successful rebuild" ($runs[$name].exit -eq 0 -and $data.success -and $life.completed -eq 2) 'two rebuilds'
            Check "$name has exactly two zero live-object results" (@($life.entries).Count -eq 2 -and @($life.entries | Where-Object {$_.liveObjectsAfterShutdown -ne 0}).Count -eq 0) 'tracked query interfaces included'
            Check "$name actual debug layer has no errors" ($validation.validationLayer -and $validation.validationErrors -eq 0) "errors=$($validation.validationErrors) warnings=$($validation.validationWarnings)"
        }
        Check 'writing service restarts empty in the new epoch' ($runs.lifecycle.data.probes.gpuEffects.deposits.available -and $runs.lifecycle.data.probes.gpuEffects.deposits.instances -eq 0 -and $runs.lifecycle.data.probes.gpuEffects.deposits.allocationBytes -eq 0) 'lifecycle exits just after initialization'
    }
    foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Deposit fixture changed during validation: $($entry.Key)"};$destination=Join-Path $context.directory ('asset-source-snapshot/'+$entry.Key);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Key -Destination $destination}
    $sourceChangesAfterFreeze=@();foreach($entry in $hostBuild.sources.PSObject.Properties){if((Get-FileHash $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$sourceChangesAfterFreeze+=$entry.Name}}
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0;$report=Join-Path $context.directory 'effects-deposit-host-evidence.json'
    [ordered]@{schema='nativegpu.effects-deposit-host-evidence/1';ok=$ok;hostBuild=$hostBuild;buildSnapshotFrozenBeforeRuns=$true;sourceChangesAfterFreeze=$sourceChangesAfterFreeze;inputs=$hashes;checks=$checks;runs=$runs;lifecycleDeltaOnly=[bool]$LifecycleOnly;admissionDeltaOnly=[bool]$AdmissionOnly;transactionDeltaOnly=[bool]$TransactionOnly;referenceReport=$ReferenceReport;fullRegressionRun=$false;scope='Incremental real renderer GPU LEF/WF deposition; transaction-only mode covers normal publication and final metadata-only withdrawal; admission-only mode covers bounded spawn. No unified volume or full A-G budget acceptance.'} | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $report -Encoding UTF8
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-deposit-host-evidence.json') | Out-Null
    Write-Host "Deposit host: $($checks.Count-@($checks | Where-Object {-not $_.ok}).Count)/$($checks.Count); $report";if(-not $ok){exit 1}
}finally{Pop-Location}

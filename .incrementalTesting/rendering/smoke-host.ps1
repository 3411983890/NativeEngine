param([switch]$SkipBuild,[string[]]$Cases=@(),[string]$HostBuildDirectory='build/smoke-host',[string]$SourceCapture='')
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if(-not $SkipBuild){& (Join-Path $root 'tools/build-effects-host.ps1') -OutputDirectory $HostBuildDirectory;if($LASTEXITCODE -ne 0){throw 'Smoke host dependency build failed.'}}
    & node tools/smoke-host-fixtures.mjs | Out-Null;if($LASTEXITCODE -ne 0){throw 'Smoke optical fixture cook failed.'}
    . tools/report-lib.ps1;$context=New-ReportContext -Mode 'smoke-host'
    $directory=Join-Path $root $HostBuildDirectory;$build=Get-Content (Join-Path $directory 'host-inputs.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Copy-Item -LiteralPath (Join-Path $directory 'NativeGPU.exe'),(Join-Path $directory 'host-inputs.json'),(Join-Path $directory 'compile.log'),(Join-Path $directory 'link.log') -Destination $context.directory
    $exe=Join-Path $context.directory 'NativeGPU.exe';if((Get-FileHash $exe -Algorithm SHA256).Hash -ne $build.binaryHash){throw 'Frozen smoke host executable mismatch.'}
    foreach($entry in $build.sources.PSObject.Properties){$relative=$entry.Name.Substring($root.Length+1);$target=Join-Path $context.directory ('host-source-snapshot/'+$relative);New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $target;if((Get-FileHash $target -Algorithm SHA256).Hash -ne $entry.Value){throw ('Host source changed before freeze: '+$relative)}}
    $inputs=@('.incrementalTesting/rendering/smoke-host.ps1','tools/smoke-host-fixtures.mjs','tools/smoke-host-metrics.mjs','tools/textures.mjs','tools/effects.mjs','engine/effects/asset.mjs','engine/assets/canonical.mjs','engine/scene/index.mjs','.screen/town.scene.json','assets/effects/smoke_solver_reference.effect.json','assets/textures/flat.albedo.ngtex','assets/textures/flat.normal.ngtex','assets/textures/flat.orm.ngtex','assets/effects/textures/reference_glow.albedo.ngtex')
    $inputs+=@(Get-ChildItem -LiteralPath build/smoke-host/fixtures -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1)})
    $hashes=[ordered]@{};foreach($p in $inputs){$hashes[$p]=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash;$target=Join-Path $context.directory ('asset-source-snapshot/'+$p);New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null;Copy-Item -LiteralPath $p -Destination $target;if((Get-FileHash $target -Algorithm SHA256).Hash -ne $hashes[$p]){throw 'Smoke asset changed before freeze.'}}
    $caseList=@(
        @{name='clear-q0';effect='medium_clear';scene='plain-q0';count=1},
        @{name='absorb-q0';effect='medium_absorb';scene='plain-q0';count=1},
        @{name='overlap-q0';effect='medium_absorb';scene='plain-q0';count=2},
        @{name='blocked-clear-q0';effect='medium_clear';scene='blocked-q0';count=1},
        @{name='blocked-absorb-q0';effect='medium_absorb';scene='blocked-q0';count=1},
        @{name='emission-clear-q0';effect='medium_clear';scene='emission-q0';count=1},
        @{name='emission-hot-q0';effect='medium_hot';scene='emission-q0';count=1},
        @{name='clear-q1';effect='medium_clear';scene='plain-q1';count=1},
        @{name='absorb-q1';effect='medium_absorb';scene='plain-q1';count=1},
        @{name='blocked-clear-q1';effect='medium_clear';scene='blocked-q1';count=1},
        @{name='blocked-absorb-q1';effect='medium_absorb';scene='blocked-q1';count=1},
        @{name='emission-hot-q1';effect='medium_hot';scene='emission-q1';count=1},
        @{name='scattering-q0';effect='medium_scatter';scene='scattering-q0';count=1},
        @{name='scattering-q1';effect='medium_scatter';scene='scattering-q1';count=1},
        @{name='stopped-q0';effect='medium_absorb';scene='plain-q0';count=1;extra=@('--effect-stop-tick','20')},
        @{name='recovery-q0';effect='medium_absorb';scene='plain-q0';count=1;extra=@('--lifecycle','2');capture=$false},
        @{name='far-clear-q0';effect='medium_clear';scene='plain-q0';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-absorb-q0';effect='medium_absorb';scene='plain-q0';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-clear-q1';effect='medium_clear';scene='plain-q1';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-absorb-q1';effect='medium_absorb';scene='plain-q1';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-mixed-q0';effect='medium_absorb';scene='plain-q0';count=2;camera='0,1,-30,0,1,0';origin='0.375,0,0.375'},
        @{name='far-blocked-q0';effect='medium_absorb';scene='blocked-q0';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-hot-q0';effect='medium_hot';scene='emission-q0';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-scatter-q1';effect='medium_scatter';scene='scattering-q1';count=1;camera='0,1,-30,0,1,0'},
        @{name='far-stopped-q0';effect='medium_absorb';scene='plain-q0';count=1;camera='0,1,-30,0,1,0';extra=@('--effect-stop-tick','20')},
        @{name='near-coarse-q0';effect='medium_coarse';scene='plain-q0';count=1;camera='0.5,0.5,-6,0.5,0.5,0'},
        @{name='far-coarse-q0';effect='medium_coarse';scene='plain-q0';count=1;camera='0.5,0.5,-30,0.5,0.5,0'},
        @{name='far-coarse-q1';effect='medium_coarse';scene='plain-q1';count=1;camera='0.5,0.5,-30,0.5,0.5,0'},
        @{name='fog-clear-q0';effect='medium_clear';scene='fog-q0';count=1},
        @{name='fog-absorb-q0';effect='medium_absorb';scene='fog-q0';count=1},
        @{name='fog-clear-q1';effect='medium_clear';scene='fog-q1';count=1},
        @{name='fog-absorb-q1';effect='medium_absorb';scene='fog-q1';count=1},
        @{name='recovery-q1';effect='medium_absorb';scene='plain-q1';count=1;extra=@('--lifecycle','2');capture=$false}
    )
    $runs=[ordered]@{};$checks=@()
    foreach($case in $caseList){if($Cases.Count -and $case.name -notin $Cases){continue}
        $report=Join-Path $context.directory ($case.name+'.json');$image=Join-Path $context.directory ($case.name+'.png')
        $capture= -not $case.ContainsKey('capture') -or $case.capture
        $arguments=@('--smoke','--validation','--no-watch','--no-stress','--audio','off','--frames','240','--render-size','160x160','--textures','assets/textures','--camera','0,1,-6,0,1,0','--scene',('build/smoke-host/fixtures/'+$case.scene+'.scene.json'),'--effect-backend','gpu','--effect',$case.effect,'--effect-origin','0,0,0','--effect-count',([string]$case.count),'--effect-library','build/smoke-host/fixtures/effects-cooked/library.json','--report',(Get-RelativePath $report))
        if($capture){$arguments+=@('--screenshot',(Get-RelativePath $image),'--screenshot-frame','240')}
        if($case.camera){$arguments[$arguments.IndexOf('--camera')+1]=$case.camera}
        if($case.origin){$arguments[$arguments.IndexOf('--effect-origin')+1]=$case.origin}
        if($case.extra){$arguments+=@($case.extra)}
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
        if(-not(Test-Path -LiteralPath $report)){throw "Missing smoke host report $($case.name), exit=$($process.ExitCode); frozen binary: $exe"}
        $data=Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json;$v=$data.probes.renderTargets
        $runs[$case.name]=[ordered]@{exit=$process.ExitCode;data=$data;imageHash=$(if(Test-Path -LiteralPath $image){(Get-FileHash $image -Algorithm SHA256).Hash}else{''})}
        $ok=$process.ExitCode -eq 0 -and $data.success -and $v.validationLayer -and $v.validationErrors -eq 0 -and (-not $capture -or (Test-Path -LiteralPath $image))
        $checks+=@([ordered]@{name=($case.name+' actual renderer/debug/capture');ok=[bool]$ok;detail=$data.error});Write-Host "Smoke host $($case.name): exit=$($process.ExitCode) errors=$($v.validationErrors)"
    }
    $runs | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $context.directory 'runs.json') -Encoding UTF8
    & node tools/smoke-host-metrics.mjs (Get-RelativePath $context.directory);$metricCode=$LASTEXITCODE
    $metrics=Get-Content (Join-Path $context.directory 'optical-metrics.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $changes=@();foreach($entry in $build.sources.PSObject.Properties){if((Get-FileHash $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$changes+=$entry.Name}}
    $workspaceChanges=@();$capture=$null;if($SourceCapture){$capture=Get-Content -LiteralPath $SourceCapture -Raw -Encoding UTF8 | ConvertFrom-Json;Copy-Item -LiteralPath $SourceCapture -Destination (Join-Path $context.directory 'source-capture.json');foreach($entry in $capture.sources.PSObject.Properties){$current=Join-Path $root $entry.Name;if(-not(Test-Path -LiteralPath $current) -or (Get-FileHash $current -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value){$workspaceChanges+=$entry.Name}}}
    foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw ('Smoke fixture changed after freeze: '+$entry.Key)}}
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0 -and $metricCode -eq 0 -and $metrics.ok
    $report=Join-Path $context.directory 'smoke-host-evidence.json'
    [ordered]@{schema='nativegpu.smoke-host-evidence/1';ok=[bool]$ok;hostBuild=$build;inputs=$hashes;buildSnapshotFrozenBeforeRuns=$true;sourceChangesAfterFreeze=$changes;sourceCapture=$capture;workspaceChangesSinceCapture=$workspaceChanges;cases=$runs;checks=$checks;optical=$metrics;fullRegressionRun=$false;scope='Incremental native smoke instance -> direct scalar SRVs -> compatibility/froxel optical integration, Beer-Lambert absorption, additive overlapping media, depth blockers, emission, stop and renderer lifecycle. Frozen source capture is authoritative for these runs; subsequent workspace changes are listed separately. No cloud-mixture/temporal/WF/complete D or A-G acceptance.'} | ConvertTo-Json -Depth 34 | Set-Content -LiteralPath $report -Encoding UTF8
    Register-Report -Context $context -Path $report -LegacyAliases @('smoke-host-evidence.json') | Out-Null
    Write-Host "Smoke host: $ok / $($runs.Count) cases; $report";if(-not $ok){exit 1}
}finally{Pop-Location}

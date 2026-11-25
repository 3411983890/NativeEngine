param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    . tools/report-lib.ps1
    $context=New-ReportContext -Mode 'simulation-fields'
    $buildArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/cloth-fields-host');if($SkipBuild){$buildArguments+='-SkipBuild'}
    & powershell @buildArguments
    if($LASTEXITCODE -ne 0){throw 'Shared fields host dependency build failed.'}
    $build=Get-Content build/cloth-fields-host/host-inputs.json -Raw -Encoding UTF8 | ConvertFrom-Json
    $exe=Join-Path $context.directory 'NativeGPU.exe';Copy-Item -LiteralPath build/cloth-fields-host/NativeGPU.exe -Destination $exe
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $build.binaryHash){throw 'Shared field binary snapshot mismatch.'}
    foreach($entry in $build.sources.PSObject.Properties){$destination=Join-Path $context.directory ('source-snapshot/'+$entry.Name.Substring($root.Length+1));New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Shared field source changed before freeze.'}}
    & node tools/simulation-fields-fixtures.mjs
    if($LASTEXITCODE -ne 0){throw 'Shared field fixtures failed.'}
    $inputs=@('.incrementalTesting/simulation/simulation-fields.ps1','tools/simulation-fields-fixtures.mjs','tools/cloth.mjs','tools/effects.mjs','engine/cloth/asset.mjs','engine/cloth/topology.mjs','engine/effects/asset.mjs','engine/assets/canonical.mjs','.screen/courtyard.scene.json','assets/fields/gust_wind.field.json','assets/effects/wind_drift.effect.json','assets/effects/sprite_demo.effect.json')
    $inputs+=@(Get-ChildItem assets/cloth -Filter '*.cloth.json' -File | ForEach-Object {'assets/cloth/'+$_.Name})
    $inputs+=@(Get-ChildItem build/simulation-fields -Recurse -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')})
    $library=Get-Content build/simulation-fields/effects/library.json -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($entry in $library.dependencies.PSObject.Properties){$inputs+='assets/'+$entry.Name}
    $hashes=[ordered]@{};foreach($file in ($inputs | Select-Object -Unique)){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
    function Assert-Assets {foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Shared field fixture changed: $($entry.Key)"}}}
    $checks=[System.Collections.Generic.List[object]]::new();$runs=[ordered]@{}
    function Check([string]$name,[bool]$ok,[string]$detail){$checks.Add([ordered]@{name=$name;ok=$ok;detail=$detail});if(-not $ok){Write-Host "FAIL: $name ($detail)"}}
    function Run([string]$name,[string[]]$extra,[bool]$stress=$false,[bool]$capture=$true){
        $report=Join-Path $context.directory ($name+'.json');$image=Join-Path $context.directory ($name+'.png')
        $arguments=@('--smoke','--validation','--no-watch','--audio','off','--frames','240','--render-size','640x360','--scene','build/simulation-fields/preview.scene.json','--camera','2,2.2,4,0,1.3,0','--cloth-library','build/simulation-fields/cloth/library.json','--effect-library','build/simulation-fields/effects/library.json','--report',(Get-RelativePath $report))+$extra
        if(-not $stress){$arguments+='--no-stress'};if($capture){$arguments+=@('--screenshot',(Get-RelativePath $image),'--screenshot-frame',$(if($stress){'210'}else{'90'}))}
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -Wait
        if(-not(Test-Path -LiteralPath $report)){throw "Missing shared field report: $name exit=$($process.ExitCode)"}
        $runs[$name]=[ordered]@{exit=$process.ExitCode;data=(Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json);imageHash=$(if(Test-Path -LiteralPath $image){(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash}else{''})}
        Write-Host "Shared field run: $name exit=$($process.ExitCode)"
    }
    $cloth=@('--cloth','flag_banner');$field=@('--simulation-field','build/simulation-fields/fixed.field.json');$effect=@('--effect','wind_drift','--effect-backend','gpu')
    Run 'off' @()
    Run 'uniform' $cloth
    Run 'cloth-only' ($cloth+$field)
    Run 'cloth-zero' ($cloth+@('--simulation-field','build/simulation-fields/zero.field.json'))
    Run 'cloth-outside' ($cloth+@('--simulation-field','build/simulation-fields/outside.field.json'))
    Run 'particles-only' ($effect+$field)
    Run 'particles-stop' ($effect+$field+@('--effect-stop-tick','30'))
    Run 'shared-stop' ($cloth+$effect+$field+@('--effect-stop-tick','30'))
    Run 'shared' ($cloth+$effect+$field)
    Run 'shared-multi' ($cloth+$effect+$field+@('--cloth-count','2'))
    Run 'pause-resume' ($cloth+$field+@('--cloth-pause-tick','10','--cloth-resume-tick','40'))
    Run 'teleport' ($cloth+$field+@('--cloth-teleport-tick','30','--cloth-teleport-origin','100,1,0'))
    Run 'reset' ($cloth+$field+@('--cloth-reset-tick','100'))
    Run 'recovery' ($cloth+$effect+$field) $true
    Run 'unicode' ($cloth+@('--simulation-field',('build/simulation-fields/'+[char]0x5171+[char]0x4EAB+[char]0x98CE+'.field.json')))
    Run 'legacy-effect' ($effect+@('--effect-field','build/simulation-fields/fixed.field.json'))
    Run 'legacy-stop' ($effect+@('--effect-field','build/simulation-fields/fixed.field.json','--effect-stop-tick','30'))
    Run 'authority-rejected' ($cloth+@('--simulation-field','build/simulation-fields/authority.field.json')) $false $false
    Run 'flow-rejected' ($cloth+@('--simulation-field','build/simulation-fields/flow.field.json')) $false $false
    Run 'no-consumer' $field $false $false
    Run 'two-producers' ($cloth+$effect+$field+@('--effect-field','build/simulation-fields/fixed.field.json')) $false $false
    Run 'undeclared-particle' ($cloth+$field+@('--effect','sprite_demo','--effect-backend','gpu')) $false $false
    foreach($name in @('off','uniform','cloth-only','cloth-zero','cloth-outside','particles-only','particles-stop','shared-stop','shared','shared-multi','pause-resume','teleport','reset','recovery','unicode','legacy-effect','legacy-stop')){
        $r=$runs[$name];Check "$name completes" ($r.exit -eq 0 -and $r.data.success) $r.data.error
        Check "$name actual validation layer" ($r.data.probes.renderTargets.validationLayer -and $r.data.probes.renderTargets.validationErrors -eq 0) "errors=$($r.data.probes.renderTargets.validationErrors)"
        Check "$name preserves game authority" ($r.data.determinism.finalHash -eq $runs.off.data.determinism.finalHash) 'same fixed tick scene and input'
    }
    foreach($name in @('cloth-only','cloth-zero','cloth-outside','particles-only','shared','shared-multi','pause-resume','teleport','reset','unicode')){
        $r=$runs[$name].data;$f=$r.simulationField;$s=$r.probes.gpuEffects.fields
        Check "$name uses one shared field service" ($s.available -and $f.leaseActive -and $f.domain -eq 'visual' -and -not $f.gameAuthorityIntegrated -and -not $f.readback -and $f.worldTick -eq $f.tick -and $f.cpuLeases -eq 2 -and $f.cpuSources -eq 3 -and $s.leases -eq 1 -and $s.rejectedSnapshots -eq 0 -and $s.rejectedBindings -eq 0) "tick=$($f.tick) leases=$($s.leases) sources=$($f.cpuSources)"
        Check "$name published once per field frame" ($s.bindingsCreated -eq $f.bindings -and $f.bindings -eq $f.sourcePublications -and $f.bindings -le 240) "publications=$($f.sourcePublications) bindings=$($f.bindings)"
        if($r.probes.gpuCloth.available){Check "$name cloth uses explicit shared field time" ($r.probes.gpuCloth.sharedWindBound -and $r.probes.gpuCloth.sharedWindTick -eq $f.tick -and $r.clothRuntime.sharedWindSource) 'separate cloth age and field query tick'}
    }
    Check 'field service works without GPU particles' (-not $runs.'cloth-only'.data.probes.gpuEffects.available -and $runs.'cloth-only'.data.probes.gpuEffects.fields.available) 'cloth is an independent field consumer'
    Check 'constant field matches uniform wind pixels' ($runs.'cloth-only'.imageHash -eq $runs.uniform.imageHash) 'same 4 m/s wind and full mesh'
    Check 'zero field changes actual cloth pixels' ($runs.'cloth-zero'.imageHash -ne $runs.'cloth-only'.imageHash) 'spatial input affects deformation'
    Check 'unknown coverage uses declared uniform fallback' ($runs.'cloth-outside'.imageHash -eq $runs.uniform.imageHash) 'whole-cloth unavailable queries preserve uniform fallback'
    Check 'Unicode source has identical pixels' ($runs.unicode.imageHash -eq $runs.'cloth-only'.imageHash) 'same source content'
    Check 'shared consumers use same producer history' ($runs.shared.data.simulationField.worldDigest -eq $runs.'cloth-only'.data.simulationField.worldDigest -and $runs.shared.data.simulationField.sourcePublications -eq $runs.'cloth-only'.data.simulationField.sourcePublications) 'no per-consumer source accumulation'
    Check 'particles and cloth render together' ($runs.shared.data.probes.gpuEffects.frames -eq 240 -and $runs.shared.data.probes.gpuCloth.draws -eq 240 -and $runs.'shared-multi'.data.probes.gpuCloth.draws -eq 480) 'shared snapshot, distinct solvers'
    Check 'shared particle binding matches legacy constant source pixels' ($runs.'particles-only'.imageHash -eq $runs.'legacy-effect'.imageHash) 'same modules and 4 m/s input'
    $stopped=$runs.'particles-stop'.data
    Check 'last consumer releases field leases and producer' (-not $stopped.simulationField.leaseActive -and $stopped.simulationField.cpuLeases -eq 0 -and $stopped.simulationField.cpuSources -eq 0 -and $stopped.probes.gpuEffects.fields.leases -eq 0 -and $stopped.probes.gpuEffects.fields.defaultBytes -eq 0 -and $stopped.probes.gpuEffects.fields.uploadBytes -eq 0 -and $stopped.probes.gpuEffects.fields.metadataBytes -eq 0 -and $stopped.simulationField.bindings -lt 240) 'actual fence completion reclaims pages and binding metadata'
    Check 'cloth keeps source subscribed after particles stop' ($runs.'shared-stop'.data.simulationField.leaseActive -and $runs.'shared-stop'.data.probes.gpuCloth.sharedWindBound -and $runs.'shared-stop'.data.probes.gpuCloth.frames -eq 240) 'remaining reader prevents premature producer removal'
    Check 'pause does not rewind the field clock' ($runs.'pause-resume'.data.clothRuntime.pausedGameTicks -eq 30 -and $runs.'pause-resume'.data.probes.gpuCloth.tick -eq ($runs.'pause-resume'.data.clothRuntime.gameAge-30)) 'separate producer/consumer clocks'
    Check 'reset preserves the field game tick' ($runs.reset.data.clothRuntime.resets -eq 1 -and $runs.reset.data.probes.gpuCloth.tick -eq ($runs.reset.data.clothRuntime.gameAge-99)) 'reset solver only'
    Check 'recovery rebuilds both field and cloth consumers' ($runs.recovery.data.simulationField.epochs -ge 2 -and $runs.recovery.data.clothRuntime.epochs -ge 2 -and $runs.recovery.data.probes.gpuCloth.sharedWindTick -eq $runs.recovery.data.simulationField.tick) 'source time persists; GPU leases and views recreated'
    Check 'legacy field entry remains usable' ($runs.'legacy-effect'.data.effectField.leaseActive -and -not $runs.'legacy-effect'.data.simulationField.requested) 'original explicit per-effect field clock'
    Check 'legacy last-consumer release remains usable' (-not $runs.'legacy-stop'.data.effectField.leaseActive -and $runs.'legacy-stop'.data.probes.gpuEffects.fields.leases -eq 0) 'lifetime service no longer depends on effect object ownership'
    foreach($name in @('authority-rejected','flow-rejected','no-consumer','two-producers','undeclared-particle')){Check "$name explicitly rejected" ($runs[$name].exit -ne 0 -and -not $runs[$name].data.success) $runs[$name].data.error}
    Assert-Assets
    $changed=@();foreach($entry in $build.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$changed+=$entry.Name}}
    $ok=@($checks | Where-Object {-not $_.ok}).Count -eq 0
    $report=Join-Path $context.directory 'simulation-fields-evidence.json'
    [ordered]@{schema='nativegpu.simulation-fields-evidence/1';ok=$ok;fullRegressionRun=$false;scope='Incremental shared authored visual wind, standalone cloth and particle/cloth consumers, clocks, recovery and authority isolation';build=$build;assets=$hashes;buildFrozenBeforeRuns=$true;sourceChangesAfterFreeze=$changed;checks=$checks;runs=$runs} | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $report -Encoding UTF8
    foreach($entry in $hashes.GetEnumerator()){$destination=Join-Path $context.directory ('asset-snapshot/'+$entry.Key);New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null;Copy-Item -LiteralPath $entry.Key -Destination $destination;if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entry.Value){throw 'Shared field asset snapshot mismatch.'}}
    Copy-Item -LiteralPath build/cloth-fields-host/host-inputs.json,build/cloth-fields-host/compile.log,build/cloth-fields-host/link.log -Destination $context.directory
    Assert-Assets
    Register-Report -Context $context -Path $report -LegacyAliases @('simulation-fields-evidence.json') | Out-Null
    Write-Host "Shared fields: $($checks.Count-@($checks | Where-Object {-not $_.ok}).Count)/$($checks.Count); $report"
    if(-not $ok){exit 1}
}finally{Pop-Location}

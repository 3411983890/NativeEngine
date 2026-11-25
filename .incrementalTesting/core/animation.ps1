param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')
$context = New-ReportContext -Mode 'animation'
$checks = [System.Collections.Generic.List[object]]::new()
$success = $false
function Check([string]$name, [bool]$ok) {
    $checks.Add([pscustomobject]@{ name=$name; ok=$ok })
    if (-not $ok) { throw "Animation check failed: $name" }
}
try {
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    }
    $context.build = Get-BuildIdentity
    & node --test 'tests/animation.test.mjs'
    Check 'offline import and rejection tests' ($LASTEXITCODE -eq 0)
    $native = Join-Path $context.directory 'animation-native.json'
    & '.\build\native_animation_tests.exe' --evidence $native
    Check 'native runtime contracts' ($LASTEXITCODE -eq 0)
    $nativeEvidence=Get-Content -LiteralPath $native -Raw | ConvertFrom-Json
    Check 'M1-M4 geometry, deterministic continuation and deformation contracts' ([int]$nativeEvidence.milestones.checks -gt 0)
    Register-Report -Context $context -Path $native | Out-Null
    $reference = Get-FileHash -LiteralPath 'build\animation\mannequin\animation.bin'
    & node 'tools/offline/animation.mjs' import --in 'build/animation/mannequin.gltf' --out 'build/animation/reimport'
    Check 'repeat import succeeds' ($LASTEXITCODE -eq 0)
    Check 'binary import hash is repeatable' ($reference.Hash -eq (Get-FileHash -LiteralPath 'build\animation\reimport\animation.bin').Hash)
    Check 'header import hash is repeatable' ((Get-FileHash -LiteralPath 'build\animation\mannequin\character.json').Hash -eq (Get-FileHash -LiteralPath 'build\animation\reimport\character.json').Hash)
    $record = Join-Path $context.directory 'animation-record.csv'
    $first = Join-Path $context.directory 'animation-smoke.json'
    $second = Join-Path $context.directory 'animation-replay.json'
    $image = Join-Path $context.directory 'animation.png'
    $disabledReport = Join-Path $context.directory 'animation-disabled.json'
    $disabledImage = Join-Path $context.directory 'animation-disabled.png'
    $argsFirst = @('--animation-demo','--no-stress','--frames','240','--record',(Get-RelativePath $record),'--report',(Get-RelativePath $first),'--screenshot',(Get-RelativePath $image),'--screenshot-frame','75','--camera','3,2,8,0,1,3','--mesh-shaders','off','--validation')
    $process = Start-Process -FilePath '.\build\NativeGPU.exe' -ArgumentList $argsFirst -WindowStyle Hidden -Wait -PassThru
    Check 'animated renderer runs' ($process.ExitCode -eq 0)
    $process = Start-Process -FilePath '.\build\NativeGPU.exe' -ArgumentList @('--smoke','--no-stress','--frames','240','--replay',(Get-RelativePath $record),'--replay-strict','--report',(Get-RelativePath $second),'--mesh-shaders','off') -WindowStyle Hidden -Wait -PassThru
    Check 'strict replay runs' ($process.ExitCode -eq 0)
    $a = Get-Content -LiteralPath $first -Raw | ConvertFrom-Json
    $b = Get-Content -LiteralPath $second -Raw | ConvertFrom-Json
    Check 'GPU skinning actually drew' ([bool]$a.probes.skinning.implemented -and [int]$a.probes.skinning.draws -ge 240)
    Check 'motion database is active' ([bool]$a.animation.motionMatching -and [int]$a.animation.databaseFrames -gt 0)
    Check 'GPU morph and soft deformation paths drew' ([int]$a.probes.skinning.morphDraws -ge 240 -and [int]$a.probes.skinning.softDraws -ge 240)
    Check 'M1-M4 capabilities reported honestly' ([bool]$a.animation.footIK -and [bool]$a.animation.muscles -and [bool]$a.animation.softTissue -and -not [bool]$a.animation.facial)
    Check 'notifications reached the host queue' ([int]$a.animation.notifyEvents -gt 0)
    Check 'five stages are reported' (@($a.animation.stages).Count -eq 5)
    Check 'final simulation digest matches replay' ($a.determinism.finalHash -eq $b.determinism.finalHash)
    for ($i=0; $i -lt 5; $i++) { Check "stage $i matches replay" ($a.animation.stages[$i].hash -eq $b.animation.stages[$i].hash) }
    Check 'screenshot exists' (Test-Path -LiteralPath $image)
    Check 'screenshot contains rendered pixels' ([double]$a.probes.renderTargets.lastScreenshotMean -gt 3)
    $process = Start-Process -FilePath '.\build\NativeGPU.exe' -ArgumentList @('--animation-demo','--animation-features','off','--no-stress','--frames','100','--report',(Get-RelativePath $disabledReport),'--screenshot',(Get-RelativePath $disabledImage),'--screenshot-frame','75','--camera','3,2,8,0,1,3','--mesh-shaders','off') -WindowStyle Hidden -Wait -PassThru
    Check 'feature-disabled renderer runs' ($process.ExitCode -eq 0)
    $disabled=Get-Content -LiteralPath $disabledReport -Raw | ConvertFrom-Json
    Check 'disabled capabilities are reported as disabled' (-not [bool]$disabled.animation.featuresEnabled -and -not [bool]$disabled.animation.motionMatching -and -not [bool]$disabled.animation.footIK -and -not [bool]$disabled.animation.muscles -and -not [bool]$disabled.animation.softTissue)
    Check 'disabled deformation returns to zero' ([double]$disabled.animation.muscleWeight -eq 0 -and [double]$disabled.animation.softLodWeight -eq 0 -and [double]$disabled.animation.softOffsetM -eq 0)
    Check 'bounded decisions are available for diagnosis' (@($a.animation.decisions).Count -gt 0 -and @($a.animation.decisions).Count -le 4096)
    foreach ($file in @($first,$second,$record,$image,$disabledReport,$disabledImage)) { Register-Report -Context $context -Path $file | Out-Null }
    $success = $true
} finally {
    $summary = [pscustomobject]@{ schema='nativegpu.animation-evidence/1'; success=$success; version=$context.version; commit=$context.commit; worktreeDirty=$context.dirty; checks=$checks }
    $summaryPath = Join-Path $context.directory 'animation-evidence.json'
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding utf8
    Register-Report -Context $context -Path $summaryPath | Out-Null
    Complete-Run -Context $context -Success $success | Out-Null
    Pop-Location
}
if (-not $success) { exit 1 }
Write-Host "Animation evidence: $($context.directory)"

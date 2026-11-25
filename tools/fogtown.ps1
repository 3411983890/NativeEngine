param([switch]$Tour,[switch]$Continue,[switch]$Verify,[switch]$Package,[switch]$SkipBuild,[string]$BuildDirectory='build-fogtown')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $exe=Join-Path $root "$BuildDirectory/NativeGPU.exe"
    if(-not $SkipBuild) {
        $stale=-not(Test-Path -LiteralPath $exe)
        $inputsPath=Join-Path $root "$BuildDirectory/source-inputs.json"
        if(-not $stale -and (Test-Path -LiteralPath $inputsPath)) {
            $inputs=Get-Content -LiteralPath $inputsPath -Raw -Encoding UTF8 | ConvertFrom-Json
            foreach($entry in $inputs.PSObject.Properties) {
                if(-not(Test-Path -LiteralPath $entry.Name) -or (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){$stale=$true;break}
            }
        } elseif(-not(Test-Path -LiteralPath $inputsPath)) {$stale=$true}
        if($stale) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/build.ps1" -OutputDirectory $BuildDirectory
            if($LASTEXITCODE -ne 0){throw 'Native build failed.'}
        }
    }
    if(-not(Test-Path -LiteralPath $exe)){throw 'The Fogtown executable is missing.'}
    if($Verify -or $Package -or ($Tour -and -not(Test-Path -LiteralPath 'outputs/fogtown/checkpoints/square.json'))) {
        & (Join-Path $root "$BuildDirectory/native_fogtown_tests.exe")
        if($LASTEXITCODE -ne 0){throw 'Fogtown playthrough failed.'}
    }
    if($Verify -or $Package) {
        & node --test tests/fogtown.test.mjs tests/gameplay.test.mjs tests/scene.test.mjs
        if($LASTEXITCODE -ne 0){throw 'Fogtown content tests failed.'}
        foreach($point in @('arrival','repair','square','complete')) {
            $arguments=@('--smoke','--no-stress','--no-watch','--scene','.screen/fogtown.scene.json','--frames','240','--render-size','1280x720','--load-save',"outputs/fogtown/checkpoints/$point.json",'--screenshot',"reports/fogtown/$point.png",'--screenshot-frame','12','--report',"reports/fogtown/render-$point.json")
            $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru
            if(-not $process.WaitForExit(90000)){$process.Kill();throw "Render check timed out: $point"}
            if($process.ExitCode -ne 0){throw "Render check failed: $point"}
            $report=Get-Content -LiteralPath "reports/fogtown/render-$point.json" -Raw -Encoding UTF8 | ConvertFrom-Json
            if(-not $report.success){throw "Native renderer reported failure: $point"}
        }
        & node tools/fogtown/report.mjs
        if($LASTEXITCODE -ne 0){throw 'Fogtown evidence validation failed.'}
        Write-Host 'Fogtown gameplay, restore, retry, deterministic replay and native captures passed.'
        if(-not $Package){return}
    }
    if($Package) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/fogtown/package.ps1" -BuildDirectory $BuildDirectory
        if($LASTEXITCODE -ne 0){throw 'Fogtown package failed.'}
        return
    }
    $arguments=@('--scene','.screen/fogtown.scene.json','--textures','assets/textures','--save','saves/fogtown-session.json','--report','reports/fogtown/session.json')
    if($Tour){$arguments+=@('--tour','outputs/fogtown/checkpoints')}
    if($Continue) {
        $save=@('saves/fogtown-session.json','saves/fogtown-checkpoint.json') | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1
        if($save){$arguments+=@('--load-save',$save)}
    }
    Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root
} finally {Pop-Location}

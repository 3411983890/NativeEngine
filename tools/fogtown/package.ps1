param([string]$BuildDirectory='build-fogtown')
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$outputs=[IO.Path]::GetFullPath((Join-Path $root 'outputs'))
$destination=[IO.Path]::GetFullPath((Join-Path $outputs 'Fogtown-FirstPerson-win64'))
$zip=[IO.Path]::GetFullPath((Join-Path $outputs 'Fogtown-FirstPerson-win64.zip'))
$prefix=$outputs.TrimEnd('\')+'\'
if(-not $destination.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or -not $zip.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Package paths escaped outputs.'}
Push-Location $root
try {
    New-Item -ItemType Directory -Force -Path $outputs | Out-Null
    $staging=Join-Path $outputs ('.fogtown-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    foreach($folder in @('assets','assets/scripts','animation/mannequin','checkpoints','docs','reports')){New-Item -ItemType Directory -Force -Path (Join-Path $staging $folder) | Out-Null}
    Copy-Item -LiteralPath "$BuildDirectory/NativeGPU.exe" -Destination $staging
    Copy-Item -LiteralPath '.screen/fogtown.scene.json','assets/scene.schema.json' -Destination (Join-Path $staging 'assets')
    Copy-Item -LiteralPath 'assets/gameplay','assets/textures','assets/fogtown' -Destination (Join-Path $staging 'assets') -Recurse
    Copy-Item -LiteralPath 'assets/scripts/fogtown' -Destination (Join-Path $staging 'assets/scripts') -Recurse
    Copy-Item -LiteralPath 'build/animation/mannequin/character.json','build/animation/mannequin/animation.bin' -Destination (Join-Path $staging 'animation/mannequin')
    Get-ChildItem -LiteralPath 'outputs/fogtown/checkpoints' -Filter '*.json' -File | Copy-Item -Destination (Join-Path $staging 'checkpoints')
    Get-ChildItem -LiteralPath '.developmentAgent/samples/fogtown/docs' -File | Copy-Item -Destination (Join-Path $staging 'docs')
    Copy-Item -LiteralPath 'samples/fogtown/project.json' -Destination $staging
    Copy-Item -LiteralPath 'third_party/LUA-LICENSE.txt' -Destination $staging
    if(Test-Path -LiteralPath 'reports/fogtown/evidence.json'){Copy-Item -LiteralPath 'reports/fogtown/evidence.json' -Destination (Join-Path $staging 'reports')}
    foreach($name in @('README.md','regression.json','node-tests.log','verify.log','arrival.png','repair.png','square.png','complete.png','render-arrival.json','render-repair.json','render-square.json','render-complete.json')){
        $source=Join-Path $root "reports/fogtown/$name"
        if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination (Join-Path $staging 'reports')}
    }
    $common='--scene assets\fogtown.scene.json --textures assets\textures --character animation\mannequin\character.json --save saves\fogtown-session.json --report reports\session.json'
    $play=@('@echo off','cd /d "%~dp0"',('start "" NativeGPU.exe '+$common))
    $play | Set-Content -LiteralPath (Join-Path $staging 'Play.cmd') -Encoding ascii
    @('@echo off','cd /d "%~dp0"',('start "" NativeGPU.exe '+$common+' --tour checkpoints')) | Set-Content -LiteralPath (Join-Path $staging 'Tour.cmd') -Encoding ascii
    @('@echo off','cd /d "%~dp0"','if exist saves\fogtown-session.json (',('  start "" NativeGPU.exe '+$common+' --load-save saves\fogtown-session.json'),') else if exist saves\fogtown-checkpoint.json (',('  start "" NativeGPU.exe '+$common+' --load-save saves\fogtown-checkpoint.json'),') else (','  call Play.cmd',')') | Set-Content -LiteralPath (Join-Path $staging 'Continue.cmd') -Encoding ascii
    @'
# 雾镇重启 · 第一人称示例

双击 Play.cmd 开始，Continue.cmd 继续上次进度，Tour.cmd 开发者导览。
点击游戏画面后，WASD 移动、鼠标转向、E 交互、Shift 跑、空格跳。
Esc 暂停并释放鼠标，F7 保存，F12 恢复，F5 重新开始。
导览 F1 切换诊断侧栏，F2 加载完整检查点。

要求 Windows 11 x64、支持 D3D12 的显卡；无需 Node 或编译工具。
内容源见 assets/，制作、路线、覆盖范围与限制见 docs/。
'@ | Set-Content -LiteralPath (Join-Path $staging 'README.md') -Encoding utf8

    # Both ends of the adventure must load using only the staged package's files.
    foreach($point in @('arrival','complete')) {
        $arguments=@('--smoke','--no-stress','--no-watch','--scene','assets/fogtown.scene.json','--textures','assets/textures','--character','animation/mannequin/character.json','--load-save',"checkpoints/$point.json",'--frames','240','--render-size','960x540','--report',"reports/package-$point.json")
        $process=Start-Process -FilePath (Join-Path $staging 'NativeGPU.exe') -ArgumentList $arguments -WorkingDirectory $staging -WindowStyle Hidden -PassThru
        if(-not $process.WaitForExit(90000)){$process.Kill();throw "Standalone package timed out: $point"}
        if($process.ExitCode -ne 0){throw "Standalone package failed: $point (inspect $staging/reports)"}
        $report=Get-Content -LiteralPath (Join-Path $staging "reports/package-$point.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        if(-not $report.success){throw "Standalone package reported failure: $point"}
    }
    $files=Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($staging.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $manifest=[ordered]@{format='nativegpu.fogtown-package/1';engineVersion=(Get-Content -LiteralPath 'VERSION' -Raw).Trim();entryPoint='Play.cmd';standaloneVerified=$true;files=$files}
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $staging 'manifest.json') -Encoding utf8
    if(Test-Path -LiteralPath $destination){Remove-Item -LiteralPath $destination -Recurse -Force}
    Move-Item -LiteralPath $staging -Destination $destination
    if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip -Force}
    Compress-Archive -LiteralPath $destination -DestinationPath $zip
    Write-Host "Standalone package verified: $destination"
    Write-Host "ZIP: $zip"
} finally {Pop-Location}

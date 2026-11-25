param([switch]$SkipBuild,[string]$BuildDirectory='build')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    if(-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$root/tools/build.ps1" -OutputDirectory $BuildDirectory
        if($LASTEXITCODE -ne 0){throw 'Gameplay build failed'}
    }
    foreach($name in @('native_gameplay_tests','native_gameplay_integration_tests','native_simulation_tests','native_physics_tests','native_destruction_tests','native_animation_tests','native_scene_tests')) {
        & (Join-Path $root "$BuildDirectory/$name.exe")
        if($LASTEXITCODE -ne 0){throw "$name failed"}
    }
    & node --test tests/gameplay.test.mjs tests/ai-tools.test.mjs tests/editor-service.test.mjs tests/scene.test.mjs
    if($LASTEXITCODE -ne 0){throw 'Gameplay tool tests failed'}
    New-Item -ItemType Directory -Force -Path 'reports/gameplay' | Out-Null
    [ordered]@{ok=$true;format='nativegpu.gameplay-evidence/1';nativeSuites=7;toolSuites=4;validationLevel='automated';acceptanceLevelCreated=$false} |
        ConvertTo-Json | Set-Content -LiteralPath 'reports/gameplay/evidence.json' -Encoding utf8
} finally {Pop-Location}

param([switch]$DebugBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $compiler = Join-Path $root '.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) {
        & "$PSScriptRoot\bootstrap.ps1"
        if ($LASTEXITCODE -ne 0) { throw 'Toolchain setup failed.' }
    }
    New-Item -ItemType Directory -Force -Path 'build','reports' | Out-Null
    $flags = @('-std=c++17','-Wall','-Wextra','-Wpedantic','-DUNICODE','-D_UNICODE','-DNOMINMAX','-I.','-static')
    if ($DebugBuild) { $flags += @('-O0','-g') } else { $flags += '-O2' }
    & $compiler @flags 'host/main.cpp' 'host/scene_loader.cpp' 'renderer/d3d12_renderer.cpp' 'engine/native/game.cpp' '-municode' '-mwindows' '-o' 'build/NativeGPU.exe' '-ld3d12' '-ldxgi' '-ld3dcompiler' '-luser32' '-lgdi32' '-lcomdlg32' '-lshell32' '-lole32' '-luuid'
    if ($LASTEXITCODE -ne 0) { throw 'Native engine build failed.' }
    & $compiler @flags 'tests/native_simulation.cpp' 'engine/native/game.cpp' '-o' 'build/native_simulation_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Simulation test build failed.' }
    & $compiler @flags 'tests/native_scene.cpp' 'host/scene_loader.cpp' '-o' 'build/native_scene_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Scene test build failed.' }
    Write-Host 'Built build\NativeGPU.exe and native tests.'
} finally { Pop-Location }

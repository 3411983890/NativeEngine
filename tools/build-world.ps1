param(
    [switch]$SkipBuild,
    [switch]$Force
)

# Builds the world cook worker and the world contract tests into build/world-host.
#
# A separate output directory from every other specialist build (build/assets-host,
# build/viewport-host, build/vegetation, ...) because the world module links its own copy of
# the compiler and a shared directory would let one feature's object files satisfy another's
# link. The worker has no D3D12 dependency: it must run on a build machine with no GPU.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $directory = Join-Path $root 'build/world-host'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $compiler = Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) {
        throw 'Run tools/bootstrap.ps1 once to install the LLVM toolchain.'
    }

    $flags = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-DNOMINMAX', '-DUNICODE', '-D_UNICODE', '-Ibuild/generated', '-static')

    $worldSources = @(
        'engine/native/world/world_path.cpp',
        'engine/native/world/world_terrain.cpp',
        'engine/native/world/world_scatter.cpp',
        'engine/native/world/world_settlement.cpp',
        'engine/native/world/world_chunk.cpp',
        'engine/native/world/world_compiler.cpp',
        'engine/native/world/world_runtime.cpp'
    )

    $worker = Join-Path $directory 'world_cook.exe'
    $tests = Join-Path $directory 'native_world_tests.exe'

    $stale = $Force
    if (-not $stale) {
        foreach ($binary in @($worker, $tests)) {
            if (-not (Test-Path -LiteralPath $binary)) { $stale = $true; break }
        }
    }
    if (-not $stale) {
        $newest = Get-ChildItem -LiteralPath 'engine/native/world' -File |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        foreach ($source in @($worldSources + @('host/world_cook_main.cpp', '.incrementalTesting/core/world/native_world_tests.cpp'))) {
            if ((Get-Item -LiteralPath $source).LastWriteTime -gt (Get-Item -LiteralPath $worker).LastWriteTime) {
                $stale = $true
                break
            }
        }
        if ($newest -and $newest.LastWriteTime -gt (Get-Item -LiteralPath $worker).LastWriteTime) { $stale = $true }
    }

    if ($SkipBuild -or -not $stale) {
        if (-not (Test-Path -LiteralPath $worker)) { throw 'The world cook worker is missing; rerun without -SkipBuild.' }
        Write-Host "World host is up to date: $worker"
        return
    }

    $log = Join-Path $directory 'compile.log'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $compiler @flags `
            'host/world_cook_main.cpp' `
            @worldSources `
            'engine/native/assets/sha256.cpp' `
            '-lbcrypt' `
            '-o' $worker *> $log
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "World cook worker build failed (exit $code); tail of $log`:" -ForegroundColor Red
            Get-Content -LiteralPath $log -Tail 60
            throw 'World cook worker build failed.'
        }

        & $compiler @flags `
            '.incrementalTesting/core/world/native_world_tests.cpp' `
            @worldSources `
            '-o' $tests *> $log
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "World contract test build failed (exit $code); tail of $log`:" -ForegroundColor Red
            Get-Content -LiteralPath $log -Tail 60
            throw 'World contract test build failed.'
        }
    } finally {
        $ErrorActionPreference = $previous
    }

    Write-Host "Built $worker and $tests."
} finally {
    Pop-Location
}

param([string]$OutputDirectory = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $compiler = Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) { throw 'Pinned native compiler is missing; run bootstrap first.' }
    $inputs = @('tests/native_fields.cpp', 'tests/native_field_projection.hpp', 'tests/native_field_sampling.hpp', 'engine/native/fields/world.hpp', 'engine/native/fields/world.cpp',
        'engine/native/fields/local.hpp', 'engine/native/fields/local.cpp',
        'engine/native/simulation/clock.hpp', 'engine/native/determinism.hpp', 'engine/native/determinism.cpp',
        'tools/build-fields.ps1')
    $hashes = [ordered]@{}
    foreach ($source in $inputs) { $hashes[$source] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $exe = Join-Path $OutputDirectory 'native_fields_tests.exe'
    $arguments = @('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static',
        'tests/native_fields.cpp','engine/native/fields/world.cpp','engine/native/fields/local.cpp','engine/native/determinism.cpp','-o',$exe)
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Field reference build failed.' }
    foreach ($source in $inputs) {
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $hashes[$source]) {
            throw "Field input changed during build: $source"
        }
    }
    [ordered]@{
        schema = 'nativegpu.fields-build/1'
        sources = $hashes
        compilerHash = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash
        binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'fields-source-inputs.json') -Encoding utf8
} finally { Pop-Location }

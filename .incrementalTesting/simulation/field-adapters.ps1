param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/field-adapters'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_field_adapters_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    if(-not $SkipBuild) {
        $sources=@('tests/native_field_adapters.cpp','engine/native/simulation/field_adapters.hpp','engine/native/simulation/field_adapters.cpp',
            'engine/native/fields/world.hpp','engine/native/fields/world.cpp','engine/native/cloth/solver_cpu.hpp','engine/native/cloth/solver_cpu.cpp',
            'engine/native/cloth/asset.hpp','engine/native/cloth/asset.cpp','engine/native/water/body.hpp','engine/native/water/body.cpp',
            'engine/native/water/asset.cpp','engine/native/vec3.hpp','engine/native/assets/sha256.hpp','engine/native/assets/sha256.cpp',
            'engine/native/determinism.hpp','engine/native/determinism.cpp','third_party/json.hpp','.incrementalTesting/simulation/field-adapters.ps1',
            'tools/cloth.mjs','engine/cloth/asset.mjs','engine/cloth/topology.mjs','engine/native/cloth/triangle_math.hpp','engine/assets/canonical.mjs')
        $sources+=@(Get-ChildItem -LiteralPath assets/cloth -Filter '*.cloth.json' -File | ForEach-Object { 'assets/cloth/'+$_.Name })
        $hashes=[ordered]@{}
        foreach($source in $sources){$hashes[$source]=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash}
        & node tools/cloth.mjs cook --in assets/cloth --out build/field-adapters/cloth *> (Join-Path $directory 'cook.log')
        if($LASTEXITCODE -ne 0){throw 'Cloth cook failed for field adapter validation.'}
        $cooked=[ordered]@{}
        $library=Get-Content -LiteralPath build/field-adapters/cloth/library.json -Raw -Encoding UTF8 | ConvertFrom-Json
        $cooked['build/field-adapters/cloth/library.json']=(Get-FileHash -LiteralPath build/field-adapters/cloth/library.json -Algorithm SHA256).Hash
        foreach($entry in $library.cloths){$path='build/field-adapters/cloth/'+$entry.hash+'.ngcloth.json';$cooked[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
        $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
        & $compiler -std=c++17 -Wall -Wextra -Wpedantic -O2 -static tests/native_field_adapters.cpp engine/native/simulation/field_adapters.cpp engine/native/fields/world.cpp engine/native/cloth/solver_cpu.cpp engine/native/cloth/asset.cpp engine/native/water/body.cpp engine/native/water/asset.cpp engine/native/assets/sha256.cpp engine/native/determinism.cpp -lbcrypt -o $exe *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 30;throw 'Field adapter build failed.'}
        foreach($source in $sources){if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $hashes[$source]){throw "Field adapter source changed during build: $source"}}
        [ordered]@{sources=$hashes;cooked=$cooked;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($collection in @($manifest.sources,$manifest.cooked)) {
        foreach($entry in $collection.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale field adapter input: $($entry.Name)"}}
    }
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'Field adapter binary differs from manifest.'}
    $raw=& $exe --cloth-library build/field-adapters/cloth/library.json
    if($LASTEXITCODE -ne 0){throw 'Field adapter checks failed.'}
    $result=$raw | ConvertFrom-Json
    if(-not $result.ok){throw 'Field adapter report is not successful.'}
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'field-adapters'
    $report=Join-Path $context.directory 'field-adapters-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;results=$result;scope='CPU spatial cloth wind and per-source atomic water query projection; explicit grid/units/authority, no GPU readback; host, water mass transfer and wetness remain pending'} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding utf8
    Register-Report -Context $context -Path $report -LegacyAliases @('field-adapters-evidence.json') | Out-Null
    Write-Host "Field adapters: $($result.groups) groups, $($result.checks) checks; $report"
} finally {Pop-Location}

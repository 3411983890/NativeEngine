param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $directory=Join-Path $root 'build/effects-cpu'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_effects_tests.exe'
    $manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not $SkipBuild){
        & node tools/effects.mjs cook --in assets/effects --out build/effects-cpu/cooked *> (Join-Path $directory 'cook.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'cook.log') -Tail 20;throw 'Scoped CPU effect cook failed.'}
        $sources=@('tests/native_effects.cpp','engine/native/effects/runtime.cpp','engine/native/effects/asset.cpp','engine/native/assets/sha256.cpp','engine/native/determinism.cpp')
        $files=$sources+@('engine/native/effects/runtime.hpp','engine/native/smoke/effect_config.hpp','engine/native/smoke/configuration.hpp','engine/native/smoke/solver.hpp','engine/native/assets/sha256.hpp','engine/native/determinism.hpp','engine/native/vec3.hpp','engine/native/simulation/clock.hpp','third_party/json.hpp',
            'engine/effects/asset.mjs','tools/effects.mjs','.incrementalTesting/simulation/effects-cpu.ps1')
        $files+=@(Get-ChildItem -LiteralPath assets/effects,build/effects-cpu/cooked -Recurse -File | Where-Object {$_.Extension -in @('.json','.ngtex')} | ForEach-Object {$_.FullName.Substring($root.Length+1)})
        $library=Get-Content -LiteralPath build/effects-cpu/cooked/library.json -Raw | ConvertFrom-Json
        $files+=@($library.dependencies.PSObject.Properties | ForEach-Object {Join-Path 'assets' $_.Name})
        $hashes=[ordered]@{};foreach($file in $files){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX')+$sources+@('-o',$exe,'-lbcrypt')
        & $compiler @arguments *> (Join-Path $directory 'compile.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $directory 'compile.log') -Tail 30;throw 'CPU effect reference compile failed.'}
        foreach($file in $files){if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hashes[$file]){throw "CPU effect input changed during build: $file"}}
        [ordered]@{sources=$hashes;compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;binaryHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    function Assert-CpuInputs {
        foreach($entry in $manifest.sources.PSObject.Properties){if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale CPU effect input: $($entry.Name)"}}
        if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $manifest.binaryHash){throw 'CPU effect binary differs from build manifest.'}
    }
    Assert-CpuInputs
    $raw=& $exe --library build/effects-cpu/cooked/library.json
    if($LASTEXITCODE -ne 0){throw 'CPU effect reference regression failed.'}
    $result=$raw | ConvertFrom-Json
    if(-not $result.ok){throw 'CPU effect reference report is not successful.'}
    Assert-CpuInputs
    . (Join-Path $root 'tools/report-lib.ps1')
    $context=New-ReportContext -Mode 'effects-cpu'
    $report=Join-Path $context.directory 'effects-cpu-evidence.json'
    [ordered]@{timestamp=$context.timestamp;build=$manifest;results=$result;fullRegressionRun=$false;scope='incremental CPU effect asset loader, modules, instance/events/collision and production sprite reference only; no host or unrelated native/JS tests'} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding utf8
    Copy-Item -LiteralPath $exe,(Join-Path $directory 'compile.log'),(Join-Path $directory 'cook.log') -Destination $context.directory
    foreach($entry in $manifest.sources.PSObject.Properties){$destination=Join-Path (Join-Path $context.directory 'source-snapshot') $entry.Name;New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null;Copy-Item -LiteralPath $entry.Name -Destination $destination}
    Assert-CpuInputs
    Register-Report -Context $context -Path $report -LegacyAliases @('effects-cpu-evidence.json') | Out-Null
    Write-Host "CPU effects: $($result.groups) groups / $($result.checks) checks; $report"
} finally {Pop-Location}

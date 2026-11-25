param(
    [switch]$SkipCook,
    [string]$OutputDirectory = 'build/vegetation-hair'
)
$ErrorActionPreference = 'Stop'
# Incremental evidence for the vegetation/groom slice only. It never runs the full test
# suite: the workspace agreement (AGENTS.md) requires dependency-scoped checks, and a
# 67-person-week plan cannot be validated by one command anyway.
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $compiler = Join-Path $root '.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe'
    if (-not (Test-Path -LiteralPath $compiler)) { throw 'Run tools/bootstrap.ps1 once to install the LLVM toolchain.' }
    New-Item -ItemType Directory -Force -Path $OutputDirectory, 'reports/vegetation-hair' | Out-Null

    if (-not $SkipCook) {
        # The canonical author texture set lives in assets/source/vegetation/oak-imagegen-v1/complete.
        # assets/vegetation/textures/source holds the earlier exec-* dumps, whose file names do not
        # match assets/vegetation/textures/roles.json; cooking from them fails on a missing input
        # rather than silently cooking the wrong image into a slot.
        & node 'tools/vegetation-textures.mjs' cook --in 'assets/source/vegetation/oak-imagegen-v1/complete' --out 'build/vegetation/textures' --roles 'assets/vegetation/textures/roles.json'
        if ($LASTEXITCODE -ne 0) { throw 'Vegetation texture cook failed.' }
        & node 'tools/vegetation-fixtures.mjs' 'build/vegetation/source'
        if ($LASTEXITCODE -ne 0) { throw 'Vegetation fixture generation failed.' }
        & node 'tools/vegetation.mjs' library --in 'build/vegetation/source' --out 'build/vegetation/cooked' --textures 'build/vegetation/textures'
        if ($LASTEXITCODE -ne 0) { throw 'Vegetation species cook failed.' }
        & node 'tools/vegetation.mjs' regions --in 'build/vegetation/source' --out 'build/vegetation/cooked' --species 'build/vegetation/cooked'
        if ($LASTEXITCODE -ne 0) { throw 'Vegetation region cook failed.' }
        & node 'tools/groom-fixtures.mjs' 'build/groom/source'
        if ($LASTEXITCODE -ne 0) { throw 'Groom fixture generation failed.' }
        & node 'tools/groom.mjs' library --in 'build/groom/source' --out 'build/groom/cooked'
        if ($LASTEXITCODE -ne 0) { throw 'Groom library cook failed.' }
    }

    $flags = @('-std=c++17','-O2','-Wall','-Wextra','-DNOMINMAX','-Ibuild/generated','-static')
    $targets = @(
        @{ name='vegetation'; sources=@('tests/native_vegetation.cpp','engine/native/vegetation/asset.cpp','engine/native/vegetation/world.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp','renderer/hzb.cpp'); libs=@('-lbcrypt') },
        @{ name='groom';      sources=@('tests/native_groom.cpp','engine/native/groom/asset.cpp','engine/native/groom/solver.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp'); libs=@('-lbcrypt') },
        @{ name='draw';       sources=@('tests/native_vegetation_draw.cpp','renderer/vegetation_draw.cpp','renderer/png_writer.cpp','renderer/matrix_math.cpp','engine/native/vegetation/asset.cpp','engine/native/assets/package.cpp','engine/native/assets/sha256.cpp'); libs=@('-ld3d12','-ldxgi','-ld3dcompiler','-lbcrypt','-lole32','-luuid') }
    )
    $results = [ordered]@{}
    foreach ($target in $targets) {
        $exe = Join-Path $OutputDirectory ("native_vegetation_{0}_tests.exe" -f $target.name)
        $log = Join-Path $OutputDirectory ("compile-{0}.log" -f $target.name)
        $arguments = @($flags) + $target.sources + $target.libs + @('-o', $exe)
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { & $compiler @arguments *> $log; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
        if ($code -ne 0) { Get-Content -LiteralPath $log -Tail 40; throw "Compile failed for $($target.name)." }
        $output = & $exe 2>&1
        if ($LASTEXITCODE -ne 0) { throw "$($target.name) tests failed: $output" }
        $results[$target.name] = ($output | Select-Object -Last 1 | ConvertFrom-Json)
    }

    $nodeOutput = & node --test tests/vegetation.test.mjs tests/groom.test.mjs tests/vegetation-editor.test.mjs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Node tests failed: $($nodeOutput | Select-Object -Last 20)" }
    $summary = ($nodeOutput | Select-String -Pattern '^# (tests|pass|fail) ' | ForEach-Object { $_.Line }) -join '; '

    $report = [ordered]@{
        schema = 'nativegpu.vegetation-hair-evidence/1'
        planVersion = 'vegetation-hair/1'
        generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
        status = 'measured'
        scope = 'Vegetation species/region cook, page streaming, CPU culling and LOD, GPU indirect instanced draw; groom cook, scalp binding, XPBD guide solve, interpolation and coverage. No frame-budget, shadow, voxel, assembly or strand-renderer evidence.'
        node = @{ command = 'node --test tests/vegetation.test.mjs tests/groom.test.mjs'; summary = $summary }
        native = $results
        capacity = (& node 'tools/vegetation.mjs' capacity --instances 30000000 | ConvertFrom-Json)
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'reports/vegetation-hair/evidence.json' -Encoding utf8
    Write-Host 'Wrote reports/vegetation-hair/evidence.json'
    $report.native.GetEnumerator() | ForEach-Object { Write-Host ("{0}: {1} checks, {2} groups" -f $_.Key, $_.Value.checks, $_.Value.groups) }
} finally { Pop-Location }

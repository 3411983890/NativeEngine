param([switch]$SkipBuild,[switch]$SkipCook,[string]$Node='node')
$ErrorActionPreference='Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    # Native skinned-garment reference. Builds a standalone binary from the cloth solver and
    # its dependencies only — it does not invoke tools/build.ps1, the GPU host or any
    # unrelated suite, so the cost stays with the change.
    $directory=Join-Path $root 'build/garment-reference'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $exe=Join-Path $directory 'native_garment_tests.exe'
    $cookedDirectory=Join-Path $root 'build/garment'
    $manifestPath=Join-Path $directory 'inputs.json'
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    if(-not(Test-Path -LiteralPath $compiler)){throw "Compiler missing: $compiler"}

    $sources=@('tests/native_garment.cpp','engine/native/cloth/asset.cpp','engine/native/cloth/solver_cpu.cpp',
        'engine/native/assets/sha256.cpp','engine/native/determinism.cpp')
    $inputs=@($sources+@('engine/native/cloth/asset.hpp','engine/native/cloth/solver_cpu.hpp','engine/native/cloth/skin.hpp',
        'engine/native/cloth/triangle_math.hpp','engine/native/vec3.hpp','engine/native/determinism.hpp',
        'engine/native/assets/sha256.hpp','third_party/json.hpp','engine/garment/cook.mjs','engine/garment/inspect.mjs',
        'engine/garment/weights.mjs','engine/garment/partition.mjs','engine/garment/validate.mjs','engine/garment/physics.mjs',
        'engine/garment/grid.mjs','engine/cloth/topology.mjs','engine/character/rig-mesh.mjs','engine/hair/scalp.mjs',
        'engine/assets/canonical.mjs','tools/garment.mjs','assets/builtin/robot/character.json',
        '.incrementalTesting/simulation/garment-skinned.ps1') | Sort-Object -Unique)
    $hashes=[ordered]@{};foreach($file in $inputs){$hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}

    if(-not $SkipCook){
        # The cook refuses to overwrite a content-addressed object whose bytes differ, so a
        # stale cook has to go first. Deleted through the file API on purpose: this is a
        # generated directory under build/, and the pattern is limited to cooked objects.
        if(Test-Path -LiteralPath $cookedDirectory){
            foreach($stale in (Get-ChildItem -LiteralPath $cookedDirectory -File -Filter '*.ngcloth.json')){[System.IO.File]::Delete($stale.FullName)}
            $libraryFile=Join-Path $cookedDirectory 'library.json'
            if(Test-Path -LiteralPath $libraryFile){[System.IO.File]::Delete($libraryFile)}
        }
        # Captured into a variable rather than redirected: $LASTEXITCODE is only reliable here
        # when the native process is invoked directly.
        $cookOutput = & $Node tools/garment.mjs build/garment 2>&1
        $cookCode = $LASTEXITCODE
        Set-Content -LiteralPath (Join-Path $directory 'cook.log') -Value $cookOutput -Encoding utf8
        # Checked by artifact as well as by exit code: some hosts do not propagate
        # $LASTEXITCODE for a native child process, and a silent "success" with no library is
        # the failure mode that would otherwise let the binary test the previous cook.
        $cookLibrary = Join-Path $cookedDirectory 'library.json'
        if($cookCode -ne 0 -and $cookCode -ne $null){Get-Content (Join-Path $directory 'cook.log') -Tail 25;throw "Garment cook failed (exit $cookCode)."}
        if(-not(Test-Path -LiteralPath $cookLibrary)){Get-Content (Join-Path $directory 'cook.log') -Tail 25;throw 'Garment cook produced no library.json.'}

        # Cook again into a scratch directory and compare the library hash. A cook that is not
        # byte deterministic cannot be content-addressed, and the binary below would then be
        # validating a document that will not be the one shipped.
        $probeDirectory=Join-Path $directory 'cook-repeat'
        New-Item -ItemType Directory -Force -Path $probeDirectory | Out-Null
        foreach($stale in (Get-ChildItem -LiteralPath $probeDirectory -File)){[System.IO.File]::Delete($stale.FullName)}
        $probeOutput = & $Node tools/garment.mjs ('build/garment-reference/cook-repeat') 2>&1
        $probe = $probeOutput | Select-Object -Last 1
        $probeHash = ($probe | ConvertFrom-Json).libraryHash
        $firstHash = (Get-Content -LiteralPath $cookLibrary -Raw -Encoding UTF8 | ConvertFrom-Json).libraryHash
        if($probeHash -ne $firstHash){throw "Garment cook is not deterministic: $firstHash vs $probeHash"}
    }

    if(-not $SkipBuild){
        $arguments=@('-std=c++17','-Wall','-Wextra','-Wpedantic','-O2','-static','-DNOMINMAX')
        $compileOutput = & $compiler @arguments @sources -o $exe -lbcrypt 2>&1
        $compileCode = $LASTEXITCODE
        Set-Content -LiteralPath (Join-Path $directory 'compile.log') -Value $compileOutput -Encoding utf8
        if($compileCode -ne 0 -and $compileCode -ne $null){Get-Content (Join-Path $directory 'compile.log') -Tail 40;throw "Native garment build failed (exit $compileCode)."}
        if(-not(Test-Path -LiteralPath $exe)){throw 'Native garment build produced no executable.'}
        foreach($file in $inputs){if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hashes[$file]){throw "Garment input changed during build: $file"}}
        $hashes['build/garment-reference/native_garment_tests.exe']=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        foreach($file in (Get-ChildItem -LiteralPath $cookedDirectory -File)){$hashes['build/garment/'+$file.Name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
        [ordered]@{compilerHash=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash;sources=$hashes} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    } else {
        $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach($entry in $manifest.sources.PSObject.Properties){
            if(Test-Path -LiteralPath $entry.Key){
                if((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -ne $entry.Value){throw "Stale garment input: $($entry.Key)"}
            }
        }
    }

    $raw = & $exe --library build/garment/library.json
    $runCode = $LASTEXITCODE
    $evidence = $null
    try { $evidence = $raw | ConvertFrom-Json } catch { $evidence = $null }
    if($runCode -ne 0 -and $runCode -ne $null){throw "Native skinned garment reference failed (exit $runCode)."}
    if($null -eq $evidence -or $evidence.ok -ne $true){throw 'Native skinned garment reference produced no passing evidence document.'}
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Value $raw -Encoding utf8
    Write-Host ("Garment reference: groups={0} checks={1} poses={2} overPlanTarget={3}" -f `
        $evidence.groups,$evidence.checks,$evidence.poses.Count,$evidence.posesOverPlanPenetrationTarget)
} finally {
    Pop-Location
}

param([string]$OutputDirectory='build/gameplay')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $compiler=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    $archiver=Join-Path $root '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/llvm-ar.exe'
    if(-not(Test-Path -LiteralPath $compiler)){ & "$PSScriptRoot/bootstrap.ps1" }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $sources=@(Get-ChildItem -LiteralPath 'third_party/lua-5.4.9/src' -Filter '*.c' -File | Where-Object {$_.Name -notin @('lua.c','luac.c')})
    $native=@('engine/native/gameplay/world.cpp','engine/native/gameplay/runtime.cpp','engine/native/gameplay_snapshot.cpp','engine/native/gameplay_bridge.cpp')
    $inputs=@($sources.FullName)+$native+@('third_party/json.hpp')+@(Get-ChildItem -LiteralPath 'engine/native','third_party/lua-5.4.9/src' -Recurse -File | Where-Object {$_.Extension -in @('.h','.hpp') } | ForEach-Object {$_.FullName})
    $hashes=@{}; foreach($inputFile in $inputs){$hashes[$inputFile]=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash}
    $manifest=Join-Path $OutputDirectory 'inputs.json'
    $library=Join-Path $OutputDirectory 'nativegpu-gameplay.a'
    $signature=$hashes | ConvertTo-Json -Compress
    if((Test-Path -LiteralPath $manifest) -and (Test-Path -LiteralPath $library) -and (Get-Content -LiteralPath $manifest -Raw).Trim() -eq $signature){return}
    $objects=@()
    foreach($source in (@($sources.FullName)+$native)) {
        $objectFile=Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source))+'.o')
        $log=Join-Path $OutputDirectory 'compile.log'
        $previous=$ErrorActionPreference; $ErrorActionPreference='Continue'
        & $compiler '-x' 'c++' '-std=c++17' '-O2' '-DNOMINMAX' '-w' '-c' $source '-o' $objectFile *> $log
        $code=$LASTEXITCODE; $ErrorActionPreference=$previous
        if($code -ne 0){Get-Content -LiteralPath $log -Tail 70; throw "Gameplay compilation failed: $source"}
        $objects+=$objectFile
    }
    & $archiver rcs $library @objects
    if($LASTEXITCODE -ne 0){throw 'Gameplay archive failed'}
    Set-Content -LiteralPath $manifest -Value $signature -Encoding utf8
    Write-Host 'Built Lua 5.4.9 and gameplay runtime.'
} finally {Pop-Location}

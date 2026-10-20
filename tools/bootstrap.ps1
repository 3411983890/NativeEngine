$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$name = 'llvm-mingw-20261006-ucrt-x86_64'
$toolsDir = Join-Path $root '.tools'
$compiler = Join-Path $toolsDir "$name\bin\clang++.exe"
if (Test-Path -LiteralPath $compiler) {
    Write-Output $compiler
    exit 0
}
New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
$archive = Join-Path $toolsDir "$name.zip"
$digest = '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'
if (-not (Test-Path -LiteralPath $archive)) {
    Write-Host 'Downloading pinned portable C++ toolchain (191 MB)...'
    Invoke-WebRequest -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$name.zip" -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) {
    throw 'Toolchain checksum mismatch. Remove the incomplete archive and retry.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $toolsDir -Force
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Compiler missing after extraction.' }
Write-Output $compiler

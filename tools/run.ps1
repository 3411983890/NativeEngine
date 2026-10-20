$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\NativeGPU.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    & "$PSScriptRoot\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
Start-Process -FilePath $exe -WorkingDirectory $root

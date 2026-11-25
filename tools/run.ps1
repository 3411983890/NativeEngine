$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\NativeGPU.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    & "$PSScriptRoot\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
. (Join-Path $PSScriptRoot 'native-build-state.ps1')
try { Assert-NativeBuildCurrent } catch {
    & "$PSScriptRoot\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Native rebuild failed; refusing unverified outputs.' }
    Assert-NativeBuildCurrent
}
Start-Process -FilePath $exe -WorkingDirectory $root

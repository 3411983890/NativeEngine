param([int]$Frames = 240)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $executable = if (Test-Path -LiteralPath 'build\NativeGPU.exe') {
        (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
    } elseif (Test-Path -LiteralPath 'NativeGPU.exe') {
        (Resolve-Path -LiteralPath 'NativeGPU.exe').Path
    } else {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
        (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
    }
    New-Item -ItemType Directory -Force -Path 'reports' | Out-Null
    $process = Start-Process -FilePath $executable `
        -ArgumentList @('--probe','--frames',"$Frames",'--report','reports/probe.json') `
        -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Probe exited $($process.ExitCode)." }
    $report = Get-Content -LiteralPath 'reports\probe.json' -Raw | ConvertFrom-Json
    if (-not $report.success) { throw 'Probe reported success=false.' }
    $report.probes | ConvertTo-Json -Depth 6
    Write-Host "Probe report: reports\probe.json"
} finally {
    Pop-Location
}

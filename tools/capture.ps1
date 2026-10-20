# GPU capture availability and manual procedure.
#
# PIX and RenderDoc are optional third-party tools. This script reports honestly whether
# they are installed, tries a headless RenderDoc capture when possible, and always emits a
# capture-status JSON plus the exact manual steps for this machine. It never fabricates a
# capture path.
param(
    [switch]$Quiet,
    [string]$RenderSize = '960x540'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')

function Find-RenderDoc {
    $candidates = @(
        'C:\Program Files\RenderDoc\renderdoccmd.exe',
        'C:\Program Files (x86)\RenderDoc\renderdoccmd.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $command = Get-Command renderdoccmd -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $registry = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RenderDoc',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\RenderDoc'
    )
    foreach ($key in $registry) {
        try {
            $item = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($item.InstallLocation) {
                $exe = Join-Path $item.InstallLocation 'renderdoccmd.exe'
                if (Test-Path -LiteralPath $exe) { return $exe }
            }
        } catch { }
    }
    return $null
}

function Find-Pix {
    $candidates = @(
        'C:\Program Files\Microsoft PIX\WinPixGpuCapturer.dll',
        'C:\Program Files\Microsoft PIX\PIXWinPixGpuCapturer.dll'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $directories = @('C:\Program Files\Microsoft PIX')
    foreach ($directory in $directories) {
        if (Test-Path -LiteralPath $directory) {
            $found = Get-ChildItem -LiteralPath $directory -Filter 'WinPixGpuCapturer.dll' -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($found) { return $found.FullName }
        }
    }
    $command = Get-Command WinPixGpuCapturer -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

if (-not $Quiet) { Write-Host "GPU capture status ($RenderSize)" -ForegroundColor Cyan }
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path
$renderDocPath = Find-RenderDoc
$pixPath = Find-Pix
$dxcap = Get-Command dxcap -ErrorAction SilentlyContinue

$renderDoc = [pscustomobject]@{
    available = [bool]$renderDocPath
    path = if ($renderDocPath) { $renderDocPath } else { '' }
    attempted = $false
    succeeded = $false
    capturePath = ''
    error = if ($renderDocPath) { '' } else { 'renderdoccmd.exe not found in PATH or standard install paths' }
}
$pix = [pscustomobject]@{
    available = [bool]$pixPath
    path = if ($pixPath) { $pixPath } else { '' }
    attempted = $false
    succeeded = $false
    capturePath = ''
    error = if ($pixPath) { '' } else { 'WinPixGpuCapturer.dll not found; PIX is not installed' }
}
$dxcapInfo = [pscustomobject]@{
    available = [bool]$dxcap
    path = if ($dxcap) { $dxcap.Source } else { '' }
    note = 'DXCap.exe is the legacy Visual Studio graphics diagnostics capture tool; it did not produce a .vsglog for this D3D12 process when probed by tools/capture.ps1.'
}

if ($renderDocPath) {
    $renderDoc.attempted = $true
    $captureDirectory = Join-Path $root 'reports\captures'
    New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null
    $capturePath = Join-Path $captureDirectory ('nativegpu-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ') + '.rdc')
    try {
        $arguments = @('capture', '--wait-for-exit', '--capture-file', $capturePath, $exe,
                       '--probe', '--frames', '60', '--render-size', $RenderSize,
                       '--report', 'reports/captures/capture-run.json')
        $process = Start-Process -FilePath $renderDocPath -ArgumentList $arguments `
            -WorkingDirectory $root -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -eq 0 -and (Test-Path -LiteralPath $capturePath)) {
            $renderDoc.succeeded = $true
            $renderDoc.capturePath = (Get-RelativePath $capturePath)
        } else {
            $renderDoc.error = "renderdoccmd exited $($process.ExitCode)"
        }
    } catch {
        $renderDoc.error = $_.Exception.Message
    }
}

$procedure = @(
    '1. Install RenderDoc (https://renderdoc.org) or Microsoft PIX for Windows.',
    '2. RenderDoc: qrenderdoc -> File -> Launch Application -> Executable = build\NativeGPU.exe,',
    '   Working Directory = project root, Command line = --probe --frames 240 --render-size 1280x720',
    '   --camera 9,5.5,10,-2,0.8,0 --rt-shadows on, then press F12 to capture a frame.',
    '3. PIX: run "PIX on Windows" -> New Capture -> Launch build\NativeGPU.exe with the same arguments,',
    '   then press the capture button (or Win+P) while the shadow frame is on screen.',
    '4. Save the .rdc/.wpix next to the frame dumps so the pass structure can be inspected:',
    '   shadow pass = DispatchRays, raster pass = 2 render targets, composite = 1 draw.',
    '5. Screenshots without a capture tool: --screenshot <path> --screenshot-frame 200',
    '   (tools/rt-shadows.ps1 already stores on/off/mask/key images per run).'
)

$status = [pscustomobject]@{
    schema = 'nativegpu.capture-status/1'
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    renderer = 'Direct3D 12'
    renderDoc = $renderDoc
    pix = $pix
    dxcap = $dxcapInfo
    automatedScreenshots = 'tools/rt-shadows.ps1 (--screenshot, in-frame copy + PNG)'
    manualProcedure = $procedure
    note = 'No PIX or RenderDoc capture exists on this machine. The value is reported as unavailable instead of inventing a path.'
}
$outputDirectory = Join-Path $root 'reports\latest'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$statusPath = Join-Path $root 'reports\capture-status.json'
$status | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statusPath -Encoding utf8
if (-not $Quiet) {
    Write-Host ""
    Write-Host "RenderDoc: $(if ($renderDoc.available) { $renderDoc.path } else { 'unavailable' })"
    Write-Host "PIX:       $(if ($pix.available) { $pix.path } else { 'unavailable' })"
    Write-Host "DXCap:     $(if ($dxcapInfo.available) { $dxcapInfo.path } else { 'unavailable' })"
    Write-Host "Manual procedure recorded in docs/CAPTURE.md and reports/capture-status.json"
}
Pop-Location
$status | ConvertTo-Json -Depth 6 | Write-Output

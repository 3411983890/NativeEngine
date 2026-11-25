# Night acceptance capture: fixed camera, 60 frames, screenshot + performance evidence.
# This is the reproducible gate for the foggy-night traffic scene. It does not rebuild by
# default; pass -SkipBuild:$false or run tools/build.ps1 first when C++ changed.
param(
    [switch]$SkipBuild,
    [string]$Screenshot = 'reports/night/acceptance.png',
    [string]$Camera = '0,2.4,30,0,2.2,5',
    [string]$RenderSize = '1600x900',
    [int]$Frame = 30
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    New-Item -ItemType Directory -Force -Path 'reports/night' | Out-Null
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File 'tools/build.ps1' -SkipShaders
        if ($LASTEXITCODE -ne 0) { throw 'native build failed' }
    }
    if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { throw 'build/NativeGPU.exe is missing' }
    $arguments = @(
        '--scene','.screen/town.scene.json',
        '--textures','assets/textures',
        '--smoke','--frames','60',
        '--render-size',$RenderSize,
        '--camera',$Camera,
        '--screenshot',$Screenshot,
        '--screenshot-frame',"$Frame",
        '--report','reports/night/run.json'
    )
    $process = Start-Process -FilePath 'build\NativeGPU.exe' -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "NativeGPU exited with $($process.ExitCode)" }
    if (-not (Test-Path -LiteralPath $Screenshot)) { throw "screenshot missing: $Screenshot" }

    $run = Get-Content -LiteralPath 'reports/night/run.json' -Raw | ConvertFrom-Json
    $scene = Get-Content -LiteralPath '.screen/town.scene.json' -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath 'assets/textures/manifest.json' -Raw | ConvertFrom-Json
    if (-not $run.success) { throw "run failed: $($run.error)" }
    if ($run.probes.sceneLights.maxLights -lt 64) { throw 'light buffer did not report 64 slots' }
    if ($run.probes.reliability.drawCalls -lt 600) { throw 'traffic scene lost its draw calls' }

    $evidence = [ordered]@{
        schema = 'nativegpu.night-acceptance/1'
        scene = '.screen/town.scene.json'
        projectId = $scene.projectId
        revision = $scene.revision
        entities = @($scene.entities).Count
        materials = @($scene.materials.PSObject.Properties).Count
        textures = @($manifest.textures).Count
        textureBytes = $manifest.totalBytes
        textureMiB = [math]::Round($manifest.totalBytes / 1048576, 2)
        textureFormats = @($manifest.textures | ForEach-Object { $_.format } | Sort-Object -Unique)
        maxLights = $run.probes.sceneLights.maxLights
        screenshot = $Screenshot
        screenshotSha256 = (Get-FileHash -LiteralPath $Screenshot -Algorithm SHA256).Hash.ToLowerInvariant()
        camera = $Camera
        renderSize = $RenderSize
        frames = $run.frames
        drawCalls = $run.probes.reliability.drawCalls
        triangles = $run.probes.reliability.triangles
        gpuP50Ms = $run.gpuP50Ms
        gpuP95Ms = $run.gpuP95Ms
        adapter = $run.capabilities.adapter
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'reports/night/evidence.json' -Encoding utf8
    Write-Host ("Night acceptance: {0} entities, {1} materials, {2} textures ({3} MiB), {4} draws, GPU P50 {5} ms / P95 {6} ms" -f `
        $evidence.entities, $evidence.materials, $evidence.textures, $evidence.textureMiB,
        $evidence.drawCalls, [math]::Round($evidence.gpuP50Ms, 2), [math]::Round($evidence.gpuP95Ms, 2))
    Write-Host "Screenshot: $Screenshot"
    Write-Host 'Evidence:   reports/night/evidence.json'
} finally { Pop-Location }

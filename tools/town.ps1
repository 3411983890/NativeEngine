# Builds the town demo end to end: procedural source plates -> cooked .ngtex -> scene ->
# native capture -> evidence JSON. Run from the repository root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/town.ps1
param(
    [switch]$SkipBuild,
    [string]$Screenshot = 'reports/town/hero.png',
    [string]$Camera = '7,6,45,-2,2.6,2',
    [string]$RenderSize = '1600x900',
    [int]$Frame = 40,
    [string]$Format = 'auto',
    [double]$BudgetMiB = 4.5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    New-Item -ItemType Directory -Force -Path 'reports/town' | Out-Null

    Write-Host '[1/6] source plates'
    & node 'tools/town/gen-textures.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'texture plate generation failed' }

    Write-Host '[2/6] cook'
    $cookArguments = @('tools/textures.mjs', 'cook', '--source', 'assets/source/textures',
        '--out', 'assets/textures', '--report', 'reports/textures', '--size', '256',
        '--format', $Format)
    if ($BudgetMiB -gt 0) { $cookArguments += @('--budget-mib', "$BudgetMiB") }
    & node @cookArguments
    if ($LASTEXITCODE -ne 0) { throw 'texture cook failed' }

    Write-Host '[3/6] scene'
    & node 'tools/town/gen-scene.mjs'
    if ($LASTEXITCODE -ne 0) { throw 'scene generation failed' }

    Write-Host '[4/6] validate'
    $validation = & node 'tools/scene.mjs' validate '.screen/town.scene.json' | ConvertFrom-Json
    if (-not $validation.ok) { throw "scene validation failed: $($validation | ConvertTo-Json -Depth 6 -Compress)" }

    Write-Host '[5/6] native build'
    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File 'tools/build.ps1' -SkipShaders
        if ($LASTEXITCODE -ne 0) { throw 'native build failed' }
    }
    if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { throw 'build/NativeGPU.exe is missing' }

    Write-Host '[6/6] capture'
    $arguments = @(
        '--scene', '.screen/town.scene.json',
        '--textures', 'assets/textures',
        '--smoke',
        '--frames', '30',
        '--render-size', $RenderSize,
        '--camera', $Camera,
        '--screenshot', $Screenshot,
        '--screenshot-frame', "$Frame",
        '--report', 'reports/town/run.json'
    )
    $process = Start-Process -FilePath 'build\NativeGPU.exe' -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "NativeGPU exited with $($process.ExitCode)" }
    if (-not (Test-Path -LiteralPath $Screenshot)) { throw "screenshot missing: $Screenshot" }

    $scene = Get-Content -LiteralPath '.screen/town.scene.json' -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath 'assets/textures/manifest.json' -Raw | ConvertFrom-Json
    $run = Get-Content -LiteralPath 'reports/town/run.json' -Raw | ConvertFrom-Json
    $screenshotHash = (Get-FileHash -LiteralPath $Screenshot -Algorithm SHA256).Hash.ToLowerInvariant()
    $materialTextures = @($manifest.textures | Group-Object { $_.name }).Count
    $widths = @($manifest.textures | ForEach-Object { $_.width } | Sort-Object -Unique)
    $formats = @($manifest.textures | Group-Object { $_.format } | ForEach-Object { "$($_.Name):$($_.Count)" })
    $evidence = [ordered]@{
        schema = 'nativegpu.town-evidence/1'
        scene = '.screen/town.scene.json'
        projectId = $scene.projectId
        revision = $scene.revision
        entities = @($scene.entities).Count
        materials = @($scene.materials.PSObject.Properties).Count
        textureFiles = @($manifest.textures).Count
        textureBytes = $manifest.totalBytes
        textureMiB = [math]::Round($manifest.totalBytes / 1048576, 2)
        textureSize = $(if ($widths.Count -eq 1) { $widths[0] } else { 'mixed' })
        textureWidths = $widths
        textureFormatMix = $formats
        budgetBytes = $manifest.budgetBytes
        overBudget = $manifest.overBudget
        decisions = @($manifest.decisions).Count
        materialSets = $materialTextures
        screenshot = $Screenshot
        screenshotSha256 = $screenshotHash
        screenshotBytes = (Get-Item -LiteralPath $Screenshot).Length
        camera = $Camera
        renderSize = $RenderSize
        run = [ordered]@{
            ok = $run.success
            frames = $run.frames
            drawCalls = $run.probes.reliability.drawCalls
            triangles = $run.probes.reliability.triangles
            adapter = $run.capabilities.adapter
            shaderModel = $run.capabilities.shaderModel
            materialTableSlots = $run.capabilities.materialTableSlots
            materialPath = $run.capabilities.materialPath
            materialBindless = $run.capabilities.materialBindless
            materialRecords = $run.capabilities.materialRecords
            materialTextureSlices = $run.capabilities.materialTextureSlices
            materialTextureMips = $run.capabilities.materialTextureMips
            gpuP50Ms = $run.gpuP50Ms
            cpuP50Ms = $run.cpuP50Ms
        }
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'reports/town/evidence.json' -Encoding utf8
    Write-Host ("Town: {0} entities, {1} materials, {2} textures, {3} MiB, {4} draws" -f `
        $evidence.entities, $evidence.materials, $evidence.textureFiles, $evidence.textureMiB, $evidence.run.drawCalls)
    Write-Host "Screenshot: $Screenshot"
    Write-Host "Evidence:   reports/town/evidence.json"
} finally { Pop-Location }

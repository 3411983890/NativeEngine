# Physics + destruction evidence gate.
#
# Runs the authored lab scene twice from the same build and camera:
#   1. intact: no demo actions, screenshot at frame 30
#   2. destroyed: three projectile impacts and one radial blast, screenshot at frame 290
# It then verifies report counters, image difference, a deterministic repeat, and writes
# reports/physics-evidence.json. This is evidence for the implementation in
# engine/native/physics.cpp and the destruction layer in engine/native/game.cpp.
param([switch]$SkipValidation)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    Add-Type -AssemblyName System.Drawing
    $outDir = Join-Path $root 'reports\physics'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $exe = Join-Path $root 'build\NativeGPU.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw 'build/NativeGPU.exe is missing; run tools/build.ps1 first.' }
    $scene = '.screen/physics-lab.scene.json'
    $camera = '8.0,4.5,10.5,0.0,1.4,-2.0'

    if (-not $SkipValidation) {
        $validation = & node 'tools/scene.mjs' validate $scene 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Scene validation failed: $validation" }
        $parsedValidation = $validation | ConvertFrom-Json
        if (-not $parsedValidation.ok -or -not $parsedValidation.result.valid) {
            throw 'physics-lab.scene.json failed validation.'
        }
        $lint = & node 'tools/scene.mjs' lint $scene --json 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Scene lint failed: $lint" }
        $parsedLint = $lint | ConvertFrom-Json
        if (-not $parsedLint.ok -or -not $parsedLint.result.ok) {
            throw 'physics-lab.scene.json has lint errors.'
        }
    }

    function Invoke-Native([string[]]$Arguments) {
        $process = Start-Process -FilePath $exe -ArgumentList $Arguments -WorkingDirectory $root -PassThru -Wait
        if ($process.ExitCode -ne 0) {
            throw "NativeGPU exited with $($process.ExitCode): $($Arguments -join ' ')"
        }
    }

    $intactPng = Join-Path $outDir 'intact.png'
    $brokenPng = Join-Path $outDir 'destroyed.png'
    $intactReport = Join-Path $outDir 'intact.json'
    $brokenReport = Join-Path $outDir 'destroyed.json'
    $repeatReport = Join-Path $outDir 'destroyed-repeat.json'
    foreach ($file in @($intactPng, $brokenPng, $intactReport, $brokenReport, $repeatReport)) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }

    # 1. Intact baseline. --smoke exits after --frames; --no-stress keeps the camera and
    # renderer lifecycle fixed. The screenshot is taken before any authored action.
    Invoke-Native @('--scene', $scene, '--smoke', '--no-stress', '--frames', '240',
        '--camera', $camera, '--render-size', '1280x720',
        '--screenshot', 'reports/physics/intact.png', '--screenshot-frame', '30',
        '--report', 'reports/physics/intact.json')

    # 2. Destruction run. The host fires at frames 45/85/125 and detonates at 165.
    Invoke-Native @('--scene', $scene, '--physics-demo', '--frames', '320',
        '--camera', $camera, '--render-size', '1280x720',
        '--screenshot', 'reports/physics/destroyed.png', '--screenshot-frame', '290',
        '--report', 'reports/physics/destroyed.json')

    # 3. Same simulation, no screenshot: final digest must match the destruction run.
    Invoke-Native @('--scene', $scene, '--physics-demo', '--frames', '320',
        '--camera', $camera, '--render-size', '1280x720',
        '--report', 'reports/physics/destroyed-repeat.json')

    $intact = Get-Content -LiteralPath $intactReport -Raw | ConvertFrom-Json
    $broken = Get-Content -LiteralPath $brokenReport -Raw | ConvertFrom-Json
    $repeat = Get-Content -LiteralPath $repeatReport -Raw | ConvertFrom-Json

    function Compare-Png([string]$LeftPath, [string]$RightPath) {
        $left = [System.Drawing.Bitmap]::FromFile($LeftPath)
        $right = [System.Drawing.Bitmap]::FromFile($RightPath)
        try {
            if ($left.Width -ne $right.Width -or $left.Height -ne $right.Height) {
                throw 'Screenshots differ in size.'
            }
            $width = 192
            $height = 108
            $leftSmall = New-Object System.Drawing.Bitmap($width, $height)
            $rightSmall = New-Object System.Drawing.Bitmap($width, $height)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($leftSmall)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
                $graphics.DrawImage($left, 0, 0, $width, $height)
                $graphics.Dispose()
                $graphics = [System.Drawing.Graphics]::FromImage($rightSmall)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
                $graphics.DrawImage($right, 0, 0, $width, $height)
                $graphics.Dispose()
                [double]$sum = 0
                [long]$changed = 0
                $pixels = $width * $height
                for ($y = 0; $y -lt $height; ++$y) {
                    for ($x = 0; $x -lt $width; ++$x) {
                        $a = $leftSmall.GetPixel($x, $y)
                        $b = $rightSmall.GetPixel($x, $y)
                        $delta = [Math]::Abs($a.R - $b.R) + [Math]::Abs($a.G - $b.G) + [Math]::Abs($a.B - $b.B)
                        $sum += $delta / 3.0
                        if ($delta -gt 18) { ++$changed }
                    }
                }
                return [pscustomobject]@{
                    width = $left.Width
                    height = $left.Height
                    meanAbsError = [Math]::Round($sum / $pixels, 4)
                    changedPercent = [Math]::Round(100.0 * $changed / $pixels, 3)
                    sample = "${width}x${height}"
                }
            } finally {
                $leftSmall.Dispose()
                $rightSmall.Dispose()
            }
        } finally {
            $left.Dispose()
            $right.Dispose()
        }
    }

    $image = Compare-Png $intactPng $brokenPng
    $intactHash = (Get-FileHash -LiteralPath $intactPng -Algorithm SHA256).Hash.ToLowerInvariant()
    $brokenHash = (Get-FileHash -LiteralPath $brokenPng -Algorithm SHA256).Hash.ToLowerInvariant()
    $sceneHash = (Get-FileHash -LiteralPath $scene -Algorithm SHA256).Hash.ToLowerInvariant()

    $checks = [System.Collections.Generic.List[object]]::new()
    function Add-Check([string]$Name, [bool]$Ok, [string]$Detail) {
        $checks.Add([pscustomobject]@{ name = $Name; ok = $Ok; detail = $Detail })
    }
    Add-Check 'intact-scene' ($intact.success -and $intact.physics.destructibles -ge 1 `
        -and $intact.physics.intactDestructibles -eq $intact.physics.destructibles `
        -and $intact.physics.fractures -eq 0) `
        "intact=$($intact.physics.intactDestructibles)/$($intact.physics.destructibles) fractures=$($intact.physics.fractures)"
    Add-Check 'rigid-world-built' ($intact.physics.bodies -ge 15 -and $intact.physics.dynamicBodies -ge 5) `
        "bodies=$($intact.physics.bodies) dynamic=$($intact.physics.dynamicBodies)"
    Add-Check 'projectiles-fired' ($broken.physics.projectilesFired -eq 3) "projectiles=$($broken.physics.projectilesFired)"
    Add-Check 'fractures-observed' ($broken.physics.fractures -ge 1 -and $broken.physics.intactDestructibles -eq 0) `
        "fractures=$($broken.physics.fractures) intact=$($broken.physics.intactDestructibles)"
    Add-Check 'structural-collapse' ($broken.physics.structuralCollapses -ge 2) "collapses=$($broken.physics.structuralCollapses)"
    Add-Check 'debris-spawned' ($broken.physics.fragmentsSpawned -ge 60 -and $broken.physics.debris -ge 50) `
        "fragments=$($broken.physics.fragmentsSpawned) activeDebris=$($broken.physics.debris)"
    Add-Check 'impact-binding' ($broken.physics.impacts -ge 1 -and $broken.physics.peakImpactImpulse -gt 1) `
        "impacts=$($broken.physics.impacts) peak=$($broken.physics.peakImpactImpulse)"
    Add-Check 'deterministic-repeat' ($broken.determinism.finalHash -eq $repeat.determinism.finalHash) `
        "hash=$($broken.determinism.finalHash)"
    Add-Check 'image-changed' ($intactHash -ne $brokenHash -and $image.meanAbsError -ge 1.0 -and $image.changedPercent -ge 2.0) `
        "meanAbs=$($image.meanAbsError) changed=$($image.changedPercent)%"
    Add-Check 'step-budget' ($broken.physics.averageStepMs -lt 3.0 -and $broken.physics.maxStepMs -lt 25.0) `
        "avg=$($broken.physics.averageStepMs)ms max=$($broken.physics.maxStepMs)ms"
    Add-Check 'no-errors' ($intact.error -eq '' -and $broken.error -eq '') `
        "intactError='$($intact.error)' brokenError='$($broken.error)'"

    $ok = -not ($checks | Where-Object { -not $_.ok })
    $evidence = [ordered]@{
        ok = $ok
        generatedAt = (Get-Date).ToUniversalTime().ToString('o')
        version = $broken.version
        commit = $broken.build.commit
        worktreeDirty = $broken.build.dirty
        scene = $scene
        sceneSha256 = $sceneHash
        camera = $camera
        resolution = '1280x720'
        intact = [ordered]@{
            success = $intact.success
            report = 'reports/physics/intact.json'
            screenshot = 'reports/physics/intact.png'
            screenshotSha256 = $intactHash
            physics = $intact.physics
        }
        destroyed = [ordered]@{
            success = $broken.success
            report = 'reports/physics/destroyed.json'
            screenshot = 'reports/physics/destroyed.png'
            screenshotSha256 = $brokenHash
            finalHash = $broken.determinism.finalHash
            physics = $broken.physics
        }
        repeatFinalHash = $repeat.determinism.finalHash
        image = $image
        checks = $checks
        limitations = $broken.physics.limitations
    }
    $evidencePath = Join-Path $root 'reports\physics-evidence.json'
    $json = $evidence | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($evidencePath, $json, (New-Object Text.UTF8Encoding($false)))
    $checks | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "Physics evidence: $evidencePath"
    Write-Host ("Physics: {0} bodies, {1} fragments, {2} fractures, {3} structural collapses" -f `
        $broken.physics.bodies, $broken.physics.fragmentsSpawned, $broken.physics.fractures,
        $broken.physics.structuralCollapses)
    if (-not $ok) { throw 'Physics evidence checks failed.' }
} finally {
    Pop-Location
}

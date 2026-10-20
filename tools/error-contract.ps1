# Declared image error contracts (R07 / INN-03).
#
# tests/image-contracts.json owns the thresholds. Each contract declares two runs,
# one metric and its tolerance. This script executes both runs for real, measures
# the screenshots with System.Drawing, and fails when a measurement leaves the
# declared interval. Evidence (measured values, thresholds, pass/fail, paths) is
# written to reports/<version>/<commit12>/<stamp>/error-contract-evidence.json.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/error-contract.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/error-contract.ps1 -Contract shadow-on-vs-off
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/error-contract.ps1 -SkipBuild
#
# Exit 0 = every selected contract passed. Exit 1 = at least one violation.
param(
    [string]$Contract = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')
Add-Type -AssemblyName System.Drawing

$contractsRelative = 'tests/image-contracts.json'
$contractsPath = Join-Path $root $contractsRelative
$runnerFlags = @('--probe', '--scene', '--frames', '--render-size', '--camera',
                 '--screenshot-frame', '--screenshot', '--report')
$metricRanges = @{
    meanAbsError = @(0.0, 255.0)
    changedPercent = @(0.0, 100.0)
    maxChannelDelta = @(0.0, 255.0)
}

function Get-ImageData([string]$path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($path)
    $width = $bitmap.Width
    $height = $bitmap.Height
    $rect = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
    $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $locked.Stride
    $bytes = New-Object byte[] ($stride * $height)
    [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $bytes, 0, $bytes.Length)
    $bitmap.UnlockBits($locked)
    $bitmap.Dispose()
    return [pscustomobject]@{ bytes = $bytes; stride = $stride; width = $width; height = $height }
}

# One full-resolution pass over both images. Every pixel and all three colour
# channels are inspected: sampling would let a small regression slip through.
function Measure-Pair([object]$left, [object]$right, [int]$pixelThreshold) {
    if ($left.width -ne $right.width -or $left.height -ne $right.height) {
        throw ("image sizes differ: {0}x{1} vs {2}x{3}" -f $left.width, $left.height, $right.width, $right.height)
    }
    $sum = 0.0
    $changed = 0
    $max = 0
    $total = 0
    for ($y = 0; $y -lt $left.height; ++$y) {
        $row = $y * $left.stride
        for ($x = 0; $x -lt $left.width; ++$x) {
            $index = $row + $x * 4
            $dr = [Math]::Abs([int]$left.bytes[$index] - [int]$right.bytes[$index])
            $dg = [Math]::Abs([int]$left.bytes[$index + 1] - [int]$right.bytes[$index + 1])
            $db = [Math]::Abs([int]$left.bytes[$index + 2] - [int]$right.bytes[$index + 2])
            $sum += ($dr + $dg + $db)
            $pixelMax = $dr
            if ($dg -gt $pixelMax) { $pixelMax = $dg }
            if ($db -gt $pixelMax) { $pixelMax = $db }
            if ($pixelMax -gt $pixelThreshold) { ++$changed }
            if ($pixelMax -gt $max) { $max = $pixelMax }
            ++$total
        }
    }
    return [pscustomobject]@{
        meanAbsError = [Math]::Round($sum / (3.0 * $total), 4)
        changedPercent = [Math]::Round(100.0 * $changed / $total, 2)
        maxChannelDelta = $max
        changedPixels = $changed
        totalPixels = $total
    }
}

function Format-Tolerance([object]$tolerance) {
    if ($tolerance -is [System.Management.Automation.PSCustomObject]) {
        return ("min {0} max {1}" -f [double]$tolerance.min, [double]$tolerance.max)
    }
    return "$tolerance"
}

function Test-Measurement([string]$expect, [object]$tolerance, [double]$measured) {
    if ($expect -eq 'identical') {
        $limit = [double]$tolerance
        if ($measured -le $limit) {
            return [pscustomobject]@{ pass = $true; detail = '' }
        }
        return [pscustomobject]@{
            pass = $false
            detail = ("measured {0} exceeds tolerance {1}" -f $measured, $limit)
        }
    }
    $floor = [double]$tolerance.min
    $ceiling = [double]$tolerance.max
    if ($measured -lt $floor) {
        return [pscustomobject]@{
            pass = $false
            detail = ("measured {0} is below floor {1}" -f $measured, $floor)
        }
    }
    if ($measured -gt $ceiling) {
        return [pscustomobject]@{
            pass = $false
            detail = ("measured {0} is above ceiling {1}" -f $measured, $ceiling)
        }
    }
    return [pscustomobject]@{ pass = $true; detail = '' }
}

function Assert-RunArgs([object[]]$arguments, [string]$label) {
    foreach ($arg in $arguments) {
        if ($null -eq $arg -or $arg -isnot [string]) {
            throw "$label arguments must all be strings."
        }
        if ($script:runnerFlags -contains $arg) {
            throw "$label must not set $arg; tools/error-contract.ps1 owns the capture flags."
        }
        if ($arg -match '^[A-Za-z]:' -or $arg -match '^\\\\') {
            throw "$label must use relative paths only: $arg"
        }
    }
}

function Assert-ContractDefinition([object]$definition) {
    if ($definition.schema -ne 'nativegpu.image-contracts/1') {
        throw "Unsupported contract schema '$($definition.schema)'; expected nativegpu.image-contracts/1."
    }
    if (-not $definition.scene) { throw "Contract file has no scene." }
    if (-not $definition.PSObject.Properties['frames']) { throw "Contract file has no frames." }
    if (-not $definition.PSObject.Properties['pixelThreshold']) {
        throw "Contract file has no pixelThreshold."
    }
    if ($definition.scene -match '^[A-Za-z]:' -or $definition.scene -match '^\\\\') {
        throw "Contract scene must be a project-relative path: $($definition.scene)"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $script:root $definition.scene))) {
        throw "Contract scene does not exist: $($definition.scene)"
    }
    $frames = [int]$definition.frames
    if ($frames -lt 1) { throw "Contract frames must be positive." }
    $pixelThreshold = [int]$definition.pixelThreshold
    if ($pixelThreshold -lt 0 -or $pixelThreshold -gt 255) {
        throw "Contract pixelThreshold must be between 0 and 255."
    }
    $contracts = @($definition.contracts)
    if ($contracts.Count -eq 0) { throw "Contract file contains no contracts." }
    $seen = @{}
    $index = 0
    foreach ($spec in $contracts) {
        ++$index
        $label = "contract #$index"
        foreach ($field in @('id', 'description', 'reference', 'candidate', 'metric', 'expect',
                             'tolerance', 'resolution', 'camera', 'screenshotFrame')) {
            if (-not $spec.PSObject.Properties[$field]) {
                throw "$label is missing field '$field'."
            }
        }
        if (-not $spec.id) { throw "$label has an empty id." }
        $label = "contract '$($spec.id)'"
        if ($seen.ContainsKey($spec.id)) { throw "Duplicate contract id '$($spec.id)'." }
        $seen[$spec.id] = $true
        if (-not $spec.description) { throw "$label has an empty description." }
        if (-not $metricRanges.ContainsKey($spec.metric)) {
            throw "$label metric '$($spec.metric)' is not one of meanAbsError, changedPercent, maxChannelDelta."
        }
        if ($spec.expect -ne 'identical' -and $spec.expect -ne 'changed') {
            throw "$label expect must be 'identical' or 'changed', not '$($spec.expect)'."
        }
        $range = $metricRanges[$spec.metric]
        if ($spec.expect -eq 'identical') {
            if ($spec.tolerance -is [System.Management.Automation.PSCustomObject]) {
                throw "$label is identical and needs a numeric tolerance, not an object."
            }
            if ($null -eq $spec.tolerance) { throw "$label has a null tolerance." }
            $limit = [double]$spec.tolerance
            if ($limit -lt $range[0] -or $limit -gt $range[1]) {
                throw "$label tolerance $limit is outside the metric range $($range[0])..$($range[1])."
            }
        } else {
            if (-not ($spec.tolerance -is [System.Management.Automation.PSCustomObject])) {
                throw "$label is changed and needs a tolerance object with min and max."
            }
            if (-not $spec.tolerance.PSObject.Properties['min'] -or
                -not $spec.tolerance.PSObject.Properties['max']) {
                throw "$label tolerance must contain min and max."
            }
            if ($null -eq $spec.tolerance.min -or $null -eq $spec.tolerance.max) {
                throw "$label tolerance min and max must not be null."
            }
            $floor = [double]$spec.tolerance.min
            $ceiling = [double]$spec.tolerance.max
            if ($floor -gt $ceiling) {
                throw "$label tolerance min $floor is greater than max $ceiling."
            }
            if ($floor -lt $range[0] -or $ceiling -gt $range[1]) {
                throw "$label tolerance $floor..$ceiling is outside the metric range $($range[0])..$($range[1])."
            }
        }
        if ($spec.resolution -notmatch '^[0-9]+x[0-9]+$') {
            throw "$label resolution must look like 1280x720, not '$($spec.resolution)'."
        }
        if ($spec.camera -notmatch '^-?[0-9]+(\.[0-9]+)?(,-?[0-9]+(\.[0-9]+)?){5}$') {
            throw "$label camera must be x,y,z,tx,ty,tz, not '$($spec.camera)'."
        }
        $screenshotFrame = [int]$spec.screenshotFrame
        if ($screenshotFrame -lt 1) { throw "$label screenshotFrame must be positive." }
        if ($screenshotFrame + 2 -gt $frames) {
            throw "$label screenshotFrame $screenshotFrame needs at least $($screenshotFrame + 2) frames; the file declares $frames."
        }
        Assert-RunArgs -arguments @($spec.reference) -label "$label reference"
        Assert-RunArgs -arguments @($spec.candidate) -label "$label candidate"
    }
}

$definition = Get-Content -LiteralPath $contractsPath -Raw | ConvertFrom-Json
Assert-ContractDefinition $definition
$frames = [int]$definition.frames
$pixelThreshold = [int]$definition.pixelThreshold

$selected = @($definition.contracts)
if ($Contract) {
    $selected = @($selected | Where-Object { $_.id -eq $Contract })
    if ($selected.Count -eq 0) {
        throw "No contract with id '$Contract' in $contractsRelative."
    }
    if ($selected.Count -gt 1) {
        throw "Contract id '$Contract' is not unique in $contractsRelative."
    }
}

if (-not $SkipBuild -or -not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    throw 'build\NativeGPU.exe is missing after the build.'
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path

$context = New-ReportContext -Mode 'error-contract'
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Invoke-Capture {
    param(
        [Parameter(Mandatory)][object]$CaptureContract,
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][object[]]$RunArgs
    )
    $reportPath = Join-Path $stage "$Tag.json"
    $imagePath = Join-Path $stage "$Tag.png"
    # Start-Process joins ArgumentList without quoting, so every path handed to
    # the executable is project-relative: the project directory contains spaces.
    $arguments = @(
        '--probe',
        '--scene', $definition.scene,
        '--frames', "$frames",
        '--render-size', $CaptureContract.resolution,
        '--camera', $CaptureContract.camera,
        '--screenshot-frame', "$($CaptureContract.screenshotFrame)"
    ) + @($RunArgs) + @(
        '--screenshot', (Get-RelativePath $imagePath),
        '--report', (Get-RelativePath $reportPath)
    )
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "capture '$Tag' exited $($process.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $reportPath)) {
        throw "capture '$Tag' wrote no report: $(Get-RelativePath $reportPath)"
    }
    if (-not (Test-Path -LiteralPath $imagePath)) {
        throw "capture '$Tag' wrote no screenshot: $(Get-RelativePath $imagePath)"
    }
    $data = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (-not [bool]$data.success) {
        throw "capture '$Tag' reported success=false: $($data.error)"
    }
    $expectedResolution = $CaptureContract.resolution -split 'x'
    $actualResolution = @($data.resolution)
    if ($actualResolution.Count -ne 2 -or
        [int]$actualResolution[0] -ne [int]$expectedResolution[0] -or
        [int]$actualResolution[1] -ne [int]$expectedResolution[1]) {
        throw "capture '$Tag' rendered '$($data.resolution -join 'x')', expected '$($CaptureContract.resolution)'."
    }
    if (-not $data.rtShadows) {
        throw "capture '$Tag' report has no rtShadows block."
    }
    if ([int]$data.rtShadows.screenshotFrame -ne [int]$CaptureContract.screenshotFrame) {
        throw "capture '$Tag' used screenshot frame $($data.rtShadows.screenshotFrame), expected $($CaptureContract.screenshotFrame)."
    }
    if (-not [bool]$data.rtShadows.screenshotTaken) {
        throw "capture '$Tag' did not take a screenshot: $($data.rtShadows.screenshotError)"
    }
    return [pscustomobject]@{
        data = $data
        report = $reportPath
        image = $imagePath
        arguments = $arguments
    }
}

Write-Host ("Image error contracts ({0}, {1} selected, scene {2}, {3} frames, pixel threshold {4})" -f `
    $contractsRelative, $selected.Count, $definition.scene, $frames, $pixelThreshold) -ForegroundColor Cyan
$checks = New-Object System.Collections.Generic.List[object]
$violations = New-Object System.Collections.Generic.List[object]
$results = New-Object System.Collections.Generic.List[object]

foreach ($spec in $selected) {
    $reference = Invoke-Capture -CaptureContract $spec -Tag "$($spec.id)-reference" `
        -RunArgs @($spec.reference)
    $candidate = Invoke-Capture -CaptureContract $spec -Tag "$($spec.id)-candidate" `
        -RunArgs @($spec.candidate)
    $referenceArtifact = Publish-Artifact -Context $context -SourcePath $reference.image `
        -Name "$($spec.id)-reference.png"
    $candidateArtifact = Publish-Artifact -Context $context -SourcePath $candidate.image `
        -Name "$($spec.id)-candidate.png"

    $left = Get-ImageData $reference.image
    $right = Get-ImageData $candidate.image
    $metrics = Measure-Pair -left $left -right $right -pixelThreshold $pixelThreshold
    $measuredValue = $metrics.PSObject.Properties[$spec.metric].Value
    $measured = [double]$measuredValue
    $verdict = Test-Measurement -expect $spec.expect -tolerance $spec.tolerance -measured $measured
    $toleranceText = Format-Tolerance $spec.tolerance

    $checks.Add([pscustomobject]@{
        contract = $spec.id
        metric = $spec.metric
        measured = $measured
        expected = $spec.expect
        tolerance = $spec.tolerance
        ok = [bool]$verdict.pass
        detail = $verdict.detail
    })
    if ($verdict.pass) {
        Write-Host ("  ok   {0,-28} {1}={2} (expected {3}, tolerance {4})" -f `
            $spec.id, $spec.metric, $measured, $spec.expect, $toleranceText) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL {0,-28} {1}={2} (expected {3}, tolerance {4}) - {5}" -f `
            $spec.id, $spec.metric, $measured, $spec.expect, $toleranceText, $verdict.detail) `
            -ForegroundColor Red
        $violations.Add([pscustomobject]@{
            contract = $spec.id
            metric = $spec.metric
            measured = $measured
            expected = $spec.expect
            tolerance = $spec.tolerance
        })
    }

    $results.Add([pscustomobject]@{
        id = $spec.id
        description = $spec.description
        metric = $spec.metric
        expect = $spec.expect
        tolerance = $spec.tolerance
        measured = $measured
        pass = [bool]$verdict.pass
        detail = $verdict.detail
        metrics = $metrics
        referenceArgs = @($spec.reference)
        candidateArgs = @($spec.candidate)
        resolution = $spec.resolution
        camera = $spec.camera
        screenshotFrame = [int]$spec.screenshotFrame
        images = [pscustomobject]@{
            reference = (Get-RelativePath $referenceArtifact)
            candidate = (Get-RelativePath $candidateArtifact)
        }
        stageImages = [pscustomobject]@{
            reference = (Get-RelativePath $reference.image)
            candidate = (Get-RelativePath $candidate.image)
        }
        reports = [pscustomobject]@{
            reference = (Get-RelativePath $reference.report)
            candidate = (Get-RelativePath $candidate.report)
        }
        imageSha256 = [pscustomobject]@{
            reference = (Get-FileHash -LiteralPath $reference.image -Algorithm SHA256).Hash.ToLowerInvariant()
            candidate = (Get-FileHash -LiteralPath $candidate.image -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        reportFields = [pscustomobject]@{
            frames = [int]$reference.data.frames
            resolution = $spec.resolution
            adapter = $reference.data.capabilities.adapter
            referenceSuccess = [bool]$reference.data.success
            candidateSuccess = [bool]$candidate.data.success
            referenceScreenshotTaken = [bool]$reference.data.rtShadows.screenshotTaken
            candidateScreenshotTaken = [bool]$candidate.data.rtShadows.screenshotTaken
        }
    })
}

$success = ($violations.Count -eq 0)
$summary = [pscustomobject]@{
    schema = 'nativegpu.error-contract/1'
    version = $context.version
    commit = $context.commit
    worktreeDirty = $context.dirty
    contractsFile = $contractsRelative
    contractsFileSha256 = (Get-FileHash -LiteralPath $contractsPath -Algorithm SHA256).Hash.ToLowerInvariant()
    scene = $definition.scene
    sceneSha256 = $context.sceneHash
    frames = $frames
    pixelThreshold = $pixelThreshold
    measurement = $definition.measurement
    adapter = $context.adapter
    contracts = $results
    violations = $violations
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'error-contract-evidence.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('error-contract-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Image error contracts: {0} checks, {1} failed" -f $checks.Count, $violations.Count)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

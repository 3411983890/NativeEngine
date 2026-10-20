# Image regression gate with an accepted baseline (Q08 / INN-11).
#
# tools/error-contract.ps1 compares two runs of the same build. This script covers the
# other half: it captures one fixed-view screenshot today and compares it against an
# accepted baseline PNG from an earlier commit. Four metrics are measured on the full
# frame: meanAbsError, changedPercent, maxChannelDelta and an 8x8 block SSIM
# approximation (luminance only, mean over blocks). Thresholds live in
# tests/image-baselines.json; the PNGs and evidence live under the gitignored reports/.
#
# -Accept is the explicit human step: it captures, writes reports/image-baselines/<id>.png
# and records source commit + accepted-at + sha256 in tests/image-baselines.json. It never
# fails on metric drift (accepting is the decision); it does require a valid capture.
#
# Without -Accept the script is a gate: a missing/tampered baseline or any metric outside
# its declared bounds exits 1 and prints the offending metric.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Accept
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Baseline courtyard-default -SkipBuild
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Baselines reports/tmp/image-baselines-negative.json -SkipBuild
#
# Exit 0 = capture valid and every tolerance satisfied (or -Accept succeeded).
# Exit 1 = tolerance violation, missing/tampered baseline, or invalid configuration.
param(
    [string]$Baseline = '',
    [string]$Baselines = 'tests/image-baselines.json',
    [switch]$Accept,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
. (Join-Path $PSScriptRoot 'report-lib.ps1')
Add-Type -AssemblyName System.Drawing

# Metric identities, directions and legal ranges. Every metric is read in one full-frame
# pass; there is no sampling. Lower is better for the first three, higher is better for
# SSIM, but every metric may declare either a floor, a ceiling, or both. That keeps the
# comparison code symmetric and lets a test declare an impossible floor (negative test).
$script:metricOrder = @('meanAbsError', 'changedPercent', 'maxChannelDelta', 'ssim')
$script:metricRanges = [ordered]@{
    meanAbsError    = @{ min = 0.0;   max = 255.0 }
    changedPercent  = @{ min = 0.0;   max = 100.0 }
    maxChannelDelta = @{ min = 0.0;   max = 255.0 }
    ssim            = @{ min = -1.0;  max = 1.0 }
}

# SSIM approximation constants. The JSON repeats them so a reader of the baseline file can
# see the formula it was accepted with; Assert-BaselineDocument refuses any drift from
# these values, so the metric cannot silently change meaning.
$script:ssimBlockSize = 8
$script:ssimK1 = 0.01
$script:ssimK2 = 0.03
$script:ssimDynamicRange = 255.0
$script:ssimLumaR = 0.2126
$script:ssimLumaG = 0.7152
$script:ssimLumaB = 0.0722
$script:ssimC1 = [Math]::Pow($ssimK1 * $ssimDynamicRange, 2)   # 6.5025
$script:ssimC2 = [Math]::Pow($ssimK2 * $ssimDynamicRange, 2)   # 58.5225
$script:diffAmplify = 4

$runnerFlags = @('--probe', '--scene', '--frames', '--render-size', '--camera',
                 '--screenshot-frame', '--screenshot', '--report')

function ConvertTo-MetricNumber([object]$value, [string]$label) {
    if ($null -eq $value) { throw "$label is null." }
    if ($value -is [bool]) { throw "$label must be numeric, got a boolean." }
    try { return [double]$value } catch { throw "$label must be numeric, got '$value'." }
}

function Format-MetricValue([string]$name, [double]$value) {
    if ($name -eq 'maxChannelDelta') {
        return ([Math]::Round($value, 0)).ToString('0', [System.Globalization.CultureInfo]::InvariantCulture)
    }
    if ($name -eq 'ssim') {
        return $value.ToString('0.00000000', [System.Globalization.CultureInfo]::InvariantCulture)
    }
    if ($name -eq 'changedPercent') {
        return $value.ToString('0.##', [System.Globalization.CultureInfo]::InvariantCulture)
    }
    return $value.ToString('0.####', [System.Globalization.CultureInfo]::InvariantCulture)
}

function Format-Bounds([string]$name, [object]$bounds) {
    $parts = New-Object System.Collections.Generic.List[string]
    if ($null -ne $bounds.PSObject.Properties['min']) {
        $parts.Add('min ' + (Format-MetricValue $name ([double]$bounds.min)))
    }
    if ($null -ne $bounds.PSObject.Properties['max']) {
        $parts.Add('max ' + (Format-MetricValue $name ([double]$bounds.max)))
    }
    return ($parts -join ', ')
}

function Test-MetricBounds([string]$name, [double]$measured, [object]$bounds) {
    $lower = $null
    $upper = $null
    if ($null -ne $bounds.PSObject.Properties['min']) { $lower = [double]$bounds.min }
    if ($null -ne $bounds.PSObject.Properties['max']) { $upper = [double]$bounds.max }
    if ($null -ne $lower -and $measured -lt $lower) {
        return [pscustomobject]@{
            ok = $false
            detail = ("measured {0} is below floor {1}" -f (Format-MetricValue $name $measured),
                                                          (Format-MetricValue $name $lower))
        }
    }
    if ($null -ne $upper -and $measured -gt $upper) {
        return [pscustomobject]@{
            ok = $false
            detail = ("measured {0} exceeds ceiling {1}" -f (Format-MetricValue $name $measured),
                                                            (Format-MetricValue $name $upper))
        }
    }
    return [pscustomobject]@{ ok = $true; detail = '' }
}

function Assert-MetricDefinition([object]$metric, [string]$name, [string]$label) {
    if ($metric -isnot [System.Management.Automation.PSCustomObject]) {
        throw "$label must be an object with min and/or max."
    }
    foreach ($property in @($metric.PSObject.Properties)) {
        if (@('min', 'max', 'measured') -notcontains $property.Name) {
            throw "$label has unknown field '$($property.Name)'; allowed fields are min, max, measured."
        }
    }
    $hasMin = $null -ne $metric.PSObject.Properties['min']
    $hasMax = $null -ne $metric.PSObject.Properties['max']
    if (-not $hasMin -and -not $hasMax) { throw "$label needs min and/or max." }
    $range = $script:metricRanges[$name]
    $lower = $null
    $upper = $null
    if ($hasMin) {
        $lower = ConvertTo-MetricNumber $metric.min "$label.min"
        if ($lower -lt $range.min -or $lower -gt $range.max) {
            throw ("$label.min {0} is outside the {1} range {2}..{3}." -f $lower, $name, $range.min, $range.max)
        }
    }
    if ($hasMax) {
        $upper = ConvertTo-MetricNumber $metric.max "$label.max"
        if ($upper -lt $range.min -or $upper -gt $range.max) {
            throw ("$label.max {0} is outside the {1} range {2}..{3}." -f $upper, $name, $range.min, $range.max)
        }
    }
    if ($null -ne $lower -and $null -ne $upper -and $lower -gt $upper) {
        throw "$label floor $lower is greater than ceiling $upper."
    }
    if ($null -ne $metric.PSObject.Properties['measured']) {
        $measured = ConvertTo-MetricNumber $metric.measured "$label.measured"
        if ($measured -lt $range.min -or $measured -gt $range.max) {
            throw ("$label.measured {0} is outside the {1} range {2}..{3}." -f $measured, $name, $range.min, $range.max)
        }
    }
}

function Assert-CaptureArgs([object[]]$arguments, [string]$label) {
    foreach ($arg in $arguments) {
        if ($null -eq $arg -or $arg -isnot [string]) {
            throw "$label arguments must all be strings."
        }
        if ($runnerFlags -contains $arg) {
            throw "$label must not set $arg; tools/image-regression.ps1 owns the capture flags."
        }
        if ($arg -match '^[A-Za-z]:' -or $arg -match '^\\\\') {
            throw "$label must use relative paths only: $arg"
        }
    }
}

function Assert-BaselineDocument([object]$doc) {
    if ($doc.schema -ne 'nativegpu.image-baselines/1') {
        throw "Unsupported baseline schema '$($doc.schema)'; expected nativegpu.image-baselines/1."
    }
    foreach ($field in @('frames', 'pixelThreshold', 'ssim', 'baselines')) {
        if ($null -eq $doc.PSObject.Properties[$field]) { throw "Baseline file has no '$field'." }
    }
    $frames = [int]$doc.frames
    if ($frames -lt 240) {
        throw "Baseline file frames must be at least 240 for --probe, got $frames."
    }
    $pixelThreshold = [int]$doc.pixelThreshold
    if ($pixelThreshold -lt 0 -or $pixelThreshold -gt 255) {
        throw "Baseline file pixelThreshold must be between 0 and 255."
    }
    if ($doc.ssim -isnot [System.Management.Automation.PSCustomObject]) {
        throw "Baseline file 'ssim' must be an object."
    }
    if ([int]$doc.ssim.blockSize -ne $script:ssimBlockSize) {
        throw "Baseline file ssim.blockSize must be $($script:ssimBlockSize)."
    }
    if ([Math]::Abs(([double]$doc.ssim.k1) - $script:ssimK1) -gt 1e-12) {
        throw "Baseline file ssim.k1 must be $($script:ssimK1)."
    }
    if ([Math]::Abs(([double]$doc.ssim.k2) - $script:ssimK2) -gt 1e-12) {
        throw "Baseline file ssim.k2 must be $($script:ssimK2)."
    }
    if ([double]$doc.ssim.dynamicRange -ne $script:ssimDynamicRange) {
        throw "Baseline file ssim.dynamicRange must be $($script:ssimDynamicRange)."
    }
    $weights = @($doc.ssim.luminanceWeights)
    if ($weights.Count -ne 3) { throw 'Baseline file ssim.luminanceWeights must have three numbers.' }
    $expectedWeights = @($script:ssimLumaR, $script:ssimLumaG, $script:ssimLumaB)
    for ($i = 0; $i -lt 3; ++$i) {
        if ([Math]::Abs(([double]$weights[$i]) - [double]$expectedWeights[$i]) -gt 1e-12) {
            throw "Baseline file ssim.luminanceWeights[$i] must be $($expectedWeights[$i])."
        }
    }

    $baselines = @($doc.baselines)
    if ($baselines.Count -eq 0) { throw 'Baseline file contains no baselines.' }
    $seen = @{}
    $index = 0
    foreach ($entry in $baselines) {
        ++$index
        $label = "baseline #$index"
        foreach ($field in @('id', 'scene', 'camera', 'resolution', 'screenshotFrame', 'metrics',
                             'sourceCommit', 'acceptedAtUtc', 'sha256')) {
            if ($null -eq $entry.PSObject.Properties[$field]) {
                throw "$label is missing field '$field'."
            }
        }
        if (-not $entry.id) { throw "$label has an empty id." }
        $label = "baseline '$($entry.id)'"
        if ($entry.id -notmatch '^[a-z0-9][a-z0-9-]*$') {
            throw "$label id must be lowercase letters, digits and hyphens."
        }
        if ($seen.ContainsKey($entry.id)) { throw "Duplicate baseline id '$($entry.id)'." }
        $seen[$entry.id] = $true
        if (-not $entry.scene) { throw "$label has an empty scene." }
        if ($entry.scene -match '^[A-Za-z]:' -or $entry.scene -match '^\\\\' -or
            $entry.scene -match '(^|/)\.\.(/|$)') {
            throw "$label scene must be a project-relative path: $($entry.scene)"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $root $entry.scene))) {
            throw "$label scene does not exist: $($entry.scene)"
        }
        if ($entry.camera -notmatch '^-?[0-9]+(\.[0-9]+)?(,-?[0-9]+(\.[0-9]+)?){5}$') {
            throw "$label camera must be x,y,z,tx,ty,tz, not '$($entry.camera)'."
        }
        if ($entry.resolution -notmatch '^[0-9]+x[0-9]+$') {
            throw "$label resolution must look like 1280x720, not '$($entry.resolution)'."
        }
        $size = $entry.resolution -split 'x'
        $width = [int]$size[0]
        $height = [int]$size[1]
        if ($width -lt $script:ssimBlockSize -or $height -lt $script:ssimBlockSize) {
            throw "$label resolution must be at least $($script:ssimBlockSize)x$($script:ssimBlockSize)."
        }
        if ($width % $script:ssimBlockSize -ne 0 -or $height % $script:ssimBlockSize -ne 0) {
            throw ("$label resolution {0} must be a multiple of {1} so every pixel belongs to a full SSIM block." -f
                $entry.resolution, $script:ssimBlockSize)
        }
        $screenshotFrame = [int]$entry.screenshotFrame
        if ($screenshotFrame -lt 1) { throw "$label screenshotFrame must be positive." }
        if ($screenshotFrame + 2 -gt $frames) {
            throw "$label screenshotFrame $screenshotFrame needs at least $($screenshotFrame + 2) frames; the file declares $frames."
        }
        if ($entry.metrics -isnot [System.Management.Automation.PSCustomObject]) {
            throw "$label needs a metrics object."
        }
        foreach ($metricName in $script:metricOrder) {
            if ($null -eq $entry.metrics.PSObject.Properties[$metricName]) {
                throw "$label metrics is missing '$metricName'."
            }
            Assert-MetricDefinition $entry.metrics.$metricName $metricName "$label metric '$metricName'"
        }
        foreach ($property in @($entry.metrics.PSObject.Properties)) {
            if ($script:metricOrder -notcontains $property.Name) {
                throw "$label metrics has unknown metric '$($property.Name)'."
            }
        }
        if ($entry.sourceCommit -isnot [string]) { throw "$label sourceCommit must be a string." }
        if ($entry.acceptedAtUtc -isnot [string]) { throw "$label acceptedAtUtc must be a string." }
        if ($entry.sha256 -isnot [string]) { throw "$label sha256 must be a string." }
        if ($entry.sha256 -and $entry.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw "$label sha256 must be 64 lowercase hex characters or empty."
        }
        if ($null -ne $entry.PSObject.Properties['captureArgs']) {
            Assert-CaptureArgs -arguments @($entry.captureArgs) -label "$label captureArgs"
        }
    }
}

# Reads a PNG into a 32bpp ARGB byte buffer. Same LockBits approach as
# tools/error-contract.ps1 and tools/rt-shadows.ps1; the bitmap is disposed immediately.
function Get-ImageData([string]$path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($path)
    $locked = $null
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $rect = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
        $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                                   [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = $locked.Stride
            $bytes = New-Object byte[] ($stride * $height)
            [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $bytes, 0, $bytes.Length)
        } finally {
            if ($null -ne $locked) { $bitmap.UnlockBits($locked) }
        }
    } finally {
        $bitmap.Dispose()
    }
    return [pscustomobject]@{ bytes = $bytes; stride = $stride; width = $width; height = $height }
}

# One full-resolution pass over both images. Metrics (documented in docs/IMAGE-REGRESSION.md):
#
#   meanAbsError    mean over all pixels and R/G/B of abs(a-b), 0..255
#   changedPercent  percent of pixels whose largest channel delta is > pixelThreshold
#   maxChannelDelta largest single-channel abs delta anywhere in the frame, 0..255
#   ssim            mean of per-8x8-block SSIM on Rec.709 luminance:
#                   Y = 0.2126 R + 0.7152 G + 0.0722 B
#                   SSIM(x,y) = ((2 muX muY + C1)(2 covXY + C2)) /
#                               ((muX^2 + muY^2 + C1)(varX + varY + C2))
#                   C1 = (0.01*255)^2, C2 = (0.03*255)^2, moments are population
#                   moments over the 64 pixels (divide by 64), no Gaussian window.
#
# The same pass writes the diff PNG: every output channel is min(255, abs(delta) * 4),
# i.e. a per-pixel absolute delta amplified 4x. The PNGs are 1280x720 / multiples of 8,
# so every pixel is inside a full block; Assert-BaselineDocument enforces that.
function Measure-AndWriteDiff {
    param(
        [Parameter(Mandatory)][object]$left,
        [Parameter(Mandatory)][object]$right,
        [Parameter(Mandatory)][string]$diffPath,
        [Parameter(Mandatory)][int]$pixelThreshold
    )
    if ($left.width -ne $right.width -or $left.height -ne $right.height) {
        throw ("image sizes differ: {0}x{1} vs {2}x{3}" -f $left.width, $left.height, $right.width, $right.height)
    }
    $width = $left.width
    $height = $left.height
    if ($width % $script:ssimBlockSize -ne 0 -or $height % $script:ssimBlockSize -ne 0) {
        throw ("resolution {0}x{1} is not a multiple of {2}" -f $width, $height, $script:ssimBlockSize)
    }
    $stride = $left.stride
    if ($right.stride -ne $stride) { throw 'images have different strides.' }
    $lb = $left.bytes
    $rb = $right.bytes
    $blocksX = [int]($width / $script:ssimBlockSize)
    $blocksY = [int]($height / $script:ssimBlockSize)
    $blockPixels = $script:ssimBlockSize * $script:ssimBlockSize
    $invBlockPixels = 1.0 / $blockPixels
    $c1 = $script:ssimC1
    $c2 = $script:ssimC2
    $wR = $script:ssimLumaR
    $wG = $script:ssimLumaG
    $wB = $script:ssimLumaB
    $amplify = $script:diffAmplify

    $bitmap = New-Object System.Drawing.Bitmap($width, $height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $locked = $null
    try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
        $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
                                   [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $diffStride = $locked.Stride
        $diffBytes = New-Object byte[] ($diffStride * $height)
        $sum = 0.0
        $changed = 0
        $maxDelta = 0
        $ssimSum = 0.0
        $blockCount = 0
        $total = $width * $height

        for ($by = 0; $by -lt $blocksY; ++$by) {
            $blockTop = $by * $script:ssimBlockSize
            for ($bx = 0; $bx -lt $blocksX; ++$bx) {
                $blockLeft = $bx * $script:ssimBlockSize
                $sumX = 0.0
                $sumY = 0.0
                $sumXX = 0.0
                $sumYY = 0.0
                $sumXY = 0.0
                for ($dy = 0; $dy -lt $script:ssimBlockSize; ++$dy) {
                    $row = ($blockTop + $dy) * $stride
                    $diffRow = ($blockTop + $dy) * $diffStride
                    for ($dx = 0; $dx -lt $script:ssimBlockSize; ++$dx) {
                        $index = $row + ($blockLeft + $dx) * 4
                        $dr = [Math]::Abs([int]$lb[$index] - [int]$rb[$index])
                        $dg = [Math]::Abs([int]$lb[$index + 1] - [int]$rb[$index + 1])
                        $db = [Math]::Abs([int]$lb[$index + 2] - [int]$rb[$index + 2])
                        $sum += ($dr + $dg + $db)
                        $pixelMax = $dr
                        if ($dg -gt $pixelMax) { $pixelMax = $dg }
                        if ($db -gt $pixelMax) { $pixelMax = $db }
                        if ($pixelMax -gt $pixelThreshold) { ++$changed }
                        if ($pixelMax -gt $maxDelta) { $maxDelta = $pixelMax }

                        # PNG is BGRA in memory on Windows; component-wise deltas do not
                        # care about the order, alpha is forced opaque.
                        $outIndex = $diffRow + ($blockLeft + $dx) * 4
                        $diffBytes[$outIndex] = [byte][Math]::Min(255, $db * $amplify)
                        $diffBytes[$outIndex + 1] = [byte][Math]::Min(255, $dg * $amplify)
                        $diffBytes[$outIndex + 2] = [byte][Math]::Min(255, $dr * $amplify)
                        $diffBytes[$outIndex + 3] = 255

                        $lx = $wR * [int]$lb[$index + 2] + $wG * [int]$lb[$index + 1] + $wB * [int]$lb[$index]
                        $ly = $wR * [int]$rb[$index + 2] + $wG * [int]$rb[$index + 1] + $wB * [int]$rb[$index]
                        $sumX += $lx
                        $sumY += $ly
                        $sumXX += $lx * $lx
                        $sumYY += $ly * $ly
                        $sumXY += $lx * $ly
                    }
                }
                $meanX = $sumX * $invBlockPixels
                $meanY = $sumY * $invBlockPixels
                $varX = $sumXX * $invBlockPixels - $meanX * $meanX
                $varY = $sumYY * $invBlockPixels - $meanY * $meanY
                $covXY = $sumXY * $invBlockPixels - $meanX * $meanY
                if ($varX -lt 0.0) { $varX = 0.0 }
                if ($varY -lt 0.0) { $varY = 0.0 }
                $numerator = (2.0 * $meanX * $meanY + $c1) * (2.0 * $covXY + $c2)
                $denominator = ($meanX * $meanX + $meanY * $meanY + $c1) * ($varX + $varY + $c2)
                $ssimSum += $numerator / $denominator
                ++$blockCount
            }
        }

        [System.Runtime.InteropServices.Marshal]::Copy($diffBytes, 0, $locked.Scan0, $diffBytes.Length)
        $bitmap.UnlockBits($locked)
        $locked = $null
        $bitmap.Save($diffPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($null -ne $locked) { $bitmap.UnlockBits($locked) }
        $bitmap.Dispose()
    }

    return [pscustomobject]@{
        meanAbsError = [Math]::Round($sum / (3.0 * $total), 4)
        changedPercent = [Math]::Round(100.0 * $changed / $total, 2)
        maxChannelDelta = [int]$maxDelta
        ssim = [Math]::Round($ssimSum / $blockCount, 8)
        changedPixels = [int]$changed
        totalPixels = [int]$total
        ssimBlocks = [int]$blockCount
        ssimBlockSize = [int]$script:ssimBlockSize
        diffAmplify = [int]$amplify
    }
}

function Save-JsonNoBom([string]$path, [object]$value) {
    $json = $value | ConvertTo-Json -Depth 16
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $json + [Environment]::NewLine, $encoding)
}

# --- load and validate configuration ------------------------------------------------
$baselinesPath = $Baselines
if (-not [System.IO.Path]::IsPathRooted($baselinesPath)) {
    $baselinesPath = Join-Path $root $baselinesPath
}
$baselinesPath = [System.IO.Path]::GetFullPath($baselinesPath)
if (-not (Test-Path -LiteralPath $baselinesPath)) {
    throw "Baseline file not found: $baselinesPath"
}
$document = Get-Content -LiteralPath $baselinesPath -Raw | ConvertFrom-Json
Assert-BaselineDocument $document
$allBaselines = @($document.baselines)
$frames = [int]$document.frames
$pixelThreshold = [int]$document.pixelThreshold

if ($Baseline) {
    $selected = @($allBaselines | Where-Object { $_.id -eq $Baseline })
    if ($selected.Count -eq 0) { throw "No baseline with id '$Baseline' in $($Baselines)." }
    if ($selected.Count -gt 1) { throw "Baseline id '$Baseline' is not unique in $($Baselines)." }
} else {
    if ($allBaselines.Count -gt 1) {
        throw "Baseline file has $($allBaselines.Count) entries; select one with -Baseline <id>."
    }
    $selected = $allBaselines
}
if ($selected.Count -ne 1) { throw 'Select exactly one baseline.' }
$entry = $selected[0]
if ($Accept -and $allBaselines.Count -gt 1 -and -not $Baseline) {
    throw '-Accept needs -Baseline <id> when the file contains more than one baseline.'
}

$baselineRelative = "reports/image-baselines/$($entry.id).png"
$baselinePath = Join-Path $root $baselineRelative

# --- build ---------------------------------------------------------------------------
if (-not $SkipBuild -or -not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) {
    throw 'build\NativeGPU.exe is missing after the build.'
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path

$context = New-ReportContext -Mode 'image-regression'
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$baselineDir = Split-Path $baselinePath -Parent
New-Item -ItemType Directory -Force -Path $baselineDir | Out-Null

function Invoke-Capture {
    param([Parameter(Mandatory)][object]$Definition, [Parameter(Mandatory)][string]$Tag)
    $reportPath = Join-Path $stage "$Tag.json"
    $imagePath = Join-Path $stage "$Tag.png"
    # Start-Process joins ArgumentList without quoting, so every path handed to the
    # executable is project-relative: the project directory contains spaces.
    $extra = @()
    if ($null -ne $Definition.PSObject.Properties['captureArgs']) {
        $extra = @($Definition.captureArgs)
    }
    $arguments = @(
        '--probe',
        '--scene', $Definition.scene,
        '--frames', "$frames",
        '--render-size', $Definition.resolution,
        '--camera', $Definition.camera,
        '--screenshot-frame', "$($Definition.screenshotFrame)"
    ) + $extra + @(
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
    if (-not $data.PSObject.Properties['rtShadows'] -or $null -eq $data.rtShadows) {
        throw "capture '$Tag' report has no rtShadows block."
    }
    $expectedResolution = $Definition.resolution -split 'x'
    $actualResolution = @($data.resolution)
    if ($actualResolution.Count -ne 2 -or
        [int]$actualResolution[0] -ne [int]$expectedResolution[0] -or
        [int]$actualResolution[1] -ne [int]$expectedResolution[1]) {
        throw "capture '$Tag' rendered '$($data.resolution -join 'x')', expected '$($Definition.resolution)'."
    }
    if ([int]$data.rtShadows.screenshotFrame -ne [int]$Definition.screenshotFrame) {
        throw "capture '$Tag' used screenshot frame $($data.rtShadows.screenshotFrame), expected $($Definition.screenshotFrame)."
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

Write-Host ("Image regression ({0}, baseline '{1}', scene {2}, {3}, frame {4})" -f `
    (Get-RelativePath $baselinesPath), $entry.id, $entry.scene, $entry.resolution,
    $entry.screenshotFrame) -ForegroundColor Cyan

$capture = Invoke-Capture -Definition $entry -Tag "$($entry.id)-capture"
$captureSha = (Get-FileHash -LiteralPath $capture.image -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host ("  capture {0} (sha256 {1})" -f (Get-RelativePath $capture.image), $captureSha.Substring(0, 12))

$captureImage = Get-ImageData $capture.image
$baselineExists = Test-Path -LiteralPath $baselinePath
$previousSha = ''
$baselineSha = ''
$metrics = $null
$metricsBasis = ''
$previousProblem = ''

if ($baselineExists) {
    $previousSha = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $baselineSha = $previousSha
}

if ($Accept) {
    if ($baselineExists) {
        $previousImage = Get-ImageData $baselinePath
        if ($previousImage.width -ne $captureImage.width -or $previousImage.height -ne $captureImage.height) {
            $previousProblem = ("previous baseline is {0}x{1}; current capture is {2}x{3}" -f `
                $previousImage.width, $previousImage.height, $captureImage.width, $captureImage.height)
            $metrics = Measure-AndWriteDiff -left $captureImage -right $captureImage `
                -diffPath (Join-Path $stage "$($entry.id)-diff.png") -pixelThreshold $pixelThreshold
            $metricsBasis = 'self-bootstrap'
        } else {
            $metrics = Measure-AndWriteDiff -left $previousImage -right $captureImage `
                -diffPath (Join-Path $stage "$($entry.id)-diff.png") -pixelThreshold $pixelThreshold
            $metricsBasis = 'previous-baseline'
        }
    } else {
        $metrics = Measure-AndWriteDiff -left $captureImage -right $captureImage `
            -diffPath (Join-Path $stage "$($entry.id)-diff.png") -pixelThreshold $pixelThreshold
        $metricsBasis = 'self-bootstrap'
    }
} else {
    if (-not $baselineExists) {
        throw ("baseline PNG is missing: {0}. Run tools/image-regression.ps1 -Accept to create it." -f `
            (Get-RelativePath $baselinePath))
    }
    $declaredSha = "$($entry.sha256)"
    if (-not $declaredSha) {
        throw ("baseline '$($entry.id)' has no sha256 in {0}; run -Accept to record it." -f `
            (Get-RelativePath $baselinesPath))
    }
    if ($previousSha -ne $declaredSha) {
        throw ("baseline sha256 mismatch: {0} is {1}, {2} declares {3}. Review the image, then run -Accept to re-baseline." -f `
            (Get-RelativePath $baselinePath), $previousSha, (Get-RelativePath $baselinesPath), $declaredSha)
    }
    $previousImage = Get-ImageData $baselinePath
    if ($previousImage.width -ne $captureImage.width -or $previousImage.height -ne $captureImage.height) {
        throw ("baseline is {0}x{1} but the capture is {2}x{3}; re-baseline or fix resolution." -f `
            $previousImage.width, $previousImage.height, $captureImage.width, $captureImage.height)
    }
    $metrics = Measure-AndWriteDiff -left $previousImage -right $captureImage `
        -diffPath (Join-Path $stage "$($entry.id)-diff.png") -pixelThreshold $pixelThreshold
    $metricsBasis = 'accepted-baseline'
}

$diffPath = Join-Path $stage "$($entry.id)-diff.png"
if (-not (Test-Path -LiteralPath $diffPath)) { throw "diff image was not written: $diffPath" }

$capturedArtifact = Publish-Artifact -Context $context -SourcePath $capture.image `
    -Name "$($entry.id)-captured.png"
$diffArtifact = Publish-Artifact -Context $context -SourcePath $diffPath `
    -Name "$($entry.id)-diff.png"

$checks = New-Object System.Collections.Generic.List[object]
$violations = New-Object System.Collections.Generic.List[object]
$acceptedAtUtc = ''

if ($Accept) {
    Copy-Item -LiteralPath $capture.image -Destination $baselinePath -Force
    $baselineSha = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($baselineSha -ne $captureSha) { throw 'baseline copy is not byte-identical to the capture.' }
    $acceptedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    $entry.sourceCommit = $context.commit
    $entry.acceptedAtUtc = $acceptedAtUtc
    $entry.sha256 = $baselineSha
    Save-JsonNoBom -path $baselinesPath -value $document
    $readBack = Get-Content -LiteralPath $baselinesPath -Raw | ConvertFrom-Json
    $readBackEntry = @($readBack.baselines | Where-Object { $_.id -eq $entry.id })[0]
    if ($readBackEntry.sha256 -ne $baselineSha) {
        throw 'baseline metadata write did not persist the sha256.'
    }
    Write-Host ("  accept  {0}" -f (Get-RelativePath $baselinePath)) -ForegroundColor Green
    Write-Host ("  commit  {0} at {1}" -f $context.commit, $acceptedAtUtc)
    Write-Host ("  sha256  {0}" -f $baselineSha)
    Write-Host ("  drift   basis {0} (recorded, not gated)" -f $metricsBasis)
    foreach ($metricName in $script:metricOrder) {
        Write-Host ("    {0,-18} {1}" -f $metricName, (Format-MetricValue $metricName ([double]$metrics.$metricName)))
    }
} else {
    foreach ($metricName in $script:metricOrder) {
        $measured = [double]$metrics.$metricName
        $bounds = $entry.metrics.$metricName
        $verdict = Test-MetricBounds $metricName $measured $bounds
        $boundsText = Format-Bounds $metricName $bounds
        $checks.Add([pscustomobject]@{
            metric = $metricName
            measured = $measured
            bounds = $bounds
            pass = [bool]$verdict.ok
            detail = $verdict.detail
        })
        if ($verdict.ok) {
            Write-Host ("  ok   {0,-18} {1} (bounds {2})" -f `
                $metricName, (Format-MetricValue $metricName $measured), $boundsText) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL {0,-18} {1} (bounds {2}) - {3}" -f `
                $metricName, (Format-MetricValue $metricName $measured), $boundsText,
                $verdict.detail) -ForegroundColor Red
            $violations.Add([pscustomobject]@{
                baseline = $entry.id
                metric = $metricName
                measured = $measured
                bounds = $bounds
                detail = $verdict.detail
            })
        }
    }
}

$success = ($violations.Count -eq 0)
$failure = ''
if (-not $success) {
    $failure = (($violations | ForEach-Object {
        "{0}={1} outside {2}" -f $_.metric, $_.measured, (Format-Bounds $_.metric $_.bounds)
    }) -join '; ')
}

$baselineRelPath = Get-RelativePath $baselinePath
$capturedRelPath = Get-RelativePath $capturedArtifact
$diffRelPath = Get-RelativePath $diffArtifact
$captureReportRelPath = Get-RelativePath $capture.report
$baselinesRelPath = Get-RelativePath $baselinesPath
$acceptance = $null
if ($Accept) {
    $acceptance = [pscustomobject]@{
        acceptedAtUtc = $acceptedAtUtc
        sourceCommit = $context.commit
        previousBaselineSha256 = $previousSha
        baselineSha256 = $baselineSha
        previousBaselineProblem = $previousProblem
        note = 'Accept does not evaluate thresholds; thresholds and measured values are human-owned.'
    }
}

$adapter = [pscustomobject]@{
    name = $context.adapter.name
    driverVersion = $context.adapter.driverVersion
    driverDate = $context.adapter.driverDate
    reportedByCapture = [string]$capture.data.capabilities.adapter
}
$captureResolutionText = (@($capture.data.resolution) -join 'x')
$captureFrames = [int]$capture.data.frames
$captureScreenshotFrame = [int]$capture.data.rtShadows.screenshotFrame
$captureScreenshotTaken = [bool]$capture.data.rtShadows.screenshotTaken

$baselineInfo = [pscustomobject]@{
    id = $entry.id
    file = $baselineRelPath
    path = $baselineRelPath
    sha256 = $baselineSha
    sha256Declared = [string]$entry.sha256
    previousSha256 = $previousSha
    sourceCommit = [string]$entry.sourceCommit
    acceptedAtUtc = [string]$entry.acceptedAtUtc
    scene = [string]$entry.scene
    camera = [string]$entry.camera
    resolution = [string]$entry.resolution
    screenshotFrame = [int]$entry.screenshotFrame
    metrics = $entry.metrics
}
$captureInfo = [pscustomobject]@{
    file = $capturedRelPath
    path = $capturedRelPath
    sha256 = $captureSha
    stageImage = (Get-RelativePath $capture.image)
    report = $captureReportRelPath
    reportSha256 = (Get-FileHash -LiteralPath $capture.report -Algorithm SHA256).Hash.ToLowerInvariant()
    resolution = $captureResolutionText
    frames = $captureFrames
    adapter = [string]$capture.data.capabilities.adapter
    screenshotFrame = $captureScreenshotFrame
    screenshotTaken = $captureScreenshotTaken
    arguments = @($capture.arguments)
}
$diffInfo = [pscustomobject]@{
    path = $diffRelPath
    amplify = [int]$script:diffAmplify
    basis = $metricsBasis
    formula = 'per-pixel channel abs delta, min(255, delta * 4), alpha opaque'
}
$artifactInfo = [pscustomobject]@{
    captured = $capturedRelPath
    diff = $diffRelPath
    baseline = $baselineRelPath
    captureReport = $captureReportRelPath
}

$mode = 'compare'
if ($Accept) { $mode = 'accept' }
$gateEnabled = -not $Accept
$acceptedFlag = [bool]$Accept

$baselinesFileHash = (Get-FileHash -LiteralPath $baselinesPath -Algorithm SHA256).Hash.ToLowerInvariant()
$sceneHash = (Get-SceneHash $entry.scene)
# Windows PowerShell 5.1 throws "Argument types do not match" for @($genericList);
# List<T>.ToArray() is the portable way to materialize these evidence arrays.
$checksArray = $checks.ToArray()
$violationsArray = $violations.ToArray()

# Built incrementally rather than as one large nested literal so a bad value fails on its
# own line; Windows PowerShell 5.1 otherwise surfaces some construction failures as a
# non-descriptive "Argument types do not match" at an unrelated property. An ordered
# dictionary serializes to the same JSON object and keeps the evidence field order stable.
$summary = [ordered]@{}
$summary['schema'] = 'nativegpu.image-regression/1'
$summary['version'] = $context.version
$summary['commit'] = $context.commit
$summary['worktreeDirty'] = $context.dirty
$summary['mode'] = $mode
$summary['baselinesFile'] = $baselinesRelPath
$summary['baselinesFileSha256'] = $baselinesFileHash
$summary['baseline'] = $baselineInfo
$summary['scene'] = [string]$entry.scene
$summary['sceneSha256'] = $sceneHash
$summary['frames'] = $frames
$summary['pixelThreshold'] = $pixelThreshold
$summary['measurement'] = $document.measurement
$summary['ssim'] = $document.ssim
$summary['adapter'] = $adapter
$summary['capture'] = $captureInfo
$summary['metrics'] = $metrics
$summary['metricsBasis'] = $metricsBasis
$summary['thresholds'] = $entry.metrics
$summary['checks'] = $checksArray
$summary['violations'] = $violationsArray
$summary['gateEvaluated'] = $gateEnabled
$summary['accepted'] = $acceptedFlag
$summary['acceptance'] = $acceptance
$summary['diff'] = $diffInfo
$summary['artifacts'] = $artifactInfo
$summary['success'] = $success
$summary['failure'] = $failure

$evidencePath = Join-Path $context.directory 'image-regression-evidence.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('image-regression-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success -Error $failure

Write-Host ""
if ($Accept) {
    Write-Host ("Image regression: baseline '{0}' accepted" -f $entry.id) -ForegroundColor Green
} elseif ($success) {
    Write-Host ("Image regression: {0} checks passed" -f $checks.Count) -ForegroundColor Green
} else {
    Write-Host ("Image regression: {0} check(s) FAILED" -f $violations.Count) -ForegroundColor Red
}
Write-Host "Captured: $capturedRelPath"
Write-Host "Diff:     $diffRelPath"
Write-Host "Baseline: $baselineRelPath"
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

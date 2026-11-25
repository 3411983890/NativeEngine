# Frame budget evidence: proves the governor walks a bounded quality ladder when the
# rolling window stays over target, records why, and never fires when the window is calm.
#
# Exit code 0 means every check passed and the evidence was archived under
# reports/<version>/<commit12>/<UTC stamp>/.
param(
    [int]$Frames = 240,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
. (Join-Path $root 'tools/report-lib.ps1')

$context = New-ReportContext -Mode 'budget'
$checks = New-Object System.Collections.Generic.List[object]
$stage = Join-Path $context.directory 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Add-Check([string]$name, [bool]$ok, [string]$detail = '') {
    $checks.Add([pscustomobject]@{ name = $name; ok = $ok; detail = $detail })
    if ($ok) { Write-Host "  ok   $name" } else { Write-Host "  FAIL $name - $detail" -ForegroundColor Red }
}

if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { $SkipBuild = $false }
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$exe = (Resolve-Path -LiteralPath 'build\NativeGPU.exe').Path

function Invoke-Session {
    param([string]$Tag, [string[]]$Extra = @())
    $report = Join-Path $stage "$Tag.json"
    $arguments = @('--smoke', '--no-watch', '--no-stress',
                   '--scene', '.screen/courtyard.scene.json',
                   '--frames', "$Frames", '--render-size', '1280x720',
                   '--report', (Get-RelativePath $report)) + $Extra
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root `
        -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Session $Tag exited $($process.ExitCode)." }
    $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if (-not $data.success) { throw "Session $Tag reported success=false: $($data.error)" }
    return [pscustomobject]@{ data = $data; report = $report }
}

# A target far below the measured frame cost guarantees the ladder runs to the end.
$tight = Invoke-Session -Tag 'tight' -Extra @('--budget', 'auto', '--budget-target-ms', '0.0001',
    '--budget-window', '8', '--budget-sustain', '2', '--rt-shadows', 'on', '--mesh-shaders', 'on')
# A target far above it must never move.
$loose = Invoke-Session -Tag 'loose' -Extra @('--budget', 'auto', '--budget-target-ms', '100000',
    '--budget-window', '120', '--budget-sustain', '30')
# No flags at all: the governor stays out of the way.
$off = Invoke-Session -Tag 'off'

$tightBudget = $tight.data.budget
$looseBudget = $loose.data.budget
$offBudget = $off.data.budget
$tightSteps = @($tightBudget.steps)

Add-Check 'tight run reports success' ([bool]$tight.data.success) $tight.data.error
Add-Check 'tight run reaches the end of the ladder' ([string]$tightBudget.status -eq 'exhausted') `
    "status=$($tightBudget.status) level=$($tightBudget.level)"
Add-Check 'tight run applied exactly two steps' ($tightSteps.Count -eq 2) "count=$($tightSteps.Count)"
Add-Check 'first step drops the DXR shadow pass' ($tightSteps[0].name -eq 'rt-shadows-off') `
    "$($tightSteps[0].name)"
Add-Check 'second step drops the mesh shader path' ($tightSteps[1].name -eq 'mesh-path-off') `
    "$($tightSteps[1].name)"
Add-Check 'ladder is monotonic in time' ([int]$tightSteps[0].frame -le [int]$tightSteps[1].frame) `
    "frames=$($tightSteps[0].frame),$($tightSteps[1].frame)"
Add-Check 'shadow pass really was disabled after the downgrade' ([bool]$tight.data.rtShadows.enabled -eq $false) `
    "enabled=$($tight.data.rtShadows.enabled) requested=$($tight.data.rtShadows.requested)"
Add-Check 'every step records an over-target reason' `
    (@($tightSteps | Where-Object { [string]::IsNullOrEmpty([string]$_.reason) }).Count -eq 0) `
    'a step has no reason'
Add-Check 'reasons name the measured median' `
    (@($tightSteps | Where-Object { [string]$_.reason -notmatch 'over target' }).Count -eq 0) `
    ($tightSteps.reason -join ' | ')
Add-Check 'over-budget frame counter advanced' ([int]$tightBudget.overBudgetFrames -gt 0) `
    "$($tightBudget.overBudgetFrames)"
Add-Check 'governor recorded frame samples' ([int]$tightBudget.samples -gt 0) "$($tightBudget.samples)"

Add-Check 'loose target never downgrades' ([string]$looseBudget.status -eq 'within') `
    "status=$($looseBudget.status)"
Add-Check 'loose target records no steps' (@($looseBudget.steps).Count -eq 0) `
    "count=$(@($looseBudget.steps).Count)"
Add-Check 'loose target keeps the shadow pass enabled' ([bool]$loose.data.rtShadows.enabled) `
    "enabled=$($loose.data.rtShadows.enabled)"
Add-Check 'loose target still reports a GPU percentile' ($null -ne $looseBudget.gpuP95Ms) `
    'gpuP95Ms is null'

Add-Check 'no flags means disabled' ([bool]$offBudget.enabled -eq $false -and [string]$offBudget.status -eq 'disabled') `
    "enabled=$($offBudget.enabled) status=$($offBudget.status)"
Add-Check 'disabled governor applies no steps' (@($offBudget.steps).Count -eq 0) `
    "count=$(@($offBudget.steps).Count)"
Add-Check 'ladder names are exposed in the report' (@($tightBudget.ladder).Count -eq 2) `
    "ladder=$($tightBudget.ladder -join ',')"

$failed = @($checks | Where-Object { -not $_.ok }).Count
$success = $failed -eq 0

$summary = [pscustomobject]@{
    schema = 'nativegpu.budget/1'
    frames = $Frames
    tight = [pscustomobject]@{
        targetGpuMs = [double]$tightBudget.targetGpuMs
        status = [string]$tightBudget.status
        level = [int]$tightBudget.level
        steps = $tightSteps
        rtShadowEnabledAfter = [bool]$tight.data.rtShadows.enabled
        gpuP95Ms = $tightBudget.gpuP95Ms
        overBudgetFrames = [int]$tightBudget.overBudgetFrames
    }
    loose = [pscustomobject]@{
        targetGpuMs = [double]$looseBudget.targetGpuMs
        status = [string]$looseBudget.status
        level = [int]$looseBudget.level
        gpuP95Ms = $looseBudget.gpuP95Ms
        cpuP95Ms = $looseBudget.cpuP95Ms
        samples = [int]$looseBudget.samples
    }
    disabled = [pscustomobject]@{ status = [string]$offBudget.status; steps = @($offBudget.steps).Count }
    checks = $checks
    success = $success
}
$evidencePath = Join-Path $context.directory 'budget-evidence.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding utf8
Register-Report -Context $context -Path $evidencePath -LegacyAliases @('budget-evidence.json') | Out-Null
$runPath = Complete-Run -Context $context -Success $success

Write-Host ""
Write-Host ("Budget evidence: {0} checks, {1} failed" -f $checks.Count, $failed)
Write-Host "Evidence: $(Get-RelativePath $context.directory)"
Write-Host "Manifest: $(Get-RelativePath $runPath)"
Pop-Location
if (-not $success) { exit 1 }

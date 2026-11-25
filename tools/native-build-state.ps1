$ErrorActionPreference = 'Stop'
function Enter-NativeOutputLock {
    param([string]$Directory = 'build')
    $projectRoot = Split-Path $PSScriptRoot -Parent
    $outputPath = if ([IO.Path]::IsPathRooted($Directory)) { [IO.Path]::GetFullPath($Directory) } else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Directory)) }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $key = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($outputPath.ToLowerInvariant())))).Replace('-','') } finally { $hasher.Dispose() }
    $mutex = New-Object Threading.Mutex($false,('Local\NativeGPU-build-'+$key))
    $held = $false
    try { $held = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
    if (-not $held) { $mutex.Dispose(); throw 'Native output is in use by a build or verification run.' }
    return $mutex
}
function Exit-NativeOutputLock([object]$Mutex) {
    if ($Mutex) { $Mutex.ReleaseMutex(); $Mutex.Dispose() }
}
function Assert-NativeBuildCurrent {
    param([string]$Directory = 'build')
    $projectRoot = Split-Path $PSScriptRoot -Parent
    $buildDirectory = if ([IO.Path]::IsPathRooted($Directory)) { [IO.Path]::GetFullPath($Directory) } else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Directory)) }
    $manifestPath = Join-Path $buildDirectory 'source-inputs.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Native build provenance is missing; run tools/build.ps1.' }
    $statePath = Join-Path $buildDirectory 'last-build-state.json'
    if (Test-Path -LiteralPath $statePath) {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($state.state -ne 'verified') { throw "Native build is $($state.state); its outputs are unverified. $($state.error)" }
        if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $state.sourceManifestHash) { throw 'Native source manifest differs from the verified build.' }
        foreach ($entry in $state.executables.PSObject.Properties) {
            if (-not (Test-Path -LiteralPath $entry.Name) -or (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value) { throw "Native binary differs from the verified build: $($entry.Name)" }
        }
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $declared = @{}
    foreach ($entry in $manifest.PSObject.Properties) {
        $declared[[IO.Path]::GetFullPath($entry.Name)] = $true
        if (-not (Test-Path -LiteralPath $entry.Name) -or (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash -ne $entry.Value) {
            throw "Native source changed after compilation: $($entry.Name). Run tools/build.ps1."
        }
    }
    foreach ($directoryName in @('host','renderer','engine/native','engine/audio','third_party','shaders','tests','build/generated')) {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $projectRoot $directoryName) -Recurse -File | Where-Object { $_.Extension -in @('.cpp','.c','.h','.hpp','.hlsl','.hlsli','.json') })) {
            if (-not $declared.ContainsKey($file.FullName)) { throw "Native input is not part of the verified build: $($file.FullName). Run tools/build.ps1." }
        }
    }
}

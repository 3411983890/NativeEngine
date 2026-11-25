# Shared report plumbing for .incrementalTesting/core/verify.ps1, .incrementalTesting/core/ci.ps1 and the G1 gate.
#
# Evidence layout (never overwritten by a later run):
#   reports/<version>/<commit12>/<runStamp>/*.json   authoritative, append-only
#   reports/latest/*.json                            alias for the newest run
#   reports/<legacy-name>.json                       alias kept for older docs
$ErrorActionPreference = 'Stop'

function Get-ProjectRoot {
    return (Split-Path $PSScriptRoot -Parent)
}

function Get-ProjectVersion {
    $path = Join-Path (Get-ProjectRoot) 'VERSION'
    if (-not (Test-Path -LiteralPath $path)) { return '0.0.0' }
    return (Get-Content -LiteralPath $path -Raw).Trim()
}

function Get-GitCommit {
    try {
        $head = & git rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $head) { return "$head".Trim() }
    } catch { }
    return 'no-git'
}

function Get-GitDirty {
    try {
        $status = & git status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0) { return [bool]($status | Where-Object { $_ }) }
    } catch { }
    return $true
}

function Get-SceneHash([string]$relativePath = '.screen/courtyard.scene.json') {
    $path = Join-Path (Get-ProjectRoot) $relativePath
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-DxcIdentity {
    $result = [pscustomobject]@{
        available = $false
        version = 'unavailable'
        sha256 = ''
        path = ''
        error = 'dxcUnavailable: pinned compiler not installed'
    }
    $exe = Join-Path (Get-ProjectRoot) '.tools\dxc\1.9.2609\bin\x64\dxc.exe'
    $manifest = Join-Path (Get-ProjectRoot) '.tools\dxc\1.9.2609\dxc.json'
    if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $manifest)) {
        try {
            $info = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
            $result.available = $true
            $result.version = $info.version
            $result.sha256 = $info.sha256
            $result.path = $exe
            $result.error = ''
        } catch {
            $result.error = "dxcUnavailable: $($_.Exception.Message)"
        }
    }
    return $result
}

function Get-BuildIdentity {
    $path = Join-Path (Get-ProjectRoot) 'build\generated\nativegpu_version.h'
    $result = [pscustomobject]@{ commit = 'unknown'; dirty = $null; version = '' }
    if (-not (Test-Path -LiteralPath $path)) { return $result }
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '#define NATIVEGPU_VERSION "([^"]*)"') { $result.version = $Matches[1] }
        if ($line -match '#define NATIVEGPU_BUILD_COMMIT "([^"]*)"') { $result.commit = $Matches[1] }
        if ($line -match '#define NATIVEGPU_BUILD_DIRTY (0|1)') { $result.dirty = ($Matches[1] -eq '1') }
    }
    return $result
}

function Get-AdapterInfo {
    try {
        $controllers = Get-CimInstance Win32_VideoController -ErrorAction Stop |
            Where-Object { $_.Name -notmatch 'Basic Display|Remote Display' }
        $primary = $controllers | Select-Object -First 1
        if ($primary) {
            $driverDate = ''
            if ($primary.DriverDate) { $driverDate = ([datetime]$primary.DriverDate).ToString('yyyy-MM-dd') }
            return [pscustomobject]@{
                name = $primary.Name
                driverVersion = $primary.DriverVersion
                driverDate = $driverDate
                adapters = @($controllers | ForEach-Object { $_.Name })
            }
        }
    } catch { }
    return [pscustomobject]@{ name = 'unknown'; driverVersion = ''; driverDate = ''; adapters = @() }
}

function Get-RelativePath([string]$path) {
    $root = (Get-ProjectRoot).TrimEnd('\') + '\'
    $resolved = [System.IO.Path]::GetFullPath($path)
    if ($resolved.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $resolved.Substring($root.Length).Replace('\', '/')
    }
    return $resolved
}

function New-ReportContext {
    param([string]$Mode = 'verify')
    $root = Get-ProjectRoot
    $node = Get-Command node -ErrorAction SilentlyContinue
    $nodeVersion = 'missing'
    if ($node) { $nodeVersion = (& $node.Source --version).Trim() }
    $commit = Get-GitCommit
    $runStamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    $version = Get-ProjectVersion
    $commitPart = 'no-git'
    if ($commit -ne 'no-git') { $commitPart = $commit.Substring(0, [Math]::Min(12, $commit.Length)) }
    $directory = Join-Path $root ("reports\{0}\{1}\{2}" -f $version, $commitPart, $runStamp)
    # Two runs in the same second must not share a directory.
    if (Test-Path -LiteralPath $directory) {
        $directory = "$directory-" + [guid]::NewGuid().ToString('N').Substring(0, 6)
    }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    return [pscustomobject]@{
        mode = $Mode
        version = $version
        commit = $commit
        commitShort = $commitPart
        dirty = Get-GitDirty
        timestamp = (Get-Date).ToUniversalTime().ToString('o')
        runStamp = $runStamp
        directory = $directory
        node = $nodeVersion
        powershell = $PSVersionTable.PSVersion.ToString()
        os = [System.Environment]::OSVersion.Version.ToString()
        adapter = (Get-AdapterInfo)
        dxc = (Get-DxcIdentity)
        build = (Get-BuildIdentity)
        sceneHash = (Get-SceneHash)
        artifacts = New-Object System.Collections.Generic.List[object]
    }
}

# Registers a file that already lives in the run directory as evidence and refreshes
# the latest/ alias plus any legacy top-level names.
function Register-Report {
    param(
        [Parameter(Mandatory)][object]$Context,
        [Parameter(Mandatory)][string]$Path,
        [string[]]$LegacyAliases = @()
    )
    $root = Get-ProjectRoot
    if (-not (Test-Path -LiteralPath $Path)) { throw "Register-Report: missing report $Path" }
    $name = Split-Path $Path -Leaf
    $latestDir = Join-Path $root 'reports\latest'
    New-Item -ItemType Directory -Force -Path $latestDir | Out-Null
    $resolvedSource = (Resolve-Path -LiteralPath $Path).Path
    $resolvedLatest = [System.IO.Path]::GetFullPath((Join-Path $latestDir $name))
    if ($resolvedSource -ne $resolvedLatest) {
        Copy-Item -LiteralPath $Path -Destination (Join-Path $latestDir $name) -Force
    }
    foreach ($alias in $LegacyAliases) {
        $aliasPath = Join-Path $root (Join-Path 'reports' $alias)
        $parent = Split-Path $aliasPath -Parent
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        if ([System.IO.Path]::GetFullPath($aliasPath) -ne $resolvedSource) {
            Copy-Item -LiteralPath $Path -Destination $aliasPath -Force
        }
    }
    $Context.artifacts.Add([pscustomobject]@{
        name = $name
        path = (Get-RelativePath $Path)
        bytes = (Get-Item -LiteralPath $Path).Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    })
    return $Path
}

# Copies an external file into the run directory, then registers it.
function Publish-Report {
    param(
        [Parameter(Mandatory)][object]$Context,
        [Parameter(Mandatory)][string]$SourcePath,
        [string]$Name = '',
        [string[]]$LegacyAliases = @()
    )
    if (-not (Test-Path -LiteralPath $SourcePath)) { throw "Publish-Report: source missing: $SourcePath" }
    if (-not $Name) { $Name = Split-Path $SourcePath -Leaf }
    $destination = Join-Path $Context.directory $Name
    $resolvedSource = (Resolve-Path -LiteralPath $SourcePath).Path
    if ([System.IO.Path]::GetFullPath($destination) -ne $resolvedSource) {
        Copy-Item -LiteralPath $SourcePath -Destination $destination -Force
    }
    return (Register-Report -Context $Context -Path $destination -LegacyAliases $LegacyAliases)
}

function Publish-Artifact {
    param(
        [Parameter(Mandatory)][object]$Context,
        [Parameter(Mandatory)][string]$SourcePath,
        [string]$Name = ''
    )
    if (-not $Name) { $Name = Split-Path $SourcePath -Leaf }
    $targetDirectory = Join-Path $Context.directory 'artifacts'
    New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null
    $destination = Join-Path $targetDirectory $Name
    Copy-Item -LiteralPath $SourcePath -Destination $destination -Force
    $Context.artifacts.Add([pscustomobject]@{
        name = "artifacts/$Name"
        path = (Get-RelativePath $destination)
        bytes = (Get-Item -LiteralPath $destination).Length
        sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    })
    return $destination
}

function Complete-Run {
    param(
        [Parameter(Mandatory)][object]$Context,
        [bool]$Success,
        [string]$Error = ''
    )
    $root = Get-ProjectRoot
    $staleBuild = [bool]($Context.build.commit -ne $Context.commit -or $Context.build.dirty -eq $true)
    $manifest = [pscustomobject]@{
        schema = 'nativegpu.run/1'
        mode = $Context.mode
        version = $Context.version
        commit = $Context.commit
        commitShort = $Context.commitShort
        worktreeDirty = $Context.dirty
        staleBuild = $staleBuild
        build = $Context.build
        startedAtUtc = $Context.timestamp
        finishedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
        node = $Context.node
        powershell = $Context.powershell
        os = $Context.os
        adapter = $Context.adapter
        dxc = $Context.dxc
        sceneHash = $Context.sceneHash
        success = $Success
        error = $Error
        artifacts = $Context.artifacts
    }
    $path = Join-Path $Context.directory 'run.json'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
    $latestDir = Join-Path $root 'reports\latest'
    New-Item -ItemType Directory -Force -Path $latestDir | Out-Null
    Copy-Item -LiteralPath $path -Destination (Join-Path $latestDir 'run.json') -Force
    [pscustomobject]@{
        schema = 'nativegpu.pointer/1'
        manifestPath = (Get-RelativePath $path)
        runDirectory = (Get-RelativePath $Context.directory)
        version = $Context.version
        commit = $Context.commit
        worktreeDirty = $Context.dirty
        staleBuild = $staleBuild
        success = $Success
        finishedAtUtc = $manifest.finishedAtUtc
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $latestDir 'POINTER.json') -Encoding utf8
    return $path
}

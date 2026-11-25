# Compiles every shader in shaders/manifest.json with the pinned DXC and embeds the
# resulting DXIL blobs into build/generated/nativegpu_shaders.h.
#
# A missing or broken DXC is *not* fatal for the build: the generated header reports
# kDxcAvailable=false and the renderer must surface `dxcUnavailable` in every report.
# A broken shader source *is* fatal, because that is a programming error, not an
# environment limitation.
param(
    [switch]$Quiet,
    [switch]$RequireDxc,
    [switch]$SelfTest,
    [string[]]$Source=@(),
    [string]$OutputDirectory='build'
)
$ErrorActionPreference = 'Stop'

function Write-Info([string]$text) { if (-not $Quiet) { Write-Host $text } }

$root = Split-Path $PSScriptRoot -Parent
$manifestPath = Join-Path $root 'shaders\manifest.json'
$outputRoot = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$shaderOutDir = Join-Path $outputRoot 'shaders'
$generatedDir = Join-Path $outputRoot 'generated'
$headerPath = Join-Path $generatedDir 'nativegpu_shaders.h'
$buildManifestPath = Join-Path $shaderOutDir 'manifest.json'

New-Item -ItemType Directory -Force -Path $shaderOutDir | Out-Null
New-Item -ItemType Directory -Force -Path $generatedDir | Out-Null

function ConvertTo-CppString([string]$text) {
    $builder = New-Object System.Text.StringBuilder
    foreach ($ch in $text.ToCharArray()) {
        switch ($ch) {
            '\' { [void]$builder.Append('\\') }
            '"' { [void]$builder.Append('\"') }
            "`n" { [void]$builder.Append('\n') }
            "`r" { }
            "`t" { [void]$builder.Append('\t') }
            default {
                $code = [int]$ch
                if ($code -lt 32 -or $code -gt 126) {
                    [void]$builder.Append(('\u{0:x4}' -f $code))
                } else {
                    [void]$builder.Append($ch)
                }
            }
        }
    }
    return $builder.ToString()
}

# PowerShell wraps native stderr in error records; drop the wrapper noise but keep the
# compiler's own diagnostics, which are the evidence G1 needs for a failed compile.
function Clean-CompilerLog([string]$text) {
    $lines = $text -split "`r?`n" | ForEach-Object { $_ -replace '^dxc\.exe : ', '' } | Where-Object {
        $_ -notmatch '^\s*\+\s' -and
        $_ -notmatch 'CategoryInfo|FullyQualifiedErrorId|所在位置|^\s*At line:|^\s*$' -and
        $_ -notmatch '^\s*\+ CategoryInfo'
    }
    return (($lines -join "`n").Trim())
}

function Format-ByteArray([string]$symbol, [byte[]]$bytes) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("inline const unsigned char ${symbol}Bytes[] = {")
    $chunk = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        $chunk.Add(('0x{0:x2}' -f $bytes[$i]))
        if ($chunk.Count -eq 20) {
            $lines.Add('    ' + ($chunk -join ',') + ',')
            $chunk.Clear()
        }
    }
    if ($chunk.Count -gt 0) { $lines.Add('    ' + ($chunk -join ',') + ',') }
    $lines.Add('};')
    return ($lines -join "`n")
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$blobs = @()
if ($manifest.blobs) { $blobs = @($manifest.blobs) }
if ($Source) {
    $blobs = @($blobs | Where-Object { $Source -contains $_.source })
    if ($blobs.Count -eq 0) { throw "No shader entries for source: $Source" }
}
$shaderInputs = [ordered]@{}
function Add-ShaderInput([string]$path) {
    $absolute = [IO.Path]::GetFullPath($path)
    if ($shaderInputs.Contains($absolute)) { return }
    $shaderInputs[$absolute] = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
    foreach ($line in (Get-Content -LiteralPath $absolute)) {
        if ($line -match '^\s*#\s*include\s*"([^"]+)"') {
            Add-ShaderInput (Join-Path (Split-Path $absolute -Parent) $Matches[1])
        }
    }
}
$shaderInputs[$manifestPath] = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
foreach ($blob in $blobs) { Add-ShaderInput (Join-Path $root $blob.source) }

$dxcPath = $null
$dxcVersion = 'unavailable'
$dxcSha = 'unavailable'
$dxcError = ''
try {
    $candidate = & (Join-Path $PSScriptRoot 'bootstrap-dxc.ps1') -Quiet
    if ($LASTEXITCODE -ne 0 -or -not $candidate) { throw 'tools/bootstrap-dxc.ps1 did not return a compiler path.' }
    $dxcPath = @($candidate)[-1]
    $dxcInfo = Get-Content -LiteralPath (Join-Path (Split-Path (Split-Path (Split-Path $dxcPath -Parent) -Parent) -Parent) 'dxc.json') -Raw | ConvertFrom-Json
    $dxcVersion = $dxcInfo.version
    $dxcSha = $dxcInfo.sha256
} catch {
    $dxcPath = $null
    $dxcError = "dxcUnavailable: $($_.Exception.Message)"
}

if ($SelfTest) {
    # Negative-path evidence for G1: a deliberately broken shader must fail the compile
    # and preserve the compiler diagnostics instead of silently producing a blob.
    if (-not $dxcPath) { throw 'DXC is required for -SelfTest.' }
    $probeSource = Join-Path $env:TEMP ('nativegpu-broken-' + [guid]::NewGuid().ToString('N') + '.hlsl')
    $probeOutput = Join-Path $env:TEMP ('nativegpu-broken-' + [guid]::NewGuid().ToString('N') + '.dxil')
    try {
        Set-Content -LiteralPath $probeSource -Encoding ascii -Value @(
            'float4 BrokenPS() : SV_Target {',
            '    return this_symbol_does_not_exist;',
            '}'
        )
        # PowerShell 5.1 promotes native stderr to an error record, which would abort
        # under $ErrorActionPreference = 'Stop' before the exit code can be inspected.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $log = & $dxcPath -T ps_6_0 -E BrokenPS -Fo $probeOutput $probeSource 2>&1 | Out-String
            $exit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        $log = Clean-CompilerLog $log
        if ($exit -eq 0) { throw 'DXC accepted a broken shader; the DXC self-test is invalid.' }
        if ($log -notmatch 'error') { throw 'DXC failed without a diagnostic containing "error".' }
        $result = [pscustomobject]@{
            ok = $true
            dxcVersion = $dxcVersion
            failedAsExpected = $true
            exitCode = $exit
            diagnosticLines = @($log -split "`n" | Where-Object { $_ -match 'error' } | Select-Object -First 3)
        }
        $result | ConvertTo-Json -Depth 4 | Write-Output
        exit 0
    } finally {
        Remove-Item -LiteralPath $probeSource, $probeOutput -Force -ErrorAction SilentlyContinue
    }
}

if (-not $dxcPath -and $RequireDxc) { throw $dxcError }

$compiled = New-Object System.Collections.Generic.List[object]
$compileLog = New-Object System.Collections.Generic.List[string]
$failures = New-Object System.Collections.Generic.List[string]

foreach ($blob in $blobs) {
    if ($null -ne $blob.ieeeStrict -and $blob.ieeeStrict -isnot [bool]) { throw "ieeeStrict must be boolean: $($blob.symbol)" }
    $sourcePath = Join-Path $root $blob.source
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        $failures.Add("Shader source missing: $($blob.source)")
        continue
    }
    $outputPath = Join-Path $shaderOutDir ($blob.symbol + '.dxil')
    if (-not $dxcPath) {
        $compiled.Add([pscustomobject]@{
            symbol = $blob.symbol
            source = $blob.source
            target = $blob.target
            entry = $blob.entry
            ieeeStrict = [bool]$blob.ieeeStrict
            bytes = 0
            sha256 = ''
            status = 'dxcUnavailable'
            output = $outputPath
        })
        continue
    }
    $arguments = @('-T', $blob.target, '-HV', '2018', '-O3')
    if ($blob.ieeeStrict) { $arguments += '-Gis' }
    if ($blob.entry) { $arguments += @('-E', $blob.entry) }
    if ($blob.defines) { foreach ($define in $blob.defines) { $arguments += @('-D', $define) } }
    $arguments += @('-Fo', $outputPath, $sourcePath)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $log = & $dxcPath @arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    $log = Clean-CompilerLog $log
    if ($exit -ne 0 -or -not (Test-Path -LiteralPath $outputPath)) {
        $failures.Add("DXC failed for $($blob.source) ($($blob.target), exit $exit): $($log.Trim())")
        $compileLog.Add("FAIL $($blob.symbol): $(($log.Trim() -split "`n" | Select-Object -First 4) -join ' | ')")
        continue
    }
    $bytes = [System.IO.File]::ReadAllBytes($outputPath)
    if ($bytes.Length -lt 32) { $failures.Add("DXC produced a suspiciously small blob for $($blob.source)."); continue }
    $compiled.Add([pscustomobject]@{
        symbol = $blob.symbol
        source = $blob.source
        target = $blob.target
        entry = $blob.entry
        ieeeStrict = [bool]$blob.ieeeStrict
        bytes = $bytes.Length
        sha256 = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToLowerInvariant()
        status = 'ok'
        output = $outputPath
    })
    $compileLog.Add("ok $($blob.symbol) [$($blob.target)] $($bytes.Length) bytes")
    Write-Info "Compiled $($blob.source) -> $($blob.symbol).dxil ($($blob.target), $($bytes.Length) bytes)"
}

if ($failures.Count -gt 0) {
    throw ("Shader build failed:`n" + ($failures -join "`n"))
}
foreach ($inputPath in $shaderInputs.Keys) {
    if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $shaderInputs[$inputPath]) { throw "Shader input changed during compilation: $inputPath" }
}

$buildManifest = [pscustomobject]@{
    generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    dxcAvailable = [bool]$dxcPath
    dxcVersion = $dxcVersion
    dxcSha256 = $dxcSha
    dxcError = $dxcError
    sourceSelection = $(if($Source.Count -eq 1){$Source[0]}else{@($Source)})
    sourceInputs = $shaderInputs
    blobs = $compiled
}
$buildManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildManifestPath -Encoding utf8

# --- generated header -------------------------------------------------------
$header = New-Object System.Collections.Generic.List[string]
$header.Add('#pragma once')
$header.Add('// Generated by tools/compile-shaders.ps1. Do not edit.')
$header.Add('// DXC identity and per-blob hashes: build/shaders/manifest.json')
$header.Add('')
$header.Add('namespace nativegpu {')
$header.Add('namespace shaders {')
$header.Add('')
$header.Add('struct Blob {')
$header.Add('    const unsigned char* data;')
$header.Add('    unsigned size;')
$header.Add('    const char* name;')
$header.Add('};')
$header.Add('')
$header.Add(('inline constexpr bool kDxcAvailable = ' + $(if ($dxcPath) { 'true' } else { 'false' }) + ';'))
$header.Add(('inline constexpr const char* kDxcVersion = "' + (ConvertTo-CppString $dxcVersion) + '";'))
$header.Add(('inline constexpr const char* kDxcSha256 = "' + (ConvertTo-CppString $dxcSha) + '";'))
$header.Add(('inline constexpr const char* kDxcError = "' + (ConvertTo-CppString $dxcError) + '";'))
$header.Add(('inline constexpr const char* kBuildManifestJson = "' + (ConvertTo-CppString ($buildManifest | ConvertTo-Json -Depth 6 -Compress)) + '";'))
$header.Add('')

foreach ($item in $compiled) {
    if ($item.status -eq 'ok') {
        $bytes = [System.IO.File]::ReadAllBytes($item.output)
        $header.Add((Format-ByteArray $item.symbol $bytes))
        $header.Add(('inline const Blob {0} = {{ {0}Bytes, {1}, "{2}" }};' -f $item.symbol, $bytes.Length, (ConvertTo-CppString ($item.source + ':' + $item.target + ':' + $item.entry))))
    } else {
        $header.Add(('inline const Blob {0} = {{ nullptr, 0, "{1}" }};' -f $item.symbol, (ConvertTo-CppString ($item.source + ':' + $item.status))))
    }
    $header.Add('')
}

$header.Add('} // namespace shaders')
$header.Add('} // namespace nativegpu')
$header.Add('')
Set-Content -LiteralPath $headerPath -Encoding ascii -Value $header

Write-Info ("Shaders: {0} blob(s), DXC {1} -> {2}" -f $compiled.Count, $dxcVersion, $headerPath)
if (-not $dxcPath) { Write-Warning $dxcError }

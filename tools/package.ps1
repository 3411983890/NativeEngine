param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw 'Node.js 22 or newer is required on PATH.' }
    $packageJson = Get-Content -LiteralPath 'package.json' -Raw | ConvertFrom-Json
    $version = $packageJson.version
    $outputsRoot = [IO.Path]::GetFullPath((Join-Path $root 'outputs'))
    $destination = [IO.Path]::GetFullPath((Join-Path $outputsRoot "NativeGPU-$version-win64"))
    $zip = [IO.Path]::GetFullPath((Join-Path $outputsRoot "NativeGPU-$version-win64.zip"))
    $prefix = $outputsRoot.TrimEnd('\') + '\'
    if (-not $destination.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not $zip.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package paths escaped the outputs directory.'
    }

    if (-not $SkipBuild) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    }
    if (-not (Test-Path -LiteralPath 'build\NativeGPU.exe')) { throw 'build\NativeGPU.exe is missing.' }

    New-Item -ItemType Directory -Force -Path $outputsRoot | Out-Null
    $staging = Join-Path $outputsRoot ('.package-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $staging 'ai'),(Join-Path $staging 'assets'),(Join-Path $staging 'docs'),(Join-Path $staging 'engine\scene'),(Join-Path $staging 'tools') | Out-Null
        Copy-Item -LiteralPath 'build\NativeGPU.exe' -Destination $staging
        Copy-Item -LiteralPath 'assets\courtyard.scene.json' -Destination (Join-Path $staging 'assets')
        Copy-Item -LiteralPath 'assets\scene.schema.json' -Destination (Join-Path $staging 'assets')
        Copy-Item -LiteralPath 'README.md' -Destination $staging
        Copy-Item -LiteralPath 'docs\STATUS.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath 'ai\README.md' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'ai\example-transaction.json' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'ai\tool-service.mjs' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'ai\mcp-server.mjs' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'engine\scene\index.mjs' -Destination (Join-Path $staging 'engine\scene')
        Copy-Item -LiteralPath 'tools\scene.mjs' -Destination (Join-Path $staging 'tools')
        Copy-Item -LiteralPath 'tools\probe.ps1' -Destination (Join-Path $staging 'tools')
        @"
NativeGPU $version runtime package
- Run NativeGPU.exe for the playable courtyard sample.
- Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe.ps1
- Start the local MCP server with: node ai\mcp-server.mjs
The npm commands in README.md describe the source workspace and are not required to run this bundle.
"@ | Set-Content -LiteralPath (Join-Path $staging 'PACKAGE-NOTES.txt') -Encoding utf8

        $packages = Join-Path $staging 'packages'
        $buildOutput = & $node.Source 'tools\scene.mjs' build 'assets\courtyard.scene.json' $packages
        if ($LASTEXITCODE -ne 0) { throw 'Scene package build failed.' }
        $built = $buildOutput | ConvertFrom-Json
        if (-not $built.ok) { throw 'Scene package build returned an error.' }

        $files = Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
            $relative = $_.FullName.Substring($staging.Length + 1).Replace('\', '/')
            [ordered]@{
                path = $relative
                bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
        $manifest = [ordered]@{
            formatVersion = 1
            product = 'NativeGPU'
            version = $version
            target = 'windows-x64'
            entryPoint = 'NativeGPU.exe'
            scenePackage = $built.result.packageDirectory.Substring($staging.Length + 1).Replace('\', '/')
            files = $files
        }
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $staging 'manifest.json') -Encoding utf8

        if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
        Move-Item -LiteralPath $staging -Destination $destination
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Compress-Archive -Path (Join-Path $destination '*') -DestinationPath $zip -CompressionLevel Optimal
        Write-Host "Package: $destination"
        Write-Host "Archive: $zip"
        Write-Host ("Files: {0}, bytes: {1}" -f $files.Count, (Get-Item -LiteralPath $zip).Length)
    } finally {
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
    }
} finally {
    Pop-Location
}

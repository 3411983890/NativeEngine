param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
$packageLock = $null
try {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw 'Node.js 22 or newer is required on PATH.' }
    $packageJson = Get-Content -LiteralPath 'package.json' -Raw | ConvertFrom-Json
    $version = (Get-Content -LiteralPath 'VERSION' -Raw).Trim()
    if ($packageJson.version -ne $version) {
        throw "package.json version ($($packageJson.version)) does not match VERSION ($version). Update both together."
    }
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
    . (Join-Path $PSScriptRoot 'native-build-state.ps1')
    $packageLock = Enter-NativeOutputLock
    Assert-NativeBuildCurrent

    New-Item -ItemType Directory -Force -Path $outputsRoot | Out-Null
    $staging = Join-Path $outputsRoot ('.package-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $staging 'ai'),(Join-Path $staging 'assets'),(Join-Path $staging 'docs'),(Join-Path $staging 'engine\scene'),(Join-Path $staging 'tools') | Out-Null
        Copy-Item -LiteralPath 'build\NativeGPU.exe' -Destination $staging
        $animationDestination = Join-Path $staging 'animation\mannequin'
        New-Item -ItemType Directory -Force -Path $animationDestination | Out-Null
        Copy-Item -LiteralPath 'build\animation\mannequin\character.json','build\animation\mannequin\animation.bin' -Destination $animationDestination
        Copy-Item -LiteralPath '.developmentAgent\animation-audio\ANIMATION.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.developmentAgent\rendering\MATERIAL-PIPELINE.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.developmentAgent\rendering\LIGHTING-QUALITY.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.developmentAgent\simulation-effects\PHYSICS-DESTRUCTION.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.screen\courtyard.scene.json' -Destination (Join-Path $staging 'assets')
        Copy-Item -LiteralPath '.screen\town.scene.json' -Destination (Join-Path $staging 'assets')
        Copy-Item -LiteralPath '.screen\physics-lab.scene.json' -Destination (Join-Path $staging 'assets')
        Copy-Item -LiteralPath 'assets\scene.schema.json' -Destination (Join-Path $staging 'assets')
        # Imported scene geometry retains content-addressed relative references.
        Get-ChildItem -LiteralPath 'assets' -Filter '*.scene.json' -File | Copy-Item -Destination (Join-Path $staging 'assets')
        if(Test-Path -LiteralPath 'assets/imported') {
            foreach($importDirectory in (Get-ChildItem -LiteralPath 'assets/imported' -Directory | Where-Object {$_.Name -match '^[a-f0-9]{64}$'})) {
                $importManifest=Get-Content -LiteralPath (Join-Path $importDirectory.FullName 'import.json') -Raw -Encoding UTF8 | ConvertFrom-Json
                $importTarget=Join-Path (Join-Path $staging 'assets/imported') $importDirectory.Name
                New-Item -ItemType Directory -Force -Path $importTarget | Out-Null
                Copy-Item -LiteralPath (Join-Path $importDirectory.FullName 'import.json') -Destination $importTarget
                foreach($entry in $importManifest.files) {
                    if($entry.file -match '(^/|\\|:|(^|/)\.\.(/|$))'){throw 'Imported file escaped its package.'}
                    $sourceFile=Join-Path $importDirectory.FullName $entry.file
                    if((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256){throw "Imported asset changed: $sourceFile"}
                    $targetFile=Join-Path $importTarget $entry.file
                    New-Item -ItemType Directory -Force -Path (Split-Path $targetFile -Parent) | Out-Null
                    Copy-Item -LiteralPath $sourceFile -Destination $targetFile
                }
            }
        }
        Copy-Item -LiteralPath 'assets\scripts','assets\gameplay' -Destination (Join-Path $staging 'assets') -Recurse
        Copy-Item -LiteralPath 'assets\builtin' -Destination (Join-Path $staging 'assets') -Recurse
        New-Item -ItemType Directory -Force -Path (Join-Path $staging '.screen') | Out-Null
        Copy-Item -LiteralPath '.screen\basic.scene.json' -Destination (Join-Path $staging '.screen')
        Copy-Item -LiteralPath '.developmentAgent\simulation-effects\GAMEPLAY.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath 'third_party\LUA-LICENSE.txt' -Destination $staging
        # Encoder caches are authoring data, not runtime assets.
        $textureDestination = Join-Path $staging 'assets\textures'
        New-Item -ItemType Directory -Force -Path $textureDestination | Out-Null
        Get-ChildItem -LiteralPath 'assets\textures' -Filter '*.ngtex' -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $textureDestination
        }
        Copy-Item -LiteralPath 'assets\textures\manifest.json' -Destination $textureDestination
        # Publish only the manifest's live immutable effect objects. Dependencies
        # keep their authored asset-root paths so native SHA checks work in a bundle.
        $effectDestination = Join-Path $staging 'effects'
        New-Item -ItemType Directory -Force -Path $effectDestination | Out-Null
        $effectLibrary = Get-Content -LiteralPath 'build/effects/library.json' -Raw | ConvertFrom-Json
        Copy-Item -LiteralPath 'build/effects/library.json' -Destination $effectDestination
        foreach ($effect in $effectLibrary.effects) {
            if ($effect.file -notmatch '^[a-f0-9]{64}\.ngeffect\.json$') { throw 'Invalid cooked effect object name.' }
            Copy-Item -LiteralPath (Join-Path 'build/effects' $effect.file) -Destination $effectDestination
        }
        foreach ($dependency in $effectLibrary.dependencies.PSObject.Properties) {
            $effectSource = [IO.Path]::GetFullPath((Join-Path (Join-Path $root 'assets') $dependency.Name))
            $assetPrefix = [IO.Path]::GetFullPath((Join-Path $root 'assets')).TrimEnd('\') + '\'
            if (-not $effectSource.StartsWith($assetPrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Effect dependency escaped asset root.' }
            if ((Get-FileHash -LiteralPath $effectSource -Algorithm SHA256).Hash -ne $dependency.Value) { throw 'Cooked effect dependency changed; recook before packaging.' }
            $effectTarget = Join-Path (Join-Path $staging 'assets') $dependency.Name
            New-Item -ItemType Directory -Force -Path (Split-Path $effectTarget -Parent) | Out-Null
            Copy-Item -LiteralPath $effectSource -Destination $effectTarget -Force
        }
        $fieldDestination=Join-Path $staging 'assets/fields'
        New-Item -ItemType Directory -Force -Path $fieldDestination | Out-Null
        Get-ChildItem -LiteralPath 'assets/fields' -Filter '*.field.json' -File | Copy-Item -Destination $fieldDestination
        Copy-Item -LiteralPath '.developmentAgent\simulation-effects\SIMULATION-DEVELOPMENT.md','.developmentAgent\simulation-effects\PARTICLES-FIELDS-CLOTH-WATER-PLAN.md','.developmentAgent\simulation-effects\FIELD-REFERENCE.md','.developmentAgent\simulation-effects\FIELD-HOST.md','.developmentAgent\simulation-effects\EFFECT-FIELD-CONSUMER.md','.developmentAgent\simulation-effects\EFFECT-GPU-REFERENCE.md','.developmentAgent\simulation-effects\EFFECT-GPU-RENDERING.md' -Destination (Join-Path $staging 'docs')
        # The sound bank and the .ngsnd files are what the host loads by default; a
        # package without them runs silently but reports an audio error.
        Copy-Item -LiteralPath 'assets\audio' -Destination (Join-Path $staging 'assets') -Recurse
        Copy-Item -LiteralPath '.developmentAgent\animation-audio\AUDIO-PLAN.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.developmentAgent\project\root-readme.md' -Destination (Join-Path $staging 'README.md')
        Copy-Item -LiteralPath '.developmentAgent\core\STATUS.md' -Destination (Join-Path $staging 'docs')
        Copy-Item -LiteralPath '.developmentAgent\project\ai-readme.md' -Destination (Join-Path $staging 'ai\README.md')
        Copy-Item -LiteralPath 'ai\example-transaction.json' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'ai\tool-service.mjs' -Destination (Join-Path $staging 'ai')
        Copy-Item -LiteralPath 'ai\mcp-server.mjs' -Destination (Join-Path $staging 'ai')
        Get-ChildItem -LiteralPath 'engine\scene' -Filter '*.mjs' -File | Copy-Item -Destination (Join-Path $staging 'engine\scene')
        New-Item -ItemType Directory -Path (Join-Path $staging 'engine\assets') | Out-Null
        foreach($assetModule in @('audio-data.mjs','gltf-document.mjs','canonical.mjs','environment-container.mjs')) {
            Copy-Item -LiteralPath (Join-Path 'engine\assets' $assetModule) -Destination (Join-Path $staging 'engine\assets')
        }
        New-Item -ItemType Directory -Path (Join-Path $staging 'engine\material') | Out-Null
        Get-ChildItem -LiteralPath 'engine\material' -Filter '*.mjs' -File | Copy-Item -Destination (Join-Path $staging 'engine\material')
        Copy-Item -LiteralPath 'tools\material.mjs' -Destination (Join-Path $staging 'tools')
        Copy-Item -LiteralPath 'tools\textures.mjs' -Destination (Join-Path $staging 'tools')
        Copy-Item -LiteralPath 'tools\bc.mjs' -Destination (Join-Path $staging 'tools')
        Copy-Item -LiteralPath 'tools\scene.mjs' -Destination (Join-Path $staging 'tools')
        @"
NativeGPU $version runtime package
- Run NativeGPU.exe for the playable courtyard sample.
- Run the bundled third-person robot starter with:
  NativeGPU.exe --scene .screen\basic.scene.json --textures assets\builtin\textures
- Run the textured town with:
  NativeGPU.exe --scene assets\town.scene.json --textures assets\textures
- Run the physics/destruction demo:
  NativeGPU.exe --scene assets\physics-lab.scene.json --physics-demo
- Run the CPU reference particle / GPU sprite preview:
  NativeGPU.exe --effect sprite_demo --effect-count 4 --effect-library effects/library.json
- Start the local MCP server with: node ai\mcp-server.mjs
The npm commands in README.md describe the source workspace and are not required to run this bundle.
"@ | Set-Content -LiteralPath (Join-Path $staging 'PACKAGE-NOTES.txt') -Encoding utf8

        $packages = Join-Path $staging 'packages'
        $buildOutput = & $node.Source 'tools\scene.mjs' build '.screen\courtyard.scene.json' $packages
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
        [IO.File]::WriteAllText((Join-Path $staging 'manifest.json'),($manifest | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))

        Assert-NativeBuildCurrent
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
    if ($packageLock) { Exit-NativeOutputLock $packageLock }
    Pop-Location
}

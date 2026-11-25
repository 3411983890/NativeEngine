param([switch]$SkipBuild,[switch]$SkipNodeTests)
$ErrorActionPreference='Stop'
$assetRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $assetRoot
try {
    if(-not $SkipNodeTests) {
        & node --test tests/asset-import.test.mjs tests/character-import.test.mjs tests/gltf-compression.test.mjs tests/animation-retarget.test.mjs tests/character-instances.test.mjs tests/asset-coordinates.test.mjs tests/character-fit.test.mjs tests/audio-import.test.mjs tests/scene-import.test.mjs tests/texture-import.test.mjs tests/emissive-import.test.mjs tests/texture-mapping.test.mjs tests/texture-coordinates.test.mjs tests/vertex-color-unlit.test.mjs tests/transparency-import.test.mjs tests/occlusion-import.test.mjs tests/environment-import.test.mjs tests/tangent-import.test.mjs tests/tangent-morph.test.mjs tests/mikktspace-import.test.mjs tests/ior-import.test.mjs tests/clearcoat-import.test.mjs tests/asset-source-identity.test.mjs tests/asset-reimport.test.mjs tests/animation.test.mjs tests/textures.test.mjs tests/material.test.mjs tests/mir.test.mjs tests/graph.test.mjs tests/editor-material.test.mjs tests/editor-service.test.mjs tests/editor-server.test.mjs
        if($LASTEXITCODE -ne 0){throw 'Asset import incremental tests failed.'}
    }
    if($SkipBuild){& powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-effects-host.ps1 -OutputDirectory build/assets-host -SkipBuild}
    else {& powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-effects-host.ps1 -OutputDirectory build/assets-host}
    if($LASTEXITCODE -ne 0){throw 'Asset host dependency build failed.'}
    $assetOutput=Join-Path $assetRoot 'build/asset-import-smoke'
    New-Item -ItemType Directory -Force -Path $assetOutput | Out-Null
    $assetModule=@'
import {writeAssetFixture,writeCharacterFixture,writeCompressedAssetFixture,writeCutoutAssetFixture,writeMotionFixture} from './tools/asset-fixtures.mjs';
import {importAsset,appendAnimation} from './engine/assets/import-service.mjs';
import fs from 'node:fs/promises';
import path from 'node:path';
import {canonicalJson,sha256Hex} from './engine/assets/canonical.mjs';
const result=await importAsset({source:await writeAssetFixture(),output:'build/asset-import-smoke',size:64});
await fs.writeFile('build/asset-import-smoke/active.txt',result.directory);
const character=await importAsset({source:await writeCharacterFixture(),output:'build/asset-import-smoke',size:64});
await fs.writeFile('build/asset-import-smoke/character-active.txt',character.directory);
const compressed=await importAsset({source:await writeCompressedAssetFixture(),output:'build/asset-import-smoke',size:64});
await fs.writeFile('build/asset-import-smoke/compressed-active.txt',compressed.directory);
for(const masked of [false,true]) {
  const result=await importAsset({source:await writeCutoutAssetFixture(masked),output:'build/asset-import-smoke',size:64});
  await fs.writeFile('build/asset-import-smoke/'+(masked?'mask':'opaque')+'-active.txt',result.directory);
}
const motion=await writeMotionFixture(),base=await importAsset({source:motion.characterSource,output:'build/asset-import-smoke',size:64});
const retargeted=await appendAnimation({directory:base.directory,source:motion.source});
const bvh=await appendAnimation({directory:base.directory,source:motion.bvhSource});
await fs.writeFile('build/asset-import-smoke/motion-base-active.txt',base.directory);
await fs.writeFile('build/asset-import-smoke/retarget-active.txt',retargeted.directory);
await fs.writeFile('build/asset-import-smoke/bvh-active.txt',bvh.directory);
const omitted=path.resolve('build/asset-import-smoke/omitted-preview');await fs.mkdir(omitted,{recursive:true});await fs.cp(retargeted.directory,omitted,{recursive:true});
const manifest=structuredClone(retargeted.manifest);manifest.files=manifest.files.filter(f=>f.file!==manifest.previewCharacter);manifest.identity.files=manifest.identity.files.filter(f=>f.file!==manifest.previewCharacter);
manifest.identityJson=canonicalJson(manifest.identity);manifest.id=sha256Hex(manifest.identityJson);
const scene=structuredClone(manifest.identity.previewScene),uuid=n=>{const h=sha256Hex(`${manifest.id}:${n}`);return `${h.slice(0,8)}-${h.slice(8,12)}-4${h.slice(13,16)}-8${h.slice(17,20)}-${h.slice(20,32)}`;};
scene.projectId=uuid('project');scene.entities.forEach((entity,i)=>entity.id=uuid(i));const sceneBytes=Buffer.from(canonicalJson(scene)+'\n');await fs.writeFile(path.join(omitted,'preview.scene.json'),sceneBytes);
const entry=manifest.files.find(f=>f.file==='preview.scene.json');entry.bytes=sceneBytes.length;entry.sha256=sha256Hex(sceneBytes);await fs.writeFile(path.join(omitted,'import.json'),canonicalJson(manifest)+'\n');
'@
    $assetModule | node --input-type=module
    if($LASTEXITCODE -ne 0){throw 'Original import fixture failed.'}
    $assetDirectory=(Get-Content (Join-Path $assetOutput 'active.txt') -Raw).Trim()
    $assetReportPath=Join-Path $assetOutput 'native-incremental.json'
    $assetArguments=@('--smoke','--scene',('"'+(Join-Path $assetDirectory 'preview.scene.json')+'"'),'--textures',('"'+(Join-Path $assetDirectory 'textures')+'"'),'--frames','240','--camera','7,6,10,0,1,1','--mesh-shaders','on','--screenshot',('"'+(Join-Path $assetOutput 'preview.png')+'"'),'--report',('"'+$assetReportPath+'"'),'--audio','off')
    $assetProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList $assetArguments -WindowStyle Hidden -PassThru -Wait
    if($assetProcess.ExitCode -ne 0){throw "Native asset smoke failed: exit $($assetProcess.ExitCode)"}
    $assetReport=Get-Content -LiteralPath $assetReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if(-not $assetReport.success -or $assetReport.physics.convexMeshes -ne 2 -or $assetReport.physics.convexMeshBytes -le 0 -or -not $assetReport.capabilities.materialBindless){throw 'Native asset upload/material evidence is incomplete.'}
    $tamperedDirectory=Join-Path $assetOutput ('tampered-'+[guid]::NewGuid().ToString('N'))
    Copy-Item -LiteralPath $assetDirectory -Destination $tamperedDirectory -Recurse
    $tamperedManifest=Get-Content -LiteralPath (Join-Path $tamperedDirectory 'import.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $tamperedMesh=$tamperedManifest.files | Where-Object {$_.file -like '*.mesh.json'} | Select-Object -First 1
    Add-Content -LiteralPath (Join-Path $tamperedDirectory $tamperedMesh.file) -Value ' '
    $rejectionReport=Join-Path $assetOutput 'native-rejected.json'
    $rejection=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList @('--smoke','--asset',('"'+$tamperedDirectory+'"'),'--report',('"'+$rejectionReport+'"'),'--audio','off') -WindowStyle Hidden -PassThru -Wait
    $rejected=Get-Content -LiteralPath $rejectionReport -Raw -Encoding UTF8 | ConvertFrom-Json
    if($rejection.ExitCode -ne 10 -or $rejected.errorCode -ne 'NG-E-ASSET-LOAD'){throw 'Tampered native asset was not rejected before rendering.'}
    $omittedReport=Join-Path $assetOutput 'native-preview-rejected.json'
    $omittedProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList @('--smoke','--asset',('"'+(Join-Path $assetOutput 'omitted-preview')+'"'),'--report',('"'+$omittedReport+'"'),'--audio','off') -WindowStyle Hidden -PassThru -Wait
    $omittedRejected=Get-Content -LiteralPath $omittedReport -Raw -Encoding UTF8 | ConvertFrom-Json
    if($omittedProcess.ExitCode -ne 10 -or $omittedRejected.errorCode -ne 'NG-E-ASSET-LOAD' -or $omittedRejected.error -notlike '*preview is missing from import identity*'){throw 'Unlisted character preview was not rejected before loading.'}
    $characterDirectory=(Get-Content (Join-Path $assetOutput 'character-active.txt') -Raw).Trim()
    $characterCompiler=Join-Path $assetRoot '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
    $characterObjects=@('engine_native_animation.cpp.o','engine_native_animation_features.cpp.o','engine_native_animation_asset.cpp.o','engine_native_determinism.cpp.o') | ForEach-Object {Join-Path $assetRoot ('build/assets-host/objects/'+$_)}
    & $characterCompiler -std=c++17 -O2 -static tests/native_asset_character.cpp @characterObjects -o build/assets-host/asset_character_tests.exe
    if($LASTEXITCODE -ne 0){throw 'External character incremental test build failed.'}
    $characterEvidence=& build/assets-host/asset_character_tests.exe (Join-Path $characterDirectory 'character/character.json') (Join-Path $assetOutput 'character-negative')
    if($LASTEXITCODE -ne 0){throw 'External character native contracts failed.'}
    $characterEvidence | Set-Content -LiteralPath (Join-Path $assetOutput 'character-contracts.json') -Encoding utf8
    $characterReportPath=Join-Path $assetOutput 'native-multi-character.json'
    $characterArguments=@('--smoke','--no-stress','--asset',('"'+$characterDirectory+'"'),'--frames','240','--animation-features','off','--screenshot-frame','3','--screenshot',('"'+(Join-Path $assetOutput 'multi-character.png')+'"'),'--report',('"'+$characterReportPath+'"'),'--audio','off')
    $characterProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList $characterArguments -WindowStyle Hidden -PassThru -Wait
    $characterReport=Get-Content -LiteralPath $characterReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if($characterProcess.ExitCode -ne 0 -or -not $characterReport.success -or $characterReport.animation.meshParts -ne 2 -or $characterReport.probes.skinning.draws -lt 480 -or $characterReport.probes.skinning.morphDraws -lt 480){throw 'Multi-skin character material/morph draws failed.'}
    $pixelCheck=@'
import sharp from 'sharp';
import fs from 'node:fs/promises';
const {data}=await sharp('build/asset-import-smoke/multi-character.png').ensureAlpha().raw().toBuffer({resolveWithObject:true});
let red=0,blue=0;for(let i=0;i<data.length;i+=4){if(data[i]>25&&data[i]>data[i+1]*1.5&&data[i]>data[i+2]*1.5)red++;if(data[i+2]>25&&data[i+2]>data[i+1]*1.5&&data[i+2]>data[i]*1.5)blue++;}
if(red<100||blue<100)throw new Error('Both external character materials must produce visible pixels');
await fs.writeFile('build/asset-import-smoke/character-pixels.json',JSON.stringify({success:true,red,blue}));
'@
    $pixelCheck | node --input-type=module
    if($LASTEXITCODE -ne 0){throw 'External character material pixels failed.'}
    $compressedDirectory=(Get-Content (Join-Path $assetOutput 'compressed-active.txt') -Raw).Trim()
    $compressedReportPath=Join-Path $assetOutput 'native-compressed.json'
    $compressedProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList @('--smoke','--no-stress','--asset',('"'+$compressedDirectory+'"'),'--frames','60','--report',('"'+$compressedReportPath+'"'),'--audio','off') -WindowStyle Hidden -PassThru -Wait
    $compressedReport=Get-Content -LiteralPath $compressedReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if($compressedProcess.ExitCode -ne 0 -or -not $compressedReport.success -or $compressedReport.physics.convexMeshes -ne 2){throw 'Compressed external model native import failed.'}
    foreach($alphaMode in @('opaque','mask')) {
        $alphaDirectory=(Get-Content (Join-Path $assetOutput ($alphaMode+'-active.txt')) -Raw).Trim()
        $alphaProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList @('--smoke','--no-stress','--asset',('"'+$alphaDirectory+'"'),'--camera','0,1,4,0,1,0','--frames','4','--screenshot-frame','1','--screenshot',('"'+(Join-Path $assetOutput ($alphaMode+'.png'))+'"'),'--report',('"'+(Join-Path $assetOutput ($alphaMode+'.json'))+'"'),'--audio','off') -WindowStyle Hidden -PassThru -Wait
        if($alphaProcess.ExitCode -ne 0){throw "Native alpha $alphaMode fixture failed."}
    }
    $alphaPixels=@'
import sharp from 'sharp';
import fs from 'node:fs/promises';
const counts={};for(const name of ['opaque','mask']) {
  const {data}=await sharp('build/asset-import-smoke/'+name+'.png').ensureAlpha().raw().toBuffer({resolveWithObject:true});
  counts[name]=0;for(let i=0;i<data.length;i+=4)if(data[i]>20&&data[i]>data[i+1]*1.5&&data[i]>data[i+2]*1.5)counts[name]++;
}
const ratio=counts.mask/counts.opaque;if(counts.opaque<100||ratio<.35||ratio>.65)throw new Error('Masked panel must retain half the opaque coverage: '+JSON.stringify(counts));
await fs.writeFile('build/asset-import-smoke/alpha-pixels.json',JSON.stringify({success:true,...counts,ratio}));
'@
    $alphaPixels | node --input-type=module
    if($LASTEXITCODE -ne 0){throw 'Native alpha material pixel coverage failed.'}
    $retargetDirectory=(Get-Content (Join-Path $assetOutput 'retarget-active.txt') -Raw).Trim()
    $retargetEvidence=& build/assets-host/asset_character_tests.exe (Join-Path $retargetDirectory 'character/character.json') (Join-Path $assetOutput 'retarget-negative') retarget
    if($LASTEXITCODE -ne 0){throw 'Retargeted motion native contracts failed.'}
    $retargetEvidence | Set-Content -LiteralPath (Join-Path $assetOutput 'retarget-contracts.json') -Encoding utf8
    $bvhDirectory=(Get-Content (Join-Path $assetOutput 'bvh-active.txt') -Raw).Trim()
    $bvhEvidence=& build/assets-host/asset_character_tests.exe (Join-Path $bvhDirectory 'character/character.json') (Join-Path $assetOutput 'bvh-negative') bvh
    if($LASTEXITCODE -ne 0){throw 'BVH root-motion native contracts failed.'}
    $bvhEvidence | Set-Content -LiteralPath (Join-Path $assetOutput 'bvh-contracts.json') -Encoding utf8
    foreach($motionMode in @('motion-base','retarget','bvh')) {
        $motionDirectory=(Get-Content (Join-Path $assetOutput ($motionMode+'-active.txt')) -Raw).Trim()
        $motionReportPath=Join-Path $assetOutput ($motionMode+'.json')
        $motionProcess=Start-Process -FilePath 'build/assets-host/NativeGPU.exe' -ArgumentList @('--smoke','--no-stress','--asset',('"'+$motionDirectory+'"'),'--frames','120','--animation-features','off','--screenshot-frame','30','--screenshot',('"'+(Join-Path $assetOutput ($motionMode+'.png'))+'"'),'--report',('"'+$motionReportPath+'"'),'--audio','off') -WindowStyle Hidden -PassThru -Wait
        $motionReport=Get-Content -LiteralPath $motionReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if($motionProcess.ExitCode -ne 0 -or -not $motionReport.success -or $motionReport.probes.skinning.draws -lt 240){throw 'Retargeted character draw evidence failed.'}
        if($motionMode -eq 'retarget' -and ($motionReport.animation.clipName -ne 'vendor_Raise' -or $motionReport.animation.asset -notlike '*preview.json')){throw 'Native preview did not execute the appended clip.'}
        if($motionMode -eq 'bvh' -and $motionReport.animation.clipName -ne 'walk_Motion'){throw 'Native preview did not execute BVH motion.'}
    }
    $retargetPixels=@'
import sharp from 'sharp';
import fs from 'node:fs/promises';
const images=await Promise.all(['motion-base','retarget'].map(name=>sharp('build/asset-import-smoke/'+name+'.png').ensureAlpha().raw().toBuffer()));
if(images[0].length!==images[1].length)throw new Error('Motion preview image dimensions differ');
let changed=0;for(let i=0;i<images[0].length;i+=4)if([0,1,2].some(c=>Math.abs(images[0][i+c]-images[1][i+c])>12))changed++;
if(changed<1000)throw new Error('Appended motion must change visible character pixels');
await fs.writeFile('build/asset-import-smoke/retarget-pixels.json',JSON.stringify({success:true,changed}));
'@
    $retargetPixels | node --input-type=module
    if($LASTEXITCODE -ne 0){throw 'Appended motion visible-pose comparison failed.'}
    & node .incrementalTesting/assets/character-instances-check.mjs
    if($LASTEXITCODE -ne 0){throw 'Scene character instance incremental checks failed.'}
    & node .incrementalTesting/assets/character-controller-check.mjs
    if($LASTEXITCODE -ne 0){throw 'Character controller fitting incremental checks failed.'}
    & node .incrementalTesting/assets/asset-audio-check.mjs
    if($LASTEXITCODE -ne 0){throw "Asset audio incremental checks failed."}
    & node .incrementalTesting/assets/asset-scene-check.mjs
    if($LASTEXITCODE -ne 0){throw "Scene import incremental checks failed."}
    & node .incrementalTesting/assets/asset-texture-check.mjs
    if($LASTEXITCODE -ne 0){throw "Texture format incremental checks failed."}
    & node .incrementalTesting/assets/asset-emissive-check.mjs
    if($LASTEXITCODE -ne 0){throw "Independent emission incremental checks failed."}
    & node .incrementalTesting/assets/asset-mapping-check.mjs
    if($LASTEXITCODE -ne 0){throw "Texture mapping incremental checks failed."}
    & node .incrementalTesting/assets/asset-vertex-color-check.mjs
    if($LASTEXITCODE -ne 0){throw "Vertex color and unlit incremental checks failed."}
    & node .incrementalTesting/assets/asset-transparency-check.mjs
    if($LASTEXITCODE -ne 0){throw "External alpha blend incremental checks failed."}
    & node .incrementalTesting/assets/asset-tangent-check.mjs
    if($LASTEXITCODE -ne 0){throw "Authored tangent incremental checks failed."}
    & node .incrementalTesting/assets/asset-tangent-morph-check.mjs
    if($LASTEXITCODE -ne 0){throw "Tangent morph incremental checks failed."}
    & node .incrementalTesting/assets/asset-mikktspace-check.mjs
    if($LASTEXITCODE -ne 0){throw "MikkTSpace incremental checks failed."}
    & node .incrementalTesting/assets/asset-clearcoat-check.mjs
    if($LASTEXITCODE -ne 0){throw "Clearcoat incremental checks failed."}
    & node .incrementalTesting/assets/asset-reimport-check.mjs
    if($LASTEXITCODE -ne 0){throw "Asset reimport incremental checks failed."}
    & node .incrementalTesting/assets/asset-ior-check.mjs
    if($LASTEXITCODE -ne 0){throw "IOR incremental checks failed."}
    & node .incrementalTesting/assets/asset-ior-hot-check.mjs
    if($LASTEXITCODE -ne 0){throw "IOR material hot-reload checks failed."}
    & node .incrementalTesting/assets/asset-environment-check.mjs
    if($LASTEXITCODE -ne 0){throw "HDR environment incremental checks failed."}
    & node .incrementalTesting/assets/asset-texture-coordinates-check.mjs
    if($LASTEXITCODE -ne 0){throw "Texture coordinate incremental checks failed."}
    & node .incrementalTesting/assets/asset-occlusion-check.mjs
    if($LASTEXITCODE -ne 0){throw "Independent AO incremental checks failed."}
    & node .incrementalTesting/assets/asset-coordinates-check.mjs
    if($LASTEXITCODE -ne 0){throw 'Asset coordinate conversion incremental checks failed.'}
    Write-Host "Asset incremental checks passed: static mesh resize/restart and fallback; multi-skin/material character, authored morph curves and deterministic controller. Report: $assetReportPath"
} finally {Pop-Location}

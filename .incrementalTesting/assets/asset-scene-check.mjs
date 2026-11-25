import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {randomUUID} from 'node:crypto';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {writeSceneFixture} from '../../tools/offline/scene-fixture.mjs';
import {importAsset,checkoutSceneImport} from '../../engine/assets/import-service.mjs';
import {buildScene} from '../../engine/scene/index.mjs';
import {prepareAssetHost} from '../../tools/asset-host-scope.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkAssetScene({context}={}){
  context??=await prepareAssetHost();await context.assertStable();
  const output=path.join(root,'build/asset-import-smoke/scenes');await fs.mkdir(output,{recursive:true});const sourceDir=path.join(output,'source-'+randomUUID()),catalog=path.join(output,'catalog-'+randomUUID()),source=await writeSceneFixture(sourceDir,{fracture:true,importDirectory:catalog});
  // The source scene must already be self-contained before whole-scene import.
  const hiddenCatalog=path.join(output,'hidden-catalog-'+randomUUID());assert.equal(path.dirname(catalog),output);assert.equal(path.dirname(hiddenCatalog),output);await fs.rename(catalog,hiddenCatalog);
  const asset=await importAsset({source,output:path.join(output,'imports')});
  const hidden=path.join(output,'hidden-source-'+randomUUID());assert.equal(path.dirname(sourceDir),output);assert.equal(path.dirname(hidden),output);await fs.rename(sourceDir,hidden);
  const opened=await checkoutSceneImport({directory:asset.directory,outputDirectory:path.join(output,'editable')}),built=await buildScene(opened.scenePath,path.join(output,'packages'));
  const host=context.directory,compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'),objects=['engine_native_game.cpp','engine_native_physics.cpp','engine_native_animation.cpp','engine_native_animation_features.cpp','engine_native_animation_asset.cpp','engine_native_determinism.cpp','host_scene_loader.cpp','engine_native_assets_sha256.cpp'].map(n=>path.join(host,'objects',n+'.o')),test=path.join(host,'asset_scene_tests.exe');
  await context.assertHeaders();await run(compiler,['-std=c++17','-O2','-static',await context.contractSource('tests/native_asset_scene.cpp'),...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});const contracts=JSON.parse((await run(test,[opened.scenePath],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768})).stdout);
  const integration=path.join(host,'asset_scene_gameplay_tests.exe');await context.assertHeaders();await run(compiler,['-std=c++17','-O2','-static',await context.contractSource('tests/native_gameplay_integration.cpp'),...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',integration],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});await run(integration,[],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const exe=path.join(host,'NativeGPU.exe'),record=path.join(output,'record.csv'),base=['--smoke','--no-stress','--frames','240','--animation-features','off','--audio','offline','--camera','7,6,10,0,1,1','--screenshot-frame','200'];const reports=[];
  for(const mode of ['imported','editable','packaged']){
    await context.assertStable();const report=path.join(output,mode+'.json'),image=path.join(output,mode+'.png');await run(exe,[...base,...(mode==='imported'?['--asset',asset.directory,'--record',record]:['--scene',mode==='editable'?opened.scenePath:path.join(built.packageDirectory,'scene.json'),'--replay',record,'--replay-strict']),'--report',report,'--screenshot',image,'--textures',path.join(mode==='imported'?asset.directory:mode==='editable'?opened.directory:built.packageDirectory,'textures')],{cwd:mode==='packaged'?built.packageDirectory:root,windowsHide:true,timeout:120000,maxBuffer:32768});
    const data=JSON.parse(await fs.readFile(report));assert.equal(data.success,true);assert.equal(data.audio.error,'');assert.equal(data.animation.instances.length,1);assert.equal(data.physics.convexMeshes,98);assert.ok(data.audio.mixer.peakMax>0);if(mode!=='imported'){assert.equal(data.replay.mismatches,0);assert.equal(data.audio.mixer.pcmHash,reports[0].audio.mixer.pcmHash);assert.equal(data.determinism.finalHash,reports[0].determinism.finalHash);}reports.push(data);
  }
  const original=await sharp(path.join(output,'imported.png')).removeAlpha().raw().toBuffer();for(const mode of ['editable','packaged'])assert.deepEqual(await sharp(path.join(output,mode+'.png')).removeAlpha().raw().toBuffer(),original);
  await context.assertStable();const evidence={success:true,host:context.fingerprint,contracts,legacyGameplayContinuation:true,sourceRemoved:true,externalCatalogRemoved:true,nestedAttachment:true,customScriptExecuted:true,bakedFracture:true,imported:true,editable:true,packaged:true,strictReplay:true,identicalPixels:true,pcmHash:reports[0].audio.mixer.pcmHash,dependencyCount:built.manifest.dependencies.length};await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkAssetScene().then(v=>console.log(JSON.stringify(v))).catch(e=>{console.error(e.stderr||e.stack);process.exitCode=1;});

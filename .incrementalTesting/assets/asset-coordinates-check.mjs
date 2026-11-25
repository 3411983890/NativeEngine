import * as fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {importAsset} from '../../engine/assets/import-service.mjs';
import {readGltf} from '../../engine/assets/gltf-document.mjs';
import {sha256Hex} from '../../engine/assets/canonical.mjs';
import {writeAssetFixture,writeCharacterFixture} from '../../tools/asset-fixtures.mjs';
import {encodeCentimeterZUp} from '../../tools/offline/coordinate-fixture.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile),coordinates={unit:'cm',upAxis:'+Z',forwardAxis:'-Y'};
export async function checkCoordinateImports({publicContent=false}={}) {
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const output=path.join(root,'build/asset-import-smoke/coordinates');await fs.mkdir(output,{recursive:true});
  const host=path.join(root,'build/assets-host'),compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
  const objects=['engine_native_animation.cpp.o','engine_native_animation_features.cpp.o','engine_native_animation_asset.cpp.o','engine_native_determinism.cpp.o'].map(name=>path.join(host,'objects',name)),test=path.join(host,'asset_coordinate_tests.exe');
  await run(compiler,['-std=c++17','-O2','-static','tests/native_asset_coordinates.cpp',...objects,'-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const cases=[];
  cases.push({name:'static',file:await writeAssetFixture(path.join(output,'source')),camera:'7,6,10,0,1,1',size:64},{name:'character',file:await writeCharacterFixture(path.join(output,'source')),camera:'3,3,9,0,1,5',size:64});
  if(publicContent){
    const cache=path.join(root,'build/asset-import-smoke/public'),source=JSON.parse(await fs.readFile(path.join(cache,'source.json'),'utf8')),file=path.join(cache,'CesiumMan.glb');assert.equal(sha256Hex(await fs.readFile(file)),source.sha256);
    for(const notice of ['metadata.json','Cesium-logo-notice.txt'])await fs.copyFile(path.join(cache,notice),path.join(output,'public-'+notice));
    await fs.writeFile(path.join(output,'public-source.json'),JSON.stringify({...source,derivedEncoding:'centimetre, +Z up, -Y front; generated from cached source'},null,2)+'\n');
    cases.push({name:'public-cesium',file,camera:'3,3,9,0,1,5',size:256});
  }
  const results=[];
  for(const item of cases) {
    const doc=encodeCentimeterZUp(await readGltf(item.file)),file=path.join(output,`${item.name}-cm-zup.gltf`);await fs.writeFile(file,JSON.stringify(doc)+'\n');
    const original=await importAsset({source:item.file,output:path.join(output,'imports'),size:item.size}),converted=await importAsset({source:file,output:path.join(output,'imports'),size:item.size,coordinates});
    let contracts;
    if(original.manifest.kind==='character')contracts=JSON.parse((await run(test,[path.join(original.directory,'character/character.json'),path.join(converted.directory,'character/character.json')],{cwd:root,windowsHide:true,timeout:30000,maxBuffer:32768})).stdout);
    const reports=[],screenshots=[],record=path.join(output,item.name+'.record.csv');
    for(const [mode,asset]of [['original',original],['converted',converted]]){
      const report=path.join(output,`${item.name}-${mode}.json`),screenshot=path.join(output,`${item.name}-${mode}.png`);screenshots.push(screenshot);
      await run(path.join(host,'NativeGPU.exe'),['--smoke','--no-stress','--asset',asset.directory,'--frames','240','--camera',item.camera,'--animation-features','off','--audio','off','--screenshot-frame','30','--screenshot',screenshot,'--report',report,...(mode==='converted'?['--record',record]:[])],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
      const data=JSON.parse(await fs.readFile(report,'utf8'));assert.equal(data.success,true);reports.push(data);
      if(asset.manifest.kind==='character')assert.equal(data.animation.bones,asset.manifest.bones);else assert.equal(data.physics.convexMeshes,2);
    }
    const replayReport=path.join(output,item.name+'-replay.json');await run(path.join(host,'NativeGPU.exe'),['--smoke','--no-stress','--asset',converted.directory,'--frames','240','--camera',item.camera,'--animation-features','off','--audio','off','--replay',record,'--replay-strict','--report',replayReport],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
    const replay=JSON.parse(await fs.readFile(replayReport,'utf8'));assert.equal(replay.success,true);assert.equal(replay.replay.mismatches,0);assert.equal(replay.determinism.finalHash,reports[1].determinism.finalHash);
    const images=await Promise.all(screenshots.map(file=>sharp(file).ensureAlpha().raw().toBuffer()));assert.equal(images[0].length,images[1].length);
    let changed=0,colored=0,exactChangedPixels=0,maxChannelDifference=0;for(let i=0;i<images[0].length;i+=4){const delta=Math.max(...[0,1,2].map(k=>Math.abs(images[0][i+k]-images[1][i+k])));if(delta>12)changed++;if(delta)exactChangedPixels++;maxChannelDifference=Math.max(maxChannelDifference,delta);const [r,g,b]=images[0].subarray(i,i+3);if(Math.max(r,g,b)-Math.min(r,g,b)>25)colored++;}
    assert.ok(colored>100,'Coordinate reference must show visible textured content');assert.ok(changed<=Math.max(64,colored*.02),`Coordinate conversion changed the native image: ${changed} / ${colored}`);
    results.push({name:item.name,success:true,original:original.directory,converted:converted.directory,...(contracts?{contracts}:{}),coloredPixels:colored,changedPixels:changed,exactChangedPixels,maxChannelDifference,strictReplay:true,hash:replay.determinism.finalHash});
  }
  const evidence={success:true,coordinates,results};await fs.writeFile(path.join(output,publicContent?'public-acceptance.json':'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkCoordinateImports({publicContent:process.argv.includes('--public')}).then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.stderr??error.stack);process.exitCode=1;});

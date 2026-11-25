#!/usr/bin/env node
import * as fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {randomUUID} from 'node:crypto';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {importAsset,appendAnimation,attachModel} from '../../engine/assets/import-service.mjs';
import {loadScene,buildScene} from '../../engine/scene/index.mjs';
import {writeMotionFixture} from '../../tools/asset-fixtures.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkCharacterInstances(){
  const output=path.join(root,'build/asset-import-smoke'),motion=await writeMotionFixture(path.join(output,'instances-source'));
  const base=await importAsset({source:motion.characterSource,output,size:64}),action=await appendAnimation({directory:base.directory,source:motion.source});
  const doc=JSON.parse(await fs.readFile(motion.characterSource,'utf8')),bytes=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64');
  for(const mesh of doc.meshes)for(const primitive of mesh.primitives){const accessor=doc.accessors[primitive.attributes.POSITION],view=doc.bufferViews[accessor.bufferView];for(let i=0;i<accessor.count;i++){const at=(view.byteOffset??0)+(accessor.byteOffset??0)+i*12;bytes.writeFloatLE(bytes.readFloatLE(at)*.6,at);}}
  doc.buffers[0].uri='data:application/octet-stream;base64,'+bytes.toString('base64');const slimSource=path.join(output,'instances-source/slim.gltf');await fs.writeFile(slimSource,JSON.stringify(doc));
  const slim=await importAsset({source:slimSource,output,size:64}),scene=await loadScene(path.join(base.directory,'preview.scene.json')),file=path.join(output,`instances-${randomUUID()}.scene.json`);
  delete scene.materials;for(const entity of scene.entities)delete entity.material;
  scene.entities.find(e=>e.kind==='player').position=[-5,0,0];await fs.writeFile(file,JSON.stringify(scene)+'\n');
  await attachModel({directory:base.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});
  await attachModel({directory:base.directory,scenePath:file,expectedRevision:1,characterOptions:{clip:'idle',speed:.5,timeOffset:.1,position:[-1.8,0,0]}});
  await attachModel({directory:action.directory,scenePath:file,expectedRevision:2,characterOptions:{clip:'vendor_Raise',speed:0,timeOffset:.5,position:[1.8,0,0],rootMotion:'preserve'}});
  await attachModel({directory:slim.directory,scenePath:file,expectedRevision:3,characterOptions:{clip:'run',speed:1.5,timeOffset:.35,position:[5,0,0]}});
  const compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'),host=path.join(root,'build/assets-host');
  const objects=['host_scene_loader.cpp.o','engine_native_game.cpp.o','engine_native_physics.cpp.o','engine_native_animation.cpp.o','engine_native_animation_features.cpp.o','engine_native_animation_asset.cpp.o','engine_native_determinism.cpp.o','engine_native_assets_sha256.cpp.o'].map(name=>path.join(host,'objects',name));
  const test=path.join(host,'character_instance_tests.exe');await run(compiler,['-std=c++17','-O2','-static','tests/native_character_instances.cpp',...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const contracts=JSON.parse((await run(test,[file],{cwd:root,windowsHide:true,timeout:30000})).stdout);await fs.writeFile(path.join(output,'instances-contracts.json'),JSON.stringify(contracts)+'\n');
  const exe=path.join(host,'NativeGPU.exe'),record=path.join(output,'instances.record.json'),common=['--smoke','--scene',file,'--textures',path.join(output,'textures'),'--frames','240','--camera','0,5,18,0,1,0','--animation-features','off','--audio','off'];
  for(const replay of [false,true]){
    const report=path.join(output,replay?'instances-replay.json':'instances-native.json');
    await run(exe,[...common,...(replay?['--replay',record,'--replay-strict']:['--record',record,'--screenshot-frame','30','--screenshot',path.join(output,'instances.png')]),'--report',report],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
    const data=JSON.parse((await fs.readFile(report,'utf8')).replace(/^\uFEFF/,'')),skin=data.probes.skinning;
    assert.equal(data.success,true);assert.equal(data.animation.instances.length,4);assert.equal(skin.bindingsLastFrame,4);assert.equal(skin.geometryEntries,2);assert.equal(skin.geometryUploads,2);
    // Counters restart at frame 160. Replay compares the final tick before its draw.
    assert.ok(data.frames>=239);assert.ok(skin.draws>=(data.frames-160)*8);
    assert.ok(data.resizeEvents>0&&data.rendererReinitializations>0);assert.equal(new Set(data.animation.instances.map(c=>c.poseHash)).size,4);
    if(replay){assert.equal(data.replay.mismatches,0);assert.equal(data.replay.compared,240);}
  }
  const a=JSON.parse(await fs.readFile(path.join(output,'instances-native.json'),'utf8')),b=JSON.parse(await fs.readFile(path.join(output,'instances-replay.json'),'utf8'));assert.deepEqual(a.animation.instances,b.animation.instances);assert.equal(a.determinism.finalHash,b.determinism.finalHash);
  const built=await buildScene(file,path.join(output,'instances-packages'));const packageReport=path.join(output,'instances-package.json');
  await run(exe,['--smoke','--no-stress','--scene',path.join(built.packageDirectory,'scene.json'),'--textures',path.join(built.packageDirectory,'textures'),'--frames','4','--animation-features','off','--audio','off','--report',packageReport],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const packaged=JSON.parse(await fs.readFile(packageReport,'utf8'));assert.equal(packaged.success,true);assert.equal(packaged.animation.instances.length,4);
  const crowd=await loadScene(file),actor=crowd.entities.at(-1);for(let i=0;i<60;i++)crowd.entities.push({...structuredClone(actor),id:randomUUID(),position:[(i%10-5)*3,0,Math.floor(i/10)*3+3]});
  const crowdFile=path.join(output,'instances-64.scene.json'),crowdReport=path.join(output,'instances-64.json');await fs.writeFile(crowdFile,JSON.stringify(crowd)+'\n');
  await run(exe,['--smoke','--no-stress','--scene',crowdFile,'--textures',path.join(output,'textures'),'--frames','12','--animation-features','off','--audio','off','--report',crowdReport],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const crowdData=JSON.parse(await fs.readFile(crowdReport,'utf8'));assert.equal(crowdData.success,true);assert.equal(crowdData.probes.skinning.bindingsLastFrame,64);assert.equal(crowdData.probes.skinning.geometryEntries,2);assert.equal(crowdData.probes.skinning.draws,crowdData.frames*64*2);
  const hot=await loadScene(file),hotId=randomUUID(),hotFile=path.join(output,`instances-hot-${hotId}.scene.json`),readyImage=path.join(output,`instances-ready-${hotId}.png`),hotReport=path.join(output,'instances-hot.json');await fs.writeFile(hotFile,JSON.stringify(hot)+'\n');
  let finished=false;const hotRun=run(exe,['--smoke','--no-stress','--watch','--scene',hotFile,'--textures',path.join(output,'textures'),'--frames','600','--animation-features','off','--audio','off','--screenshot-frame','1','--screenshot',readyImage,'--report',hotReport],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768}).finally(()=>{finished=true;});
  const deadline=Date.now()+30000;while(true){try{await fs.stat(readyImage);break;}catch(error){if(error.code!=='ENOENT')throw error;}if(finished||Date.now()>deadline){await hotRun;throw new Error('Hot reload fixture never reached its first rendered frame');}await new Promise(resolve=>setTimeout(resolve,25));}
  const hotActor=hot.entities.at(-1),hotSource=hotActor.character.materials.m1;hot.materials.hot_character_part={...hot.materials[hotSource],roughness:.3};hotActor.character.materials.m1='hot_character_part';hot.revision++;
  await fs.writeFile(hotFile+'.tmp',JSON.stringify(hot)+'\n');await fs.rename(hotFile+'.tmp',hotFile);await hotRun;
  const hotData=JSON.parse(await fs.readFile(hotReport,'utf8'));assert.equal(hotData.success,true);assert.ok(hotData.hotReloads>=1);assert.ok(hotData.rendererReinitializations>=1);
  // The host adds one built-in grid material when its texture is available; it
  // is not an authored scene material but does occupy a renderer material slot.
  const gridAvailable=await fs.stat(path.join(root,'assets/builtin/textures/engine_grid.albedo.ngtex')).then(()=>true,error=>{if(error.code==='ENOENT')return false;throw error;});
  assert.equal(hotData.materialHandle.count,Object.keys(hot.materials).length+(gridAvailable?1:0));assert.equal(hotData.animation.instances.length,4);
  const {data,info}=await sharp(path.join(output,'instances.png')).ensureAlpha().raw().toBuffer({resolveWithObject:true});
  const colored=[];let minX=info.width,maxX=0;for(let y=0;y<info.height;y++)for(let x=0;x<info.width;x++){const at=(y*info.width+x)*4,red=data[at]>25&&data[at]>data[at+1]*1.5&&data[at]>data[at+2]*1.5,blue=data[at+2]>25&&data[at+2]>data[at+1]*1.5&&data[at+2]>data[at]*1.5;if(red||blue){colored.push({x,red});minX=Math.min(minX,x);maxX=Math.max(maxX,x);}}
  const bands=Array.from({length:4},()=>({red:0,blue:0}));for(const pixel of colored)bands[Math.min(3,Math.floor((pixel.x-minX)/(maxX-minX+1)*4))][pixel.red?'red':'blue']++;
  assert.ok(bands.every(b=>b.red>30&&b.blue>30),`Four visible independent roles required: ${JSON.stringify(bands)}`);
  const evidence={success:true,sceneFile:file,contracts,instances:4,budgetInstances:64,sharedGeometry:2,strictReplay:true,packagedScene:true,materialHotReload:true,pixels:bands};await fs.writeFile(path.join(output,'instances-acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkCharacterInstances().then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.stderr??error.stack);process.exitCode=1;});

import * as fs from 'node:fs/promises';
import path from 'node:path';
import {randomUUID} from 'node:crypto';
import {execFile} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {promisify} from 'node:util';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {writeCharacterFixture} from '../../tools/asset-fixtures.mjs';
import {importAsset,attachModel,fitSceneCharacter} from '../../engine/assets/import-service.mjs';
import {loadScene,commitSceneUpdate} from '../../engine/scene/index.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkCharacterController(){
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const output=path.join(root,'build/asset-import-smoke/controller');await fs.mkdir(output,{recursive:true});
  const imported=await importAsset({source:await writeCharacterFixture(path.join(output,'source')),output:path.join(output,'imports'),size:64}),scene=await loadScene(path.join(imported.directory,'preview.scene.json'));
  delete scene.materials;for(const entity of scene.entities){delete entity.character;delete entity.material;}
  const file=path.join(output,`controller-${randomUUID()}.scene.json`);await fs.writeFile(file,JSON.stringify(scene)+'\n');
  await attachModel({directory:imported.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});
  await attachModel({directory:imported.directory,scenePath:file,expectedRevision:1,collider:true,characterOptions:{position:[2,0,0]}});
  const fitted=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:2,collider:true,characterOptions:{position:[-2,1,0]}});
  await commitSceneUpdate(file,{projectId:fitted.projectId,expectedRevision:3},next=>{next.entities.at(-1).physics={mode:'dynamic',mass:14};return next;});
  const host=path.join(root,'build/assets-host'),compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
  const objects=['engine_native_game.cpp.o','engine_native_physics.cpp.o','engine_native_animation.cpp.o','engine_native_animation_features.cpp.o','engine_native_animation_asset.cpp.o','engine_native_determinism.cpp.o'].map(n=>path.join(host,'objects',n)),library=path.join(host,'gameplay/nativegpu-gameplay.a');
  const test=path.join(host,'character_controller_tests.exe');await run(compiler,['-std=c++17','-O2','-static','tests/native_character_controller.cpp',...objects,path.join(host,'objects/host_scene_loader.cpp.o'),path.join(host,'objects/engine_native_assets_sha256.cpp.o'),library,'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const contracts=JSON.parse((await run(test,[file],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768})).stdout);
  const legacy=path.join(host,'asset_movement_tests.exe');await run(compiler,['-std=c++17','-O2','-static','tests/native_simulation.cpp',...objects,path.join(host,'objects/engine_native_geometry_class.cpp.o'),path.join(host,'objects/engine_native_material_handle.cpp.o'),library,'-o',legacy],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  await run(legacy,[],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const record=path.join(output,'controller.record.csv'),base=['--smoke','--no-stress','--scene',file,'--textures',path.join(output,'textures'),'--frames','240','--animation-features','off','--audio','off'];
  const reports=[];for(const replay of [false,true]){
    const report=path.join(output,replay?'replay.json':'native.json');await run(path.join(host,'NativeGPU.exe'),[...base,'--report',report,...(replay?['--replay',record,'--replay-strict']:['--record',record,'--screenshot-frame','200','--screenshot',path.join(output,'controller.png')])],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
    const data=JSON.parse(await fs.readFile(report,'utf8'));assert.equal(data.success,true);assert.equal(data.animation.instances.length,3);assert.ok(data.characterController.height>1.5&&data.characterController.radius<.5);reports.push(data);
  }
  assert.equal(reports[1].replay.mismatches,0);assert.equal(reports[0].determinism.finalHash,reports[1].determinism.finalHash);
  const current=await loadScene(file),player=current.entities.find(e=>e.kind==='player');await commitSceneUpdate(file,{projectId:current.projectId,expectedRevision:current.revision},next=>{next.entities.find(e=>e.id===player.id).character.scale=[.01,.01,.01];return next;});
  const tiny=await loadScene(file);await fitSceneCharacter({scenePath:file,entityId:player.id,expectedRevision:tiny.revision,mode:'humanoid'});
  const tinyReport=path.join(output,'tiny.json'),tinyImage=path.join(output,'tiny.png');await run(path.join(host,'NativeGPU.exe'),[...base,'--report',tinyReport,'--screenshot-frame','30','--screenshot',tinyImage],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const tinyData=JSON.parse(await fs.readFile(tinyReport,'utf8'));assert.equal(tinyData.success,true);assert.ok(tinyData.characterController.height<.03&&tinyData.characterController.eyeHeight<.03);
  const pixels=await sharp(tinyImage).ensureAlpha().raw().toBuffer();let colored=0;for(let i=0;i<pixels.length;i+=4)if(Math.max(...pixels.subarray(i,i+3))-Math.min(...pixels.subarray(i,i+3))>25)colored++;assert.ok(colored>100,'Tiny fitted character must remain visible with its adaptive camera and near plane');
  const evidence={success:true,contracts,legacyMovement:true,strictReplay:true,tinyCharacter:{height:tinyData.characterController.height,eyeHeight:tinyData.characterController.eyeHeight,coloredPixels:colored}};
  await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkCharacterController().then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.stderr??error.stack);process.exitCode=1;});

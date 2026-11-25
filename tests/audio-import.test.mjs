import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {importAsset,attachAudio,verifyImport} from '../engine/assets/import-service.mjs';
import {loadScene,buildScene,validateScene,undoSceneFile} from '../engine/scene/index.mjs';
import {inspectNgsnd} from '../engine/assets/audio-data.mjs';
import {writeAudioFixture} from '../tools/offline/audio-fixture.mjs';
import {main} from '../tools/assets.mjs';
import {createEditorService} from '../editor/service.mjs';

async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-audio-import-'));try{const source=await writeAudioFixture(path.join(dir,'tone.wav')),asset=await importAsset({source,output:path.join(dir,'assets/imported')});await fn(dir,asset,source);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('audio import has deterministic verified preview and finite cooked samples',()=>fixture(async(dir,asset,source)=>{
  assert.equal((await importAsset({source,output:path.join(dir,'assets/imported')})).manifest.id,asset.manifest.id);await verifyImport(asset.directory);
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));assert.equal(scene.audio.emitters[0].entity,scene.entities[0].id);assert.equal(scene.audio.emitters[0].spatial,false);
  const sound=Object.values(scene.audio.sounds)[0],bytes=await fs.readFile(path.join(asset.directory,sound.file));assert.equal(inspectNgsnd(bytes).frames,12000);
  for(const n of [0,35,36,39,43,bytes.length-1])assert.throws(()=>inspectNgsnd(bytes.subarray(0,n)));
  const bad=Buffer.from(bytes);bad.writeFloatLE(NaN,44);assert.throws(()=>inspectNgsnd(bad));
}));
test('audio attachment revisions, options, undo and standalone dependency package',()=>fixture(async(dir,asset)=>{
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.audio;const file=path.join(dir,'assets/test.scene.json');await fs.writeFile(file,JSON.stringify(scene));
  const result=await attachAudio({directory:asset.directory,scenePath:file,expectedRevision:0,options:{bus:'music',spatial:false,loop:true,pitch:.8,offset:[1,2,3]}});
  assert.equal(result.revision,1);assert.equal(result.audio.emitters[0].pitch,.8);
  const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.includedAssetTypes.includes('cooked-audio'));assert.equal(built.manifest.dependencies.filter(d=>d.path.endsWith('.ngsnd')).length,1);
  assert.equal((await buildScene(file,path.join(dir,'packages'))).cacheHit,true);
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:0}));
  const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal(undone.scene.audio,undefined);
}));
test('invalid sound references, dangling emitter, bounds and malformed options reject',()=>fixture(async(dir,asset)=>{
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));
  for(const edit of [s=>Object.values(s.audio.sounds)[0].file='../escape.ngsnd',s=>Object.values(s.audio.sounds)[0].sha256='bad',s=>s.audio.emitters[0].entity='00000000-0000-4000-8000-000000000000',s=>s.audio.emitters[0].pitch=8,s=>s.audio.emitters[0].spatial=1,s=>Object.assign(s.audio.emitters[0],{minDistance:10,maxDistance:2})]){const next=structuredClone(scene);edit(next);assert.equal(validateScene(next).valid,false);}
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:path.join(asset.directory,'preview.scene.json'),expectedRevision:0,options:{unknown:true}}));
  const ref=Object.values(scene.audio.sounds)[0];await fs.appendFile(path.join(asset.directory,ref.file),'bad');await assert.rejects(buildScene(path.join(asset.directory,'preview.scene.json'),path.join(dir,'packages')));await assert.rejects(verifyImport(asset.directory));
}));
test('CLI and editor can attach an audio package through the normal asset action',()=>fixture(async(dir,asset)=>{
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.audio;const file=path.join(dir,'assets/test.scene.json');await fs.writeFile(file,JSON.stringify(scene));
  await main(['attach','--in',asset.directory,'--scene',file,'--revision','0']);
  const service=createEditorService({root:dir});const result=await service.call('asset_attach',{id:asset.manifest.id,scenePath:'assets/test.scene.json',expectedRevision:1,audioOptions:{loop:true,spatial:false}});assert.equal(result.ok,true,JSON.stringify(result));assert.equal((await loadScene(file)).audio.emitters.length,2);await service.close();
}));

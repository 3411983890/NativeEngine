import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {writeSceneFixture} from '../tools/offline/scene-fixture.mjs';
import {importAsset,checkoutSceneImport,verifyImport,attachModel,attachAudio} from '../engine/assets/import-service.mjs';
import {loadScene,commitScene,commitSceneUpdate,undoSceneFile,buildScene,sceneHash,validateScene} from '../engine/scene/index.mjs';
import {materialInstance} from '../tools/material.mjs';
import {createEditorService} from '../editor/service.mjs';
import {createToolService} from '../ai/tool-service.mjs';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-scene-import-'));try{await fn(dir,await writeSceneFixture(path.join(dir,'external')));}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('scene import locks and relocates custom scripts, geometry, character, textures and audio',()=>fixture(async(dir,source)=>{
  const a=await importAsset({source,output:path.join(dir,'project/assets/imported')}),b=await importAsset({source,output:path.join(dir,'project/assets/imported')});assert.equal(a.manifest.id,b.manifest.id);await verifyImport(a.directory);
  const files=a.manifest.files.map(f=>f.file);for(const file of ['scripts/vendor/move.lua','scripts/vendor/library.lua','gameplay/components.json','scene.scene.json'])assert.ok(files.includes(file));assert.ok(files.some(f=>f.endsWith('.ngsnd')));assert.ok(files.some(f=>f.endsWith('animation.bin')));assert.ok(files.some(f=>f.endsWith('.ngtex')));
  const original=await loadScene(source),copy=await loadScene(path.join(a.directory,'scene.scene.json'));assert.deepEqual(copy.entities.map(e=>e.id),original.entities.map(e=>e.id));assert.deepEqual(copy.entities.at(-1).components,original.entities.at(-1).components);assert.equal(copy.projectId,original.projectId);assert.ok(copy.entities.find(e=>e.mesh).mesh.file.startsWith('meshes/'));
  await fs.rename(path.join(dir,'external'),path.join(dir,'source-removed'));
  const opened=await checkoutSceneImport({directory:a.directory,outputDirectory:path.join(dir,'project/assets/scenes')}),scene=await loadScene(opened.scenePath);assert.equal(sceneHash(scene),sceneHash(copy));
  const actor=scene.entities.at(-1),edited=await commitScene(opened.scenePath,{projectId:scene.projectId,expectedRevision:scene.revision,commands:[{op:'set',entityId:actor.id,changes:{name:'Edited externally imported scene'}}]});assert.equal(edited.scene.entities.at(-1).name,'Edited externally imported scene');
  const undone=await undoSceneFile(opened.scenePath,{projectId:scene.projectId,expectedRevision:edited.scene.revision});assert.equal(undone.scene.entities.at(-1).name,actor.name);
  const material=Object.keys(scene.materials)[0];assert.ok(materialInstance(scene,material,'external_copy').scene.materials.external_copy);
  const built=await buildScene(opened.scenePath,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(f=>f.path==='scripts/vendor/library.lua'));
}));
test('scene import rejects altered dependencies before publishing',()=>fixture(async(dir,source)=>{
  const out=path.join(dir,'imports'),scene=await loadScene(source),ref=scene.entities.find(e=>e.mesh).mesh;await fs.appendFile(path.join(path.dirname(source),ref.file),'bad');await assert.rejects(importAsset({source,output:out}),/changed/);
  assert.equal((await fs.readdir(out)).length,0);
}));
test('scene dependencies cannot leave the source through a junction',()=>fixture(async(dir,source)=>{
  const scene=await loadScene(source),ref=scene.entities.find(e=>e.mesh).mesh,outside=path.join(dir,'outside');await fs.mkdir(outside);await fs.copyFile(path.join(path.dirname(source),ref.file),path.join(outside,'mesh.json'));await fs.symlink(outside,path.join(path.dirname(source),'escape'),'junction');
  await commitSceneUpdate(source,{projectId:scene.projectId,expectedRevision:scene.revision},next=>{next.entities.find(e=>e.mesh).mesh.file='escape/mesh.json';return next;});
  await assert.rejects(importAsset({source,output:path.join(dir,'imports')}),/escaped/);
}));
test('concurrent scene operations isolate independent custom catalogs and keep pure editor context',async()=>{
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-scene-context-'));try {
    const files=await Promise.all(['number','string'].map(async(type,i)=>{const root=path.join(dir,String(i)),file=await writeSceneFixture(root),catalogPath=path.join(root,'gameplay/components.json'),catalog=JSON.parse(await fs.readFile(catalogPath));catalog.external_test={schema:{type:'object',properties:{value:{type}},required:['value'],additionalProperties:false}};await fs.writeFile(catalogPath,JSON.stringify(catalog));
      const scene=await loadScene(file);await commitSceneUpdate(file,{projectId:scene.projectId,expectedRevision:scene.revision},next=>{next.entities.at(-1).components.external_test={value:i?'second':2};return next;});return file;}));
    await Promise.all(files.map(async(file,i)=>{for(let n=0;n<4;n++){const scene=await loadScene(file);assert.equal(validateScene(scene).valid,true);assert.ok(sceneHash(scene));assert.equal(validateScene(scene,{assetsDirectory:path.dirname(files[1-i])}).valid,false);assert.equal(validateScene(scene).valid,true);const raw=structuredClone(scene);assert.equal(validateScene(raw).valid,false);assert.equal(validateScene(raw,{assetsDirectory:path.dirname(file)}).valid,true);await commitScene(file,{projectId:scene.projectId,expectedRevision:scene.revision,commands:[{op:'set',entityId:scene.entities[0].id,changes:{name:`Context ${i} revision ${n}`}}]});}}));
  }finally{await fs.rm(dir,{recursive:true,force:true});}
});
test('editor opens a mutable scene copy and preserves the imported package',()=>fixture(async(dir,source)=>{
  const project=path.join(dir,'project'),asset=await importAsset({source,output:path.join(project,'assets/imported')}),service=createEditorService({root:project});
  try{
    const outside=path.join(dir,'outside-output');await fs.mkdir(outside);const link=path.join(project,'assets/scenes');await fs.symlink(outside,link,'junction');const escaped=await service.call('asset_open_scene',{id:asset.manifest.id});assert.equal(escaped.ok,false);assert.equal(escaped.error.code,'INVALID_PATH');assert.deepEqual(await fs.readdir(outside),[]);await fs.unlink(link);
    const forbidden=await service.call('scene_apply',{scenePath:`assets/imported/${asset.manifest.id}/scene.scene.json`,transaction:{}});assert.equal(forbidden.ok,false);assert.equal(forbidden.error.code,'INVALID_PATH');
    const opened=await service.call('asset_open_scene',{id:asset.manifest.id});assert.equal(opened.ok,true,JSON.stringify(opened));const scene=await service.call('scene_open',{scenePath:opened.result.scenePath});assert.equal(scene.ok,true,JSON.stringify(scene));const projectInfo=await service.call('project_info');assert.ok(projectInfo.result.scenes.every(s=>!s.path.startsWith('assets/imported/')));
    const tools=createToolService({root:project});for(const name of ['scene_validate','gameplay_validate']){const result=await tools.callTool(name,{scenePath:opened.result.scenePath});assert.equal(result.ok,true,JSON.stringify(result));assert.equal(result.result.valid,true);}await verifyImport(asset.directory);
  }finally{await service.close();}
}));
test('native scene build entry imports directly and its file manifest rejects tampering',()=>fixture(async(dir,source)=>{
  const built=await buildScene(source,path.join(dir,'packages')),entry=path.join(built.packageDirectory,'scene.json'),imported=await importAsset({source:entry,output:path.join(dir,'imports')});assert.equal(imported.manifest.kind,'scene');
  await fs.appendFile(path.join(built.packageDirectory,'scripts/vendor/library.lua'),'-- changed');await assert.rejects(importAsset({source:entry,output:path.join(dir,'imports')}),/package dependency changed/);
}));
test('legacy sound banks relocate to direct scene sound references',()=>fixture(async(dir,source)=>{
  const scene=await loadScene(source),ref=Object.values(scene.audio.sounds)[0],soundName=scene.audio.emitters[0].sound,assets=path.dirname(source);await fs.mkdir(path.join(assets,'audio'));await fs.copyFile(path.join(assets,ref.file),path.join(assets,'audio/legacy.ngsnd'));await fs.writeFile(path.join(assets,'audio/bank.json'),JSON.stringify({sounds:[{name:soundName,file:'assets/audio/legacy.ngsnd',sha256:ref.sha256}]}));
  await commitSceneUpdate(source,{projectId:scene.projectId,expectedRevision:scene.revision},next=>{delete next.audio.sounds;return next;});const imported=await importAsset({source,output:path.join(dir,'imports')}),copy=await loadScene(path.join(imported.directory,'scene.scene.json'));assert.equal(copy.audio.sounds[soundName].sha256,ref.sha256);assert.ok(copy.audio.sounds[soundName].file.startsWith('audio/'));
}));
test('packaged scene CLI runs with only its declared JavaScript modules',()=>fixture(async(dir,source)=>{
  const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),runtime=path.join(dir,'runtime');await fs.mkdir(path.join(runtime,'engine'),{recursive:true});await fs.cp(path.join(root,'engine/scene'),path.join(runtime,'engine/scene'),{recursive:true});await fs.mkdir(path.join(runtime,'engine/assets'));for(const name of ['audio-data.mjs','gltf-document.mjs','canonical.mjs','environment-container.mjs'])await fs.copyFile(path.join(root,'engine/assets',name),path.join(runtime,'engine/assets',name));await fs.mkdir(path.join(runtime,'engine/material'));for(const name of ['texture-mapping.mjs','ior.mjs'])await fs.copyFile(path.join(root,'engine/material',name),path.join(runtime,'engine/material',name));await fs.mkdir(path.join(runtime,'tools'));await fs.copyFile(path.join(root,'tools/scene.mjs'),path.join(runtime,'tools/scene.mjs'));
  const {stdout}=await promisify(execFile)(process.execPath,[path.join(runtime,'tools/scene.mjs'),'validate',source],{cwd:runtime,windowsHide:true});assert.equal(JSON.parse(stdout).ok,true);
}));
test('scene import relocates baked fracture data and builds it into the dependency package',async()=>{
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-scene-fracture-'));try{const source=await writeSceneFixture(path.join(dir,'external'),{fracture:true}),asset=await importAsset({source,output:path.join(dir,'imports')}),file=path.join(asset.directory,'scene.scene.json'),scene=await loadScene(file),ref=scene.entities.find(e=>e.destructible?.voronoi).destructible.voronoi;assert.ok(ref.startsWith('simulation/'));assert.ok(asset.manifest.files.some(f=>f.file===ref));const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(f=>f.path===ref));}finally{await fs.rm(dir,{recursive:true,force:true});}
});

test('nested scenes attach global model, character and audio packages with immutable local dependencies and editor undo',async()=>{
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-nested-attach-'));
  const catalog=path.join(dir,'assets/imported'),nested=path.join(dir,'assets/levels/sample');
  try {
    const file=await writeSceneFixture(nested,{importDirectory:catalog}),scene=await loadScene(file),entries=await fs.readdir(catalog);
    assert.equal(entries.length,3);
    for(const id of entries){const source=await verifyImport(path.join(catalog,id)),local=await verifyImport(path.join(nested,'imported',id));assert.deepEqual(local,source);}
    assert.ok(scene.entities.find(e=>e.mesh).mesh.file.startsWith('imported/'));
    assert.ok(scene.entities.find(e=>e.character).character.file.startsWith('imported/'));
    assert.ok(Object.values(scene.audio.sounds)[0].file.startsWith('imported/'));
    const audioId=entries.find(id=>scene.audio.sounds[Object.keys(scene.audio.sounds)[0]].file.includes(id));
    const service=createEditorService({root:dir});
    try {
      const attached=await service.call('asset_attach',{id:audioId,scenePath:'assets/levels/sample/external.scene.json',expectedRevision:4,audioOptions:{loop:true}});
      assert.equal(attached.ok,true,JSON.stringify(attached));assert.equal((await loadScene(file)).audio.emitters.length,2);
      const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:5});assert.equal(undone.scene.audio.emitters.length,1);
      assert.equal((await fs.readdir(path.join(nested,'imported'))).length,3);
    }finally{await service.close();}
    await fs.rename(catalog,path.join(dir,'catalog-removed'));
    const built=await buildScene(file,path.join(dir,'packages'));
    for(const suffix of ['.mesh.json','character.json','animation.bin','.ngsnd','.ngtex'])assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith(suffix)),suffix);
    const imported=await importAsset({source:file,output:path.join(dir,'scene-imports')});await verifyImport(imported.directory);
  }finally{await fs.rm(dir,{recursive:true,force:true});}
});

test('nested attachment rejects stale revisions, invalid options, changed packages and destination links without changing scene',()=>fixture(async(dir,source)=>{
  const asset=await importAsset({source:await fs.realpath(path.join(path.dirname(source),'tone.wav')),output:path.join(dir,'catalog')}),nested=path.join(dir,'nested');
  await fs.mkdir(nested);for(const name of ['gameplay','scripts'])await fs.cp(path.join(path.dirname(source),name),path.join(nested,name),{recursive:true});const file=path.join(nested,'main.scene.json'),scene=await loadScene(source);delete scene.audio;
  await fs.writeFile(file,JSON.stringify(scene));const original=await fs.readFile(file);
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:0}),e=>e.code==='REVISION_CONFLICT');
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:4,options:{pitch:8}}),/invalid/);
  await assert.rejects(fs.stat(path.join(nested,'imported')),e=>e.code==='ENOENT');
  const outside=path.join(dir,'outside');await fs.mkdir(outside);await fs.symlink(outside,path.join(nested,'imported'),'junction');
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:4}),/escapes/);assert.deepEqual(await fs.readdir(outside),[]);await fs.unlink(path.join(nested,'imported'));
  const target=path.join(nested,'imported',asset.manifest.id);await fs.mkdir(path.dirname(target));await fs.symlink(asset.directory,target,'junction');
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:4}),/escapes/);await fs.unlink(target);
  await fs.cp(asset.directory,target,{recursive:true});const sound=asset.manifest.files.find(f=>f.file.endsWith('.ngsnd'));await fs.appendFile(path.join(target,sound.file),'changed');
  await assert.rejects(attachAudio({directory:asset.directory,scenePath:file,expectedRevision:4}),/changed/);
  assert.deepEqual(await fs.readFile(file),original);assert.ok(!(await fs.readdir(path.dirname(target))).some(n=>n.startsWith('.attach-')));
}));

test('model attachment rejects escaped texture destinations and does not relocate packages on failed revision or options',()=>fixture(async(dir,source)=>{
  const modelDir=(await fs.readdir(path.join(path.dirname(source),'imported'))).map(id=>path.join(path.dirname(source),'imported',id));
  let asset;for(const directory of modelDir)if((await verifyImport(directory)).kind==='model')asset=directory;
  const nested=path.join(dir,'nested');await fs.mkdir(nested);for(const name of ['gameplay','scripts'])await fs.cp(path.join(path.dirname(source),name),path.join(nested,name),{recursive:true});const file=path.join(nested,'main.scene.json'),scene=await loadScene(source);await fs.writeFile(file,JSON.stringify(scene));const original=await fs.readFile(file);
  await assert.rejects(attachModel({directory:asset,scenePath:file,expectedRevision:0}),e=>e.code==='REVISION_CONFLICT');
  await assert.rejects(attachModel({directory:asset,scenePath:file,expectedRevision:4,characterOptions:{clip:'invalid'}}),/Character options/);
  await assert.rejects(fs.stat(path.join(nested,'imported')),e=>e.code==='ENOENT');
  const outside=path.join(dir,'outside-textures');await fs.mkdir(outside);await fs.symlink(outside,path.join(nested,'textures'),'junction');
  await assert.rejects(attachModel({directory:asset,scenePath:file,expectedRevision:4}),/escapes/);assert.deepEqual(await fs.readdir(outside),[]);assert.deepEqual(await fs.readFile(file),original);
}));

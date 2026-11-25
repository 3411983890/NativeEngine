import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {randomUUID} from 'node:crypto';
import {EventEmitter} from 'node:events';
import {execFileSync} from 'node:child_process';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {importAsset,appendAnimation,attachModel,listImports} from '../engine/assets/import-service.mjs';
import {loadScene,saveScene,buildScene,validateScene,undoSceneFile,previewTransaction} from '../engine/scene/index.mjs';
import {createEditorService} from '../editor/service.mjs';
import {patchMaterial,listMaterials,materialInstance,textureNames,applyMaterialPatch} from '../tools/material.mjs';

async function fixture(t) {
  const root=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-instances-'));t.after(()=>fs.rm(root,{recursive:true,force:true}));
  const motion=await writeMotionFixture(path.join(root,'source')),output=path.join(root,'assets/imported');
  const base=await importAsset({source:motion.characterSource,output,size:64}),action=await appendAnimation({directory:base.directory,source:motion.source});
  const scene=await loadScene(path.join(base.directory,'preview.scene.json'));
  // Preview ground materials are copied by attachment with the same namespace as character parts.
  delete scene.materials;for(const entity of scene.entities){delete entity.material;delete entity.character;}
  const file=path.join(root,'assets/main.scene.json');await saveScene(file,scene);
  await fs.mkdir(path.join(root,'assets/gameplay'),{recursive:true});await fs.copyFile('assets/gameplay/components.json',path.join(root,'assets/gameplay/components.json'));
  return {root,file,scene,base,action,output};
}
test('characters attach as independent production references with clips, namespaced materials and undo',async t=>{
  const {file,base,action,scene}=await fixture(t);
  const one=await attachModel({directory:base.directory,scenePath:file,expectedRevision:0,characterOptions:{clip:'idle',position:[-2,0,0]}});
  const two=await attachModel({directory:action.directory,scenePath:file,expectedRevision:1,characterOptions:{clip:'vendor_Raise',speed:.5,timeOffset:.25,position:[2,0,0],modelScale:[2,2,2],offset:[0,.1,0],yaw:.5,rootMotion:'preserve'}});
  const [a,b]=two.entities.filter(e=>e.character);assert.notEqual(a.id,b.id);assert.notEqual(a.character.file,b.character.file);
  assert.match(a.character.file,/character\/character.json$/);assert.ok(!b.character.file.includes('preview.json'));
  assert.equal(b.character.speed,.5);assert.equal(b.character.rootMotion,'preserve');assert.deepEqual(b.character.scale,[2,2,2]);assert.equal(b.collider,false);
  assert.notEqual(a.character.materials.m1,b.character.materials.m1);assert.ok(two.materials[b.character.materials.m1]);
  const header=action.manifest.files.find(f=>f.file==='character/character.json'),binary=action.manifest.files.find(f=>f.file==='character/animation.bin');
  assert.equal(b.character.sha256,header.sha256);assert.equal(b.character.binarySha256,binary.sha256);
  const project=path.dirname(path.dirname(file));assert.ok(textureNames(project).includes(b.character.materials.m1));
  await fs.writeFile(path.join(path.dirname(file),'textures/manifest.json'),JSON.stringify({textures:[{name:'existing'}]}));
  assert.ok(textureNames(project).includes('existing'));assert.ok(textureNames(project).includes(b.character.materials.m1));
  assert.ok(await fs.stat(path.join(path.dirname(file),'textures',b.character.materials.m1+'.albedo.ngtex')));
  await assert.rejects(attachModel({directory:action.directory,scenePath:file,expectedRevision:1}),e=>e.code==='REVISION_CONFLICT');
  const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:2});assert.deepEqual(undone.scene.entities,one.entities);
});
test('replacing player keeps entity identity and gameplay components; fixed timing is explicit',async t=>{
  const {file,action,scene}=await fixture(t),player=scene.entities.find(e=>e.kind==='player');
  const changed=await attachModel({directory:action.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});
  assert.equal(changed.entities.length,scene.entities.length);const actor=changed.entities.find(e=>e.kind==='player');assert.equal(actor.id,player.id);assert.deepEqual(actor.position,player.position);assert.equal(actor.collider,true);
  await assert.rejects(attachModel({directory:action.directory,scenePath:file,expectedRevision:1,characterOptions:{replacePlayer:true,speed:2}}),/fixed clip/);
  const fixed=await attachModel({directory:action.directory,scenePath:file,expectedRevision:1,characterOptions:{replacePlayer:true,clip:'vendor_Raise',speed:2}});
  assert.equal(fixed.entities.find(e=>e.kind==='player').character.speed,2);
  const listed=await listImports(path.dirname(action.directory));assert.ok(listed.find(e=>e.id===action.manifest.id).clips.includes('vendor_Raise'));
});
test('character options and scene transactions reject invalid paths, maps, timing and geometry combinations',async t=>{
  const {file,base}=await fixture(t),before=await fs.readFile(file);
  for(const characterOptions of [{unknown:1},{clip:'missing'},{speed:-1},{timeOffset:121},{modelScale:[1,0,1]},{replacePlayer:'true'},{rootMotion:'mystery'}])
    await assert.rejects(attachModel({directory:base.directory,scenePath:file,expectedRevision:0,characterOptions}));
  assert.deepEqual(await fs.readFile(file),before);
  const outside=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-role-link-'));t.after(()=>fs.rm(outside,{recursive:true,force:true}));await fs.cp(base.directory,path.join(outside,'package'),{recursive:true});
  const link=path.join(path.dirname(file),'linked-package');await fs.symlink(path.join(outside,'package'),link,'junction');
  await assert.rejects(attachModel({directory:link,scenePath:file,expectedRevision:0}),e=>e.code==='NG-E-ASSET-PATH');
  const scene=await attachModel({directory:base.directory,scenePath:file,expectedRevision:0}),entity=scene.entities.at(-1),ref=entity.character;
  for(const value of [{...ref,file:'../character.json'},{...ref,file:'C:/character.json'},{...ref,file:'foo\u0000.json'},{...ref,binarySha256:'bad'},{...ref,materials:{m1:'missing'}},{...ref,scale:[1,Infinity,1]},{...ref,speed:17}]){
    const copy=structuredClone(scene);copy.entities.at(-1).character=value;assert.equal(validateScene(copy).valid,false);
  }
  const copy=structuredClone(scene);copy.entities.at(-1).mesh={file:'foo.mesh.json',sha256:ref.sha256};assert.equal(validateScene(copy).valid,false);
  const tx={projectId:scene.projectId,expectedRevision:1,commands:[{op:'set',entityId:entity.id,changes:{character:{...ref,clip:'run',speed:2}}}]};
  assert.equal(previewTransaction(scene,tx).scene.entities.at(-1).character.clip,'run');
});
test('character instance limit includes the fallback player',async t=>{
  const {file,base}=await fixture(t),scene=await attachModel({directory:base.directory,scenePath:file,expectedRevision:0}),entity=scene.entities.at(-1);
  while(scene.entities.filter(e=>e.character).length<63)scene.entities.push({...structuredClone(entity),id:randomUUID()});
  assert.equal(validateScene(scene).valid,true);scene.entities.push({...structuredClone(entity),id:randomUUID()});assert.equal(validateScene(scene).valid,false);
  scene.entities.find(e=>e.kind==='player').character=structuredClone(entity.character);scene.entities.pop();assert.equal(validateScene(scene).valid,true);
});
test('character part materials count each entity once and clone individual or shared bindings',async t=>{
  const {file,base}=await fixture(t),scene=await attachModel({directory:base.directory,scenePath:file,expectedRevision:0}),entity=scene.entities.at(-1),source=entity.character.materials.m1;
  entity.character.materials.m2=source;
  assert.equal(listMaterials(scene).find(m=>m.name===source).entityCount,1);assert.equal(patchMaterial(scene,source,{roughness:.4}).affectedEntities,1);
  const one=materialInstance(scene,source,'role_part',entity.id,'m1').scene.entities.at(-1);
  assert.equal(one.character.materials.m1,'role_part');assert.equal(one.character.materials.m2,source);assert.equal(one.material,undefined);
  const both=materialInstance(scene,source,'role_all',entity.id).scene.entities.at(-1);assert.equal(both.character.materials.m1,'role_all');assert.equal(both.character.materials.m2,'role_all');
  assert.throws(()=>materialInstance(scene,source,'role_bad',entity.id,'missing'),/source slot/);
  assert.throws(()=>materialInstance(scene,source,'role_bad',scene.entities[0].id,'m1'),/character entity/);
  await applyMaterialPatch({scenePath:file,material:source,changes:{roughness:.17},expectedRevision:1,root:path.dirname(path.dirname(file))});
  const attached=await attachModel({directory:base.directory,scenePath:file,expectedRevision:2});assert.equal(attached.materials[source].roughness,.17);
  const role=attached.entities.at(-1),cli=JSON.parse(execFileSync(process.execPath,['tools/material.mjs','instance','--scene',file,'--from',role.character.materials.m2,'--name','cli_role_part','--entity',role.id,'--source-slot','m2','--revision',String(attached.revision),'--apply'],{windowsHide:true,encoding:'utf8'}));
  assert.equal(cli.sourceSlot,'m2');assert.equal((await loadScene(file)).entities.at(-1).character.materials.m2,'cli_role_part');
});
test('character packages include exact header/binary dependencies and guard hashes and cache contents',async t=>{
  const {file,base,action,root}=await fixture(t);
  await attachModel({directory:base.directory,scenePath:file,expectedRevision:0});
  const scene=await attachModel({directory:action.directory,scenePath:file,expectedRevision:1,characterOptions:{clip:'vendor_Raise'}});
  const output=path.join(root,'package'),built=await buildScene(file,output);
  for(const type of ['skinned-mesh','animation','cooked-texture'])assert.ok(built.manifest.includedAssetTypes.includes(type));
  for(const entity of scene.entities.filter(e=>e.character)) {
    for(const name of [entity.character.file,path.posix.join(path.posix.dirname(entity.character.file),'animation.bin')])
      assert.deepEqual(await fs.readFile(path.join(built.packageDirectory,name)),await fs.readFile(path.join(path.dirname(file),name)));
  }
  assert.equal((await buildScene(file,output)).cacheHit,true);
  const copy=structuredClone(scene),entity=copy.entities.at(-1);entity.character={...copy.entities.at(-2).character,sha256:'0'.repeat(64)};
  await assert.rejects(buildScene(copy,output,{sceneDirectory:path.dirname(file)}),/Conflicting asset hashes/);
  const packaged=path.join(built.packageDirectory,scene.entities.at(-1).character.file);await fs.appendFile(packaged,' ');await assert.rejects(buildScene(file,output),/Packaged dependency changed/);
  await fs.appendFile(path.join(action.directory,'character/animation.bin'),' ');await assert.rejects(buildScene(file,path.join(root,'new-package')),/Asset dependency changed/);
});
test('editor attaches characters and captures a frozen package with external textures',async t=>{
  const {root,file,base}=await fixture(t),executable=path.join(root,'fake.exe');await fs.writeFile(executable,'fake');await fs.writeFile(path.join(root,'VERSION'),'0.1.0');
  const calls=[],spawnProcess=(exe,args,options)=>{const child=new EventEmitter();child.pid=1;child.kill=()=>{child.emit('exit',0);return true;};calls.push({args,options});
    process.nextTick(async()=>{child.emit('spawn');if(args.includes('--screenshot')){await fs.writeFile(args[args.indexOf('--screenshot')+1],Buffer.from([137,80,78,71,13,10,26,10,0]));child.emit('exit',0);}});return child;};
  const service=createEditorService({root,executable,spawnProcess});t.after(()=>service.close());
  const attached=await service.call('asset_attach',{id:base.manifest.id,scenePath:'assets/main.scene.json',expectedRevision:0,characterOptions:{replacePlayer:true,clip:'idle'}});assert.equal(attached.ok,true,JSON.stringify(attached));
  const actor=attached.result.entities.find(e=>e.kind==='player'),source=actor.character.materials.m1;
  const cloned=await service.call('material_instance',{scenePath:'assets/main.scene.json',source,name:'custom_role_part',entityId:actor.id,sourceSlot:'m1',expectedRevision:1,apply:true});assert.equal(cloned.ok,true,JSON.stringify(cloned));
  assert.equal((await loadScene(file)).entities.find(e=>e.kind==='player').character.materials.m1,'custom_role_part');
  await service.call('runtime_start',{scenePath:'assets/main.scene.json'});assert.ok(calls[0].args.includes(path.join(path.dirname(file),'textures')));await service.call('runtime_stop');
  const capture=await service.call('runtime_capture',{scenePath:'assets/main.scene.json'});assert.equal(capture.ok,true,JSON.stringify(capture));
  const args=calls.at(-1).args,sceneFile=args[args.indexOf('--scene')+1],scene=await loadScene(sceneFile);assert.ok(sceneFile.includes('.assets'));
  assert.ok(await fs.stat(path.join(path.dirname(sceneFile),scene.entities.find(e=>e.character).character.file)));assert.ok(args.includes(path.join(path.dirname(sceneFile),'textures')));
});

import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { componentCatalog, gameplayCatalog, validateComponents, gameplayDependencies } from '../engine/scene/gameplay.mjs';
import { loadScene, validateScene, previewTransaction, buildScene } from '../engine/scene/index.mjs';
import { createToolService } from '../ai/tool-service.mjs';

test('component/behavior descriptors enforce independent typed instances and custom properties', () => {
  const catalog=gameplayCatalog();
  assert.ok(catalog.components.transform);
  assert.ok(catalog.behaviors.some(b=>b.script==='gameplay/locked_door'));
  const components={behavior:{instances:[{id:'lock',script:'gameplay/locked_door',properties:{requiredItem:'silver'}}]}};
  const normalized=validateComponents(components);
  assert.equal(normalized.behavior.instances[0].properties.requiredItem,'silver');
  assert.throws(()=>validateComponents({missing:{}}),/Unknown component/);
  assert.throws(()=>validateComponents({collider:{enabled:7}}),/boolean/);
  assert.throws(()=>validateComponents({behavior:{instances:[...components.behavior.instances,...components.behavior.instances]}}),/duplicate/);
  assert.throws(()=>validateComponents({behavior:{instances:[{id:'a',script:'../escape'}]}}),/path/);
  assert.throws(()=>validateComponents({behavior:{instances:[{id:'a',script:'gameplay/locked_door',properties:{consumeItem:'yes'}}]}}),/boolean/);
});

test('scene component changes use the same atomic preview/revision contract',async()=>{
  const scene=await loadScene('.screen/courtyard.scene.json'),entity=scene.entities.find(e=>e.kind==='door');
  const transaction={projectId:scene.projectId,expectedRevision:scene.revision,commands:[{op:'set',entityId:entity.id,changes:{components:{behavior:{instances:[{id:'silver-lock',script:'gameplay/locked_door',properties:{requiredItem:'silver'}}]}}}}]};
  const preview=previewTransaction(scene,transaction);
  assert.equal(preview.scene.entities.find(e=>e.id===entity.id).components.behavior.instances[0].id,'silver-lock');
  assert.equal(entity.components,undefined);
  transaction.commands.push({op:'set',entityId:entity.id,changes:{components:{collider:{enabled:'bad'}}}});
  assert.throws(()=>previewTransaction(scene,transaction));
  assert.equal(entity.components,undefined);
  assert.equal(validateScene(scene).valid,true);
});

test('content packages lock and verify Lua/config dependencies as well as scene data',async t=>{
  const directory=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-gameplay-package-'));
  t.after(()=>fs.rm(directory,{recursive:true,force:true}));
  const assets=path.join(directory,'assets');await fs.mkdir(assets);
  await fs.cp('assets/gameplay',path.join(assets,'gameplay'),{recursive:true});
  await fs.cp('assets/scripts',path.join(assets,'scripts'),{recursive:true});
  const scene=await loadScene('.screen/courtyard.scene.json');
  const first=await buildScene(scene,path.join(directory,'out'),{assetsDirectory:assets});
  assert.ok(first.manifest.files.some(f=>f.path==='scripts/gameplay/locked_door.lua'));
  assert.equal((await buildScene(scene,path.join(directory,'out'),{assetsDirectory:assets})).cacheHit,true);
  await fs.appendFile(path.join(assets,'scripts/gameplay/locked_door.lua'),'\n-- changed behavior version\n');
  const changed=await buildScene(scene,path.join(directory,'out'),{assetsDirectory:assets});
  assert.notEqual(first.manifest.buildKey,changed.manifest.buildKey);
  await fs.appendFile(path.join(changed.packageDirectory,'scripts/gameplay/locked_door.lua'),'tampered');
  await assert.rejects(buildScene(scene,path.join(directory,'out'),{assetsDirectory:assets}),{code:'BUILD_CONFLICT'});
  assert.equal(gameplayDependencies(scene,assets).filter(f=>f.path.endsWith('.lua')).length,3);
});

test('AI reads the component catalog and bounded runtime diagnostics',async t=>{
  const service=createToolService();
  const catalog=await service.callTool('gameplay_catalog',{});assert.equal(catalog.ok,true);
  assert.deepEqual(catalog.result.components,componentCatalog());
  const validation=await service.callTool('gameplay_validate',{scenePath:'.screen/courtyard.scene.json'});
  assert.equal(validation.result.valid,true);assert.equal(validation.result.luaExecuted,false);
  const escape=await service.callTool('gameplay_inspect',{reportPath:'../outside.json'});assert.equal(escape.ok,false);
});

test('declared behavior dependencies are transitive and cycles are rejected',async t=>{
  const directory=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-gameplay-dependencies-'));
  t.after(()=>fs.rm(directory,{recursive:true,force:true}));
  await fs.cp('assets/gameplay',path.join(directory,'gameplay'),{recursive:true});
  await fs.cp('assets/scripts',path.join(directory,'scripts'),{recursive:true});
  const file=path.join(directory,'scripts/gameplay/pickup.behavior.json');
  const descriptor=JSON.parse(await fs.readFile(file,'utf8'));descriptor.dependencies=['gameplay/locked_door'];
  await fs.writeFile(file,JSON.stringify(descriptor));
  const scene={entities:[{kind:'key'}]};
  assert.equal(gameplayDependencies(scene,directory).filter(f=>f.path.endsWith('.lua')).length,2);
  const doorFile=path.join(directory,'scripts/gameplay/locked_door.behavior.json');
  const door=JSON.parse(await fs.readFile(doorFile,'utf8'));door.dependencies=['gameplay/pickup'];await fs.writeFile(doorFile,JSON.stringify(door));
  assert.throws(()=>gameplayDependencies(scene,directory),/cycle/);
});

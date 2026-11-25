import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {writeIorFixture} from '../tools/offline/ior-fixture.mjs';
import {importAsset,verifyImport,appendAnimation,attachModel} from '../engine/assets/import-service.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {dielectricF0,MAX_IOR} from '../engine/material/ior.mjs';
import {compileMir,materialToMir} from '../engine/material/mir.mjs';
import {compileGraph} from '../engine/material/graph.mjs';
import {patchMaterial,materialInstance} from '../tools/material.mjs';
import {loadScene,validateScene,buildScene,undoSceneFile} from '../engine/scene/index.mjs';
import {createEditorService} from '../editor/service.mjs';
import {createToolService} from '../ai/tool-service.mjs';
import {createMcpHandler} from '../ai/mcp-server.mjs';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-ior-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('IOR source extensions preserve zero, air, water, default, diamond and finite Float32 maximum on models and characters',()=>fixture(async dir=>{
  for(const character of [false,true])for(const ior of [0,1,1.33,1.5,2.42,MAX_IOR]){const asset=await importAsset({source:await writeIorFixture(path.join(dir,`${character}-${ior}`),{character,ior}),output:path.join(dir,'imports'),size:64});assert.equal(asset.manifest.materials.m1.ior,ior);assert.equal((await loadScene(path.join(asset.directory,'preview.scene.json'))).materials.m1.ior,ior);await verifyImport(asset.directory);}
  assert.equal(dielectricF0(0),1);assert.equal(dielectricF0(1),0);assert.ok(Math.abs(dielectricF0(1.5)-.04)<1e-12);assert.ok(dielectricF0(MAX_IOR)<=1);
}));
test('default IOR is optional and invalid source types, gaps, range and extension members reject atomically',()=>fixture(async dir=>{
  const source=await writeIorFixture(path.join(dir,'source')),original=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports');
  for(const value of [null,[],0,'1.5',{ior:null},{ior:-1},{ior:.5},{ior:.999999999999},{ior:'1.5'},{ior:1e39},{ior:2,unsupported:true}]){const doc=structuredClone(original);doc.materials[0].extensions.KHR_materials_ior=value;await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}),/ior|IOR/);assert.deepEqual(await fs.readdir(output),[]);}
  const doc=structuredClone(original);doc.materials[0].extensions.KHR_materials_ior={};await fs.writeFile(source,JSON.stringify(doc));assert.equal((await importAsset({source,output,size:64})).manifest.materials.m1.ior,1.5);delete doc.materials[0].extensions.KHR_materials_ior;await fs.writeFile(source,JSON.stringify(doc));assert.equal((await importAsset({source,output,size:64})).manifest.materials.m1.ior,undefined);
}));
test('IOR changes immutable material identity while appended character motions keep material and geometry bytes',()=>fixture(async dir=>{
  const source=await writeIorFixture(path.join(dir,'source'),{character:true,ior:2.42}),asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),motion=await writeMotionFixture(path.join(dir,'motion')),next=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'imports')});assert.deepEqual(next.manifest.materials,asset.manifest.materials);const old=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),newHeader=JSON.parse(await fs.readFile(path.join(next.directory,next.manifest.character)));assert.deepEqual(old.mesh,newHeader.mesh);const a=await fs.readFile(path.join(asset.directory,'character/animation.bin')),b=await fs.readFile(path.join(next.directory,'character/animation.bin'));assert.deepEqual(a.subarray(0,old.clips[0].offset),b.subarray(0,old.clips[0].offset));
  const doc=JSON.parse(await fs.readFile(source));for(const mat of doc.materials)mat.extensions.KHR_materials_ior.ior=1;await fs.writeFile(source,JSON.stringify(doc));const changed=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.notEqual(changed.manifest.id,asset.manifest.id);
}));
test('IOR survives MIR, graph, material instances and editor patch undo, scene attachment and source-independent packages',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeIorFixture(path.join(dir,'source'),{ior:2.42}),output:path.join(dir,'imports'),size:64}),file=path.join(dir,'assets/main.scene.json');await fs.mkdir(path.dirname(file),{recursive:true});const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));scene.entities=scene.entities.filter(e=>!e.mesh);for(const entity of scene.entities)delete entity.material;delete scene.materials;await fs.writeFile(file,JSON.stringify(scene));const attached=await attachModel({directory:asset.directory,scenePath:file,expectedRevision:0}),name=asset.manifest.id.slice(0,12)+'_m1';assert.equal(attached.materials[name].ior,2.42);
  const mir=compileMir(materialToMir(name,attached.materials[name]));assert.equal(mir.material.ior,2.42);assert.ok(mir.features.includes('dielectric-ior'));assert.equal(materialInstance(attached,name,'copy').scene.materials.copy.ior,2.42);const graph=value=>({schema:'nativegpu.material-graph/1',name:'ior',nodes:[{id:'s',type:'scalar',params:{value}},{id:'out',type:'output'}],links:[{from:'s',to:'out',toPort:'ior'}]});assert.equal(compileGraph(graph(0)).compiled.material.ior,0);
  for(const value of [null,-1,.5,Infinity,1e39,'2']){assert.throws(()=>patchMaterial(attached,name,{ior:value}));assert.throws(()=>compileMir(materialToMir(name,{albedo:'flat',ior:value})));assert.throws(()=>compileGraph(graph(value)));const bad=structuredClone(attached);bad.materials[name].ior=value;assert.equal(validateScene(bad).valid,false);}
  const handle=createMcpHandler({service:createToolService({root:dir})});for(const value of [0,.5]){const response=await handle({jsonrpc:'2.0',id:1,method:'tools/call',params:{name:'material_patch',arguments:{scenePath:'assets/main.scene.json',material:name,changes:{ior:value},expectedRevision:1}}});assert.equal(response.result.isError,value===.5,JSON.stringify(response));}
  const service=createEditorService({root:dir});try{const result=await service.call('material_patch',{scenePath:'assets/main.scene.json',material:name,changes:{ior:0},expectedRevision:1,apply:true});assert.equal(result.ok,true,JSON.stringify(result));}finally{await service.close();}assert.equal((await loadScene(file)).materials[name].ior,0);await undoSceneFile(file,{projectId:attached.projectId,expectedRevision:2});assert.equal((await loadScene(file)).materials[name].ior,2.42);const built=await buildScene(file,path.join(dir,'packages'));assert.equal(JSON.parse(await fs.readFile(path.join(built.packageDirectory,'scene.json'))).materials[name].ior,2.42);
}));

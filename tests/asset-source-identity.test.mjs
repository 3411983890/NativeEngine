import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {sourceIdentities,sourceMaterialIdentities} from '../engine/assets/source-identity.mjs';
import {writeAssetFixture,writeCharacterFixture,writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {importAsset,verifyImport,appendAnimation} from '../engine/assets/import-service.mjs';
import {readGltf} from '../engine/assets/gltf-document.mjs';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-source-id-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('source part keys remain stable across named-node reordering and geometry edits while immutable packages change',()=>fixture(async dir=>{
  const source=await writeAssetFixture(path.join(dir,'source')),original=JSON.parse(await fs.readFile(source));original.nodes[0].name='Door';original.nodes[1].name='Window';await fs.writeFile(source,JSON.stringify(original));const first=await importAsset({source,output:path.join(dir,'imports'),size:64});
  const doc=structuredClone(original);doc.nodes.reverse();for(const scene of doc.scenes)scene.nodes=scene.nodes.map(i=>doc.nodes.length-1-i);doc.nodes[1].translation=[8,2,0];await fs.writeFile(source,JSON.stringify(doc));const second=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.notEqual(second.manifest.id,first.manifest.id);for(const part of first.manifest.parts){const next=second.manifest.parts.find(p=>p.name===part.name);assert.equal(next.source.key,part.source.key);assert.equal(next.source.stable,true);}await verifyImport(first.directory);await verifyImport(second.directory);
}));
test('source material names and author ids survive material-array reorder and preserve default material identity',()=>{
  const first=sourceMaterialIdentities([{name:'Wood'},{name:'Steel'}]),next=sourceMaterialIdentities([{name:'Steel'},{name:'Wood'}]);assert.deepEqual(first.m0,next.m0);assert.equal(first.m1.key,next.m2.key);assert.equal(first.m2.key,next.m1.key);assert.ok(first.m1.stable);const explicit=sourceMaterialIdentities([{name:'Old',extras:{nativegpu:{sourceId:'surface-wood'}}}]),renamed=sourceMaterialIdentities([{name:'New',extras:{nativegpu:{sourceId:'surface-wood'}}}]);assert.equal(explicit.m1.key,renamed.m1.key);assert.equal(sourceMaterialIdentities([{name:'same'},{name:'same'}]).m1.stable,false);
});
test('ambiguous sibling names and unnamed nodes remain importable but cannot claim stable automatic correspondence',()=>{
  const doc={scenes:[{nodes:[0,1,2]}],nodes:[{name:'same'},{name:'same'},{}]},ids=sourceIdentities(doc);const records=[0,1,2].map(i=>ids.part(i,{},0));assert.ok(records.every(r=>!r.stable));assert.equal(new Set(records.map(r=>r.key)).size,3);
  const explicit={scenes:[{nodes:[0]}],nodes:[{name:'Old parent',children:[1]},{name:'Old mesh',extras:{nativegpu:{sourceId:'stable-door'}}}]},first=sourceIdentities(explicit).part(1,{},0);explicit.nodes[0].name='New parent';explicit.nodes[1].name='New mesh';assert.equal(sourceIdentities(explicit).part(1,{},0).key,first.key);assert.equal(first.stable,true);
  const primitive={extras:{nativegpu:{sourceId:'body'}}};assert.equal(sourceIdentities(explicit).part(1,primitive,0).key,sourceIdentities(explicit).part(1,primitive,4).key);
});
test('source provenance survives character compilation and separate animation append without modifying geometry bytes',()=>fixture(async dir=>{
  const source=await writeCharacterFixture(path.join(dir,'source')),asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),input=await readGltf(source),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character)));assert.ok(asset.manifest.parts.every(p=>/^[a-f0-9]{64}$/.test(p.source.key)));assert.deepEqual(h.mesh.parts.map(p=>p.source),asset.manifest.parts.map(p=>p.source));assert.equal(Object.keys(asset.manifest.materialSources).length,input.document.materials.length+1);
  const motion=await writeMotionFixture(path.join(dir,'motion')),next=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'imports')}),nh=JSON.parse(await fs.readFile(path.join(next.directory,next.manifest.character)));assert.deepEqual(nh.mesh,h.mesh);assert.deepEqual(next.manifest.materialSources,asset.manifest.materialSources);await verifyImport(next.directory);
}));
test('invalid and duplicate authored source ids reject atomically while unused scene nodes do not affect identity',()=>fixture(async dir=>{
  const source=await writeAssetFixture(path.join(dir,'source')),original=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports');for(const value of ['',4,'bad\nkey']){const doc=structuredClone(original);doc.nodes[0].extras={nativegpu:{sourceId:value}};await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}),/sourceId/);assert.deepEqual(await fs.readdir(output),[]);}const dup=structuredClone(original);for(const n of dup.nodes)n.extras={nativegpu:{sourceId:'duplicate'}};await fs.writeFile(source,JSON.stringify(dup));await assert.rejects(importAsset({source,output,size:64}),/Duplicate node sourceId/);assert.deepEqual(await fs.readdir(output),[]);
  const normal=sourceIdentities(original).part(0,{},0);original.nodes.push({extras:{nativegpu:{sourceId:4}}});assert.equal(sourceIdentities(original).part(0,{},0).key,normal.key);
}));
test('duplicate explicit primitive and material identities reject before a package becomes visible',()=>fixture(async dir=>{
  const source=await writeAssetFixture(path.join(dir,'source')),original=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports');for(const type of ['primitive','material']){const doc=structuredClone(original);if(type==='primitive'){const first=doc.meshes[0].primitives[0];first.extras={nativegpu:{sourceId:'surface'}};doc.meshes[0].primitives.push(structuredClone(first));}else for(const material of doc.materials)material.extras={nativegpu:{sourceId:'surface'}};await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:32}),/Duplicate .*sourceId/);assert.deepEqual(await fs.readdir(output),[]);}
}));

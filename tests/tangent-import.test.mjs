import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {importAsset,attachModel,appendAnimation,verifyImport,fitSceneCharacter} from '../engine/assets/import-service.mjs';
import {readStaticModel} from '../engine/assets/model-import.mjs';
import {readGltf,readAccessor} from '../engine/assets/gltf-document.mjs';
import {loadScene,buildScene} from '../engine/scene/index.mjs';
import {writeTangentFixture} from '../tools/offline/tangent-fixture.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {MeshoptEncoder} from 'meshoptimizer/encoder';
const dot=(a,b)=>a.reduce((sum,v,k)=>sum+v*b[k],0);
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-tangent-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}

test('static authored tangents retain handedness, source axes, reflected/nonuniform transforms and double-sided frames',()=>fixture(async dir=>{
  for(const componentType of [5126,5120,5122])for(const scale of [[2,3,4],[-2,3,4]]){
    const source=await writeTangentFixture(path.join(dir,componentType+'-'+scale[0]),{angle:Math.PI/4,sign:-1,componentType,scale,doubleSided:true}),model=await readStaticModel(source),asset=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);
    for(const part of asset.manifest.parts){const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,part.mesh)));assert.equal(mesh.schema,'nativegpu.mesh/4');const count=mesh.vertices.length/2;for(let i=0;i<count;i++){const a=mesh.vertices[i],b=mesh.vertices[i+count];assert.equal(a.length,24);assert.ok(Math.abs(dot(a.slice(3,6),a.slice(20,23)))<.00001);assert.equal(a[23],Math.sign(scale[0]));assert.equal(b[23],-a[23]);assert.deepEqual(b.slice(20,23),a.slice(20,23));assert.ok(Math.abs(Math.hypot(...a.slice(20,23))-1)<.00001);}}
    const converted=await readStaticModel(source,{coordinates:{upAxis:'+Z',forwardAxis:'+Y',handedness:'left'}});for(let i=0;i<model.primitives.length;i++)assert.equal(converted.primitives[i].tangents[0][3],-model.primitives[i].tangents[0][3]);
  }
}));

test('skinned tangents use an optional sidecar retained with UVs, colors, fitting, motion append and packages',()=>fixture(async dir=>{
  const source=await writeTangentFixture(path.join(dir,'source'),{character:true,componentType:5122,doubleSided:true,angle:Math.PI/3}),asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),header=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),bytes=await fs.readFile(path.join(asset.directory,'character/animation.bin'));
  assert.equal(header.mesh.tangentOffset,header.mesh.colorOffset+header.mesh.colorBytes);assert.equal(header.mesh.tangentBytes,header.mesh.vertices*16);assert.equal(header.mesh.indexOffset,header.mesh.tangentOffset+header.mesh.tangentBytes);const tangents=bytes.subarray(header.mesh.tangentOffset,header.mesh.indexOffset);assert.ok(tangents.some(v=>v!==0));
  const motion=await writeMotionFixture(path.join(dir,'motion')),appended=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'motions'),options:{}}),next=JSON.parse(await fs.readFile(path.join(appended.directory,'character/character.json'))),nextBytes=await fs.readFile(path.join(appended.directory,'character/animation.bin'));assert.deepEqual(nextBytes.subarray(next.mesh.tangentOffset,next.mesh.indexOffset),tangents);
  const view=path.join(dir,'view');await fs.cp(asset.directory,view,{recursive:true});const scenePath=path.join(view,'main.scene.json'),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));scene.entities=scene.entities.filter(e=>!e.animation);await fs.writeFile(scenePath,JSON.stringify(scene));await attachModel({directory:asset.directory,scenePath,expectedRevision:0,characterOptions:{replacePlayer:true}});await fitSceneCharacter({scenePath,entityId:scene.entities.find(e=>e.kind==='player').id,expectedRevision:1,mode:'humanoid'});const built=await buildScene(scenePath,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith('/animation.bin')));
}));

test('absent tangents retain the old representation and tangents without authored normals are ignored',()=>fixture(async dir=>{
  for(const character of [false,true]){const source=await writeTangentFixture(path.join(dir,String(character)),{character}),doc=JSON.parse(await fs.readFile(source));for(const mesh of doc.meshes)for(const primitive of mesh.primitives)delete primitive.attributes.NORMAL;for(const material of doc.materials)delete material.normalTexture;await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});if(character){const header=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character)));assert.equal(header.mesh.tangentOffset,undefined);}else for(const part of asset.manifest.parts)assert.notEqual(JSON.parse(await fs.readFile(path.join(asset.directory,part.mesh))).schema,'nativegpu.mesh/4');assert.ok(asset.manifest.warnings.some(w=>w.includes('without authored normals')));}
}));

test('bad tangent types, counts, lengths and signs fail before publication',()=>fixture(async dir=>{
  for(const character of [false,true]){const source=await writeTangentFixture(path.join(dir,String(character)),{character}),original=JSON.parse(await fs.readFile(source)),primitive=original.meshes[0].primitives[0],index=primitive.attributes.TANGENT;for(const edit of [d=>d.accessors[index].type='VEC3',d=>d.accessors[index].count--,d=>{const a=d.accessors[index],b=d.bufferViews[a.bufferView],raw=Buffer.from(d.buffers[b.buffer].uri.split(',')[1],'base64');raw.writeFloatLE(0,12);d.buffers[b.buffer].uri='data:application/octet-stream;base64,'+raw.toString('base64');}]){const bad=structuredClone(original);edit(bad);await fs.writeFile(source,JSON.stringify(bad));await assert.rejects(importAsset({source,output:path.join(dir,'imports'),size:64}));}}
}));

test('meshopt compressed signed tangents and sparse overrides retain source frames in both import paths',()=>fixture(async dir=>{
  await MeshoptEncoder.ready;for(const character of [false,true])for(const sparse of [false,true]){
    const source=await writeTangentFixture(path.join(dir,character+'-'+sparse),{character,componentType:5122}),doc=JSON.parse(await fs.readFile(source)),spec=doc.accessors[doc.meshes[0].primitives[0].attributes.TANGENT],view=doc.bufferViews[spec.bufferView],input=Buffer.from(doc.buffers[view.buffer].uri.split(',')[1],'base64'),buffer=doc.buffers.length,newView=doc.bufferViews.length;
    if(sparse){const bytes=Buffer.alloc(12);bytes[0]=0;bytes.writeInt16LE(32767,6);bytes.writeInt16LE(32767,10);doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:1},{buffer,byteOffset:4,byteLength:8});spec.sparse={count:1,indices:{bufferView:newView,componentType:5121},values:{bufferView:newView+1}};}
    else{const compressed=Buffer.from(MeshoptEncoder.encodeGltfBuffer(input,spec.count,8,'ATTRIBUTES'));doc.buffers.push({byteLength:compressed.length,uri:'data:application/octet-stream;base64,'+compressed.toString('base64')},{byteLength:input.length,extensions:{EXT_meshopt_compression:{fallback:true}}});doc.bufferViews.push({buffer:buffer+1,byteLength:input.length,byteStride:8,extensions:{EXT_meshopt_compression:{buffer,byteLength:compressed.length,byteStride:8,count:spec.count,mode:'ATTRIBUTES'}}});spec.bufferView=newView;doc.extensionsRequired.push('EXT_meshopt_compression');}
    await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});let tangent;if(character){const h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));tangent=Array.from({length:4},(_,i)=>b.readFloatLE(h.mesh.tangentOffset+i*4));}else{const m=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));tangent=m.vertices[0].slice(20);}assert.deepEqual(tangent.map(v=>v||0),sparse?[0,1,0,-1]:[1,0,0,-1]);
  }
}));

test('mixed character primitives preserve authored frames and use an explicit zero fallback for primitives without tangents',()=>fixture(async dir=>{
  const source=await writeTangentFixture(path.join(dir,'source'),{character:true}),doc=JSON.parse(await fs.readFile(source));const primitive=structuredClone(doc.meshes[0].primitives[0]);delete primitive.attributes.TANGENT;const material=structuredClone(doc.materials[primitive.material]);delete material.normalTexture;primitive.material=doc.materials.length;doc.materials.push(material);doc.meshes[0].primitives.push(primitive);await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));let authored=0,fallback=0;for(let i=0;i<h.mesh.vertices;i++){const row=Array.from({length:4},(_,k)=>b.readFloatLE(h.mesh.tangentOffset+i*16+k*4));if(row[3]===0){assert.deepEqual(row,[0,0,0,0]);fallback++;}else authored++;}assert.ok(authored>0&&fallback>0);
}));

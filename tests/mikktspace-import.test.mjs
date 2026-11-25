import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {writeMikkFixture} from '../tools/offline/mikktspace-fixture.mjs';
import {readGltf,readAccessor} from '../engine/assets/gltf-document.mjs';
import {generateMissingTangents} from '../engine/assets/tangent-generation.mjs';
import {importAsset,appendAnimation,verifyImport} from '../engine/assets/import-service.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {fitCharacterBounds} from '../engine/assets/character-bounds.mjs';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-mikk-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('bundled robot tolerates collapsed tangent corners and produces finite orthogonal frames',async()=>{
  const original=await readGltf(path.resolve(import.meta.dirname,'../assets/source/builtin/robot.glb'));
  const generated=await generateMissingTangents(original),primitive=generated.document.meshes[0].primitives[0];
  const normals=readAccessor(generated,primitive.attributes.NORMAL,'VEC3'),tangents=readAccessor(generated,primitive.attributes.TANGENT,'VEC4');
  assert.ok(tangents.length>20000);
  for(const [i,tangent]of tangents.entries()){
    assert.ok(tangent.every(Number.isFinite));
    assert.ok(Math.abs(Math.hypot(...tangent.slice(0,3))-1)<.02);
    assert.ok(Math.abs(tangent.slice(0,3).reduce((sum,v,k)=>sum+v*normals[i][k],0))<.02);
  }
});
test('MikkTSpace uses the normal texture UV set, splits handedness seams and retains colors and original geometry',()=>fixture(async dir=>{
  const source=await writeMikkFixture(path.join(dir,'source')),original=await readGltf(source),generated=await generateMissingTangents(original),p=generated.document.meshes[0].primitives[0],positions=readAccessor(generated,p.attributes.POSITION,'VEC3'),t=readAccessor(generated,p.attributes.TANGENT,'VEC4'),colors=readAccessor(generated,p.attributes.COLOR_0,'VEC4');assert.equal(positions.length,6);assert.deepEqual([...new Set(t.map(v=>v[3]))].sort(),[-1,1]);assert.deepEqual(positions[0],positions[3]);assert.deepEqual(colors[0],colors[3]);assert.equal(original.document.meshes[0].primitives[0].attributes.TANGENT,undefined);const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),again=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.equal(asset.manifest.id,again.manifest.id);await verifyImport(asset.directory);const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.equal(mesh.schema,'nativegpu.mesh/4');assert.equal(mesh.vertices.length,12);assert.ok(asset.manifest.warnings.some(w=>w.includes('MikkTSpace')));
}));
test('normal-map generation creates flat normals when absent and ignores supplied tangents with no authored normals',()=>fixture(async dir=>{
  const results=[];for(const authored of [false,true]){const source=await writeMikkFixture(path.join(dir,String(authored)),{normals:false,nonplanar:true,authored}),asset=await importAsset({source,output:path.join(dir,'imports'),size:64});results.push(asset);const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.equal(mesh.vertices.length,12);assert.notDeepEqual(mesh.vertices[0].slice(3,6),mesh.vertices[3].slice(3,6));if(authored)assert.ok(asset.manifest.warnings.some(w=>w.includes('without authored normals')));}assert.equal(results[0].manifest.parts[0].sha256,results[1].manifest.parts[0].sha256);
}));
test('generated shape tangents preserve source position deltas, seam duplication, 40-byte records and fitting',()=>fixture(async dir=>{
  const source=await writeMikkFixture(path.join(dir,'source'),{morph:true}),asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(h.mesh.vertices,12);assert.ok(h.mesh.tangentBytes>0);assert.equal(h.morphs[0].stride,40);let moved=false,tangent=false;for(let i=0;i<h.morphs[0].count;i++){const at=h.morphs[0].offset+i*40;moved ||= Math.abs(b.readFloatLE(at+4))>.2;tangent ||= [0,1,2].some(k=>Math.abs(b.readFloatLE(at+28+k*4))>.01);}assert.ok(moved&&tangent);assert.ok(fitCharacterBounds(h,b).height>0);const motion=await writeMotionFixture(path.join(dir,'motion')),motionDoc=JSON.parse(await fs.readFile(motion.source)),next=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'motions'),options:{boneMap:{[h.bones[0].name]:motionDoc.nodes[0].name}}});await verifyImport(next.directory);const nh=JSON.parse(await fs.readFile(path.join(next.directory,next.manifest.character))),nb=await fs.readFile(path.join(next.directory,'character/animation.bin'));assert.deepEqual(nh.mesh,h.mesh);assert.deepEqual(nb.subarray(0,h.clips[0].offset),b.subarray(0,h.clips[0].offset));
}));
test('authored frames and unlit fallback materials skip generation, while unselected meshes cannot fail the selected scene',()=>fixture(async dir=>{
  for(const options of [{authored:true},{unlit:true}]){const source=await writeMikkFixture(path.join(dir,Object.keys(options).join('-')),options),original=await readGltf(source);assert.equal(await generateMissingTangents(original),original);}
  const source=await writeMikkFixture(path.join(dir,'unused')),doc=JSON.parse(await fs.readFile(source));doc.meshes.push({primitives:[{attributes:{POSITION:99999},material:0}]});await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);
}));
test('triangle strips and fans keep triangle topology during tangent seam splitting; missing UVs fail atomically',()=>fixture(async dir=>{
  for(const mode of [5,6]){const source=await writeMikkFixture(path.join(dir,String(mode)),{mode}),generated=await generateMissingTangents(await readGltf(source)),p=generated.document.meshes[0].primitives[0];assert.equal(p.mode,4);assert.equal(readAccessor(generated,p.indices,'SCALAR').length,6);}
  const source=await writeMikkFixture(path.join(dir,'invalid')),doc=JSON.parse(await fs.readFile(source));delete doc.meshes[0].primitives[0].attributes.TEXCOORD_3;await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output:path.join(dir,'imports'),size:64}),/missing TEXCOORD_3/);assert.equal(await fs.stat(path.join(dir,'imports')).then(()=>true).catch(()=>false),true);assert.equal((await fs.readdir(path.join(dir,'imports'))).filter(name=>/^[a-f0-9]{64}$/.test(name)).length,0);
}));

test('soft-region source vertex selections follow every generated seam copy and double-sided vertex',()=>fixture(async dir=>{
  const source=await writeMikkFixture(path.join(dir,'source'),{morph:true}),doc=JSON.parse(await fs.readFile(source));doc.extras.nativegpu.softRegions=[{bone:'Mikk panel',center:[0,1,0],radius:.2,maxOffset:.01,vertices:[0,2]}];await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(h.softRegions.length,1);assert.equal(h.softRegions[0].vertices.length,8);for(const v of h.softRegions[0].vertices){const p=[0,1,2].map(k=>b.readFloatLE(v*64+k*4));assert.ok((p[0]===-1&&p[1]===0)||(p[0]===1&&p[1]===2));}
}));

test('meshes without normal textures still receive flat normals, preserving integer color/UV storage and normal morphs',()=>fixture(async dir=>{
  for(const morph of [false,true]){const source=await writeMikkFixture(path.join(dir,String(morph)),{morph,normals:false,nonplanar:true}),doc=JSON.parse(await fs.readFile(source));delete doc.materials[0].normalTexture;await fs.writeFile(source,JSON.stringify(doc));const generated=await generateMissingTangents(await readGltf(source)),p=generated.document.meshes[0].primitives[0],n=readAccessor(generated,p.attributes.NORMAL,'VEC3');assert.equal(n.length,6);assert.notDeepEqual(n[0],n[3]);assert.equal(p.attributes.TANGENT,undefined);const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});if(morph){const h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character)));assert.equal(h.mesh.tangentOffset,undefined);assert.equal(h.morphs[0].stride,undefined);assert.ok(h.morphs[0].count>0);}else assert.equal(JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh))).vertices[0].length,20);}
}));

test('soft-region selections of unused source vertices reject before seam remapping can erase them',()=>fixture(async dir=>{
  const source=await writeMikkFixture(path.join(dir,'source'),{morph:true}),doc=JSON.parse(await fs.readFile(source)),p=doc.meshes[0].primitives[0];doc.accessors[p.indices].count=3;doc.extras.nativegpu.softRegions=[{bone:'Mikk panel',center:[0,1,0],radius:.2,maxOffset:.01,vertices:[3]}];await fs.writeFile(source,JSON.stringify(doc));const output=path.join(dir,'imports');await assert.rejects(importAsset({source,output,size:64}),/unused source vertex/);assert.deepEqual(await fs.readdir(output),[]);
}));

test('seam generation preserves normalized integer UV and color accessor types, values and invalid flag rejection',()=>fixture(async dir=>{
  const source=await writeMikkFixture(path.join(dir,'source')),input=await readGltf(source),doc=structuredClone(input.document),p=doc.meshes[0].primitives[0];
  for(const [semantic,componentType,max,scale]of [['TEXCOORD_3',5123,65535,.5],['COLOR_0',5121,255,1]]){const index=p.attributes[semantic],spec=doc.accessors[index],rows=readAccessor(input,index),size=componentType===5123?2:1,bytes=Buffer.alloc(rows.length*rows[0].length*size);rows.flat().forEach((v,i)=>bytes[componentType===5123?'writeUInt16LE':'writeUInt8'](Math.round(v*scale*max),i*size));const buffer=doc.buffers.length,view=doc.bufferViews.length;doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});Object.assign(spec,{bufferView:view,componentType,normalized:true});}
  await fs.writeFile(source,JSON.stringify(doc));const original=await readGltf(source),generated=await generateMissingTangents(original),out=generated.document.meshes[0].primitives[0],remap=generated.tangentVertexRemaps['0:0'].vertices;assert.equal(remap.length,6);
  for(const semantic of ['TEXCOORD_3','COLOR_0']){const before=original.document.accessors[p.attributes[semantic]],after=generated.document.accessors[out.attributes[semantic]];assert.equal(after.componentType,before.componentType);assert.equal(after.normalized,true);assert.deepEqual(readAccessor(generated,out.attributes[semantic]),remap.map(i=>readAccessor(original,p.attributes[semantic])[i]));}
  const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh))),uv=readAccessor(original,p.attributes.TEXCOORD_3),colors=readAccessor(original,p.attributes.COLOR_0);for(let i=0;i<remap.length;i++){assert.deepEqual(mesh.vertices[i].slice(8,10),uv[remap[i]]);assert.deepEqual(mesh.vertices[i].slice(16,20),colors[remap[i]]);}
  doc.accessors[p.attributes.TEXCOORD_3].normalized=0;await fs.writeFile(source,JSON.stringify(doc));const output=path.join(dir,'invalid');await assert.rejects(importAsset({source,output,size:64}),/normalized must be boolean/);assert.deepEqual(await fs.readdir(output),[]);
}));

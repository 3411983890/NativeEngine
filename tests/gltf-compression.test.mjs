import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import draco from 'draco3dgltf';
import {MeshoptEncoder} from 'meshoptimizer/encoder';
import {readGltf,readAccessor} from '../engine/assets/gltf-document.mjs';
import {importAsset,verifyImport} from '../engine/assets/import-service.mjs';
import {makeAnimationFixture} from '../tools/offline/animation-fixture.mjs';
import {importAnimation} from '../tools/offline/animation.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-compression-'));try{await fn(directory);}finally{await fs.rm(directory,{recursive:true,force:true});}}
async function save(directory,doc){const file=path.join(directory,'compressed.gltf');await fs.writeFile(file,JSON.stringify(doc));return file;}
function document(bytes,views,accessors,primitive,name) {
  return {asset:{version:'2.0'},scene:0,scenes:[{nodes:[0]}],nodes:[{mesh:0}],meshes:[{primitives:[primitive]}],buffers:[{byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')}],bufferViews:views,accessors,extensionsUsed:[name],extensionsRequired:[name]};
}
async function meshoptFixture(name='EXT_meshopt_compression') {
  await MeshoptEncoder.ready;
  const vertices=Buffer.alloc(36);[0,0,0,1,0,0,0,1,0].forEach((v,i)=>vertices.writeFloatLE(v,i*4));
  const indices=Buffer.from([0,0,1,0,2,0]);
  const a=Buffer.from(MeshoptEncoder.encodeGltfBuffer(vertices,3,12,'ATTRIBUTES',name.startsWith('EXT')?0:1)),b=Buffer.from(MeshoptEncoder.encodeGltfBuffer(indices,3,2,'TRIANGLES'));
  const doc=document(Buffer.concat([a,b]),[{buffer:1,byteLength:36,byteStride:12,extensions:{[name]:{buffer:0,byteOffset:0,byteLength:a.length,byteStride:12,count:3,mode:'ATTRIBUTES'}}},
    {buffer:1,byteOffset:36,byteLength:6,extensions:{[name]:{buffer:0,byteOffset:a.length,byteLength:b.length,byteStride:2,count:3,mode:'TRIANGLES'}}}],
    [{bufferView:0,count:3,componentType:5126,type:'VEC3'},{bufferView:1,count:3,componentType:5123,type:'SCALAR'}],{attributes:{POSITION:0},indices:1},name);
  doc.buffers.push({byteLength:42,extensions:{[name]:{fallback:true}}});return doc;
}
for(const name of ['EXT_meshopt_compression','KHR_meshopt_compression'])test(`${name} decodes required-extension fallback buffers into renderable immutable packages`,()=>fixture(async directory=>{
  const doc=await meshoptFixture(name),file=await save(directory,doc),asset=await readGltf(file);
  assert.deepEqual(readAccessor(asset,0),[[0,0,0],[1,0,0],[0,1,0]]);assert.deepEqual(readAccessor(asset,1),[[0],[1],[2]]);
  assert.deepEqual(asset.decodedCompression,[name]);assert.equal(asset.decodedBytes,42);
  const imported=await importAsset({source:file,output:path.join(directory,'imports'),size:64});assert.equal(imported.manifest.parts.length,1);await verifyImport(imported.directory);
}));
test('meshopt rejects malformed ranges, decoded budgets and corrupt streams before publication',()=>fixture(async directory=>{
  const doc=await meshoptFixture();doc.bufferViews[0].extensions.EXT_meshopt_compression.byteOffset=1e6;
  await assert.rejects(readGltf(await save(directory,doc)),/exceeds its buffer/);
  doc.bufferViews[0].extensions.EXT_meshopt_compression.byteOffset=0;doc.bufferViews[0].extensions.EXT_meshopt_compression.count=3000000;doc.bufferViews[0].extensions.EXT_meshopt_compression.byteStride=252;doc.bufferViews[0].byteLength=756000000;
  await assert.rejects(readGltf(await save(directory,doc)),/256 MiB/);
  const bad=await meshoptFixture(),buffer=Buffer.from(bad.buffers[0].uri.split(',')[1],'base64');buffer[0]=0;bad.buffers[0].uri='data:application/octet-stream;base64,'+buffer.toString('base64');
  await assert.rejects(importAsset({source:await save(directory,bad),output:path.join(directory,'imports'),size:64}),/Compressed geometry decode failed/);
}));
async function dracoPrimitive(rows,indices,attributes) {
  const module=await draco.createEncoderModule({}),mesh=new module.Mesh(),builder=new module.MeshBuilder(),encoder=new module.Encoder(),encoded=new module.DracoInt8Array();
  try {
    builder.AddFacesToMesh(mesh,indices.length/3,new Uint32Array(indices));const ids={};
    for(const [name,values]of Object.entries(rows))ids[name]=builder.AddFloatAttributeToMesh(mesh,name==='POSITION'?module.POSITION:name==='NORMAL'?module.NORMAL:name==='TEXCOORD_0'?module.TEX_COORD:module.GENERIC,values.length,values[0].length,new Float32Array(values.flat()));
    encoder.SetSpeedOptions(5,5);encoder.SetEncodingMethod(module.MESH_SEQUENTIAL_ENCODING);
    const length=encoder.EncodeMeshToDracoBuffer(mesh,encoded);assert.ok(length>0);
    const bytes=Buffer.alloc(length);for(let i=0;i<length;i++)bytes.writeInt8(encoded.GetValue(i),i);
    return {bytes,primitive:{attributes,indices:Object.keys(attributes).length,extensions:{KHR_draco_mesh_compression:{bufferView:0,attributes:ids}}}};
  }finally{module.destroy(encoded);module.destroy(encoder);module.destroy(builder);module.destroy(mesh);}
}
test('Draco attributes and triangle topology are decoded using their unique attribute identifiers',()=>fixture(async directory=>{
  const {bytes,primitive}=await dracoPrimitive({POSITION:[[0,0,0],[1,0,0],[0,1,0]],TEXCOORD_0:[[0,0],[1,0],[0,1]]},[0,1,2],{POSITION:0,TEXCOORD_0:1});
  const doc=document(bytes,[{buffer:0,byteLength:bytes.length}],[{count:3,componentType:5126,type:'VEC3'},{count:3,componentType:5126,type:'VEC2'},{count:3,componentType:5123,type:'SCALAR'}],primitive,'KHR_draco_mesh_compression'),file=await save(directory,doc),asset=await readGltf(file),decoded=asset.document.meshes[0].primitives[0];
  assert.deepEqual(readAccessor(asset,decoded.attributes.POSITION),[[0,0,0],[1,0,0],[0,1,0]]);assert.deepEqual(readAccessor(asset,decoded.indices),[[0],[1],[2]]);
  const imported=await importAsset({source:file,output:path.join(directory,'imports'),size:64});await verifyImport(imported.directory);
  const again=await importAsset({source:file,output:path.join(directory,'imports'),size:64});assert.equal(again.manifest.id,imported.manifest.id);
  const converted=await importAsset({source:file,output:path.join(directory,'imports'),size:64,coordinates:{unit:'cm',yawDegrees:90}});
  assert.ok(converted.manifest.parts[0].position.every((v,i)=>Math.abs(v-[0,.005,.005][i])<1e-7));
  doc.meshes[0].primitives[0].extensions.KHR_draco_mesh_compression.attributes.POSITION=999;
  await assert.rejects(readGltf(await save(directory,doc)),/attribute is missing/);
}));
test('compressed character geometry preserves integer skin indices and all bone animation samples',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),plain=await importAnimation(await save(directory,doc),{normalize:true}),plainConverted=await importAnimation(await save(directory,doc),{normalize:true,coordinates:{unit:'cm',yawDegrees:90}}),parsed=await readGltf(await save(directory,doc)),p=doc.meshes[0].primitives[0],rows={};
  for(const [key,index]of Object.entries(p.attributes))rows[key]=readAccessor(parsed,index);
  const {bytes,primitive}=await dracoPrimitive(rows,readAccessor(parsed,p.indices).flat(),p.attributes),old=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),combined=Buffer.concat([old,bytes]);
  const view=doc.bufferViews.length;doc.bufferViews.push({buffer:0,byteOffset:old.length,byteLength:bytes.length});doc.buffers[0]={byteLength:combined.length,uri:'data:application/octet-stream;base64,'+combined.toString('base64')};
  // Keep the original accessor metadata, which describes normalized/integer types.
  primitive.indices=p.indices;primitive.extensions.KHR_draco_mesh_compression.bufferView=view;doc.meshes[0].primitives[0]=primitive;doc.extensionsRequired=['KHR_draco_mesh_compression'];
  const compressed=await importAnimation(await save(directory,doc),{normalize:true});assert.deepEqual(compressed.binary,plain.binary);assert.deepEqual(compressed.header.bones,plain.header.bones);
  const converted=await importAnimation(await save(directory,doc),{normalize:true,coordinates:{unit:'cm',yawDegrees:90}});assert.deepEqual(converted.binary,plainConverted.binary);
}));

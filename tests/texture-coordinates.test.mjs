import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {MeshoptEncoder} from 'meshoptimizer/encoder';
import {writeMultiUvFixture} from '../tools/offline/texture-coordinates-fixture.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {readGltf,readAccessor} from '../engine/assets/gltf-document.mjs';
import {importAsset,verifyImport,appendAnimation,attachModel} from '../engine/assets/import-service.mjs';
import {loadScene,buildScene} from '../engine/scene/index.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'ng-texture-coordinates-'));try{await fn(directory);}finally{await fs.rm(directory,{recursive:true,force:true});}}
const slots=['albedo','normal','orm','emissive','occlusion'];

test('static materials retain five independent source UV sets, transform overrides and doubled back faces',()=>fixture(async dir=>{
  const source=await writeMultiUvFixture(path.join(dir,'source'),{all:true}),input=await readGltf(source),asset=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);
  const part=asset.manifest.parts[0],mesh=JSON.parse(await fs.readFile(path.join(asset.directory,part.mesh))),prim=input.document.meshes[0].primitives[0];
  assert.equal(mesh.schema,'nativegpu.mesh/4');assert.equal(mesh.vertices[0].length,24);assert.deepEqual(part.textureCoordinates,Object.fromEntries(slots.map((slot,i)=>[slot,i+1])));
  const count=input.document.accessors[prim.attributes.POSITION].count;assert.equal(mesh.vertices.length,count*2);
  for(let slot=0;slot<5;slot++){const uv=readAccessor(input,prim.attributes[`TEXCOORD_${slot+1}`],'VEC2');for(let v=0;v<count;v++){assert.deepEqual(mesh.vertices[v].slice(6+slot*2,8+slot*2),uv[v]);assert.deepEqual(mesh.vertices[count+v].slice(6+slot*2,8+slot*2),uv[v]);}}
  assert.equal(asset.manifest.materials.m1.textureMappings.occlusion.rotation,.27);
}));

test('characters keep per-slot UV sidecars through skin merge, morphs, fitting, appended motion and source-independent packages',()=>fixture(async dir=>{
  const source=await writeMultiUvFixture(path.join(dir,'source'),{all:true,character:true}),input=await readGltf(source),asset=await importAsset({source,output:path.join(dir,'assets/imported'),size:64}),header=JSON.parse(await fs.readFile(path.join(asset.directory,'character/character.json'))),bytes=await fs.readFile(path.join(asset.directory,'character/animation.bin'));
  assert.equal(header.mesh.textureUvOffset,header.mesh.vertices*64);assert.equal(header.mesh.textureUvBytes,header.mesh.vertices*32);assert.equal(header.mesh.tangentOffset,header.mesh.vertices*96);assert.equal(header.mesh.tangentBytes,header.mesh.vertices*16);assert.equal(header.mesh.indexOffset,header.mesh.vertices*112);assert.ok(header.morphs.length>0);
  const primitive=input.document.meshes[0].primitives[0],sourceCount=input.document.accessors[primitive.attributes.POSITION].count;
  for(let slot=0;slot<4;slot++){const uv=readAccessor(input,primitive.attributes[`TEXCOORD_${slot+2}`],'VEC2');for(let v=0;v<sourceCount;v++)for(let k=0;k<2;k++)assert.equal(bytes.readFloatLE(header.mesh.textureUvOffset+v*32+slot*8+k*4),uv[v][k]);}
  const motion=await writeMotionFixture(path.join(dir,'motion')),appended=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'assets/imported')}),newHeader=JSON.parse(await fs.readFile(path.join(appended.directory,'character/character.json'))),newBytes=await fs.readFile(path.join(appended.directory,'character/animation.bin'));
  assert.deepEqual(newHeader.mesh,header.mesh);assert.deepEqual(newBytes.subarray(0,header.mesh.indexOffset),bytes.subarray(0,header.mesh.indexOffset));
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.materials;for(const entity of scene.entities){delete entity.character;delete entity.material;}const file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,JSON.stringify(scene));
  const attached=await attachModel({directory:appended.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});assert.ok(attached.entities[0].character);assert.ok(attached.entities[0].scale.every(v=>v>0));
  const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith('animation.bin')));
}));

test('unused UV streams do not change legacy geometry and nonzero source selection changes immutable identity',()=>fixture(async dir=>{
  const source=await writeMultiUvFixture(path.join(dir,'source')),doc=JSON.parse(await fs.readFile(source)),info=doc.materials[0].pbrMetallicRoughness.baseColorTexture;
  info.texCoord=0;delete info.extensions.KHR_texture_transform.texCoord;await fs.writeFile(source,JSON.stringify(doc));const legacy=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.equal(JSON.parse(await fs.readFile(path.join(legacy.directory,legacy.manifest.parts[0].mesh))).schema,'nativegpu.mesh/1');
  delete doc.meshes[0].primitives[0].attributes.TEXCOORD_7;await fs.writeFile(source,JSON.stringify(doc));const noUnused=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.equal(legacy.manifest.parts[0].sha256,noUnused.manifest.parts[0].sha256);
  const multi=await importAsset({source:await writeMultiUvFixture(path.join(dir,'changed')),output:path.join(dir,'imports'),size:64});assert.notEqual(multi.manifest.parts[0].sha256,legacy.manifest.parts[0].sha256);
}));

test('missing UV sets, wrong accessor counts and invalid coordinate selectors fail without publishing partial packages',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const source=await writeMultiUvFixture(path.join(dir,'source-'+character),{character}),original=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports-'+character);
    for(const edit of [doc=>{delete doc.meshes[0].primitives[0].attributes.TEXCOORD_7;},doc=>{const index=doc.meshes[0].primitives[0].attributes.TEXCOORD_7;doc.accessors[index].count--;},doc=>{doc.materials[0].pbrMetallicRoughness.baseColorTexture.extensions.KHR_texture_transform.texCoord=-1;},doc=>{doc.materials[0].pbrMetallicRoughness.baseColorTexture.extensions.KHR_texture_transform.texCoord=null;}]){
      const doc=structuredClone(original);edit(doc);await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}));assert.deepEqual(await fs.readdir(output),[]);
    }
  }
}));

test('meshopt compressed and normalized integer source UVs retain independently selected channels',()=>fixture(async dir=>{
  for(const character of [false,true]){
  const source=await writeMultiUvFixture(path.join(dir,'source-'+character),{character}),doc=JSON.parse(await fs.readFile(source)),prim=doc.meshes[0].primitives[0],accessor=doc.accessors[prim.attributes.TEXCOORD_7],count=accessor.count;
  const bytes=Buffer.alloc(count*4);for(let i=0;i<count;i++){bytes.writeUInt16LE(i%2?49152:16384,i*4);bytes.writeUInt16LE(i<2?8192:57344,i*4+2);}
  await MeshoptEncoder.ready;const compressed=Buffer.from(MeshoptEncoder.encodeGltfBuffer(bytes,count,4,'ATTRIBUTES')),buffer=doc.buffers.length,view=doc.bufferViews.length;
  doc.buffers.push({byteLength:compressed.length,uri:'data:application/octet-stream;base64,'+compressed.toString('base64')},{byteLength:bytes.length,extensions:{EXT_meshopt_compression:{fallback:true}}});doc.bufferViews.push({buffer:buffer+1,byteLength:bytes.length,byteStride:4,extensions:{EXT_meshopt_compression:{buffer,byteLength:compressed.length,byteStride:4,count,mode:'ATTRIBUTES'}}});Object.assign(accessor,{bufferView:view,componentType:5123,normalized:true});doc.extensionsRequired.push('EXT_meshopt_compression');await fs.writeFile(source,JSON.stringify(doc));
  const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});
  if(character){const binary=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(binary.readFloatLE(24),Math.fround(16384/65535));assert.equal(binary.readFloatLE(28),Math.fround(8192/65535));assert.equal(binary.readFloatLE(64+24),Math.fround(49152/65535));}
  else{const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.equal(mesh.vertices[0][6],16384/65535);assert.equal(mesh.vertices[0][7],8192/65535);assert.equal(mesh.vertices[1][6],49152/65535);}
  }
}));

test('sparse source UV accessors retain zero-filled and overridden coordinates on static and character geometry',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const source=await writeMultiUvFixture(path.join(dir,'source-'+character),{character}),doc=JSON.parse(await fs.readFile(source)),primitive=doc.meshes[0].primitives[0],index=primitive.attributes.TEXCOORD_7,count=doc.accessors[index].count,bytes=Buffer.alloc(20),buffer=doc.buffers.length,view=doc.bufferViews.length;
    bytes.writeUInt16LE(1,0);bytes.writeUInt16LE(count-1,2);[.25,.5,.75,1].forEach((v,i)=>bytes.writeFloatLE(v,4+i*4));doc.buffers.push({byteLength:20,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:4},{buffer,byteOffset:4,byteLength:16});doc.accessors[index]={componentType:5126,type:'VEC2',count,sparse:{count:2,indices:{bufferView:view,componentType:5123},values:{bufferView:view+1}}};await fs.writeFile(source,JSON.stringify(doc));
    const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});
    if(character){const binary=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(binary.readFloatLE(24),0);assert.equal(binary.readFloatLE(64+24),.25);assert.equal(binary.readFloatLE(64+28),.5);}
    else{const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.deepEqual(mesh.vertices[0].slice(6,8),[0,0]);assert.deepEqual(mesh.vertices[1].slice(6,8),[.25,.5]);assert.deepEqual(mesh.vertices[count-1].slice(6,8),[.75,1]);}
  }
}));

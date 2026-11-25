import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import sharp from 'sharp';
import ffmpeg from 'ffmpeg-static';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { importAsset, verifyImport, attachModel, inspectAsset } from '../engine/assets/import-service.mjs';
import { readGltf, readAccessor } from '../engine/assets/gltf-document.mjs';
import { readStaticModel } from '../engine/assets/model-import.mjs';
import { loadScene, saveScene, undoSceneFile, buildScene } from '../engine/scene/index.mjs';
import { makeAnimationFixture } from '../tools/offline/animation-fixture.mjs';
import { importAnimation } from '../tools/offline/animation.mjs';
import { createEditorService } from '../editor/service.mjs';
import { sha256Hex } from '../engine/assets/canonical.mjs';
import { readNgtex,decodeFirstMip } from '../tools/textures.mjs';
import { patchMaterial } from '../tools/material.mjs';

async function fixture(fn) {const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-import-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
async function gltf(dir,{glb=false,materials=false}={}) {
  const binary=Buffer.alloc(3*3*4+3*2*4+3*2);let at=0;
  for(const value of [0,0,0,2,0,0,0,2,0,0,0,1,0,0,1]){binary.writeFloatLE(value,at);at+=4;}
  [0,1,2].forEach((v,i)=>binary.writeUInt16LE(v,60+i*2));
  const doc={asset:{version:'2.0'},scene:0,scenes:[{nodes:[0]}],nodes:[{name:'Imported triangle',translation:[2,3,4],scale:[2,1,1],mesh:0}],
    meshes:[{primitives:[{attributes:{POSITION:0,TEXCOORD_0:1},indices:2,...(materials?{material:0}:{})}]}],
    accessors:[{bufferView:0,componentType:5126,count:3,type:'VEC3'},{bufferView:1,componentType:5126,count:3,type:'VEC2'},{bufferView:2,componentType:5123,count:3,type:'SCALAR'}],
    bufferViews:[{buffer:0,byteOffset:0,byteLength:36},{buffer:0,byteOffset:36,byteLength:24},{buffer:0,byteOffset:60,byteLength:6}],buffers:[{byteLength:binary.length}]};
  if(materials){const image=await sharp({create:{width:2,height:2,channels:4,background:{r:220,g:80,b:30,alpha:1}}}).jpeg().toBuffer();
    doc.images=[{uri:'data:image/jpeg;base64,'+image.toString('base64')}];doc.textures=[{source:0}];doc.materials=[{pbrMetallicRoughness:{baseColorTexture:{index:0},roughnessFactor:.7,metallicFactor:.2},doubleSided:true}];}
  const file=path.join(dir,glb?'model.glb':'model.gltf');
  if(glb)await fs.writeFile(file,toGlb(doc,binary));else{doc.buffers[0].uri='data:application/octet-stream;base64,'+binary.toString('base64');await fs.writeFile(file,JSON.stringify(doc));}
  return file;
}
function toGlb(doc,binary) {
  const json=Buffer.from(JSON.stringify(doc)),padded=Buffer.alloc(Math.ceil(json.length/4)*4,32);json.copy(padded);
  const bin=Buffer.alloc(Math.ceil(binary.length/4)*4);binary.copy(bin);
  const bytes=Buffer.alloc(12+8+padded.length+8+bin.length);bytes.writeUInt32LE(0x46546c67);bytes.writeUInt32LE(2,4);bytes.writeUInt32LE(bytes.length,8);
  bytes.writeUInt32LE(padded.length,12);bytes.writeUInt32LE(0x4e4f534a,16);padded.copy(bytes,20);const at=20+padded.length;
  bytes.writeUInt32LE(bin.length,at);bytes.writeUInt32LE(0x004e4942,at+4);bin.copy(bytes,at+8);return bytes;
}

test('GLB and glTF imports preserve transformed geometry, UVs, materials and deterministic identity',()=>fixture(async dir=>{
  const file=await gltf(dir,{glb:true,materials:true}),out=path.join(dir,'imports');
  const first=await importAsset({source:file,output:out,size:64}),second=await importAsset({source:file,output:out,size:64});
  assert.equal(first.manifest.id,second.manifest.id);assert.deepEqual(await verifyImport(first.directory),first.manifest);
  assert.deepEqual(first.manifest.parts[0].position,[4,4,-4]);assert.deepEqual(first.manifest.parts[0].scale,[4,2,.001]);
  const mesh=JSON.parse(await fs.readFile(path.join(first.directory,first.manifest.parts[0].mesh),'utf8'));
  assert.equal(mesh.vertices.length,6);assert.deepEqual(mesh.vertices[1].slice(6),[1,0]);assert.equal(mesh.indices.length,6);
  const scene=await loadScene(path.join(first.directory,'preview.scene.json'));assert.equal(scene.entities.filter(e=>e.mesh).length,1);
  const material=first.manifest.materials.m1;assert.equal(material.albedo,'m1');
  assert.ok(first.manifest.files.some(f=>f.file==='textures/m1.albedo.ngtex'));
}));
test('tampered geometry and import metadata are rejected',()=>fixture(async dir=>{
  const result=await importAsset({source:await gltf(dir),output:path.join(dir,'imports'),size:64});
  const mesh=path.join(result.directory,result.manifest.parts[0].mesh);await fs.appendFile(mesh,' ');
  await assert.rejects(verifyImport(result.directory),e=>e.code==='NG-E-ASSET-HASH');
}));
test('preview scene content and generated identifiers are locked into package identity',()=>fixture(async dir=>{
  const result=await importAsset({source:await gltf(dir),output:path.join(dir,'imports'),size:64});
  const file=path.join(result.directory,'preview.scene.json'),original=await fs.readFile(file),scene=JSON.parse(original);
  assert.equal(result.manifest.identity.version,2);scene.entities[0].position[0]+=1;
  const bytes=Buffer.from(JSON.stringify(scene));await fs.writeFile(file,bytes);
  const manifest=structuredClone(result.manifest),entry=manifest.files.find(f=>f.file==='preview.scene.json');entry.bytes=bytes.length;entry.sha256=sha256Hex(bytes);
  await fs.writeFile(path.join(result.directory,'import.json'),JSON.stringify(manifest));
  await assert.rejects(verifyImport(result.directory),/Preview scene differs/);
  scene.entities[0].position[0]-=1;scene.projectId='00000000-0000-4000-8000-000000000000';
  const changed=Buffer.from(JSON.stringify(scene));await fs.writeFile(file,changed);entry.bytes=changed.length;entry.sha256=sha256Hex(changed);await fs.writeFile(path.join(result.directory,'import.json'),JSON.stringify(manifest));
  await assert.rejects(verifyImport(result.directory),/Preview scene differs/);
}));
test('attach updates scene references, copies textures, checks revision and supports undo',()=>fixture(async dir=>{
  const result=await importAsset({source:await gltf(dir),output:path.join(dir,'imports'),size:64});
  const scene=await loadScene(path.join(result.directory,'preview.scene.json'));scene.entities=scene.entities.filter(e=>!e.mesh);
  const file=path.join(dir,'main.scene.json');await saveScene(file,scene);
  const changed=await attachModel({directory:result.directory,scenePath:file,expectedRevision:0,collider:true});
  assert.equal(changed.revision,1);assert.equal(changed.entities.at(-1).collider,true);
  assert.ok(changed.entities.at(-1).mesh.file.startsWith('imports/'));assert.ok(await fs.stat(path.join(dir,'textures',Object.values(changed.materials).at(-1).albedo+'.albedo.ngtex')));
  await assert.rejects(attachModel({directory:result.directory,scenePath:file,expectedRevision:0}),e=>e.code==='REVISION_CONFLICT');
  const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal(undone.scene.entities.length,2);
}));
test('OBJ supports negative indices, UVs, generated normals and MTL image dependencies',()=>fixture(async dir=>{
  await fs.writeFile(path.join(dir,'material.mtl'),'newmtl red\nKd 1 0.5 0.2\n');
  const file=path.join(dir,'mesh.obj');await fs.writeFile(file,'mtllib material.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl red\nf -3/1 -2/2 -1/3\n');
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});assert.equal(result.manifest.parts[0].material,'m1');await verifyImport(result.directory);
}));
test('shared accessor supports sparse normalized values and rejects out of range replacements',()=>fixture(async dir=>{
  const bytes=Buffer.from([1,255,128]);const asset={document:{accessors:[{componentType:5121,normalized:true,type:'SCALAR',count:3,sparse:{count:1,indices:{bufferView:0,componentType:5121},values:{bufferView:1}}}],bufferViews:[{buffer:0,byteOffset:0,byteLength:1},{buffer:0,byteOffset:1,byteLength:1}]},buffers:[bytes]};
  assert.deepEqual(readAccessor(asset,0,'SCALAR'),[[0],[1],[0]]);bytes[0]=3;assert.throws(()=>readAccessor(asset,0));
}));
test('unsafe dependencies and unsupported required extensions fail without a published package',()=>fixture(async dir=>{
  const file=await gltf(dir),doc=JSON.parse(await fs.readFile(file,'utf8'));doc.buffers[0].uri='../outside.bin';await fs.writeFile(file,JSON.stringify(doc));
  await assert.rejects(readGltf(file));
  doc.buffers[0].uri='data:application/octet-stream;base64,'+Buffer.alloc(66).toString('base64');doc.extensionsRequired=['KHR_draco_mesh_compression'];await fs.writeFile(file,JSON.stringify(doc));
  await assert.rejects(importAsset({source:file,output:path.join(dir,'imports'),size:64}),e=>e.code==='NG-E-ASSET-UNSUPPORTED');assert.deepEqual(await fs.readdir(path.join(dir,'imports')),[]);
}));
test('PNG/JPEG textures use semantic conversion and immutable cooked packages',()=>fixture(async dir=>{
  const file=path.join(dir,'color.jpg');await sharp({create:{width:4,height:4,channels:3,background:'#c06020'}}).jpeg().toFile(file);
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});assert.equal(result.manifest.kind,'texture');await verifyImport(result.directory);
}));
test('MASK materials retain alpha through cooking, attachment, schema and material patches',()=>fixture(async dir=>{
  const file=await gltf(dir,{materials:true}),doc=JSON.parse(await fs.readFile(file,'utf8'));
  const pixels=Buffer.from([255,50,20,0,255,50,20,255,255,50,20,0,255,50,20,255]);
  const png=await sharp(pixels,{raw:{width:2,height:2,channels:4}}).png().toBuffer();doc.images[0].uri='data:image/png;base64,'+png.toString('base64');
  doc.materials[0].alphaMode='MASK';doc.materials[0].alphaCutoff=.3;doc.materials[0].pbrMetallicRoughness.baseColorFactor=[1,1,1,.5];await fs.writeFile(file,JSON.stringify(doc));
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});assert.equal(result.manifest.materials.m1.alphaMode,'MASK');assert.equal(result.manifest.materials.m1.alphaCutoff,.3);
  const texture=readNgtex(await fs.readFile(path.join(result.directory,'textures/m1.albedo.ngtex'))),decoded=decodeFirstMip(texture,'albedo');assert.notEqual(texture.format,1);
  const alpha=Array.from(decoded).filter((_,i)=>i%4===3);assert.ok(Math.min(...alpha)<10);assert.ok(Math.max(...alpha)>115&&Math.max(...alpha)<140);
  const scene=await loadScene(path.join(result.directory,'preview.scene.json')),changed=patchMaterial(scene,'m1',{alphaMode:'OPAQUE',alphaCutoff:.8});assert.equal(changed.after.alphaCutoff,.8);
  assert.throws(()=>patchMaterial(scene,'m1',{alphaMode:'INVALID'}),/OPAQUE, MASK or BLEND/);assert.throws(()=>patchMaterial(scene,'m1',{alphaCutoff:1.1}),/\[0, 1\]/);
  const sceneFile=path.join(dir,'main.scene.json');await saveScene(sceneFile,scene);const attached=await attachModel({directory:result.directory,scenePath:sceneFile,expectedRevision:0});
  assert.equal(attached.materials[result.manifest.id.slice(0,12)+'_m1'].alphaMode,'MASK');await verifyImport(result.directory);
}));
test('animated GLB shares the source reader and produces the same pose samples as JSON glTF',()=>fixture(async dir=>{
  const file=path.join(dir,'character.gltf');await fs.writeFile(file,JSON.stringify(makeAnimationFixture()));
  const parsed=await readGltf(file),doc=parsed.document;delete doc.buffers[0].uri;
  const glbFile=path.join(dir,'character.glb');await fs.writeFile(glbFile,toGlb(doc,parsed.buffers[0]));
  const a=await importAnimation(file),b=await importAnimation(glbFile);assert.deepEqual(a.binary,b.binary);
  assert.equal((await inspectAsset(glbFile)).kind,'character');
}));
test('Assimp adapter imports STL without an installed DCC application',()=>fixture(async dir=>{
  const file=path.join(dir,'triangle.stl');await fs.writeFile(file,'solid test\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid test\n');
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});assert.equal(result.manifest.kind,'model');assert.equal(result.manifest.parts.length,1);await verifyImport(result.directory);
}));
test('WAV import produces a portable native sound bank',()=>fixture(async dir=>{
  const file=path.join(dir,'sound.wav'),bytes=Buffer.alloc(44+480*2);bytes.write('RIFF');bytes.writeUInt32LE(bytes.length-8,4);bytes.write('WAVEfmt ',8);bytes.writeUInt32LE(16,16);bytes.writeUInt16LE(1,20);bytes.writeUInt16LE(1,22);bytes.writeUInt32LE(48000,24);bytes.writeUInt32LE(96000,28);bytes.writeUInt16LE(2,32);bytes.writeUInt16LE(16,34);bytes.write('data',36);bytes.writeUInt32LE(960,40);await fs.writeFile(file,bytes);
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});await verifyImport(result.directory);
  const bank=JSON.parse(await fs.readFile(path.join(result.directory,result.manifest.bank),'utf8'));assert.equal(bank.sounds[0].file,'audio/sound.ngsnd');assert.equal(bank.sampleRate,48000);
}));
test('cloth and water imports reuse native asset validators',()=>fixture(async dir=>{
  for(const [kind,file]of [['cloth','assets/cloth/banner.cloth.json'],['water','assets/water/pond_still.water.json']]) {
    const source=kind==='cloth'?(await fs.readdir('assets/cloth')).find(f=>f.endsWith('.cloth.json')):null;
    const result=await importAsset({source:source?path.join('assets/cloth',source):file,output:path.join(dir,'imports'),size:64});assert.equal(result.manifest.kind,kind);await verifyImport(result.directory);
  }
}));
test('compressed audio decodes to the native sound bank without a system FFmpeg install',()=>fixture(async dir=>{
  const file=path.join(dir,'tone.mp3');await promisify(execFile)(ffmpeg,['-nostdin','-hide_banner','-loglevel','error','-f','lavfi','-i','sine=frequency=440:duration=0.05',file],{windowsHide:true});
  const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});await verifyImport(result.directory);
  const bank=JSON.parse(await fs.readFile(path.join(result.directory,result.manifest.bank),'utf8'));assert.equal(bank.sounds[0].channels,2);assert.ok(bank.sounds[0].frames>=2400);
}));
test('effect import locks and relocates real texture dependencies',()=>fixture(async dir=>{
  const result=await importAsset({source:'assets/effects/sprite_demo.effect.json',output:path.join(dir,'imports'),size:64});await verifyImport(result.directory);
  assert.ok(result.manifest.files.some(f=>f.file==='assets/effects/textures/reference_glow.albedo.ngtex'));
}));
test('external clip and bone names produce explicit controller bindings and a verified character package',()=>fixture(async dir=>{
  const doc=makeAnimationFixture();doc.nodes.forEach((n,i)=>n.name='ExternalBone'+i);doc.animations=doc.animations.slice(0,1);doc.animations[0].name='VendorTake';
  const file=path.join(dir,'vendor.gltf');await fs.writeFile(file,JSON.stringify(doc));const result=await importAsset({source:file,output:path.join(dir,'imports'),size:64});await verifyImport(result.directory);
  assert.deepEqual(result.manifest.controller,Array(5).fill('VendorTake'));assert.equal(result.manifest.attachments.chest,'');assert.ok(result.manifest.warnings.length>=5);
  const scene=await loadScene(path.join(result.directory,'preview.scene.json'));assert.ok(scene.materials.default);
}));
test('editor asset actions import, catalog and attach through revision-controlled scene edits',()=>fixture(async dir=>{
  const assets=path.join(dir,'assets');await fs.mkdir(assets);
  const initial=await importAsset({source:await gltf(dir),output:path.join(assets,'imported'),size:64});
  const scene=await loadScene(path.join(initial.directory,'preview.scene.json'));scene.entities=scene.entities.filter(e=>!e.mesh);await saveScene(path.join(assets,'main.scene.json'),scene);
  const editor=createEditorService({root:dir});try {
    const catalog=await editor.call('asset_list');assert.equal(catalog.ok,true);assert.equal(catalog.result.length,1);
    const attached=await editor.call('asset_attach',{id:initial.manifest.id,scenePath:'assets/main.scene.json',expectedRevision:0});assert.equal(attached.ok,true);assert.equal(attached.result.revision,1);
    const conflict=await editor.call('asset_attach',{id:initial.manifest.id,scenePath:'assets/main.scene.json',expectedRevision:0});assert.equal(conflict.ok,false);assert.equal(conflict.error.code,'REVISION_CONFLICT');
    assert.equal((await editor.call('asset_attach',{id:'../outside',scenePath:'assets/main.scene.json',expectedRevision:1})).ok,false);
  }finally{await editor.close();}
}));
test('scene builds collect binary textures and geometry and detect tampered cached dependencies',()=>fixture(async dir=>{
  const imported=await importAsset({source:await gltf(dir),output:path.join(dir,'imports'),size:64}),file=path.join(imported.directory,'preview.scene.json');
  const built=await buildScene(file,path.join(dir,'build'));assert.ok(built.manifest.includedAssetTypes.includes('imported-mesh'));
  const mesh=built.manifest.files.find(item=>item.path.endsWith('.mesh.json'));assert.ok(mesh);
  const texture=built.manifest.files.find(item=>item.path.endsWith('.ngtex'));assert.deepEqual(await fs.readFile(path.join(built.packageDirectory,texture.path)),await fs.readFile(path.join(imported.directory,texture.path)));
  assert.equal((await buildScene(file,path.join(dir,'build'))).cacheHit,true);
  await fs.appendFile(path.join(built.packageDirectory,mesh.path),' ');await assert.rejects(buildScene(file,path.join(dir,'build')),e=>e.code==='BUILD_CONFLICT');
}));
test('CUBICSPLINE rotation resampling preserves endpoints and yields finite quantized poses',()=>fixture(async dir=>{
  const doc=makeAnimationFixture(),file=path.join(dir,'curves.gltf');await fs.writeFile(file,JSON.stringify(doc));
  const original=await importAnimation(file),parsed=await readGltf(file),animation=doc.animations.find(a=>a.channels.some(c=>c.target.path==='rotation'));
  const channel=animation.channels.find(c=>c.target.path==='rotation'),sampler=animation.samplers[channel.sampler],values=readAccessor(parsed,sampler.output,'VEC4');
  const cubic=Buffer.alloc(values.length*3*4*4);values.forEach((value,i)=>value.forEach((v,k)=>cubic.writeFloatLE(v,(i*3+1)*16+k*4)));
  const base=Buffer.concat([parsed.buffers[0],Buffer.alloc((4-parsed.buffers[0].length%4)%4)]),binary=Buffer.concat([base,cubic]);
  const view=doc.bufferViews.length;doc.bufferViews.push({buffer:0,byteOffset:base.length,byteLength:cubic.length});
  sampler.output=doc.accessors.length;doc.accessors.push({bufferView:view,componentType:5126,type:'VEC4',count:values.length*3});sampler.interpolation='CUBICSPLINE';
  doc.buffers[0]={byteLength:binary.length,uri:'data:application/octet-stream;base64,'+binary.toString('base64')};await fs.writeFile(file,JSON.stringify(doc));
  const result=await importAnimation(file),a=original.header.clips.find(c=>c.name===animation.name),b=result.header.clips.find(c=>c.name===animation.name),stride=result.header.bones.length*40;
  assert.deepEqual(result.binary.subarray(b.offset,b.offset+stride),original.binary.subarray(a.offset,a.offset+stride));
  assert.deepEqual(result.binary.subarray(b.offset+(b.frames-1)*stride,b.offset+b.frames*stride),original.binary.subarray(a.offset+(a.frames-1)*stride,a.offset+a.frames*stride));
}));

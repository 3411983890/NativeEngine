import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {makeAnimationFixture} from '../tools/offline/animation-fixture.mjs';
import {importAnimation} from '../tools/offline/animation.mjs';
import {readGltf,readAccessor,nodeMatrix,identityMatrix,multiplyMatrix} from '../engine/assets/gltf-document.mjs';
import {decomposeCharacterNode} from '../engine/assets/character-source.mjs';
import {importAsset,verifyImport,inspectAsset} from '../engine/assets/import-service.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-character-'));try{await fn(directory);}finally{await fs.rm(directory,{recursive:true,force:true});}}
function append(doc,rows,type,componentType=5126) {
  const old=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),base=Buffer.concat([old,Buffer.alloc((4-old.length%4)%4)]),components={SCALAR:1,VEC2:2,VEC3:3,VEC4:4,MAT4:16}[type],stride=components*(componentType===5123?2:4),bytes=Buffer.alloc(rows.length*stride);
  rows.forEach((row,i)=>row.forEach((v,k)=>componentType===5123?bytes.writeUInt16LE(v,i*stride+k*2):bytes.writeFloatLE(v,i*stride+k*4)));
  const buffer=Buffer.concat([base,bytes]),view=doc.bufferViews.length,index=doc.accessors.length;
  doc.buffers[0]={byteLength:buffer.length,uri:'data:application/octet-stream;base64,'+buffer.toString('base64')};doc.bufferViews.push({buffer:0,byteOffset:base.length,byteLength:bytes.length});doc.accessors.push({bufferView:view,type,componentType,count:rows.length});return index;
}
async function source(directory,doc){const file=path.join(directory,'character.gltf');await fs.writeFile(file,JSON.stringify(doc));return file;}
const transform=(m,p,w=1)=>[0,1,2].map(i=>m[i]*p[0]+m[i+4]*p[1]+m[i+8]*p[2]+m[i+12]*w);
function restPalette(header) {
  const globals=[];
  return header.bones.map((bone,i)=>{const m=nodeMatrix({translation:bone.rest.slice(0,3),rotation:bone.rest.slice(3,7),scale:bone.rest.slice(7)});globals[i]=bone.parent<0?m:multiplyMatrix(globals[bone.parent],m);return multiplyMatrix(globals[i],bone.inverseBind);});
}
function skinVertex(result,vertex,palette) {
  const base=vertex*64,point=[0,1,2].map(k=>result.binary.readFloatLE(base+k*4)),output=[0,0,0];
  for(let i=0;i<4;i++){const joint=result.binary.readUInt32LE(base+32+i*4),weight=result.binary.readFloatLE(base+48+i*4),p=transform(palette[joint],point);for(let k=0;k<3;k++)output[k]+=p[k]*weight;}
  return output;
}
function near(a,b){assert.equal(a.length,b.length);a.forEach((v,i)=>assert.ok(Math.abs(v-b[i])<.0002,`${a} != ${b}`));}

test('normalization preserves material ranges and reuses shared joints across primitives',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),primitive=doc.meshes[0].primitives[0];primitive.material=0;
  doc.meshes[0].primitives.push({...structuredClone(primitive),material:1});doc.materials=[{},{pbrMetallicRoughness:{baseColorFactor:[.2,.6,1,1]}}];
  const result=await importAnimation(await source(directory,doc),{normalize:true}),parts=result.header.mesh.parts;
  assert.equal(parts.length,2);assert.equal(parts[0].material,'m1');assert.equal(parts[1].material,'m2');assert.equal(parts[1].firstIndex,parts[0].indexCount);
  assert.equal(result.header.mesh.indices,parts[0].indexCount+parts[1].indexCount);assert.equal(result.header.bones.length,16);
  assert.equal(result.header.mesh.vertices,doc.accessors[primitive.attributes.POSITION].count*2);
}));
test('different inverse binds across skins produce independent palettes and correct rest positions',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),file=await source(directory,doc),parsed=await readGltf(file),primitive=doc.meshes[0].primitives[0];
  const binds=readAccessor(parsed,doc.skins[0].inverseBindMatrices,'MAT4').map(m=>m.map((v,k)=>k===12?v+.75:v));
  doc.skins.push({...doc.skins[0],inverseBindMatrices:append(doc,binds,'MAT4')});doc.nodes.push({name:'SecondSkin',mesh:0,skin:1});doc.scenes[0].nodes.push(doc.nodes.length-1);
  const result=await importAnimation(await source(directory,doc),{normalize:true}),count=doc.accessors[primitive.attributes.POSITION].count;
  assert.equal(result.header.bones.length,32);assert.equal(result.header.mesh.parts.length,2);
  const palette=restPalette(result.header);
  for(let v=0;v<count;v++){const a=skinVertex(result,v,palette),b=skinVertex(result,count+v,palette);near(b,[a[0]+.75,a[1],a[2]]);}
}));
test('a rigid attachment becomes a bone-bound part and source wrappers retain their transforms',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),head=doc.nodes.findIndex(n=>n.name==='head');assert.ok(head>=0);
  const p=append(doc,[[-.2,0,0],[.2,0,0],[0,.3,0]],'VEC3'),indices=append(doc,[[0],[1],[2]],'SCALAR',5123);
  doc.meshes.push({primitives:[{attributes:{POSITION:p},indices,material:0}]});doc.nodes[head].mesh=1;
  const root=doc.nodes.length;doc.nodes.push({name:'SourceWrapper',matrix:[1,0,0,0,0,1,0,0,0,0,1,0,2,0,3,1],children:doc.scenes[0].nodes});doc.scenes[0].nodes=[root];
  const result=await importAnimation(await source(directory,doc),{normalize:true});assert.equal(result.header.mesh.parts.length,2);
  assert.deepEqual(result.header.bones[0].rest.slice(0,3),[2,0,-3]);const attachment=result.header.mesh.parts.find(p=>p.name==='head');assert.ok(attachment);
  const firstVertex=result.binary.readUInt32LE(result.header.mesh.indexOffset+attachment.firstIndex*4),point=skinVertex(result,firstVertex,restPalette(result.header));assert.ok(point[0]>1&&point[2]<-2);
}));
test('multiple skeleton roots receive a synthetic root and skin aliases stay under the animated joint',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),secondRoot=doc.nodes.length;doc.nodes.push({name:'OtherRoot',translation:[1,0,0]});doc.scenes[0].nodes.push(secondRoot);
  doc.skins.push({joints:[secondRoot]});const primitive=structuredClone(doc.meshes[0].primitives[0]),count=doc.accessors[primitive.attributes.POSITION].count;
  primitive.attributes.JOINTS_0=append(doc,Array.from({length:count},()=>[0,0,0,0]),'VEC4',5123);doc.meshes.push({primitives:[primitive]});doc.nodes.push({name:'OtherMesh',mesh:1,skin:1});doc.scenes[0].nodes.push(doc.nodes.length-1);
  const result=await importAnimation(await source(directory,doc),{normalize:true});assert.equal(result.header.bones[0].name,'__asset_root');assert.equal(result.header.bones.filter(b=>b.parent===-1).length,1);
}));
test('double-sided character primitives duplicate normals, weights and morph deltas together',()=>fixture(async directory=>{
  const doc=makeAnimationFixture({advanced:true}),original=doc.accessors[doc.meshes[0].primitives[0].attributes.POSITION].count;doc.meshes[0].primitives[0].material=0;doc.materials=[{doubleSided:true}];
  const result=await importAnimation(await source(directory,doc),{normalize:true});assert.equal(result.header.mesh.vertices,original*2);assert.ok(result.header.softRegions[0].vertices.some(i=>i>=original));
  for(let i=0;i<original;i++){near([0,1,2].map(k=>result.binary.readFloatLE(i*64+k*4)),[0,1,2].map(k=>result.binary.readFloatLE((original+i)*64+k*4)));near([0,1,2].map(k=>result.binary.readFloatLE(i*64+12+k*4)),[0,1,2].map(k=>-result.binary.readFloatLE((original+i)*64+12+k*4)));}
  assert.ok(result.header.morphs.every(m=>m.count%2===0));
}));
test('character assets without animation receive an explicit bind-pose preview',()=>fixture(async directory=>{
  const doc=makeAnimationFixture();delete doc.animations;const result=await importAsset({source:await source(directory,doc),output:path.join(directory,'imports'),size:64});
  assert.equal(result.manifest.clips,1);assert.equal(result.manifest.controller[0],'bind_pose');assert.ok(result.manifest.warnings.some(w=>w.includes('bind-pose')));await verifyImport(result.directory);
}));
test('matrix decomposition reconstructs mirrored affine transforms and rejects shear',()=>{
  for(const rotation of [[0,0,0,1],[.70710678,0,0,.70710678],[0,.70710678,0,.70710678],[0,0,1,0]]){
    const original=nodeMatrix({translation:[2,3,4],rotation,scale:[2,2,2]}),result=nodeMatrix(decomposeCharacterNode({matrix:original}));
    near(result,original.map((v,i)=>v*(i%4===2?-1:1)*(Math.floor(i/4)===2?-1:1)));
  }
  const shear=identityMatrix();shear[4]=.3;assert.throws(()=>decomposeCharacterNode({matrix:shear}),/shear/);
});
test('skin normalization rejects joints outside the selected scene and weight animation without morphs',()=>fixture(async directory=>{
  const doc=makeAnimationFixture();doc.nodes.push({name:'NotInScene'});doc.skins[0].joints.push(doc.nodes.length-1);
  await assert.rejects(importAnimation(await source(directory,doc),{normalize:true}),/outside the selected scene/);
  const other=makeAnimationFixture();other.animations[0].channels[0].target.path='weights';await assert.rejects(importAnimation(await source(directory,other),{normalize:true}),/without morph targets/);
}));

function morphFixture() {
  const doc=makeAnimationFixture({advanced:true});delete doc.extras.nativegpu;
  doc.meshes[0].weights=[.25,-.1];return doc;
}
function morphTrack(doc,node,values,interpolation='LINEAR') {
  return {name:'expression',channels:[{sampler:0,target:{node,path:'weights'}}],samplers:[{input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,values.map(v=>[v]),'SCALAR'),interpolation}],extras:{nativegpu:{loop:false}}};
}
const morphAt=(result,frame,target,clip=0)=>{const c=result.header.clips[clip];return result.binary.readInt32LE(c.morphOffset+(frame*c.morphTargets+target)*4)/10000;};
test('default shape weights are baked into every clip, with node defaults overriding mesh defaults',()=>fixture(async directory=>{
  const doc=morphFixture();let result=await importAnimation(await source(directory,doc),{normalize:true});
  near([morphAt(result,0,0),morphAt(result,30,1)],[.25,-.1]);
  doc.nodes.find(n=>n.mesh===0).weights=[.6,0];result=await importAnimation(await source(directory,doc),{normalize:true});near([morphAt(result,15,0),morphAt(result,15,1)],[.6,0]);
}));
for(const interpolation of ['LINEAR','STEP','CUBICSPLINE'])test(`morph-only ${interpolation} clips retain scalar grouping, endpoints and interpolation`,()=>fixture(async directory=>{
  const doc=morphFixture(),node=doc.nodes.findIndex(n=>n.mesh===0),values=interpolation==='CUBICSPLINE'?[0,0,0,.2,0,0,0,0,1,.8,0,0]:[0,.2,1,.8];
  doc.animations=[morphTrack(doc,node,values,interpolation)];const result=await importAnimation(await source(directory,doc),{normalize:true});
  near([morphAt(result,0,0),morphAt(result,0,1),morphAt(result,60,0),morphAt(result,60,1)],[0,.2,1,.8]);
  near([morphAt(result,30,0),morphAt(result,30,1)],interpolation==='STEP'?[0,.2]:[.5,.5]);
  assert.equal(result.header.clips[0].stepMorphMask,interpolation==='STEP'?3:0);
}));
test('shared mesh instances retain independent shape defaults and animation channels',()=>fixture(async directory=>{
  const doc=morphFixture(),node=doc.nodes.findIndex(n=>n.mesh===0),second=doc.nodes.length;
  doc.nodes.push({name:'second',mesh:0,skin:0,weights:[.8,.9]});doc.scenes[0].nodes.push(second);
  doc.animations=[morphTrack(doc,node,[0,0,1,1])];const result=await importAnimation(await source(directory,doc),{normalize:true});
  assert.equal(result.header.morphs.length,4);near([0,1,2,3].map(i=>morphAt(result,30,i)),[.5,.5,.8,.9]);
  const other=morphTrack(doc,second,[.2,.3,.4,.5]);doc.animations[0].channels.push({...other.channels[0],sampler:1});doc.animations[0].samplers.push(other.samplers[0]);
  const dual=await importAnimation(await source(directory,doc),{normalize:true});near([0,1,2,3].map(i=>morphAt(dual,30,i)),[.5,.5,.3,.4]);
}));
test('generated per-node shape names cannot collide with authored target names',()=>fixture(async directory=>{
  const doc=morphFixture(),original=doc.nodes.findIndex(n=>n.mesh===0);
  doc.nodes.push({name:'second',mesh:0,skin:0});doc.scenes[0].nodes.push(doc.nodes.length-1);
  doc.meshes.push({...structuredClone(doc.meshes[0]),extras:{targetNames:[`node_${original}_left_bicep`,'unique_expression']}});
  doc.nodes.push({name:'third',mesh:1,skin:0});doc.scenes[0].nodes.push(doc.nodes.length-1);
  const result=await importAnimation(await source(directory,doc),{normalize:true}),names=result.header.morphs.map(m=>m.name);
  assert.equal(names.length,6);assert.equal(new Set(names).size,6);assert.ok(names.includes(`node_${original}_left_bicep`));assert.ok(names.includes(`node_${original}_left_bicep_`));
}));
test('malformed and duplicated shape curves fail before publication',()=>fixture(async directory=>{
  const doc=morphFixture(),node=doc.nodes.findIndex(n=>n.mesh===0);doc.animations=[morphTrack(doc,node,[0,0,1])];
  await assert.rejects(importAnimation(await source(directory,doc),{normalize:true}),/key count/);
  doc.animations=[morphTrack(doc,node,[0,0,1,1])];doc.animations[0].channels.push({...doc.animations[0].channels[0]});
  await assert.rejects(importAnimation(await source(directory,doc),{normalize:true}),/Duplicate animation channel/);
  doc.animations=[morphTrack(doc,node,[0,0,100,1])];await assert.rejects(importAsset({source:await source(directory,doc),output:path.join(directory,'imports'),size:64}),/morph weight/);
  assert.deepEqual(await fs.readdir(path.join(directory,'imports')),[]);
}));
test('rigid object animation and morph-only meshes select the deforming runtime without requiring a source skin',()=>fixture(async directory=>{
  const doc=morphFixture();delete doc.skins;delete doc.extras.nativegpu;
  for(const node of doc.nodes)delete node.skin;
  const ni=doc.nodes.findIndex(n=>n.mesh===0);doc.nodes=doc.nodes.map((node,i)=>i===ni?{...node,name:'Animated prop'}:node);
  doc.animations=[morphTrack(doc,ni,[0,0,1,1])];let file=await source(directory,doc);
  assert.equal((await inspectAsset(file)).kind,'character');const result=await importAsset({source:file,output:path.join(directory,'imports'),size:64});
  const header=JSON.parse(await fs.readFile(path.join(result.directory,result.manifest.character),'utf8'));assert.equal(header.bones.length,1);assert.equal(header.morphs.length,2);
  delete doc.meshes[0].primitives[0].targets;delete doc.meshes[0].weights;delete doc.meshes[0].extras;
  doc.animations=[{name:'prop_move',channels:[{sampler:0,target:{node:ni,path:'translation'}}],samplers:[{input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,[[0,0,0],[0,1,0]],'VEC3')}]}];
  file=await source(directory,doc);const rigid=await importAnimation(file,{normalize:true});assert.equal(rigid.header.bones.length,1);assert.equal(rigid.header.clips[0].name,'prop_move');
  assert.equal(rigid.binary.readInt32LE(rigid.header.clips[0].offset+60*40+4),10000);assert.equal(rigid.header.rootMotion,'preserve');
}));
test('exporter scale noise below pose quantization is canonicalized to a uniform runtime scale',()=>fixture(async directory=>{
  const doc=makeAnimationFixture(),input=append(doc,[[0],[1]],'SCALAR'),output=append(doc,[[.12344998,.12345002,.12345],[1.000001,1.000002,1.0000015]],'VEC3');
  doc.nodes[4].scale=[1.000002,1.000003,1.000004];doc.animations=[{name:'scale_noise',channels:[{target:{node:4,path:'scale'},sampler:0}],samplers:[{input,output}]}];
  const result=await importAnimation(await source(directory,doc),{normalize:true}),bone=result.header.bones.findIndex(b=>b.name==='left_arm'),clip=result.header.clips[0];
  assert.equal(new Set(result.header.bones[bone].rest.slice(7)).size,1);
  for(let frame=0;frame<clip.frames;frame++){const at=clip.offset+(frame*result.header.bones.length+bone)*40;assert.equal(new Set([7,8,9].map(k=>result.binary.readInt32LE(at+k*4))).size,1);}
  doc.nodes[4].scale=[1,1.01,1];await assert.rejects(importAnimation(await source(directory,doc),{normalize:true}),/Nonuniform joint scale/);
}));

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {makeAnimationFixture} from '../tools/offline/animation-fixture.mjs';
import {importAsset,appendAnimation,verifyImport,inspectAsset} from '../engine/assets/import-service.mjs';
import {createEditorService} from '../editor/service.mjs';
import {main as assetCli} from '../tools/assets.mjs';
import {product,globalTransforms} from '../engine/assets/animation-sampling.mjs';
import {readBvhMotion} from '../engine/assets/bvh-motion.mjs';
import {readAccessor} from '../engine/assets/gltf-document.mjs';
import {canonicalJson,sha256Hex} from '../engine/assets/canonical.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-retarget-'));try{await fn(directory);}finally{await fs.rm(directory,{recursive:true,force:true});}}
async function save(directory,name,doc){const file=path.join(directory,name);await fs.writeFile(file,JSON.stringify(doc));return file;}
async function target(directory,{advanced=false}={}) {
  const source=await save(directory,'character.gltf',makeAnimationFixture({advanced}));return importAsset({source,output:path.join(directory,'assets/imported'),size:64});
}
function motion() {
  const doc=makeAnimationFixture();doc.nodes.pop();doc.scenes[0].nodes=[0];delete doc.meshes;delete doc.skins;delete doc.extras;
  doc.animations=[doc.animations.find(a=>a.name==='walk')];doc.animations[0].name='Take';return doc;
}
function append(doc,values,type,componentType=5126) {
  const spec={5126:[4,'writeFloatLE'],5122:[2,'writeInt16LE'],5121:[1,'writeUInt8']}[componentType];
  const old=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),padding=Buffer.alloc((4-old.length%4)%4),bytes=Buffer.alloc(values.flat().length*spec[0]);values.flat().forEach((v,i)=>bytes[spec[1]](v,i*spec[0]));
  const all=Buffer.concat([old,padding,bytes]),view=doc.bufferViews.length,index=doc.accessors.length;doc.buffers[0]={byteLength:all.length,uri:'data:application/octet-stream;base64,'+all.toString('base64')};
  doc.bufferViews.push({buffer:0,byteOffset:old.length+padding.length,byteLength:bytes.length});doc.accessors.push({bufferView:view,count:values.length,componentType,type,...(componentType===5126?{}:{normalized:true})});return index;
}
async function character(result) {
  return {header:JSON.parse(await fs.readFile(path.join(result.directory,'character/character.json'),'utf8')),binary:await fs.readFile(path.join(result.directory,'character/animation.bin'))};
}
function transformAt(data,clipName,frame,boneName) {
  const clip=data.header.clips.find(c=>c.name===clipName),bone=data.header.bones.findIndex(b=>b.name===boneName);
  return Array.from({length:10},(_,k)=>data.binary.readInt32LE(clip.offset+(frame*data.header.bones.length+bone)*40+k*4)/10000);
}
const near=(a,b,epsilon=.0003)=>{assert.equal(a.length,b.length);for(let i=0;i<a.length;i++)assert.ok(Math.abs(a[i]-b[i])<epsilon,`${a} != ${b}`);};
test('standalone motion appends deterministic clips without altering the parent geometry, textures or controller',()=>fixture(async directory=>{
  const parent=await target(directory),original=await character(parent),source=await save(directory,'walk.gltf',motion());assert.equal((await inspectAsset(source)).kind,'animation');
  const first=await appendAnimation({directory:parent.directory,source,options:{mode:'exact'}}),again=await appendAnimation({directory:parent.directory,source,options:{mode:'exact'}}),data=await character(first);
  assert.equal(first.manifest.id,again.manifest.id);assert.notEqual(first.manifest.id,parent.manifest.id);assert.equal(data.header.clips.length,7);assert.deepEqual(data.header.mesh,original.header.mesh);
  assert.deepEqual(data.binary.subarray(0,original.binary.length),original.binary);assert.deepEqual(data.header.controller,original.header.controller);
  for(const bone of original.header.bones)near(transformAt(data,'walk_Take',15,bone.name),transformAt(original,'walk',15,bone.name));
  assert.deepEqual(first.manifest.files.filter(f=>f.file.startsWith('textures/')),parent.manifest.files.filter(f=>f.file.startsWith('textures/')));
  const preview=JSON.parse(await fs.readFile(path.join(first.directory,first.manifest.previewCharacter),'utf8'));assert.deepEqual(preview.controller,Array(5).fill('walk_Take'));
  await verifyImport(first.directory);assert.deepEqual(await verifyImport(parent.directory),parent.manifest);
}));
test('automatic humanoid aliases and centimeter units transfer bone motion with proportional root translation',()=>fixture(async directory=>{
  const parent=await target(directory),sourceDoc=motion(),names=['Root','Hips','Spine2','Head','LeftArm','LeftForeArm','LeftHand','RightArm','RightForeArm','RightHand','LeftUpLeg','LeftLeg','LeftFoot','RightUpLeg','RightLeg','RightFoot'];
  sourceDoc.nodes.forEach((node,i)=>{node.name='mixamorig:'+names[i];node.translation=node.translation.map(v=>v*100);});
  const animation=sourceDoc.animations[0];animation.channels.push({target:{node:0,path:'translation'},sampler:animation.samplers.length});animation.samplers.push({input:append(sourceDoc,[[0],[1]],'SCALAR'),output:append(sourceDoc,[[0,0,0],[0,0,-200]],'VEC3')});
  const result=await appendAnimation({directory:parent.directory,source:await save(directory,'centimeters.gltf',sourceDoc)}),data=await character(result),report=JSON.parse(await fs.readFile(path.join(result.directory,result.manifest.retarget),'utf8'));
  assert.ok(Math.abs(report.translationScale-.01)<1e-7);assert.equal(report.mappings.length,16);near(transformAt(data,'centimeters_Take',60,'root').slice(0,3),[0,0,2]);
  const original=await character(parent);near(transformAt(data,'centimeters_Take',15,'left_thigh'),transformAt(original,'walk',15,'left_thigh'));
}));
test('rest-pose retargeting transfers world-space rotation between different local bone axes',()=>fixture(async directory=>{
  const parent=await target(directory),doc=motion(),rest=[0,0,Math.SQRT1_2,Math.SQRT1_2],end=product(rest,[Math.sin(Math.PI/8),0,0,Math.cos(Math.PI/8)]);
  doc.nodes[4].rotation=rest;doc.animations=[{name:'AxisMotion',channels:[{target:{node:4,path:'rotation'},sampler:0}],samplers:[{input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,[rest,end],'VEC4')}]}];
  const source=await save(directory,'axes.gltf',doc);await assert.rejects(appendAnimation({directory:parent.directory,source,options:{mode:'exact'}}),/Exact skeleton mismatch/);
  const result=await appendAnimation({directory:parent.directory,source}),data=await character(result);
  near(transformAt(data,'axes_AxisMotion',0,'left_arm').slice(3,7),[0,0,0,1]);near(transformAt(data,'axes_AxisMotion',60,'left_arm').slice(3,7),[0,-Math.sin(Math.PI/8),0,Math.cos(Math.PI/8)]);
  near(transformAt(data,'axes_AxisMotion',60,'left_forearm').slice(3,7),[0,0,0,1]);
}));
test('ambiguous names require an explicit mapping and crossed branches are rejected atomically',()=>fixture(async directory=>{
  const parent=await target(directory),doc=motion();doc.nodes[4].name='Vendor:LeftArm';doc.nodes.push({name:'Other:LeftArm',translation:[0,0,0]});
  const source=await save(directory,'ambiguous.gltf',doc),before=await fs.readdir(path.dirname(parent.directory));
  await assert.rejects(appendAnimation({directory:parent.directory,source}),/Ambiguous mapping for left_arm/);assert.deepEqual(await fs.readdir(path.dirname(parent.directory)),before);
  const result=await appendAnimation({directory:parent.directory,source,options:{boneMap:{left_arm:4}}});await verifyImport(result.directory);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{boneMap:{left_arm:4,left_calf:14,right_calf:11}}}),/crosses hierarchy/);
}));
test('motion import reports omitted branches and rejects clips that cannot affect the target',()=>fixture(async directory=>{
  const parent=await target(directory),doc=motion(),node=doc.nodes.length;doc.nodes.push({name:'UnknownCameraBone'});
  const channel={target:{node,path:'translation'},sampler:doc.animations[0].samplers.length};doc.animations[0].channels.push(channel);doc.animations[0].samplers.push({input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,[[0,0,0],[1,0,0]],'VEC3')});
  let source=await save(directory,'partial.gltf',doc),result=await appendAnimation({directory:parent.directory,source});assert.ok(result.manifest.warnings.some(w=>w.includes('UnknownCameraBone')));
  doc.animations[0].channels=[channel];source=await save(directory,'empty.gltf',doc);await assert.rejects(appendAnimation({directory:parent.directory,source}),/No animation channels/);
}));
test('unrelated source geometry transforms do not prevent motion extraction',()=>fixture(async directory=>{
  const parent=await target(directory),doc=makeAnimationFixture();doc.nodes[16].scale=[1,2,3];doc.animations=[doc.animations[1]];
  const result=await appendAnimation({directory:parent.directory,source:await save(directory,'prop-scale.gltf',doc)});await verifyImport(result.directory);
  assert.deepEqual(result.manifest.parts,parent.manifest.parts);
}));
test('independent shape clips retain defaults and LINEAR, STEP and CUBICSPLINE expressions',()=>fixture(async directory=>{
  const parent=await target(directory,{advanced:true});
  for(const interpolation of ['LINEAR','STEP','CUBICSPLINE']) {
    const doc=makeAnimationFixture({advanced:true}),values=interpolation==='CUBICSPLINE'?[0,0,.2,.4,0,0,0,0,.8,.6,0,0]:[.2,.4,.8,.6];
    doc.animations=[{name:'Expression',channels:[{target:{node:16,path:'weights'},sampler:0}],samplers:[{input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,values.map(v=>[v]),'SCALAR'),interpolation}]}];
    const result=await appendAnimation({directory:parent.directory,source:await save(directory,interpolation+'.gltf',doc)}),data=await character(result),clip=data.header.clips.at(-1);
    const at=frame=>[0,1].map(i=>data.binary.readInt32LE(clip.morphOffset+(frame*2+i)*4)/10000);
    near(at(0),[.2,.4]);near(at(60),[.8,.6]);near(at(30),interpolation==='STEP'?[.2,.4]:[.5,.5]);assert.equal(clip.stepMorphMask,interpolation==='STEP'?3:0);
  }
}));
test('normalized integer quaternion and shape samples decode in both motion append and character import',()=>fixture(async directory=>{
  const parent=await target(directory,{advanced:true}),doc=makeAnimationFixture({advanced:true}),input=append(doc,[[0],[1]],'SCALAR');
  doc.animations=[{name:'QuantizedTake',channels:[{target:{node:4,path:'rotation'},sampler:0},{target:{node:16,path:'weights'},sampler:1}],samplers:[{input,output:append(doc,[[0,0,0,32767],[0,0,23170,23170]],'VEC4',5122)},{input,output:append(doc,[[0],[64],[255],[128]],'SCALAR',5121)}]}];
  const source=await save(directory,'normalized.gltf',doc),appended=await appendAnimation({directory:parent.directory,source}),fresh=await importAsset({source,output:path.join(directory,'fresh'),size:64});
  for(const [result,clipName]of [[appended,'normalized_QuantizedTake'],[fresh,'QuantizedTake']]) {
    const data=await character(result),clip=data.header.clips.find(c=>c.name===clipName);near(transformAt(data,clipName,30,'left_arm').slice(3,7),[0,0,.3826834,.9238795]);
    const at=clip.morphOffset+30*2*4;near([data.binary.readInt32LE(at)/10000,data.binary.readInt32LE(at+4)/10000],[.5,96/255]);
  }
}));
test('clip naming, controller bindings, selected clips and invalid options are validated',()=>fixture(async directory=>{
  const parent=await target(directory),doc=motion();doc.animations[0].name='walk';const source=await save(directory,'control.gltf',doc);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{prefix:''}}),/Clip name collision/);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{clips:['missing']}}),/Unknown or duplicate/);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{translationScale:-1}}),/translationScale/);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{typo:true}}),/Unknown motion/);
  await assert.rejects(appendAnimation({directory:parent.directory,source,options:{mode:'exact',translationScale:2}}),/Exact mode requires/);
  const result=await appendAnimation({directory:parent.directory,source,options:{prefix:'custom_',controller:{walk:'custom_walk'},rootMotion:'preserve',loop:false}}),data=await character(result);
  assert.equal(data.header.controller[1],'custom_walk');assert.equal(data.header.rootMotion,'preserve');assert.equal(data.header.clips.at(-1).loop,false);
}));
test('motion matching velocity, yaw, contacts and notifies survive coordinate and unit conversion',()=>fixture(async directory=>{
  const parent=await target(directory),doc=motion();doc.nodes.forEach(n=>n.translation=n.translation.map(v=>v*100));
  doc.animations[0].extras={nativegpu:{loop:false,notifies:[{frame:15,name:'contact'}],motion:{velocity:[100,0,-200],turnRate:1,tags:3,contacts:[[0,.4],[.6,1]]}}};
  const result=await appendAnimation({directory:parent.directory,source:await save(directory,'metadata.gltf',doc)}),data=await character(result),clip=data.header.clips.at(-1);
  near(clip.velocity,[1,0,2]);assert.equal(clip.turnRate,-1);assert.equal(clip.tags,3);assert.equal(clip.matchable,true);assert.equal(clip.loop,false);assert.deepEqual(clip.notifies,[{frame:15,name:'contact'}]);
  const preview=JSON.parse(await fs.readFile(path.join(result.directory,result.manifest.previewCharacter),'utf8'));assert.ok(preview.clips.every(c=>c.matchable===false));
}));
test('CLI and editor append-motion actions keep the same immutable import contract',()=>fixture(async directory=>{
  const parent=await target(directory),source=await save(directory,'cli.gltf',motion());
  const result=await assetCli(['append-animation','--in',source,'--target',parent.directory]);await verifyImport(result.directory);
  const service=createEditorService({root:directory});try {
    const appended=await service.call('asset_append_animation',{id:parent.manifest.id,source});assert.equal(appended.ok,true);assert.equal(appended.result.manifest.id,result.manifest.id);
    assert.equal((await service.call('asset_append_animation',{id:'../outside',source})).ok,false);
  }finally{await service.close();}
}));
test('BVH motion converts without source geometry and retargets centimeter root motion',()=>fixture(async directory=>{
  const parent=await target(directory),source=path.join(directory,'capture.bvh');
  await fs.writeFile(source,`HIERARCHY
ROOT Hips
{
 OFFSET 0 82 0
 CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
 JOINT Spine2
 {
  OFFSET 0 34 0
  CHANNELS 3 Zrotation Xrotation Yrotation
  JOINT Head
  {
   OFFSET 0 43 0
   CHANNELS 3 Zrotation Xrotation Yrotation
   End Site
   {
    OFFSET 0 10 0
   }
  }
 }
}
MOTION
Frames: 3
Frame Time: 0.5
0 0 0 0 0 0 0 0 0 0 0 0
100 0 0 0 0 0 45 0 0 0 0 0
200 0 0 0 0 0 90 0 0 0 0 0
`);
  const result=await appendAnimation({directory:parent.directory,source}),data=await character(result),clip=data.header.clips.at(-1);
  near(transformAt(data,clip.name,60,'root').slice(0,3),[2,0,0]);near(transformAt(data,clip.name,60,'hips').slice(0,3),[0,.82,0]);near(transformAt(data,clip.name,60,'chest').slice(3,7),[0,0,Math.SQRT1_2,Math.SQRT1_2]);await verifyImport(result.directory);
}));
test('BVH preserves declared Euler order and rejects malformed, duplicate or excess frame data',()=>fixture(async directory=>{
  const valid='HIERARCHY ROOT root { OFFSET 1 2 3 CHANNELS 3 Zrotation Xrotation Yrotation End Site { OFFSET 0 1 0 } } MOTION Frames: 2 Frame Time: 1 0 0 0 90 90 0';
  const file=path.join(directory,'order.bvh');await fs.writeFile(file,valid);const source=await readBvhMotion(file),rotation=source.document.animations[0].samplers[1];
  near(readAccessor(source,rotation.output)[1],[.5,.5,.5,.5]);near(readAccessor(source,source.document.animations[0].samplers[0].output)[0],[1,2,3]);
  for(const invalid of [valid.replace('Xrotation','Zrotation'),valid+' 1',valid.slice(0,-2),valid.replace('Time: 1','Time: -1'),valid.replace('Frames: 2','Frames: 20000')]) {
    await fs.writeFile(file,invalid);await assert.rejects(readBvhMotion(file));
  }
}));
test('hip root-motion routing preserves world poses under a rotated root and keeps vertical body motion',()=>fixture(async directory=>{
  const targetDoc=makeAnimationFixture();targetDoc.nodes[0].rotation=[0,Math.SQRT1_2,0,Math.SQRT1_2];
  const parent=await importAsset({source:await save(directory,'turned.gltf',targetDoc),output:path.join(directory,'assets/imported'),size:64}),doc=motion();doc.nodes.forEach(n=>n.translation=n.translation.map(v=>v*100));
  doc.animations=[{name:'HipMotion',channels:[{target:{node:1,path:'translation'},sampler:0}],samplers:[{input:append(doc,[[0],[1]],'SCALAR'),output:append(doc,[[0,82,0],[100,132,100]],'VEC3')}]}];
  const source=await save(directory,'hips.gltf',doc),unrouted=await character(await appendAnimation({directory:parent.directory,source,options:{motionRoot:false}})),routed=await character(await appendAnimation({directory:parent.directory,source}));
  const globals=(data,frame)=>globalTransforms(data.header.bones,data.header.bones.map((_,i)=>i),data.header.bones.map(b=>{const t=transformAt(data,'hips_HipMotion',frame,b.name);return {translation:t.slice(0,3),rotation:t.slice(3,7),scale:t.slice(7)};}));
  for(const frame of [0,30,60]){const a=globals(unrouted,frame),b=globals(routed,frame);for(let i=1;i<a.length;i++)near(a[i].translation,b[i].translation,.001);}
  near(transformAt(routed,'hips_HipMotion',60,'root').slice(0,3),[1,0,-1]);assert.ok(transformAt(routed,'hips_HipMotion',60,'hips')[1]>.82);
  const xyz=await character(await appendAnimation({directory:parent.directory,source,options:{rootMotionAxes:'xyz'}}));near(transformAt(xyz,'hips_HipMotion',60,'root').slice(0,3),[1,.5,-1]);near(transformAt(xyz,'hips_HipMotion',60,'hips').slice(0,3),[0,.82,0]);
}));
test('a character preview cannot load an unlisted header even when the remaining identity is consistent',()=>fixture(async directory=>{
  const parent=await target(directory),result=await appendAnimation({directory:parent.directory,source:await save(directory,'preview.gltf',motion())}),manifest=structuredClone(result.manifest);
  manifest.files=manifest.files.filter(f=>f.file!==manifest.previewCharacter);manifest.identity.files=manifest.identity.files.filter(f=>f.file!==manifest.previewCharacter);
  manifest.identityJson=canonicalJson(manifest.identity);manifest.id=sha256Hex(manifest.identityJson);
  const scene=structuredClone(manifest.identity.previewScene),uuid=n=>{const h=sha256Hex(`${manifest.id}:${n}`);return `${h.slice(0,8)}-${h.slice(8,12)}-4${h.slice(13,16)}-8${h.slice(17,20)}-${h.slice(20,32)}`;};
  scene.projectId=uuid('project');scene.entities.forEach((entity,i)=>entity.id=uuid(i));const bytes=Buffer.from(canonicalJson(scene)+'\n');await fs.writeFile(path.join(result.directory,'preview.scene.json'),bytes);
  const entry=manifest.files.find(f=>f.file==='preview.scene.json');entry.bytes=bytes.length;entry.sha256=sha256Hex(bytes);await fs.writeFile(path.join(result.directory,'import.json'),canonicalJson(manifest)+'\n');
  await assert.rejects(verifyImport(result.directory),error=>error.code==='NG-E-ASSET-DEPENDENCY'&&error.message.includes('preview is missing'));
}));

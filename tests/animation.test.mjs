import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { makeAnimationFixture } from '../tools/offline/animation-fixture.mjs';
import { importAnimation, exportAnimation } from '../tools/offline/animation.mjs';

async function fixture(action, mutate = () => {}) {
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-animation-'));
  try {const doc=makeAnimationFixture();mutate(doc);const file=path.join(dir,'model.gltf');await fs.writeFile(file,JSON.stringify(doc));return await action(file,dir,doc);}
  finally {await fs.rm(dir,{recursive:true,force:true});}
}
test('animation import is byte-identical, resampled at 60 Hz, and emits binary clips',async()=>{
  await fixture(async(file,dir)=>{
    const a=await importAnimation(file),b=await importAnimation(file);
    assert.equal(a.text,b.text);assert.deepEqual(a.binary,b.binary);assert.equal(a.contentHash,b.contentHash);
    assert.equal(a.header.bones.length,16);assert.equal(a.header.clips.length,6);
    assert.ok(a.header.clips.every(c=>c.frames===61));assert.equal(a.header.sampleRate,60);
    const out=path.join(dir,'out');await exportAnimation(file,out);
    assert.equal(await fs.readFile(path.join(out,'character.json'),'utf8'),a.text);
    assert.deepEqual(await fs.readFile(path.join(out,'animation.bin')),a.binary);
    const walk=a.header.clips.find(c=>c.name==='walk'), stride=16*40;
    assert.notEqual(a.binary.readInt32LE(walk.offset+15*stride+10*40+12),0);
  });
});
for(const [name,code,mutate] of [
  ['cycle','SKELETON',d=>d.nodes[1].children.push(0)],
  ['missing joint','SKELETON',d=>d.skins[0].joints.push(999)],
  ['duplicate joint','SKELETON',d=>d.skins[0].joints.push(0)],
  ['matrix transform','UNSUPPORTED',d=>d.nodes[2].matrix=Array(16).fill(0)],
  ['nonuniform scale','UNSUPPORTED',d=>d.nodes[2].scale=[1,2,1]],
  ['missing weights','ACCESSOR',d=>delete d.meshes[0].primitives[0].attributes.WEIGHTS_0],
  ['broken accessor','ACCESSOR',d=>d.accessors[1].byteOffset=999999],
  ['sparse','ACCESSOR',d=>d.accessors[1].sparse={}],
  ['unsupported interpolation','UNSUPPORTED',d=>d.animations[0].samplers[0].interpolation='CUBICSPLINE'],
  ['duplicate channel','CLIP',d=>d.animations[0].channels.push(d.animations[0].channels[0])],
  ['morph target','UNSUPPORTED',d=>d.meshes[0].primitives[0].targets=[]],
  ['unknown extension','UNSUPPORTED',d=>d.extensionsRequired=['KHR_unknown']],
  ['duplicate names','CLIP',d=>d.animations[1].name='idle'],
  ['bad notify','CLIP',d=>d.animations[0].extras.nativegpu.notifies=[{frame:1000,name:'bad'}]],
])test(`animation rejects ${name}`,async()=>{
  await fixture(file=>assert.rejects(importAnimation(file),{code:`NG-E-ANIM-${code}`}),mutate);
});
test('skin uses actual joint remapping, not node indices',async()=>{
  await fixture(async file=>{
    const result=await importAnimation(file);assert.equal(result.binary.readUInt32LE(32),1);
  });
});
test('STEP interpolation survives the baked clip format',async()=>{
  await fixture(async file=>{const result=await importAnimation(file);assert.equal(result.header.clips[0].stepChannels[4],2);},
    d=>d.animations[0].samplers[0].interpolation='STEP');
});

test('M1-M4 asset round trip retains morphs, rig, motion and soft embedding',async()=>{
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-feature-'));
  try {const file=path.join(dir,'advanced.gltf'),doc=makeAnimationFixture({advanced:true});await fs.writeFile(file,JSON.stringify(doc));
    const a=await importAnimation(file),b=await importAnimation(file);
    assert.equal(a.header.format,'nativegpu.animation/2');assert.equal(a.header.rig.legs.length,2);assert.equal(a.header.morphs.length,2);
    assert.equal(a.header.softRegions.length,2);assert.equal(a.header.muscles.length,2);assert.equal(a.header.clips.filter(c=>c.matchable).length,8);
    assert.equal(a.contentHash,b.contentHash);assert.deepEqual(a.binary,b.binary);
    const morph=a.header.morphs[0];assert.ok(morph.count>0);assert.ok(a.binary.readUInt32LE(morph.offset)<a.header.mesh.vertices);
    doc.extras.nativegpu.rig.legs[0].tip='head';await fs.writeFile(file,JSON.stringify(doc));await assert.rejects(importAnimation(file),{code:'NG-E-ANIM-SKELETON'});
  } finally {await fs.rm(dir,{recursive:true,force:true});}
});
for(const [name,code,mutate] of [
  ['morph count','SKIN',d=>d.accessors[d.meshes[0].primitives[0].targets[0].POSITION].count=1],
  ['muscle target','SKIN',d=>d.extras.nativegpu.muscles[0].target='missing'],
  ['soft index','SKIN',d=>d.extras.nativegpu.softRegions[0].vertices=[999999]],
  ['contact phase','CLIP',d=>d.animations[0].extras.nativegpu.motion.contacts=[[0,2],[0,1]]],
])test(`M1-M4 rejects invalid ${name}`,async()=>{
  const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-feature-invalid-'));try{const file=path.join(dir,'invalid.gltf'),doc=makeAnimationFixture({advanced:true});mutate(doc);await fs.writeFile(file,JSON.stringify(doc));await assert.rejects(importAnimation(file),{code:`NG-E-ANIM-${code}`});}finally{await fs.rm(dir,{recursive:true,force:true});}
});

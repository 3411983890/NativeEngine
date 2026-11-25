import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {importAsset,appendAnimation,verifyImport} from '../engine/assets/import-service.mjs';
import {writeTangentMorphFixture} from '../tools/offline/tangent-morph-fixture.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {fitCharacterBounds} from '../engine/assets/character-bounds.mjs';
const near=(a,b)=>assert.ok(Math.abs(a-b)<.00001,`${a} != ${b}`);
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-morph-tangent-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('tangent-only morphs preserve deltas, handedness, double sides and default shape weights before skinning',()=>fixture(async dir=>{
  const source=await writeTangentMorphFixture(path.join(dir,'source'),{weight:.5}),asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));await verifyImport(asset.directory);assert.ok(h.morphs.length>0);assert.ok(h.defaultMorphWeights.every(w=>w===.5));for(const m of h.morphs){assert.equal(m.stride,40);assert.ok(m.count>0);for(let i=0;i<m.count;i++){const at=m.offset+i*40,v=b.readUInt32LE(at),p=Array.from({length:3},(_,k)=>b.readFloatLE(at+4+k*4)),n=Array.from({length:3},(_,k)=>b.readFloatLE(at+16+k*4)),t=Array.from({length:3},(_,k)=>b.readFloatLE(at+28+k*4));assert.ok(p.every(x=>x===0)&&n.every(x=>x===0));near(Math.hypot(...t),Math.SQRT2);assert.equal(Math.abs(b.readFloatLE(h.mesh.tangentOffset+v*16+12)),1);}}
  const fit=fitCharacterBounds(h,b);assert.ok(fit.height>0);const zero=structuredClone(h);zero.defaultMorphWeights=zero.defaultMorphWeights.map(()=>0);assert.deepEqual(fitCharacterBounds(zero,b),fit);
  const motion=await writeMotionFixture(path.join(dir,'motion')),next=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'motion-imports')}),nh=JSON.parse(await fs.readFile(path.join(next.directory,next.manifest.character))),nb=await fs.readFile(path.join(next.directory,'character/animation.bin'));assert.deepEqual(nh.morphs,h.morphs);const end=h.clips[0].offset;assert.deepEqual(nb.subarray(0,end),b.subarray(0,end));
}));
test('tangent delta coordinates change direction without applying length units and fitting reads 40-byte position records',()=>fixture(async dir=>{
  const source=await writeTangentMorphFixture(path.join(dir,'source'),{deltaOnly:false}),assets=[];for(const coordinates of [undefined,{unit:'cm',handedness:'left'}])assets.push(await importAsset({source,coordinates,output:path.join(dir,'imports'),size:64}));
  const data=await Promise.all(assets.map(async a=>({h:JSON.parse(await fs.readFile(path.join(a.directory,a.manifest.character))),b:await fs.readFile(path.join(a.directory,'character/animation.bin'))})));near(data[1].b.readFloatLE(data[1].h.morphs[0].offset+8),.001);near(Math.hypot(...[0,1,2].map(k=>data[1].b.readFloatLE(data[1].h.morphs[0].offset+28+k*4))),Math.SQRT2);assert.ok(fitCharacterBounds(data[0].h,data[0].b).height>0);
}));
test('tangent morphs reject missing bases, bad counts, nonfinite data and unsupported attributes without publication',()=>fixture(async dir=>{
  const source=await writeTangentMorphFixture(path.join(dir,'source')),original=JSON.parse(await fs.readFile(source)),index=original.meshes[0].primitives[0].targets[0].TANGENT;for(const edit of [d=>delete d.meshes[0].primitives[0].attributes.TANGENT,d=>d.accessors[index].count--,d=>d.meshes[0].primitives[0].targets[0].TEXCOORD_0=index,d=>{const spec=d.accessors[index],view=d.bufferViews[spec.bufferView],bytes=Buffer.from(d.buffers[view.buffer].uri.split(',')[1],'base64');bytes.writeFloatLE(Infinity);d.buffers[view.buffer].uri='data:application/octet-stream;base64,'+bytes.toString('base64');}]){const doc=structuredClone(original);edit(doc);await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output:path.join(dir,'imports'),size:64}));}
}));

test('normal-only shape targets keep the legacy record format and never require a position accessor',()=>fixture(async dir=>{
  const source=await writeTangentMorphFixture(path.join(dir,'source')),doc=JSON.parse(await fs.readFile(source));for(const mesh of doc.meshes)for(const p of mesh.primitives)p.targets=[{NORMAL:p.targets[0].TANGENT}];await fs.writeFile(source,JSON.stringify(doc));const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),h=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.character))),b=await fs.readFile(path.join(asset.directory,'character/animation.bin'));for(const m of h.morphs){assert.equal(m.stride,undefined);assert.ok(m.count>0);for(let r=0;r<m.count;r++){const at=m.offset+r*28;assert.ok([0,1,2].every(k=>b.readFloatLE(at+4+k*4)===0));near(Math.hypot(...[0,1,2].map(k=>b.readFloatLE(at+16+k*4))),Math.SQRT2);}}
}));

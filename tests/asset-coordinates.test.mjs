import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {coordinateTransform} from '../engine/assets/coordinates.mjs';
import {readGltf,readAccessor,nodeMatrix,multiplyMatrix} from '../engine/assets/gltf-document.mjs';
import {readStaticModel} from '../engine/assets/model-import.mjs';
import {importAnimation} from '../tools/offline/animation.mjs';
import {makeAnimationFixture} from '../tools/offline/animation-fixture.mjs';
import {encodeCentimeterZUp} from '../tools/offline/coordinate-fixture.mjs';
import {writeAssetFixture,writeCharacterFixture,writeCompressedAssetFixture} from '../tools/asset-fixtures.mjs';
import {importAsset,appendAnimation,verifyImport} from '../engine/assets/import-service.mjs';
import {main as cli} from '../tools/assets.mjs';
import {createEditorService} from '../editor/service.mjs';

export const centimeterZUp={unit:'cm',upAxis:'+Z',forwardAxis:'-Y'};
const near=(a,b,epsilon=.0003)=>{assert.equal(a.length,b.length);for(let i=0;i<a.length;i++)assert.ok(Math.abs(a[i]-b[i])<epsilon,`${a} != ${b}`);};
const point=(m,p)=>[0,1,2].map(i=>m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i]);
async function fixture(t){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-coordinates-'));t.after(()=>fs.rm(dir,{recursive:true,force:true}));return dir;}
async function save(dir,name,doc){const file=path.join(dir,name);await fs.writeFile(file,JSON.stringify(doc));return file;}
async function alternate(file,dir,name='centimeter.gltf'){return save(dir,name,encodeCentimeterZUp(await readGltf(file)));}
function palette(data,clip,frame) {
  const globals=[];return data.header.bones.map((bone,i)=>{
    const values=Array.from({length:10},(_,k)=>data.binary.readInt32LE(clip.offset+(frame*data.header.bones.length+i)*40+k*4)/10000),local=nodeMatrix({translation:values.slice(0,3),rotation:values.slice(3,7),scale:values.slice(7)});
    globals[i]=bone.parent<0?local:multiplyMatrix(globals[bone.parent],local);return multiplyMatrix(globals[i],bone.inverseBind);
  });
}
function vertex(data,clip,frame,index) {
  const matrices=palette(data,clip,frame),at=index*64,p=[0,1,2].map(i=>data.binary.readFloatLE(at+i*4));
  for(const [m,morph]of data.header.morphs.entries()){
    const weight=data.binary.readInt32LE(clip.morphOffset+(frame*data.header.morphs.length+m)*4)/10000;
    for(let record=0;record<morph.count;record++){const offset=morph.offset+record*28;if(data.binary.readUInt32LE(offset)===index)for(let i=0;i<3;i++)p[i]+=data.binary.readFloatLE(offset+4+i*4)*weight;}
  }
  const output=[0,0,0];for(let i=0;i<4;i++){const joint=data.binary.readUInt32LE(at+32+i*4),weight=data.binary.readFloatLE(at+48+i*4),v=point(matrices[joint],p);for(let k=0;k<3;k++)output[k]+=v[k]*weight;}return output;
}

test('basis conversion preserves default output and composes every signed perpendicular axis and handedness',()=>{
  const old=coordinateTransform();assert.equal(old.isDefault,true);assert.deepEqual(old.position([1,2,3]),[1,2,-3]);assert.deepEqual(old.rotation([.1,.2,.3,.9]),[-.1,-.2,.3,.9]);
  const axes=['+X','-X','+Y','-Y','+Z','-Z'],q=[.2,.3,.4,Math.sqrt(.71)];
  for(const upAxis of axes)for(const forwardAxis of axes)if(upAxis.at(-1)!==forwardAxis.at(-1))for(const handedness of ['right','left'])for(const yawDegrees of [0,35]) {
    const c=coordinateTransform({unit:'cm',upAxis,forwardAxis,handedness,yawDegrees}),m=nodeMatrix({translation:[2,3,4],rotation:q,scale:[2,2,2]}),converted=c.transformMatrix(m),local=[1,2,3];
    near(point(converted,c.position(local)),c.position(point(m,local)),1e-6);near(point(c.inverse,c.position(local)),local,1e-7);
    const rotated=nodeMatrix({rotation:c.rotation(q)}),expected=c.transformMatrix(nodeMatrix({rotation:q}));near(rotated,expected,1e-7);
    near(c.direction(axes.includes(upAxis)?['X','Y','Z'].map(key=>key===upAxis.at(-1)?(upAxis[0]==='-'?-1:1):0):[]),[0,1,0],1e-7);
    assert.equal(c.determinant,handedness==='right'?-1:1);
  }
  assert.equal(coordinateTransform({yawDegrees:360}).isDefault,true);assert.deepEqual(coordinateTransform({unit:'cm'}).config,coordinateTransform({metersPerUnit:.01}).config);
  assert.equal(coordinateTransform({handedness:'left',forwardAxis:'-Z'}).determinant,1);
});
test('coordinate options reject ambiguous, nonfinite or unsupported inputs',()=>{
  for(const options of [null,[],{unknown:1},{unit:'yards'},{metersPerUnit:0},{metersPerUnit:Infinity},{metersPerUnit:'1'},{unit:'cm',metersPerUnit:.01},{upAxis:'W'},{upAxis:'+Z',forwardAxis:'-Z'},{handedness:'auto'},{yawDegrees:NaN},{yawDegrees:3601}])assert.throws(()=>coordinateTransform(options));
});
test('static models and OBJ apply units, axes, normals and winding before generating draw and collision extents',async t=>{
  const dir=await fixture(t),file=await writeAssetFixture(dir),other=await alternate(file,dir),a=await readStaticModel(file),b=await readStaticModel(other,{coordinates:centimeterZUp});
  a.primitives.forEach((primitive,i)=>{primitive.positions.forEach((p,j)=>near(p,b.primitives[i].positions[j]));primitive.normals.forEach((n,j)=>near(n,b.primitives[i].normals[j]));assert.deepEqual(primitive.indices,b.primitives[i].indices);});
  const x=await importAsset({source:file,output:path.join(dir,'assets/imported'),size:64}),y=await importAsset({source:other,output:path.join(dir,'assets/imported'),size:64,coordinates:centimeterZUp});
  x.manifest.parts.forEach((part,i)=>{near(part.position,y.manifest.parts[i].position);near(part.scale,y.manifest.parts[i].scale);});await verifyImport(y.directory);
  const obj=path.join(dir,'triangle.obj');await fs.writeFile(obj,'v 0 0 1\nv 1 0 1\nv 0 1 1\nvn 0 0 1\nf 1//1 2//1 3//1\n');
  const left=await readStaticModel(obj,{coordinates:{handedness:'left',forwardAxis:'-Z',unit:'cm'}});near(left.primitives[0].positions[0],[0,0,.01]);near(left.primitives[0].normals[0],[0,0,1]);assert.deepEqual(left.primitives[0].indices,[0,1,2]);
});
test('merged skins, inverse binds, animation samples and morph deltas produce the same metre-space posed vertices',async t=>{
  const dir=await fixture(t),file=await writeCharacterFixture(dir),other=await alternate(file,dir),base=await importAnimation(file,{normalize:true}),changed=await importAnimation(other,{normalize:true,coordinates:centimeterZUp});
  const encoded=await readGltf(other);for(const [index,accessor]of encoded.document.accessors.entries())if(accessor.min&&accessor.max)for(const row of readAccessor(encoded,index,accessor.type))row.forEach((value,i)=>assert.ok(value>=accessor.min[i]&&value<=accessor.max[i]));
  assert.equal(base.header.bones.length,changed.header.bones.length);assert.equal(changed.header.coordinates.metersPerUnit,.01);
  base.header.bones.forEach((bone,i)=>{near(bone.rest,changed.header.bones[i].rest);near(bone.inverseBind,changed.header.bones[i].inverseBind);});
  for(let i=0;i<base.header.clips.length;i++)for(const frame of [0,15,30,60])for(const index of [0,25,100,base.header.mesh.vertices-1])near(vertex(base,base.header.clips[i],frame,index),vertex(changed,changed.header.clips[i],frame,index),.0005);
  const scaled=await importAnimation(file,{normalize:true,coordinates:{unit:'cm'}});near(scaled.header.bones[1].rest.slice(0,3),base.header.bones[1].rest.slice(0,3).map(v=>v*.01));
  assert.throws(()=>coordinateTransform({upAxis:'Y',forwardAxis:'Y'}));await assert.rejects(importAnimation(file,{coordinates:{unit:'cm'}}),/normalized/);
});
test('rig, soft regions, velocities and cubic quaternion tangents share the same spatial conversion',async t=>{
  const dir=await fixture(t),doc=makeAnimationFixture({advanced:true});doc.animations[0].extras.nativegpu.motion.turnRate=.4;
  const file=await save(dir,'advanced.gltf',doc),other=await alternate(file,dir),base=await importAnimation(file,{normalize:true}),changed=await importAnimation(other,{normalize:true,coordinates:centimeterZUp});
  base.header.rig.legs.forEach((leg,i)=>{near(leg.pole,changed.header.rig.legs[i].pole);near([leg.soleHeight],[changed.header.rig.legs[i].soleHeight]);});
  base.header.softRegions.forEach((region,i)=>{near(region.center,changed.header.softRegions[i].center);near([region.radius,region.maxOffset],[changed.header.softRegions[i].radius,changed.header.softRegions[i].maxOffset]);});
  base.header.clips.forEach((clip,i)=>{if(clip.velocity){near(clip.velocity,changed.header.clips[i].velocity);near([clip.turnRate],[changed.header.clips[i].turnRate]);}});
  const cubic=structuredClone(doc),anim=cubic.animations[0],channel=anim.channels.find(c=>c.target.path==='rotation'),sampler=anim.samplers[channel.sampler],parsed=await readGltf(file);
  const times=readAccessor(parsed,sampler.input,'SCALAR'),keys=readAccessor(parsed,sampler.output,'VEC4'),values=keys.flatMap(q=>[[.1,.2,.3,0],q,[.1,.2,.3,0]]),bytes=Buffer.alloc(values.length*16);values.flat().forEach((v,i)=>bytes.writeFloatLE(v,i*4));
  const buffer=cubic.buffers.length,view=cubic.bufferViews.length,accessor=cubic.accessors.length;cubic.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});cubic.bufferViews.push({buffer,byteLength:bytes.length});cubic.accessors.push({bufferView:view,type:'VEC4',componentType:5126,count:times.length*3});sampler.output=accessor;sampler.interpolation='CUBICSPLINE';
  const cfile=await save(dir,'cubic.gltf',cubic),cother=await alternate(cfile,dir,'cubic-cm.gltf'),a=await importAnimation(cfile,{normalize:true}),b=await importAnimation(cother,{normalize:true,coordinates:centimeterZUp});
  for(const frame of [0,13,30,47,60])near(vertex(a,a.header.clips[0],frame,10),vertex(b,b.header.clips[0],frame,10),.0005);
});
test('standalone centimetre Z-up motion retargets exactly without changing target geometry or applying units twice',async t=>{
  const dir=await fixture(t),doc=makeAnimationFixture(),file=await save(dir,'target.gltf',doc),base=await importAsset({source:file,output:path.join(dir,'assets/imported'),size:64});
  doc.nodes.pop();delete doc.meshes;delete doc.skins;doc.scenes[0].nodes=[0];doc.animations=[doc.animations.find(a=>a.name==='walk')];
  const motion=await save(dir,'walk.gltf',doc),other=await alternate(motion,dir),a=await appendAnimation({directory:base.directory,source:motion,options:{prefix:'axis_',mode:'exact'}}),b=await appendAnimation({directory:base.directory,source:other,options:{prefix:'axis_',mode:'exact',coordinates:centimeterZUp}});
  const binaryA=await fs.readFile(path.join(a.directory,'character/animation.bin')),binaryB=await fs.readFile(path.join(b.directory,'character/animation.bin'));assert.deepEqual(binaryA,binaryB);
  const report=JSON.parse(await fs.readFile(path.join(b.directory,'character/retarget.json'),'utf8'));near([report.translationScale],[1]);assert.equal(report.coordinates.upAxis,'+Z');
});
test('CLI and editor propagate canonical options, while explicit defaults preserve package identity',async t=>{
  const dir=await fixture(t),file=await writeAssetFixture(dir),other=await alternate(file,dir),output=path.join(dir,'assets/imported');
  const base=await importAsset({source:file,output,size:64}),same=await importAsset({source:file,output,size:64,coordinates:{unit:'m',upAxis:'y',forwardAxis:'z',yawDegrees:360}});assert.equal(base.manifest.id,same.manifest.id);
  const options=await save(dir,'import-options.json',{size:64,coordinates:centimeterZUp}),result=await cli(['import','--in',other,'--out',output,'--options',options]);assert.equal(result.manifest.identity.options.coordinates.metersPerUnit,.01);
  const service=createEditorService({root:dir});t.after(()=>service.close());const imported=await service.call('asset_import',{source:other,size:64,coordinates:{metersPerUnit:.01,upAxis:'z',forwardAxis:'-y'}});assert.equal(imported.ok,true,JSON.stringify(imported));assert.equal(imported.result.manifest.id,result.manifest.id);
  await fs.writeFile(options,JSON.stringify({unknown:true}));await assert.rejects(cli(['import','--in',file,'--out',output,'--options',options]),/Unknown import/);
});
test('decoded compressed geometry honors coordinate overrides and invalid settings never publish a package',async t=>{
  const dir=await fixture(t),file=await writeCompressedAssetFixture(dir),a=await importAsset({source:file,output:path.join(dir,'assets/imported'),size:64}),b=await importAsset({source:file,output:path.join(dir,'assets/imported'),size:64,coordinates:{unit:'cm',yawDegrees:90}});
  a.manifest.parts.forEach((part,i)=>{near(b.manifest.parts[i].position,[-part.position[2]*.01,part.position[1]*.01,part.position[0]*.01]);near(b.manifest.parts[i].scale,[part.scale[2]*.01,part.scale[1]*.01,part.scale[0]*.01]);});
  const output=path.join(dir,'bad');await assert.rejects(importAsset({source:file,output,coordinates:{upAxis:'Z',forwardAxis:'-Z'}}));assert.equal(await fs.stat(output).then(()=>true).catch(()=>false),false);
});

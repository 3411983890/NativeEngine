// Original, procedural test content. No downloaded character or licensed motion.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
export function makeAnimationFixture({advanced=false}={}) {
  const chunks=[], views=[], accessors=[];let length=0;
  function data(values,type,componentType=5126) {
    const size=componentType===5126?4:2, count={SCALAR:1,VEC2:2,VEC3:3,VEC4:4,MAT4:16}[type];
    while(length%4){chunks.push(Buffer.alloc(1));length++;}
    const bytes=Buffer.alloc(values.length*size);
    values.forEach((v,i)=>componentType===5126?bytes.writeFloatLE(v,i*4):bytes.writeUInt16LE(v,i*2));
    views.push({buffer:0,byteOffset:length,byteLength:bytes.length});chunks.push(bytes);length+=bytes.length;
    accessors.push({bufferView:views.length-1,componentType,count:values.length/count,type});return accessors.length-1;
  }
  const bones=[
    ['root',-1,[0,0,0]],['hips',0,[0,0.82,0]],['chest',1,[0,0.34,0]],['head',2,[0,0.43,0]],
    ['left_arm',2,[-0.4,0.24,0]],['left_forearm',4,[0,-0.30,0]],['left_hand',5,[0,-0.26,0]],
    ['right_arm',2,[0.4,0.24,0]],['right_forearm',7,[0,-0.30,0]],['right_hand',8,[0,-0.26,0]],
    ['left_thigh',1,[-0.15,-0.02,0]],['left_calf',10,[0,-0.34,0]],['left_foot',11,[0,-0.32,0]],
    ['right_thigh',1,[0.15,-0.02,0]],['right_calf',13,[0,-0.34,0]],['right_foot',14,[0,-0.32,0]],
  ];
  const globals=bones.map((b,i)=>b[1]<0?[...b[2]]:b[2].map((v,k)=>v+0));
  bones.forEach((b,i)=>{if(b[1]>=0)globals[i]=b[2].map((v,k)=>v+globals[b[1]][k]);});
  const nodes=bones.map(([name,parent,translation],i)=>({name,translation,children:bones.flatMap((b,j)=>b[1]===i?[j]:[])}));
  const inverse=data(globals.flatMap(p=>[1,0,0,0,0,1,0,0,0,0,1,0,-p[0],-p[1],-p[2],1]),'MAT4');
  const positions=[],normals=[],uvs=[],joints=[],weights=[],indices=[];
  // Cube surfaces are in bind mesh space, weighted to the articulated hierarchy.
  function box(bone,center,scale) {
    const faces=[[[0,0,-1],[-1,-1,-1],[-1,1,-1],[1,1,-1],[1,-1,-1]],
      [[0,0,1],[1,-1,1],[1,1,1],[-1,1,1],[-1,-1,1]],
      [[-1,0,0],[-1,-1,1],[-1,1,1],[-1,1,-1],[-1,-1,-1]],
      [[1,0,0],[1,-1,-1],[1,1,-1],[1,1,1],[1,-1,1]],
      [[0,1,0],[-1,1,-1],[-1,1,1],[1,1,1],[1,1,-1]],
      [[0,-1,0],[-1,-1,1],[-1,-1,-1],[1,-1,-1],[1,-1,1]]];
    for(const [normal,...corners] of faces) {
      const base=positions.length/3;
      corners.forEach((p,i)=>{positions.push(...p.map((v,k)=>globals[bone][k]+center[k]+v*scale[k]/2));normals.push(...normal);
        uvs.push(i>=2?1:0,i===0||i===3?1:0);joints.push(bone,0,0,0);weights.push(1,0,0,0);});
      indices.push(base,base+1,base+2,base,base+2,base+3);
    }
  }
  box(1,[0,-0.04,0],[0.48,0.2,0.30]);box(2,[0,0,0],[0.62,0.6,0.32]);box(3,[0,0,0],[0.38,0.39,0.35]);
  for(const b of [4,7])box(b,[0,-0.15,0],[0.18,0.3,0.18]);
  for(const b of [5,8])box(b,[0,-0.13,0],[0.15,0.26,0.15]);
  for(const b of [6,9])box(b,[0,0,0],[0.17,0.17,0.17]);
  for(const b of [10,13])box(b,[0,-0.17,0],[0.19,0.34,0.19]);
  for(const b of [11,14])box(b,[0,-0.16,0],[0.17,0.32,0.17]);
  for(const b of [12,15])box(b,[0,-0.06,-0.06],[0.21,0.13,0.32]);
  const primitive={attributes:{POSITION:data(positions,'VEC3'),NORMAL:data(normals,'VEC3'),TEXCOORD_0:data(uvs,'VEC2'),
    JOINTS_0:data(joints,'VEC4',5123),WEIGHTS_0:data(weights,'VEC4')},indices:data(indices,'SCALAR',5123)};
  nodes.push({name:'mannequin',mesh:0,skin:0});
  const animations=[];
  for(const [name,amplitude,loop] of [['idle',0,true],['walk',0.4,true],['run',0.75,true],['jump',0.3,false],['fall',0.2,true],['interact',0.7,false]]) {
    const times=Array.from({length:61},(_,i)=>i/60),input=data(times,'SCALAR'),samplers=[],channels=[];
    for(const b of [4,5,7,8,10,11,13,14]) {
      const values=times.flatMap(t=>{
        const side=[4,5,10,11].includes(b)?1:-1;
        let angle=Math.sin(t*Math.PI*2)*amplitude*side;
        if([4,5,7,8].includes(b))angle=-angle;
        if([5,8].includes(b))angle-=0.2;
        if([11,14].includes(b))angle=Math.max(angle,0)*0.65;
        if(name==='jump'||name==='fall')angle=b<10?-0.5:(b===11||b===14?0.8:-0.3);
        if(name==='interact')angle=b===7?-Math.sin(Math.PI*t)*1.3:b===8?-0.5:0;
        return [Math.sin(angle/2),0,0,Math.cos(angle/2)];
      });
      samplers.push({input,output:data(values,'VEC4'),interpolation:'LINEAR'});channels.push({sampler:samplers.length-1,target:{node:b,path:'rotation'}});
    }
    const notifies=name==='walk'||name==='run'?[{frame:15,name:'foot_left'},{frame:45,name:'foot_right'}]:name==='interact'?[{frame:30,name:'interaction_contact'}]:[];
    animations.push({name,samplers,channels,extras:{nativegpu:{loop,notifies}}});
  }
  let extras={};
  if(advanced){
    const targetNames=['left_bicep','right_bicep'];primitive.targets=[];
    for(const bone of [4,7]){
      const delta=positions.map(()=>0),normalDelta=positions.map(()=>0);
      for(let v=0;v<positions.length/3;v++)if(joints[v*4]===bone){const y=positions[v*3+1]-globals[bone][1];const bulge=Math.max(0,1-Math.abs(y+.15)/.17);
        for(const k of [0,2]){delta[v*3+k]=(positions[v*3+k]-globals[bone][k])*bulge*.24;normalDelta[v*3+k]=normals[v*3+k]*bulge*.04;}}
      primitive.targets.push({POSITION:data(delta,'VEC3'),NORMAL:data(normalDelta,'VEC3')});
    }
    const directions=[['walk_left',[-3,0,0]],['walk_right',[3,0,0]],['walk_back',[0,0,3]],['run_left',[-6,0,0]],['run_right',[6,0,0]]];
    for(const [name,velocity] of directions){const base=animations.find(a=>a.name===(name.startsWith('run')?'run':'walk'));animations.push({...base,name,extras:{nativegpu:{loop:true,notifies:base.extras.nativegpu.notifies,motion:{velocity}}}});}
    for(const a of animations){const n=a.extras.nativegpu;if(['idle','walk','run'].includes(a.name))n.motion={velocity:a.name==='idle'?[0,0,0]:[0,0,a.name==='walk'?-3:-6],contacts:a.name==='idle'?[[0,1],[0,1]]:[[0,.5],[.5,1]]};}
    extras={nativegpu:{rig:{pelvis:'hips',lookBone:'head',legs:[['left_thigh','left_calf','left_foot'],['right_thigh','right_calf','right_foot']].map(([root,joint,tip])=>({root,joint,tip,pole:[0,0,-1],soleHeight:.125}))},
      muscles:[{bone:'left_forearm',target:targetNames[0],angleStart:0,angleEnd:1.0},{bone:'right_forearm',target:targetNames[1],angleStart:0,angleEnd:1.0}],
      softRegions:[4,7].map(bone=>({bone:bones[bone][0],center:[bone===4?-.075:.075,-.15,0],radius:.08,maxOffset:.025,vertices:joints.flatMap((_,i)=>i%4===0&&joints[i]===bone?[i/4]:[])}))}};
    extras.targetNames=targetNames;
  }
  const binary=Buffer.concat(chunks);
  return {asset:{version:'2.0',generator:'nativegpu-original-animation-fixture'},scene:0,scenes:[{nodes:[0,nodes.length-1]}],nodes,
    meshes:[{primitives:[primitive],...(advanced?{extras:{targetNames:extras.targetNames}}:{})}],extras,skins:[{joints:bones.map((_,i)=>i),inverseBindMatrices:inverse,skeleton:0}],animations,
    accessors,bufferViews:views,buffers:[{byteLength:binary.length,uri:`data:application/octet-stream;base64,${binary.toString('base64')}`}]};
}
export async function writeAnimationFixture(out) {await fs.mkdir(path.dirname(out),{recursive:true});await fs.writeFile(out,`${JSON.stringify(makeAnimationFixture({advanced:true}))}\n`);return out;}
if(process.argv[1] && path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) console.log(await writeAnimationFixture(process.argv[2] ?? 'build/animation/mannequin.gltf'));

import fs from 'node:fs/promises';
import path from 'node:path';
import {requireAsset} from './gltf-document.mjs';
import {product} from './animation-sampling.mjs';

// BVH position channels are offsets from OFFSET, and rotation channels are
// composed in their declared order. Keep these semantics before glTF sampling.
export async function readBvhMotion(file) {
  const source=await fs.readFile(file);requireAsset(source.length>0&&source.length<=256*1024*1024,'BVH source exceeds 256 MiB','BUDGET');
  const tokens=source.toString('utf8').replace(/^\uFEFF/,'').match(/[{}]|[^\s{}]+/g)??[];let at=0;
  const next=()=>{requireAsset(at<tokens.length,'Truncated BVH source');return tokens[at++];};
  const expect=token=>requireAsset(next()===token,`Expected BVH ${token}`);
  const number=()=>{const value=Number(next());requireAsset(Number.isFinite(value)&&Math.abs(value)<=1e7,'Invalid BVH number');return value;};
  const integer=(low,high)=>{const value=number();requireAsset(Number.isInteger(value)&&value>=low&&value<=high,'Invalid BVH count','BUDGET');return value;};
  const nodes=[],channels=[],roots=[];
  function node(type,depth=0) {
    requireAsset(depth<256&&nodes.length<10000,'BVH hierarchy exceeds node/depth budget','BUDGET');
    const name=type==='End'?`__end_${nodes.length}`:next();if(type==='End')expect('Site');
    const i=nodes.length;nodes.push({name,translation:[0,0,0],children:[]});channels.push([]);expect('{');expect('OFFSET');nodes[i].translation=[number(),number(),number()];
    if(type!=='End') {
      expect('CHANNELS');const count=integer(0,6);channels[i]=Array.from({length:count},()=>next());
      requireAsset(new Set(channels[i]).size===count&&channels[i].every(c=>/^[XYZ](position|rotation)$/.test(c)),'Invalid or duplicated BVH channels');
      while(tokens[at]!=='}'){const childType=next();requireAsset(childType==='JOINT'||childType==='End','Expected BVH JOINT or End Site');nodes[i].children.push(node(childType,depth+1));}
    }
    expect('}');return i;
  }
  expect('HIERARCHY');while(tokens[at]==='ROOT'){next();roots.push(node('ROOT'));}requireAsset(roots.length>0,'BVH has no root');expect('MOTION');
  const label=name=>{const token=next();if(token===name+':')return;requireAsset(token===name,`Expected BVH ${name}`);expect(':');};
  label('Frames');const frames=integer(2,14401);expect('Frame');label('Time');const frameTime=number(),duration=(frames-1)*frameTime;
  requireAsset(frameTime>0&&duration>0&&duration<=120,'BVH duration must be in (0,120] seconds','BUDGET');
  const animated=nodes.flatMap((_,i)=>channels[i].length?[i]:[]);
  requireAsset(animated.length>0&&animated.length*frames*28<=64*1024*1024,'BVH decoded tracks exceed 64 MiB','BUDGET');
  const positions=new Map(animated.map(i=>[i,Buffer.alloc(frames*12)])),rotations=new Map(animated.map(i=>[i,Buffer.alloc(frames*16)]));
  for(let f=0;f<frames;f++)for(const i of animated) {
    const position=[...nodes[i].translation];let rotation=[0,0,0,1];
    for(const channel of channels[i]) {
      const value=number(),axis='XYZ'.indexOf(channel[0]);
      if(channel.endsWith('position'))position[axis]+=value;
      else {const angle=value*Math.PI/360,q=[0,0,0,Math.cos(angle)];q[axis]=Math.sin(angle);rotation=product(rotation,q);}
    }
    position.forEach((v,k)=>positions.get(i).writeFloatLE(v,(f*3+k)*4));rotation.forEach((v,k)=>rotations.get(i).writeFloatLE(v,(f*4+k)*4));
  }
  requireAsset(at===tokens.length,'Unexpected trailing BVH motion values');
  const doc={asset:{version:'2.0',generator:'nativegpu-bvh-motion'},scene:0,scenes:[{nodes:roots}],nodes,buffers:[],bufferViews:[],accessors:[],animations:[]},buffers=[];
  function append(bytes,type,count) {
    const index=doc.accessors.length,buffer=buffers.length,view=doc.bufferViews.length;buffers.push(bytes);doc.buffers.push({byteLength:bytes.length});
    doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,componentType:5126,type,count});return index;
  }
  const times=Buffer.alloc(frames*4);for(let i=0;i<frames;i++)times.writeFloatLE(i*frameTime,i*4);const input=append(times,'SCALAR',frames);
  const animation={name:'Motion',samplers:[],channels:[]};
  for(const i of animated)for(const [property,data,type]of [['translation',positions.get(i),'VEC3'],['rotation',rotations.get(i),'VEC4']]) {
    animation.channels.push({sampler:animation.samplers.length,target:{node:i,path:property}});animation.samplers.push({input,output:append(data,type,frames),interpolation:'LINEAR'});
  }
  doc.animations.push(animation);return {file:path.resolve(file),source,document:doc,buffers,dependencies:[]};
}

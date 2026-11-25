import {requireAsset,nodeMatrix,multiplyMatrix} from './gltf-document.mjs';

const point=(m,p)=>[0,1,2].map(i=>m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i]);
const vector=v=>Array.isArray(v)&&v.length===3&&v.every(Number.isFinite);
// Rest-pose skinning includes authored shape defaults and per-skin inverse binds.
// No animated frame or visual-only soft tissue is used as the collision authoring pose.
export function fitCharacterBounds(header,binary,{scale=[1,1,1],yaw=0,mode='bounds'}={}) {
  requireAsset(['bounds','humanoid'].includes(mode),'Fit mode must be bounds or humanoid','ARGUMENT');
  requireAsset(vector(scale)&&scale.every(v=>v>0&&v<=10000)&&Number.isFinite(yaw),'Invalid fit model transform','ARGUMENT');
  requireAsset(['nativegpu.animation/1','nativegpu.animation/2'].includes(header.format)&&header.binary==='animation.bin'&&header.binaryBytes===binary.length&&header.sampleRate===60&&header.quantization===10000,'Unsupported fitting animation payload','FORMAT');
  const mesh=header.mesh,bones=header.bones,morphs=header.morphs??[];
  requireAsset(mesh&&Number.isInteger(mesh.vertices)&&mesh.vertices>0&&mesh.vertices<=100000&&mesh.vertexOffset===0&&mesh.vertices*64<=binary.length&&Array.isArray(bones)&&bones.length>0&&bones.length<=256&&morphs.length<=16,'Invalid character geometry for fitting','FORMAT');
  const globals=[],palettes=bones.map((bone,i)=>{
    requireAsset(Number.isInteger(bone.parent)&&bone.parent>=-1&&bone.parent<i&&Array.isArray(bone.rest)&&bone.rest.length===10&&bone.rest.every(Number.isFinite)&&Array.isArray(bone.inverseBind)&&bone.inverseBind.length===16&&bone.inverseBind.every(Number.isFinite),'Invalid fitting skeleton');
    const local=nodeMatrix({translation:bone.rest.slice(0,3),rotation:bone.rest.slice(3,7),scale:bone.rest.slice(7)});globals[i]=bone.parent<0?local:multiplyMatrix(globals[bone.parent],local);return multiplyMatrix(globals[i],bone.inverseBind);
  });
  const delta=new Float64Array(mesh.vertices*3),defaults=header.defaultMorphWeights??morphs.map(()=>0);
  requireAsset(defaults.length===morphs.length&&defaults.every(v=>Number.isFinite(v)&&Math.abs(v)<=8),'Invalid fit morph defaults');
  for(const [index,morph]of morphs.entries()) {
    const stride=morph.stride??28;requireAsset([28,40].includes(stride)&&Number.isInteger(morph.count)&&morph.count>=0&&morph.count<=mesh.vertices&&Number.isInteger(morph.offset)&&morph.offset>=0&&morph.offset+morph.count*stride<=binary.length,'Invalid fitting morph range');
    for(let record=0;record<morph.count;record++){const at=morph.offset+record*stride,vertex=binary.readUInt32LE(at);requireAsset(vertex<mesh.vertices,'Invalid fitting morph vertex');for(let i=0;i<3;i++){const value=binary.readFloatLE(at+4+i*4);requireAsset(Number.isFinite(value),'Invalid fitting morph delta');delta[vertex*3+i]+=value*defaults[index];}}
  }
  const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity],c=Math.cos(yaw),s=Math.sin(yaw);
  for(let vertex=0;vertex<mesh.vertices;vertex++) {
    const at=vertex*64,p=[0,1,2].map(i=>binary.readFloatLE(at+i*4)+delta[vertex*3+i]),posed=[0,0,0];let total=0;
    requireAsset(p.every(Number.isFinite),'Invalid fitting vertex');
    for(let influence=0;influence<4;influence++){const joint=binary.readUInt32LE(at+32+influence*4),weight=binary.readFloatLE(at+48+influence*4);requireAsset(joint<palettes.length&&Number.isFinite(weight)&&weight>=0,'Invalid fitting skin influences');if(weight){const v=point(palettes[joint],p);for(let i=0;i<3;i++)posed[i]+=v[i]*weight;total+=weight;}}
    requireAsset(total>0&&Math.abs(total-1)<.02,'Invalid fitting skin weight sum');const x=posed[0]/total*scale[0],y=posed[1]/total*scale[1],z=posed[2]/total*scale[2],world=[c*x-s*z,y,s*x+c*z];
    for(let i=0;i<3;i++){requireAsset(Number.isFinite(world[i])&&Math.abs(world[i])<=100000,'Fit bounds exceed world limits','BUDGET');min[i]=Math.min(min[i],world[i]);max[i]=Math.max(max[i],world[i]);delta[vertex*3+i]=world[i];}
  }
  const size=min.map((v,i)=>Math.max(.001,max[i]-v));requireAsset(size.every(v=>v<=10000),'Fitted collider exceeds scene dimensions','BUDGET');
  const center=[(min[0]+max[0])*.5,(min[2]+max[2])*.5];let radial=0;for(let vertex=0;vertex<mesh.vertices;vertex++)radial=Math.max(radial,Math.hypot(delta[vertex*3]-center[0],delta[vertex*3+2]-center[1]));
  const radius=mode==='humanoid'?Math.max(.0005,Math.min(radial,size[1]/6)):Math.max(.0005,radial);
  const d=[-(min[0]+max[0])*.5,-min[1],-(min[2]+max[2])*.5],offset=[(c*d[0]+s*d[2])/scale[0],d[1]/scale[1],(-s*d[0]+c*d[2])/scale[2]];
  return {mode,bounds:{min,max},dimensions:mode==='humanoid'?[radius*2,size[1],radius*2]:size,offset,height:size[1],radius};
}

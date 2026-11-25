#!/usr/bin/env node
// Animation has a separate format: deforming geometry must never enter the static
// cluster package. See .developmentAgent/animation-audio/ANIMATION.md for the supported glTF subset.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { stableJSON } from './geometry-package.mjs';
import { readGltf, readAccessor } from '../../engine/assets/gltf-document.mjs';
import { normalizeCharacterSource } from '../../engine/assets/character-source.mjs';
import {coordinateTransform} from '../../engine/assets/coordinates.mjs';
import { representableUniformScale } from '../../engine/assets/animation-sampling.mjs';
import {generateMissingTangents} from '../../engine/assets/tangent-generation.mjs';
import {detectHumanoidRig, defaultRigSettings, inferFootContacts} from '../../engine/assets/humanoid-rig.mjs';

export class AnimationError extends Error {
  constructor(code, message) { super(message); this.code = `NG-E-ANIM-${code}`; }
}
const check = (ok, code, text) => { if (!ok) throw new AnimationError(code, text); };
const finite = v => typeof v === 'number' && Number.isFinite(v);
const integer = (v, lo, hi) => Number.isSafeInteger(v) && v >= lo && v <= hi;
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
export function binaryHash(bytes) {
  let h = 1469598103934665603n;
  for (const v of bytes) h = BigInt.asUintN(64, (h ^ BigInt(v)) * 1099511628211n);
  return h.toString(16).padStart(16, '0');
}
const quat = q => {
  const n = Math.hypot(...q); check(q.length === 4 && q.every(finite) && n > 1e-8, 'POSE', 'Invalid quaternion');
  return q.map(v => v / n);
};
function slerp(a, b, w) {
  a = quat(a); b = quat(b); let d = a.reduce((s, v, i) => s + v * b[i], 0);
  if (d < 0) { b = b.map(v => -v); d = -d; }
  let x = 1 - w, y = w;
  if (d < 0.9995) { const t = Math.acos(Math.max(-1, Math.min(1, d))); x = Math.sin((1-w)*t)/Math.sin(t); y = Math.sin(w*t)/Math.sin(t); }
  return quat(a.map((v, i) => v * x + b[i] * y));
}
const identity = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
function accessor(doc, buffers, index, type, allowed) {
  if(doc.accessors?.[index]?.sparse) {
    check(doc.accessors[index].type===type&&allowed.includes(doc.accessors[index].componentType),'ACCESSOR','Unsupported accessor');
    try {return {values:readAccessor({document:doc,buffers:buffers.map(b=>b.bytes)},index,type),accessor:doc.accessors[index]};}
    catch(error) {throw new AnimationError('ACCESSOR',error.message);}
  }
  check(integer(index, 0, (doc.accessors?.length ?? 0)-1), 'ACCESSOR', `Missing accessor ${index}`);
  const a = doc.accessors[index];
  check(a && a.type === type && allowed.includes(a.componentType) && !a.sparse, 'ACCESSOR', `Unsupported accessor ${index}`);
  const n = {SCALAR:1,VEC2:2,VEC3:3,VEC4:4,MAT4:16}[type];
  const size = {5121:1,5123:2,5125:4,5126:4}[a.componentType];
  const v = doc.bufferViews?.[a.bufferView], b = buffers[v?.buffer];
  check(v && b && integer(a.count, 1, 1000000), 'ACCESSOR', `Invalid view/count ${index}`);
  check(integer(v.byteOffset ?? 0, 0, b.byteLength) && integer(v.byteLength, 0, b.byteLength) && (v.byteOffset ?? 0)+v.byteLength <= b.byteLength,
    'ACCESSOR', 'Buffer view exceeds declared buffer');
  const offset = a.byteOffset ?? 0, stride = v.byteStride ?? n*size, start = (v.byteOffset ?? 0)+offset;
  check(integer(offset,0,v.byteLength) && integer(stride,n*size,252) && stride%size === 0 && start%size === 0 && offset+(a.count-1)*stride+n*size <= v.byteLength,
    'ACCESSOR', `Accessor ${index} exceeds view or is misaligned`);
  check(!a.normalized || a.componentType === 5121 || a.componentType === 5123, 'ACCESSOR', 'Invalid normalization');
  const values = [];
  for (let i=0;i<a.count;i++) {
    const value=[];
    for (let k=0;k<n;k++) {
      const at=start+i*stride+k*size;
      let f = a.componentType===5126 ? b.bytes.readFloatLE(at) : size===1 ? b.bytes.readUInt8(at) : size===2 ? b.bytes.readUInt16LE(at) : b.bytes.readUInt32LE(at);
      if (a.normalized) f /= size===1 ? 255 : 65535;
      check(finite(f), 'ACCESSOR', `Non-finite value in ${index}`); value.push(f);
    }
    values.push(value);
  }
  return {values, accessor:a};
}
function transform(node) {
  check(node && !node.matrix && !node.extensions, 'UNSUPPORTED', 'Only node TRS is supported');
  const t=node.translation ?? [0,0,0], r=quat(node.rotation ?? [0,0,0,1]), s=node.scale ?? [1,1,1];
  check(t.length===3 && t.every(v => finite(v) && Math.abs(v)<=1000) && s.length===3 && s.every(v => finite(v) && v>0 && v<=100), 'POSE', 'Invalid node TRS');
  // Uniform joint scale keeps the palette normal transform well-defined in VS.
  check(representableUniformScale(s), 'UNSUPPORTED', 'Nonuniform joint scale is not supported');
  const common=s.reduce((a,b)=>a+b,0)/3;return [...t,...r,common,common,common];
}
export async function importAnimation(input, {normalize = false,coordinates} = {}) {
  const conversion=coordinateTransform(coordinates);check(normalize||conversion.isDefault,'UNSUPPORTED','Coordinate overrides require normalized character import');
  const original = await readGltf(input), parsed = normalize ? normalizeCharacterSource(await generateMissingTangents(original),{coordinates}) : original;
  const {source,document:doc} = parsed;
  check(doc.asset?.version==='2.0' && (!doc.extensionsUsed?.length) && (!doc.extensionsRequired?.length) && !doc.extensions,
    'UNSUPPORTED', 'Only core glTF 2.0 is supported');
  check(doc.skins?.length===1 && doc.scenes?.length===1 && (doc.scene ?? 0)===0, 'UNSUPPORTED', 'Exactly one scene and skin required');
  check(Array.isArray(doc.nodes) && doc.nodes.length<=(normalize?257:256), 'SKELETON', 'Character node budget exceeded');
  const parents=doc.nodes.map(() => -1), reachable=new Set(), active=new Set(), ordered=[];
  for (let i=0;i<doc.nodes.length;i++) {
    transform(doc.nodes[i]);
    const children=doc.nodes[i].children ?? []; check(Array.isArray(children), 'SKELETON', 'Invalid children');
    for (const c of children) { check(integer(c,0,doc.nodes.length-1) && parents[c]===-1 && c!==i, 'SKELETON', 'Invalid or multiply-parented node'); parents[c]=i; }
  }
  function walk(i) {
    check(integer(i,0,doc.nodes.length-1) && !active.has(i) && !reachable.has(i), 'SKELETON', 'Cycle or duplicate scene node');
    active.add(i);reachable.add(i);ordered.push(i);
    for (const c of doc.nodes[i].children ?? []) walk(c); active.delete(i);
  }
  check(Array.isArray(doc.scenes[0].nodes), 'SKELETON', 'Missing scene roots');
  for (const r of doc.scenes[0].nodes) {check(parents[r]===-1, 'SKELETON', 'Scene root has a parent');walk(r);}
  check(reachable.size===doc.nodes.length, 'UNSUPPORTED', 'Disconnected nodes are not supported');
  const meshNodes=ordered.filter(i => doc.nodes[i].mesh!==undefined);
  check(meshNodes.length===1 && doc.nodes[meshNodes[0]].skin===0, 'UNSUPPORTED', 'Need one skinned mesh node');
  const meshNode=doc.nodes[meshNodes[0]];
  check(transform(meshNode).every((v,i) => v===[0,0,0,0,0,0,1,1,1,1][i]) && parents[meshNodes[0]]===-1,
    'UNSUPPORTED', 'Skinned mesh node must be an identity scene root');
  const skin=doc.skins[0]; check(Array.isArray(skin.joints) && skin.joints.length>0 && skin.joints.length<=256 && new Set(skin.joints).size===skin.joints.length,
    'SKELETON', 'Invalid skin joints');
  const boneNodes=ordered.filter(i => i!==meshNodes[0]);
  check(boneNodes.length>0 && boneNodes.length<=256 && skin.joints.every(i => boneNodes.includes(i)), 'SKELETON', 'Missing joint');
  const remap=new Map(boneNodes.map((v,i)=>[v,i]));
  const buffers=parsed.buffers.map(bytes=>({bytes,byteLength:bytes.length}));
  const inverse=skin.inverseBindMatrices===undefined ? skin.joints.map(()=>identity) : accessor(doc,buffers,skin.inverseBindMatrices,'MAT4',[5126]).values;
  check(inverse.length===skin.joints.length,'SKELETON','Inverse bind count mismatch');
  const bones=boneNodes.map(i=>({name:doc.nodes[i].name ?? `bone_${i}`,parent:parents[i]<0?-1:remap.get(parents[i]),
    rest:transform(doc.nodes[i]),inverseBind:skin.joints.includes(i)?inverse[skin.joints.indexOf(i)]:identity}));
  check(bones.filter(b=>b.parent===-1).length===1 && new Set(bones.map(b=>b.name)).size===bones.length,
    'SKELETON','Need one skeleton root and unique names');
  const primitives=doc.meshes?.[meshNode.mesh]?.primitives;
  check(primitives?.length===1, 'UNSUPPORTED', 'One triangle primitive is supported');
  const prim=primitives[0]; check((prim.mode ?? 4)===4 && !prim.extensions, 'UNSUPPORTED', 'Need core TRIANGLES');
  const attrs=prim.attributes;
  check(attrs && Object.keys(attrs).every(k=>['POSITION','NORMAL','TEXCOORD_0','JOINTS_0','WEIGHTS_0',...(parsed.hasVertexColors?['COLOR_0']:[]),...(parsed.hasTangents?['TANGENT']:[]),...(parsed.hasLayerUvs?Array.from({length:3},(_,i)=>`_NATIVEGPU_LAYERUV_${i}`):[]),...(parsed.hasTextureUvs?Array.from({length:4},(_,i)=>`_NATIVEGPU_UV_${i}`):[])].includes(k)), 'UNSUPPORTED','Unsupported vertex attributes');
  const pos=accessor(doc,buffers,attrs.POSITION,'VEC3',[5126]).values;
  const norm=accessor(doc,buffers,attrs.NORMAL,'VEC3',[5126]).values;
  const uv=attrs.TEXCOORD_0===undefined ? pos.map(()=>[0,0]) : accessor(doc,buffers,attrs.TEXCOORD_0,'VEC2',[5126]).values;
  const slotUvs=parsed.hasTextureUvs?Array.from({length:4},(_,i)=>accessor(doc,buffers,attrs[`_NATIVEGPU_UV_${i}`],'VEC2',[5126]).values):[];
  const colors=parsed.hasVertexColors?accessor(doc,buffers,attrs.COLOR_0,'VEC4',[5126]).values:[];
  check(!parsed.hasVertexColors||colors.length===pos.length&&colors.every(row=>row.every(v=>v>=0&&v<=1)),'SKIN','Invalid vertex colors');
  check(slotUvs.every(stream=>stream.length===pos.length&&stream.every(row=>row.every(v=>Math.abs(v)<=1000000))),'SKIN','Invalid per-slot texture UV stream');
  const j=accessor(doc,buffers,attrs.JOINTS_0,'VEC4',[5121,5123]);check(!j.accessor.normalized,'SKIN','JOINTS_0 must not be normalized');
  const w=accessor(doc,buffers,attrs.WEIGHTS_0,'VEC4',[5121,5123,5126]);
  check(w.accessor.componentType===5126 || w.accessor.normalized,'SKIN','Integer WEIGHTS_0 must be normalized');
  check([norm,uv,j.values,w.values].every(a=>a.length===pos.length) && pos.length<=100000, 'SKIN','Vertex count mismatch or capacity exceeded');
  const indices=accessor(doc,buffers,prim.indices,'SCALAR',[5121,5123,5125]);
  check(!indices.accessor.normalized && indices.values.length%3===0 && indices.values.every(a=>a[0]<pos.length), 'SKIN','Invalid triangle indices');
  const vertices=Buffer.alloc(pos.length*64);
  for(let i=0;i<pos.length;i++) {
    const weights=w.values[i], sum=weights.reduce((s,v)=>s+v,0);
    check(weights.every(v=>v>=0) && sum>1e-8 && Math.abs(sum-1)<0.02 && j.values[i].every(k=>integer(k,0,skin.joints.length-1)), 'SKIN','Invalid skin weights/joints');
    check(Math.hypot(...norm[i])>1e-6,'SKIN','Zero normal');
    [...pos[i],...norm[i],...uv[i]].forEach((v,k)=>vertices.writeFloatLE(v,i*64+k*4));
    j.values[i].forEach((v,k)=>vertices.writeUInt32LE(remap.get(skin.joints[v]),i*64+32+k*4));
    weights.forEach((v,k)=>vertices.writeFloatLE(v/sum,i*64+48+k*4));
  }
  const indexBytes=Buffer.alloc(indices.values.length*4);indices.values.forEach((v,i)=>indexBytes.writeUInt32LE(v[0],i*4));
  const textureUvs=Buffer.alloc(parsed.hasTextureUvs?pos.length*32:0);for(let i=0;i<pos.length;i++)slotUvs.forEach((stream,slot)=>stream[i].forEach((v,k)=>textureUvs.writeFloatLE(v,i*32+slot*8+k*4)));
  const tangents=parsed.hasTangents?accessor(doc,buffers,attrs.TANGENT,'VEC4',[5126]).values:[],tangentBytes=Buffer.alloc(parsed.hasTangents?pos.length*16:0);check(!parsed.hasTangents||tangents.length===pos.length,'SKIN','Invalid tangent count');tangents.forEach((row,i)=>row.forEach((v,k)=>tangentBytes.writeFloatLE(v,i*16+k*4)));
  const colorBytes=Buffer.alloc(parsed.hasVertexColors?pos.length*16:0);colors.forEach((row,i)=>row.forEach((v,k)=>colorBytes.writeFloatLE(v,i*16+k*4)));
  const layerUvs=parsed.hasLayerUvs?Array.from({length:3},(_,i)=>accessor(doc,buffers,attrs[`_NATIVEGPU_LAYERUV_${i}`],'VEC2',[5126]).values):[];check(layerUvs.every(stream=>stream.length===pos.length&&stream.every(row=>row.every(v=>Math.abs(v)<=1000000))),'SKIN','Invalid coating UV stream');
  const layerUvBytes=Buffer.alloc(parsed.hasLayerUvs?pos.length*24:0);for(let i=0;i<pos.length;i++)layerUvs.forEach((stream,slot)=>stream[i].forEach((v,k)=>layerUvBytes.writeFloatLE(v,i*24+slot*8+k*4)));
  const chunks=[vertices,textureUvs,colorBytes,tangentBytes,layerUvBytes,indexBytes], clips=[];let offset=vertices.length+textureUvs.length+colorBytes.length+tangentBytes.length+layerUvBytes.length+indexBytes.length;
  const morphs=[];
  check(!prim.targets || (Array.isArray(prim.targets)&&prim.targets.length>0&&prim.targets.length<=16),'UNSUPPORTED','Need 1..16 morph targets when targets is present');
  for(const [i,target] of (prim.targets??[]).entries()) {
    check(target && Object.keys(target).length>0 && Object.keys(target).every(k=>['POSITION','NORMAL','TANGENT'].includes(k)),'UNSUPPORTED','Morph requires POSITION, NORMAL or TANGENT');
    check(target.TANGENT===undefined||attrs.TANGENT!==undefined,'SKIN','Tangent morph requires a base tangent');
    const delta=target.POSITION===undefined?pos.map(()=>[0,0,0]):accessor(doc,buffers,target.POSITION,'VEC3',[5126]).values;
    const normal=target.NORMAL===undefined?pos.map(()=>[0,0,0]):accessor(doc,buffers,target.NORMAL,'VEC3',[5126]).values;
    const tangent=target.TANGENT===undefined?pos.map(()=>[0,0,0]):accessor(doc,buffers,target.TANGENT,'VEC3',[5126]).values,stride=target.TANGENT===undefined?28:40;
    check(delta.length===pos.length&&normal.length===pos.length&&tangent.length===pos.length,'SKIN','Morph vertex count mismatch');
    const records=[];for(let vertex=0;vertex<pos.length;vertex++){
      check(Math.hypot(...delta[vertex])<=10&&Math.hypot(...normal[vertex])<=10&&Math.hypot(...tangent[vertex])<=10,'SKIN','Morph vector exceeds range');
      if([...delta[vertex],...normal[vertex],...tangent[vertex]].some(v=>v!==0))records.push({vertex,p:delta[vertex],n:normal[vertex],t:tangent[vertex]});
    }
    const bytes=Buffer.alloc(records.length*stride);records.forEach((r,k)=>{bytes.writeUInt32LE(r.vertex,k*stride);[...r.p,...r.n,...(stride===40?r.t:[])].forEach((v,j)=>{check(Math.abs(v)<=10,'SKIN','Morph delta exceeds range');bytes.writeFloatLE(v,k*stride+4+j*4);});});
    const name=doc.meshes[meshNode.mesh].extras?.targetNames?.[i]??`morph_${i}`;
    check(typeof name==='string'&&name.length>0&&!morphs.some(m=>m.name===name),'SKIN','Invalid morph name');
    morphs.push({name,offset,count:records.length,...(stride===40?{stride}:{} )});chunks.push(bytes);offset+=bytes.length;
  }
  const feature=doc.extras?.nativegpu??{};
  const importWarnings=[...(parsed.warnings??[])];
  const defaultMorphWeights=meshNode.weights??doc.meshes[meshNode.mesh].weights??morphs.map(()=>0);
  check(defaultMorphWeights.length===morphs.length&&defaultMorphWeights.every(v=>finite(v)&&Math.abs(v)<=8),'SKIN','Invalid default morph weights');
  const boneIndex=name=>{const index=bones.findIndex(b=>b.name===name);check(index>=0,'SKELETON',`Unknown rig bone ${name}`);return index;};
  const vec=v=>Array.isArray(v)&&v.length===3&&v.every(x=>finite(x)&&Math.abs(x)<1000);
  let minY=Infinity,maxY=-Infinity;for(const p of pos){minY=Math.min(minY,p[1]);maxY=Math.max(maxY,p[1]);}
  const meshHeight=Number.isFinite(minY)&&maxY>minY?maxY-minY:1.8;
  const autoRig=detectHumanoidRig({bones,positions:pos,joints:j.values,weights:w.values});
  const settingsOf=(input,fallback)=>{
    const out={...fallback};if(input===undefined)return out;
    check(input&&typeof input==='object'&&!Array.isArray(input),'SKELETON','Invalid rig settings');
    const numeric={height:[.1,10],turnThreshold:[.05,1.5],turnStepHeight:[0,.5],turnStepDuration:[.05,1.5],turnCooldown:[0,1],maxTwist:[.1,2.5],maxSlopeCosine:[.1,1],swingPrediction:[0,2],maxFootCorrection:[.01,2],pelvisLower:[-1,0],pelvisUpper:[0,1],maxStepHeight:[0,1]};
    for(const [key,[lo,hi]] of Object.entries(numeric)){if(input[key]===undefined)continue;check(finite(input[key])&&input[key]>=lo&&input[key]<=hi,'SKELETON',`Invalid rig setting ${key}`);out[key]=input[key];}
    for(const key of ['footLock','turnInPlace'])if(input[key]!==undefined){check(typeof input[key]==='boolean','SKELETON',`Invalid rig setting ${key}`);out[key]=input[key];}
    check(out.height>0&&out.turnStepDuration>0&&out.maxFootCorrection>0,'SKELETON','Invalid rig scale or timing');
    return out;
  };
  let rig;
  // One mapper for both chain kinds. A leg carries sole proportions, an arm does
  // not; everything else (hierarchy, bend limits, pole fallback) is shared so a
  // hand and a foot cannot drift into different validation rules.
  const mapChain=(c,pool,foot)=>{
    const root=boneIndex(c.root),joint=boneIndex(c.joint),tip=boneIndex(c.tip);
    const candidate=pool.find(x=>x.root===root&&x.joint===joint&&x.tip===tip);
    const chain={root,joint,tip,...(foot&&c.toe?{toe:boneIndex(c.toe)}:(foot&&candidate?.toe!==undefined?{toe:candidate.toe}:{})),pole:c.pole??candidate?.pole??[0,0,-1],minBend:c.minBend??0.01,maxBend:c.maxBend??2.9,...(foot?{soleHeight:c.soleHeight??candidate?.soleHeight??.06,footLength:c.footLength??candidate?.footLength??.15}:{})};
    check(bones[joint].parent===root&&bones[tip].parent===joint&&vec(chain.pole)&&finite(chain.minBend)&&finite(chain.maxBend)&&chain.minBend>=0&&chain.maxBend>chain.minBend&&chain.maxBend<Math.PI&&(chain.toe===undefined||bones[chain.toe].parent===tip),'SKELETON','Invalid rig chain');
    if(foot)check(finite(chain.soleHeight)&&chain.soleHeight>=0&&chain.soleHeight<.5&&finite(chain.footLength)&&chain.footLength>0&&chain.footLength<2,'SKELETON','Invalid rig chain');
    return chain;
  };
  if(feature.rig){
    const authored=feature.rig;
    check(authored&&typeof authored==='object'&&!Array.isArray(authored),'SKELETON','Invalid rig');
    rig={version:Number.isSafeInteger(authored.version)&&authored.version>=1&&authored.version<=100?authored.version:2,source:'authored',confidence:1,
      pelvis:boneIndex(authored.pelvis),lookBone:authored.lookBone?boneIndex(authored.lookBone):-1,spine:authored.spine?boneIndex(authored.spine):autoRig.spine,
      legs:(authored.legs??[]).map(c=>mapChain(c,autoRig.legs,true)),
      // An authored rig that names its legs but not its arms still inherits the
      // diagnosed arm chains: hand contacts must not depend on the author
      // remembering to list them. Those already carry bone indices.
      arms: authored.arms ? authored.arms.map(c=>mapChain(c,autoRig.arms,false)) : autoRig.arms,
      settings:settingsOf(authored.settings,defaultRigSettings(meshHeight)),diagnostics:{source:'authored',missing:[],warnings:[],ambiguous:[]}};
  }else{
    rig=autoRig;
    importWarnings.push(...autoRig.diagnostics.warnings);
  }
  check(Array.isArray(rig.legs)&&rig.legs.length<=2,'SKELETON','At most two leg chains');
  check(Array.isArray(rig.arms)&&rig.arms.length<=2,'SKELETON','At most two arm chains');
  const muscles=(feature.muscles??[]).map(m=>{const result={bone:boneIndex(m.bone),target:morphs.findIndex(t=>t.name===m.target),angleStart:m.angleStart??0,angleEnd:m.angleEnd??1.5,maxWeight:m.maxWeight??1};
    check(result.target>=0&&finite(result.angleStart)&&finite(result.angleEnd)&&result.angleEnd>result.angleStart&&finite(result.maxWeight)&&result.maxWeight>=0&&result.maxWeight<=1,'SKIN','Invalid muscle driver');return result;});
  const softRegions=(feature.softRegions??[]).map(r=>{const result={bone:boneIndex(r.bone),center:r.center,radius:r.radius??.12,maxOffset:r.maxOffset??.035,vertices:r.vertices};
    check(vec(result.center)&&finite(result.radius)&&result.radius>=.02&&result.radius<=1&&finite(result.maxOffset)&&result.maxOffset>0&&result.maxOffset<=.1&&Array.isArray(result.vertices)&&result.vertices.length>0&&new Set(result.vertices).size===result.vertices.length&&result.vertices.every(v=>integer(v,0,pos.length-1)),'SKIN','Invalid soft region');return result;});
  check(muscles.length<=64&&softRegions.length<=16,'SKIN','Feature budget exceeded');
  check(Array.isArray(doc.animations) && doc.animations.length>0 && doc.animations.length<=64, 'CLIP','Need 1..64 animations');
  for(const anim of doc.animations) {
    check(!anim.extensions && Array.isArray(anim.channels) && (anim.channels.length>0||anim.morphTracks?.length>0), 'CLIP','Empty animation');
    const seen=new Set();let duration=0;
    const tracks=[...anim.channels,...(normalize?anim.morphTracks??[]:[])].map(c=>{
      const bone=remap.get(c.target?.node), channel=c.target?.path, spec=anim.samplers?.[c.sampler];
      const targets=c.targets??(channel==='weights'&&c.target.node===meshNodes[0]?morphs.map((_,i)=>i):null);
      check((targets?.length>0||bone!==undefined&&['translation','rotation','scale'].includes(channel)) && spec && !c.extensions && !spec.extensions,
        'UNSUPPORTED','Only skeleton TRS and mesh morph-weight channels supported');
      const keys=targets?targets.map(i=>`morph:${i}`):[`${bone}:${channel}`];
      check(keys.every(key=>!seen.has(key)),'CLIP','Duplicate animation channel');keys.forEach(key=>seen.add(key));
      const times=accessor(doc,buffers,spec.input,'SCALAR',[5126]).values.map(a=>a[0]);
      check(times[0]>=0 && times.every((t,i)=>i===0 || t>times[i-1]), 'CLIP','Key times must increase');
      duration=Math.max(duration,times.at(-1));
      let values=accessor(doc,buffers,spec.output,targets?'SCALAR':channel==='rotation'?'VEC4':'VEC3',[5126]).values;
      const cubic=spec.interpolation==='CUBICSPLINE';
      check(values.length===times.length*(cubic?3:1)*(targets?.length??1) && ['LINEAR','STEP','CUBICSPLINE'].includes(spec.interpolation ?? 'LINEAR'),'UNSUPPORTED','Unsupported interpolation or key count');
      if(targets)values=Array.from({length:values.length/targets.length},(_,i)=>values.slice(i*targets.length,(i+1)*targets.length).map(v=>v[0]));
      if(channel==='rotation') values.filter((_,i)=>!cubic||i%3===1).forEach(quat);
      if(channel==='scale')check(values.filter((_,i)=>!cubic||i%3===1).every(representableUniformScale),'UNSUPPORTED','Only positive uniform scale tracks supported');
      return {bone,channel,targets,times,values,cubic,step:spec.interpolation==='STEP'};
    });
    check(duration>0 && duration<=120, 'CLIP','Animation duration must be in (0,120] seconds');
    const frames=Math.ceil(duration*60)+1;
    const bytes=Buffer.alloc(frames*bones.length*40);
    const morphBytes=Buffer.alloc(frames*morphs.length*4);
    check(offset+bytes.length+morphBytes.length<=64*1024*1024,'FORMAT','Animation package exceeds 64 MiB');
    for(let f=0;f<frames;f++) {
      const time=Math.min(f/60,duration), samples=bones.map(b=>[...b.rest]),morphSample=[...defaultMorphWeights];
      for(const t of tracks) {
        let a=0;while(a+1<t.times.length && t.times[a+1]<=time)a++;
        const b=Math.min(a+1,t.times.length-1), weight=t.step||a===b ? 0 : Math.max(0,Math.min(1,(time-t.times[a])/(t.times[b]-t.times[a])));
        let value;
        if(t.cubic) {
          const h=t.times[b]-t.times[a],u=weight,u2=u*u,u3=u2*u;
          value=t.values[a*3+1].map((v,k)=>(2*u3-3*u2+1)*v+(u3-2*u2+u)*h*t.values[a*3+2][k]+(-2*u3+3*u2)*t.values[b*3+1][k]+(u3-u2)*h*t.values[b*3][k]);
          if(t.channel==='rotation')value=quat(value);
        } else value=t.channel==='rotation'?slerp(t.values[a],t.values[b],weight):t.values[a].map((v,k)=>v+(t.values[b][k]-v)*weight);
        if(t.targets)value.forEach((v,k)=>morphSample[t.targets[k]]=v);
        else {const start=t.channel==='translation'?0:t.channel==='rotation'?3:7;value.forEach((v,k)=>samples[t.bone][start+k]=v);}
      }
      morphSample.forEach((v,i)=>{check(finite(v)&&Math.abs(v)<=8,'SKIN','Sampled morph weight exceeds [-8,8]');morphBytes.writeInt32LE(Math.round(v*10000),(f*morphs.length+i)*4);});
      samples.forEach((s,b)=>{const scale=s.slice(7);check(representableUniformScale(scale),'UNSUPPORTED','Sampled scale must stay positive and uniform');const common=scale.reduce((a,b)=>a+b,0)/3;s[7]=s[8]=s[9]=common;s.forEach((v,k)=>{const q=Math.round(v*10000);check(integer(q,-2147483648,2147483647),'CLIP','Quantized transform overflow');bytes.writeInt32LE(q,(f*bones.length+b)*40+k*4);});});
    }
    const name=anim.name ?? `clip_${clips.length}`;
    const notifies=anim.extras?.nativegpu?.notifies ?? [];
    check(Array.isArray(notifies) && notifies.length<=256 && notifies.every(n=>integer(n.frame,0,frames-1)&&typeof n.name==='string'&&n.name.length>0), 'CLIP','Invalid notify metadata');
    const loop=anim.extras?.nativegpu?.loop ?? true;check(typeof loop==='boolean','CLIP','loop must be boolean');
    const stepChannels=Array(bones.length).fill(0);
    let stepMorphMask=0;
    for(const track of tracks)if(track.step){if(track.targets)for(const i of track.targets)stepMorphMask|=1<<i;else stepChannels[track.bone]|={translation:1,rotation:2,scale:4}[track.channel];}
    const motion=anim.extras?.nativegpu?.motion;
    if(motion)check(typeof (motion.matchable??true)==='boolean'&&vec(motion.velocity??[0,0,0])&&finite(motion.turnRate??0)&&Math.abs(motion.turnRate??0)<=20&&integer(motion.tags??1,1,65535)&&(motion.contacts===undefined||Array.isArray(motion.contacts)&&motion.contacts.length===2&&motion.contacts.every(r=>Array.isArray(r)&&r.length===2&&r.every(v=>finite(v)&&v>=0&&v<=1))),'CLIP','Invalid motion metadata');
    const inferred=motion?.contacts===undefined?inferFootContacts({bones,rig,binary:bytes,frameCount:frames,boneCount:bones.length,loop}):null;
    const contacts=motion?.contacts??inferred?.contacts??[[0,0],[0,0]];
    const contactConfidence=motion?.contacts?1:(inferred?.confidence??0);
    const contactSource=motion?.contacts?'authored':(inferred?'inferred':'unknown');
    check(contacts.length===2&&contacts.every(r=>Array.isArray(r)&&r.length===2&&r.every(v=>finite(v)&&v>=0&&v<=1))&&finite(contactConfidence)&&contactConfidence>=0&&contactConfidence<=1,'CLIP','Invalid contact metadata');
    const metadata={contacts,contactConfidence,contactSource,...(inferred?{locomotionSpeed:inferred.locomotionSpeed}:{}),...(motion?{matchable:motion.matchable??true,velocity:motion.velocity??[0,0,0],turnRate:motion.turnRate??0,tags:motion.tags??1}:{})};
    check(finite(metadata.locomotionSpeed??0)&&(metadata.locomotionSpeed??0)>=0&&(metadata.locomotionSpeed??0)<=50,'CLIP','Invalid locomotion speed');
    clips.push({name,frames,offset,loop,stepChannels,...(morphs.length?{morphOffset:offset+bytes.length,morphTargets:morphs.length,stepMorphMask}:{}),...metadata,notifies:[...notifies].sort((a,b)=>a.frame-b.frame || (a.name<b.name?-1:a.name>b.name?1:0))});
    chunks.push(bytes,morphBytes);offset+=bytes.length+morphBytes.length;
  }
  check(new Set(clips.map(c=>c.name)).size===clips.length,'CLIP','Duplicate clip names');
  const binary=Buffer.concat(chunks);
  const header={format:'nativegpu.animation/2',sampleRate:60,quantization:10000,sourceSha256:sha256(source),
    binary:'animation.bin',binaryBytes:binary.length,binaryHash:binaryHash(binary),bones,clips,
    morphs,rig,muscles,softRegions,...(morphs.length?{defaultMorphWeights}:{}),mesh:{vertexOffset:0,vertices:pos.length,indexOffset:vertices.length+textureUvs.length+colorBytes.length+tangentBytes.length+layerUvBytes.length,...(parsed.hasLayerUvs?{layerUvOffset:vertices.length+textureUvs.length+colorBytes.length+tangentBytes.length,layerUvBytes:layerUvBytes.length}:{}),...(parsed.hasTangents?{tangentOffset:vertices.length+textureUvs.length+colorBytes.length,tangentBytes:tangentBytes.length}:{}),indices:indices.values.length,...(parsed.hasVertexColors?{colorOffset:vertices.length+textureUvs.length,colorBytes:colorBytes.length}:{}),...(parsed.hasTextureUvs?{textureUvOffset:vertices.length,textureUvBytes:textureUvs.length}:{})}};
  if(parsed.parts) {header.mesh.parts=parsed.parts;header.coordinateSystem='left-handed-y-up';header.rootMotion=parsed.rootMotion;}
  if(importWarnings.length)header.importWarnings=importWarnings;
  if(!conversion.isDefault)header.coordinates=conversion.config;
  const text=`${stableJSON(header)}\n`;
  check(Buffer.byteLength(text)<=4*1024*1024,'FORMAT','Animation header exceeds runtime 4 MiB limit');
  return {header,text,binary,contentHash:sha256(Buffer.concat([Buffer.from(text),binary]))};
}
export async function exportAnimation(input,out,options={}) {
  const result=await importAnimation(input,options);await fs.mkdir(out,{recursive:true});
  await fs.writeFile(path.join(out,'animation.bin'),result.binary);
  await fs.writeFile(path.join(out,'character.json'),result.text);
  return {header:path.resolve(out,'character.json'),contentHash:result.contentHash,bones:result.header.bones.length,clips:result.header.clips.length,bytes:result.binary.length,parts:result.header.mesh.parts??[],warnings:result.header.importWarnings??[]};
}
async function main(args) {
  check(args[0]==='import' && args.length===5 && args[1]==='--in' && args[3]==='--out','ARGUMENT','Usage: node tools/offline/animation.mjs import --in character.gltf --out build/animation');
  return exportAnimation(args[2],args[4]);
}
if(process.argv[1] && path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).then(r=>console.log(JSON.stringify({ok:true,result:r}))).catch(e=>{console.error(JSON.stringify({ok:false,error:{code:e.code ?? 'NG-E-ANIM-IO',message:e.message}}));process.exitCode=1;});
}

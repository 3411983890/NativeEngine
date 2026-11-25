// Versioned authoring and CPU reference contract for layered PBR materials.
// This module does not publish a renderer feature: callers must only advertise
// layered rendering after the native loader and shader consume this contract.
import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { normalizeTextureMapping } from './texture-mapping.mjs';

export const LAYERED_SCHEMA = 'nativegpu.layered-material/1';
export const LAYERED_SCHEMA_V2 = 'nativegpu.layered-material/2';
export const LAYERED_COMPILER_VERSION = 'layered-pbr/1';
export const LAYERED_MAX_LAYERS = 4;
export const SURFACE_RELIEF_MODES = Object.freeze(['off','pom','bakedDisplacement']);
export const LAYER_TEXTURE_SLOTS = Object.freeze(['albedo','normal','orm','roughness','metallic','occlusion','emissive','height']);
const SLOT_SEMANTIC = Object.freeze({albedo:'albedo',normal:'normal',orm:'orm',roughness:'roughness',metallic:'metallic',occlusion:'occlusion',emissive:'emissive',height:'height',mask:'mask'});
const namePattern = /^[A-Za-z0-9_.-]{1,64}$/;
const hashPattern = /^[a-f0-9]{64}$/;
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const clamp = value => Math.max(0, Math.min(1, value));

export class LayeredMaterialError extends Error {
  constructor(path, message) { super(`${path}: ${message}`); this.name='LayeredMaterialError'; this.code='NG-E-MAT-LAYER'; this.path=path; }
}
function fail(path,message) { throw new LayeredMaterialError(path,message); }
function keys(value,allowed,path) {
  if(!object(value))fail(path,'must be an object');
  for(const key of Object.keys(value))if(!allowed.includes(key))fail(`${path}.${key}`,'unsupported field');
}
function number(value,path,min,max) {
  if(typeof value!=='number'||!Number.isFinite(value)||value<min||value>max)fail(path,`must be finite in [${min}, ${max}]`);
  return value;
}
function vector(value,path,length,min,max) {
  if(!Array.isArray(value)||value.length!==length)fail(path,`must contain ${length} numbers`);
  return value.map((v,i)=>number(v,`${path}[${i}]`,min,max));
}
function identifier(value,path) { if(typeof value!=='string'||!namePattern.test(value))fail(path,'must be 1–64 ASCII letters, digits, dots, hyphens or underscores');return value; }
function mapping(value,path) { try{return normalizeTextureMapping(value??{});}catch(error){fail(path,error.message);} }
function uv(value,path) {
  keys(value,['set','mapping'],path);
  const set=value.set??0;if(!Number.isSafeInteger(set)||set<0||set>15)fail(`${path}.set`,'must be an integer in [0, 15]');
  return {set,mapping:mapping(value.mapping,`${path}.mapping`)};
}
function texture(value,path,defaultUv) {
  keys(value,['name','uv','channel'],path);
  const result={name:identifier(value.name,`${path}.name`),uv:value.uv===undefined?structuredClone(defaultUv):uv(value.uv,`${path}.uv`)};
  if(value.channel!==undefined){if(!['r','g','b','a'].includes(value.channel))fail(`${path}.channel`,'must be r, g, b or a');result.channel=value.channel;}
  return result;
}
function blend(value,path,defaultUv) {
  value??={};keys(value,['mode','source','weight','texture','channel','invert','gain','bias','width','heightBias'],path);
  const mode=value.mode??'mask',source=value.source??'constant';
  if(!['mask','height'].includes(mode))fail(`${path}.mode`,'must be mask or height');
  if(!['constant','textureMask','vertexColor'].includes(source))fail(`${path}.source`,'must be constant, textureMask or vertexColor');
  if(source==='textureMask'&&value.texture===undefined)fail(`${path}.texture`,'textureMask requires a texture');
  if(source!=='textureMask'&&value.texture!==undefined)fail(`${path}.texture`,'only textureMask accepts a texture');
  const channel=value.channel??'r';if(!['r','g','b','a'].includes(channel))fail(`${path}.channel`,'must be r, g, b or a');
  if(value.invert!==undefined&&typeof value.invert!=='boolean')fail(`${path}.invert`,'must be boolean');
  return {mode,source,weight:number(value.weight??1,`${path}.weight`,0,1),channel,invert:value.invert??false,
    gain:number(value.gain??1,`${path}.gain`,0,1000),bias:number(value.bias??0,`${path}.bias`,-1000,1000),
    width:number(value.width??.2,`${path}.width`,.0001,1000),heightBias:number(value.heightBias??0,`${path}.heightBias`,-1000,1000),
    ...(value.texture===undefined?{}:{texture:texture(value.texture,`${path}.texture`,defaultUv)})};
}
function integer(value,path,min,max) {
  if(!Number.isSafeInteger(value)||value<min||value>max)fail(path,`must be an integer in [${min}, ${max}]`);
  return value;
}
function composite(value,path) {
  keys(value,['resolution','groundMetres','baseLayer','overlayLayer','overlayMaskLayer'],path);
  return {resolution:integer(value.resolution??2048,`${path}.resolution`,16,8192),
    groundMetres:number(value.groundMetres??16,`${path}.groundMetres`,.25,4096),
    baseLayer:identifier(value.baseLayer,`${path}.baseLayer`),
    overlayLayer:identifier(value.overlayLayer,`${path}.overlayLayer`),
    overlayMaskLayer:value.overlayMaskLayer===undefined?null:identifier(value.overlayMaskLayer,`${path}.overlayMaskLayer`)};
}
function displacementBake(value,path) {
  value??={};keys(value,['tileMetres','cellsPerTile','lodLevels','maxErrorMetres','composite'],path);
  return {tileMetres:number(value.tileMetres??2,`${path}.tileMetres`,.25,64),
    cellsPerTile:integer(value.cellsPerTile??64,`${path}.cellsPerTile`,4,256),
    lodLevels:integer(value.lodLevels??2,`${path}.lodLevels`,1,8),
    maxErrorMetres:number(value.maxErrorMetres??.005,`${path}.maxErrorMetres`,0,.25),
    composite:value.composite===undefined||value.composite===null?null:composite(value.composite,`${path}.composite`)};
}
function heightSource(value,path,defaultUv) {
  keys(value,['kind','layer','texture'],path);
  const kind=value.kind??'layer';
  if(kind==='layer'){
    if(value.texture!==undefined)fail(`${path}.texture`,'only texture height sources accept a texture');
    return {kind,layer:identifier(value.layer,`${path}.layer`)};
  }
  if(kind==='texture'){
    if(value.layer!==undefined)fail(`${path}.layer`,'only layer height sources accept a layer');
    if(value.texture===undefined)fail(`${path}.texture`,'texture height source requires a texture');
    return {kind,texture:texture(value.texture,`${path}.texture`,defaultUv)};
  }
  fail(`${path}.kind`,'must be layer or texture');
}
export function defaultSurfaceRelief(layerId) {
  return {mode:'off',heightSource:{kind:'layer',layer:layerId},amplitudeMetres:0,referenceHeight:1,invert:false,
    minSteps:8,maxSteps:24,refineSteps:2,fadeStartMetres:6,fadeEndMetres:14,grazingFade:.1,selfShadow:false,shadowSteps:4,
    displacementBake:displacementBake(undefined,'$.surfaceRelief.displacementBake')};
}
function surfaceRelief(value,path,defaultUv) {
  if(value===undefined)fail(path,'is required for schema v2');
  keys(value,['mode','heightSource','amplitudeMetres','referenceHeight','invert','minSteps','maxSteps','refineSteps','fadeStartMetres','fadeEndMetres','grazingFade','selfShadow','shadowSteps','displacementBake'],path);
  const mode=value.mode??'off';
  if(!SURFACE_RELIEF_MODES.includes(mode))fail(`${path}.mode`,'must be off, pom or bakedDisplacement');
  if(value.invert!==undefined&&typeof value.invert!=='boolean')fail(`${path}.invert`,'must be boolean');
  if(value.selfShadow!==undefined&&typeof value.selfShadow!=='boolean')fail(`${path}.selfShadow`,'must be boolean');
  const minSteps=integer(value.minSteps??8,`${path}.minSteps`,1,128);
  const maxSteps=integer(value.maxSteps??24,`${path}.maxSteps`,1,256);
  if(maxSteps<minSteps)fail(`${path}.maxSteps`,'must be at least minSteps');
  const fadeStart=number(value.fadeStartMetres??6,`${path}.fadeStartMetres`,0,1000);
  const fadeEnd=number(value.fadeEndMetres??14,`${path}.fadeEndMetres`,0,1000);
  if(fadeEnd<fadeStart)fail(`${path}.fadeEndMetres`,'must be at least fadeStartMetres');
  return {mode,heightSource:heightSource(value.heightSource??{kind:'layer',layer:''},`${path}.heightSource`,defaultUv),
    amplitudeMetres:number(value.amplitudeMetres??0,`${path}.amplitudeMetres`,0,1),
    referenceHeight:number(value.referenceHeight??1,`${path}.referenceHeight`,0,1),
    invert:value.invert??false,minSteps,maxSteps,
    refineSteps:integer(value.refineSteps??2,`${path}.refineSteps`,0,8),
    fadeStartMetres:fadeStart,fadeEndMetres:fadeEnd,
    grazingFade:number(value.grazingFade??.1,`${path}.grazingFade`,0,1),
    selfShadow:value.selfShadow??false,shadowSteps:integer(value.shadowSteps??4,`${path}.shadowSteps`,1,32),
    displacementBake:displacementBake(value.displacementBake,`${path}.displacementBake`)};
}
/** Height in metres for a normalized height sample. Top-referenced by default. */
export function surfaceHeightMetres(height01,relief,{invert}={}) {
  number(height01,'height01',0,1);
  const inverted=invert??relief?.invert??false;
  const h=inverted?1-height01:height01;
  return (h-(relief?.referenceHeight??1))*(relief?.amplitudeMetres??0);
}
function layer(value,path) {
  keys(value,['id','name','enabled','uv','textures','baseColor','roughness','metallic','occlusionStrength','normalStrength','emissiveFactor','emissiveStrength','heightScale','heightOffset','blend'],path);
  const id=identifier(value.id,`${path}.id`),name=value.name??id;
  if(typeof name!=='string'||!name.trim()||name.length>128)fail(`${path}.name`,'must contain 1–128 characters');
  if(value.enabled!==undefined&&typeof value.enabled!=='boolean')fail(`${path}.enabled`,'must be boolean');
  const defaultUv=uv(value.uv??{},`${path}.uv`),textures=value.textures??{};keys(textures,LAYER_TEXTURE_SLOTS,`${path}.textures`);
  const normalizedTextures={};for(const [slot,ref] of Object.entries(textures))normalizedTextures[slot]=texture(ref,`${path}.textures.${slot}`,defaultUv);
  if(normalizedTextures.orm&&(normalizedTextures.roughness||normalizedTextures.metallic))fail(`${path}.textures`,'ORM cannot be combined with independent roughness or metallic maps');
  return {id,name,enabled:value.enabled??true,uv:defaultUv,textures:normalizedTextures,
    baseColor:vector(value.baseColor??[1,1,1],`${path}.baseColor`,3,0,1),
    roughness:number(value.roughness??1,`${path}.roughness`,0,1),metallic:number(value.metallic??0,`${path}.metallic`,0,1),
    occlusionStrength:number(value.occlusionStrength??1,`${path}.occlusionStrength`,0,1),normalStrength:number(value.normalStrength??1,`${path}.normalStrength`,0,8),
    emissiveFactor:vector(value.emissiveFactor??[1,1,1],`${path}.emissiveFactor`,3,0,1),emissiveStrength:number(value.emissiveStrength??1,`${path}.emissiveStrength`,0,1000000),
    heightScale:number(value.heightScale??1,`${path}.heightScale`,-1000,1000),heightOffset:number(value.heightOffset??0,`${path}.heightOffset`,-1000,1000),
    blend:blend(value.blend,`${path}.blend`,defaultUv)};
}

/** Validate and normalize without mutating the supplied authoring asset. */
export function normalizeLayeredMaterial(value) {
  if(!object(value))fail('$','must be an object');
  if(value.schema!==LAYERED_SCHEMA&&value.schema!==LAYERED_SCHEMA_V2)fail('$.schema',`must equal ${LAYERED_SCHEMA} or ${LAYERED_SCHEMA_V2}`);
  const v2=value.schema===LAYERED_SCHEMA_V2;
  keys(value,v2?['schema','name','layers','surfaceRelief']:['schema','name','layers'], '$');
  const name=identifier(value.name,'$.name');
  if(!Array.isArray(value.layers)||!value.layers.length||value.layers.length> LAYERED_MAX_LAYERS)fail('$.layers',`must contain 1–${LAYERED_MAX_LAYERS} layers`);
  const layers=value.layers.map((v,i)=>layer(v,`$.layers[${i}]`)),ids=new Set();
  for(const [i,item] of layers.entries()){if(ids.has(item.id))fail(`$.layers[${i}].id`,'must be unique');ids.add(item.id);}
  if(!layers[0].enabled)fail('$.layers[0].enabled','base layer must be enabled');
  if(layers[0].blend.mode!=='mask'||layers[0].blend.source!=='constant'||layers[0].blend.weight!==1||layers[0].blend.invert||layers[0].blend.gain!==1||layers[0].blend.bias!==0)fail('$.layers[0].blend','base layer must have full constant coverage');
  if(!v2)return {schema:LAYERED_SCHEMA,name,layers};
  const authored=value.surfaceRelief??fail('$.surfaceRelief','is required for schema v2');
  const relief=surfaceRelief({...authored,heightSource:authored.heightSource??{kind:'layer',layer:layers[0].id}},'$.surfaceRelief',uv({},'uv'));
  if(relief.heightSource.kind==='layer'&&!ids.has(relief.heightSource.layer))fail('$.surfaceRelief.heightSource.layer','must reference an existing layer');
  if(relief.mode==='bakedDisplacement'){
    const bake=relief.displacementBake.composite;
    if(bake){
      for(const [field,id] of [['baseLayer',bake.baseLayer],['overlayLayer',bake.overlayLayer]])if(!ids.has(id))fail(`$.surfaceRelief.displacementBake.composite.${field}`,'must reference an existing layer');
      if(bake.overlayMaskLayer!==null&&!ids.has(bake.overlayMaskLayer))fail('$.surfaceRelief.displacementBake.composite.overlayMaskLayer','must reference an existing layer');
    }
  }
  return {schema:LAYERED_SCHEMA_V2,name,layers,surfaceRelief:relief};
}

/** Explicit v1→v2 migration. Default relief is off; nothing is enabled silently. */
export function migrateLayeredMaterialToV2(value) {
  const normalized=normalizeLayeredMaterial(value);
  if(normalized.schema===LAYERED_SCHEMA_V2)return structuredClone(normalized);
  const migrated=structuredClone(value);
  migrated.schema=LAYERED_SCHEMA_V2;
  migrated.surfaceRelief=defaultSurfaceRelief(value.layers[0].id);
  return normalizeLayeredMaterial(migrated);
}

/** Relief config for any schema; v1 materials are always off. */
export function surfaceReliefOf(material) {
  return material.surfaceRelief??defaultSurfaceRelief(material.layers[0].id);
}

/** Compute the complete immutable texture closure. `hashes` maps semantic/name to SHA-256. */
export function compileLayeredMaterial(value,{hashes,shaderFingerprint,uvSets=16}={}) {
  const material=normalizeLayeredMaterial(value);
  if(!object(hashes))fail('$.dependencies','a semantic/name to content hash map is required');
  if(typeof shaderFingerprint!=='string'||!hashPattern.test(shaderFingerprint))fail('$.shaderFingerprint','must be a lowercase SHA-256');
  if(!Number.isSafeInteger(uvSets)||uvSets<1||uvSets>16)fail('$.uvSets','must be an integer in [1, 16]');
  const dependencies=new Map(),sets=new Set(),active=[];let samples=0;
  const add=(slot,ref,path)=>{
    if(ref.uv.set>=uvSets)fail(`${path}.uv.set`,`UV set ${ref.uv.set} is unavailable`);
    sets.add(ref.uv.set);const key=`${SLOT_SEMANTIC[slot]}/${ref.name}`,hash=hashes[key];
    if(typeof hash!=='string'||!hashPattern.test(hash))fail(path,`missing valid content hash for ${key}`);
    dependencies.set(key,{semantic:SLOT_SEMANTIC[slot],name:ref.name,sha256:hash});samples++;
  };
  // Disabled and constant-zero overlay layers are omitted from runtime work,
  // but every authored dependency is still locked for reproducible editing.
  for(const [i,item] of material.layers.entries()){
    for(const [slot,ref] of Object.entries(item.textures))add(slot,ref,`$.layers[${i}].textures.${slot}`);
    if(item.blend.texture)add('mask',item.blend.texture,`$.layers[${i}].blend.texture`);
    if(item.enabled&&(i===0||item.blend.source!=='constant'||item.blend.weight!==0))active.push(item.id);
  }
  if(material.surfaceRelief?.heightSource.kind==='texture')add('height',material.surfaceRelief.heightSource.texture,'$.surfaceRelief.heightSource.texture');
  const closure=[...dependencies.entries()].sort(([a],[b])=>a<b?-1:a>b?1:0).map(([,entry])=>entry);
  const identity={compiler:LAYERED_COMPILER_VERSION,shaderFingerprint,material,dependencies:closure,uvSets:[...sets].sort((a,b)=>a-b)};
  return {material,identity,hash:sha256Hex(canonicalJson(identity)),dependencies:closure,activeLayers:active,authoredTextureSamples:samples};
}

/** Resolve a sampled mask channel and apply layer weight and height semantics. */
export function evaluateLayerCoverage(spec, sample, bottomHeight, topHeight) {
  const b=blend(spec,'blend',uv({},'uv'));
  number(sample,'sample',0,1);
  const raw=b.source==='constant'?1:sample;
  const mapped=clamp((b.invert?1-raw:raw)*b.gain+b.bias);
  const coverage=b.weight*mapped;
  return b.mode==='height'?heightCoverage(coverage,bottomHeight,topHeight,b.width,b.heightBias):coverage;
}

/** Smooth symmetric height-aware coverage. Endpoints are exact by contract. */
export function heightCoverage(coverage,bottomHeight,topHeight,width,bias=0) {
  number(coverage,'coverage',0,1);number(bottomHeight,'bottomHeight',-1000000,1000000);number(topHeight,'topHeight',-1000000,1000000);
  number(width,'width',.0001,1000);number(bias,'bias',-1000,1000);
  if(coverage===0||coverage===1)return coverage;
  const relative=(topHeight-bottomHeight+bias)/width;
  // coverage supplies the threshold, and the centered height difference makes
  // equal heights reproduce the ordinary mask without a visible discontinuity.
  return clamp(coverage + relative * coverage * (1-coverage));
}

/** CPU reference: sequentially blend already sampled linear PBR values. */
export function blendLayerSamples(bottom,top,coverage) {
  number(coverage,'coverage',0,1);
  const read=(sample,path)=>{
    keys(sample,['albedo','normal','roughness','metallic','ao','emissive','height'],path);
    return {albedo:vector(sample.albedo,`${path}.albedo`,3,0,1),normal:vector(sample.normal,`${path}.normal`,3,-1,1),
      roughness:number(sample.roughness,`${path}.roughness`,0,1),metallic:number(sample.metallic,`${path}.metallic`,0,1),ao:number(sample.ao,`${path}.ao`,0,1),
      emissive:vector(sample.emissive,`${path}.emissive`,3,0,1000000),height:number(sample.height,`${path}.height`,-1000000,1000000)};
  };
  const a=read(bottom,'bottom'),b=read(top,'top'),mix=(x,y)=>x+(y-x)*coverage;
  const normal=a.normal.map((v,i)=>mix(v,b.normal[i])),length=Math.hypot(...normal);
  // Antipodal normals have no unique interpolation direction. Deterministically
  // select the stronger contributor, then normalize; reject zero input normals.
  for(const [sample,path] of [[a,'bottom.normal'],[b,'top.normal']])if(Math.hypot(...sample.normal)<1e-8)fail(path,'must not be zero');
  const chosen=length<1e-8?(coverage<.5?a.normal:b.normal):normal,n=Math.hypot(...chosen);
  return {albedo:a.albedo.map((v,i)=>mix(v,b.albedo[i])),normal:chosen.map(v=>v/n),roughness:mix(a.roughness,b.roughness),metallic:mix(a.metallic,b.metallic),ao:mix(a.ao,b.ao),emissive:a.emissive.map((v,i)=>mix(v,b.emissive[i])),height:mix(a.height,b.height)};
}

/**
 * Reference evaluation of a material at one surface point. `sampleTexture`
 * must return linear RGBA in [0,1], applying the supplied wrap/filter policy.
 * Normal maps require `normalToWorld`, which converts their own tangent frame
 * to the shared world frame. This avoids silently treating different UV sets as
 * if they had the same basis.
 */
export function evaluateLayeredMaterial(value,{uvSets,sampleTexture,normalToWorld,surfaceNormal=[0,0,1],vertexColor=[1,1,1,1]}={}) {
  const material=normalizeLayeredMaterial(value);
  if(!Array.isArray(uvSets))fail('uvSets','must provide surface UV coordinates');
  if(typeof sampleTexture!=='function')fail('sampleTexture','must be a function');
  surfaceNormal=vector(surfaceNormal,'surfaceNormal',3,-1,1);
  const normalLength=Math.hypot(...surfaceNormal);if(normalLength<1e-8)fail('surfaceNormal','must not be zero');
  surfaceNormal=surfaceNormal.map(v=>v/normalLength);
  vertexColor=vector(vertexColor,'vertexColor',4,0,1);
  const channels={r:0,g:1,b:2,a:3};
  const sample=(ref,semantic,layerId,slot)=>{
    const base=vector(uvSets[ref.uv.set],`uvSets[${ref.uv.set}]`,2,-1000000,1000000),m=ref.uv.mapping;
    const c=Math.cos(m.rotation),s=Math.sin(m.rotation),x=base[0]*m.scale[0],y=base[1]*m.scale[1];
    const coordinate=[c*x-s*y+m.offset[0],s*x+c*y+m.offset[1]];
    const result=sampleTexture({semantic,name:ref.name,uv:coordinate,uvSet:ref.uv.set,mapping:m,layerId,slot});
    return vector(result,`sampleTexture(${layerId}.${slot})`,4,0,1);
  };
  const sampled=layer=>{
    const t=layer.textures,get=(slot,fallback)=>t[slot]?sample(t[slot],SLOT_SEMANTIC[slot],layer.id,slot):fallback;
    const color=get('albedo',[1,1,1,1]),orm=get('orm',[1,1,1,1]),rough=t.roughness?get('roughness',[1,1,1,1])[channels[t.roughness.channel??'r']]:orm[1];
    const metal=t.metallic?get('metallic',[1,1,1,1])[channels[t.metallic.channel??'r']]:orm[2];
    const occlusion=t.occlusion?get('occlusion',[1,1,1,1])[channels[t.occlusion.channel??'r']]:orm[0];
    const emission=get('emissive',[0,0,0,1]),height=t.height?get('height',[.5,.5,.5,1])[channels[t.height.channel??'r']]:.5;
    let normal=surfaceNormal;
    if(t.normal){
      if(typeof normalToWorld!=='function')fail(`layers.${layer.id}.textures.normal`,'normalToWorld is required for the selected UV tangent frame');
      const encoded=get('normal',[.5,.5,1,1]),tangent=[(encoded[0]*2-1)*layer.normalStrength,(encoded[1]*2-1)*layer.normalStrength,encoded[2]*2-1];
      const world=vector(normalToWorld({normal:tangent,layerId:layer.id,uvSet:t.normal.uv.set,mapping:t.normal.uv.mapping}),`normalToWorld(${layer.id})`,3,-1e6,1e6);
      const length=Math.hypot(...world);if(length<1e-8)fail(`normalToWorld(${layer.id})`,'must not return zero');normal=world.map(v=>v/length);
    }
    return {albedo:layer.baseColor.map((v,i)=>v*color[i]),normal,roughness:rough*layer.roughness,metallic:metal*layer.metallic,
      ao:1-(1-occlusion)*layer.occlusionStrength,emissive:layer.emissiveFactor.map((v,i)=>v*emission[i]*layer.emissiveStrength),height:height*layer.heightScale+layer.heightOffset};
  };
  let result=sampled(material.layers[0]);
  for(const layer of material.layers.slice(1)){
    if(!layer.enabled||layer.blend.weight===0)continue;
    const b=layer.blend,mask=b.source==='textureMask'?sample(b.texture,'mask',layer.id,'mask')[channels[b.channel]]:b.source==='vertexColor'?vertexColor[channels[b.channel]]:1;
    const preliminary=evaluateLayerCoverage({...b,mode:'mask'},mask,0,0);
    if(preliminary===0)continue;
    const top=sampled(layer),coverage=b.mode==='height'?heightCoverage(preliminary,result.height,top.height,b.width,b.heightBias):preliminary;
    result=blendLayerSamples(result,top,coverage);
  }
  return result;
}

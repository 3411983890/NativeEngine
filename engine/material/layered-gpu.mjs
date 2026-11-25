// Produces the intermediate structured-buffer layout consumed by
// shaders/material_layer_sampling.hlsli. The renderer does not yet bind it.
import { normalizeLayeredMaterial, surfaceReliefOf, LayeredMaterialError } from './layered.mjs';
import { compileTextureMapping } from './texture-mapping.mjs';

export const LAYER_GPU_BYTES=512;
export const SURFACE_RELIEF_GPU_BYTES=96;
const slots=['albedo','normal','orm','roughness','metallic','occlusion','emissive','height','mask'];
const channels={r:0,g:1,b:2,a:3};

export function encodeLayerGpuRecords(value,{textures,uvSets=16}={}) {
  const material=normalizeLayeredMaterial(value);
  if(!textures||typeof textures!=='object'||Array.isArray(textures))throw new LayeredMaterialError('$.textures','descriptor table is required');
  if(!Number.isSafeInteger(uvSets)||uvSets<1||uvSets>16)throw new LayeredMaterialError('$.uvSets','must be an integer in [1, 16]');
  const buffer=Buffer.alloc(material.layers.length*LAYER_GPU_BYTES);
  const f=(offset,value)=>buffer.writeFloatLE(value,offset),u=(offset,value)=>buffer.writeUInt32LE(value,offset);
  for(const [index,layer] of material.layers.entries()){
    const base=index*LAYER_GPU_BYTES;
    for(const [slotIndex,semantic] of slots.entries()){
      const ref=semantic==='mask'?layer.blend.texture:layer.textures[semantic],offset=base+slotIndex*48;
      const mapping=compileTextureMapping(ref?.uv.mapping);
      mapping.row0.forEach((v,i)=>f(offset+i*4,v));mapping.row1.forEach((v,i)=>f(offset+16+i*4,v));
      if(!ref){u(offset+12,0xffffffff);continue;}
      const key=`${semantic}/${ref.name}`,descriptor=textures[key];
      if(!descriptor||!Number.isSafeInteger(descriptor.index)||descriptor.index<0||descriptor.index>=0xffffffff)throw new LayeredMaterialError(`$.layers[${index}].textures.${semantic}`,`missing GPU descriptor for ${key}`);
      if(ref.uv.set>=uvSets)throw new LayeredMaterialError(`$.layers[${index}].textures.${semantic}`,`UV set ${ref.uv.set} is unavailable`);
      if(descriptor.bc5!==undefined&&typeof descriptor.bc5!=='boolean')throw new LayeredMaterialError(`$.layers[${index}].textures.${semantic}`,'bc5 must be boolean');
      u(offset+12,descriptor.index);u(offset+28,mapping.sampler);u(offset+32,ref.uv.set);
      u(offset+36,channels[semantic==='mask'?layer.blend.channel:ref.channel??'r']);
      u(offset+40,semantic==='normal'&&descriptor.bc5?1:0);
    }
    layer.baseColor.forEach((v,i)=>f(base+432+i*4,v));f(base+444,layer.roughness);
    layer.emissiveFactor.forEach((v,i)=>f(base+448+i*4,v));f(base+460,layer.metallic);
    [layer.normalStrength,layer.occlusionStrength,layer.emissiveStrength,layer.heightScale,layer.heightOffset,layer.blend.weight,layer.blend.gain,layer.blend.bias,layer.blend.width,layer.blend.heightBias].forEach((v,i)=>f(base+464+i*4,v));
    u(base+504,{constant:0,textureMask:1,vertexColor:2}[layer.blend.source]);
    u(base+508,(layer.blend.invert?1:0)|(layer.blend.mode==='height'?2:0)|(!layer.enabled?4:0));
    // Vertex-color blend has no mask texture but still needs its selected channel.
    if(layer.blend.source==='vertexColor')u(base+8*48+36,channels[layer.blend.channel]);
  }
  return {buffer,layerCount:material.layers.length,material};
}

/** Encode the material-level relief record appended to each GpuMaterial. */
export function encodeSurfaceRelief(value,{textures}={}) {
  const material=normalizeLayeredMaterial(value),relief=surfaceReliefOf(material);
  const buffer=Buffer.alloc(SURFACE_RELIEF_GPU_BYTES);
  const f=(offset,value)=>buffer.writeFloatLE(value,offset),u=(offset,value)=>buffer.writeUInt32LE(value,offset);
  let flags=({off:0,pom:1,bakedDisplacement:2})[relief.mode];
  if(relief.invert)flags|=1<<2;
  if(relief.selfShadow)flags|=1<<3;
  let heightLayer=0xffffffff,texture=0xffffffff,sampler=0,uvSet=0,channel=0;
  if(relief.heightSource.kind==='layer'){
    heightLayer=material.layers.findIndex(layer=>layer.id===relief.heightSource.layer);
    if(heightLayer<0)throw new LayeredMaterialError('$.surfaceRelief.heightSource.layer','must reference an existing layer');
  }else{
    flags|=1<<4;
    const ref=relief.heightSource.texture,mapping=compileTextureMapping(ref.uv.mapping);
    mapping.row0.forEach((v,i)=>f(i*4,v));mapping.row1.forEach((v,i)=>f(16+i*4,v));
    sampler=mapping.sampler;uvSet=ref.uv.set;channel=channels[ref.channel??'r'];
    if(relief.mode!=='off'){
      if(!textures||typeof textures!=='object')throw new LayeredMaterialError('$.surfaceRelief.texture','descriptor table is required');
      const descriptor=textures[`height/${ref.name}`];
      if(!descriptor||!Number.isSafeInteger(descriptor.index)||descriptor.index<0||descriptor.index>=0xffffffff)throw new LayeredMaterialError('$.surfaceRelief.heightSource.texture',`missing GPU descriptor for height/${ref.name}`);
      texture=descriptor.index;
    }
  }
  u(12,texture);u(28,sampler);u(32,uvSet);u(36,channel);u(40,flags);u(44,heightLayer);
  f(48,relief.amplitudeMetres);f(52,relief.referenceHeight);f(56,relief.minSteps);f(60,relief.maxSteps);
  f(64,relief.refineSteps);f(68,relief.fadeStartMetres);f(72,relief.fadeEndMetres);f(76,relief.grazingFade);
  f(80,relief.shadowSteps);f(84,0);f(88,0);f(92,0);
  return {buffer,relief,material};
}

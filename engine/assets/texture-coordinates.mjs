import {requireAsset,readAccessor} from './gltf-document.mjs';
import {TEXTURE_SLOTS} from '../material/texture-mapping.mjs';
import {effectiveSourceMaterial} from './material-source.mjs';

export function sourceTextureCoordinate(info) {
  const override=info?.extensions?.KHR_texture_transform?.texCoord;
  for(const value of [info?.texCoord,override])requireAsset(value===undefined||Number.isSafeInteger(value)&&value>=0,'Texture coordinate index must be a nonnegative integer');
  return override??info?.texCoord??0;
}

// Compile the selected source coordinate set into each material slot's stream.
// Five base streams and three optional coating streams select arbitrary source sets.
export function materialTextureCoordinates(asset,attributes,material,count) {
  material=effectiveSourceMaterial(material);
  const coat=material?.extensions?.KHR_materials_clearcoat;
  const layerInfos=[coat?.clearcoatTexture,coat?.clearcoatRoughnessTexture,coat?.clearcoatNormalTexture],hasLayers=layerInfos.some(info=>info!==undefined);
  const infos=[material?.pbrMetallicRoughness?.baseColorTexture,material?.normalTexture,material?.pbrMetallicRoughness?.metallicRoughnessTexture,material?.emissiveTexture,material?.occlusionTexture,...(hasLayers?layerInfos:[])];
  const cache=new Map();
  const read=(index,required)=>{
    if(cache.has(index))return cache.get(index);
    const accessor=attributes[`TEXCOORD_${index}`];
    requireAsset(!required||accessor!==undefined,`Material requires missing TEXCOORD_${index} UV stream`,'DEPENDENCY');
    const values=accessor===undefined?Array.from({length:count},()=>[0,0]):readAccessor(asset,accessor,'VEC2');
    requireAsset(values.length===count&&values.every(row=>row.every(v=>Number.isFinite(v)&&Math.abs(v)<=1000000)),'Texture UV count or range is invalid');
    if(accessor!==undefined)cache.set(index,values);return values;
  };
  const indices=infos.map(info=>info?sourceTextureCoordinate(info):0),streams=indices.map((index,i)=>read(index,!!infos[i]));
  return {uv:streams[0],extraUvs:indices.slice(0,5).some(index=>index!==0)?streams.slice(1,5):undefined,...(hasLayers?{layerUvs:streams.slice(5)}:{}),textureCoordinates:Object.fromEntries(TEXTURE_SLOTS.slice(0,infos.length).map((slot,i)=>[slot,indices[i]]))};
}

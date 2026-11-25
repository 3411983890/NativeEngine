import {readAccessor,requireAsset} from './gltf-document.mjs';
import {validIor} from '../material/ior.mjs';

export function sourceIor(material){
  const extension=material?.extensions?.KHR_materials_ior;
  if(extension===undefined)return undefined;
  requireAsset(extension!==null&&typeof extension==='object'&&!Array.isArray(extension),'Invalid KHR_materials_ior extension');
  requireAsset(Object.keys(extension).every(k=>['ior','extras'].includes(k)),'Unsupported KHR_materials_ior property','UNSUPPORTED');
  const value=extension.ior===undefined?1.5:extension.ior;requireAsset(validIor(value),'ior must be 0 or a finite number in [1, Float32 maximum]');return value;
}
export function sourceClearcoat(material){
  const value=material?.extensions?.KHR_materials_clearcoat;
  if(value===undefined)return {};
  requireAsset(value!==null&&typeof value==='object'&&!Array.isArray(value),'Invalid KHR_materials_clearcoat extension');
  requireAsset(Object.keys(value).every(k=>['clearcoatFactor','clearcoatRoughnessFactor','clearcoatTexture','clearcoatRoughnessTexture','clearcoatNormalTexture','extras'].includes(k)),'Unsupported clearcoat property','UNSUPPORTED');
  for(const key of ['clearcoatFactor','clearcoatRoughnessFactor'])requireAsset(value[key]===undefined||Number.isFinite(value[key])&&value[key]>=0&&value[key]<=1,'Invalid '+key);
  const scale=value.clearcoatNormalTexture?.scale;requireAsset(scale===undefined||Number.isFinite(scale)&&Math.abs(scale)<=1000000,'Invalid clearcoat normal scale');return value;
}

export function sourceUsesUnlit(material) {
  const extensions=material?.extensions;
  if(!extensions||!Object.hasOwn(extensions,'KHR_materials_unlit'))return false;
  const value=extensions.KHR_materials_unlit;
  requireAsset(value!==null&&typeof value==='object'&&!Array.isArray(value),'Invalid KHR_materials_unlit extension');
  return true;
}
export function effectiveSourceMaterial(material={}) {
  if(!sourceUsesUnlit(material))return material;
  // Unlit uses base color, vertex color, alpha coverage and sidedness. PBR
  // fallback maps must not introduce dependencies or change its appearance.
  const pbr=material.pbrMetallicRoughness??{};
  return {...material,pbrMetallicRoughness:{baseColorFactor:pbr.baseColorFactor??[1,1,1,1],...(pbr.baseColorTexture?{baseColorTexture:pbr.baseColorTexture}:{}),metallicFactor:0,roughnessFactor:1},normalTexture:undefined,occlusionTexture:undefined,emissiveTexture:undefined,emissiveFactor:[0,0,0],extensions:{KHR_materials_unlit:material.extensions.KHR_materials_unlit}};
}
export function readVertexColors(asset,attributes,count) {
  if(attributes.COLOR_0===undefined)return undefined;
  const index=attributes.COLOR_0,spec=asset.document.accessors?.[index];
  requireAsset(spec?.normalized===undefined||typeof spec.normalized==='boolean','COLOR_0 normalized must be boolean');
  requireAsset(spec&&['VEC3','VEC4'].includes(spec.type)&&[5121,5123,5126].includes(spec.componentType)&&((spec.componentType===5126&&!spec.normalized)||(spec.componentType!==5126&&spec.normalized===true)),'COLOR_0 must use float or normalized unsigned RGB/RGBA');
  const values=readAccessor(asset,index,spec.type);
  requireAsset(values.length===count&&values.every(row=>row.every(v=>Number.isFinite(v)&&v>=0&&v<=1)),'Vertex color count or range is invalid');
  return values.map(row=>row.length===3?[...row,1]:row);
}

export function readVertexTangents(asset,attributes,count) {
  // glTF requires tangents to be ignored when no authored normals are present.
  if(attributes.TANGENT===undefined||attributes.NORMAL===undefined)return undefined;
  const spec=asset.document.accessors?.[attributes.TANGENT],quantized=(asset.document.extensionsUsed??[]).includes('KHR_mesh_quantization');
  requireAsset(spec?.normalized===undefined||typeof spec.normalized==='boolean','TANGENT normalized must be boolean');
  requireAsset(spec?.type==='VEC4'&&((spec.componentType===5126&&!spec.normalized)||(quantized&&[5120,5122].includes(spec.componentType)&&spec.normalized===true)),'TANGENT must use float or declared normalized signed VEC4');
  const values=readAccessor(asset,attributes.TANGENT,'VEC4');
  requireAsset(values.length===count&&values.every(row=>row.every(Number.isFinite)&&Math.abs(row[3])===1&&Math.abs(Math.hypot(...row.slice(0,3))-1)<.02),'Invalid tangent count, unit direction or handedness');
  const normals=readAccessor(asset,attributes.NORMAL,'VEC3');requireAsset(normals.length===count,'Tangent normal count differs');
  // glTF 2.0 requires the client to orthogonalize TANGENT against NORMAL. DCC
  // exports and quantized sources routinely ship a few degrees of drift, so
  // rejecting them would refuse valid content; Gram-Schmidt here instead.
  return values.map((row,i)=>{
    const length=Math.hypot(...row.slice(0,3)),t=row.slice(0,3).map(v=>v/length),n=normals[i],nLength=Math.hypot(...n);
    requireAsset(nLength>1e-8,'Tangent normal must be a unit vector');
    const unitN=n.map(v=>v/nLength),dot=t[0]*unitN[0]+t[1]*unitN[1]+t[2]*unitN[2];
    let orthogonal=t.map((v,k)=>v-dot*unitN[k]),oLength=Math.hypot(...orthogonal);
    // A tangent parallel to its normal has no defined bitangent; use a stable frame
    // instead of emitting a zero vector.
    if(!(oLength>1e-6)){
      const axis=Math.abs(unitN[0])<.8?[1,0,0]:[0,1,0];
      orthogonal=[axis[1]*unitN[2]-axis[2]*unitN[1],axis[2]*unitN[0]-axis[0]*unitN[2],axis[0]*unitN[1]-axis[1]*unitN[0]];
      oLength=Math.hypot(...orthogonal);
    }
    return [...orthogonal.map(v=>v/oLength),row[3]];
  });
}

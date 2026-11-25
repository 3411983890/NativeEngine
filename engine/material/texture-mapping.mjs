export const TEXTURE_SLOTS=Object.freeze(['albedo','normal','orm','emissive','occlusion','coat','coatRoughness','coatNormal']);
export const WRAP_MODES=Object.freeze(['repeat','clamp','mirror']);
export const MIN_FILTERS=Object.freeze(['nearest','linear','nearest-mipmap-nearest','linear-mipmap-nearest','nearest-mipmap-linear','linear-mipmap-linear']);
const object=v=>v!==null&&typeof v==='object'&&!Array.isArray(v);
export function normalizeTextureMapping(value){
  if(!object(value)||Object.keys(value).some(k=>!['offset','scale','rotation','wrapS','wrapT','magFilter','minFilter'].includes(k)))throw new Error('Texture mapping contains unsupported fields');
  if(Object.values(value).some(v=>v===null||v===undefined))throw new Error('Texture mapping values cannot be null or undefined');
  const mapping={offset:value.offset??[0,0],scale:value.scale??[1,1],rotation:value.rotation??0,wrapS:value.wrapS??'repeat',wrapT:value.wrapT??'repeat',magFilter:value.magFilter??'linear',minFilter:value.minFilter??'linear-mipmap-linear'};
  for(const key of ['offset','scale'])if(!Array.isArray(mapping[key])||mapping[key].length!==2||[...mapping[key]].some(v=>!Number.isFinite(v)||Math.abs(v)>1000000))throw new Error('Texture '+key+' must contain two finite numbers in [-1000000,1000000]');
  if(!Number.isFinite(mapping.rotation)||Math.abs(mapping.rotation)>1000000)throw new Error('Texture rotation exceeds supported range');
  if(!WRAP_MODES.includes(mapping.wrapS)||!WRAP_MODES.includes(mapping.wrapT)||!['linear','nearest'].includes(mapping.magFilter)||!MIN_FILTERS.includes(mapping.minFilter))throw new Error('Unsupported texture wrap or filter');
  return structuredClone(mapping);
}
export function validateTextureMappings(value){
  if(!object(value)||Object.keys(value).some(k=>!TEXTURE_SLOTS.includes(k)))throw new Error('Texture mappings must use named material slots');
  for(const mapping of Object.values(value))normalizeTextureMapping(mapping);return true;
}
export function compileTextureMapping(value){
  if(value===undefined)return {row0:[1,0,0],row1:[0,1,0],sampler:0};
  const m=normalizeTextureMapping(value),c=Math.cos(m.rotation),s=Math.sin(m.rotation);
  return {row0:[c*m.scale[0],-s*m.scale[1],m.offset[0]],row1:[s*m.scale[0],c*m.scale[1],m.offset[1]],sampler:1+(WRAP_MODES.indexOf(m.wrapS)*3+WRAP_MODES.indexOf(m.wrapT))*12+MIN_FILTERS.indexOf(m.minFilter)*2+(m.magFilter==='linear'?1:0)};
}

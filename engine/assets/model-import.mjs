import * as fs from 'node:fs/promises';
import path from 'node:path';
import sharp from 'sharp';
import { canonicalJson, sha256Hex } from './canonical.mjs';
import { requireAsset, readGltf, readAccessor, readRelative, identityMatrix, nodeMatrix, multiplyMatrix } from './gltf-document.mjs';
import { encodePng, cookDirectory, resizeRgba, targetDimensions } from '../../tools/textures.mjs';
import {coordinateTransform} from './coordinates.mjs';
import {isContainerTexture,decodeContainerTexture} from './texture-decode.mjs';
import {isLegacyRaster,decodeLegacyRaster} from './raster-image-decode.mjs';
import {normalizeTextureMapping} from '../material/texture-mapping.mjs';
import {sourceTextureCoordinate,materialTextureCoordinates} from './texture-coordinates.mjs';
import {sourceUsesUnlit,effectiveSourceMaterial,readVertexColors,readVertexTangents,sourceIor,sourceClearcoat} from './material-source.mjs';
import {generateMissingTangents} from './tangent-generation.mjs';
import {sourceIdentities,sourceMaterialIdentities,sourceObjectPart} from './source-identity.mjs';

const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const normalize = a => { const length = Math.hypot(...a); return length > 1e-10 ? a.map(x => x / length) : [0, 1, 0]; };
const vector = (m, p, w) => [0, 1, 2].map(i => m[i]*p[0] + m[i+4]*p[1] + m[i+8]*p[2] + m[i+12]*w);
function transformedNormals(m, normals) {
  const a = m.slice(0,3), b = m.slice(4,7), c = m.slice(8,11), bc = cross(b,c), ca = cross(c,a), ab = cross(a,b);
  const det = a.reduce((s,x,i) => s + x*bc[i],0);
  requireAsset(Math.abs(det) > 1e-12, 'Singular model transform');
  return { det, normals: normals?.map(n => normalize([0,1,2].map(i => (bc[i]*n[0]+ca[i]*n[1]+ab[i]*n[2])/det))) };
}
function normalsFor(positions, indices) {
  const normals = positions.map(() => [0,0,0]);
  for (let i=0;i<indices.length;i+=3) {
    const [a,b,c] = indices.slice(i,i+3), n = cross(positions[b].map((v,k)=>v-positions[a][k]), positions[c].map((v,k)=>v-positions[a][k]));
    for (const index of [a,b,c]) for(let k=0;k<3;k++) normals[index][k] += n[k];
  }
  return normals.map(normalize);
}
function triangles(source, mode) {
  requireAsset([4,5,6].includes(mode), `Primitive mode ${mode} is not a triangle surface`, 'UNSUPPORTED');
  if (mode === 4) { requireAsset(source.length % 3 === 0, 'Incomplete triangles'); return source; }
  const result=[];
  for(let i=2;i<source.length;i++) result.push(...(mode === 6 ? [source[0],source[i-1],source[i]]
    : i%2===0 ? [source[i-2],source[i-1],source[i]] : [source[i-1],source[i-2],source[i]]));
  return result;
}

export async function readStaticModel(file,{coordinates}={}) {
  const conversion=coordinateTransform(coordinates),model=await readStaticSourceModel(file);
  if(!conversion.isDefault)for(const primitive of model.primitives) {
    // The source readers already apply Fz. Compose C * Fz to replace that basis.
    primitive.positions=primitive.positions.map(p=>conversion.position([p[0],p[1],-p[2]]));
    primitive.normals=primitive.normals.map(n=>conversion.direction([n[0],n[1],-n[2]]));
    if(primitive.tangents)primitive.tangents=primitive.tangents.map(t=>[...conversion.direction([t[0],t[1],-t[2]]),t[3]*-conversion.determinant]);
    if(conversion.determinant>0)for(let i=0;i<primitive.indices.length;i+=3)[primitive.indices[i+1],primitive.indices[i+2]]=[primitive.indices[i+2],primitive.indices[i+1]];
  }
  return model;
}
async function readStaticSourceModel(file) {
  if(path.extname(file).toLowerCase()==='.obj') return readObj(file);
  const asset = await generateMissingTangents(await readGltf(file)), doc=asset.document;
  requireAsset(!(doc.extensionsRequired ?? []).some(e => !['KHR_materials_clearcoat','KHR_materials_ior','KHR_materials_emissive_strength','KHR_materials_unlit','KHR_texture_transform','KHR_mesh_quantization','KHR_texture_basisu','MSFT_texture_dds','EXT_texture_webp','EXT_texture_avif'].includes(e)),
    `Required glTF extensions need a decoder: ${(doc.extensionsRequired??[]).join(', ')}`, 'UNSUPPORTED');
  requireAsset(!doc.skins?.length && !doc.animations?.length, 'Use character import for skinned or animated assets', 'UNSUPPORTED');
  const identities=sourceIdentities(doc),primitives=[], visited=new Set(), active=new Set(), warnings=[...(asset.tangentWarnings??[])];
  function walk(index,parent) {
    requireAsset(Number.isSafeInteger(index) && doc.nodes?.[index] && !active.has(index) && !visited.has(index), 'Invalid node hierarchy');
    active.add(index); visited.add(index);
    const node=doc.nodes[index], world=multiplyMatrix(parent,nodeMatrix(node));
    if(node.mesh!==undefined) {
      const mesh=doc.meshes?.[node.mesh]; requireAsset(mesh?.primitives?.length, 'Missing mesh primitives');
      requireAsset(!mesh.weights?.some(v=>v!==0), 'Default morph deformation requires character import', 'UNSUPPORTED');
      for(const [primitiveIndex,prim]of mesh.primitives.entries()) {
        requireAsset(!prim.extensions?.KHR_draco_mesh_compression, 'Draco decoder is required', 'UNSUPPORTED');
        requireAsset(prim.attributes?.POSITION!==undefined, 'Missing POSITION');
        const positions=readAccessor(asset,prim.attributes.POSITION,'VEC3');
        const source=prim.indices===undefined ? positions.map((_,i)=>i) : readAccessor(asset,prim.indices,'SCALAR').flat();
        requireAsset(source.every(i=>Number.isSafeInteger(i)&&i>=0&&i<positions.length),'Invalid triangle index');
        const indices=triangles(source,prim.mode??4);
        const inputNormals=prim.attributes.NORMAL===undefined?null:readAccessor(asset,prim.attributes.NORMAL,'VEC3');
        const {uv,extraUvs,layerUvs,textureCoordinates}=materialTextureCoordinates(asset,prim.attributes,doc.materials?.[prim.material],positions.length);
        requireAsset(uv.length===positions.length && (!inputNormals||inputNormals.length===positions.length),'Attribute count mismatch');
        const colors=readVertexColors(asset,prim.attributes,positions.length),inputTangents=readVertexTangents(asset,prim.attributes,positions.length),transformed=transformedNormals(world,inputNormals);
        if(transformed.det<0) for(let i=0;i<indices.length;i+=3) [indices[i+1],indices[i+2]]=[indices[i+2],indices[i+1]];
        // glTF is right handed; engine scenes and cameras use left handed +Z forward.
        const worldPositions=positions.map(p=>vector(world,p,1).map((v,i)=>i===2?-v:v));
        for(let i=0;i<indices.length;i+=3) [indices[i+1],indices[i+2]]=[indices[i+2],indices[i+1]];
        const normals=transformed.normals?.map(n=>[n[0],n[1],-n[2]])??normalsFor(worldPositions,indices);
        const tangents=inputTangents?.map(t=>[...normalize(vector(world,t,0)).map((v,k)=>k===2?-v:v),-Math.sign(transformed.det)*t[3]]);
        primitives.push({source:identities.part(index,prim,primitiveIndex),name:node.name??mesh.name??`Mesh ${primitives.length+1}`,positions:worldPositions,normals,uv,...(tangents?{tangents}:{}),...(colors?{colors}:{}),...(extraUvs?{extraUvs}:{}),...(layerUvs?{layerUvs}:{}),...(extraUvs||layerUvs?{textureCoordinates}:{}),indices,material:prim.material??-1});
        if(prim.targets) warnings.push('Static morph targets are preserved only by character import.');
        if(prim.attributes.TANGENT!==undefined&&!inputNormals)warnings.push('Tangents without authored normals are ignored as required by glTF.');
      }
    }
    if(node.camera!==undefined) warnings.push('Source camera is not assigned as the gameplay camera.');
    for(const child of node.children??[]) walk(child,world);
    active.delete(index);
  }
  const scene=doc.scenes?.[doc.scene??0]; requireAsset(scene?.nodes?.length,'No default scene');
  for(const root of scene.nodes) walk(root,identityMatrix());
  requireAsset(primitives.length>0 && primitives.length<=4096,'Model primitive budget exceeded','BUDGET');
  return {primitives,materials:doc.materials??[],asset,warnings:[...new Set(warnings)]};
}

async function readObj(file) {
  const source=await fs.readFile(file,'utf8'); requireAsset(source.length<=64*1024*1024,'OBJ exceeds 64 MiB','BUDGET');
  const p=[],n=[],uv=[],vertexColors=[],groups=[],materials=[],materialNames=new Map(); let group, name=path.basename(file), material=-1;
  const fresh=()=>group={name,material,positions:[],normals:[],uv:[],colors:[],hasColor:false,indices:[],missingNormal:false};
  const item=(items,value)=>{const v=Number(value),i=v<0?items.length+v:v-1;requireAsset(Number.isInteger(i)&&items[i],`Invalid OBJ index ${value}`);return items[i];};
  for(const raw of source.split(/\r?\n/)) {
    const line=raw.replace(/#.*/,'').trim(); if(!line)continue;
    const [op,...parts]=line.split(/\s+/);
    if(op==='v'||op==='vn'||op==='vt') {const values=parts.slice(0,op==='vt'?2:3).map(Number);requireAsset(values.length===(op==='vt'?2:3)&&values.every(Number.isFinite),'Invalid OBJ attribute');(op==='v'?p:op==='vn'?n:uv).push(values);if(op==='v'){let color=null;if(parts.length===6){color=parts.slice(3,6).map(Number);requireAsset(color.every(v=>Number.isFinite(v)&&v>=0&&v<=1),'Invalid OBJ vertex color');color.push(1);}vertexColors.push(color);}}
    if(op==='o'||op==='g') {name=parts.join(' ')||name;group=null;}
    if(op==='mtllib') {
      const bytes=await readRelative(path.dirname(file),parts.join(' ')); let current;
      for(const mline of bytes.toString('utf8').split(/\r?\n/)) {
        const [key,...values]=mline.replace(/#.*/,'').trim().split(/\s+/);
        if(key==='newmtl') { current={name:values.join(' '),pbrMetallicRoughness:{metallicFactor:0,roughnessFactor:1}};materialNames.set(current.name,materials.length);materials.push(current); }
        else if(current&&key==='Kd')current.pbrMetallicRoughness.baseColorFactor=[...values.slice(0,3).map(Number),current.pbrMetallicRoughness.baseColorFactor?.[3]??1];
        else if(current&&key==='map_Kd')current.baseColorUri=values.join(' ');
        else if(current&&(key==='d'||key==='Tr')){
          const value=Number(values[0]);requireAsset(values.length===1&&Number.isFinite(value)&&value>=0&&value<=1,'MTL opacity must be in [0,1]');
          const alpha=key==='Tr'?1-value:value;current.pbrMetallicRoughness.baseColorFactor=[...(current.pbrMetallicRoughness.baseColorFactor??[1,1,1,1]).slice(0,3),alpha];current.alphaMode=alpha<1?'BLEND':'OPAQUE';
        }
      }
    }
    if(op==='usemtl') {requireAsset(materialNames.has(parts.join(' ')),'Unknown OBJ material');material=materialNames.get(parts.join(' '));group=null;}
    if(op==='f') {
      requireAsset(parts.length>=3&&parts.length<=256,'Invalid OBJ polygon');
      if(!group){fresh();groups.push(group);}
      const base=group.positions.length;
      for(const vertex of parts) {const [a,b,c]=vertex.split('/');const v=item(p,a),color=vertexColors[Number(a)<0?p.length+Number(a):Number(a)-1];group.positions.push([v[0],v[1],-v[2]]);group.colors.push(color??[1,1,1,1]);group.hasColor ||= !!color;const t=b?item(uv,b):[0,0];group.uv.push([t[0],1-t[1]]);const normal=c?item(n,c):[0,0,0];group.normals.push([normal[0],normal[1],-normal[2]]);if(!c)group.missingNormal=true;}
      for(let i=2;i<parts.length;i++)group.indices.push(base,base+i,base+i-1);
    }
  }
  requireAsset(groups.length>0&&groups.length<=4096,'No usable OBJ surfaces');
  for(const [index,g]of groups.entries()){if(g.missingNormal)g.normals=normalsFor(g.positions,g.indices);if(!g.hasColor)delete g.colors;delete g.hasColor;g.source=sourceObjectPart(g.name,materials[g.material]?.name,index);}
  return {primitives:groups,materials,asset:{file,source:Buffer.from(source),dependencies:[]},warnings:['OBJ polygons are triangulated as fans; triangulate concave faces in the source tool.']};
}

export async function decodeImage(bytes,options={}) {
  if(options.basisMaterial)requireAsset(bytes.length>=104&&bytes.subarray(0,12).equals(Buffer.from([0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,13,10,26,10])),'KHR_texture_basisu must reference a KTX2 image');
  if(options.ddsMaterial)requireAsset(bytes.subarray(0,4).toString('ascii')==='DDS ','MSFT_texture_dds must reference a DDS image');
  if(isContainerTexture(bytes))return decodeContainerTexture(bytes,options);
  if(isLegacyRaster(bytes,options.extension))return decodeLegacyRaster(bytes,options);
  const {data,info}=await sharp(bytes,{limitInputPixels:64*1024*1024}).toColourspace('srgb').ensureAlpha().raw().toBuffer({resolveWithObject:true});
  return {width:info.width,height:info.height,data};
}
export async function imageFor(asset,info,semantic='albedo') {
  if(info===undefined) return null;
  const mapping=sourceTextureMapping(asset,info);
  const texture=asset.document?.textures?.[info.index],extensions=texture?.extensions??{},source=extensions.KHR_texture_basisu?.source??extensions.MSFT_texture_dds?.source??extensions.EXT_texture_avif?.source??extensions.EXT_texture_webp?.source??texture?.source,image=asset.document?.images?.[source];
  requireAsset(image,'Missing texture image');
  let bytes;
  if(image.uri!==undefined)bytes=await readRelative(path.dirname(asset.file),image.uri);
  else {const view=asset.document.bufferViews?.[image.bufferView], buffer=asset.buffers[view?.buffer];requireAsset(view&&buffer&&Number.isSafeInteger(view.byteLength)&&(view.byteOffset??0)>=0&&(view.byteOffset??0)+view.byteLength<=buffer.length,'Invalid image bufferView');bytes=buffer.subarray(view.byteOffset??0,(view.byteOffset??0)+view.byteLength);}
  return {...await decodeImage(bytes,{semantic,basisMaterial:!!extensions.KHR_texture_basisu,ddsMaterial:!extensions.KHR_texture_basisu&&!!extensions.MSFT_texture_dds,extension:image.mimeType??(image.uri&&!image.uri.startsWith('data:')?path.extname(image.uri):'')}),textureMapping:mapping};
}
export function sourceTextureMapping(asset,info){
  requireAsset(info&&typeof info==='object'&&!Array.isArray(info),'Invalid texture info');
  requireAsset(Number.isSafeInteger(info.index)&&info.index>=0,'Texture index must be a nonnegative integer');
  requireAsset(info.extensions===undefined||info.extensions&&typeof info.extensions==='object'&&!Array.isArray(info.extensions),'Invalid texture info extensions');
  requireAsset(Object.keys(info.extensions??{}).every(k=>k==='KHR_texture_transform'),'Texture info extension needs an adapter','UNSUPPORTED');
  const transform=info.extensions?.KHR_texture_transform;
  requireAsset(transform===undefined||transform&&typeof transform==='object'&&!Array.isArray(transform),'Invalid texture transform');
  sourceTextureCoordinate(info);
  const texture=asset.document?.textures?.[info.index];requireAsset(texture,'Missing texture');
  requireAsset(texture.sampler===undefined||Number.isSafeInteger(texture.sampler)&&texture.sampler>=0,'Sampler index must be a nonnegative integer');
  const sampler=texture.sampler===undefined?{}:asset.document.samplers?.[texture.sampler];requireAsset(sampler&&typeof sampler==='object'&&!Array.isArray(sampler),'Missing texture sampler');
  for(const key of ['wrapS','wrapT','minFilter','magFilter'])requireAsset(sampler[key]===undefined||Number.isSafeInteger(sampler[key]),'Sampler values must be integers');
  const wraps={10497:'repeat',33071:'clamp',33648:'mirror'},filters={9728:'nearest',9729:'linear',9984:'nearest-mipmap-nearest',9985:'linear-mipmap-nearest',9986:'nearest-mipmap-linear',9987:'linear-mipmap-linear'};
  requireAsset(wraps[sampler.wrapS??10497]&&wraps[sampler.wrapT??10497]&&[undefined,9728,9729].includes(sampler.magFilter)&&[undefined,9728,9729,9984,9985,9986,9987].includes(sampler.minFilter),'Unsupported glTF texture sampler','UNSUPPORTED');
  const value={...(transform??{})};delete value.texCoord;
  requireAsset(Object.keys(value).every(k=>['offset','scale','rotation','extras'].includes(k)),'Unknown texture transform field');delete value.extras;
  const changed=Object.keys(value).length||(sampler.wrapS??10497)!==10497||(sampler.wrapT??10497)!==10497||sampler.minFilter!==undefined||sampler.magFilter!==undefined;
  if(!changed)return undefined;
  return normalizeTextureMapping({...value,wrapS:wraps[sampler.wrapS??10497],wrapT:wraps[sampler.wrapT??10497],...(sampler.minFilter===undefined?{}:{minFilter:filters[sampler.minFilter]}),...(sampler.magFilter===undefined?{}:{magFilter:filters[sampler.magFilter]})});
}
/** Largest edge a cooked material map may keep. `auto` preserves the source. */
export const MODEL_TEXTURE_LIMIT = 4096;
export function textureLimit(size) {
  if (size === undefined || size === null || size === 'auto' || size === 'source' || size === 0) return MODEL_TEXTURE_LIMIT;
  requireAsset(Number.isInteger(size) && size >= 16 && size <= MODEL_TEXTURE_LIMIT && (size & (size - 1)) === 0,
    'Texture size must be auto or a power of two from 16 to 4096', 'ARGUMENT');
  return size;
}
const srgb = x => x <= .0031308 ? 12.92 * x : 1.055 * Math.pow(x, 1 / 2.4) - .055;
const linear = x => x <= .04045 ? x / 12.92 : Math.pow((x + .055) / 1.055, 2.4);

/**
 * Cap one source map at the import limit.
 *
 * The cap keeps the source aspect ratio and never magnifies, so a 4:1 decal stays
 * 4:1 instead of being squashed into a square, and a 64² detail map is not
 * inflated to the array size. Resampling uses area averaging (see resizeRgba), so
 * a 2048² source keeps every texel's contribution at 512².
 */
function capImage(image, semantic, limit) {
  if (!image) return null;
  const dims = targetDimensions(image.width, image.height, limit, true);
  return resizeRgba(image, dims.width, dims.height, semantic);
}
const constant = (rgba) => ({ width: 1, height: 1, data: Buffer.from(rgba) });

function albedoChannel(image, factor, alphaMode, limit) {
  const base = capImage(image, 'albedo', limit) ?? constant([255, 255, 255, 255]);
  const data = Buffer.from(base.data);
  const tinted = factor[0] !== 1 || factor[1] !== 1 || factor[2] !== 1;
  for (let at = 0; at < data.length; at += 4) {
    if (tinted) for (let k = 0; k < 3; k++) data[at + k] = Math.round(srgb(linear(data[at + k] / 255) * factor[k]) * 255);
    if (alphaMode === 'OPAQUE') data[at + 3] = 255;
    else if (alphaMode === 'MASK') data[at + 3] = Math.round(data[at + 3] * factor[3]);
  }
  return { width: base.width, height: base.height, data };
}
function normalChannel(image, scale, semantic, limit) {
  const base = capImage(image, semantic, limit) ?? constant([128, 128, 255, 255]);
  const data = Buffer.alloc(base.data.length);
  for (let at = 0; at < base.data.length; at += 4) {
    const vector = [(base.data[at] / 127.5 - 1) * scale, (base.data[at + 1] / 127.5 - 1) * scale, base.data[at + 2] / 127.5 - 1];
    const length = Math.hypot(...vector) || 1;
    for (let k = 0; k < 3; k++) data[at + k] = Math.round((vector[k] / length * .5 + .5) * 255);
    data[at + 3] = 255;
  }
  return { width: base.width, height: base.height, data };
}
function ormChannel(image, roughness, metallic, limit) {
  const base = capImage(image, 'orm', limit) ?? constant([255, 255, 255, 255]);
  const data = Buffer.alloc(base.data.length);
  for (let at = 0; at < base.data.length; at += 4) {
    data[at] = 255;
    data[at + 1] = Math.round(base.data[at + 1] * roughness);
    data[at + 2] = Math.round(base.data[at + 2] * metallic);
    data[at + 3] = 0;
  }
  return { width: base.width, height: base.height, data };
}
function singleChannel(image, channel, semantic, limit) {
  const base = capImage(image, semantic, limit);
  const data = Buffer.alloc(base.data.length);
  for (let at = 0; at < base.data.length; at += 4) {
    const value = base.data[at + channel];
    data[at] = value; data[at + 1] = value; data[at + 2] = value; data[at + 3] = 255;
  }
  return { width: base.width, height: base.height, data };
}
function colorChannel(image, semantic, limit) {
  const base = capImage(image, semantic, limit);
  const data = Buffer.alloc(base.data.length);
  for (let at = 0; at < base.data.length; at += 4) {
    data[at] = base.data[at]; data[at + 1] = base.data[at + 1]; data[at + 2] = base.data[at + 2]; data[at + 3] = 255;
  }
  return { width: base.width, height: base.height, data };
}

export async function cookModelMaterials(model, directory, { size = 'auto' } = {}) {
  const limit = textureLimit(size);
  const source = path.join(directory, 'source-textures'), out = path.join(directory, 'textures');
  await fs.mkdir(source, { recursive: true });
  const materials = {}, names = [], written = new Map();
  // Reuse a cooked map when a second material resolves to the same bytes. Source
  // libraries commonly share one atlas across many materials; without this the
  // same image is stored (and uploaded) once per material. Synthesised constants
  // stay per material so a factor-only material keeps a stable texture name.
  const publish = async (name, semantic, image, shareable = true) => {
    const key = shareable ? `${semantic}:${image.width}x${image.height}:${sha256Hex(image.data)}` : `${name}.${semantic}`;
    const existing = written.get(key);
    if (existing) return existing;
    await fs.writeFile(path.join(source, `${name}.${semantic}.png`), encodePng(image));
    written.set(key, name);
    return name;
  };
  for (let i = -1; i < model.materials.length; i++) {
    const authored = i < 0 ? {} : model.materials[i], unlit = sourceUsesUnlit(authored), mat = effectiveSourceMaterial(authored), pbr = mat.pbrMetallicRoughness ?? {}, name = `m${i + 1}`;
    for (const value of [pbr.roughnessFactor ?? 1, pbr.metallicFactor ?? 1, mat.occlusionTexture?.strength ?? 1]) requireAsset(Number.isFinite(value) && value >= 0 && value <= 1, 'Invalid PBR material factor');
    requireAsset(Number.isFinite(mat.normalTexture?.scale ?? 1), 'Invalid normal scale');
    requireAsset(['OPAQUE', 'MASK', 'BLEND'].includes(mat.alphaMode ?? 'OPAQUE'), 'Unsupported material alpha mode', 'UNSUPPORTED');
    requireAsset(Number.isFinite(mat.alphaCutoff ?? .5) && (mat.alphaCutoff ?? .5) >= 0 && (mat.alphaCutoff ?? .5) <= 1, 'Invalid alpha cutoff');
    requireAsset(!Object.keys(mat.extensions ?? {}).some(k => !['KHR_materials_clearcoat', 'KHR_materials_ior', 'KHR_materials_emissive_strength', 'KHR_materials_unlit'].includes(k)), 'Material extension is not supported', 'UNSUPPORTED');
    const ior = sourceIor(mat);
    const coat = sourceClearcoat(mat), coatMap = await imageFor(model.asset, coat.clearcoatTexture, 'occlusion'), coatRoughness = await imageFor(model.asset, coat.clearcoatRoughnessTexture, 'orm'), coatNormal = await imageFor(model.asset, coat.clearcoatNormalTexture, 'normal');
    let albedo = await imageFor(model.asset, pbr.baseColorTexture);
    if (mat.baseColorUri) albedo = await decodeImage(await readRelative(path.dirname(model.asset.file), mat.baseColorUri));
    const normal = await imageFor(model.asset, mat.normalTexture, 'normal'), orm = await imageFor(model.asset, pbr.metallicRoughnessTexture, 'orm'), ao = await imageFor(model.asset, mat.occlusionTexture, 'occlusion'), emissive = await imageFor(model.asset, mat.emissiveTexture, 'emissive');
    const occlusionStrength = mat.occlusionTexture?.strength === undefined ? 1 : mat.occlusionTexture.strength;
    requireAsset(Number.isFinite(occlusionStrength) && occlusionStrength >= 0 && occlusionStrength <= 1, 'Invalid occlusion strength');
    const factor = pbr.baseColorFactor ?? [1, 1, 1, 1]; requireAsset(factor.length === 4 && factor.every(v => Number.isFinite(v) && v >= 0 && v <= 1), 'Invalid base color factor');
    const emission = mat.emissiveFactor === undefined ? [0, 0, 0] : mat.emissiveFactor;
    requireAsset(Array.isArray(emission) && emission.length === 3 && emission.every(v => Number.isFinite(v) && v >= 0 && v <= 1), 'Invalid emissive factor');
    const strengthExtension = mat.extensions?.KHR_materials_emissive_strength;
    requireAsset(strengthExtension === undefined || strengthExtension && typeof strengthExtension === 'object' && !Array.isArray(strengthExtension), 'Invalid emissive strength extension');
    const emissionStrength = strengthExtension?.emissiveStrength === undefined ? 1 : strengthExtension.emissiveStrength;
    requireAsset(Number.isFinite(emissionStrength) && emissionStrength >= 0 && emissionStrength <= 1000000, 'Emissive strength exceeds supported [0,1000000] range', 'BUDGET');
    const hasEmission = !!emissive || emission.some(v => v > 0);
    const alphaMode = mat.alphaMode ?? 'OPAQUE';
    // Each slot is baked at its own source resolution; only the import cap shrinks it.
    const albedoName = await publish(name, 'albedo', albedoChannel(albedo, factor, alphaMode, limit), !!albedo);
    const normalName = await publish(name, 'normal', normalChannel(normal, mat.normalTexture?.scale ?? 1, 'normal', limit), !!normal);
    const ormName = await publish(name, 'orm', ormChannel(orm, pbr.roughnessFactor ?? 1, pbr.metallicFactor ?? 1, limit), !!orm);
    let occlusionName, emissiveName, coatName, coatRoughnessName, coatNormalName;
    if (ao) occlusionName = await publish(name, 'occlusion', singleChannel(ao, 0, 'occlusion', limit));
    if (hasEmission) emissiveName = await publish(name, 'emissive', emissive ? colorChannel(emissive, 'emissive', limit) : constant([255, 255, 255, 255]), !!emissive);
    if (coatMap) coatName = await publish(name, 'coat', singleChannel(coatMap, 0, 'occlusion', limit));
    if (coatRoughness) coatRoughnessName = await publish(name, 'coatRoughness', singleChannel(coatRoughness, 1, 'orm', limit));
    if (coatNormal) coatNormalName = await publish(name, 'coatNormal', normalChannel(coatNormal, coat.clearcoatNormalTexture.scale ?? 1, 'normal', limit));
    materials[name] = {
      albedo: albedoName, normal: normalName, orm: ormName, uvScale: 1, roughness: 1, metalness: 1,
      clearcoat: coat.clearcoatFactor ?? 0, clearcoatRoughness: coat.clearcoatRoughnessFactor ?? 0, ...(unlit ? { unlit: true } : {}),
      ...([albedo, normal, orm, emissive, ao, coatMap, coatRoughness, coatNormal].some(image => image?.textureMapping) ? { textureMappings: Object.fromEntries(Object.entries({ albedo, normal, orm, emissive, occlusion: ao, coat: coatMap, coatRoughness, coatNormal }).filter(([, image]) => image?.textureMapping).map(([key, image]) => [key, image.textureMapping])) } : {}),
      ...(coatName ? { coat: coatName } : {}), ...(coatRoughnessName ? { coatRoughness: coatRoughnessName } : {}), ...(coatNormalName ? { coatNormal: coatNormalName } : {}),
      ...(ior !== undefined ? { ior } : {}), ...(occlusionName ? { occlusion: occlusionName, occlusionStrength } : {}),
      ...(emissiveName ? { emissive: emissiveName, emissiveFactor: emission, emissiveStrength: emissionStrength } : {}),
      ...(alphaMode === 'MASK' ? { alphaMode: 'MASK', alphaCutoff: mat.alphaCutoff ?? .5 } : alphaMode === 'BLEND' ? { alphaMode: 'BLEND', opacity: factor[3] } : {}),
    };
    names.push(name);
    if (mat.doubleSided) model.warnings.push('Double-sided material uses duplicated back-face geometry.');
  }
  requireAsset(written.size <= 256, 'Imported material textures exceed the 256 texture runtime limit', 'BUDGET');
  cookDirectory({ source, out, size: limit, format: 'auto', preserveAspect: true });
  return { materials, names, materialSources: sourceMaterialIdentities(model.materials) };
}

export async function cookStaticModel(file,directory,options={}) {
  const model=await readStaticModel(file,options),{materials,names,materialSources}=await cookModelMaterials(model,directory,options),parts=[];
  let gpuBytes=0;
  await fs.mkdir(path.join(directory,'meshes'),{recursive:true});
  for(const [i,prim] of model.primitives.entries()) {
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
    for(const p of prim.positions)for(let k=0;k<3;k++){min[k]=Math.min(min[k],p[k]);max[k]=Math.max(max[k],p[k]);}
    const center=min.map((v,k)=>(v+max[k])/2),scale=min.map((v,k)=>Math.max(.001,max[k]-v));
    const vertices=prim.positions.map((p,j)=>[...p.map((v,k)=>(v-center[k])/scale[k]),...normalize(prim.normals[j].map((v,k)=>v*scale[k])),...prim.uv[j],...(prim.extraUvs?prim.extraUvs.flatMap(stream=>stream[j]):prim.colors||prim.tangents||prim.layerUvs?Array.from({length:4},()=>prim.uv[j]).flat():[]),...(prim.colors?prim.colors[j]:prim.tangents||prim.layerUvs?[1,1,1,1]:[]),...(prim.tangents?[...normalize(prim.tangents[j].slice(0,3).map((v,k)=>v/scale[k])),prim.tangents[j][3]]:prim.layerUvs?[0,0,0,0]:[]),...(prim.layerUvs?prim.layerUvs.flatMap(stream=>stream[j]):[])]);
    const indices=[...prim.indices];
    if(model.materials[prim.material]?.doubleSided) {
      const count=vertices.length; vertices.push(...vertices.map(v=>{const out=[...v];for(let k=3;k<6;k++)out[k]=-out[k];if(prim.tangents)out[23]=-out[23];return out;}));
      for(let j=0;j<prim.indices.length;j+=3)indices.push(count+prim.indices[j],count+prim.indices[j+2],count+prim.indices[j+1]);
    }
    const mesh={schema:prim.layerUvs?'nativegpu.mesh/5':prim.tangents?'nativegpu.mesh/4':prim.colors?'nativegpu.mesh/3':prim.extraUvs?'nativegpu.mesh/2':'nativegpu.mesh/1',vertices,indices},bytes=Buffer.from(canonicalJson(mesh)+'\n'),hash=sha256Hex(bytes),relative=`meshes/${hash}.mesh.json`;
    gpuBytes+=vertices.length*120+indices.length*4;
    requireAsset(gpuBytes<=64*1024*1024,'Imported vertex/index pool exceeds 64 MiB','BUDGET');
    requireAsset(bytes.length<=64*1024*1024,'Mesh JSON exceeds 64 MiB','BUDGET');
    await fs.writeFile(path.join(directory,relative),bytes);
    requireAsset(names[prim.material+1],'Invalid primitive material');
    parts.push({source:prim.source,name:prim.name,mesh:relative,sha256:hash,position:center,scale,material:names[prim.material+1],...(prim.extraUvs||prim.layerUvs?{textureCoordinates:prim.textureCoordinates}:{})});
  }
  return {schema:'nativegpu.model/1',modelSourceHash:sha256Hex(model.asset.source),dependencies:model.asset.dependencies,materialSources,materials,parts,warnings:[...new Set([...model.warnings,'Collision and ray-traced shadows currently use bounding-box proxies.'])]};
}

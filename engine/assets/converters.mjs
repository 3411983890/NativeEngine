import * as fs from 'node:fs/promises';
import path from 'node:path';
import { spawn } from 'node:child_process';
import assimpFactory from 'assimpjs';
import ffmpeg from 'ffmpeg-static';
import { requireAsset, AssetError } from './gltf-document.mjs';
import { readFbxMetadata, describeFbx, fbxCoordinateOptions } from './fbx-metadata.mjs';

export const CONVERTIBLE_MODELS=['.fbx','.dae','.stl','.ply','.3ds','.3mf'];
export const CONVERTIBLE_AUDIO=['.mp3','.ogg','.flac','.m4a','.aac','.aiff','.aif','.opus'];
// Formats a source texture library may use. FBX in particular references images
// by name only, so the converter needs every sibling image regardless of extension.
const SIDECAR = /\.(mtl|png|jpg|jpeg|webp|tga|bmp|dds|ktx2|basis|tif|tiff|avif|bin|exr|hdr)$/i;
// Assimp tags triangulated n-gon meshes with this marker. The geometry it emits is
// already a triangle list, so the marker only needs to be dropped; leaving it in
// `extensionsUsed` makes the strict character path reject an otherwise valid mesh.
const CONVERTER_EXTENSIONS = new Set(['FB_ngon_encoding']);
let assimp;

const textureSlots = ['baseColorTexture', 'metallicRoughnessTexture'];
const materialSlots = ['normalTexture', 'occlusionTexture', 'emissiveTexture'];
const extensionSlots = ['clearcoatTexture', 'clearcoatRoughnessTexture', 'clearcoatNormalTexture'];

function dropTexture(material, index) {
  const pbr = material.pbrMetallicRoughness;
  for (const slot of textureSlots) if (pbr?.[slot]?.index === index) delete pbr[slot];
  for (const slot of materialSlots) if (material[slot]?.index === index) delete material[slot];
  for (const extension of Object.values(material.extensions ?? {})) {
    if (!extension || typeof extension !== 'object') continue;
    for (const slot of extensionSlots) if (extension[slot]?.index === index) delete extension[slot];
  }
}

async function indexSourceImages(directory) {
  const index = new Map();
  const walk = async (current, depth) => {
    if (depth > 3 || index.size > 4096) return;
    for (const entry of await fs.readdir(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name);
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) { await walk(full, depth + 1); continue; }
      if (!SIDECAR.test(entry.name) || entry.name.endsWith('.bin') || entry.name.endsWith('.mtl')) continue;
      const key = entry.name.toLowerCase();
      if (!index.has(key)) index.set(key, full);
    }
  };
  await walk(directory, 0);
  return index;
}

/**
 * Repair the converter's glTF so the importer can consume it.
 *
 * Assimp keeps the FBX texture name as an image URI. Files that embed their
 * textures, or that point at a library that has since moved, produce a document
 * referencing a file that does not exist; the importer would then fail with a
 * filesystem error for the whole model. Resolve by basename inside the source
 * directory first, and otherwise detach only the affected material slot so the
 * rest of the model still imports.
 */
async function repairConvertedDocument(main, sourceDirectory, warnings) {
  const document = JSON.parse(await fs.readFile(main, 'utf8'));
  const images = await indexSourceImages(sourceDirectory);
  const base = path.dirname(main);
  const missing = new Set();
  for (const [index, image] of (document.images ?? []).entries()) {
    if (typeof image.uri !== 'string' || image.uri.startsWith('data:')) continue;
    const declared = path.join(base, image.uri);
    try { if ((await fs.stat(declared)).isFile()) continue; } catch { /* fall through to basename lookup */ }
    const found = images.get(path.basename(image.uri).toLowerCase());
    if (found) {
      requireAsset((await fs.stat(found)).size <= 256 * 1024 * 1024, 'Converted image exceeds 256 MiB', 'BUDGET');
      const target = path.join(base, path.basename(image.uri));
      await fs.mkdir(path.dirname(target), { recursive: true });
      await fs.copyFile(found, target);
      image.uri = path.basename(image.uri);
      continue;
    }
    missing.add(index);
  }
  if (missing.size) {
    // Detach only the slots that point at an unrecoverable image; the remaining
    // material inputs still import instead of failing the whole model.
    for (const [index, texture] of (document.textures ?? []).entries()) {
      if (!missing.has(texture.source)) continue;
      for (const material of document.materials ?? []) dropTexture(material, index);
    }
    warnings.push(`${missing.size} source texture${missing.size === 1 ? '' : 's'} could not be found next to the model; affected material slots use their factor values.`);
  }
  if (Array.isArray(document.extensionsUsed)) {
    const kept = document.extensionsUsed.filter((name) => !CONVERTER_EXTENSIONS.has(name));
    if (kept.length !== document.extensionsUsed.length) {
      if (kept.length) document.extensionsUsed = kept; else delete document.extensionsUsed;
    }
  }
  await fs.writeFile(main, JSON.stringify(document));
  return document;
}

export async function convertModel(source, directory) {
  assimp??=assimpFactory();const api=await assimp,files=new api.FileList();
  const sourceBytes=await fs.readFile(source);
  files.AddFile(path.basename(source),sourceBytes);
  // Supply common sidecars and images from the source directory, with a hard total
  // budget. The converter never receives arbitrary filesystem or network access.
  let total=0;
  for(const entry of await fs.readdir(path.dirname(source),{withFileTypes:true})) {
    if(!entry.isFile()||!SIDECAR.test(entry.name))continue;
    const file=path.join(path.dirname(source),entry.name),stat=await fs.stat(file);total+=stat.size;
    requireAsset(total<=256*1024*1024,'Model sidecars exceed 256 MiB','BUDGET');files.AddFile(entry.name,await fs.readFile(file));
  }
  const result=api.ConvertFileList(files,'gltf2');
  requireAsset(result.IsSuccess()&&result.FileCount()>0,`Model conversion failed: ${result.GetErrorCode()}`,'CONVERT');
  await fs.mkdir(directory,{recursive:true});let main;
  for(let i=0;i<result.FileCount();i++) {
    const file=result.GetFile(i),name=file.GetPath();
    requireAsset(typeof name==='string'&&!path.isAbsolute(name)&&!name.split(/[\\/]/).includes('..')&&!name.includes(':'),'Converter output escaped package','PATH');
    const target=path.join(directory,name);await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,file.GetContent());
    if(name.endsWith('.gltf'))main=target;
  }
  requireAsset(main,'Converter did not emit a glTF document','CONVERT');
  const warnings=[];
  const metadata=readFbxMetadata(sourceBytes);
  await repairConvertedDocument(main,path.dirname(source),warnings);
  if(metadata?.format){
    const described=describeFbx(metadata);
    warnings.unshift(`Converted ${metadata.format} FBX${metadata.version?` ${metadata.version}`:''}: ${described.unitScaleFactor} cm per unit, ${described.upAxis} up, ${described.frontAxis} front.`);
  }
  return {file:main,metadata,coordinates:fbxCoordinateOptions(metadata),warnings};
}
export async function convertAudio(source,target) {
  requireAsset(ffmpeg,'FFmpeg binary is unavailable','CONVERT');
  await new Promise((resolve,reject)=>{
    const child=spawn(ffmpeg,['-nostdin','-hide_banner','-loglevel','error','-protocol_whitelist','file,pipe','-i',source,'-vn','-ac','2','-ar','48000','-c:a','pcm_s16le',target],{windowsHide:true,shell:false,stdio:['ignore','ignore','pipe']});
    let error='';child.stderr.on('data',b=>error=(error+b).slice(-8192));
    const timer=setTimeout(()=>{child.kill();reject(new AssetError('CONVERT','Audio conversion exceeded 120 seconds'));},120000);
    child.once('error',e=>{clearTimeout(timer);reject(e);});child.once('exit',code=>{clearTimeout(timer);if(code===0)resolve();else reject(new AssetError('CONVERT',error||'Audio decode failed'));});
  });
}

import fs from 'node:fs/promises';
import path from 'node:path';
import {loadScene,buildScene} from '../scene/index.mjs';
import {findGameplayAssets} from '../scene/gameplay.mjs';
import {sha256Hex} from './canonical.mjs';
import {requireAsset} from './gltf-document.mjs';
import {inspectNgsnd} from './audio-data.mjs';
import {sceneResourceRoot} from '../scene/paths.mjs';

// Preserve entity/project ids and all cross references. A scene is imported as
// a complete project fragment; merging it as a prefab has a separate contract.
export async function cookSceneImport(source,staging) {
  const scene=await loadScene(source),sceneRoot=path.dirname(source),assets=findGameplayAssets(source),extra=[];
  const read=async(name,root)=>{
    requireAsset(typeof name==='string'&&name&&!/[\\:\x00-\x1f]/.test(name)&&!name.startsWith('/')&&!name.split('/').some(p=>!p||p==='.'||p==='..'),'Invalid scene dependency path','PATH');
    const base=await fs.realpath(root),file=await fs.realpath(path.join(base,name)),relative=path.relative(base,file);
    requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Scene dependency escaped its source root','PATH');
    requireAsset((await fs.stat(file)).size<=256*1024*1024,'Scene dependency exceeds 256 MiB','BUDGET');return fs.readFile(file);
  };
  let packageFiles,manifest;
  try {const manifestFile=path.join(sceneRoot,'manifest.json');requireAsset((await fs.stat(manifestFile)).size<=16*1024*1024,'Scene manifest exceeds 16 MiB','BUDGET');manifest=JSON.parse(await fs.readFile(manifestFile,'utf8'));}catch(error){if(error.code!=='ENOENT')throw error;}
  if(manifest) {
    if(manifest.tool?.name==='nativegpu-scene-build') {
      requireAsset(manifest.entryScene===path.basename(source)&&Array.isArray(manifest.files)&&manifest.files.length<=8192,'Invalid scene package manifest','DEPENDENCY');packageFiles=new Map();let total=0;
      for(const entry of manifest.files){requireAsset(!packageFiles.has(entry.path)&&Number.isSafeInteger(entry.bytes)&&entry.bytes>=0&&/^[a-f0-9]{64}$/.test(entry.sha256),'Invalid scene package file entry','DEPENDENCY');total+=entry.bytes;requireAsset(total<=1024*1024*1024,'Scene package exceeds 1 GiB','BUDGET');const bytes=await read(entry.path,sceneRoot);requireAsset(bytes.length===entry.bytes&&sha256Hex(bytes)===entry.sha256,'Scene package dependency changed','HASH');packageFiles.set(entry.path,entry.sha256);}
      requireAsset(packageFiles.has(manifest.entryScene),'Scene entry is missing from package manifest','DEPENDENCY');
    }
  }
  for(const entity of scene.entities)if(entity.destructible?.voronoi) {
    const name=entity.destructible.voronoi;let bytes;
    try{bytes=await read(name,sceneRoot);}catch(error){if(error.code!=='ENOENT')throw error;bytes=await read(name.replace(/^assets\//,''),assets);}
    const file=`simulation/${sha256Hex(bytes).slice(0,32)}.voronoi.json`;entity.destructible.voronoi=file;extra.push({file,bytes});
  }
  if(scene.audio?.emitters?.some(e=>!Object.hasOwn(scene.audio.sounds??{},e.sound))) {
    const bank=JSON.parse(await read('audio/bank.json',assets));scene.audio.sounds??={};
    for(const emitter of scene.audio.emitters)if(!Object.hasOwn(scene.audio.sounds,emitter.sound)) {
      const sound=bank.sounds.find(s=>s.name===emitter.sound);requireAsset(sound,'Scene sound is missing from its source bank','DEPENDENCY');
      const bytes=await read(sound.file.replace(/^assets\//,''),assets),hash=sha256Hex(bytes);requireAsset(hash===sound.sha256,'Source sound bank hash differs','HASH');inspectNgsnd(bytes);
      const file=`audio/${hash.slice(0,32)}.ngsnd`;scene.audio.sounds[emitter.sound]={file,sha256:hash};extra.push({file,bytes});
    }
  }
  const original=structuredClone(scene);
  // Collect normal dependencies before augmenting relocated bank/fracture data.
  const regular=structuredClone(scene);
  for(const entity of regular.entities)if(entity.destructible?.voronoi)delete entity.destructible.voronoi;
  if(regular.audio?.sounds)for(const [name,ref]of Object.entries(regular.audio.sounds))if(extra.some(e=>e.file===ref.file))delete regular.audio.sounds[name];
  const built=await buildScene(regular,path.join(staging,'.scene-build'),{sceneDirectory:sceneResourceRoot(source,assets),assetsDirectory:assets});
  let total=0;const files=new Map();
  for(const entry of built.manifest.dependencies){const bytes=await fs.readFile(path.join(built.packageDirectory,entry.path));requireAsset(sha256Hex(bytes)===entry.sha256,'Scene dependency changed during import','HASH');if(packageFiles)requireAsset(packageFiles.get(entry.path)===entry.sha256,'Dependency is missing from scene package manifest','DEPENDENCY');files.set(entry.path,bytes);}
  for(const item of extra){requireAsset(!files.has(item.file)||files.get(item.file).equals(item.bytes),'Scene dependency collision','HASH');files.set(item.file,item.bytes);}
  const relocated=new Map(),removed=new Set(),report=[];
  const relocate=(from,to)=>{const bytes=files.get(from);requireAsset(bytes,'Scene dependency is missing','DEPENDENCY');requireAsset(!relocated.has(to)||relocated.get(to).equals(bytes),'Scene relocation path collision','HASH');relocated.set(to,bytes);removed.add(from);report.push({source:from,file:to,sha256:sha256Hex(bytes),bytes:bytes.length});return to;};
  for(const entity of original.entities){
    if(entity.mesh)entity.mesh.file=relocate(entity.mesh.file,`meshes/${entity.mesh.sha256.slice(0,32)}.mesh.json`);
    if(entity.character){const ref=entity.character,bin=path.posix.join(path.posix.dirname(ref.file),'animation.bin'),key=sha256Hex(ref.sha256+ref.binarySha256).slice(0,32);ref.file=relocate(ref.file,`characters/${key}/character.json`);relocate(bin,`characters/${key}/animation.bin`);}
  }
  for(const ref of Object.values(original.audio?.sounds??{}))ref.file=relocate(ref.file,`audio/${ref.sha256.slice(0,32)}.ngsnd`);
  if(original.lighting?.environment){const ref=original.lighting.environment;ref.file=relocate(ref.file,`environment/${ref.sha256.slice(0,32)}.ngenv`);}
  for(const name of removed)files.delete(name);for(const [name,bytes]of relocated){requireAsset(!files.has(name)||files.get(name).equals(bytes),'Scene relocation path collision','HASH');files.set(name,bytes);}
  files.set('scene-relocation.json',Buffer.from(JSON.stringify({schema:'nativegpu.scene-relocation/1',files:report})+'\n'));
  for(const [file,bytes]of files){total+=bytes.length;requireAsset(total<=1024*1024*1024,'Scene dependencies exceed 1 GiB','BUDGET');const target=path.join(staging,file);await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,bytes);}
  await fs.rm(path.join(staging,'.scene-build'),{recursive:true,force:true});
  return {scene:original,dependencies:files.size};
}

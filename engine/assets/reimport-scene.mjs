import fs from 'node:fs/promises';
import path from 'node:path';
import {canonicalJson,sha256Hex} from './canonical.mjs';
import {requireAsset} from './gltf-document.mjs';
import {verifyImport,planImportAttachment} from './import-service.mjs';
import {compareSourceImports} from './reimport.mjs';
import {loadScene,validateScene,sceneAssets,commitSceneUpdate,SceneError} from '../scene/index.mjs';
import {TEXTURE_SLOTS} from '../material/texture-mapping.mjs';
import {sceneResourceRoot} from '../scene/paths.mjs';

const equal=(a,b)=>a===undefined||b===undefined?a===b:canonicalJson(a)===canonicalJson(b);
const object=value=>value!==null&&typeof value==='object'&&!Array.isArray(value);
// Merge individual source fields. Arrays are authored values, not indexed lists
// of editable children. Missing fields also carry meaning (e.g. a removed map).
export function mergeImportedMaterial(before,incoming,current){
  if(equal(before,current))return structuredClone(incoming);
  if(object(before)&&object(incoming)&&object(current)){
    const result={};for(const key of new Set([...Object.keys(before),...Object.keys(incoming),...Object.keys(current)])){
      const value=mergeImportedMaterial(before[key],incoming[key],current[key]);if(value!==undefined)result[key]=value;
    }return result;
  }
  return structuredClone(current);
}
const inside=(root,file)=>{const relative=path.relative(root,file);return relative!==''&&relative!=='..'&&!relative.startsWith('..'+path.sep)&&!path.isAbsolute(relative);};
const sourceMaterial=(manifest,name)=>{const material=structuredClone(manifest.materials[name]),prefix=manifest.id.slice(0,12)+'_';for(const slot of TEXTURE_SLOTS)if(material[slot])material[slot]=prefix+material[slot];return material;};

// Publish only immutable dependencies. Scene replacement, including its undo
// record, remains a single revision-checked commit after validation succeeds.
export async function updateSceneImport({previousDirectory,directory,scenePath,expectedRevision,entityIds}={}){
  const previous=await verifyImport(previousDirectory),next=await verifyImport(directory),comparison=compareSourceImports(previous,next);
  requireAsset(['model','character','texture','audio','environment'].includes(next.kind),'This asset type has no scene reference updater yet','UNSUPPORTED');
  const scene=await loadScene(scenePath);if(!Number.isSafeInteger(expectedRevision)||scene.revision!==expectedRevision)throw new SceneError('REVISION_CONFLICT','Scene revision changed');
  requireAsset(entityIds===undefined||Array.isArray(entityIds)&&entityIds.length>0&&entityIds.every(id=>typeof id==='string')&&new Set(entityIds).size===entityIds.length,'entityIds must contain unique entity IDs','ARGUMENT');
  if(entityIds)for(const id of entityIds)requireAsset(scene.entities.some(e=>e.id===id),'Selected scene entity is missing','ARGUMENT');
  requireAsset(!entityIds||['model','character'].includes(next.kind),'Entity selection requires a model or character','ARGUMENT');
  if(comparison.unchanged)return {scene,report:{...comparison,updatedEntities:[],updatedReferences:0,sceneReferencesUpdated:false}};
  const parent=await fs.realpath(sceneResourceRoot(scenePath)),attachment=await planImportAttachment(directory,parent,next),proposed=structuredClone(scene),prefix=next.id.slice(0,12)+'_',oldPrefix=previous.id.slice(0,12)+'_',updatedEntities=[],materialAliases=new Map(),packages=new Map();let updatedReferences=0;
  async function matches(ref,file,hash){
    if(ref?.sha256!==hash||!ref.file.endsWith('/'+file))return false;
    const packageDirectory=path.resolve(parent,ref.file.slice(0,-file.length));if(!inside(parent,packageDirectory))return false;
    if(!packages.has(packageDirectory))packages.set(packageDirectory,(async()=>{try{const real=await fs.realpath(packageDirectory);requireAsset(inside(parent,real),'Scene import reference escapes scene directory','PATH');return (await verifyImport(real)).id;}catch(error){if(error.code==='ENOENT')return null;throw error;}})());
    return await packages.get(packageDirectory)===previous.id;
  }
  const materialMatches=new Map(comparison.materials.matched.map(p=>[p.beforeName,p.afterName]));
  function updatedMaterial(beforeName,currentName,afterName=materialMatches.get(beforeName)){
    const authoredName=oldPrefix+beforeName;
    // A deliberately reassigned user material stays assigned. Imported material
    // aliases created by an earlier update still participate in the field merge.
    if(typeof currentName!=='string'||currentName!==authoredName&&!new RegExp('^'+authoredName+'_u[a-f0-9]{8}$').test(currentName))return currentName;
    requireAsset(afterName,'A referenced source material no longer has an unambiguous match','CONFLICT');
    const key=beforeName+'\0'+afterName+'\0'+currentName;if(materialAliases.has(key))return materialAliases.get(key);
    const current=proposed.materials?.[currentName];requireAsset(current,'Referenced imported material is missing','DEPENDENCY');
    const material=mergeImportedMaterial(sourceMaterial(previous,beforeName),sourceMaterial(next,afterName),current);let name=prefix+afterName;
    proposed.materials??={};if(proposed.materials[name]&&!equal(proposed.materials[name],material))name+='_u'+sha256Hex(canonicalJson(material)).slice(0,8);
    requireAsset(!proposed.materials[name]||equal(proposed.materials[name],material),'Updated material name conflicts with scene content','HASH');proposed.materials[name]=material;materialAliases.set(key,name);return name;
  }
  if(next.kind==='model'){
    const mapped=new Map(comparison.parts.matched.map(p=>[p.before,p.after])),partKeys=new Map(),geometry=new Map();
    for(const [index,part]of previous.parts.entries()){for(const [map,key]of [[partKeys,part.source?.key],[geometry,part.mesh+'\0'+part.sha256]]){const values=map.get(key)??[];values.push(index);map.set(key,values);}}
    for(const entity of proposed.entities){
      if(entityIds&&!entityIds.includes(entity.id))continue;if(entity.imported&&entity.imported.asset!==previous.id)continue;
      const candidates=[],indices=entity.imported?partKeys.get(entity.imported.part)??[]:previous.parts.length&&entity.mesh?geometry.get(entity.mesh.file.split('/').slice(-2).join('/')+'\0'+entity.mesh.sha256)??[]:[];for(const index of indices){const part=previous.parts[index];if(await matches(entity.mesh,part.mesh,part.sha256))candidates.push(index);}
      if(entity.imported?.asset===previous.id)requireAsset(candidates.length===1,'Imported entity geometry was replaced or its source part is ambiguous','CONFLICT');
      if(!candidates.length)continue;requireAsset(candidates.length===1,'Legacy scene geometry has multiple source parts; attach it again to record source identity','CONFLICT');
      requireAsset(!comparison.parts.added.length&&!comparison.parts.removed.length&&!comparison.parts.ambiguous.length,'Source part structure changed; review the reimport plan before replacing scene geometry','CONFLICT');
      const before=previous.parts[candidates[0]],after=next.parts[mapped.get(candidates[0])];requireAsset(after,'Source part has no unambiguous match','CONFLICT');
      // Imported rigid geometry is normalized to its bounds. Preserve the user's
      // per-axis scale and displacement relative to the old source bounds.
      const scale=entity.scale.map((value,i)=>value/before.scale[i]),offset=entity.position.map((value,i)=>value-scale[i]*before.position[i]);entity.position=after.position.map((value,i)=>offset[i]+scale[i]*value);entity.scale=after.scale.map((value,i)=>scale[i]*value);
      entity.mesh={file:attachment.relative+'/'+after.mesh,sha256:after.sha256};entity.material=updatedMaterial(before.material,entity.material,after.material);
      entity.imported={asset:next.id,part:after.source?.key??sha256Hex(after.mesh),instance:entity.imported?.instance??entity.id};updatedEntities.push(entity.id);updatedReferences++;
    }
  }else if(next.kind==='character'){
    const oldFile=previous.files.find(f=>f.file===previous.character),file=next.files.find(f=>f.file===next.character),binary=next.files.find(f=>f.file==='character/animation.bin'),header=JSON.parse(await fs.readFile(path.join(directory,next.character),'utf8')),beforeParts=new Map(comparison.parts.matched.map(p=>[p.after,p.before])),beforeMaterials=new Map(comparison.materials.matched.map(p=>[p.afterName,p.beforeName]));
    for(const entity of proposed.entities){
      if(entityIds&&!entityIds.includes(entity.id))continue;if(entity.imported&&entity.imported.asset!==previous.id)continue;
      const match=await matches(entity.character,previous.character,oldFile.sha256);if(entity.imported?.asset===previous.id)requireAsset(match,'Imported character reference was replaced','CONFLICT');if(!match)continue;
      const ref=entity.character;requireAsset(!ref.clip||header.clips.some(c=>c.name===ref.clip),'Selected character clip was removed; choose a replacement before updating','CONFLICT');const materials={};
      for(const [index,part]of next.parts.entries()){
        const beforeIndex=beforeParts.get(index),beforeName=beforeIndex===undefined?beforeMaterials.get(part.material):previous.parts[beforeIndex].material;
        if(!beforeName){requireAsset(!comparison.parts.ambiguous.length,'Character parts and materials have no unambiguous source correspondence','CONFLICT');continue;}
        const name=updatedMaterial(beforeName,ref.materials?.[beforeName]??oldPrefix+beforeName,part.material);requireAsset(!materials[part.material]||materials[part.material]===name,'Source character materials were merged but scene overrides differ','CONFLICT');materials[part.material]=name;
      }
      for(const part of next.parts)if(!materials[part.material]){const name=prefix+part.material,material=sourceMaterial(next,part.material);proposed.materials??={};requireAsset(!proposed.materials[name]||equal(proposed.materials[name],material),'Character material conflicts with scene content','HASH');proposed.materials[name]=material;materials[part.material]=name;}
      Object.assign(ref,{file:attachment.relative+'/'+next.character,sha256:file.sha256,binarySha256:binary.sha256,materials});entity.imported={asset:next.id,part:sha256Hex('character-root'),instance:entity.imported?.instance??entity.id};updatedEntities.push(entity.id);updatedReferences++;
    }
  }else if(next.kind==='texture'){
    requireAsset(previous.semantic===next.semantic,'Reimport cannot change the texture semantic','ARGUMENT');const beforeName=previous.id.slice(0,16)+'_texture',afterName=next.id.slice(0,16)+'_texture';
    for(const material of Object.values(proposed.materials??{}))if(material[next.semantic]===beforeName){const old=previous.files.find(f=>f.file===`textures/texture.${next.semantic}.ngtex`),file=await fs.realpath(path.join(parent,'textures',beforeName+'.'+next.semantic+'.ngtex'));requireAsset(inside(parent,file)&&sha256Hex(await fs.readFile(file))===old.sha256,'Scene texture differs from the previous immutable import','HASH');material[next.semantic]=afterName;updatedReferences++;}
  }else if(next.kind==='audio'){
    const beforeBank=JSON.parse(await fs.readFile(path.join(previousDirectory,previous.bank),'utf8')),afterBank=JSON.parse(await fs.readFile(path.join(directory,next.bank),'utf8'));
    for(const ref of Object.values(proposed.audio?.sounds??{}))for(const before of beforeBank.sounds)if(await matches(ref,before.file,before.sha256)){
      const after=afterBank.sounds.find(s=>s.name===before.name)??(beforeBank.sounds.length===1&&afterBank.sounds.length===1?afterBank.sounds[0]:undefined);requireAsset(after,'Referenced sound has no unambiguous match','CONFLICT');Object.assign(ref,{file:attachment.relative+'/'+after.file,sha256:after.sha256});updatedReferences++;break;
    }
  }else if(next.kind==='environment'){
    const ref=proposed.lighting?.environment;if(await matches(ref,previous.environment,previous.environmentSha256)){Object.assign(ref,{file:attachment.relative+'/'+next.environment,sha256:next.environmentSha256});updatedReferences++;}
  }
  if(entityIds)requireAsset(entityIds.every(id=>updatedEntities.includes(id)),'Some selected entities do not reference the previous import','CONFLICT');
  requireAsset(updatedReferences>0,'The scene does not reference this asset version','ARGUMENT');
  const validation=validateScene(proposed,{assetsDirectory:sceneAssets(scene)});requireAsset(validation.valid,'Updated scene is invalid: '+validation.errors.join('; '),'VALIDATION');
  if(['model','character','texture'].includes(next.kind)){
    const textures=path.join(parent,'textures');await fs.mkdir(textures,{recursive:true});requireAsset(inside(parent,await fs.realpath(textures)),'Texture directory escapes scene','PATH');
    for(const entry of next.files.filter(f=>f.file.startsWith('textures/')&&f.file.endsWith('.ngtex'))){const name=(next.kind==='texture'?next.id.slice(0,16)+'_':prefix)+path.basename(entry.file),target=path.join(textures,name),bytes=await fs.readFile(path.join(directory,entry.file));requireAsset(bytes.length===entry.bytes&&sha256Hex(bytes)===entry.sha256,'Imported texture changed during update','HASH');try{await fs.writeFile(target,bytes,{flag:'wx'});}catch(error){if(error.code!=='EEXIST')throw error;requireAsset(inside(parent,await fs.realpath(target))&&sha256Hex(await fs.readFile(target))===entry.sha256,'Texture destination conflicts with immutable content','HASH');}}
  }
  await attachment.publish();await verifyImport(previousDirectory);await verifyImport(directory);
  const changed=await commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},()=>proposed);
  return {scene:changed,report:{...comparison,updatedEntities,updatedReferences,sceneReferencesUpdated:true}};
}

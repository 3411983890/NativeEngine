#!/usr/bin/env node
// Material patch surface for editors and AI agents.
//
// A patch is a typed change to one entry of scene.materials. The result is validated with
// the same validator the runtime uses, previewed as a diff with the affected entity count,
// and only then written atomically. This is deliberately not a free-form JSON editor.
import { existsSync, readFileSync, statSync, readdirSync, realpathSync } from 'node:fs';
import { basename, dirname, join, resolve, relative, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';
import { validateScene, commitSceneUpdate, sceneAssets } from '../engine/scene/index.mjs';
import {findGameplayAssets} from '../engine/scene/gameplay.mjs';
import { compileMir, materialToMir } from '../engine/material/mir.mjs';
import { compileGraph } from '../engine/material/graph.mjs';
import {validateTextureMappings} from '../engine/material/texture-mapping.mjs';
import {validateIor} from '../engine/material/ior.mjs';
import {normalizeLayeredMaterial} from '../engine/material/layered.mjs';
import {readNgtex} from './textures.mjs';

export const MATERIAL_FIELDS = Object.freeze([
  'albedo', 'normal', 'orm', 'uvScale', 'roughness', 'metalness', 'wetness', 'wear',
  'clearcoat', 'clearcoatRoughness', 'ior', 'alphaMode', 'alphaCutoff', 'emissive', 'emissiveFactor', 'emissiveStrength', 'occlusion', 'coat', 'coatRoughness', 'coatNormal', 'occlusionStrength', 'textureMappings', 'unlit', 'opacity', 'layered',
]);
const TEXTURE_FIELDS = new Set(['albedo', 'normal', 'orm', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal']);
const SCALAR_FIELDS = new Set(['opacity', 'ior', 'uvScale', 'roughness', 'metalness', 'wetness', 'wear',
  'clearcoat', 'clearcoatRoughness', 'alphaCutoff', 'emissiveStrength', 'occlusionStrength']);
const MAX_SCENE_BYTES = 16 * 1024 * 1024;

/** Texture base names available in the cooked manifest (shared by editor/MCP). */
function textureDirectory(root,scenePath){
  const assets=resolve(root,'assets'),parent=scenePath?dirname(resolve(root,scenePath)):assets;
  const sceneTextures=join(parent,'textures'),candidates=basename(parent)==='.screen'?[sceneTextures,join(assets,'textures')]:[sceneTextures];
  for(const directory of candidates){
    if(!existsSync(directory))continue;
    const real=realpathSync(directory),bases=basename(parent)==='.screen'?[parent,assets]:[parent];
    const allowed=bases.some(base=>{const child=relative(realpathSync(base),real);return child&&!child.startsWith('..')&&!isAbsolute(child);});
    if(!allowed)throw new Error('texture directory escapes the scene');
    return directory;
  }
  return sceneTextures;
}
function textureFile(directory,name){const file=join(directory,name);if(existsSync(file)){const child=relative(realpathSync(dirname(directory)),realpathSync(file));if(!child||child.startsWith('..')||isAbsolute(child))throw new Error('texture file escapes the scene');}return file;}
export function textureNames(root = process.cwd(),scenePath=null) {
  const manifestPath = join(textureDirectory(root,scenePath), 'manifest.json');
  const names=new Set();
  try {
    if(existsSync(manifestPath))for(const entry of JSON.parse(readFileSync(manifestPath,'utf8')).textures??[])if(typeof entry.name==='string')names.add(entry.name);
  } catch {}
  try{for(const file of readdirSync(dirname(manifestPath),{withFileTypes:true}))if(file.isFile()){const match=/^([A-Za-z0-9_.-]{1,64})\.(?:albedo|normal|orm|emissive|occlusion|coat|coatRoughness|coatNormal|height|mask|roughness|metallic)\.ngtex$/.exec(file.name);if(match)names.add(match[1]);}}catch{}
  return [...names].sort();
}

function validateTextureChanges(root, changes,scenePath=null) {
  const textureRoot = textureDirectory(root,scenePath);
  if (!existsSync(textureRoot)) {
    if(changes.layered){const material=normalizeLayeredMaterial(changes.layered);if(material.layers.some(layer=>Object.keys(layer.textures).length||layer.blend.texture))throw new Error('layered material texture directory is missing');}
    return;
  }
  for (const [key, value] of Object.entries(changes)) {
    if(key==='layered'&&value!==null){
      const material=normalizeLayeredMaterial(value);
      const checkTexture=(semantic,ref)=>{const file=textureFile(textureRoot,`${ref.name}.${semantic}.ngtex`);if(!existsSync(file)||!statSync(file).isFile())throw new Error(`texture ${ref.name}.${semantic}.ngtex is not cooked`);const bytes=readFileSync(file),image=readNgtex(bytes,file);if(image.semantic!==semantic||image.srgb!==(semantic==='albedo'||semantic==='emissive'))throw new Error(`texture ${ref.name}.${semantic}.ngtex has incorrect semantic or color space`);if(semantic==='height'&&image.format!==0&&image.format!==5)throw new Error(`texture ${ref.name}.${semantic}.ngtex must use RGBA8 or R16`);const count=image.width&&image.height?1+Math.floor(Math.log2(Math.max(image.width,image.height))):0;if(!count||image.mipCount!==count||bytes.readUInt32LE(20)+bytes.readUInt32LE(16)!==bytes.length)throw new Error(`texture ${ref.name}.${semantic}.ngtex has invalid dimensions, mip chain or payload`);};
      for(const layer of material.layers){
        const refs=[...Object.entries(layer.textures),...(layer.blend.texture?[['mask',layer.blend.texture]]:[])];
        for(const [semantic,ref] of refs)checkTexture(semantic,ref);
      }
      if(material.surfaceRelief?.heightSource.kind==='texture')checkTexture('height',material.surfaceRelief.heightSource.texture);
      continue;
    }
    if (!TEXTURE_FIELDS.has(key) || typeof value !== 'string') continue;
    const file=textureFile(textureRoot, `${value}.${key}.ngtex`);
    if (!existsSync(file)||!statSync(file).isFile()) {
      throw new Error(`texture ${value}.${key}.ngtex is not cooked`);
    }
  }
}

function assertObject(value, label) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
}

function readScene(scenePath) {
  const path = resolve(scenePath);
  if (!existsSync(path)) throw new Error(`scene not found: ${scenePath}`);
  if (statSync(path).size > MAX_SCENE_BYTES) throw new Error('scene exceeds 16 MiB');
  const scene = JSON.parse(readFileSync(path, 'utf8'));
  const validation = validateScene(scene,{assetsDirectory:findGameplayAssets(path)});
  if (!validation.valid) throw new Error(`scene is invalid: ${validation.errors.join('; ')}`);
  return { path, scene };
}

function textureBudget(root, scene, onlyMaterial = null,scenePath=null) {
  const directory=textureDirectory(root,scenePath),manifestPath = join(directory, 'manifest.json');
  if (!existsSync(directory)) return null;
  try {
    const manifest = existsSync(manifestPath)?JSON.parse(readFileSync(manifestPath, 'utf8')):{textures:[]};
    const byName = new Map((manifest.textures ?? []).map((entry) => [`${entry.name}.${entry.semantic}`, entry]));
    let totalBytes=0;for(const file of readdirSync(directory,{withFileTypes:true}))if(file.isFile()&&file.name.endsWith('.ngtex')){const key=file.name.slice(0,-6),bytes=statSync(textureFile(directory,file.name)).size;byName.set(key,{...byName.get(key),bytes});totalBytes+=bytes;}
    let bytes = 0;
    let files = 0;
    const counted = new Set();
    const materials = onlyMaterial ? [scene.materials?.[onlyMaterial]].filter(Boolean) : Object.values(scene.materials ?? {});
    for (const material of materials) {
      for (const semantic of ['albedo', 'normal', 'orm', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal']) {
        const base = material[semantic];
        if (typeof base !== 'string') continue;
        const entry = byName.get(`${base}.${semantic}`);
        const key = `${base}.${semantic}`;
        if (entry && !counted.has(key)) {
          counted.add(key);
          bytes += entry.bytes;
          files += 1;
        }
      }
      for(const layer of material.layered?.layers??[]){
        const refs=[...Object.entries(layer.textures),...(layer.blend.texture?[['mask',layer.blend.texture]]:[])];
        for(const [semantic,ref] of refs){const key=`${ref.name}.${semantic}`,entry=byName.get(key);if(entry&&!counted.has(key)){counted.add(key);bytes+=entry.bytes;files++;}}
      }
    }
    return { bytes, files, totalBytes };
  } catch {
    return null;
  }
}

/** Apply a typed material change to a scene clone; returns a new scene and a diff. */
export function patchMaterial(scene, materialName, changes) {
  assertObject(scene, 'scene');
  assertObject(changes, 'changes');
  if (typeof materialName !== 'string' || !materialName) throw new Error('material name is required');
  const names = Object.keys(changes);
  if (!names.length) throw new Error('changes must not be empty');
  for (const key of names) {
    if (!MATERIAL_FIELDS.includes(key)) throw new Error(`unsupported material field: ${key}`);
  }
  const next = structuredClone(scene);
  if (!next.materials || !Object.hasOwn(next.materials, materialName)) {
    throw new Error(`unknown material: ${materialName}`);
  }
  const before = { ...next.materials[materialName] };
  const after = { ...before };
  for (const key of names) {
    const value = changes[key];
    if(key==='layered'){if(value===null){delete after.layered;continue;}after.layered=normalizeLayeredMaterial(value);continue;}
    if(key==='textureMappings'){if(value===null){delete after.textureMappings;continue;}validateTextureMappings(value);}
    if(['emissive','occlusion','coat','coatRoughness','coatNormal'].includes(key)&&value===null){delete after[key];continue;}
    if (TEXTURE_FIELDS.has(key)) {
      if (typeof value !== 'string' || !/^[A-Za-z0-9_.-]{1,64}$/.test(value)) {
        throw new Error(`${key} must be a texture base name`);
      }
    } else if(key==='emissiveFactor') {
      if(!Array.isArray(value)||value.length!==3||value.some(v=>!Number.isFinite(v)||v<0||v>1))throw new Error('emissiveFactor must contain three numbers in [0,1]');
    } else if(key==='unlit') {
      if(typeof value!=='boolean')throw new Error('unlit must be boolean');
    } else if(key==='alphaMode') {
      if(!['OPAQUE','MASK','BLEND'].includes(value))throw new Error('alphaMode must be OPAQUE, MASK or BLEND');
    } else if(key==='ior') {
      validateIor(value);
    } else if (SCALAR_FIELDS.has(key)) {
      if (!Number.isFinite(value)) throw new Error(`${key} must be a finite number`);
      const high = key === 'uvScale' ? 1000 : key==='emissiveStrength'?1000000:1;
      const low = key === 'uvScale' ? 0.05 : 0;
      if (value < low || value > high) throw new Error(`${key} must be in [${low}, ${high}]`);
    }
    after[key] = structuredClone(value);
  }
  next.materials[materialName] = after;
  next.revision = (Number.isSafeInteger(next.revision) ? next.revision : 0) + 1;
  const validation = validateScene(next,{assetsDirectory:sceneAssets(scene)});
  if (!validation.valid) throw new Error(`patched scene is invalid: ${validation.errors.join('; ')}`);
  const affected = (next.entities ?? []).filter((entity) => usesMaterial(entity,materialName));
  return {
    scene: next,
    material: materialName,
    before,
    after,
    changed: names.filter((key) => before[key] !== after[key]),
    affectedEntities: affected.length,
    affectedNames: affected.slice(0, 20).map((entity) => entity.name),
    revision: next.revision,
  };
}

function usesMaterial(entity,name){return entity.character?Object.values(entity.character.materials??{}).includes(name):entity.material===name;}
export function listMaterials(scene, root = process.cwd(),scenePath=null) {
  const budget = textureBudget(root, scene,null,scenePath);
  return Object.entries(scene.materials ?? {}).map(([name, material]) => ({
    name,
    ...material,
    entityCount: (scene.entities ?? []).filter((entity) => usesMaterial(entity,name)).length,
    sharedBudget: textureBudget(root, scene, name,scenePath),
  }));
}

export function previewMaterialPatch({ scenePath, material, changes, root = process.cwd() }) {
  const { scene } = readScene(scenePath);
  validateTextureChanges(root, changes,scenePath);
  const result = patchMaterial(scene, material, changes);
  return {
    ok: true,
    applied: false,
    scenePath,
    baseRevision: scene.revision,
    revision: result.revision,
    material: result.material,
    before: result.before,
    after: result.after,
    changed: result.changed,
    affectedEntities: result.affectedEntities,
    affectedNames: result.affectedNames,
    textureBudget: textureBudget(root, result.scene, result.material,scenePath),
  };
}

export async function applyMaterialPatch({ scenePath, material, changes, expectedRevision, projectId, root = process.cwd() }) {
  const { path, scene } = readScene(scenePath);
  validateTextureChanges(root, changes,scenePath);
  const result = patchMaterial(scene, material, changes);
  await commitSceneUpdate(path, { projectId: projectId ?? scene.projectId, expectedRevision }, () => result.scene);
  return {
    ok: true,
    applied: true,
    scenePath,
    revision: result.revision,
    material: result.material,
    before: result.before,
    after: result.after,
    changed: result.changed,
    affectedEntities: result.affectedEntities,
    textureBudget: textureBudget(root, result.scene, result.material,scenePath),
  };
}

/** Clone an existing material under a new name and optionally bind it to one entity. */
export function materialInstance(scene, source, name, entityId = null, sourceSlot = null) {
  assertObject(scene, 'scene');
  if (!/^[A-Za-z0-9_.-]{1,64}$/.test(name ?? '')) throw new Error('instance name is invalid');
  if (source === name) throw new Error('instance name must differ from source');
  if (!scene.materials || !Object.hasOwn(scene.materials, source)) {
    throw new Error(`unknown material: ${source}`);
  }
  if (Object.hasOwn(scene.materials, name)) throw new Error(`material already exists: ${name}`);
  if(sourceSlot!==null&&(!entityId||typeof sourceSlot!=='string'||!/^[A-Za-z0-9_.-]{1,64}$/.test(sourceSlot)))throw new Error('sourceSlot requires an entity and valid character material slot');
  const next = structuredClone(scene);
  next.materials[name] = structuredClone(next.materials[source]);
  if (entityId) {
    const entity = (next.entities ?? []).find((candidate) => candidate.id === entityId);
    if (!entity) throw new Error(`unknown entity: ${entityId}`);
    if(entity.character) {
      const bindings=entity.character.materials??{},slots=sourceSlot===null?Object.keys(bindings).filter(slot=>bindings[slot]===source):[sourceSlot];
      if(!slots.length||slots.some(slot=>bindings[slot]!==source))throw new Error('character material source slot does not reference this material');
      for(const slot of slots)bindings[slot]=name;
    }else{if(sourceSlot!==null)throw new Error('sourceSlot requires a character entity');entity.material = name;}
  }
  next.revision = (Number.isSafeInteger(next.revision) ? next.revision : 0) + 1;
  const validation = validateScene(next,{assetsDirectory:sceneAssets(scene)});
  if (!validation.valid) throw new Error(`instance scene is invalid: ${validation.errors.join('; ')}`);
  return {
    scene: next,
    source,
    material: name,
    entityId,
    ...(sourceSlot!==null?{sourceSlot}:{}),
    revision: next.revision,
    after: { ...next.materials[name] },
  };
}

export function previewMaterialInstance({ scenePath, source, name, entityId = null, sourceSlot = null, root = process.cwd() }) {
  const { scene } = readScene(scenePath);
  const result = materialInstance(scene, source, name, entityId, sourceSlot);
  return {
    ok: true, applied: false, scenePath, baseRevision: scene.revision, source: result.source, material: result.material,
    entityId: result.entityId, revision: result.revision, after: result.after,
    ...(result.sourceSlot!==undefined?{sourceSlot:result.sourceSlot}:{}),
    textureBudget: textureBudget(root, result.scene, result.material,scenePath),
  };
}

export async function applyMaterialInstance({ scenePath, source, name, entityId = null, sourceSlot = null, expectedRevision, projectId, root = process.cwd() }) {
  const { path, scene } = readScene(scenePath);
  const result = materialInstance(scene, source, name, entityId, sourceSlot);
  await commitSceneUpdate(path, { projectId: projectId ?? scene.projectId, expectedRevision }, () => result.scene);
  return {
    ok: true, applied: true, scenePath, source: result.source, material: result.material,
    entityId: result.entityId, revision: result.revision, after: result.after,
    ...(result.sourceSlot!==undefined?{sourceSlot:result.sourceSlot}:{}),
    textureBudget: textureBudget(root, result.scene, result.material,scenePath),
  };
}

function parseArgs(argv) {
  const options = { apply: false };
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (!arg.startsWith('--')) continue;
    const key = arg.slice(2);
    if (key === 'apply') {
      options.apply = true;
      continue;
    }
    const value = argv[index + 1];
    if (value === undefined || value.startsWith('--')) throw new Error(`--${key} needs a value`);
    options[key] = value;
    index += 1;
  }
  return options;
}

async function main(argv) {
  const command = argv[0] ?? 'list';
  const options = parseArgs(argv.slice(1));
  const scenePath = options.scene ?? '.screen/town.scene.json';
  const root = resolve(dirname(scenePath), '..');
  if (command === 'list') {
    const { scene } = readScene(scenePath);
    process.stdout.write(`${JSON.stringify({ ok: true, scenePath, materials: listMaterials(scene, root,scenePath) }, null, 2)}\n`);
    return;
  }
  if (command === 'set' || command === 'preview') {
    if (!options.material) throw new Error('--material is required');
    const changes = {};
    for (const key of MATERIAL_FIELDS) {
      if (options[key] === undefined) continue;
      changes[key] = SCALAR_FIELDS.has(key) ? Number(options[key]) : options[key];
    }
    if (!Object.keys(changes).length) throw new Error('no material fields supplied');
    const result = (command === 'set' && options.apply)
      ? await applyMaterialPatch({ scenePath, material: options.material, changes, root, expectedRevision: Number(options.revision) })
      : previewMaterialPatch({ scenePath, material: options.material, changes, root });
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
    return;
  }
  if (command === 'graph') {
    if (!options.file) throw new Error('--file is required');
    const graph = JSON.parse(readFileSync(resolve(options.file), 'utf8'));
    const result = compileGraph(graph);
    process.stdout.write(`${JSON.stringify({ ok: true, file: options.file, ...result }, null, 2)}\n`);
    return;
  }
  if (command === 'mir') {
    if (!options.material) throw new Error('--material is required');
    const { scene } = readScene(scenePath);
    const material = scene.materials?.[options.material];
    if (!material) throw new Error(`unknown material: ${options.material}`);
    const mir = materialToMir(options.material, material);
    const compiled = compileMir(mir);
    process.stdout.write(`${JSON.stringify({ ok: true, scenePath, mir, compiled }, null, 2)}\n`);
    return;
  }
  if (command === 'instance') {
    if (!options.from) throw new Error('--from is required');
    if (!options.name) throw new Error('--name is required');
    const args = {
      scenePath, source: options.from, name: options.name,
      entityId: options.entity ?? null, root,
      sourceSlot:options['source-slot']??null,
    };
    const result = await (options.apply ? applyMaterialInstance : previewMaterialInstance)({ ...args, expectedRevision: Number(options.revision) });
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
    return;
  }
  throw new Error(`unknown command: ${command}`);
}

const invokedDirectly = process.argv[1]
  && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invokedDirectly) {
  try {
    await main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}

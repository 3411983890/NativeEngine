import { validateComponents, gameplayDependencies, withGameplayAssets, currentGameplayAssets, findGameplayAssets } from './gameplay.mjs';
import {inspectNgsnd,AUDIO_SCENE_BYTES_LIMIT,AUDIO_BYTES_LIMIT} from '../assets/audio-data.mjs';
import {validateTextureMappings} from '../material/texture-mapping.mjs';
import {validIor} from '../material/ior.mjs';
import {normalizeLayeredMaterial} from '../material/layered.mjs';
import {readNgtex} from '../../tools/textures.mjs';
import {inspectEnvironment} from '../assets/environment-container.mjs';
import {compileWater} from '../water/asset.mjs';
import { createHash, randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';

export const TOOL_VERSION = '0.3.0';
export const ENTITY_KINDS = Object.freeze(['ground', 'wall', 'prop', 'player', 'key', 'door', 'goal', 'light', 'vehicle']);
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const SCENE_KEYS = ['schemaVersion', 'projectId', 'name', 'revision', 'entities', 'settings'];
const SCENE_OPTIONAL_KEYS = ['materials', 'lighting', 'audio', 'joints', 'water', 'world'];
const ENTITY_KEYS = ['id', 'name', 'kind', 'position', 'scale', 'color', 'collider'];
const ENTITY_OPTIONAL_KEYS = ['material', 'light', 'vehicle', 'physics', 'destructible', 'components', 'mesh', 'character', 'imported', 'heightfield'];
const MATERIAL_KEYS = ['albedo', 'roughness', 'metalness', 'pattern'];
const NAMED_MATERIAL_REQUIRED = ['albedo'];
const NAMED_MATERIAL_OPTIONAL = ['normal', 'orm', 'emissive', 'emissiveFactor', 'emissiveStrength', 'occlusion', 'coat', 'coatRoughness', 'coatNormal', 'occlusionStrength', 'textureMappings', 'unlit', 'opacity', 'uvScale', 'roughness', 'metalness', 'wetness', 'wear', 'clearcoat', 'clearcoatRoughness', 'ior', 'alphaMode', 'alphaCutoff', 'layered'];
const TEXTURE_NAME = /^[A-Za-z0-9_.-]{1,64}$/;
const LIGHT_KEYS = ['color', 'intensity', 'range'];
const MATERIAL_PATTERNS = Object.freeze(['checker', 'brick', 'metal']);
const SETTINGS_KEYS = ['walkSpeed', 'runSpeed', 'jumpSpeed', 'gravity'];
const MAX_ENTITIES = 10000;
const MAX_FILE_BYTES = 16 * 1024 * 1024;
const SETTINGS_RANGE = Object.freeze({
  walkSpeed: [0.1, 30],
  runSpeed: [0.1, 60],
  jumpSpeed: [0.1, 30],
  gravity: [0.1, 100],
});

export class SceneError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'SceneError';
    this.code = code;
    this.details = details;
  }
}

function object(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function exactKeys(value, keys, location, errors, optional = []) {
  if (!object(value)) {
    errors.push(`${location} must be an object`);
    return false;
  }
  const allowed = [...keys, ...optional];
  for (const key of Object.keys(value)) {
    if (!allowed.includes(key)) errors.push(`${location}.${key} is not supported`);
  }
  for (const key of keys) {
    if (!Object.hasOwn(value, key)) errors.push(`${location}.${key} is required`);
  }
  return true;
}

function text(value) {
  return typeof value === 'string' && [...value].length > 0 && [...value].length <= 128
    && !/[\u0000-\u001f]/.test(value) && /\S/.test(value);
}

function vector(value, predicate) {
  return Array.isArray(value) && value.length === 3
    && value.every(number => Number.isFinite(number) && predicate(number));
}

function validateEntity(entity, location, errors, materialNames) {
  if (!exactKeys(entity, ENTITY_KEYS, location, errors, ENTITY_OPTIONAL_KEYS)) return;
  if(entity.imported!==undefined&&exactKeys(entity.imported,['asset','part','instance'],`${location}.imported`,errors)){
    for(const key of ['asset','part'])if(typeof entity.imported[key]!=='string'||!/^[a-f0-9]{64}$/.test(entity.imported[key]))errors.push(`${location}.imported.${key} must be a source SHA-256 key`);
    if(typeof entity.imported.instance!=='string'||!UUID.test(entity.imported.instance))errors.push(`${location}.imported.instance must be a UUID`);
    if(!entity.mesh&&!entity.character)errors.push(`${location}.imported requires mesh or character geometry`);
  }
  if (typeof entity.id !== 'string' || !UUID.test(entity.id)) errors.push(`${location}.id must be a UUID`);
  if (!text(entity.name)) errors.push(`${location}.name must contain 1-28 printable characters`);
  if (!ENTITY_KINDS.includes(entity.kind)) errors.push(`${location}.kind is unsupported`);
  if (!vector(entity.position, n => Math.abs(n) <= 100000)) errors.push(`${location}.position must contain three finite coordinates within ±100000`);
  if (!vector(entity.scale, n => n >= 0.001 && n <= 10000)) errors.push(`${location}.scale must contain three dimensions between 0.001 and 10000`);
  if (!vector(entity.color, n => n >= 0 && n <= 1)) errors.push(`${location}.color must contain three channels in [0, 1]`);
  if (typeof entity.collider !== 'boolean') errors.push(`${location}.collider must be boolean`);
  if(entity.mesh!==undefined && exactKeys(entity.mesh,['file','sha256'],`${location}.mesh`,errors)) {
    if(typeof entity.mesh.file!=='string'||!entity.mesh.file||/[\\:\x00]/.test(entity.mesh.file)||entity.mesh.file.startsWith('/')||entity.mesh.file.split('/').includes('..'))errors.push(`${location}.mesh.file must stay inside the scene directory`);
    if(!/^[a-f0-9]{64}$/.test(entity.mesh.sha256))errors.push(`${location}.mesh.sha256 must contain 64 lowercase hex digits`);
    if(entity.destructible)errors.push(`${location}: imported mesh destruction needs a separately baked fracture asset`);
  }
  if(entity.heightfield!==undefined&&exactKeys(entity.heightfield,['file','sha256','enabled','origin','size','minHeightMetres','maxHeightMetres','maxErrorMetres'],`${location}.heightfield`,errors)){
    const field=entity.heightfield,where=`${location}.heightfield`;
    if(typeof field.file!=='string'||!field.file.endsWith('.ngtex')||/[\\:\x00]/.test(field.file)||field.file.startsWith('/')||field.file.split('/').includes('..'))errors.push(`${where}.file must reference a cooked R16 height inside the scene directory`);
    if(!/^[a-f0-9]{64}$/.test(field.sha256))errors.push(`${where}.sha256 must contain 64 lowercase hex digits`);
    if(typeof field.enabled!=='boolean')errors.push(`${where}.enabled must be boolean`);
    for(const key of ['origin','size'])if(!Array.isArray(field[key])||field[key].length!==2||field[key].some(value=>!Number.isFinite(value)))errors.push(`${where}.${key} must contain two finite numbers`);
    if(Array.isArray(field.size)&&field.size.some(value=>!(value>0)))errors.push(`${where}.size must be positive`);
    for(const key of ['minHeightMetres','maxHeightMetres'])if(!Number.isFinite(field[key]))errors.push(`${where}.${key} must be finite`);
    if(Number.isFinite(field.maxHeightMetres)&&Number.isFinite(field.minHeightMetres)&&field.maxHeightMetres<field.minHeightMetres)errors.push(`${where}.maxHeightMetres must not be below minHeightMetres`);
    if(!Number.isFinite(field.maxErrorMetres)||field.maxErrorMetres<0||field.maxErrorMetres>1)errors.push(`${where}.maxErrorMetres must be in [0, 1]`);
  }
  if(entity.character!==undefined&&exactKeys(entity.character,['file','sha256','binarySha256'],`${location}.character`,errors,['clip','speed','timeOffset','yaw','scale','offset','rootMotion','materials','anchor','controller','rig'])) {
    const ref=entity.character,where=`${location}.character`;
    if(typeof ref.file!=='string'||!ref.file.endsWith('.json')||/[\\:\x00]/.test(ref.file)||ref.file.startsWith('/')||ref.file.split('/').includes('..'))errors.push(`${where}.file must reference a JSON header inside the scene directory`);
    for(const key of ['sha256','binarySha256'])if(!/^[a-f0-9]{64}$/.test(ref[key]))errors.push(`${where}.${key} must contain 64 lowercase hex digits`);
    if(entity.mesh||entity.destructible)errors.push(`${where} cannot also use rigid mesh/fracture geometry`);
    if(ref.clip!==undefined&&(typeof ref.clip!=='string'||ref.clip.length>256||/[\x00-\x1f]/.test(ref.clip)))errors.push(`${where}.clip must be a clip name`);
    for(const [key,lo,hi]of [['speed',0,16],['timeOffset',0,120],['yaw',-1000,1000]])if(ref[key]!==undefined&&(!Number.isFinite(ref[key])||ref[key]<lo||ref[key]>hi))errors.push(`${where}.${key} must be in [${lo},${hi}]`);
    if(entity.kind==='player'&&!ref.clip&&((ref.speed??1)!==1||(ref.timeOffset??0)!==0))errors.push(`${where}: player locomotion timing requires a fixed clip for speed/timeOffset overrides`);
    if(ref.scale!==undefined&&!vector(ref.scale,v=>v>=.0001&&v<=10000))errors.push(`${where}.scale must contain three positive model dimensions`);
    if(ref.offset!==undefined&&!vector(ref.offset,v=>Math.abs(v)<=100000))errors.push(`${where}.offset must contain three finite model coordinates`);
    if(ref.rootMotion!==undefined&&!['asset','preserve','extract'].includes(ref.rootMotion))errors.push(`${where}.rootMotion must be asset, preserve or extract`);
    if(ref.anchor!==undefined&&(!['feet','center'].includes(ref.anchor)||entity.kind==='player'&&ref.anchor!=='feet'))errors.push(`${where}.anchor must be feet or center (player uses feet)`);
    if(ref.controller!==undefined) {
      if(entity.kind!=='player')errors.push(`${where}.controller requires a player entity`);
      if(exactKeys(ref.controller,[],`${where}.controller`,errors,['stepHeight','eyeHeight','interactionHeight','orbitDistance','orbitHeight','targetHeight']))for(const [key,value]of Object.entries(ref.controller))if(!Number.isFinite(value)||value<(key==='orbitDistance'?.001:0)||value>100000)errors.push(`${where}.controller.${key} is outside its finite metre range`);
    }
    if(ref.rig!==undefined) {
      if(entity.kind!=='player')errors.push(`${where}.rig requires a player entity`);
      const flags=['enabled','footLock','turnInPlace'];
      const ranges={height:[.1,100],turnThreshold:[.05,3.14],turnStepHeight:[0,5],turnStepDuration:[.01,10],turnCooldown:[0,10],maxTwist:[0,6.28],maxSlopeCosine:[0,1],swingPrediction:[0,10],maxFootCorrection:[0,10],pelvisLower:[-10,0],pelvisUpper:[0,10],maxStepHeight:[0,10],maxPelvisShift:[0,1],soleRoll:[0,1.5]};
      if(exactKeys(ref.rig,[],`${where}.rig`,errors,[...flags,...Object.keys(ranges)]))for(const [key,value]of Object.entries(ref.rig)) {
        if(flags.includes(key)){if(typeof value!=='boolean')errors.push(`${where}.rig.${key} must be boolean`);continue;}
        const range=ranges[key];
        if(!range)continue;
        const [lo,hi]=range;
        if(!Number.isFinite(value)||value<lo||value>hi)errors.push(`${where}.rig.${key} must be in [${lo},${hi}]`);
      }
    }
    if(ref.materials!==undefined){
      if(!object(ref.materials)||Object.keys(ref.materials).length>256)errors.push(`${where}.materials must map source slots to scene material names`);
      else for(const [slot,name]of Object.entries(ref.materials))if(!TEXTURE_NAME.test(slot)||typeof name!=='string'||!TEXTURE_NAME.test(name)||materialNames&&!materialNames.has(name))errors.push(`${where}.materials references an invalid source slot or scene material`);
    }
  }
  if (entity.components !== undefined) {
    try { validateComponents(entity.components); } catch(error) { errors.push(`${location}.components: ${error.message}`); }
  }
  if (entity.material !== undefined) {
    if (typeof entity.material === 'string') {
      if (materialNames&&!materialNames.has(entity.material)) {
        errors.push(`${location}.material references unknown material '${entity.material}'`);
      }
    } else if (!object(entity.material)) {
      errors.push(`${location}.material must be a material name or an inline object`);
    } else {
      exactKeys(entity.material, MATERIAL_KEYS, `${location}.material`, errors);
      if (!vector(entity.material.albedo, n => n >= 0 && n <= 1)) {
        errors.push(`${location}.material.albedo must contain three channels in [0, 1]`);
      }
      for (const key of ['roughness', 'metalness', 'wetness', 'wear']) {
        const value = entity.material[key];
        // wetness/wear are optional on inline v1 materials; roughness/metalness are not.
        if (value === undefined && (key === 'wetness' || key === 'wear')) continue;
        if (!Number.isFinite(value) || value < 0 || value > 1) {
          errors.push(`${location}.material.${key} must be a number in [0, 1]`);
        }
      }
      if (!MATERIAL_PATTERNS.includes(entity.material.pattern)) {
        errors.push(`${location}.material.pattern must be checker, brick or metal`);
      }
    }
  }
  if (entity.kind === 'light') {
    if (!object(entity.light)) {
      errors.push(`${location}.light is required for a light entity`);
    } else {
      exactKeys(entity.light, LIGHT_KEYS, `${location}.light`, errors,
        ['castSurfaceShadow','castVolumetricShadow','volumetricIntensity']);
      for (const key of ['castSurfaceShadow','castVolumetricShadow']) {
        if(entity.light[key] !== undefined && typeof entity.light[key] !== 'boolean') errors.push(`${location}.light.${key} must be boolean`);
      }
      if(entity.light.volumetricIntensity !== undefined && (!Number.isFinite(entity.light.volumetricIntensity)
        || entity.light.volumetricIntensity < 0 || entity.light.volumetricIntensity > 10)) errors.push(`${location}.light.volumetricIntensity must be in [0, 10]`);
      if (!vector(entity.light.color, n => n >= 0 && n <= 1)) {
        errors.push(`${location}.light.color must contain three channels in [0, 1]`);
      }
      if (!Number.isFinite(entity.light.intensity) || entity.light.intensity <= 0 || entity.light.intensity > 100) {
        errors.push(`${location}.light.intensity must be a finite number in (0, 100]`);
      }
      if (!Number.isFinite(entity.light.range) || entity.light.range <= 0 || entity.light.range > 1000) {
        errors.push(`${location}.light.range must be a finite number in (0, 1000]`);
      }
    }
  } else if (entity.light !== undefined) {
    errors.push(`${location}.light is only supported on a light entity`);
  }
  if (entity.vehicle !== undefined) {
    if (entity.kind !== 'vehicle') {
      errors.push(`${location}.vehicle is only supported on a vehicle entity`);
    } else if (!exactKeys(entity.vehicle, ['speed'], `${location}.vehicle`, errors, ['direction'])) {
      // exactKeys already reported the shape error
    } else {
      if (!Number.isFinite(entity.vehicle.speed) || entity.vehicle.speed < 0 || entity.vehicle.speed > 80) {
        errors.push(`${location}.vehicle.speed must be a finite number in [0, 80]`);
      }
      if (entity.vehicle.direction !== undefined
          && entity.vehicle.direction !== 1 && entity.vehicle.direction !== -1) {
        errors.push(`${location}.vehicle.direction must be 1 or -1`);
      }
    }
  } else if (entity.kind === 'vehicle') {
    errors.push(`${location}.vehicle is required for a vehicle entity`);
  }
  if (entity.physics !== undefined) {
    if (!['ground', 'wall', 'prop'].includes(entity.kind)) {
      errors.push(`${location}.physics is only supported on ground, wall and prop entities`);
    }
    if (exactKeys(entity.physics, ['mode'], `${location}.physics`, errors,
      ['mass', 'friction', 'restitution'])) {
      if (!['static', 'dynamic'].includes(entity.physics.mode)) {
        errors.push(`${location}.physics.mode must be static or dynamic`);
      }
      if (entity.physics.mode === 'dynamic' && entity.kind === 'ground') {
        errors.push(`${location}.physics.mode cannot be dynamic for ground`);
      }
      const ranges = { mass: [0.05, 1000], friction: [0, 1.5], restitution: [0, 0.95] };
      for (const [key, [min, max]] of Object.entries(ranges)) {
        const value = entity.physics[key];
        if (value !== undefined && (!Number.isFinite(value) || value < min || value > max)) {
          errors.push(`${location}.physics.${key} must be in [${min}, ${max}]`);
        }
      }
      if (entity.physics.mode === 'dynamic' && entity.collider !== true) {
        errors.push(`${location}.physics dynamic requires collider=true`);
      }
    }
  }
  if (entity.destructible !== undefined) {
    if (!['wall', 'prop'].includes(entity.kind)) {
      errors.push(`${location}.destructible is only supported on wall and prop entities`);
    }
    if (exactKeys(entity.destructible, ['enabled', 'strength', 'fragments'],
      `${location}.destructible`, errors, ['debrisLifetime', 'structural', 'seed', 'mode', 'cells', 'voronoi'])) {
      if (typeof entity.destructible.enabled !== 'boolean') {
        errors.push(`${location}.destructible.enabled must be boolean`);
      }
      if (entity.destructible.enabled === true) {
        if (entity.collider !== true) {
          errors.push(`${location}.destructible requires collider=true`);
        }
        if (!Number.isFinite(entity.destructible.strength) || entity.destructible.strength < 1
            || entity.destructible.strength > 1000000) {
          errors.push(`${location}.destructible.strength must be in [1, 1000000]`);
        }
        if (!Number.isInteger(entity.destructible.fragments) || entity.destructible.fragments < 4
            || entity.destructible.fragments > 28) {
          errors.push(`${location}.destructible.fragments must be an integer in [4, 28]`);
        }
        if (entity.destructible.debrisLifetime !== undefined
            && (!Number.isFinite(entity.destructible.debrisLifetime)
              || entity.destructible.debrisLifetime < 1
              || entity.destructible.debrisLifetime > 120)) {
          errors.push(`${location}.destructible.debrisLifetime must be in [1, 120]`);
        }
        if (entity.destructible.structural !== undefined
            && typeof entity.destructible.structural !== 'boolean') {
          errors.push(`${location}.destructible.structural must be boolean`);
        }
        if (entity.destructible.seed !== undefined
            && (!Number.isInteger(entity.destructible.seed)
              || entity.destructible.seed < 0 || entity.destructible.seed > 4294967295)) {
          errors.push(`${location}.destructible.seed must be an integer in [0, 4294967295]`);
        }
        if (entity.destructible.mode !== undefined
            && entity.destructible.mode !== 'entity' && entity.destructible.mode !== 'bonded') {
          errors.push(`${location}.destructible.mode must be "entity" or "bonded"`);
        }
        if (entity.destructible.cells !== undefined
            && (!Number.isInteger(entity.destructible.cells)
              || entity.destructible.cells < 1 || entity.destructible.cells > 256)) {
          errors.push(`${location}.destructible.cells must be an integer in [1, 256]`);
        }
        if (entity.destructible.voronoi !== undefined) {
          if (typeof entity.destructible.voronoi !== 'string'
              || entity.destructible.voronoi.length === 0
              || entity.destructible.voronoi.length > 259) {
            errors.push(`${location}.destructible.voronoi must be a non-empty path string`);
          } else if (entity.destructible.mode !== 'bonded') {
            errors.push(`${location}.destructible.voronoi requires mode "bonded"`);
          }
        }
      }
    }
  }
}

function validateLighting(value, errors) {
  if (!exactKeys(value, [], 'scene.lighting', errors,
    ['ambient', 'skyColor', 'keyDirection', 'keyColor', 'keyIntensity',
      'fogColor', 'fogDensity', 'fogHeightFalloff', 'fogBaseHeight', 'toneMap', 'exposure',
      'cloudCoverage', 'cloudHeight', 'cloudThickness', 'cloudSpeed', 'quality',
      'localShadows','ibl','ddgi','cloudShadows','temporal','maxShadowLights','fogVolumes','environment'])) {
    return;
  }
  for (const key of ['ambient', 'skyColor', 'keyColor', 'fogColor']) {
    if (value[key] !== undefined && !vector(value[key], n => n >= 0 && n <= 1)) {
      errors.push(`scene.lighting.${key} must contain three channels in [0, 1]`);
    }
  }
  if(value.environment!==undefined){const ref=value.environment,where='scene.lighting.environment';if(exactKeys(ref,['file','sha256'],where,errors,['intensity','rotation'])){if(typeof ref.file!=='string'||ref.file.length>512||!ref.file.endsWith('.ngenv')||/[\\:\x00-\x1f]/.test(ref.file)||ref.file.startsWith('/')||ref.file.split('/').some(p=>!p||p==='.'||p==='..'))errors.push(`${where}.file must stay inside the scene directory`);if(typeof ref.sha256!=='string'||!/^[a-f0-9]{64}$/.test(ref.sha256))errors.push(`${where}.sha256 is invalid`);for(const [key,lo,hi]of [['intensity',0,10000],['rotation',-3600,3600]])if(ref[key]!==undefined&&(!Number.isFinite(ref[key])||ref[key]<lo||ref[key]>hi))errors.push(`${where}.${key} must be in [${lo},${hi}]`);}}
  if (value.keyDirection !== undefined && !vector(value.keyDirection, n => Math.abs(n) <= 1)) {
    errors.push('scene.lighting.keyDirection must contain three components in [-1, 1]');
  }
  if(Array.isArray(value.keyDirection) && value.keyDirection.reduce((sum,n)=>sum+n*n,0)<=1e-8)
    errors.push('scene.lighting.keyDirection must be nonzero');
  const ranges = {
    keyIntensity: [0, 100],
    fogDensity: [0, 10],
    fogHeightFalloff: [0, 10],
    fogBaseHeight: [-1000, 1000],
    cloudCoverage: [0, 1],
    cloudHeight: [1, 1000],
    cloudThickness: [1, 1000],
    cloudSpeed: [0, 10],
    exposure: [0.01, 100],
  };
  for (const [key, [low, high]] of Object.entries(ranges)) {
    if (value[key] === undefined) continue;
    if (!Number.isFinite(value[key]) || value[key] < low || value[key] > high) {
      errors.push(`scene.lighting.${key} must be a finite number in [${low}, ${high}]`);
    }
  }
  if (value.toneMap !== undefined && typeof value.toneMap !== 'boolean') {
    errors.push('scene.lighting.toneMap must be boolean');
  }
  for (const key of ['localShadows','ibl','ddgi','cloudShadows','temporal']) {
    if(value[key] !== undefined && typeof value[key] !== 'boolean') errors.push(`scene.lighting.${key} must be boolean`);
  }
  for (const [key, maximum] of [['quality',2],['maxShadowLights',4]]) {
    if(value[key] !== undefined && (!Number.isInteger(value[key]) || value[key]<0 || value[key]>maximum))
      errors.push(`scene.lighting.${key} must be an integer in [0, ${maximum}]`);
  }
  if(value.fogVolumes !== undefined) {
    if(!Array.isArray(value.fogVolumes)||value.fogVolumes.length>8) errors.push('scene.lighting.fogVolumes supports at most 8 volumes');
    else value.fogVolumes.forEach((volume,i)=>{
      const location=`scene.lighting.fogVolumes[${i}]`;
      if(!exactKeys(volume,['center','extent','density'],location,errors))return;
      if(!vector(volume.center,n=>Math.abs(n)<=100000))errors.push(`${location}.center is invalid`);
      if(!vector(volume.extent,n=>n>=0.001&&n<=10000))errors.push(`${location}.extent is invalid`);
      if(!Number.isFinite(volume.density)||volume.density<0||volume.density>10)errors.push(`${location}.density is invalid`);
    });
  }
}

function validateMaterials(value, errors) {
  const names = new Set();
  if (!object(value)) {
    errors.push('scene.materials must be an object');
    return names;
  }
  const entries = Object.entries(value);
  if (entries.length > 256) errors.push('scene.materials supports at most 256 entries');
  for (const [name, material] of entries) {
    const location = `scene.materials.${name}`;
    if (!text(name) || !TEXTURE_NAME.test(name)) {
      errors.push(`${location} must use 1-4 characters of [A-Za-z0-9_.-]`);
    }
    names.add(name);
    if (!exactKeys(material, NAMED_MATERIAL_REQUIRED, location, errors, NAMED_MATERIAL_OPTIONAL)) continue;
    if(material.unlit!==undefined&&typeof material.unlit!=='boolean')errors.push(`${location}.unlit must be boolean`);
    for (const key of ['albedo', 'normal', 'orm', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal']) {
      if (material[key] === undefined) continue;
      if (typeof material[key] !== 'string' || !TEXTURE_NAME.test(material[key])) {
        errors.push(`${location}.${key} must be a texture base name`);
      }
    }
    if (material.uvScale !== undefined
        && (!Number.isFinite(material.uvScale) || material.uvScale < 0.05 || material.uvScale > 1000)) {
      errors.push(`${location}.uvScale must be a finite number in [0.05, 1000]`);
    }
    if(material.textureMappings!==undefined)try{validateTextureMappings(material.textureMappings);}catch(error){errors.push(`${location}.textureMappings: ${error.message}`);}
    if(material.layered!==undefined)try{
      const normalized=normalizeLayeredMaterial(material.layered);
      if(stableJSON(normalized)!==stableJSON(material.layered))errors.push(`${location}.layered must use the normalized material representation`);
      if(material.unlit||(material.alphaMode??'OPAQUE')!=='OPAQUE')errors.push(`${location}.layered requires an opaque lit material`);
      for(const layer of normalized.layers)for(const ref of [...Object.values(layer.textures),...(layer.blend.texture?[layer.blend.texture]:[])])if(ref.uv.set>7)errors.push(`${location}.layered supports UV sets 0-7`);
    }catch(error){errors.push(`${location}.layered: ${error.message}`);}
    if(material.ior!==undefined&&!validIor(material.ior))errors.push(`${location}.ior must be 0 or a finite number in [1, Float32 maximum]`);
    if(material.emissiveFactor!==undefined&&!vector(material.emissiveFactor,n=>n>=0&&n<=1))errors.push(`${location}.emissiveFactor must contain three numbers in [0,1]`);
    if(material.emissiveStrength!==undefined&&(!Number.isFinite(material.emissiveStrength)||material.emissiveStrength<0||material.emissiveStrength>1000000))errors.push(`${location}.emissiveStrength must be a finite number in [0,1000000]`);
    if(material.alphaMode!==undefined&&!['OPAQUE','MASK','BLEND'].includes(material.alphaMode))errors.push(`${location}.alphaMode must be OPAQUE, MASK or BLEND`);
    for (const key of ['opacity', 'roughness', 'metalness', 'wetness', 'wear', 'clearcoat', 'clearcoatRoughness', 'alphaCutoff', 'occlusionStrength']) {
      if (material[key] === undefined) continue;
      if (!Number.isFinite(material[key]) || material[key] < 0 || material[key] > 1) {
        errors.push(`${location}.${key} must be a number in [0, 1]`);
      }
    }
  }
  return names;
}

const AUDIO_BUSES = ['sfx', 'ambience', 'music', 'voice', 'ui'];
const AUDIO_SOUND_NAME = /^[A-Za-z0-9_.\/-]{1,64}$/;

// Scene-level audio authoring. The block is optional; when it is missing the host
// keeps the engine on its defaults (.developmentAgent/animation-audio/AUDIO-PLAN.md ?4.2).
function validateWater(water,errors){
  if(!exactKeys(water,['file','sha256','origin'],'scene.water',errors,['roughness','absorption','scattering']))return;
  if(typeof water.file!=='string'||water.file.length>512||!water.file.endsWith('.ngwater.json')||/[:\\\x00-\x1f]/.test(water.file)||water.file.startsWith('/')||water.file.split('/').some(p=>!p||p==='.'||p==='..'))errors.push('Invalid water dependency path');
  if(typeof water.sha256!=='string'||!/^[a-f0-9]{64}$/.test(water.sha256))errors.push('Invalid water SHA-256');
  if(!vector(water.origin,v=>Math.abs(v)<=1.e6))errors.push('Invalid water origin');
  if(water.roughness!==undefined&&(!Number.isFinite(water.roughness)||water.roughness<.025||water.roughness>1))errors.push('Invalid water roughness');
  for(const [key,max] of [['absorption',100],['scattering',1]])if(water[key]!==undefined&&!vector(water[key],v=>v>=0&&v<=max))errors.push(`Invalid water ${key}`);
}

function validateAudio(audio, errors, entities) {
  if (!exactKeys(audio, [], 'scene.audio', errors,
    ['masterGainDb', 'emitters', 'loudnessTargetLufs', 'sounds'])) return;
  if(audio.sounds!==undefined) {
    if(!object(audio.sounds)||Object.keys(audio.sounds).length>64)errors.push('scene.audio.sounds must contain at most 64 sound references');
    else for(const [name,ref] of Object.entries(audio.sounds)) {
      if(!AUDIO_SOUND_NAME.test(name))errors.push('Invalid scene audio sound name');
      if(!exactKeys(ref,['file','sha256'],`scene.audio.sounds.${name}`,errors))continue;
      if(typeof ref.file!=='string'||ref.file.length>512||!ref.file.endsWith('.ngsnd')||/[:\\\x00-\x1f]/.test(ref.file)||ref.file.startsWith('/')||ref.file.split('/').some(p=>!p||p==='.'||p==='..'))errors.push('Invalid scene audio sound path');
      if(typeof ref.sha256!=='string'||!/^[a-f0-9]{64}$/.test(ref.sha256))errors.push('Invalid scene audio SHA-256');
    }
  }
  if (audio.masterGainDb !== undefined
      && (!Number.isFinite(audio.masterGainDb) || audio.masterGainDb < -60 || audio.masterGainDb > 6)) {
    errors.push('scene.audio.masterGainDb must be between -60 and 6');
  }
  if (audio.loudnessTargetLufs !== undefined
      && (!Number.isFinite(audio.loudnessTargetLufs) || audio.loudnessTargetLufs < -40 || audio.loudnessTargetLufs > -5)) {
    errors.push('scene.audio.loudnessTargetLufs must be between -40 and -5');
  }
  if (audio.emitters === undefined) return;
  if (!Array.isArray(audio.emitters) || audio.emitters.length > 64) {
    errors.push('scene.audio.emitters must be an array of at most 64 entries');
    return;
  }
  const ids = new Set();
  for (const [index, emitter] of audio.emitters.entries()) {
    const at = `scene.audio.emitters[${index}]`;
    if (!exactKeys(emitter, ['id', 'entity', 'sound'], at, errors,
      ['trigger', 'bus', 'gainDb', 'maxDistance', 'loop', 'occlusion', 'minDistance', 'spatial', 'offset', 'pitch'])) continue;
    if (!text(emitter.id) || emitter.id.length > 64) errors.push(`${at}.id must be 1-64 printable characters`);
    else if (ids.has(emitter.id.toLowerCase())) errors.push(`${at}.id is duplicated`);
    else ids.add(emitter.id.toLowerCase());
    if (typeof emitter.entity !== 'string' || !UUID.test(emitter.entity)) errors.push(`${at}.entity must be a UUID`);
    else if(Array.isArray(entities)&&!entities.some(e=>e?.id?.toLowerCase()===emitter.entity.toLowerCase()))errors.push(`${at}.entity does not exist`);
    if (!text(emitter.sound) || !AUDIO_SOUND_NAME.test(emitter.sound)) {
      errors.push(`${at}.sound must be 1-64 characters of [A-Za-z0-9_.-/]`);
    }
    if (emitter.trigger !== undefined && !text(emitter.trigger)) errors.push(`${at}.trigger must be a non-empty string`);
    if (emitter.bus !== undefined && !AUDIO_BUSES.includes(emitter.bus)) {
      errors.push(`${at}.bus must be one of ${AUDIO_BUSES.join(', ')}`);
    }
    if (emitter.gainDb !== undefined && (!Number.isFinite(emitter.gainDb) || emitter.gainDb < -60 || emitter.gainDb > 12)) {
      errors.push(`${at}.gainDb must be between -60 and 12`);
    }
    if (emitter.minDistance !== undefined && (!Number.isFinite(emitter.minDistance) || emitter.minDistance < 0.1 || emitter.minDistance > 100)) {
      errors.push(`${at}.minDistance must be between 0.1 and 100`);
    }
    if (emitter.maxDistance !== undefined && (!Number.isFinite(emitter.maxDistance) || emitter.maxDistance < 1 || emitter.maxDistance > 200)) {
      errors.push(`${at}.maxDistance must be between 1 and 200`);
    }
    if (emitter.loop !== undefined && typeof emitter.loop !== 'boolean') errors.push(`${at}.loop must be a boolean`);
    if (emitter.occlusion !== undefined && typeof emitter.occlusion !== 'boolean') errors.push(`${at}.occlusion must be a boolean`);
    if(emitter.spatial!==undefined&&typeof emitter.spatial!=='boolean')errors.push(`${at}.spatial must be a boolean`);
    if(emitter.pitch!==undefined&&(!Number.isFinite(emitter.pitch)||emitter.pitch<0.1||emitter.pitch>4))errors.push(`${at}.pitch must be in [0.1, 4]`);
    if(emitter.offset!==undefined&&(!Array.isArray(emitter.offset)||emitter.offset.length!==3||emitter.offset.some(n=>!Number.isFinite(n)||Math.abs(n)>100000)))errors.push(`${at}.offset must be three finite world metre offsets`);
    if((emitter.maxDistance??28)<(emitter.minDistance??1.5))errors.push(`${at}.maxDistance must be at least minDistance`);
  }
}

const JOINT_KEYS = ['type', 'bodyA'];
const JOINT_OPTIONAL_KEYS = ['bodyB', 'anchorA', 'anchorB', 'axis', 'hingeSpan', 'restLength',
  'compliance', 'lowerLimit', 'upperLimit', 'breakForce', 'collideConnected',
  'motorSpeed', 'motorMaxTorque', 'friction'];

function validateJoints(joints, entities, errors) {
  if (!Array.isArray(joints) || joints.length > 256) {
    errors.push('scene.joints must be an array of at most 256 joints');
    return;
  }
  const ids = new Set((entities ?? []).filter(object)
    .map((entity) => String(entity.id ?? '').toLowerCase()));
  const vec = (value, where) => Array.isArray(value) && value.length === 3
    && value.every((item) => Number.isFinite(item));
  for (const [index, joint] of joints.entries()) {
    const where = `scene.joints[${index}]`;
    if (!object(joint)) {
      errors.push(`${where} must be an object`);
      continue;
    }
    if (!exactKeys(joint, JOINT_KEYS, where, errors, JOINT_OPTIONAL_KEYS)) continue;
    if (joint.type !== 'point' && joint.type !== 'hinge' && joint.type !== 'rope') {
      errors.push(`${where}.type must be "point", "hinge" or "rope"`);
    }
    if (!text(joint.bodyA)) {
      errors.push(`${where}.bodyA must be an entity id`);
    } else if (!ids.has(joint.bodyA.toLowerCase())) {
      errors.push(`${where}.bodyA names an entity that does not exist: ${joint.bodyA}`);
    }
    if (joint.bodyB !== undefined) {
      if (!text(joint.bodyB)) errors.push(`${where}.bodyB must be an entity id`);
      else if (!ids.has(joint.bodyB.toLowerCase())) {
        errors.push(`${where}.bodyB names an entity that does not exist: ${joint.bodyB}`);
      }
    }
    for (const key of ['anchorA', 'anchorB', 'axis']) {
      if (joint[key] !== undefined && !vec(joint[key], where)) {
        errors.push(`${where}.${key} must be 3 finite numbers`);
      }
    }
    if (joint.axis !== undefined && vec(joint.axis, where)
        && Math.hypot(...joint.axis) <= 1e-3) {
      errors.push(`${where}.axis must not be zero`);
    }
    const ranges = {
      hingeSpan: [0.01, 10], restLength: [0, 100], compliance: [0, 1],
      lowerLimit: [-3.15, 3.15], upperLimit: [-3.15, 3.15], breakForce: [0, 1e6],
      motorSpeed: [-100, 100], motorMaxTorque: [0, 1e6], friction: [0, 1],
    };
    for (const [key, [low, high]] of Object.entries(ranges)) {
      if (joint[key] === undefined) continue;
      if (!Number.isFinite(joint[key]) || joint[key] < low || joint[key] > high) {
        errors.push(`${where}.${key} must be between ${low} and ${high}`);
      }
    }
    if (Number.isFinite(joint.lowerLimit) && Number.isFinite(joint.upperLimit)
        && joint.lowerLimit > joint.upperLimit) {
      errors.push(`${where}.lowerLimit must not exceed upperLimit`);
    }
    if (joint.collideConnected !== undefined && typeof joint.collideConnected !== 'boolean') {
      errors.push(`${where}.collideConnected must be boolean`);
    }
    if ((joint.motorMaxTorque > 0 || joint.friction > 0) && joint.type !== 'hinge') {
      errors.push(`${where} motors and friction are only supported on hinge joints`);
    }
  }
}

// A world reference. It is deliberately a *reference*: the cooked manifest and its chunks
// live under the project's assets directory, and the scene only records which manifest is
// current. Inlining a manifest here would put a few thousand chunk entries inside the
// 16 MiB scene document and would make every world edit a scene transaction.
function validateWorldRef(world, errors) {
  if (!exactKeys(world, ['worldId', 'generatorVersion', 'cookedManifest', 'manifestHash'], 'scene.world', errors,
    ['recipe', 'inputDigest', 'label'])) return;
  if (typeof world.worldId !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$/.test(world.worldId)) {
    errors.push('scene.world.worldId must be 1-64 identifier characters');
  }
  if (typeof world.generatorVersion !== 'string' || !/^worldgen-\d+$/.test(world.generatorVersion)) {
    errors.push('scene.world.generatorVersion must look like worldgen-1');
  }
  if (!/^[a-f0-9]{64}$/.test(String(world.manifestHash ?? ''))) {
    errors.push('scene.world.manifestHash must contain 64 lowercase hex digits');
  }
  if (world.inputDigest !== undefined && !/^[a-f0-9]{64}$/.test(String(world.inputDigest))) {
    errors.push('scene.world.inputDigest must contain 64 lowercase hex digits');
  }
  for (const key of ['cookedManifest', 'recipe']) {
    const ref = world[key];
    if (ref === undefined) continue;
    const where = `scene.world.${key}`;
    if (!exactKeys(ref, ['file', 'sha256'], [], where, errors)) continue;
    if (typeof ref.file !== 'string' || !ref.file.endsWith('.json') || ref.file.length > 240
        || /[\\:\x00-\x1f]/.test(ref.file) || ref.file.startsWith('/')
        || ref.file.split('/').some(part => part === '' || part === '.' || part === '..')) {
      errors.push(`${where}.file must be a project-relative .json path`);
    }
    if (!/^[a-f0-9]{64}$/.test(String(ref.sha256 ?? ''))) {
      errors.push(`${where}.sha256 must contain 64 lowercase hex digits`);
    }
  }
  if (world.cookedManifest === undefined) errors.push('scene.world.cookedManifest is required');
}

function scoped_validateScene(scene) {
  const errors = [];
  if (exactKeys(scene, SCENE_KEYS, 'scene', errors, SCENE_OPTIONAL_KEYS)) {
    if (scene.schemaVersion !== 1 && scene.schemaVersion !== 2) {
      errors.push('scene.schemaVersion must be 1 or 2');
    }
    if (scene.materials !== undefined && scene.schemaVersion !== 2) {
      errors.push('scene.materials requires schemaVersion 2');
    }
    if (scene.lighting !== undefined) validateLighting(scene.lighting, errors);
    if (scene.audio !== undefined) validateAudio(scene.audio, errors, scene.entities);
    if(scene.water!==undefined)validateWater(scene.water,errors);
    if(scene.world!==undefined)validateWorldRef(scene.world,errors);
    const materialNames = scene.materials === undefined
      ? new Set()
      : validateMaterials(scene.materials, errors);
    if (typeof scene.projectId !== 'string' || !UUID.test(scene.projectId)) errors.push('scene.projectId must be a UUID');
    if (!text(scene.name)) errors.push('scene.name must contain 1-28 printable characters');
    if (!Number.isSafeInteger(scene.revision) || scene.revision < 0) errors.push('scene.revision must be a non-negative safe integer');
    if (!Array.isArray(scene.entities) || scene.entities.length > MAX_ENTITIES) {
      errors.push(`scene.entities must be an array of at most ${MAX_ENTITIES} entities`);
    } else {
      const ids = new Set();
      for (const [index, entity] of scene.entities.entries()) {
        validateEntity(entity, `scene.entities[${index}]`, errors, materialNames);
        if (object(entity) && typeof entity.id === 'string') {
          const id = entity.id.toLowerCase();
          if (ids.has(id)) errors.push(`scene.entities[${index}].id is duplicated`);
          ids.add(id);
        }
      }
      const characterCount=scene.entities.filter(e=>e?.character!==undefined).length+(scene.entities.some(e=>e?.kind==='player'&&e.character)?0:1);
      if(characterCount>64)errors.push('scene supports at most 64 character instances including the player');
      if (scene.entities.filter(entity => entity?.kind === 'player').length !== 1) {
        errors.push('scene must contain exactly one player spawn');
      }
      if (scene.entities.filter(entity => entity?.kind === 'ground').length < 1) {
        errors.push('scene must contain at least one ground');
      }
      if (scene.entities.filter(entity => entity?.kind === 'light').length > 64) {
        errors.push('scene supports at most 64 lights');
      }
    }
    if (scene.joints !== undefined) validateJoints(scene.joints, scene.entities, errors);
    if (exactKeys(scene.settings, SETTINGS_KEYS, 'scene.settings', errors)) {
      for (const key of SETTINGS_KEYS) {
        const [low, high] = SETTINGS_RANGE[key];
        if (!Number.isFinite(scene.settings[key])
            || scene.settings[key] < low || scene.settings[key] > high) {
          errors.push(`scene.settings.${key} must be a finite number between ${low} and ${high}`);
        }
      }
      if (scene.settings.runSpeed < scene.settings.walkSpeed) errors.push('scene.settings.runSpeed must be at least walkSpeed');
    }
  }
  return { valid: errors.length === 0, errors };
}

function assertScene(scene) {
  const result = validateScene(scene);
  if (!result.valid) throw new SceneError('INVALID_SCENE', 'Scene validation failed', result.errors);
}

export function stableJSON(value) {
  if (Array.isArray(value)) return `[${value.map(stableJSON).join(',')}]`;
  if (object(value)) {
    return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${stableJSON(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}

function scoped_sceneHash(scene) {
  assertScene(scene);
  return createHash('sha256').update(stableJSON(scene)).digest('hex');
}

function sameId(a, b) {
  return typeof a === 'string' && typeof b === 'string' && a.toLowerCase() === b.toLowerCase();
}

function checkRevision(scene, projectId, expectedRevision) {
  if (!sameId(projectId, scene.projectId)) throw new SceneError('PROJECT_MISMATCH', 'The command belongs to a different project');
  if (!Number.isSafeInteger(expectedRevision) || expectedRevision !== scene.revision) {
    throw new SceneError('REVISION_CONFLICT', `Expected revision ${expectedRevision}; current revision is ${scene.revision}`);
  }
  if (scene.revision === Number.MAX_SAFE_INTEGER) throw new SceneError('REVISION_EXHAUSTED', 'Revision counter is exhausted');
}

function scoped_queryScene(scene, query = {}) {
  assertScene(scene);
  const allowed = ['kind', 'entityId', 'name'];
  if (!object(query) || Object.keys(query).some(key => !allowed.includes(key))) {
    throw new SceneError('INVALID_QUERY', 'Query accepts kind, entityId and name only');
  }
  if (query.kind !== undefined && !ENTITY_KINDS.includes(query.kind)) throw new SceneError('INVALID_QUERY', 'Unknown entity kind');
  if (query.entityId !== undefined && (typeof query.entityId !== 'string' || !UUID.test(query.entityId))) {
    throw new SceneError('INVALID_QUERY', 'entityId must be a UUID');
  }
  if (query.name !== undefined && typeof query.name !== 'string') throw new SceneError('INVALID_QUERY', 'name must be a string');
  const entities = scene.entities.filter(entity =>
    (query.kind === undefined || entity.kind === query.kind)
    && (query.entityId === undefined || sameId(entity.id, query.entityId))
    && (query.name === undefined || entity.name.toLowerCase().includes(query.name.toLowerCase())));
  return { projectId: scene.projectId, revision: scene.revision, entities: structuredClone(entities) };
}

function scoped_previewTransaction(scene, transaction) {
  assertScene(scene);
  const errors = [];
  if (!exactKeys(transaction, ['projectId', 'expectedRevision', 'commands'], 'transaction', errors)) {
    throw new SceneError('INVALID_TRANSACTION', 'Transaction must be an object', errors);
  }
  if (errors.length) throw new SceneError('INVALID_TRANSACTION', 'Transaction contains unsupported or missing fields', errors);
  checkRevision(scene, transaction.projectId, transaction.expectedRevision);
  if (!Array.isArray(transaction.commands) || transaction.commands.length === 0 || transaction.commands.length > 1000) {
    throw new SceneError('INVALID_TRANSACTION', 'A transaction must contain between 1 and 1000 commands');
  }
  const candidate = structuredClone(scene);
  const changes = [];
  for (const [index, command] of transaction.commands.entries()) {
    const commandErrors = [];
    if (!object(command) || !['create', 'set', 'remove'].includes(command.op)) {
      throw new SceneError('INVALID_COMMAND', `Command ${index} has an unsupported operation`);
    }
    const keys = command.op === 'create' ? ['op', 'entity']
      : command.op === 'set' ? ['op', 'entityId', 'changes'] : ['op', 'entityId'];
    exactKeys(command, keys, `commands[${index}]`, commandErrors);
    if (commandErrors.length) throw new SceneError('INVALID_COMMAND', `Command ${index} has invalid fields`, commandErrors);
    if (command.op === 'create') {
      validateEntity(command.entity, `commands[${index}].entity`, commandErrors);
      if (commandErrors.length) throw new SceneError('INVALID_COMMAND', `Command ${index} has an invalid entity`, commandErrors);
      if (candidate.entities.some(entity => sameId(entity.id, command.entity.id))) {
        throw new SceneError('DUPLICATE_ENTITY', `Entity ${command.entity.id} already exists`);
      }
      candidate.entities.push(structuredClone(command.entity));
      changes.push({ op: 'create', entityId: command.entity.id, name: command.entity.name });
      continue;
    }
    if (typeof command.entityId !== 'string' || !UUID.test(command.entityId)) throw new SceneError('INVALID_COMMAND', `Command ${index} entityId must be a UUID`);
    const entityIndex = candidate.entities.findIndex(entity => sameId(entity.id, command.entityId));
    if (entityIndex < 0) throw new SceneError('ENTITY_NOT_FOUND', `Entity ${command.entityId} does not exist`);
    const entity = candidate.entities[entityIndex];
    if (command.op === 'remove') {
      candidate.entities.splice(entityIndex, 1);
      changes.push({ op: 'remove', entityId: entity.id, name: entity.name });
    } else {
      if (!object(command.changes) || Object.keys(command.changes).length === 0
          || Object.keys(command.changes).some(key =>
            (!ENTITY_KEYS.includes(key) && !ENTITY_OPTIONAL_KEYS.includes(key)) || key === 'id')) {
        throw new SceneError('INVALID_COMMAND', 'set changes must contain supported mutable entity fields; id cannot change');
      }
      candidate.entities[entityIndex] = { ...entity, ...structuredClone(command.changes) };
      changes.push({ op: 'set', entityId: entity.id, before: structuredClone(entity), after: structuredClone(candidate.entities[entityIndex]) });
    }
  }
  candidate.revision += 1;
  assertScene(candidate);
  return { projectId: scene.projectId, baseRevision: scene.revision, revision: candidate.revision, changes, scene: candidate };
}

function scoped_applyTransaction(scene, transaction) {
  const result = previewTransaction(scene, transaction);
  return { ...result, undoRecord: { projectId: scene.projectId, before: structuredClone(scene) } };
}

function scoped_undoScene(scene, undoRecord, expectedRevision) {
  assertScene(scene);
  if (!object(undoRecord) || !object(undoRecord.before)) throw new SceneError('INVALID_HISTORY', 'Undo record is invalid');
  checkRevision(scene, undoRecord.projectId, expectedRevision);
  assertScene(undoRecord.before);
  if (!sameId(undoRecord.before.projectId, scene.projectId)) throw new SceneError('PROJECT_MISMATCH', 'Undo snapshot belongs to another project');
  if (undoRecord.before.revision >= scene.revision) throw new SceneError('INVALID_HISTORY', 'Undo snapshot must precede the current revision');
  return { ...structuredClone(undoRecord.before), revision: scene.revision + 1 };
}

async function readJSON(file) {
  try {
    const stat = await fs.stat(file);
    if (stat.size > MAX_FILE_BYTES) throw new SceneError('FILE_TOO_LARGE', `JSON file exceeds ${MAX_FILE_BYTES} bytes`);
    return JSON.parse(await fs.readFile(file, 'utf8'));
  } catch (error) {
    if (error instanceof SyntaxError) throw new SceneError('INVALID_JSON', `Invalid JSON in ${path.basename(file)}`);
    throw error;
  }
}

async function exists(file) {
  try { await fs.access(file); return true; }
  catch (error) { if (error.code === 'ENOENT') return false; throw error; }
}

async function atomicWrite(file, value) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  const temporary = `${file}.${randomUUID()}.tmp`;
  let handle;
  try {
    handle = await fs.open(temporary, 'wx');
    await handle.writeFile(`${stableJSON(value)}\n`, 'utf8');
    await handle.sync();
    await handle.close();
    handle = null;
    await fs.rename(temporary, file);
  } finally {
    await handle?.close();
    await fs.rm(temporary, { force: true });
  }
}

async function locked(file, action) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  const lockPath = `${file}.lock`;
  let handle;
  try {
    handle = await fs.open(lockPath, 'wx');
  } catch (error) {
    if (error.code === 'EEXIST') throw new SceneError('PROJECT_BUSY', 'Project is locked. If a process crashed, verify it has stopped before removing the .lock file.');
    throw error;
  }
  try {
    await handle.writeFile(JSON.stringify({ pid: process.pid }));
    return await action();
  } finally {
    await handle.close();
    await fs.rm(lockPath, { force: true });
  }
}

function emptyHistory(scene) {
  return { version: 1, projectId: scene.projectId, headRevision: scene.revision, headHash: sceneHash(scene), undoStack: [] };
}

function assertHistory(history, scene) {
  if (!object(history) || history.version !== 1 || !sameId(history.projectId, scene.projectId)
      || history.headRevision !== scene.revision || history.headHash !== sceneHash(scene)
      || !Array.isArray(history.undoStack) || history.undoStack.length > 100) {
    throw new SceneError('HISTORY_CONFLICT', 'History does not match the saved scene; use recover or restore a matching backup');
  }
  for (const record of history.undoStack) {
    if (!object(record) || !sameId(record.projectId, scene.projectId)
        || !sameId(record.before?.projectId, scene.projectId) || !validateScene(record.before).valid
        || record.before.revision >= scene.revision) {
      throw new SceneError('INVALID_HISTORY', 'Undo history contains an invalid snapshot');
    }
  }
}

async function readState(file) {
  const scene = await readJSON(file);
  assertScene(scene);
  const history = await exists(`${file}.history.json`) ? await readJSON(`${file}.history.json`) : emptyHistory(scene);
  assertHistory(history, scene);
  return { scene, history };
}

function validateEnvelope(envelope) {
  if (!object(envelope) || envelope.version !== 1 || !object(envelope.scene) || !object(envelope.history)) {
    throw new SceneError('INVALID_RECOVERY', 'Recovery record is invalid');
  }
  assertScene(envelope.scene);
  assertHistory(envelope.history, envelope.scene);
}

async function reconcilePending(file) {
  const pendingPath = `${file}.pending.json`;
  if (!await exists(pendingPath)) return;
  const pending = await readJSON(pendingPath);
  validateEnvelope(pending);
  const scene = await readJSON(file);
  assertScene(scene);
  const hash = sceneHash(scene);
  if (hash === sceneHash(pending.scene)) {
    // Scene replacement is the commit point. Complete an interrupted history write.
    await atomicWrite(`${file}.history.json`, pending.history);
  } else if (hash !== pending.previousHash) {
    throw new SceneError('RECOVERY_REQUIRED', 'Interrupted save conflicts with the current scene; explicit recovery is required');
  }
  await fs.rm(pendingPath);
}

async function persistState(file, previous, next) {
  if (previous) {
    await atomicWrite(`${file}.snapshot.json`, { version: 1, ...previous });
    await atomicWrite(`${file}.pending.json`, { version: 1, previousHash: sceneHash(previous.scene), ...next });
  }
  await atomicWrite(file, next.scene);
  await atomicWrite(`${file}.history.json`, next.history);
  await fs.rm(`${file}.pending.json`, { force: true });
}

async function scoped_loadScene(file) {
  file = path.resolve(file);
  if (await exists(`${file}.pending.json`)) {
    throw new SceneError('RECOVERY_REQUIRED', 'An interrupted save was detected. Run recover before opening the scene.');
  }
  return (await readState(file)).scene;
}

async function scoped_saveScene(file, scene) {
  assertScene(scene);
  file = path.resolve(file);
  return locked(file, async () => {
    if (await exists(file)) throw new SceneError('ALREADY_EXISTS', 'saveScene creates a new project; edit an existing project with commitScene');
    await persistState(file, null, { scene, history: emptyHistory(scene) });
    return structuredClone(scene);
  });
}

async function scoped_commitScene(file, transaction) {
  file = path.resolve(file);
  return locked(file, async () => {
    await reconcilePending(file);
    const previous = await readState(file);
    const result = applyTransaction(previous.scene, transaction);
    const history = {
      ...emptyHistory(result.scene),
      undoStack: [...previous.history.undoStack, result.undoRecord].slice(-100),
    };
    await persistState(file, previous, { scene: result.scene, history });
    return result;
  });
}

// Typed asset editors share scene locking, recovery and history with entity commands.
async function scoped_commitSceneUpdate(file, { projectId, expectedRevision }, update) {
  file = path.resolve(file);
  return locked(file, async () => {
    await reconcilePending(file);
    const previous = await readState(file);
    checkRevision(previous.scene, projectId, expectedRevision);
    const scene = update(structuredClone(previous.scene));
    if (scene.projectId !== previous.scene.projectId) throw new SceneError('PROJECT_MISMATCH', 'Cannot change project identity');
    scene.revision = previous.scene.revision + 1;
    assertScene(scene);
    const undoRecord = { projectId: scene.projectId, before: structuredClone(previous.scene) };
    const history = { ...emptyHistory(scene), undoStack: [...previous.history.undoStack, undoRecord].slice(-100) };
    await persistState(file, previous, { scene, history });
    return scene;
  });
}

async function scoped_undoSceneFile(file, { projectId, expectedRevision }) {
  file = path.resolve(file);
  return locked(file, async () => {
    await reconcilePending(file);
    const previous = await readState(file);
    checkRevision(previous.scene, projectId, expectedRevision);
    const record = previous.history.undoStack.at(-1);
    if (!record) throw new SceneError('NOTHING_TO_UNDO', 'The project has no saved undo history');
    const scene = undoScene(previous.scene, record, expectedRevision);
    const history = { ...emptyHistory(scene), undoStack: previous.history.undoStack.slice(0, -1) };
    await persistState(file, previous, { scene, history });
    return { scene, revision: scene.revision };
  });
}

async function scoped_recoverScene(file) {
  file = path.resolve(file);
  return locked(file, async () => {
    if (await exists(`${file}.pending.json`)) {
      try {
        await reconcilePending(file);
        return { scene: (await readState(file)).scene, recovery: 'completed-interrupted-save' };
      } catch (error) {
        if (!(error instanceof SceneError) && error.code !== 'ENOENT') throw error;
      }
    }
    // The snapshot is immutable during recovery so another failure remains recoverable.
    if (!await exists(`${file}.snapshot.json`)) throw new SceneError('NO_SNAPSHOT', 'No valid recovery snapshot exists');
    const snapshot = await readJSON(`${file}.snapshot.json`);
    validateEnvelope(snapshot);
    let greatestRevision = snapshot.scene.revision;
    for (const candidate of [file, `${file}.history.json`, `${file}.pending.json`]) {
      try {
        const data = await readJSON(candidate);
        const revision = data.revision ?? data.headRevision ?? data.scene?.revision;
        if (Number.isSafeInteger(revision) && revision >= 0) greatestRevision = Math.max(greatestRevision, revision);
      } catch { /* A corrupt candidate does not invalidate a verified snapshot. */ }
    }
    if (greatestRevision === Number.MAX_SAFE_INTEGER) throw new SceneError('REVISION_EXHAUSTED', 'Cannot increment revision during recovery');
    const scene = { ...structuredClone(snapshot.scene), revision: greatestRevision + 1 };
    const history = { ...emptyHistory(scene), undoStack: snapshot.history.undoStack };
    await atomicWrite(`${file}.pending.json`, { version: 1, previousHash: sceneHash(snapshot.scene), scene, history });
    await atomicWrite(file, scene);
    await atomicWrite(`${file}.history.json`, history);
    await fs.rm(`${file}.pending.json`, { force: true });
    return { scene, recovery: 'restored-last-snapshot' };
  });
}

async function scoped_buildScene(sceneOrFile, outputDirectory, { target = 'windows-x64', assetsDirectory, sceneDirectory } = {}) {
  if (target !== 'windows-x64') throw new SceneError('UNSUPPORTED_TARGET', 'Only windows-x64 packages are supported');
  const scene = typeof sceneOrFile === 'string' ? await loadScene(sceneOrFile,{assetsDirectory}) : structuredClone(sceneOrFile);
  assertScene(scene);
  const sceneData = `${stableJSON(scene)}\n`;
  const contentHash = createHash('sha256').update(sceneData).digest('hex');
  const dependencies=gameplayDependencies(scene,assetsDirectory);
  if(scene.entities.some(entity=>entity.mesh||entity.character||entity.destructible?.voronoi)||Object.keys(scene.materials??{}).length||Object.keys(scene.audio?.sounds??{}).length||scene.lighting?.environment||scene.water||scene.world) {
    const sourceScene=typeof sceneOrFile==='string'?path.resolve(sceneOrFile):null;
    const screenScene=sourceScene&&path.dirname(sourceScene).split(path.sep).includes('.screen');
    const sceneRoot=await fs.realpath(sceneDirectory??(sourceScene?(screenScene?path.resolve(assetsDirectory??'assets'):path.dirname(sourceScene)):path.resolve(assetsDirectory??'assets')));
    const requested=new Map(),layeredTextures=new Map();
    const request=(file,hash)=>{if(typeof file!=='string'||!file||/[\\:\x00-\x1f]/.test(file)||file.startsWith('/')||file.split('/').some(p=>p==='..'||p==='.'||!p))throw new SceneError('INVALID_PATH','Invalid scene dependency path');if(requested.has(file)&&requested.get(file)!==hash)throw new SceneError('BUILD_CONFLICT',`Conflicting asset hashes: ${file}`);requested.set(file,hash);};
    const audioFiles=new Set();let audioBytes=0;
    if(scene.lighting?.environment)request(scene.lighting.environment.file,scene.lighting.environment.sha256);
    if(scene.water)request(scene.water.file,scene.water.sha256);
    for(const ref of Object.values(scene.audio?.sounds??{})){request(ref.file,ref.sha256);audioFiles.add(ref.file);}
    for(const entity of scene.entities) {
      if(entity.mesh)request(entity.mesh.file,entity.mesh.sha256);
      if(entity.heightfield)request(entity.heightfield.file,entity.heightfield.sha256);
      if(entity.destructible?.voronoi)request(entity.destructible.voronoi,null);
      if(entity.character){request(entity.character.file,entity.character.sha256);request(path.posix.join(path.posix.dirname(entity.character.file),'animation.bin'),entity.character.binarySha256);}
    }
    for(const material of Object.values(scene.materials??{}))for(const semantic of ['albedo','normal','orm','emissive','occlusion','coat','coatRoughness','coatNormal']){
      if(['emissive','occlusion','coat','coatRoughness','coatNormal'].includes(semantic)&&material[semantic]===undefined)continue;
      request(`textures/${material[semantic]??material.albedo}.${semantic}.ngtex`,null);
    }
    for(const material of Object.values(scene.materials??{}))if(material.layered)for(const layer of material.layered.layers){
      for(const [semantic,ref] of Object.entries(layer.textures)){const file=`textures/${ref.name}.${semantic}.ngtex`;request(file,null);layeredTextures.set(file,semantic);}
      if(layer.blend.texture){const file=`textures/${layer.blend.texture.name}.mask.ngtex`;request(file,null);layeredTextures.set(file,'mask');}
    }
    for(const material of Object.values(scene.materials??{}))if(material.layered?.surfaceRelief?.heightSource?.kind==='texture'){
      const file=`textures/${material.layered.surfaceRelief.heightSource.texture.name}.height.ngtex`;request(file,null);layeredTextures.set(file,'height');
    }
    let assetBytes=dependencies.reduce((sum,file)=>sum+file.bytes,0);
    for(const [name,expectedHash]of requested) {
      const file=await fs.realpath(path.join(sceneRoot,name)),relative=path.relative(sceneRoot,file);
      if(relative.startsWith('..')||path.isAbsolute(relative))throw new SceneError('INVALID_PATH','Asset dependency escaped scene directory');
      const bytes=(await fs.stat(file)).size;assetBytes+=bytes;
      if(bytes>(audioFiles.has(name)?AUDIO_BYTES_LIMIT:256*1024*1024)||assetBytes>1024*1024*1024)throw new SceneError('BUILD_CONFLICT','Scene dependency exceeds file/scene byte budgets');
      const data=await fs.readFile(file),sha256=createHash('sha256').update(data).digest('hex');
      if(expectedHash&&sha256!==expectedHash)throw new SceneError('BUILD_CONFLICT',`Asset dependency changed: ${name}`);
      if(name===scene.water?.file){
        if(data.length>2*1024*1024)throw new SceneError('BUILD_CONFLICT','Water dependency exceeds 2 MiB');
        try{const cooked=JSON.parse(data.toString('utf8')),payload=JSON.parse(cooked.payloadJson),{derived,...source}=payload.source;
          if(cooked.schema!=='nativegpu.water-cook/1'||createHash('sha256').update(cooked.payloadJson).digest('hex')!==cooked.hash||compileWater(source).hash!==cooked.hash)throw Error('Cooked payload hash mismatch');
        }catch(error){throw new SceneError('BUILD_CONFLICT',`Invalid water dependency: ${error.message}`);}
      }
      if(layeredTextures.has(name)){
        const semantic=layeredTextures.get(name);let image;
        try{image=readNgtex(data,name);}catch(error){throw new SceneError('BUILD_CONFLICT',`Invalid layered texture ${name}: ${error.message}`);}
        if(image.semantic!==semantic||image.srgb!==(semantic==='albedo'||semantic==='emissive'))throw new SceneError('BUILD_CONFLICT',`Layered texture semantic or color space mismatch: ${name}`);
        const completeMipCount=image.width&&image.height?1+Math.floor(Math.log2(Math.max(image.width,image.height))):0;
        if(!completeMipCount||image.mipCount!==completeMipCount||data.readUInt32LE(20)+data.readUInt32LE(16)!==data.length)throw new SceneError('BUILD_CONFLICT',`Layered texture has invalid dimensions, mip chain or payload: ${name}`);
      }
      if(audioFiles.has(name)){inspectNgsnd(data);audioBytes+=data.length;if(audioBytes>AUDIO_SCENE_BYTES_LIMIT)throw new SceneError('BUILD_CONFLICT','Scene sounds exceed 256 MiB');}
      if(name===scene.lighting?.environment?.file)inspectEnvironment(data);
      const prior=dependencies.find(dependency=>dependency.path===name);
      if(prior&&prior.sha256!==sha256)throw new SceneError('BUILD_CONFLICT',`Dependency path collision: ${name}`);
      if(!prior)dependencies.push({path:name,sha256,bytes:data.length,data});
    }
  }
  // The world closure: the cooked manifest plus every chunk it names. The authoring recipe
  // and the layer files are deliberately *not* copied: a published build runs the world, it
  // does not regenerate it.
  if(scene.world){
    const manifestRel=scene.world.cookedManifest.file;
    const manifestAbs=path.resolve(sceneDirectory??assetsDirectory??'assets',manifestRel);
    let manifestBytes;
    try{manifestBytes=await fs.readFile(manifestAbs);}catch(error){throw new SceneError('BUILD_CONFLICT',`World manifest is missing: ${manifestRel}`);}
    const manifestSha=createHash('sha256').update(manifestBytes).digest('hex');
    if(manifestSha!==scene.world.cookedManifest.sha256)throw new SceneError('BUILD_CONFLICT','World manifest changed since the scene was saved');
    let manifest;
    try{manifest=JSON.parse(manifestBytes.toString('utf8'));}catch(error){throw new SceneError('BUILD_CONFLICT',`World manifest is not valid JSON: ${error.message}`);}
    if(manifest.manifestHash!==scene.world.manifestHash)throw new SceneError('BUILD_CONFLICT','World manifest hash does not match the scene reference');
    if(!Array.isArray(manifest.chunks))throw new SceneError('BUILD_CONFLICT','World manifest has no chunk list');
    dependencies.push({path:manifestRel,sha256:manifestSha,bytes:manifestBytes.length,data:manifestBytes});
    const base=path.posix.dirname(manifestRel);
    for(const chunk of manifest.chunks){
      if(typeof chunk.file!=='string'||chunk.file.split('/').some(part=>part==='..'||part==='.'||!part))throw new SceneError('INVALID_PATH','World chunk path is invalid');
      const rel=path.posix.join(base,chunk.file);
      const abs=path.resolve(sceneDirectory??assetsDirectory??'assets',rel);
      let data;
      try{data=await fs.readFile(abs);}catch(error){throw new SceneError('BUILD_CONFLICT',`World chunk is missing: ${rel}`);}
      const sha=createHash('sha256').update(data).digest('hex');
      if(sha!==chunk.sha256)throw new SceneError('BUILD_CONFLICT',`World chunk changed: ${rel}`);
      dependencies.push({path:rel,sha256:sha,bytes:data.length,data});
    }
  }
  const dependencyManifest=dependencies.map(({data,...file})=>file);
  const buildKey = createHash('sha256').update(stableJSON({ contentHash, toolVersion: TOOL_VERSION, target, dependencies:dependencyManifest })).digest('hex');
  const manifest = {
    formatVersion: 1,
    tool: { name: 'nativegpu-scene-build', version: TOOL_VERSION },
    target,
    projectId: scene.projectId,
    sceneRevision: scene.revision,
    buildKey,
    entryScene: 'scene.json',
    files: [{ path: 'scene.json', sha256: contentHash, bytes: Buffer.byteLength(sceneData) },...dependencyManifest],
    dependencies: dependencyManifest,
    entityCount: scene.entities.length,
    includedAssetTypes: ['procedural-box','lua-behavior',...(scene.entities.some(entity=>entity.mesh)?['imported-mesh']:[]),...(scene.entities.some(entity=>entity.character)?['skinned-mesh','animation']:[]),...(scene.entities.some(entity=>entity.mesh||entity.character)?['cooked-texture']:[]),...(Object.keys(scene.audio?.sounds??{}).length?['cooked-audio']:[]),...(scene.lighting?.environment?['environment-map']:[]),...(scene.water?['water-body']:[]),...(scene.world?['world-chunks','world-manifest']:[])],
  };
  const root = path.resolve(outputDirectory);
  const packageDirectory = path.join(root, buildKey);
  await fs.mkdir(root, { recursive: true });
  if (await exists(packageDirectory)) {
    const prior = await readJSON(path.join(packageDirectory, 'manifest.json'));
    const priorScene = await fs.readFile(path.join(packageDirectory, 'scene.json'), 'utf8');
    async function inventory(directory,prefix='') {
      const entries=[];
      for(const entry of await fs.readdir(directory,{withFileTypes:true})) {
        if(entry.isSymbolicLink())throw new SceneError('BUILD_CONFLICT','Package contains a symbolic link');
        if(entry.isDirectory())entries.push(...await inventory(path.join(directory,entry.name),`${prefix}${entry.name}/`));
        else entries.push(`${prefix}${entry.name}`);
      }
      return entries.sort();
    }
    const entries = await inventory(packageDirectory);
    for(const dependency of dependencies) {
      const data=await fs.readFile(path.join(packageDirectory,dependency.path));
      if(!data.equals(Buffer.from(dependency.data)))throw new SceneError('BUILD_CONFLICT','Packaged dependency changed');
    }
    if (stableJSON(prior) !== stableJSON(manifest) || priorScene !== sceneData
        || entries.join(',') !== ['manifest.json',...manifest.files.map(file=>file.path)].sort().join(',')) {
      throw new SceneError('BUILD_CONFLICT', 'An existing content-addressed package differs from its expected contents');
    }
    return { packageDirectory, manifest, cacheHit: true };
  }
  const staging = path.join(root, `.scene-build-${randomUUID()}`);
  await fs.mkdir(staging);
  try {
    await atomicWrite(path.join(staging, 'scene.json'), scene);
    await atomicWrite(path.join(staging, 'manifest.json'), manifest);
    for(const dependency of dependencies) {
      const file=path.join(staging,dependency.path);await fs.mkdir(path.dirname(file),{recursive:true});
      await fs.writeFile(file,dependency.data,'utf8');
    }
    await fs.rename(staging, packageDirectory);
  } finally {
    // Only remove the exact staging directory constructed under the resolved output root.
    if (path.dirname(staging) !== root || !path.basename(staging).startsWith('.scene-build-')) {
      throw new SceneError('UNSAFE_PATH', 'Unexpected staging path');
    }
    await fs.rm(staging, { recursive: true, force: true });
  }
  return { packageDirectory, manifest, cacheHit: false };
}

// Context never enters scene JSON or its content hash. Loaded scene objects retain
// their root for pure editor queries; serialized callers can supply assetsDirectory.
const sceneContexts=new WeakMap();
export const sceneAssets=scene=>sceneContexts.get(scene)??currentGameplayAssets();
function sceneResult(value,assets){
  if(value&&typeof value==='object'){if(value.schemaVersion)sceneContexts.set(value,assets);if(value.scene?.schemaVersion)sceneContexts.set(value.scene,assets);}
  return value;
}
function pureSceneCall(operation,scene,args,options={}){
  const assets=options.assetsDirectory??sceneContexts.get(scene)??currentGameplayAssets();
  return withGameplayAssets(assets,()=>{
    const result=operation(scene,...args);
    if(scene&&typeof scene==='object'&&(operation!==scoped_validateScene||result.valid))sceneContexts.set(scene,assets);
    return sceneResult(result,assets);
  });
}
export const validateScene=(scene,options={})=>pureSceneCall(scoped_validateScene,scene,[],options);
export const sceneHash=(scene,...args)=>pureSceneCall(scoped_sceneHash,scene,args);
export const queryScene=(scene,...args)=>pureSceneCall(scoped_queryScene,scene,args);
export const previewTransaction=(scene,...args)=>pureSceneCall(scoped_previewTransaction,scene,args);
export const applyTransaction=(scene,...args)=>pureSceneCall(scoped_applyTransaction,scene,args);
export const undoScene=(scene,...args)=>pureSceneCall(scoped_undoScene,scene,args);
export const loadScene=(file,{assetsDirectory}={})=>{const assets=assetsDirectory??findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_loadScene(file),assets));};
export const saveScene=(file,...args)=>{const assets=findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_saveScene(file,...args),assets));};
export const commitScene=(file,...args)=>{const assets=findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_commitScene(file,...args),assets));};
export const commitSceneUpdate=(file,...args)=>{const assets=findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_commitSceneUpdate(file,...args),assets));};
export const undoSceneFile=(file,...args)=>{const assets=findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_undoSceneFile(file,...args),assets));};
export const recoverScene=(file,...args)=>{const assets=findGameplayAssets(file);return withGameplayAssets(assets,async()=>sceneResult(await scoped_recoverScene(file,...args),assets));};
export const buildScene=(sceneOrFile,output,options={})=>{const assets=options.assetsDirectory??(typeof sceneOrFile==='string'?findGameplayAssets(sceneOrFile):sceneContexts.get(sceneOrFile))??currentGameplayAssets();return withGameplayAssets(assets,()=>scoped_buildScene(sceneOrFile,output,{...options,assetsDirectory:assets}));};

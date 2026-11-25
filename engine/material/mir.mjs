// Typed Material IR (MIR v0) for the material pipeline.
//
// The scene still stores a small data-driven material record. MIR is the canonical typed
// layer above it: editors and AI agents patch MIR, the runtime consumes a compiled material
// record. This module is deliberately dependency-free so it can run in the editor, CLI and
// tests without a browser or a GPU.
import { createHash } from 'node:crypto';
import {validateTextureMappings} from './texture-mapping.mjs';
import {validateIor} from './ior.mjs';

export const MIR_SCHEMA = 'nativegpu.mir/1';

const TEXTURE_KEYS = ['albedo', 'normal', 'orm', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal'];
const SCALAR_KEYS = ['opacity', 'ior', 'uvScale', 'roughness', 'metalness', 'wetness', 'wear'];
const LAYER_KEYS = ['clearcoat', 'clearcoatRoughness'];
const NAME_PATTERN = /^[A-Za-z0-9_.-]{1,64}$/;

export class MirError extends Error {
  constructor(message, path = '$') {
    super(`${path}: ${message}`);
    this.name = 'MirError';
    this.path = path;
  }
}

function fail(path, message) {
  throw new MirError(message, path);
}

function checkTexture(value, path) {
  if (typeof value !== 'string' || !NAME_PATTERN.test(value)) {
    fail(path, 'must be a texture base name');
  }
}

function checkScalar(value, key, path) {
  if(key==='ior'){try{validateIor(value);}catch(error){fail(path,error.message);}return;}
  if (!Number.isFinite(value)) fail(path, 'must be a finite number');
  const low = key === 'uvScale' ? 0.05 : 0;
  const high = key === 'uvScale' ? 1000 : 1;
  if (value < low || value > high) fail(path, `must be in [${low}, ${high}]`);
}

/** Convert a scene material into a canonical, typed MIR document. */
export function materialToMir(name, material) {
  if (!NAME_PATTERN.test(name ?? '')) fail('$.name', 'must be a material name');
  if (!material || typeof material !== 'object' || Array.isArray(material)) {
    fail('$.material', 'must be an object');
  }
  const mir = {
    schema: MIR_SCHEMA,
    name,
    base: {},
    params: {},
    layers: [],
  };
  for (const key of Object.keys(material)) {
    if (TEXTURE_KEYS.includes(key)) {
      checkTexture(material[key], `$.base.${key}`);
      mir.base[key] = { kind: 'texture', name: material[key] };
    } else if(key==='unlit'){
      if(typeof material[key]!=='boolean')fail('$.unlit','must be boolean');mir.unlit=material[key];
    } else if(key==='occlusionStrength'){
      checkScalar(material[key],key,'$.occlusion.strength');mir.occlusion={strength:material[key]};
    } else if(key==='textureMappings'){
      validateTextureMappings(material[key]);mir.textureMappings=structuredClone(material[key]);
    } else if(key==='emissiveFactor'||key==='emissiveStrength') {
      mir.emission??={factor:[1,1,1],strength:1};
      if(key==='emissiveFactor')mir.emission.factor=structuredClone(material[key]);else mir.emission.strength=material[key];
    } else if (SCALAR_KEYS.includes(key)) {
      checkScalar(material[key], key, `$.params.${key}`);
      mir.params[key] = material[key];
    } else if(key==='alphaMode'||key==='alphaCutoff') {
      mir.alpha??={mode:'OPAQUE',cutoff:.5};
      if(key==='alphaMode'){if(!['OPAQUE','MASK','BLEND'].includes(material[key]))fail('$.alpha.mode','must be OPAQUE, MASK or BLEND');mir.alpha.mode=material[key];}
      else {checkScalar(material[key],key,'$.alpha.cutoff');mir.alpha.cutoff=material[key];}
    } else if (LAYER_KEYS.includes(key)) {
      const value = material[key];
      if (!Number.isFinite(value)) fail(`$.layers[0].${key}`, 'must be a finite number');
      if (key === 'clearcoat' && (value < 0 || value > 1)) fail(`$.layers[0].${key}`, 'must be in [0, 1]');
      if (key === 'clearcoatRoughness' && (value < 0 || value > 1)) fail(`$.layers[0].${key}`, 'must be in [0, 1]');
      mir.layers[0] = { ...(mir.layers[0] ?? { kind: 'clearcoat' }), [key]: value };
    } else {
      fail(`$.material.${key}`, 'unsupported material field');
    }
  }
  if (!mir.base.albedo) mir.base.albedo = { kind: 'texture', name: 'flat' };
  if (!mir.base.normal) mir.base.normal = { kind: 'texture', name: 'flat' };
  if (!mir.base.orm) mir.base.orm = { kind: 'texture', name: 'flat' };
  if (!mir.params.uvScale) mir.params.uvScale = 1;
  if (!Number.isFinite(mir.params.roughness)) mir.params.roughness = 1;
  if (!Number.isFinite(mir.params.metalness)) mir.params.metalness = 1;
  if (!Number.isFinite(mir.params.wetness)) mir.params.wetness = 0;
  if (!Number.isFinite(mir.params.wear)) mir.params.wear = 0;
  validateMir(mir);return canonicalize(mir);
}

/** Rebuild a scene material from MIR. Unknown fields are rejected by validateMir first. */
export function mirToMaterial(mir) {
  validateMir(mir);
  const material = {
    albedo: mir.base.albedo.name,
    normal: mir.base.normal.name,
    orm: mir.base.orm.name,
    uvScale: mir.params.uvScale,
    roughness: mir.params.roughness,
    metalness: mir.params.metalness,
    wetness: mir.params.wetness,
    wear: mir.params.wear,
  };
  const layer = mir.layers[0];
  if(mir.params.opacity!==undefined)material.opacity=mir.params.opacity;
  if(mir.params.ior!==undefined)material.ior=mir.params.ior;
  if(mir.unlit!==undefined)material.unlit=mir.unlit;
  if(mir.base.occlusion)material.occlusion=mir.base.occlusion.name;
  for(const key of ['coat','coatRoughness','coatNormal'])if(mir.base[key])material[key]=mir.base[key].name;
  if(mir.occlusion)material.occlusionStrength=mir.occlusion.strength;
  if(mir.textureMappings!==undefined)material.textureMappings=structuredClone(mir.textureMappings);
  if(mir.base.emissive)material.emissive=mir.base.emissive.name;
  if(mir.emission){material.emissiveFactor=[...mir.emission.factor];material.emissiveStrength=mir.emission.strength;}
  if(mir.alpha){material.alphaMode=mir.alpha.mode;material.alphaCutoff=mir.alpha.cutoff;}
  if (layer) {
    if (layer.clearcoat !== undefined) material.clearcoat = layer.clearcoat;
    if (layer.clearcoatRoughness !== undefined) material.clearcoatRoughness = layer.clearcoatRoughness;
  }
  return material;
}

function canonicalValue(value) {
  if (Array.isArray(value)) return value.map(canonicalValue);
  if (value && typeof value === 'object') {
    const result = {};
    for (const key of Object.keys(value).sort()) result[key] = canonicalValue(value[key]);
    return result;
  }
  return value;
}

export function canonicalize(mir) {
  return canonicalValue(mir);
}

export function canonicalJson(mir) {
  return JSON.stringify(canonicalize(mir));
}

export function mirHash(mir) {
  return createHash('sha256').update(canonicalJson(mir)).digest('hex');
}

/** Feature bits consumed by the HLSL compiler / material record cache. */
export function mirFeatureBits(mir) {
  validateMir(mir);
  const bits = [];
  if(mir.unlit)bits.push('unlit');
  if(mir.params.ior!==undefined)bits.push('dielectric-ior');
  if (mir.base.normal.name !== 'flat') bits.push('normal-map');
  if (mir.params.wetness > 0) bits.push('wet');
  if (mir.params.wear > 0) bits.push('wear');
  if (mir.layers.length > 0) bits.push('clearcoat');
  if(mir.alpha?.mode==='MASK')bits.push('alpha-mask');
  if(mir.alpha?.mode==='BLEND')bits.push('alpha-blend');
  if(mir.base.emissive)bits.push('emissive-map');
  if(mir.base.occlusion)bits.push('occlusion-map');
  for(const key of ['coat','coatRoughness','coatNormal'])if(mir.base[key])bits.push(key+'-map');
  if(Object.keys(mir.textureMappings??{}).length)bits.push('texture-mapping');
  return bits;
}

export function validateMir(mir) {
  if (!mir || typeof mir !== 'object' || Array.isArray(mir)) fail('$', 'must be an object');
  for (const key of Object.keys(mir)) {
    if (!['schema','name','base','params','layers','alpha','emission','textureMappings','occlusion','unlit'].includes(key)) fail(`$.${key}`, 'unsupported field');
  }
  if (mir.schema !== MIR_SCHEMA) fail('$.schema', `must be ${MIR_SCHEMA}`);
  if(mir.unlit!==undefined&&typeof mir.unlit!=='boolean')fail('$.unlit','must be boolean');
  if(mir.occlusion!==undefined){if(!mir.occlusion||typeof mir.occlusion!=='object'||Array.isArray(mir.occlusion)||Object.keys(mir.occlusion).some(k=>k!=='strength'))fail('$.occlusion','must contain strength');checkScalar(mir.occlusion.strength,'occlusionStrength','$.occlusion.strength');}
  if(mir.textureMappings!==undefined)try{validateTextureMappings(mir.textureMappings);}catch(error){fail('$.textureMappings',error.message);}
  if (!NAME_PATTERN.test(mir.name ?? '')) fail('$.name', 'must be a material name');
  if(mir.emission!==undefined){const value=mir.emission;
    if(!value||typeof value!=='object'||Array.isArray(value)||Object.keys(value).some(k=>!['factor','strength'].includes(k)))fail('$.emission','must contain factor and strength');
    if(!Array.isArray(value.factor)||value.factor.length!==3||value.factor.some(v=>!Number.isFinite(v)||v<0||v>1))fail('$.emission.factor','must contain three numbers in [0,1]');
    if(!Number.isFinite(value.strength)||value.strength<0||value.strength>1000000)fail('$.emission.strength','must be in [0,1000000]');
  }
  if(mir.alpha!==undefined) {
    if(!mir.alpha||Array.isArray(mir.alpha)||typeof mir.alpha!=='object'||Object.keys(mir.alpha).some(key=>!['mode','cutoff'].includes(key)))fail('$.alpha','must contain mode and cutoff');
    if(!['OPAQUE','MASK','BLEND'].includes(mir.alpha.mode))fail('$.alpha.mode','must be OPAQUE, MASK or BLEND');
    checkScalar(mir.alpha.cutoff,'alphaCutoff','$.alpha.cutoff');
  }
  if (!mir.base || typeof mir.base !== 'object' || Array.isArray(mir.base)) fail('$.base', 'must be an object');
  for (const key of Object.keys(mir.base)) {
    if (!TEXTURE_KEYS.includes(key)) fail(`$.base.${key}`, 'unsupported texture');
  }
  for (const key of TEXTURE_KEYS) {
    const entry = mir.base[key];
    if(['emissive','occlusion','coat','coatRoughness','coatNormal'].includes(key)&&entry===undefined)continue;
    if (!entry || entry.kind !== 'texture') fail(`$.base.${key}`, 'must be a texture reference');
    checkTexture(entry.name, `$.base.${key}.name`);
  }
  if (!mir.params || typeof mir.params !== 'object' || Array.isArray(mir.params)) fail('$.params', 'must be an object');
  for (const key of SCALAR_KEYS) if(!['opacity','ior'].includes(key)||mir.params[key]!==undefined)checkScalar(mir.params[key],key,`$.params.${key}`);
  for (const key of Object.keys(mir.params)) {
    if (!SCALAR_KEYS.includes(key)) fail(`$.params.${key}`, 'unsupported parameter');
    checkScalar(mir.params[key], key, `$.params.${key}`);
  }
  if (!Array.isArray(mir.layers) || mir.layers.length > 1) fail('$.layers', 'supports zero or one layer');
  const layer = mir.layers[0];
  if (layer) {
    if (layer.kind !== 'clearcoat') fail('$.layers[0].kind', 'only clearcoat is supported');
    for (const key of Object.keys(layer)) {
      if (key === 'kind') continue;
      if (!LAYER_KEYS.includes(key)) fail(`$.layers[0].${key}`, 'unsupported layer field');
      if (!Number.isFinite(layer[key]) || layer[key] < 0 || layer[key] > 1) {
        fail(`$.layers[0].${key}`, 'must be in [0, 1]');
      }
    }
  }
  return true;
}

/** Compile MIR to the runtime-facing material plus its stable hash/feature bits. */
export function compileMir(mir) {
  validateMir(mir);
  return {
    schema: 'nativegpu.mir-compiled/1',
    hash: mirHash(mir),
    features: mirFeatureBits(mir),
    material: mirToMaterial(mir),
  };
}

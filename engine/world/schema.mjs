// World authoring contracts: recipe v1, library v1, layer v1, overrides v1.
//
// The engine's scene validator is strict on purpose — an unknown key is an error, not a
// warning — and this module follows the same rule. A world that silently ignored a typo in
// `budgetProfile` would cook with the wrong budget and look correct until it dropped frames.
//
// Nothing here writes files or touches the GPU. It is pure validation plus normalization,
// so the editor service, the AI tool surface and the tests all agree on one definition of
// "a valid world" without a running host.

import { WORLD_LIMITS, chunkRangeForBounds } from './chunk.mjs';
import { isSha256, validSeed } from './hash.mjs';
import { WORLD_ERROR_CODES, WorldError } from './errors.mjs';

export const RECIPE_SCHEMA = 'nativegpu.world/1';
export const LIBRARY_SCHEMA = 'nativegpu.world-library/1';
export const LAYER_SCHEMA = 'nativegpu.world-layer/1';
export const OVERRIDES_SCHEMA = 'nativegpu.world-overrides/1';
export const MANIFEST_SCHEMA = 'nativegpu.world-manifest/1';
export const CHUNK_FORMAT = 'NGRC';
export const CHUNK_VERSION = 1;
export const GENERATOR_VERSION = 'worldgen-1';

export const TERRAIN_PRESETS = Object.freeze([
  'flat', 'mountain-valley', 'rolling-hills', 'ridge-basin', 'river-plain',
]);
export const BUDGET_PROFILES = Object.freeze(['debug', 'desktop-balanced', 'desktop-quality', 'stress']);
export const LAYER_KINDS = Object.freeze([
  'terrain', 'water', 'road', 'scatter', 'settlement', 'material', 'decor',
]);
export const COMPONENT_CATEGORIES = Object.freeze([
  'ground-cover', 'grass', 'tree', 'rock', 'road-piece', 'bridge',
  'building-prefab', 'building-module',
]);
export const PATH_KINDS = Object.freeze(['river', 'road']);
export const BRUSH_SHAPES = Object.freeze(['circle', 'rectangle', 'polygon']);

// Budget table. These are the numbers the compiler enforces when it reports
// WORLD_BUDGET_EXCEEDED; the UI shows them so "why did my forest shrink" has an answer.
export const BUDGETS = Object.freeze({
  debug: { instancesPerChunk: 8192, totalInstances: 65536, chunks: 64, cookMillis: 4000 },
  'desktop-balanced': { instancesPerChunk: 32768, totalInstances: 1200000, chunks: 65536, cookMillis: 60000 },
  'desktop-quality': { instancesPerChunk: 65536, totalInstances: 2400000, chunks: 65536, cookMillis: 120000 },
  stress: { instancesPerChunk: 262144, totalInstances: 8000000, chunks: 262144, cookMillis: 300000 },
});

const FILE_REF = /^[A-Za-z0-9_./-]{1,240}$/;
const ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$/;

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function collect(errors, location, message) {
  errors.push(`${location}: ${message}`);
}

function exactKeys(value, required, optional, location, errors) {
  if (!isObject(value)) {
    collect(errors, location, 'must be an object');
    return false;
  }
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) collect(errors, location, `key '${key}' is not supported`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) collect(errors, location, `key '${key}' is required`);
  }
  return true;
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function inRange(value, low, high) {
  return finite(value) && value >= low && value <= high;
}

function vector(value, length, low, high) {
  return Array.isArray(value) && value.length === length
    && value.every(item => inRange(item, low, high));
}

function relativePath(value, suffix) {
  if (typeof value !== 'string' || value.length === 0 || value.length > 240) return false;
  if (!FILE_REF.test(value)) return false;
  if (value.startsWith('/') || /^[A-Za-z]:/.test(value)) return false;
  if (value.split('/').some(part => part === '' || part === '.' || part === '..')) return false;
  if (suffix && !value.endsWith(suffix)) return false;
  return true;
}

function id(value) {
  return typeof value === 'string' && ID_PATTERN.test(value);
}

// ---------------------------------------------------------------- recipe

const RECIPE_REQUIRED = ['schema', 'worldId', 'generatorVersion', 'seed', 'extentMetres', 'chunkSizeMetres', 'terrain'];
const RECIPE_OPTIONAL = ['originMetres', 'libraries', 'layers', 'overrides', 'budgetProfile', 'name', 'description', 'locks', 'savedAt'];

export function validateRecipe(recipe, { errors = [] } = {}) {
  if (!exactKeys(recipe, RECIPE_REQUIRED, RECIPE_OPTIONAL, 'recipe', errors)) return errors;

  if (recipe.schema !== RECIPE_SCHEMA) collect(errors, 'recipe.schema', `must be '${RECIPE_SCHEMA}'`);
  if (!id(recipe.worldId)) collect(errors, 'recipe.worldId', 'must be 1-64 identifier characters');
  if (typeof recipe.generatorVersion !== 'string' || !/^worldgen-\d+$/.test(recipe.generatorVersion)) {
    collect(errors, 'recipe.generatorVersion', 'must look like worldgen-1');
  }
  if (!validSeed(recipe.seed)) {
    collect(errors, 'recipe.seed', 'must be a decimal string within uint64 (JS numbers lose precision above 2^53)');
  }

  if (!vector(recipe.extentMetres, 2, WORLD_LIMITS.extentMetres[0], WORLD_LIMITS.extentMetres[1])) {
    collect(errors, 'recipe.extentMetres', `must be two numbers in [${WORLD_LIMITS.extentMetres.join(', ')}] metres`);
  }
  if (!inRange(recipe.chunkSizeMetres, WORLD_LIMITS.chunkSizeMetres[0], WORLD_LIMITS.chunkSizeMetres[1])) {
    collect(errors, 'recipe.chunkSizeMetres', `must be in [${WORLD_LIMITS.chunkSizeMetres.join(', ')}] metres`);
  }

  if (recipe.originMetres !== undefined && !vector(recipe.originMetres, 3, -100000, 100000)) {
    collect(errors, 'recipe.originMetres', 'must be three finite metres within ±100000');
  }

  if (exactKeys(recipe.terrain, ['sampleStepMetres'], ['preset', 'amplitudeMetres', 'baseHeightMetres', 'ridgeStrength', 'erosionPasses', 'material', 'seedOffset'], 'recipe.terrain', errors)) {
    if (!inRange(recipe.terrain.sampleStepMetres, WORLD_LIMITS.sampleStepMetres[0], WORLD_LIMITS.sampleStepMetres[1])) {
      collect(errors, 'recipe.terrain.sampleStepMetres', `must be in [${WORLD_LIMITS.sampleStepMetres.join(', ')}] metres`);
    }
    if (recipe.terrain.preset !== undefined && !TERRAIN_PRESETS.includes(recipe.terrain.preset)) {
      collect(errors, 'recipe.terrain.preset', `must be one of ${TERRAIN_PRESETS.join(', ')}`);
    }
    for (const [key, low, high] of [['amplitudeMetres', 0, 1200], ['baseHeightMetres', -500, 4000],
      ['ridgeStrength', 0, 1], ['erosionPasses', 0, 8]]) {
      if (recipe.terrain[key] !== undefined && !inRange(recipe.terrain[key], low, high)) {
        collect(errors, `recipe.terrain.${key}`, `must be in [${low}, ${high}]`);
      }
    }
    if (recipe.terrain.erosionPasses !== undefined && !Number.isInteger(recipe.terrain.erosionPasses)) {
      collect(errors, 'recipe.terrain.erosionPasses', 'must be an integer');
    }
  }

  if (recipe.budgetProfile !== undefined && !BUDGET_PROFILES.includes(recipe.budgetProfile)) {
    collect(errors, 'recipe.budgetProfile', `must be one of ${BUDGET_PROFILES.join(', ')}`);
  }

  // Libraries and layers are *references*. The recipe does not inline their content: a
  // 16 MiB recipe would defeat the file limit the scene module already enforces.
  for (const [key, limit] of [['libraries', WORLD_LIMITS.libraries], ['layers', WORLD_LIMITS.layers], ['overrides', WORLD_LIMITS.overrides]]) {
    const list = recipe[key];
    if (list === undefined) continue;
    if (!Array.isArray(list)) { collect(errors, `recipe.${key}`, 'must be an array'); continue; }
    if (list.length > limit) collect(errors, `recipe.${key}`, `supports at most ${limit} entries`);
    const seen = new Set();
    list.forEach((entry, index) => {
      const location = `recipe.${key}[${index}]`;
      const required = key === 'overrides' ? ['region'] : ['id', 'file', 'sha256'];
      const optional = key === 'overrides' ? ['id', 'file', 'sha256'] : [];
      if (!exactKeys(entry, required, optional, location, errors)) return;
      if (entry.id !== undefined) {
        if (!id(entry.id)) collect(errors, `${location}.id`, 'must be 1-64 identifier characters');
        else if (seen.has(entry.id)) collect(errors, `${location}.id`, `duplicates '${entry.id}'`);
        else seen.add(entry.id);
      }
      if (entry.file !== undefined && !relativePath(entry.file, key === 'libraries' ? 'library.json' : '.json')) {
        collect(errors, `${location}.file`, 'must be a project-relative .json path without traversal');
      }
      if (entry.sha256 !== undefined && !isSha256(entry.sha256)) {
        collect(errors, `${location}.sha256`, 'must be 64 lowercase hex digits');
      }
      if (key === 'overrides' && entry.region !== undefined) {
        const region = entry.region;
        if (!exactKeys(region, ['minX', 'minZ', 'maxX', 'maxZ'], [], `${location}.region`, errors)) return;
        if (![region.minX, region.minZ, region.maxX, region.maxZ].every(value => finite(value) && Math.abs(value) <= 100000)) {
          collect(errors, `${location}.region`, 'must be four finite metres within ±100000');
        } else if (region.maxX <= region.minX || region.maxZ <= region.minZ) {
          collect(errors, `${location}.region`, 'must have positive width and depth');
        }
      }
    });
  }

  if (recipe.locks !== undefined) {
    if (!Array.isArray(recipe.locks)) collect(errors, 'recipe.locks', 'must be an array');
    else if (recipe.locks.length > WORLD_LIMITS.overrides) collect(errors, 'recipe.locks', `supports at most ${WORLD_LIMITS.overrides} regions`);
    else recipe.locks.forEach((lock, index) => {
      const location = `recipe.locks[${index}]`;
      if (!exactKeys(lock, ['minX', 'minZ', 'maxX', 'maxZ'], ['layerId'], location, errors)) return;
      if (![lock.minX, lock.minZ, lock.maxX, lock.maxZ].every(value => finite(value) && Math.abs(value) <= 100000)) {
        collect(errors, location, 'must be four finite metres within ±100000');
      }
    });
  }

  return errors;
}

// ---------------------------------------------------------------- library

const COMPONENT_REQUIRED = ['id', 'category', 'sizeMetres'];
const COMPONENT_OPTIONAL = [
  'asset', 'axis', 'pivot', 'styleTags', 'lod', 'collision', 'scaleRange', 'placement',
  'markers', 'material', 'label', 'notes', 'missing', 'geometry',
];

// Procedural stand-in geometry. A library may name a real imported asset, or it may
// declare a primitive the compiler builds from `sizeMetres`. The second form is what makes
// a world cookable before any art exists, and the editor labels it as a stand-in so nobody
// mistakes a box for a finished building.
export const COMPONENT_SHAPES = Object.freeze(['box', 'cylinder', 'cone', 'sphere', 'prism', 'cross']);

export function validateLibrary(library, { errors = [] } = {}) {
  if (!exactKeys(library, ['schema', 'id', 'components'], ['name', 'styleTags', 'savedAt'], 'library', errors)) return errors;
  if (library.schema !== LIBRARY_SCHEMA) collect(errors, 'library.schema', `must be '${LIBRARY_SCHEMA}'`);
  if (!id(library.id)) collect(errors, 'library.id', 'must be 1-64 identifier characters');
  if (!Array.isArray(library.components)) { collect(errors, 'library.components', 'must be an array'); return errors; }
  if (library.components.length > 4096) collect(errors, 'library.components', 'supports at most 4096 components');

  const seen = new Set();
  library.components.forEach((component, index) => {
    const location = `library.components[${index}]`;
    if (!exactKeys(component, COMPONENT_REQUIRED, COMPONENT_OPTIONAL, location, errors)) return;
    if (!id(component.id)) collect(errors, `${location}.id`, 'must be 1-64 identifier characters');
    else if (seen.has(component.id)) collect(errors, `${location}.id`, `duplicates '${component.id}'`);
    else seen.add(component.id);
    if (!COMPONENT_CATEGORIES.includes(component.category)) {
      collect(errors, `${location}.category`, `must be one of ${COMPONENT_CATEGORIES.join(', ')}`);
    }
    if (!vector(component.sizeMetres, 3, 0.001, 2000)) {
      collect(errors, `${location}.sizeMetres`, 'must be three positive metre dimensions');
    }
    if (component.axis !== undefined && !['+Y', '-Z', '+Z', '+X'].includes(component.axis)) {
      collect(errors, `${location}.axis`, 'must be +Y, -Z, +Z or +X');
    }
    if (component.pivot !== undefined && !['base', 'center', 'custom'].includes(component.pivot)) {
      collect(errors, `${location}.pivot`, 'must be base, center or custom');
    }
    if (component.asset !== undefined) {
      if (exactKeys(component.asset, ['file', 'sha256'], [], `${location}.asset`, errors)) {
        if (!relativePath(component.asset.file)) collect(errors, `${location}.asset.file`, 'must be a project-relative path');
        if (!isSha256(component.asset.sha256)) collect(errors, `${location}.asset.sha256`, 'must be 64 lowercase hex digits');
      }
    }
    if (component.geometry !== undefined) {
      if (exactKeys(component.geometry, ['shape'], ['sides', 'segments', 'taper', 'tiltDegrees', 'hollow'], `${location}.geometry`, errors)) {
        if (!COMPONENT_SHAPES.includes(component.geometry.shape)) {
          collect(errors, `${location}.geometry.shape`, `must be one of ${COMPONENT_SHAPES.join(', ')}`);
        }
        if (component.geometry.sides !== undefined && (!Number.isInteger(component.geometry.sides) || component.geometry.sides < 3 || component.geometry.sides > 32)) {
          collect(errors, `${location}.geometry.sides`, 'must be an integer in [3, 32]');
        }
        for (const key of ['segments', 'taper', 'tiltDegrees', 'hollow']) {
          if (component.geometry[key] !== undefined && !Number.isFinite(component.geometry[key])) {
            collect(errors, `${location}.geometry.${key}`, 'must be finite');
          }
        }
      }
    }
    if (component.asset !== undefined && component.geometry !== undefined) {
      collect(errors, location, 'a component is either an imported asset or procedural geometry, not both');
    }
    if (component.styleTags !== undefined) {
      if (!Array.isArray(component.styleTags) || component.styleTags.length > 16
        || component.styleTags.some(tag => typeof tag !== 'string' || tag.length === 0 || tag.length > 32)) {
        collect(errors, `${location}.styleTags`, 'must be up to 16 short tags');
      }
    }
    if (component.lod !== undefined) {
      if (!Array.isArray(component.lod) || component.lod.length > 4) {
        collect(errors, `${location}.lod`, 'must be up to 4 LOD entries');
      } else {
        component.lod.forEach((level, lodIndex) => {
          const where = `${location}.lod[${lodIndex}]`;
          if (!exactKeys(level, ['file', 'sha256', 'maxDistanceMetres'], [], where, errors)) return;
          if (!relativePath(level.file)) collect(errors, `${where}.file`, 'must be a project-relative path');
          if (!isSha256(level.sha256)) collect(errors, `${where}.sha256`, 'must be 64 lowercase hex digits');
          if (!inRange(level.maxDistanceMetres, 1, 20000)) collect(errors, `${where}.maxDistanceMetres`, 'must be in [1, 20000]');
        });
      }
    }
    if (component.collision !== undefined) {
      if (exactKeys(component.collision, ['mode'], ['halfExtentsMetres', 'meshFile', 'meshSha256'], `${location}.collision`, errors)) {
        if (!['none', 'box', 'mesh', 'terrain'].includes(component.collision.mode)) {
          collect(errors, `${location}.collision.mode`, 'must be none, box, mesh or terrain');
        }
        if (component.collision.halfExtentsMetres !== undefined
          && !vector(component.collision.halfExtentsMetres, 3, 0.001, 2000)) {
          collect(errors, `${location}.collision.halfExtentsMetres`, 'must be three positive metre dimensions');
        }
        if (component.collision.mode === 'mesh') {
          if (!relativePath(component.collision.meshFile)) collect(errors, `${location}.collision.meshFile`, 'mesh collision needs a project-relative mesh');
          if (!isSha256(component.collision.meshSha256)) collect(errors, `${location}.collision.meshSha256`, 'must be 64 lowercase hex digits');
        }
      }
    }
    if (component.scaleRange !== undefined && !vector(component.scaleRange, 2, 0.05, 20)) {
      collect(errors, `${location}.scaleRange`, 'must be two factors in [0.05, 20]');
    } else if (Array.isArray(component.scaleRange) && component.scaleRange[0] > component.scaleRange[1]) {
      collect(errors, `${location}.scaleRange`, 'minimum must not exceed maximum');
    }
    if (component.placement !== undefined) {
      if (exactKeys(component.placement, [], ['maxSlopeDegrees', 'alignToSlope', 'minSpacingMetres',
        'maxSpacingMetres', 'densityPerHectare', 'requiresFlat', 'exclusionRadiusMetres'], `${location}.placement`, errors)) {
        if (component.placement.maxSlopeDegrees !== undefined && !inRange(component.placement.maxSlopeDegrees, 0, 89)) {
          collect(errors, `${location}.placement.maxSlopeDegrees`, 'must be in [0, 89]');
        }
        for (const key of ['minSpacingMetres', 'maxSpacingMetres', 'densityPerHectare', 'exclusionRadiusMetres']) {
          const value = component.placement[key];
          if (value === undefined) continue;
          const ceiling = key === 'densityPerHectare' ? 200000 : 5000;
          if (!inRange(value, 0, ceiling)) collect(errors, `${location}.placement.${key}`, `must be in [0, ${ceiling}]`);
        }
        if (component.placement.alignToSlope !== undefined && typeof component.placement.alignToSlope !== 'boolean') {
          collect(errors, `${location}.placement.alignToSlope`, 'must be boolean');
        }
        if (component.placement.requiresFlat !== undefined && typeof component.placement.requiresFlat !== 'boolean') {
          collect(errors, `${location}.placement.requiresFlat`, 'must be boolean');
        }
      }
    }
    if (component.markers !== undefined) {
      // Markers are the visual tagging tool's output: ground contact, forward, connection
      // points, entrances, walkable areas, stair ends, furniture clearance.
      if (exactKeys(component.markers, [], ['groundContact', 'forward', 'connections', 'entrances',
        'walkable', 'occupied', 'stairs', 'clearance'], `${location}.markers`, errors)) return;
      const markers = component.markers;
      if (markers.groundContact !== undefined && !vector(markers.groundContact, 3, -2000, 2000)) {
        collect(errors, `${location}.markers.groundContact`, 'must be three metres');
      }
      if (markers.forward !== undefined && !vector(markers.forward, 3, -1, 1)) {
        collect(errors, `${location}.markers.forward`, 'must be a direction within [-1, 1]');
      }
      for (const key of ['connections', 'entrances', 'walkable', 'occupied', 'stairs', 'clearance']) {
        const list = markers[key];
        if (list === undefined) continue;
        if (!Array.isArray(list) || list.length > 32) { collect(errors, `${location}.markers.${key}`, 'must be up to 32 entries'); continue; }
        list.forEach((marker, markerIndex) => {
          const where = `${location}.markers.${key}[${markerIndex}]`;
          if (!exactKeys(marker, ['position'], ['direction', 'radiusMetres', 'label', 'level'], where, errors)) return;
          if (!vector(marker.position, 3, -2000, 2000)) collect(errors, `${where}.position`, 'must be three metres');
          if (marker.direction !== undefined && !vector(marker.direction, 3, -1, 1)) collect(errors, `${where}.direction`, 'must be a direction');
          if (marker.radiusMetres !== undefined && !inRange(marker.radiusMetres, 0, 500)) collect(errors, `${where}.radiusMetres`, 'must be in [0, 500]');
          if (marker.level !== undefined && (!Number.isInteger(marker.level) || marker.level < -8 || marker.level > 64)) {
            collect(errors, `${where}.level`, 'must be an integer floor index');
          }
        });
      }
    }
  });

  return errors;
}

// ---------------------------------------------------------------- layer

const LAYER_REQUIRED = ['schema', 'id', 'kind'];
const LAYER_OPTIONAL = ['visible', 'locked', 'seedOffset', 'rules', 'paths', 'regions', 'materials',
  'lockedObjects', 'deletedObjects', 'movedObjects', 'label', 'savedAt'];

export function validateLayer(layer, { errors = [] } = {}) {
  if (!exactKeys(layer, LAYER_REQUIRED, LAYER_OPTIONAL, 'layer', errors)) return errors;
  if (layer.schema !== LAYER_SCHEMA) collect(errors, 'layer.schema', `must be '${LAYER_SCHEMA}'`);
  if (!id(layer.id)) collect(errors, 'layer.id', 'must be 1-64 identifier characters');
  if (!LAYER_KINDS.includes(layer.kind)) collect(errors, 'layer.kind', `must be one of ${LAYER_KINDS.join(', ')}`);
  if (layer.visible !== undefined && typeof layer.visible !== 'boolean') collect(errors, 'layer.visible', 'must be boolean');
  if (layer.locked !== undefined && typeof layer.locked !== 'boolean') collect(errors, 'layer.locked', 'must be boolean');
  if (layer.seedOffset !== undefined && !validSeed(layer.seedOffset)) collect(errors, 'layer.seedOffset', 'must be a decimal string within uint64');

  if (layer.rules !== undefined) validateLayerRules(layer.rules, layer.kind, errors);

  if (layer.regions !== undefined) {
    if (!Array.isArray(layer.regions) || layer.regions.length > 4096) collect(errors, 'layer.regions', 'must be up to 4096 regions');
    else layer.regions.forEach((region, index) => validateRegion(region, `layer.regions[${index}]`, errors));
  }

  if (layer.paths !== undefined) {
    if (!Array.isArray(layer.paths) || layer.paths.length > WORLD_LIMITS.pathsPerLayer) {
      collect(errors, 'layer.paths', `must be up to ${WORLD_LIMITS.pathsPerLayer} paths`);
    } else layer.paths.forEach((entry, index) => validatePath(entry, `layer.paths[${index}]`, errors));
  }

  for (const key of ['lockedObjects', 'deletedObjects']) {
    const list = layer[key];
    if (list === undefined) continue;
    if (!Array.isArray(list) || list.length > 200000) { collect(errors, `layer.${key}`, 'must be up to 200000 ids'); continue; }
    list.forEach((value, index) => {
      if (typeof value !== 'string' || value.length === 0 || value.length > 256) {
        collect(errors, `layer.${key}[${index}]`, 'must be a stable object id string');
      }
    });
  }
  if (layer.movedObjects !== undefined) {
    if (!Array.isArray(layer.movedObjects) || layer.movedObjects.length > 200000) {
      collect(errors, 'layer.movedObjects', 'must be up to 200000 entries');
    } else layer.movedObjects.forEach((entry, index) => {
      const where = `layer.movedObjects[${index}]`;
      if (!exactKeys(entry, ['id'], ['position', 'yaw', 'scale'], where, errors)) return;
      if (typeof entry.id !== 'string' || entry.id.length === 0 || entry.id.length > 256) {
        collect(errors, `${where}.id`, 'must be a stable object id string');
      }
      if (entry.position !== undefined && !vector(entry.position, 3, -100000, 100000)) {
        collect(errors, `${where}.position`, 'must be three metres within ±100000');
      }
      if (entry.yaw !== undefined && !inRange(entry.yaw, -1000, 1000)) collect(errors, `${where}.yaw`, 'must be in [-1000, 1000]');
      if (entry.scale !== undefined && !inRange(entry.scale, 0.01, 100)) collect(errors, `${where}.scale`, 'must be in [0.01, 100]');
    });
  }

  return errors;
}

function validateLayerRules(rules, kind, errors) {
  if (!isObject(rules)) { collect(errors, 'layer.rules', 'must be an object'); return; }
  const allowedByKind = {
    terrain: ['preset', 'amplitudeMetres', 'baseHeightMetres', 'ridgeStrength', 'smoothingPasses', 'brush'],
    water: ['widthMetres', 'bedDepthMetres', 'bankWidthMetres', 'material', 'flowMetresPerSecond', 'bridge'],
    road: ['widthMetres', 'shoulderMetres', 'maxSlopeDegrees', 'surface', 'bridge', 'junction'],
    scatter: ['species', 'densityPerHectare', 'maxSlopeDegrees', 'alignToSlope', 'exclusionRadiusMetres',
      'minSpacingMetres', 'edgeFalloffMetres', 'clumping', 'scaleRange', 'budgetWeight'],
    settlement: ['density', 'setbackMetres', 'styleTags', 'buildingCategory', 'maxBuildings', 'roadWidthMetres',
      'lotSizeMetres', 'blockSizeMetres', 'entranceClearanceMetres', 'components', 'buildingSpacingMetres',
      'maxSlopeDegrees', 'scaleRange'],
    material: ['surface', 'blendMetres', 'moisture', 'rockiness'],
    decor: ['species', 'densityPerHectare', 'maxSlopeDegrees', 'alignToSlope', 'scaleRange'],
  };
  const allowed = allowedByKind[kind] ?? [];
  for (const key of Object.keys(rules)) {
    if (!allowed.includes(key)) collect(errors, 'layer.rules', `key '${key}' is not supported for a ${kind} layer`);
  }
  for (const [key, value] of Object.entries(rules)) {
    if (typeof value === 'number') {
      if (!Number.isFinite(value)) collect(errors, `layer.rules.${key}`, 'must be finite');
      continue;
    }
    if (typeof value === 'boolean') continue;
    if (value === null || value === undefined) continue;
    if (typeof value === 'string') { if (value.length > 128) collect(errors, `layer.rules.${key}`, 'is too long'); continue; }
    if (Array.isArray(value)) { if (value.length > 64) collect(errors, `layer.rules.${key}`, 'has too many entries'); continue; }
    if (isObject(value)) continue;
    collect(errors, `layer.rules.${key}`, 'must be a number, string, boolean, array or object');
  }
  const numeric = {
    widthMetres: [0.5, 200], bedDepthMetres: [0.05, 40], bankWidthMetres: [0.1, 200],
    flowMetresPerSecond: [0, 20], shoulderMetres: [0, 40], maxSlopeDegrees: [0, 60],
    densityPerHectare: [0, 200000], exclusionRadiusMetres: [0, 200], minSpacingMetres: [0, 200],
    edgeFalloffMetres: [0, 500], clumping: [0, 1], budgetWeight: [0.01, 4],
    density: [0, 1], setbackMetres: [0, 60], maxBuildings: [0, 5000], roadWidthMetres: [1, 60],
    lotSizeMetres: [2, 200], blockSizeMetres: [4, 400], entranceClearanceMetres: [0, 40],
    blendMetres: [0, 200], moisture: [0, 1], rockiness: [0, 1], smoothingPasses: [0, 8],
    amplitudeMetres: [0, 1200], baseHeightMetres: [-500, 4000], ridgeStrength: [0, 1],
  };
  for (const [key, [low, high]] of Object.entries(numeric)) {
    const value = rules[key];
    if (value === undefined) continue;
    if (!inRange(value, low, high)) collect(errors, `layer.rules.${key}`, `must be in [${low}, ${high}]`);
  }
  if (rules.scaleRange !== undefined && !vector(rules.scaleRange, 2, 0.05, 20)) {
    collect(errors, 'layer.rules.scaleRange', 'must be two factors in [0.05, 20]');
  }
  if (rules.alignToSlope !== undefined && typeof rules.alignToSlope !== 'boolean') {
    collect(errors, 'layer.rules.alignToSlope', 'must be boolean');
  }
}

function validateRegion(region, location, errors) {
  if (!exactKeys(region, ['shape'], ['bounds', 'center', 'radiusMetres', 'points', 'strength', 'falloffMetres', 'erase', 'preset', 'label'], location, errors)) return;
  if (!BRUSH_SHAPES.includes(region.shape)) { collect(errors, `${location}.shape`, `must be one of ${BRUSH_SHAPES.join(', ')}`); return; }
  if (region.erase !== undefined && typeof region.erase !== 'boolean') collect(errors, `${location}.erase`, 'must be boolean');
  if (region.strength !== undefined && !inRange(region.strength, 0, 1)) collect(errors, `${location}.strength`, 'must be in [0, 1]');
  if (region.falloffMetres !== undefined && !inRange(region.falloffMetres, 0, 500)) collect(errors, `${location}.falloffMetres`, 'must be in [0, 500]');
  if (region.shape === 'circle') {
    if (!vector(region.center, 2, -100000, 100000)) collect(errors, `${location}.center`, 'must be two metres');
    if (!inRange(region.radiusMetres, 0.1, 8192)) collect(errors, `${location}.radiusMetres`, 'must be in [0.1, 8192]');
  } else if (region.shape === 'rectangle') {
    const bounds = region.bounds;
    if (!isObject(bounds) || !['minX', 'minZ', 'maxX', 'maxZ'].every(key => inRange(bounds[key], -100000, 100000))) {
      collect(errors, `${location}.bounds`, 'must be four metres');
    } else if (bounds.maxX <= bounds.minX || bounds.maxZ <= bounds.minZ) {
      collect(errors, `${location}.bounds`, 'must have positive width and depth');
    }
  } else {
    if (!Array.isArray(region.points) || region.points.length < 3 || region.points.length > 1024) {
      collect(errors, `${location}.points`, 'must be 3-1024 [x, z] points');
    } else {
      region.points.forEach((point, index) => {
        if (!vector(point, 2, -100000, 100000)) collect(errors, `${location}.points[${index}]`, 'must be two metres');
      });
    }
  }
}

function validatePath(entry, location, errors) {
  if (!exactKeys(entry, ['id', 'kind', 'points'], ['widthMetres', 'closed', 'bridge', 'surface', 'bedDepthMetres',
    'maxSlopeDegrees', 'shoulderMetres', 'flowMetresPerSecond', 'label'], location, errors)) return;
  if (!id(entry.id)) collect(errors, `${location}.id`, 'must be 1-64 identifier characters');
  if (!PATH_KINDS.includes(entry.kind)) collect(errors, `${location}.kind`, `must be one of ${PATH_KINDS.join(', ')}`);
  if (!Array.isArray(entry.points) || entry.points.length < 2 || entry.points.length > WORLD_LIMITS.pointsPerPath) {
    collect(errors, `${location}.points`, `must be 2-${WORLD_LIMITS.pointsPerPath} control points`);
  } else {
    entry.points.forEach((point, index) => {
      if (!vector(point, 2, -100000, 100000)) collect(errors, `${location}.points[${index}]`, 'must be two metres');
    });
  }
  if (entry.widthMetres !== undefined && !inRange(entry.widthMetres, 0.5, 200)) {
    collect(errors, `${location}.widthMetres`, 'must be in [0.5, 200]');
  }
  if (entry.closed !== undefined && typeof entry.closed !== 'boolean') collect(errors, `${location}.closed`, 'must be boolean');
  if (entry.bridge !== undefined && typeof entry.bridge !== 'boolean') collect(errors, `${location}.bridge`, 'must be boolean');
  if (entry.bedDepthMetres !== undefined && !inRange(entry.bedDepthMetres, 0.05, 40)) {
    collect(errors, `${location}.bedDepthMetres`, 'must be in [0.05, 40]');
  }
  if (entry.maxSlopeDegrees !== undefined && !inRange(entry.maxSlopeDegrees, 0, 60)) {
    collect(errors, `${location}.maxSlopeDegrees`, 'must be in [0, 60]');
  }
  if (entry.shoulderMetres !== undefined && !inRange(entry.shoulderMetres, 0, 40)) {
    collect(errors, `${location}.shoulderMetres`, 'must be in [0, 40]');
  }
  if (entry.flowMetresPerSecond !== undefined && !inRange(entry.flowMetresPerSecond, 0, 20)) {
    collect(errors, `${location}.flowMetresPerSecond`, 'must be in [0, 20]');
  }
}

// ---------------------------------------------------------------- overrides

export function validateOverrides(overrides, { errors = [] } = {}) {
  if (!exactKeys(overrides, ['schema'], ['regionId', 'region', 'deleted', 'moved', 'locked', 'savedAt'], 'overrides', errors)) return errors;
  if (overrides.schema !== OVERRIDES_SCHEMA) collect(errors, 'overrides.schema', `must be '${OVERRIDES_SCHEMA}'`);
  if (overrides.regionId !== undefined && !id(overrides.regionId)) collect(errors, 'overrides.regionId', 'must be 1-64 identifier characters');
  if (overrides.region !== undefined) {
    const region = overrides.region;
    if (!isObject(region) || !['minX', 'minZ', 'maxX', 'maxZ'].every(key => inRange(region[key], -100000, 100000))) {
      collect(errors, 'overrides.region', 'must be four metres');
    }
  }
  for (const key of ['deleted', 'locked']) {
    const list = overrides[key];
    if (list === undefined) continue;
    if (!Array.isArray(list) || list.length > 200000) { collect(errors, `overrides.${key}`, 'must be up to 200000 ids'); continue; }
    list.forEach((value, index) => {
      if (typeof value !== 'string' || value.length === 0 || value.length > 256) {
        collect(errors, `overrides.${key}[${index}]`, 'must be a stable object id string');
      }
    });
  }
  if (overrides.moved !== undefined) {
    if (!Array.isArray(overrides.moved) || overrides.moved.length > 200000) {
      collect(errors, 'overrides.moved', 'must be up to 200000 entries');
    } else overrides.moved.forEach((entry, index) => {
      const where = `overrides.moved[${index}]`;
      if (!exactKeys(entry, ['id'], ['position', 'yaw', 'scale'], where, errors)) return;
      if (typeof entry.id !== 'string' || entry.id.length === 0) collect(errors, `${where}.id`, 'must be a stable object id string');
      if (entry.position !== undefined && !vector(entry.position, 3, -100000, 100000)) collect(errors, `${where}.position`, 'must be three metres');
      if (entry.yaw !== undefined && !inRange(entry.yaw, -1000, 1000)) collect(errors, `${where}.yaw`, 'must be in [-1000, 1000]');
      if (entry.scale !== undefined && !inRange(entry.scale, 0.01, 100)) collect(errors, `${where}.scale`, 'must be in [0.01, 100]');
    });
  }
  return errors;
}

// ---------------------------------------------------------------- entry points

function throwIfInvalid(errors, code, what) {
  if (errors.length === 0) return;
  throw new WorldError(code, `${what} is invalid: ${errors[0]}`, {
    stage: 'validate',
    suggestion: 'Fix the listed fields; the world contract rejects unknown keys instead of ignoring them.',
    cause: errors.slice(0, 32),
  });
}

export function assertRecipe(recipe) {
  throwIfInvalid(validateRecipe(recipe), WORLD_ERROR_CODES.SCHEMA_INVALID, 'world recipe');
  return recipe;
}

export function assertLibrary(library) {
  throwIfInvalid(validateLibrary(library), WORLD_ERROR_CODES.SCHEMA_INVALID, 'world library');
  return library;
}

export function assertLayer(layer) {
  throwIfInvalid(validateLayer(layer), WORLD_ERROR_CODES.SCHEMA_INVALID, 'world layer');
  return layer;
}

export function assertOverrides(overrides) {
  throwIfInvalid(validateOverrides(overrides), WORLD_ERROR_CODES.SCHEMA_INVALID, 'world overrides');
  return overrides;
}

// Normalized recipe: defaults filled in, arrays sorted by id. Normalization is part of the
// input digest, so two authoring sessions that omit `budgetProfile` hash the same as one
// that writes the default explicitly.
export function normalizeRecipe(recipe) {
  const normalized = {
    schema: RECIPE_SCHEMA,
    worldId: recipe.worldId,
    generatorVersion: recipe.generatorVersion ?? GENERATOR_VERSION,
    seed: String(recipe.seed ?? '0'),
    extentMetres: [Number(recipe.extentMetres?.[0] ?? 256), Number(recipe.extentMetres?.[1] ?? 256)],
    originMetres: recipe.originMetres ? [...recipe.originMetres] : [
      -(Number(recipe.extentMetres?.[0] ?? 256)) / 2, 0, -(Number(recipe.extentMetres?.[1] ?? 256)) / 2,
    ],
    chunkSizeMetres: Number(recipe.chunkSizeMetres ?? 128),
    terrain: {
      sampleStepMetres: Number(recipe.terrain?.sampleStepMetres ?? 1),
      preset: recipe.terrain?.preset ?? 'mountain-valley',
      amplitudeMetres: Number(recipe.terrain?.amplitudeMetres ?? 90),
      baseHeightMetres: Number(recipe.terrain?.baseHeightMetres ?? 0),
      ridgeStrength: Number(recipe.terrain?.ridgeStrength ?? 0.55),
      erosionPasses: Number(recipe.terrain?.erosionPasses ?? 2),
    },
    libraries: [...(recipe.libraries ?? [])].sort((a, b) => String(a.id).localeCompare(String(b.id))),
    layers: [...(recipe.layers ?? [])].sort((a, b) => String(a.id).localeCompare(String(b.id))),
    overrides: [...(recipe.overrides ?? [])].sort((a, b) => String(a.id ?? a.region?.minX).localeCompare(String(b.id ?? b.region?.minX))),
    budgetProfile: recipe.budgetProfile ?? 'desktop-balanced',
  };
  if (recipe.name !== undefined) normalized.name = recipe.name;
  if (recipe.description !== undefined) normalized.description = recipe.description;
  if (recipe.locks !== undefined) normalized.locks = [...recipe.locks];
  return normalized;
}

export function budgetFor(profile) {
  return BUDGETS[profile] ?? BUDGETS['desktop-balanced'];
}

export function recipeChunkRange(recipe) {
  const [width, depth] = recipe.extentMetres;
  const [originX, , originZ] = recipe.originMetres;
  return chunkRangeForBounds({
    minX: originX, minZ: originZ, maxX: originX + width, maxZ: originZ + depth,
  }, recipe.chunkSizeMetres);
}

export function makeRecipe(overrides = {}) {
  const extent = overrides.extentMetres ?? [256, 256];
  return normalizeRecipe({
    schema: RECIPE_SCHEMA,
    worldId: overrides.worldId ?? 'world',
    generatorVersion: overrides.generatorVersion ?? GENERATOR_VERSION,
    seed: overrides.seed ?? '1',
    extentMetres: extent,
    originMetres: overrides.originMetres ?? [-extent[0] / 2, 0, -extent[1] / 2],
    chunkSizeMetres: overrides.chunkSizeMetres ?? 128,
    terrain: overrides.terrain ?? { sampleStepMetres: 1, preset: 'mountain-valley' },
    libraries: overrides.libraries ?? [],
    layers: overrides.layers ?? [],
    overrides: overrides.overrides ?? [],
    budgetProfile: overrides.budgetProfile ?? 'desktop-balanced',
  });
}

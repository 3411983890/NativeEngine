import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';

// Water authoring contract. The four backends in the plan share one asset identity so a
// scene can swap a pond from analytic to shallow water without changing the reference.
// Only backends with a CPU authority reference are cookable today; `pbf` is rejected
// explicitly instead of cooking an asset the runtime cannot simulate.
export const WATER_SCHEMA = 'nativegpu.water/1';
export const COOKED_WATER_SCHEMA = 'nativegpu.water-cook/1';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;
const BACKENDS = new Set(['analytic', 'shallow', 'spectrum']);
const DECLARED_BACKENDS = new Set(['analytic', 'shallow', 'spectrum', 'pbf']);

export class WaterError extends Error {
  constructor(code, message) { super(message); this.name = 'WaterError'; this.code = code; }
}
function requireValue(ok, message, code = 'NG-E-WATER-ASSET') {
  if (!ok) throw new WaterError(code, message);
}
function object(value, label) {
  requireValue(value && typeof value === 'object' && !Array.isArray(value)
    && (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null), `${label} must be an object`);
}
function keys(value, required, optional, label) {
  object(value, label);
  for (const key of required) requireValue(Object.hasOwn(value, key), `${label}.${key} is required`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) requireValue(allowed.has(key), `${label}.${key} is unsupported`);
}
function integer(value, low, high, label) {
  requireValue(Number.isSafeInteger(value) && value >= low && value <= high, `${label} must be an integer in [${low},${high}]`);
}
function scalar(value, low, high, label) {
  requireValue(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high,
    `${label} must be finite and in [${low},${high}]`);
}
function vector(value, length, low, high, label) {
  requireValue(Array.isArray(value) && value.length === length, `${label} requires ${length} values`);
  for (let i = 0; i < length; ++i) requireValue(Object.hasOwn(value, i), `${label} contains a missing entry`);
  value.forEach((entry, i) => scalar(entry, low, high, `${label}[${i}]`));
}
function identifier(value, label) { requireValue(typeof value === 'string' && ID.test(value), `${label} is not a valid identifier`); }

function validateBathymetry(bathymetry) {
  keys(bathymetry, ['type'], ['heightMetres', 'slopeX', 'slopeZ', 'depthMetres', 'widthMetres', 'bankSlope', 'mounds'], 'bathymetry');
  if (bathymetry.type === 'flat') {
    keys(bathymetry, ['type', 'heightMetres'], [], 'bathymetry');
    scalar(bathymetry.heightMetres, -100, 100, 'bathymetry.heightMetres');
  } else if (bathymetry.type === 'slope') {
    keys(bathymetry, ['type', 'heightMetres', 'slopeX', 'slopeZ'], [], 'bathymetry');
    scalar(bathymetry.heightMetres, -100, 100, 'bathymetry.heightMetres');
    scalar(bathymetry.slopeX, -10, 10, 'bathymetry.slopeX'); scalar(bathymetry.slopeZ, -10, 10, 'bathymetry.slopeZ');
  } else if (bathymetry.type === 'basin') {
    keys(bathymetry, ['type', 'heightMetres', 'depthMetres'], [], 'bathymetry');
    scalar(bathymetry.heightMetres, -100, 100, 'bathymetry.heightMetres');
    scalar(bathymetry.depthMetres, 0.01, 100, 'bathymetry.depthMetres');
  } else if (bathymetry.type === 'channel') {
    keys(bathymetry, ['type', 'heightMetres', 'widthMetres', 'depthMetres'], ['bankSlope'], 'bathymetry');
    scalar(bathymetry.heightMetres, -100, 100, 'bathymetry.heightMetres');
    scalar(bathymetry.widthMetres, 0.1, 500, 'bathymetry.widthMetres');
    scalar(bathymetry.depthMetres, 0.01, 100, 'bathymetry.depthMetres');
    if (Object.hasOwn(bathymetry, 'bankSlope')) scalar(bathymetry.bankSlope, 0, 100, 'bathymetry.bankSlope');
  } else if (bathymetry.type === 'mounds') {
    keys(bathymetry, ['type', 'heightMetres', 'mounds'], [], 'bathymetry');
    scalar(bathymetry.heightMetres, -100, 100, 'bathymetry.heightMetres');
    requireValue(Array.isArray(bathymetry.mounds) && bathymetry.mounds.length >= 1 && bathymetry.mounds.length <= 64,
      'bathymetry.mounds must contain 1–64 entries');
    for (const mound of bathymetry.mounds) {
      keys(mound, ['x', 'z', 'radiusMetres', 'heightMetres'], [], 'bathymetry.mounds[]');
      scalar(mound.x, -1000, 1000, 'bathymetry.mounds[].x');
      scalar(mound.z, -1000, 1000, 'bathymetry.mounds[].z');
      scalar(mound.radiusMetres, 0.05, 100, 'bathymetry.mounds[].radiusMetres');
      scalar(mound.heightMetres, 0, 100, 'bathymetry.mounds[].heightMetres');
    }
  } else requireValue(false, 'bathymetry.type must be flat, slope, basin, channel or mounds');
}

function validateOpening(opening, label) {
  keys(opening, ['x', 'z', 'radiusMetres', 'flowCubicMetresPerSecond'], ['speedMetresPerSecond', 'direction'], label);
  scalar(opening.x, -1000, 1000, `${label}.x`);
  scalar(opening.z, -1000, 1000, `${label}.z`);
  scalar(opening.radiusMetres, 0.05, 100, `${label}.radiusMetres`);
  scalar(opening.flowCubicMetresPerSecond, 0, 10000, `${label}.flowCubicMetresPerSecond`);
  if (Object.hasOwn(opening, 'speedMetresPerSecond')) scalar(opening.speedMetresPerSecond, 0, 50, `${label}.speedMetresPerSecond`);
  if (Object.hasOwn(opening, 'direction')) {
    vector(opening.direction, 2, -1, 1, `${label}.direction`);
    requireValue(Math.hypot(...opening.direction) > 1e-3, `${label}.direction must not be zero`);
  }
}

function validateObstacle(obstacle, label) {
  keys(obstacle, ['x', 'z', 'radiusMetres', 'topHeightMetres'], [], label);
  scalar(obstacle.x, -1000, 1000, `${label}.x`);
  scalar(obstacle.z, -1000, 1000, `${label}.z`);
  scalar(obstacle.radiusMetres, 0.05, 100, `${label}.radiusMetres`);
  scalar(obstacle.topHeightMetres, -100, 200, `${label}.topHeightMetres`);
}

export function compileWater(asset) {
  keys(asset, ['schema', 'id', 'revision', 'seed', 'backend', 'extentMetres', 'cellMetres', 'stillHeightMetres',
    'bathymetry', 'boundaries', 'waves', 'material', 'budget'], ['sources', 'drains', 'obstacles'], 'water');
  requireValue(asset.schema === WATER_SCHEMA, 'unsupported water schema');
  identifier(asset.id, 'water.id');
  integer(asset.revision, 0, 0xFFFFFFFF, 'revision'); integer(asset.seed, 0, 0xFFFFFFFF, 'seed');
  requireValue(DECLARED_BACKENDS.has(asset.backend), 'water.backend is not declared');
  requireValue(BACKENDS.has(asset.backend),
    `water backend ${asset.backend} has no CPU authority reference yet`, 'NG-E-WATER-UNSUPPORTED');
  vector(asset.extentMetres, 2, 0.5, 500, 'extentMetres');
  scalar(asset.cellMetres, 0.01, 5, 'cellMetres');
  scalar(asset.stillHeightMetres, -50, 200, 'stillHeightMetres');
  validateBathymetry(asset.bathymetry);
  keys(asset.boundaries, ['type', 'inflowCubicMetresPerSecond', 'outflowCubicMetresPerSecond'], [], 'boundaries');
  requireValue(['closed', 'open'].includes(asset.boundaries.type), 'boundaries.type must be closed or open');
  scalar(asset.boundaries.inflowCubicMetresPerSecond, 0, 10000, 'boundaries.inflowCubicMetresPerSecond');
  scalar(asset.boundaries.outflowCubicMetresPerSecond, 0, 10000, 'boundaries.outflowCubicMetresPerSecond');
  keys(asset.waves, ['amplitudeMetres', 'wavelengthMetres', 'speedMetresPerSecond', 'direction', 'components'], [], 'waves');
  scalar(asset.waves.amplitudeMetres, 0, 5, 'waves.amplitudeMetres');
  scalar(asset.waves.wavelengthMetres, 0.1, 200, 'waves.wavelengthMetres');
  scalar(asset.waves.speedMetresPerSecond, 0, 50, 'waves.speedMetresPerSecond');
  vector(asset.waves.direction, 2, -1, 1, 'waves.direction');
  const directionLength = Math.hypot(...asset.waves.direction);
  requireValue(directionLength > 1e-3, 'waves.direction must not be zero');
  integer(asset.waves.components, 1, 16, 'waves.components');
  keys(asset.material, ['refractionIndex', 'absorptionPerMetre', 'foamThreshold'], [], 'material');
  scalar(asset.material.refractionIndex, 1, 3, 'material.refractionIndex');
  scalar(asset.material.absorptionPerMetre, 0, 100, 'material.absorptionPerMetre');
  scalar(asset.material.foamThreshold, 0, 10, 'material.foamThreshold');
  keys(asset.budget, ['maxCells', 'maxSubsteps'], [], 'budget');
  integer(asset.budget.maxCells, 16, 1048576, 'budget.maxCells');
  integer(asset.budget.maxSubsteps, 1, 32, 'budget.maxSubsteps');
  for (const name of ['sources', 'drains']) {
    if (!Object.hasOwn(asset, name)) continue;
    requireValue(Array.isArray(asset[name]) && asset[name].length <= 32, `${name} must be an array of at most 32 openings`);
    asset[name].forEach((opening, i) => validateOpening(opening, `${name}[${i}]`));
  }
  if (Object.hasOwn(asset, 'obstacles')) {
    requireValue(Array.isArray(asset.obstacles) && asset.obstacles.length <= 64, 'obstacles must be an array of at most 64 entries');
    asset.obstacles.forEach((obstacle, i) => validateObstacle(obstacle, `obstacles[${i}]`));
  }
  const columns = Math.floor(asset.extentMetres[0] / asset.cellMetres);
  const rows = Math.floor(asset.extentMetres[1] / asset.cellMetres);
  requireValue(columns >= 4 && rows >= 4, 'water extent must contain at least 4x4 cells');
  requireValue(columns * rows <= asset.budget.maxCells, 'water grid exceeds budget.maxCells');
  const source = {
    schema: WATER_SCHEMA, id: asset.id, revision: asset.revision, seed: asset.seed, backend: asset.backend,
    extentMetres: [...asset.extentMetres], cellMetres: asset.cellMetres, stillHeightMetres: asset.stillHeightMetres,
    bathymetry: structuredClone(asset.bathymetry), boundaries: structuredClone(asset.boundaries),
    waves: { ...structuredClone(asset.waves), direction: [...asset.waves.direction] },
    material: structuredClone(asset.material), budget: structuredClone(asset.budget),
    // Optional fields are omitted when absent so existing cooked identities do not move.
    ...(Object.hasOwn(asset, 'sources') ? { sources: structuredClone(asset.sources) } : {}),
    ...(Object.hasOwn(asset, 'drains') ? { drains: structuredClone(asset.drains) } : {}),
    ...(Object.hasOwn(asset, 'obstacles') ? { obstacles: structuredClone(asset.obstacles) } : {}),
    derived: {
      columns, rows, cellMetres: asset.cellMetres,
      extentMetres: [columns * asset.cellMetres, rows * asset.cellMetres], totalCells: columns * rows,
    },
  };
  const payload = { schema: COOKED_WATER_SCHEMA, source };
  return {
    source, payload, hash: sha256Hex(canonicalJson(payload)),
    columns, rows, cells: columns * rows,
    poolBytes: columns * rows * 5 * 4,
    implemented: true, fft: false,
  };
}

export function compileWaterLibrary(assets) {
  requireValue(Array.isArray(assets) && assets.length >= 1 && assets.length <= 256, 'water library needs 1–256 assets');
  const compiled = assets.map(compileWater), ids = new Set();
  for (const water of compiled) {
    requireValue(!ids.has(water.source.id), `duplicate water id ${water.source.id}`); ids.add(water.source.id);
  }
  const bodies = compiled.sort((a, b) => a.source.id.localeCompare(b.source.id));
  return { bodies, hash: sha256Hex(canonicalJson(bodies.map((body) => [body.source.id, body.hash]))) };
}

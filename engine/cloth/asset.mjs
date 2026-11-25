import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { bakeTriangleTopology } from './topology.mjs';

// Cloth authoring contract. The source file declares intent (a grid, a material, pins,
// colliders). Everything the solver needs — rest positions, triangles, per-vertex mass
// and the three constraint families — is baked here so the runtime never invents
// topology or measures geometry at load time.
export const CLOTH_SCHEMA = 'nativegpu.cloth/2';
export const COOKED_CLOTH_SCHEMA = 'nativegpu.cloth-cook/2';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;
const KINDS = new Set(['environment', 'skinned']);

export class ClothError extends Error {
  constructor(code, message) { super(message); this.name = 'ClothError'; this.code = code; }
}
function requireValue(ok, message, code = 'NG-E-CLOTH-ASSET') {
  if (!ok) throw new ClothError(code, message);
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
function array(value, max, label, min = 0) {
  requireValue(Array.isArray(value) && value.length >= min && value.length <= max, `${label} must have ${min}–${max} entries`);
  for (let i = 0; i < value.length; ++i) requireValue(Object.hasOwn(value, i), `${label} contains a missing entry`);
}
function identifier(value, label) { requireValue(typeof value === 'string' && ID.test(value), `${label} is not a valid identifier`); }

function validateMesh(mesh) {
  keys(mesh, ['type', 'columns', 'rows', 'widthMetres', 'heightMetres'], [], 'mesh');
  requireValue(mesh.type === 'grid', 'mesh.type must be grid; arbitrary simulation meshes need an importer');
  integer(mesh.columns, 2, 128, 'mesh.columns'); integer(mesh.rows, 2, 128, 'mesh.rows');
  scalar(mesh.widthMetres, 0.02, 100, 'mesh.widthMetres'); scalar(mesh.heightMetres, 0.02, 100, 'mesh.heightMetres');
}
function validateMaterial(material,legacy) {
  keys(material, ['arealDensityKgM2', 'thicknessMetres', 'stretchCompliance', 'shearCompliance',
    legacy?'bendCompliance':'bendAngleCompliance', 'damping', 'dragCoefficient'], [], 'material');
  scalar(material.arealDensityKgM2, 0.001, 50, 'material.arealDensityKgM2');
  scalar(material.thicknessMetres, 0.0001, 0.2, 'material.thicknessMetres');
  for (const key of ['stretchCompliance', 'shearCompliance', ...(legacy?['bendCompliance']:[])])
    scalar(material[key], 0, 1, `material.${key}`);
  if(!legacy)scalar(material.bendAngleCompliance,0,1e8,'material.bendAngleCompliance');
  scalar(material.damping, 0, 50, 'material.damping');
  scalar(material.dragCoefficient, 0, 10, 'material.dragCoefficient');
}
function validatePins(pins, mesh) {
  array(pins, 4096, 'pins');
  const seen = new Set();
  for (const pin of pins) {
    keys(pin, ['column', 'row'], [], 'pin');
    integer(pin.column, 0, mesh.columns - 1, 'pin.column'); integer(pin.row, 0, mesh.rows - 1, 'pin.row');
    const key = `${pin.column},${pin.row}`;
    requireValue(!seen.has(key), `duplicate pin ${key}`); seen.add(key);
  }
  return seen;
}
function validateCollision(collision) {
  keys(collision, ['thicknessMetres', 'plane', 'spheres', 'capsules'], [], 'collision');
  scalar(collision.thicknessMetres, 0, 0.5, 'collision.thicknessMetres');
  if (collision.plane !== null) {
    keys(collision.plane, ['normal', 'offsetMetres'], [], 'collision.plane');
    vector(collision.plane.normal, 3, -1, 1, 'collision.plane.normal');
    const length = Math.hypot(...collision.plane.normal);
    requireValue(Math.abs(length - 1) <= 1e-3, 'collision.plane.normal must be unit length');
    scalar(collision.plane.offsetMetres, -1000, 1000, 'collision.plane.offsetMetres');
  }
  array(collision.spheres, 64, 'collision.spheres');
  for (const sphere of collision.spheres) {
    keys(sphere, ['centre', 'radiusMetres'], [], 'collision.sphere');
    vector(sphere.centre, 3, -10000, 10000, 'collision.sphere.centre');
    scalar(sphere.radiusMetres, 0.001, 1000, 'collision.sphere.radiusMetres');
  }
  array(collision.capsules, 64, 'collision.capsules');
  for (const capsule of collision.capsules) {
    keys(capsule, ['a', 'b', 'radiusMetres'], [], 'collision.capsule');
    vector(capsule.a, 3, -10000, 10000, 'collision.capsule.a');
    vector(capsule.b, 3, -10000, 10000, 'collision.capsule.b');
    scalar(capsule.radiusMetres, 0.001, 1000, 'collision.capsule.radiusMetres');
    requireValue(capsule.a.some((value, i) => value !== capsule.b[i]), 'collision.capsule needs two distinct endpoints');
  }
}
function validateLod(lod, mesh) {
  array(lod, 8, 'lod', 1);
  let previousDistance = 0, previousVertices = Infinity;
  lod.forEach((entry, index) => {
    keys(entry, ['maxDistanceMetres', 'columns', 'rows'], [], `lod[${index}]`);
    scalar(entry.maxDistanceMetres, 0.1, 10000, `lod[${index}].maxDistanceMetres`);
    integer(entry.columns, 2, mesh.columns, `lod[${index}].columns`);
    integer(entry.rows, 2, mesh.rows, `lod[${index}].rows`);
    requireValue(entry.maxDistanceMetres > previousDistance, 'lod distances must strictly increase');
    requireValue(entry.columns * entry.rows < previousVertices, 'lod resolutions must strictly decrease');
    previousDistance = entry.maxDistanceMetres; previousVertices = entry.columns * entry.rows;
    if (index === 0) requireValue(entry.columns === mesh.columns && entry.rows === mesh.rows,
      'lod[0] must describe the simulation mesh');
  });
}

// Bakes the simulation mesh. Positions put row 0 at the bottom edge and column 0 at the
// left edge, with the sheet in the XY plane and +Z as the front face, so a pin at
// (0, rows-1) is the top-left corner for a hanging flag or curtain.
export function bakeClothGrid(mesh, material, pins) {
  const { columns, rows, widthMetres, heightMetres } = mesh;
  const dx = widthMetres / (columns - 1), dy = heightMetres / (rows - 1);
  const positions = [], masses = [];
  for (let row = 0; row < rows; ++row)
    for (let column = 0; column < columns; ++column)
      positions.push([-widthMetres / 2 + column * dx, row * dy, 0]);
  const index = (column, row) => row * columns + column;
  const triangles = [], areas = new Array(positions.length).fill(0);
  const triangleArea = (a, b, c) => {
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    return Math.hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]) / 2;
  };
  for (let row = 0; row + 1 < rows; ++row) for (let column = 0; column + 1 < columns; ++column) {
    const a = index(column, row), b = index(column + 1, row), c = index(column + 1, row + 1), d = index(column, row + 1);
    for (const triangle of [[a, b, c], [a, c, d]]) {
      triangles.push(triangle);
      const area = triangleArea(positions[triangle[0]], positions[triangle[1]], positions[triangle[2]]);
      for (const vertex of triangle) areas[vertex] += area / 3;
    }
  }
  const totalArea = areas.reduce((sum, area) => sum + area, 0);
  requireValue(totalArea > 1e-6, 'baked cloth has zero surface area');
  for (const area of areas) masses.push(Math.max(1e-6, area * material.arealDensityKgM2));
  const stretch = [], shear = [], bend = [];
  for (let row = 0; row < rows; ++row) for (let column = 0; column < columns; ++column) {
    if (column + 1 < columns) stretch.push([index(column, row), index(column + 1, row), dx]);
    if (row + 1 < rows) stretch.push([index(column, row), index(column, row + 1), dy]);
    if (column + 1 < columns && row + 1 < rows) {
      const diagonal = Math.hypot(dx, dy);
      shear.push([index(column, row), index(column + 1, row + 1), diagonal]);
      shear.push([index(column + 1, row), index(column, row + 1), diagonal]);
    }
    if (column + 2 < columns) bend.push([index(column, row), index(column + 2, row), 2 * dx]);
    if (row + 2 < rows) bend.push([index(column, row), index(column, row + 2), 2 * dy]);
  }
  const pinned = [...pins].map((pin) => ({ vertex: index(pin.column, pin.row), anchor: positions[index(pin.column, pin.row)] }));
  return { positions, triangles, masses, stretch, shear, bend, pins: pinned, totalArea };
}

export function compileCloth(asset) {
  keys(asset, ['schema', 'id', 'revision', 'seed', 'kind', 'mesh', 'material', 'pins', 'collision', 'lod', 'budget'], [], 'cloth');
  const legacy=asset.schema==='nativegpu.cloth/1';
  requireValue(asset.schema === CLOTH_SCHEMA || legacy, 'unsupported cloth schema');
  identifier(asset.id, 'cloth.id');
  integer(asset.revision, 0, 0xFFFFFFFF, 'revision'); integer(asset.seed, 0, 0xFFFFFFFF, 'seed');
  requireValue(KINDS.has(asset.kind), 'cloth.kind must be environment or skinned');
  validateMesh(asset.mesh); validateMaterial(asset.material,legacy);
  const pinSet = validatePins(asset.pins, asset.mesh);
  validateCollision(asset.collision); validateLod(asset.lod, asset.mesh);
  keys(asset.budget, ['maxVertices', 'maxSubsteps', 'iterations'], [], 'budget');
  integer(asset.budget.maxVertices, 4, 65536, 'budget.maxVertices');
  integer(asset.budget.maxSubsteps, 1, 16, 'budget.maxSubsteps'); integer(asset.budget.iterations, 1, 64, 'budget.iterations');
  const vertices = asset.mesh.columns * asset.mesh.rows;
  requireValue(vertices <= asset.budget.maxVertices, 'cloth exceeds budget.maxVertices');
  const baked = bakeClothGrid(asset.mesh, asset.material, asset.pins);
  const source = {
    schema: asset.schema, id: asset.id, revision: asset.revision, seed: asset.seed, kind: asset.kind,
    mesh: structuredClone(asset.mesh), material: structuredClone(asset.material),
    pins: structuredClone(asset.pins), collision: structuredClone(asset.collision),
    lod: structuredClone(asset.lod), budget: structuredClone(asset.budget),
    baked: {
      positions: baked.positions, triangles: baked.triangles, masses: baked.masses,
      stretch: baked.stretch, shear: baked.shear, bend: baked.bend, pins: baked.pins,
      totalAreaMetres2: baked.totalArea,
    },
  };
  if(!legacy)source.baked.topology=bakeTriangleTopology(baked.positions,baked.triangles);
  const payload = { schema: legacy?'nativegpu.cloth-cook/1':COOKED_CLOTH_SCHEMA, source };
  return {
    source, payload, hash: sha256Hex(canonicalJson(payload)),
    vertexCount: vertices, triangleCount: baked.triangles.length,
    constraintCount: legacy?baked.stretch.length + baked.shear.length + baked.bend.length:baked.triangles.length*3+source.baked.topology.hinges.length,
    pinCount: pinSet.size,
    poolBytes: vertices * (3 * 3 * 4 + 4) + baked.triangles.length * 12 +
      (baked.stretch.length + baked.shear.length + baked.bend.length) * 12 + (legacy?0:
        vertices*36 + baked.triangles.length*48 + source.baked.topology.hinges.length*28 + baked.pins.length*16 + 4),
    poolByteAccounting:'flat CPU array estimate; GPU committed allocations are measured separately',
  };
}

// The angular parameter has different units. Migration requires an explicit
// choice, rather than silently reinterpreting a distance compliance as torque.
export function migrateCloth(asset,bendAngleCompliance){
  requireValue(asset.schema==='nativegpu.cloth/1','migration requires cloth /1');compileCloth(asset);
  scalar(bendAngleCompliance,0,1e8,'bendAngleCompliance');const result=structuredClone(asset);result.schema=CLOTH_SCHEMA;
  delete result.material.bendCompliance;result.material.bendAngleCompliance=bendAngleCompliance;return result;
}

export function compileClothLibrary(assets) {
  requireValue(Array.isArray(assets) && assets.length >= 1 && assets.length <= 256, 'cloth library needs 1–256 assets');
  const compiled = assets.map(compileCloth), ids = new Set();
  for (const cloth of compiled) {
    requireValue(!ids.has(cloth.source.id), `duplicate cloth id ${cloth.source.id}`); ids.add(cloth.source.id);
  }
  const cloths = compiled.sort((a, b) => a.source.id.localeCompare(b.source.id));
  return { cloths, hash: sha256Hex(canonicalJson(cloths.map((cloth) => [cloth.source.id, cloth.hash]))) };
}

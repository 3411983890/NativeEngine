import { Stream, deriveSeed, mix32 } from './random.mjs';
import { fractalNoise } from './surface.mjs';

// Deterministic placement.
//
// The invariance the plan asks for is "局部重建不能改变邻区". The way to get it is
// structural, not a later diff: every scatter cell is generated from its own integer
// seed and the surface only. Nothing reads a neighbour's output, so rebuilding one
// cell cannot shift another, and the stable identity of a candidate is a pure function of
// (layer, species, cell, slot) — assigned *before* any surface, slope, mask or spacing
// filter runs, which is what makes an id survive a density edit.
//
// Cross-cell and cross-species spacing is the one thing a per-cell generator cannot
// guarantee by construction, so it is a separate region-level pass (`spacingScope:
// 'region'`). That pass is still local: a candidate is accepted or rejected purely by
// higher-priority candidates within its own spacing radius, so re-running it over an
// affected chunk plus a halo of `maxSpacing` reproduces the global result exactly. The
// default stays per-cell, so existing regions cook byte-identically.

export const SCATTER_SCHEMA = 'nativegpu.vegetation-scatter/1';
const DEGREES = Math.PI / 180;

export class ScatterError extends Error {
  constructor(code, message) { super(message); this.name = 'ScatterError'; this.code = code; }
}
const fail = (ok, code, message) => { if (!ok) throw new ScatterError(code, message); };

function num(value, low, high, label) {
  fail(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high,
    'NG-E-VEG-SCATTER', `${label} must be finite and in [${low},${high}]`);
  return value;
}

// The identity of one placement candidate. It is a string because the alternative — "index in
// the filtered array" — renumbers every plant when one candidate is rejected, which is exactly
// what makes an erase or a move in the editor land on the wrong tree.
export function candidateKey({ layerId = 'layer0', species, cellX, cellZ, slot }) {
  return `${layerId}/${species}/${cellX}/${cellZ}/${slot}`;
}

// ---------------------------------------------------------------- coverage masks
// A coverage mask is the brush's persistent result: a grid of 0..1 values that a stroke writes
// into. It is sampled bilinearly so a mask edge is a gradient rather than a staircase, and it is
// a value object (not a callback) so the same numbers can be cooked, hashed and replayed.
export class CoverageMask {
  constructor({ width, height, cellSizeMetres, originX = 0, originZ = 0, values, version = 'mask/1', id = 'mask' }) {
    fail(Number.isInteger(width) && width > 0 && Number.isInteger(height) && height > 0,
      'NG-E-VEG-SCATTER', 'coverage mask needs a positive integer width and height');
    fail(values && values.length === width * height, 'NG-E-VEG-SCATTER', 'coverage mask values do not match width*height');
    fail(typeof cellSizeMetres === 'number' && cellSizeMetres > 0, 'NG-E-VEG-SCATTER', 'coverage mask needs a positive cellSizeMetres');
    this.id = id; this.width = width; this.height = height;
    this.cellSizeMetres = cellSizeMetres; this.originX = originX; this.originZ = originZ;
    this.values = values instanceof Float32Array ? values : Float32Array.from(values);
    this.version = version;
  }
  // Bilinear, clamped at the border. Outside the mask is *not* an error: a brush that stops
  // short of a region edge should fade out, not invalidate the cook.
  sample(x, z) {
    const u = (x - this.originX) / this.cellSizeMetres - 0.5;
    const v = (z - this.originZ) / this.cellSizeMetres - 0.5;
    const x0 = Math.floor(u), z0 = Math.floor(v);
    const fx = u - x0, fz = v - z0;
    const at = (ix, iz) => {
      const cx = Math.min(this.width - 1, Math.max(0, ix));
      const cz = Math.min(this.height - 1, Math.max(0, iz));
      return this.values[cz * this.width + cx];
    };
    const a = at(x0, z0), b = at(x0 + 1, z0), c = at(x0, z0 + 1), d = at(x0 + 1, z0 + 1);
    return (a * (1 - fx) + b * fx) * (1 - fz) + (c * (1 - fx) + d * fx) * fz;
  }
  // A brush stroke writes a soft disc. Kept here (not in the editor) so the tool and the tests
  // apply the same falloff the mask will be sampled with.
  stampCircle({ x, z, radiusMetres, strength = 1, falloffMetres = 0 }) {
    const radius = Math.max(this.cellSizeMetres, radiusMetres);
    const inner = Math.max(0, radius - Math.max(0, falloffMetres));
    const x0 = Math.max(0, Math.floor((x - radius - this.originX) / this.cellSizeMetres));
    const x1 = Math.min(this.width - 1, Math.ceil((x + radius - this.originX) / this.cellSizeMetres));
    const z0 = Math.max(0, Math.floor((z - radius - this.originZ) / this.cellSizeMetres));
    const z1 = Math.min(this.height - 1, Math.ceil((z + radius - this.originZ) / this.cellSizeMetres));
    for (let iz = z0; iz <= z1; ++iz) for (let ix = x0; ix <= x1; ++ix) {
      const px = this.originX + (ix + 0.5) * this.cellSizeMetres;
      const pz = this.originZ + (iz + 0.5) * this.cellSizeMetres;
      const distance = Math.hypot(px - x, pz - z);
      if (distance > radius) continue;
      const t = distance <= inner ? 1 : 1 - (distance - inner) / Math.max(1e-6, radius - inner);
      const weight = t * t * (3 - 2 * t) * strength;
      const index = iz * this.width + ix;
      this.values[index] = Math.max(this.values[index], Math.min(1, weight));
    }
    return this;
  }
}

// ---------------------------------------------------------------- exclusions
// Paths, buildings and interaction zones. A shape is expanded by its margin, so a road keeps a
// verge and a house keeps a doorstep. The list is normalized once per region, not per candidate.
const EXCLUSION_KINDS = new Set(['circle', 'rect', 'polyline']);

export function normalizeExclusions(list) {
  if (!list) return { shapes: [], bounds: null };
  fail(Array.isArray(list), 'NG-E-VEG-SCATTER', 'exclusions must be an array');
  const shapes = list.map((shape, index) => {
    fail(shape && EXCLUSION_KINDS.has(shape.kind), 'NG-E-VEG-SCATTER', `exclusions[${index}] has an unsupported kind`);
    const margin = num(shape.marginMetres ?? 0, 0, 500, `exclusions[${index}].marginMetres`);
    if (shape.kind === 'circle') {
      const radius = num(shape.radiusMetres, 0, 1e5, `exclusions[${index}].radiusMetres`);
      return { kind: 'circle', x: num(shape.x, -1e6, 1e6, `exclusions[${index}].x`), z: num(shape.z, -1e6, 1e6, `exclusions[${index}].z`), radius: radius + margin, margin };
    }
    if (shape.kind === 'rect') {
      for (const key of ['minX', 'minZ', 'maxX', 'maxZ']) num(shape[key], -1e6, 1e6, `exclusions[${index}].${key}`);
      fail(shape.maxX >= shape.minX && shape.maxZ >= shape.minZ, 'NG-E-VEG-SCATTER', `exclusions[${index}] rect is inverted`);
      return { kind: 'rect', minX: shape.minX - margin, minZ: shape.minZ - margin, maxX: shape.maxX + margin, maxZ: shape.maxZ + margin, margin };
    }
    fail(Array.isArray(shape.points) && shape.points.length >= 2, 'NG-E-VEG-SCATTER', `exclusions[${index}].points needs at least two points`);
    const points = shape.points.map((point, pointIndex) => {
      fail(Array.isArray(point) && point.length === 2, 'NG-E-VEG-SCATTER', `exclusions[${index}].points[${pointIndex}] must be [x,z]`);
      return [num(point[0], -1e6, 1e6, `exclusions[${index}].points[${pointIndex}][0]`), num(point[1], -1e6, 1e6, `exclusions[${index}].points[${pointIndex}][1]`)];
    });
    const half = num(shape.widthMetres ?? 0, 0, 1e5, `exclusions[${index}].widthMetres`) * 0.5 + margin;
    let minX = Infinity, minZ = Infinity, maxX = -Infinity, maxZ = -Infinity;
    for (const point of points) {
      minX = Math.min(minX, point[0]); maxX = Math.max(maxX, point[0]);
      minZ = Math.min(minZ, point[1]); maxZ = Math.max(maxZ, point[1]);
    }
    return { kind: 'polyline', points, half, margin, minX: minX - half, minZ: minZ - half, maxX: maxX + half, maxZ: maxZ + half };
  });
  let bounds = null;
  for (const shape of shapes) {
    const box = shape.kind === 'circle'
      ? { minX: shape.x - shape.radius, minZ: shape.z - shape.radius, maxX: shape.x + shape.radius, maxZ: shape.z + shape.radius }
      : shape;
    bounds = bounds === null ? { ...box } : {
      minX: Math.min(bounds.minX, box.minX), minZ: Math.min(bounds.minZ, box.minZ),
      maxX: Math.max(bounds.maxX, box.maxX), maxZ: Math.max(bounds.maxZ, box.maxZ),
    };
  }
  return { shapes, bounds };
}

function distanceToSegment(px, pz, ax, az, bx, bz) {
  const dx = bx - ax, dz = bz - az;
  const lengthSquared = dx * dx + dz * dz;
  const t = lengthSquared < 1e-12 ? 0 : Math.max(0, Math.min(1, ((px - ax) * dx + (pz - az) * dz) / lengthSquared));
  return Math.hypot(px - (ax + dx * t), pz - (az + dz * t));
}

function excludedBy(shapes, x, z) {
  for (const shape of shapes) {
    if (shape.kind === 'circle') {
      if (Math.hypot(x - shape.x, z - shape.z) <= shape.radius) return true;
    } else if (shape.kind === 'rect') {
      if (x >= shape.minX && x <= shape.maxX && z >= shape.minZ && z <= shape.maxZ) return true;
    } else {
      if (x < shape.minX || x > shape.maxX || z < shape.minZ || z > shape.maxZ) continue;
      for (let index = 1; index < shape.points.length; ++index) {
        const a = shape.points[index - 1], b = shape.points[index];
        if (distanceToSegment(x, z, a[0], a[1], b[0], b[1]) <= shape.half) return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------- rule validation
export function validateScatterRule(rule, index, speciesIds) {
  fail(rule && typeof rule === 'object' && !Array.isArray(rule), 'NG-E-VEG-SCATTER', `species[${index}] must be an object`);
  fail(speciesIds.has(rule.species), 'NG-E-VEG-SCATTER', `species[${index}] references unknown species ${rule.species}`);
  num(rule.densityPerHectare, 0, 200000, `species[${index}].densityPerHectare`);
  num(rule.minSpacingMetres ?? 0, 0, 100, `species[${index}].minSpacingMetres`);
  const cluster = rule.cluster ?? { scaleMetres: 0, strength: 0, threshold: 0 };
  num(cluster.scaleMetres ?? 0, 0, 5000, `species[${index}].cluster.scaleMetres`);
  num(cluster.strength ?? 0, 0, 1, `species[${index}].cluster.strength`);
  num(cluster.threshold ?? 0, 0, 1, `species[${index}].cluster.threshold`);
  const slope = rule.slope ?? { maxDegrees: 90 };
  num(slope.maxDegrees ?? 90, 0, 90, `species[${index}].slope.maxDegrees`);
  const height = rule.height ?? { minMetres: -1e5, maxMetres: 1e5 };
  num(height.minMetres ?? -1e5, -1e5, 1e5, `species[${index}].height.minMetres`);
  num(height.maxMetres ?? -1e5, -1e5, 1e5, `species[${index}].height.maxMetres`);
  fail((height.minMetres ?? -1e5) <= (height.maxMetres ?? 1e5), 'NG-E-VEG-SCATTER', `species[${index}] height range is inverted`);
  const moisture = rule.moisture ?? { min: 0, max: 1 };
  num(moisture.min ?? 0, 0, 1, `species[${index}].moisture.min`);
  num(moisture.max ?? 1, 0, 1, `species[${index}].moisture.max`);
  const range = rule.scaleRange ?? [1, 1];
  fail(Array.isArray(range) && range.length === 2, 'NG-E-VEG-SCATTER', `species[${index}].scaleRange needs two values`);
  num(range[0], 0.05, 16, `species[${index}].scaleRange[0]`);
  num(range[1], 0.05, 16, `species[${index}].scaleRange[1]`);
  fail(range[0] <= range[1], 'NG-E-VEG-SCATTER', `species[${index}].scaleRange is inverted`);
  // A coverage mask multiplies the rule's density. `strength` says how much of the mask the rule
  // obeys; 1 means "no mask value, no plant", 0 means "the mask is ignored" — which is the
  // honest way to express a layer that should not be trimmed by the brush.
  let mask = null;
  if (rule.mask !== undefined && rule.mask !== null) {
    fail(typeof rule.mask === 'object' && typeof rule.mask.id === 'string', 'NG-E-VEG-SCATTER', `species[${index}].mask needs an id`);
    mask = {
      id: rule.mask.id,
      strength: num(rule.mask.strength ?? 1, 0, 1, `species[${index}].mask.strength`),
      invert: rule.mask.invert === true,
    };
  }
  // Row planting. A field is not a random scatter: it has a heading, a row spacing, a plant
  // spacing and gaps where a plant failed. When present, spacing drives the count and
  // `densityPerHectare` becomes the reported result rather than the input.
  let rows = null;
  if (rule.rows !== undefined && rule.rows !== null) {
    fail(typeof rule.rows === 'object', 'NG-E-VEG-SCATTER', `species[${index}].rows must be an object`);
    rows = {
      spacingMetres: num(rule.rows.spacingMetres, 0.05, 50, `species[${index}].rows.spacingMetres`),
      plantSpacingMetres: num(rule.rows.plantSpacingMetres, 0.02, 50, `species[${index}].rows.plantSpacingMetres`),
      headingRadians: num(rule.rows.headingRadians ?? 0, -Math.PI, Math.PI, `species[${index}].rows.headingRadians`),
      skipFraction: num(rule.rows.skipFraction ?? 0, 0, 0.9, `species[${index}].rows.skipFraction`),
      // 田埂退让: a margin kept clear on both sides of every row, expressed as a fraction of the
      // row spacing. 0 means plants may use the whole strip.
      ridgeInsetFraction: num(rule.rows.ridgeInsetFraction ?? 0, 0, 0.45, `species[${index}].rows.ridgeInsetFraction`),
    };
  }
  const spacingScope = rule.spacingScope ?? 'cell';
  fail(spacingScope === 'cell' || spacingScope === 'region', 'NG-E-VEG-SCATTER', `species[${index}].spacingScope must be cell or region`);
  return {
    species: rule.species, densityPerHectare: rule.densityPerHectare,
    minSpacingMetres: rule.minSpacingMetres ?? 0,
    cluster: { scaleMetres: cluster.scaleMetres ?? 0, strength: cluster.strength ?? 0, threshold: cluster.threshold ?? 0 },
    slopeMaxRadians: (slope.maxDegrees ?? 90) * DEGREES,
    heightMin: height.minMetres ?? -1e5, heightMax: height.maxMetres ?? 1e5,
    moistureMin: moisture.min ?? 0, moistureMax: moisture.max ?? 1,
    scaleRange: [range[0], range[1]],
    lodBias: Math.max(0, Math.min(3, rule.lodBias ?? 0)),
    palette: Math.max(0, Math.min(4095, rule.palette ?? 0)),
    mask, rows, spacingScope,
    avoidExclusions: rule.avoidExclusions !== false,
    // Rule order is the priority for the region-level spacing pass: a forest's trees are placed
    // before its understory, so the trees win the overlap.
    priority: index,
  };
}

// Cluster acceptance is a smooth field of the world position, not of the cell. That
// is what keeps a forest edge continuous across a cell boundary.
function clusterAcceptance(rule, x, z, seed) {
  if (rule.cluster.strength <= 0 || rule.cluster.scaleMetres <= 0) return 1;
  const value = fractalNoise(x / rule.cluster.scaleMetres, z / rule.cluster.scaleMetres, seed, 3);
  const t = Math.max(0, Math.min(1, (value - rule.cluster.threshold) / Math.max(1e-3, 1 - rule.cluster.threshold)));
  const smooth = t * t * (3 - 2 * t);
  return 1 - rule.cluster.strength + rule.cluster.strength * smooth;
}

// The row lattice is anchored to the region origin and phased by the region seed, never to the
// cell. That is what makes a field continuous across a cell boundary: two adjacent cells compute
// the same global lattice and each keeps only the plants that fall inside itself.
function rowLattice(rule, region, cellMinX, cellMinZ, cellSize) {
  const { spacingMetres, plantSpacingMetres, headingRadians, skipFraction, ridgeInsetFraction } = rule.rows;
  const cos = Math.cos(headingRadians), sin = Math.sin(headingRadians);
  const phaseSeed = deriveSeed(region.seed, `rows/${rule.species}`);
  const rowPhase = ((phaseSeed & 0xFFFF) / 65536) * spacingMetres;
  const plantPhase = (((phaseSeed >>> 16) & 0xFFFF) / 65536) * plantSpacingMetres;
  const originX = region.bounds.minX, originZ = region.bounds.minZ;
  // Bounding box of the cell in row coordinates (along = u, across = n).
  let minAlong = Infinity, maxAlong = -Infinity, minAcross = Infinity, maxAcross = -Infinity;
  for (const corner of [[0, 0], [cellSize, 0], [0, cellSize], [cellSize, cellSize]]) {
    const dx = cellMinX + corner[0] - originX, dz = cellMinZ + corner[1] - originZ;
    const along = dx * cos + dz * sin;
    const across = -dx * sin + dz * cos;
    minAlong = Math.min(minAlong, along); maxAlong = Math.max(maxAlong, along);
    minAcross = Math.min(minAcross, across); maxAcross = Math.max(maxAcross, across);
  }
  const candidates = [];
  const firstRow = Math.floor((minAcross - rowPhase) / spacingMetres) - 1;
  const lastRow = Math.ceil((maxAcross - rowPhase) / spacingMetres) + 1;
  const firstPlant = Math.floor((minAlong - plantPhase) / plantSpacingMetres) - 1;
  const lastPlant = Math.ceil((maxAlong - plantPhase) / plantSpacingMetres) + 1;
  // A cell that asks for more than this is a spacing mistake (0.02 m rows over 8 m), not a field.
  const budget = 65536;
  if ((lastRow - firstRow + 1) * (lastPlant - firstPlant + 1) > budget) {
    fail(false, 'NG-E-VEG-CAPACITY', `row planting in one cell needs more than ${budget} candidates`);
  }
  for (let row = firstRow; row <= lastRow; ++row) {
    // 田埂退让 narrows the plantable strip around each row line; the remaining width is where a
    // crop may actually stand. Plants are jittered inside that strip and never outside it, so the
    // verge stays a verge instead of being trimmed afterwards.
    const usableHalf = spacingMetres * Math.max(0, 0.5 - ridgeInsetFraction);
    const rowCentre = row * spacingMetres + rowPhase;
    for (let plant = firstPlant; plant <= lastPlant; ++plant) {
      const along = plant * plantSpacingMetres + plantPhase;
      const lateralHash = mix32((mix32(row * 83492791) ^ mix32(plant * 2654435761) ^ region.seed) >>> 0);
      // A row is a line. The lateral wobble is a fifth of the usable half-width, so the row reads
      // as a row from a distance and still is not a printed grid up close.
      const lateral = usableHalf * 0.2 * (((lateralHash >>> 8) / 16777216) * 2 - 1);
      const across = rowCentre + lateral;
      const x = originX + cos * along - sin * across;
      const z = originZ + sin * along + cos * across;
      if (x < cellMinX || x >= cellMinX + cellSize || z < cellMinZ || z >= cellMinZ + cellSize) continue;
      // 缺株: a deterministic hole, not a random one, so re-cooking the same field keeps the
      // same gaps and a later "fill the gaps" edit can address them by id.
      if (skipFraction > 0) {
        const unit = (mix32((mix32(row * 73856093) ^ mix32(plant * 19349663) ^ region.seed) >>> 0) >>> 8) / 16777216;
        if (unit < skipFraction) continue;
      }
      candidates.push({ x, z, slot: row * 100003 + plant });
    }
  }
  return candidates;
}

// One cell. Reads its own integer seed, its own rules and the surface; never a neighbour.
export function scatterCell({ region, rules, surface, cellX, cellZ, masks = null, exclusions = null }) {
  const cellSize = region.cellSizeMetres;
  const minX = region.bounds.minX + cellX * cellSize;
  const minZ = region.bounds.minZ + cellZ * cellSize;
  const areaHectares = (cellSize * cellSize) / 10000;
  const cellSeed = mix32((region.seed ^ Math.imul(cellX, 73856093) ^ Math.imul(cellZ, 19349663)) >>> 0);
  const shapes = exclusions?.shapes ?? [];
  const stats = { rejectedByMask: 0, rejectedByExclusion: 0 };
  const instances = [];
  const maskFor = (id) => (masks instanceof Map ? masks.get(id)
    : Array.isArray(masks) ? masks.find((entry) => entry.id === id)
      : masks?.[id]);
  rules.forEach((rule, ruleIndex) => {
    const rng = new Stream(deriveSeed(cellSeed, `rule/${ruleIndex}/${rule.species}`), 'scatter');
    const mask = rule.mask ? maskFor(rule.mask.id) : null;
    if (rule.mask && !mask) {
      // A mask that is named but not supplied is a broken layer, not an empty one. Cooking it as
      // "no mask" would silently plant the whole area the brush was supposed to trim.
      fail(false, 'NG-E-VEG-SCATTER', `rule ${rule.species} references mask ${rule.mask.id}, which was not supplied`);
    }
    // Row planting and density scatter are two different generators. A field that is asked for
    // rows must not also be sprinkled, so the row lattice replaces the grid entirely.
    let slots;
    if (rule.rows) {
      slots = rowLattice(rule, region, minX, minZ, cellSize);
    } else {
      const expected = rule.densityPerHectare * areaHectares;
      if (expected <= 0) return;
      const whole = Math.floor(expected);
      let count = whole + (rng.unit() < expected - whole ? 1 : 0);
      if (count === 0) return;
      // Spacing is guaranteed by construction: a jittered grid whose step is at least
      // the requested spacing. Rejecting candidates against neighbours would make the
      // result depend on generation order and therefore on which cell ran first.
      let grid = Math.ceil(Math.sqrt(count));
      if (rule.minSpacingMetres > 0) grid = Math.min(grid, Math.max(1, Math.floor(cellSize / rule.minSpacingMetres)));
      count = Math.min(count, grid * grid);
      if (count === 0) return;
      const step = cellSize / grid;
      const jitter = Math.min(0.3 * step, Math.max(0, (step - rule.minSpacingMetres) / 2));
      slots = [];
      for (let slot = 0; slot < count; ++slot) {
        const gx = slot % grid, gz = Math.floor(slot / grid);
        slots.push({
          x: minX + (gx + 0.5) * step + rng.span(-jitter, jitter),
          z: minZ + (gz + 0.5) * step + rng.span(-jitter, jitter),
          slot,
        });
      }
    }
    for (const candidate of slots) {
      const localX = candidate.x, localZ = candidate.z;
      const yaw = rng.unit() * Math.PI * 2;
      const scale = rng.span(rule.scaleRange[0], rule.scaleRange[1]);
      const tiltX = rng.normal() * 0.02, tiltZ = rng.normal() * 0.02;
      const windPhase = rng.range(65536);
      // Identity is assigned here, before any filter. Every rejection below keeps the id of the
      // candidates that survive, so a density change cannot renumber the field.
      const key = candidateKey({ species: rule.species, cellX, cellZ, slot: candidate.slot });
      if (mask) {
        const raw = mask.sample(localX, localZ);
        const value = rule.mask.invert ? 1 - raw : raw;
        const acceptance = 1 - rule.mask.strength + rule.mask.strength * Math.max(0, Math.min(1, value));
        if (rng.unit() > acceptance) { ++stats.rejectedByMask; continue; }
      }
      const acceptance = clusterAcceptance(rule, localX, localZ, deriveSeed(region.seed, rule.species));
      if (rng.unit() > acceptance) continue;
      if (rule.avoidExclusions && shapes.length && excludedBy(shapes, localX, localZ)) { ++stats.rejectedByExclusion; continue; }
      const sample = surface.sample(localX, localZ);
      if (!sample) continue;
      if (sample.slopeRadians > rule.slopeMaxRadians) continue;
      if (sample.heightMetres < rule.heightMin || sample.heightMetres > rule.heightMax) continue;
      if (sample.moisture < rule.moistureMin || sample.moisture > rule.moistureMax) continue;
      instances.push({
        x: localX, y: sample.heightMetres, z: localZ,
        normalY: sample.normal[1],
        yaw, tiltX, tiltZ, scale,
        species: rule.species, lodBias: rule.lodBias, palette: rule.palette,
        windPhase, flags: 0, cellX, cellZ,
        ruleIndex, slot: candidate.slot, candidateId: key,
        minSpacingMetres: rule.minSpacingMetres, priority: rule.priority ?? ruleIndex,
        fromRows: Boolean(rule.rows),
      });
    }
  });
  // Local order is fixed by the grid slot, not by insertion or hash-map iteration.
  instances.sort((a, b) => (a.z - b.z) || (a.x - b.x) || a.species.localeCompare(b.species));
  instances.forEach((instance, index) => { instance.localIndex = index; instance.cellSeed = cellSeed; });
  return Object.assign(instances, { stats });
}

// Region-level spacing. Accepted or rejected purely by higher-priority candidates inside the
// candidate's own radius, so the pass is local: give it the affected chunks plus a halo of
// `maxSpacing` and it reproduces the global result without reading the rest of the world.
export function resolveSpacing({ instances, halo = [] }) {
  const order = [...instances];
  order.sort((a, b) => (a.priority - b.priority) || (a.cellZ - b.cellZ) || (a.cellX - b.cellX) || (a.slot - b.slot));
  let maxSpacing = 0;
  for (const instance of instances) maxSpacing = Math.max(maxSpacing, instance.minSpacingMetres ?? 0);
  for (const instance of halo) maxSpacing = Math.max(maxSpacing, instance.minSpacingMetres ?? 0);
  const cell = Math.max(1, maxSpacing);
  const buckets = new Map();
  const insert = (instance) => {
    const key = `${Math.floor(instance.x / cell)},${Math.floor(instance.z / cell)}`;
    const bucket = buckets.get(key);
    if (bucket) bucket.push(instance); else buckets.set(key, [instance]);
  };
  for (const instance of halo) insert(instance);
  const accepted = [];
  let rejected = 0;
  for (const candidate of order) {
    const spacing = candidate.minSpacingMetres ?? 0;
    if (spacing > 0) {
      const cx = Math.floor(candidate.x / cell), cz = Math.floor(candidate.z / cell);
      let blocked = false;
      for (let dz = -1; dz <= 1 && !blocked; ++dz) for (let dx = -1; dx <= 1 && !blocked; ++dx) {
        const bucket = buckets.get(`${cx + dx},${cz + dz}`);
        if (!bucket) continue;
        for (const other of bucket) {
          const radius = Math.max(spacing, other.minSpacingMetres ?? 0);
          if (radius <= 0) continue;
          const ddx = other.x - candidate.x, ddz = other.z - candidate.z;
          if (ddx * ddx + ddz * ddz < radius * radius) { blocked = true; break; }
        }
      }
      if (blocked) { ++rejected; continue; }
    }
    insert(candidate);
    accepted.push(candidate);
  }
  accepted.sort((a, b) => (a.cellZ - b.cellZ) || (a.cellX - b.cellX) || (a.slot - b.slot));
  return { instances: accepted, rejected };
}

export function scatterRegion({ region, rules, surface, masks, exclusions, spacingHalo = [], onCell }) {
  // A validated region carries its own masks and exclusions; passing them explicitly is only
  // needed when a caller assembles an unvalidated region (an editor preview, a brush draft).
  const maskSet = masks ?? region.masks ?? null;
  const exclusionSet = exclusions ?? region.exclusions ?? null;
  const cellSize = region.cellSizeMetres;
  const cellsX = Math.ceil((region.bounds.maxX - region.bounds.minX) / cellSize);
  const cellsZ = Math.ceil((region.bounds.maxZ - region.bounds.minZ) / cellSize);
  fail(cellsX > 0 && cellsZ > 0, 'NG-E-VEG-SCATTER', 'region bounds describe no cells');
  fail(cellsX * cellsZ <= 64 * 1024 * 1024, 'NG-E-VEG-CAPACITY', 'region has too many scatter cells');
  const normalizedExclusions = exclusionSet
    ? (exclusionSet.shapes ? exclusionSet : normalizeExclusions(exclusionSet))
    : null;
  const all = [];
  let rejectedBySurface = 0, rejectedByMask = 0, rejectedByExclusion = 0;
  for (let cellZ = 0; cellZ < cellsZ; ++cellZ) for (let cellX = 0; cellX < cellsX; ++cellX) {
    const before = surface.misses;
    const instances = scatterCell({
      region, rules, surface, cellX, cellZ, masks: maskSet, exclusions: normalizedExclusions,
    });
    rejectedBySurface += surface.misses - before;
    rejectedByMask += instances.stats?.rejectedByMask ?? 0;
    rejectedByExclusion += instances.stats?.rejectedByExclusion ?? 0;
    for (const instance of instances) all.push(instance);
    onCell?.(cellX, cellZ, instances.length);
  }
  // The region pass only runs for rules that asked for it; otherwise the per-cell result is the
  // final result, byte for byte.
  const regionScoped = rules.some((rule) => rule.spacingScope === 'region');
  let instances = all;
  let rejectedBySpacing = 0;
  if (regionScoped) {
    const resolved = resolveSpacing({ instances: all, halo: spacingHalo });
    instances = resolved.instances;
    rejectedBySpacing = resolved.rejected;
  }
  const perSpecies = new Map();
  for (const instance of instances) perSpecies.set(instance.species, (perSpecies.get(instance.species) ?? 0) + 1);
  return {
    instances, perSpecies, rejectedBySurface, rejectedByMask, rejectedByExclusion, rejectedBySpacing,
    spacingScope: regionScoped ? 'region' : 'cell',
    cellsX, cellsZ,
  };
}

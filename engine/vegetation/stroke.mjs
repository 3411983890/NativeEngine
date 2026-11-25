// Brush strokes.
//
// The plan's §4.2 states three rules that a naive implementation gets wrong, and each of them is
// a property of this module rather than of the editor that calls it:
//
//   1. "按世界距离重采样，不按鼠标事件数累加" — a stroke is a *path in metres*, not a list of
//      mouse events. Two recordings of the same gesture at 30 fps and at 240 fps must produce the
//      same mask, byte for byte. That is why `resamplePath` exists: every consumer works from the
//      arc-length resampling, never from the raw samples.
//   2. "绘制为目标覆盖度/密度遮罩；加密是显式操作" — painting writes a coverage *target* with
//      `max`, so painting over the same spot twice is the same as painting it once. Density is
//      only raised by an explicit `densify`, which is additive on purpose and is the only mode
//      that accumulates.
//   3. "擦除存持久遮罩或 tombstone" — erase lowers coverage to zero and keeps the zero. It does
//      not delete a plant, and it does not "forget" the area: a later paint has to raise the
//      coverage again, which is what makes undo/redo a mask edit instead of a re-cook.
//
// Strokes write into a `CoverageMask` (scatter.mjs), so the brush, the region cook and the tests
// all agree on what "the brush painted here" means.

export const STROKE_SCHEMA = 'nativegpu.vegetation-stroke/1';

export const STROKE_MODES = new Set(['paint', 'erase', 'densify']);

export class StrokeError extends Error {
  constructor(code, message) { super(message); this.name = 'StrokeError'; this.code = code; }
}
function fail(ok, code, message) { if (!ok) throw new StrokeError(code, message); }

// Arc-length resampling of a polyline. The output spacing is exact (the last point is always
// included), so two different samplings of the same geometry produce the same stamps: the number
// of input samples changes, the result does not.
export function resamplePath(points, spacingMetres) {
  fail(Array.isArray(points) && points.length >= 1, 'NG-E-VEG-STROKE', 'a stroke needs at least one point');
  fail(typeof spacingMetres === 'number' && spacingMetres > 0 && spacingMetres <= 10,
    'NG-E-VEG-STROKE', 'spacingMetres must be in (0,10]');
  const path = points.map((point, index) => {
    fail(Array.isArray(point) && point.length >= 2
      && Number.isFinite(point[0]) && Number.isFinite(point[1]),
    'NG-E-VEG-STROKE', `stroke point ${index} must be a finite [x,z] pair`);
    return [point[0], point[1]];
  });
  const out = [path[0]];
  if (path.length === 1) return out;
  // Cumulative arc length, then a stamp at every exact multiple of the spacing. Walking segment
  // by segment with a carried remainder would give the same shape but not the same numbers: the
  // same gesture recorded at 30 fps and at 240 fps is a different *subdivision* of the same
  // polyline, and a per-segment remainder accumulates a slightly different error in each case.
  // A cumulative table plus a fixed 1 µm quantization makes the output depend on the geometry
  // only, which is the property §4.2 asks for.
  const cumulative = [0];
  for (let index = 1; index < path.length; ++index) {
    const dx = path[index][0] - path[index - 1][0];
    const dz = path[index][1] - path[index - 1][1];
    cumulative.push(cumulative[index - 1] + Math.hypot(dx, dz));
  }
  const total = cumulative[cumulative.length - 1];
  const at = (target) => {
    let low = 0, high = cumulative.length - 1;
    while (low + 1 < high) {
      const middle = (low + high) >> 1;
      if (cumulative[middle] <= target) low = middle; else high = middle;
    }
    const span = cumulative[high] - cumulative[low];
    const t = span < 1e-12 ? 0 : (target - cumulative[low]) / span;
    return [
      Math.round((path[low][0] + (path[high][0] - path[low][0]) * t) * 1e6) / 1e6,
      Math.round((path[low][1] + (path[high][1] - path[low][1]) * t) * 1e6) / 1e6,
    ];
  };
  for (let step = 1; step * spacingMetres <= total; ++step) out.push(at(step * spacingMetres));
  const last = path[path.length - 1];
  const tail = out[out.length - 1];
  if (Math.hypot(last[0] - tail[0], last[1] - tail[1]) > 1e-9) out.push(last);
  return out;
}

// Dirty rectangle of a stroke. The plan requires "每次只重建笔迹与其影响半径涉及的分块", and a
// chunk rebuild needs the set of chunks to rebuild — computed here, from the geometry, before any
// mask is written.
export function strokeBounds(points, radiusMetres) {
  fail(Array.isArray(points) && points.length >= 1, 'NG-E-VEG-STROKE', 'a stroke needs at least one point');
  fail(typeof radiusMetres === 'number' && radiusMetres >= 0, 'NG-E-VEG-STROKE', 'radiusMetres must be >= 0');
  let minX = Infinity, minZ = Infinity, maxX = -Infinity, maxZ = -Infinity;
  for (const point of points) {
    minX = Math.min(minX, point[0]); maxX = Math.max(maxX, point[0]);
    minZ = Math.min(minZ, point[1]); maxZ = Math.max(maxZ, point[1]);
  }
  return {
    minX: minX - radiusMetres, minZ: minZ - radiusMetres,
    maxX: maxX + radiusMetres, maxZ: maxZ + radiusMetres,
  };
}

// One stamp of the brush: a soft disc written into the mask according to `mode`. Kept separate
// from `applyStroke` so the editor's live preview can stamp a single dab without building a path.
export function stampMask(mask, { x, z, radiusMetres, strength = 1, falloffMetres = 0, mode = 'paint' }) {
  fail(STROKE_MODES.has(mode), 'NG-E-VEG-STROKE', `unknown stroke mode ${mode}`);
  fail(typeof strength === 'number' && strength >= 0 && strength <= 1, 'NG-E-VEG-STROKE', 'strength must be in [0,1]');
  const radius = Math.max(mask.cellSizeMetres, radiusMetres);
  const inner = Math.max(0, radius - Math.max(0, falloffMetres));
  const x0 = Math.max(0, Math.floor((x - radius - mask.originX) / mask.cellSizeMetres));
  const x1 = Math.min(mask.width - 1, Math.ceil((x + radius - mask.originX) / mask.cellSizeMetres));
  const z0 = Math.max(0, Math.floor((z - radius - mask.originZ) / mask.cellSizeMetres));
  const z1 = Math.min(mask.height - 1, Math.ceil((z + radius - mask.originZ) / mask.cellSizeMetres));
  let touched = 0;
  for (let iz = z0; iz <= z1; ++iz) for (let ix = x0; ix <= x1; ++ix) {
    const px = mask.originX + (ix + 0.5) * mask.cellSizeMetres;
    const pz = mask.originZ + (iz + 0.5) * mask.cellSizeMetres;
    const distance = Math.hypot(px - x, pz - z);
    if (distance > radius) continue;
    const t = distance <= inner ? 1 : 1 - (distance - inner) / Math.max(1e-6, radius - inner);
    const weight = t * t * (3 - 2 * t) * strength;
    const index = iz * mask.width + ix;
    const current = mask.values[index];
    // The three modes differ only in how they combine with the existing value. Stating that here,
    // once, is what stops "erase" from being implemented as "paint with zero" — which would leave
    // a soft edge that a later paint could never restore.
    if (mode === 'paint') mask.values[index] = Math.max(current, weight);
    else if (mode === 'densify') mask.values[index] = Math.min(1, current + weight);
    else mask.values[index] = Math.min(current, 1 - weight);
    if (mask.values[index] !== current) ++touched;
  }
  return touched;
}

// A whole stroke. Returns the resampled path and the dirty rectangle so the caller can rebuild
// exactly the chunks the stroke touched, and nothing else.
export function applyStroke(mask, {
  points, radiusMetres, spacingMetres = null, strength = 1, falloffMetres = null, mode = 'paint',
}) {
  fail(mask && mask.values && Number.isInteger(mask.width), 'NG-E-VEG-STROKE', 'applyStroke needs a CoverageMask');
  fail(STROKE_MODES.has(mode), 'NG-E-VEG-STROKE', `unknown stroke mode ${mode}`);
  // The resampling step defaults to a third of the brush radius: fine enough that the discs
  // overlap into a continuous band, coarse enough that a long stroke is not thousands of stamps.
  const spacing = spacingMetres ?? Math.max(mask.cellSizeMetres * 0.5, radiusMetres / 3);
  const path = resamplePath(points, spacing);
  const falloff = falloffMetres ?? radiusMetres * 0.5;
  let touched = 0;
  for (const point of path) {
    touched += stampMask(mask, { x: point[0], z: point[1], radiusMetres, strength, falloffMetres: falloff, mode });
  }
  return {
    schema: STROKE_SCHEMA,
    mode,
    path,
    stamps: path.length,
    spacingMetres: spacing,
    radiusMetres,
    touchedCells: touched,
    bounds: strokeBounds(path, radiusMetres),
  };
}

// The chunks a stroke invalidates. Stated as cell coordinates so a caller can rebuild exactly
// those and leave every other chunk's digest untouched.
export function strokeCells(bounds, { cellSizeMetres, originX = 0, originZ = 0, cellsX = Infinity, cellsZ = Infinity }) {
  fail(cellSizeMetres > 0, 'NG-E-VEG-STROKE', 'cellSizeMetres must be positive');
  const minX = Math.max(0, Math.floor((bounds.minX - originX) / cellSizeMetres));
  const maxX = Math.min(cellsX - 1, Math.floor((bounds.maxX - originX) / cellSizeMetres));
  const minZ = Math.max(0, Math.floor((bounds.minZ - originZ) / cellSizeMetres));
  const maxZ = Math.min(cellsZ - 1, Math.floor((bounds.maxZ - originZ) / cellSizeMetres));
  const cells = [];
  for (let cellZ = minZ; cellZ <= maxZ; ++cellZ) for (let cellX = minX; cellX <= maxX; ++cellX) cells.push({ cellX, cellZ });
  return cells;
}

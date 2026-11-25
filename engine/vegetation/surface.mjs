// Surface sampling contract shared by scatter, the editor preview and the runtime.
//
// Every consumer asks the same question — "what is the ground here, and may a plant
// grow here?" — and gets the same answer, including the sampler version. A scatter
// cook records that version so a terrain change invalidates the placement instead of
// silently floating a forest.

export const SURFACE_SCHEMA = 'nativegpu.vegetation-surface/1';

export class SurfaceError extends Error {
  constructor(code, message) { super(message); this.name = 'SurfaceError'; this.code = code; }
}
const fail = (ok, code, message) => { if (!ok) throw new SurfaceError(code, message); };

const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

// Bilinear value noise over a hash grid. It is continuous across cell boundaries, so
// a cluster mask never shows a seam where two scatter cells meet.
function hash2(x, y, seed) {
  let h = (x * 374761393 + y * 668265263 + seed * 2246822519) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177) >>> 0;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
export function valueNoise(x, y, seed) {
  const ix = Math.floor(x), iy = Math.floor(y);
  const fx = x - ix, fy = y - iy;
  const sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const n00 = hash2(ix, iy, seed), n10 = hash2(ix + 1, iy, seed);
  const n01 = hash2(ix, iy + 1, seed), n11 = hash2(ix + 1, iy + 1, seed);
  return (n00 * (1 - sx) + n10 * sx) * (1 - sy) + (n01 * (1 - sx) + n11 * sx) * sy;
}
export function fractalNoise(x, y, seed, octaves = 3) {
  let sum = 0, amplitude = 0.5, frequency = 1, total = 0;
  for (let octave = 0; octave < octaves; ++octave) {
    sum += valueNoise(x * frequency, y * frequency, seed + octave * 7919) * amplitude;
    total += amplitude; amplitude *= 0.5; frequency *= 2.03;
  }
  return sum / total;
}

// A static triangle soup sampled vertically. This is the adapter for an already
// imported scene mesh; a terrain height field is a different adapter with the same
// interface, which is why the sampler owns the query instead of the scatter code.
export class MeshSurface {
  constructor({ positions, indices, version, name = 'mesh' }) {
    fail(positions && indices && positions.length >= 9, 'NG-E-VEG-SURFACE', 'mesh surface needs positions and indices');
    fail(indices.length % 3 === 0, 'NG-E-VEG-SURFACE', 'mesh surface indices must describe whole triangles');
    this.positions = positions; this.indices = indices;
    this.version = version ?? `mesh:${name}:${indices.length / 3}`;
    this.kind = 'mesh';
    this.triangles = indices.length / 3;
    let minX = Infinity, minZ = Infinity, maxX = -Infinity, maxZ = -Infinity, minY = Infinity, maxY = -Infinity;
    for (let i = 0; i < positions.length; i += 3) {
      minX = Math.min(minX, positions[i]); maxX = Math.max(maxX, positions[i]);
      minY = Math.min(minY, positions[i + 1]); maxY = Math.max(maxY, positions[i + 1]);
      minZ = Math.min(minZ, positions[i + 2]); maxZ = Math.max(maxZ, positions[i + 2]);
    }
    this.bounds = { minX, minZ, maxX, maxZ, minY, maxY };
    this.cell = Math.max(0.25, Math.sqrt((maxX - minX) * (maxZ - minZ) / Math.max(1, this.triangles)));
    this.grid = new Map();
    for (let triangle = 0; triangle < this.triangles; ++triangle) {
      const box = this.triangleBounds(triangle);
      const x0 = Math.floor(box.minX / this.cell), x1 = Math.floor(box.maxX / this.cell);
      const z0 = Math.floor(box.minZ / this.cell), z1 = Math.floor(box.maxZ / this.cell);
      for (let x = x0; x <= x1; ++x) for (let z = z0; z <= z1; ++z) {
        const key = `${x},${z}`;
        let list = this.grid.get(key);
        if (!list) this.grid.set(key, list = []);
        list.push(triangle);
      }
    }
  }
  triangleBounds(triangle) {
    const a = this.indices[triangle * 3], b = this.indices[triangle * 3 + 1], c = this.indices[triangle * 3 + 2];
    const p = this.positions;
    return {
      minX: Math.min(p[a * 3], p[b * 3], p[c * 3]), maxX: Math.max(p[a * 3], p[b * 3], p[c * 3]),
      minZ: Math.min(p[a * 3 + 2], p[b * 3 + 2], p[c * 3 + 2]), maxZ: Math.max(p[a * 3 + 2], p[b * 3 + 2], p[c * 3 + 2]),
    };
  }
  // Vertical ray. Returns the highest hit so a plant stands on the visible surface
  // rather than on a cave roof underneath it.
  sample(x, z) {
    const list = this.grid.get(`${Math.floor(x / this.cell)},${Math.floor(z / this.cell)}`);
    if (!list) return null;
    let best = null;
    for (const triangle of list) {
      const i0 = this.indices[triangle * 3], i1 = this.indices[triangle * 3 + 1], i2 = this.indices[triangle * 3 + 2];
      const p = this.positions;
      const ax = p[i0 * 3], ay = p[i0 * 3 + 1], az = p[i0 * 3 + 2];
      const bx = p[i1 * 3], by = p[i1 * 3 + 1], bz = p[i1 * 3 + 2];
      const cx = p[i2 * 3], cy = p[i2 * 3 + 1], cz = p[i2 * 3 + 2];
      const denominator = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
      if (Math.abs(denominator) < 1e-12) continue;
      const l0 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / denominator;
      const l1 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / denominator;
      const l2 = 1 - l0 - l1;
      if (l0 < -1e-6 || l1 < -1e-6 || l2 < -1e-6) continue;
      const y = l0 * ay + l1 * by + l2 * cy;
      if (best === null || y > best.height) {
        const normal = normalize([
          (by - ay) * (cz - az) - (bz - az) * (cy - ay),
          (bz - az) * (cx - ax) - (bx - ax) * (cz - az),
          (bx - ax) * (cy - ay) - (by - ay) * (cx - ax),
        ]);
        best = { height: y, normal, triangle };
      }
    }
    return best;
  }
}

function normalize(v) { const l = Math.hypot(v[0], v[1], v[2]); return l < 1e-9 ? [0, 1, 0] : [v[0] / l, v[1] / l, v[2] / l]; }

export class PlaneSurface {
  constructor({ height = 0, version = 'plane:0' } = {}) {
    this.height = height; this.version = version; this.kind = 'plane';
    this.bounds = { minX: -1e6, minZ: -1e6, maxX: 1e6, maxZ: 1e6, minY: height, maxY: height };
  }
  sample() { return { height: this.height, normal: [0, 1, 0], triangle: -1 }; }
}

// The sampler is what a cook or an editor talks to. It adds the derived masks every
// scatter rule reads, and refuses to answer outside the surface bounds instead of
// returning a plausible zero.
export class SurfaceSampler {
  constructor(surface, options = {}) {
    fail(surface && (surface.kind === 'plane' || surface.kind === 'mesh'), 'NG-E-VEG-SURFACE', 'unknown surface adapter');
    this.surface = surface;
    this.version = surface.version;
    this.moistureSeed = options.moistureSeed ?? 1337;
    this.moistureScale = options.moistureScale ?? 0.004;
    this.samples = 0;
    this.misses = 0;
  }
  get bounds() { return this.surface.bounds; }
  get kind() { return this.surface.kind; }
  sample(x, z) {
    const hit = this.surface.sample(x, z);
    if (!hit) { ++this.misses; return null; }
    ++this.samples;
    const slope = Math.acos(Math.min(1, Math.max(-1, hit.normal[1])));
    return {
      position: [x, hit.height, z],
      normal: hit.normal,
      slopeRadians: slope,
      heightMetres: hit.height,
      moisture: fractalNoise(x * this.moistureScale, z * this.moistureScale, this.moistureSeed, 3),
      surfaceType: this.surface.kind === 'plane' ? 'ground' : 'mesh',
      version: this.version,
    };
  }
}

export function createSurface(description) {
  fail(description && typeof description === 'object', 'NG-E-VEG-SURFACE', 'surface description must be an object');
  if (description.kind === 'plane') return new PlaneSurface({ height: description.heightMetres ?? 0, version: description.version ?? 'plane:0' });
  if (description.kind === 'mesh') return new MeshSurface({ positions: description.positions, indices: description.indices, version: description.version, name: description.name });
  throw new SurfaceError('NG-E-VEG-SURFACE', `unsupported surface kind ${description.kind}`);
}

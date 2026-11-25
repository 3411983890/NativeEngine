import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { buildPackage, digestPackage, packRecords, SectionKind } from '../assets/binary-package.mjs';
import { Stream, deriveSeed } from './random.mjs';
import { defaultGenerators, normalizeGenerators } from './generators.mjs';
import { evaluateBranchGraph, validateBranchGraph } from './branch-graph.mjs';
import { emitBranch, emitJunction } from './branch-mesh.mjs';
import { placeLeaves, emitLeafBlade, emitGrassBlade } from './leaf-placement.mjs';
import { buildAtlas } from './atlas.mjs';
import { describeTextureBindings, normalizeMaterial } from './material-contract.mjs';
import { emitComponents, normalizeComponents, placeComponents } from './components.mjs';

// Species cook. A species source declares intent (a profile, dimensions, wind
// response, material groups). Everything the runtime draws — LOD meshes, per-vertex
// wind weights, collision/shadow proxies, impostor atlas and the measured
// simplification error of every LOD — is baked here.
//
// The rule this module exists to enforce: a LOD may not claim a smaller error than
// the geometry actually has. `worldErrorMetres` is measured against LOD0 by sampling
// and reporting both the maximum and the 95th percentile deviation.

export const SPECIES_SCHEMA = 'nativegpu.vegetation-species/1';
export const SPECIES_COOK_SCHEMA = 'nativegpu.vegetation-species-cook/1';
export const SPECIES_KINDS = new Set(['tree', 'shrub', 'grass']);
export const SPECIES_PROFILES = new Set(['broadleaf', 'conifer', 'shrub', 'grass']);
const ID = /^[a-z][a-z0-9_-]{0,63}$/;

export class VegetationError extends Error {
  constructor(code, message) { super(message); this.name = 'VegetationError'; this.code = code; }
}
function requireValue(ok, message, code = 'NG-E-VEG-SPECIES') {
  if (!ok) throw new VegetationError(code, message);
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
  requireValue(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high, `${label} must be finite and in [${low},${high}]`);
}
function vector(value, length, low, high, label) {
  requireValue(Array.isArray(value) && value.length === length, `${label} requires ${length} values`);
  value.forEach((entry, i) => scalar(entry, low, high, `${label}[${i}]`));
}

// ---------------------------------------------------------------- vector helpers
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
function normalize(a) { const l = Math.hypot(a[0], a[1], a[2]); return l < 1e-9 ? [0, 1, 0] : [a[0] / l, a[1] / l, a[2] / l]; }
// ------------------------------------------------------------------ mesh builder
class MeshBuilder {
  constructor() {
    this.positions = []; this.normals = []; this.uvs = []; this.colors = []; this.wind = [];
    this.indices = []; this.primitives = []; this.open = null;
  }
  begin(material) {
    this.end();
    this.open = { material, firstIndex: this.indices.length, indexCount: 0 };
  }
  end() {
    if (!this.open) return;
    this.open.indexCount = this.indices.length - this.open.firstIndex;
    if (this.open.indexCount > 0) this.primitives.push(this.open);
    this.open = null;
  }
  vertex(position, normal, uv, color, weight) {
    this.positions.push(position[0], position[1], position[2]);
    this.normals.push(normal[0], normal[1], normal[2]);
    this.uvs.push(uv[0], uv[1]);
    this.colors.push(color[0], color[1], color[2], color[3]);
    this.wind.push(weight);
    return this.wind.length - 1;
  }
  triangle(a, b, c) { this.indices.push(a, b, c); }
  quad(a, b, c, d) { this.indices.push(a, b, c, a, c, d); }
}

// --------------------------------------------------------------------- materials
const DEFAULT_MATERIALS = {
  bark: { id: 'bark', kind: 'bark', alphaMasked: false, doubleSided: false, roughness: 0.82, thicknessMetres: 0.02, transmission: [0.06, 0.04, 0.02] },
  leaf: { id: 'leaf', kind: 'leaf', alphaMasked: true, doubleSided: true, roughness: 0.55, thicknessMetres: 0.0004, transmission: [0.22, 0.42, 0.10] },
  blade: { id: 'blade', kind: 'blade', alphaMasked: true, doubleSided: true, roughness: 0.48, thicknessMetres: 0.0003, transmission: [0.30, 0.55, 0.14] },
};

// ------------------------------------------------------- structured mesh (graph path)
// Every kind goes through the branch graph. A broadleaf, a conifer and a shrub are a trunk with
// children; a grass clump is N stems from one base with no children. Both are the same structure
// with different level tables, so attachment, sweep, junction, blade/leaf placement and wind
// binding are shared code rather than one recipe per kind.
function structuredGenerators(spec) {
  return normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
}

function finalizeMesh(mesh) {
  return {
    positions: Float32Array.from(mesh.positions),
    normals: Float32Array.from(mesh.normals),
    uvs: Float32Array.from(mesh.uvs),
    colors: Uint8Array.from(mesh.colors),
    weights: Float32Array.from(mesh.wind),
    indices: Uint32Array.from(mesh.indices),
    primitives: mesh.primitives,
  };
}

// Foliage coverage as a measurement. Leaf area is one-sided (area of the blades); LAI divides it by
// the crown's projection disc, which is the number that answers "is the crown full or a shell of
// cards" without a screenshot. Shoots are the placement unit, so a coarser LOD reports fewer shoots
// rather than fewer scattered leaves.
function foliageSummary(graph, perFrond, leafAreaMetresSquared, minLeafSpacingMetres) {
  if (!graph) return null;
  const crownRadius = Math.max(0.01, graph.stats.crownRadialMetres ?? 0.01);
  const crownHeight = Math.max(0.01, (graph.stats.boundsMaxY ?? 0) - (graph.stats.boundsMinY ?? 0));
  const projection = Math.PI * crownRadius * crownRadius;
  const volume = (4 / 3) * Math.PI * crownRadius * crownRadius * (crownHeight / 2);
  const sum = (key) => (perFrond ? perFrond.reduce((total, entry) => total + (entry[key] ?? 0), 0) : 0);
  return {
    leafAreaMetresSquared: leafAreaMetresSquared ?? 0,
    leafAreaIndex: Number(((leafAreaMetresSquared ?? 0) / projection).toFixed(4)),
    leafAreaPerCrownVolume: Number(((leafAreaMetresSquared ?? 0) / volume).toFixed(4)),
    shoots: sum('shoots'),
    emptyShoots: sum('emptyShoots'),
    spacingRejected: sum('spacingRejected'),
    minLeafSpacingMetres: minLeafSpacingMetres ?? null,
  };
}

// Components (fruit, flower, seed head, pod, compound leaf, floating pad, culm node band,
// imported instance) are placed on the branch graph and emitted after the foliage. They are a
// separate pass for a reason: their placement streams are keyed by component id, so adding a
// fruit component cannot re-roll a single leaf, and a stage filter can drop a whole part
// without touching the crown.
function buildComponents(spec, graph, quality, seed, options, mesh, fallbackMaterial) {
  const components = options.components ?? normalizeComponents(spec);
  if (!components) return { parts: [], perComponent: [] };
  const placed = placeComponents({
    graph, components, seed,
    stage: options.stage ?? spec.stage ?? null,
    quality,
    assets: options.assets ?? null,
  });
  if (placed.parts.length === 0) return { parts: [], perComponent: placed.perComponent };
  const triangles = emitComponents(mesh, placed.parts, {
    materialOf: (part) => part.material ?? fallbackMaterial,
    graph,
  });
  // The reported cost is the emitted cost. Keeping the count next to the emitter is what stops
  // "the species has 40 fruit" from being an authoring claim rather than a measurement.
  for (const entry of placed.perComponent) entry.triangles = triangles.get(entry.id) ?? 0;
  return { parts: placed.parts, perComponent: placed.perComponent, assets: placed.assets };
}

function buildStructuredMesh(spec, quality, seed, options = {}) {
  const generators = structuredGenerators(spec);
  const graph = evaluateBranchGraph({ spec, generators, seed });
  validateBranchGraph(graph);
  const mesh = new MeshBuilder();
  const atlas = options.atlas ?? null;
  const tileMetres = options.barkTiling?.tileMetres ?? 1.6;
  const bark = spec.materials.find((material) => material.kind === 'bark') ?? DEFAULT_MATERIALS.bark;
  const leafMaterial = spec.materials.find((material) => material.kind === 'leaf' || material.kind === 'blade') ?? DEFAULT_MATERIALS.leaf;
  const radial = Math.max(4, Math.round(8 * quality.radial));

  // Bark V phase is inherited from the parent at the attachment, so the tile is continuous
  // across a fork instead of restarting at every child.
  const phase = new Map();
  for (const branch of graph.branches) {
    const parentPhase = branch.parentId
      ? (phase.get(branch.parentId) ?? 0) + branch.attachment.parentArcMetres
      : 0;
    phase.set(branch.id, parentPhase);
    const shade = 1 - Math.min(0.4, branch.depth * 0.075);
    const colour = [Math.round(112 * shade), Math.round(88 * shade), Math.round(64 * shade), 255];
    emitBranch(mesh, branch, {
      radial, tileMetres, materialId: bark.id, color: colour, uvPhaseMetres: parentPhase,
      // A tip is capped; the base is capped only where it is visible (a shrub stem standing on
      // the ground). A child's base is never capped: it is inside its parent.
      capTip: branch.childIds.length === 0,
      capRoot: branch.parentId === null,
    });
  }
  for (const branch of graph.branches) {
    if (!branch.parentId) continue;
    emitJunction(mesh, graph.byId.get(branch.parentId), branch, {
      radial, tileMetres, materialId: bark.id, color: [96, 76, 56, 255],
    });
  }

  const leaves = placeLeaves({ graph, fronds: generators.fronds, atlas, spec, quality, seed });
  for (const leaf of leaves.leaves) {
    emitLeafBlade(mesh, leaf, {
      materialId: leafMaterial.id,
      color: [86, 132, 52, 255],
      cell: atlas && leaf.cell !== null ? atlas.cells[leaf.cell] : null,
    });
  }

  const components = buildComponents(spec, graph, quality, seed, options, mesh, leafMaterial.id);

  return {
    mesh: finalizeMesh(mesh),
    graph,
    leaves: leaves.leaves,
    perFrond: leaves.perFrond,
    leafAreaMetresSquared: leaves.leafAreaMetresSquared,
    minLeafSpacingMetres: leaves.spacingMetres,
    generators,
    componentParts: components.parts,
    perComponent: components.perComponent,
    componentAssets: components.assets ?? [],
  };
}

function buildMesh(spec, quality, seed, uv = { barkTiling: null, leafAtlas: null, atlas: null }) {
  if (spec.kind === 'grass') return buildGrassMesh(spec, quality, seed).mesh;
  return buildStructuredMesh(spec, quality, seed, { atlas: uv.atlas ?? null, barkTiling: uv.barkTiling }).mesh;
}

// Grass. A clump is a set of blades swept along the branch graph, one strand per blade, with
// the same stable ids, arc-length tables and per-vertex wind weights a tree uses.
//
// This used to be the last caller of the legacy mesher (`buildLegacyMesh`), which meant a grass
// clump was built by independent per-segment cylinders and four-vertex cards while the tree path
// had already moved to continuous sweeps and pivoted leaves. Two meshers is how "the shrub is
// fixed but the grass still floats" happens, so the legacy path is gone rather than kept around
// for grass.
function buildGrassMesh(spec, quality, seed, options = {}) {
  const generators = structuredGenerators(spec);
  const graph = evaluateBranchGraph({ spec, generators, seed });
  validateBranchGraph(graph);
  const mesh = new MeshBuilder();
  const material = spec.materials.find((entry) => entry.kind === 'blade' || entry.kind === 'leaf')
    ?? DEFAULT_MATERIALS.blade;
  // Blade width is the blade's own dimension, not the leaf card's: a clump whose blades are
  // as wide as a broadleaf leaf is a hosta, not grass.
  const widthMetres = spec.dimensions.leafWidthMetres
    ?? Math.max(0.006, spec.dimensions.crownRadiusMetres * 0.12);
  let blades = 0;
  // Blade identity is a per-branch key, not a per-pass index. Sorting by that key and taking a
  // prefix makes every coarser LOD a strict subset of the finer one — the same blades survive at
  // every level instead of a fresh random draw, and the count cannot fall to zero (a LOD with no
  // triangles measures an infinite error, which is exactly what a clump of eight blades did at
  // 28% density before this was rank-based).
  const candidates = graph.branches.map((branch) => {
    const rng = new Stream(deriveSeed(seed, `blades/${branch.id}`), 'blades');
    return { branch, keep: rng.unit() };
  }).sort((a, b) => (a.keep - b.keep) || (a.branch.id < b.branch.id ? -1 : 1));
  const wanted = Math.max(1, Math.min(candidates.length, Math.ceil(candidates.length * quality.leaves)));
  // Blade tessellation drops with the LOD as well. `quality.radial` is the same knob the wood
  // sweep uses, so one number controls silhouette cost for every kind.
  const step = Math.max(1, Math.round(1 / Math.max(0.05, quality.radial)));
  for (const candidate of candidates.slice(0, wanted)) {
    emitGrassBlade(mesh, candidate.branch, {
      materialId: material.id,
      widthMetres,
      step,
      color: [86, 128, 52, 255],
    });
    ++blades;
  }
  // A seed head is a component on the blade it belongs to, not a second kind of blade: rice,
  // foxtail and reed are all "a clump plus an ear", which is exactly the composition the
  // component layer exists for.
  const components = buildComponents(spec, graph, quality, seed, options, mesh, material.id);
  return {
    mesh: finalizeMesh(mesh),
    graph,
    leaves: [],
    perFrond: [{ id: 'blade', leaves: blades, hosts: graph.branches.length }],
    generators,
    componentParts: components.parts,
    perComponent: components.perComponent,
    componentAssets: components.assets ?? [],
  };
}
// ------------------------------------------------------------- error measurement
function closestPointSquared(p, a, b, c) {
  const ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return dot(ap, ap);
  const bp = sub(p, b), d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return dot(bp, bp);
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const v = d1 / (d1 - d3);
    const q = add(a, mul(ab, v)), d = sub(p, q);
    return dot(d, d);
  }
  const cp = sub(p, c), d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return dot(cp, cp);
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const w = d2 / (d2 - d6);
    const q = add(a, mul(ac, w)), d = sub(p, q);
    return dot(d, d);
  }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    const q = add(b, mul(sub(c, b), w)), d = sub(p, q);
    return dot(d, d);
  }
  const denominator = 1 / (va + vb + vc);
  const v = vb * denominator, w = vc * denominator;
  const q = add(add(a, mul(ab, v)), mul(ac, w)), d = sub(p, q);
  return dot(d, d);
}

function samplePoints(mesh, maximum) {
  const count = mesh.positions.length / 3;
  const step = Math.max(1, Math.ceil(count / maximum));
  const points = [];
  for (let index = 0; index < count; index += step) {
    points.push([mesh.positions[index * 3], mesh.positions[index * 3 + 1], mesh.positions[index * 3 + 2]]);
  }
  const triangles = mesh.indices.length / 3;
  const triangleStep = Math.max(1, Math.ceil(triangles / Math.max(1, Math.floor(maximum / 2))));
  for (let triangle = 0; triangle < triangles; triangle += triangleStep) {
    const i0 = mesh.indices[triangle * 3], i1 = mesh.indices[triangle * 3 + 1], i2 = mesh.indices[triangle * 3 + 2];
    points.push([
      (mesh.positions[i0 * 3] + mesh.positions[i1 * 3] + mesh.positions[i2 * 3]) / 3,
      (mesh.positions[i0 * 3 + 1] + mesh.positions[i1 * 3 + 1] + mesh.positions[i2 * 3 + 1]) / 3,
      (mesh.positions[i0 * 3 + 2] + mesh.positions[i1 * 3 + 2] + mesh.positions[i2 * 3 + 2]) / 3,
    ]);
  }
  return points;
}

// One-sided deviation of the dense LOD against a coarser one. Sampling the dense
// surface is the direction that matters for silhouette loss: a coarse LOD that adds
// geometry nothing asked for cannot happen here, because every LOD is a reduction.
export function measureLodError(dense, coarse, maximumSamples = 512) {
  if (coarse.indices.length === 0) return { maximum: Infinity, p95: Infinity, samples: 0 };
  const points = samplePoints(dense, maximumSamples);
  const triangleCount = coarse.indices.length / 3;
  const distances = [];
  for (const point of points) {
    let best = Infinity;
    for (let triangle = 0; triangle < triangleCount; ++triangle) {
      const i0 = coarse.indices[triangle * 3], i1 = coarse.indices[triangle * 3 + 1], i2 = coarse.indices[triangle * 3 + 2];
      const d = closestPointSquared(point,
        [coarse.positions[i0 * 3], coarse.positions[i0 * 3 + 1], coarse.positions[i0 * 3 + 2]],
        [coarse.positions[i1 * 3], coarse.positions[i1 * 3 + 1], coarse.positions[i1 * 3 + 2]],
        [coarse.positions[i2 * 3], coarse.positions[i2 * 3 + 1], coarse.positions[i2 * 3 + 2]]);
      if (d < best) best = d;
    }
    distances.push(Math.sqrt(best));
  }
  distances.sort((a, b) => a - b);
  // Surface area is measured alongside the distance, because the two failure modes of a coarse
  // LOD are different. Distance answers "did the remaining surface move"; area answers "how much
  // surface is gone". A LOD that drops 38% of the leaf cards moves no remaining vertex, so the
  // distance stays small while the crown thins — the documented defect where the error table said
  // 0.085 m for a LOD that had already lost a third of its foliage.
  const denseArea = surfaceArea(dense);
  const coarseArea = surfaceArea(coarse);
  return {
    maximum: distances[distances.length - 1],
    p95: distances[Math.min(distances.length - 1, Math.floor(distances.length * 0.95))],
    samples: distances.length,
    denseArea,
    coarseArea,
    areaRetention: denseArea > 0 ? coarseArea / denseArea : 1,
  };
}

// Sum of triangle areas. Not a projection, so it is camera independent: it answers "how much
// plant surface is still there", which is what leaf removal changes.
function surfaceArea(mesh) {
  let total = 0;
  const triangles = mesh.indices.length / 3;
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const i0 = mesh.indices[triangle * 3], i1 = mesh.indices[triangle * 3 + 1], i2 = mesh.indices[triangle * 3 + 2];
    const ax = mesh.positions[i1 * 3] - mesh.positions[i0 * 3];
    const ay = mesh.positions[i1 * 3 + 1] - mesh.positions[i0 * 3 + 1];
    const az = mesh.positions[i1 * 3 + 2] - mesh.positions[i0 * 3 + 2];
    const bx = mesh.positions[i2 * 3] - mesh.positions[i0 * 3];
    const by = mesh.positions[i2 * 3 + 1] - mesh.positions[i0 * 3 + 1];
    const bz = mesh.positions[i2 * 3 + 2] - mesh.positions[i0 * 3 + 2];
    const cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    total += Math.hypot(cx, cy, cz) * 0.5;
  }
  return total;
}

// --------------------------------------------------------------- impostor bake
// Software orthographic rasteriser. The impostor is real baked coverage and depth,
// not a placeholder: a far LOD that samples an empty atlas would be a declared
// representation the loader must reject, and this cook would rather fail here.
export function bakeImpostor(mesh, options) {
  const views = options.views, size = options.size;
  const height = options.heightMetres, radius = options.radiusMetres;
  const atlas = new Uint8Array(size * views * size * 4);
  const depth = new Float32Array(size * views * size);
  depth.fill(1);
  const light = normalize([0.4, 0.8, 0.45]);
  const triangles = mesh.indices.length / 3;
  for (let view = 0; view < views; ++view) {
    const angle = (view / views) * Math.PI * 2;
    const right = [Math.cos(angle), 0, Math.sin(angle)];
    const forward = [-Math.sin(angle), 0, Math.cos(angle)];
    for (let triangle = 0; triangle < triangles; ++triangle) {
      const idx = [mesh.indices[triangle * 3], mesh.indices[triangle * 3 + 1], mesh.indices[triangle * 3 + 2]];
      const projected = idx.map((index) => {
        const x = mesh.positions[index * 3], y = mesh.positions[index * 3 + 1], z = mesh.positions[index * 3 + 2];
        return {
          sx: (dot([x, y, z], right) / (radius * 2) + 0.5) * size,
          sy: (1 - (y / height)) * size,
          depth: (dot([x, y, z], forward) / (radius * 2) + 0.5),
          index,
        };
      });
      const normal = normalize([mesh.normals[projected[0].index * 3], mesh.normals[projected[0].index * 3 + 1], mesh.normals[projected[0].index * 3 + 2]]);
      const facing = Math.abs(dot(normal, forward));
      const shade = 0.45 + 0.55 * Math.max(0, dot(normal, light)) * (0.55 + 0.45 * facing);
      const c0 = mesh.colors[projected[0].index * 4], c1 = mesh.colors[projected[0].index * 4 + 1], c2 = mesh.colors[projected[0].index * 4 + 2];
      const minX = Math.max(0, Math.floor(Math.min(...projected.map((p) => p.sx))));
      const maxX = Math.min(size - 1, Math.ceil(Math.max(...projected.map((p) => p.sx))));
      const minY = Math.max(0, Math.floor(Math.min(...projected.map((p) => p.sy))));
      const maxY = Math.min(size - 1, Math.ceil(Math.max(...projected.map((p) => p.sy))));
      const area = (projected[1].sx - projected[0].sx) * (projected[2].sy - projected[0].sy)
        - (projected[2].sx - projected[0].sx) * (projected[1].sy - projected[0].sy);
      if (Math.abs(area) < 1e-6) continue;
      for (let y = minY; y <= maxY; ++y) {
        for (let x = minX; x <= maxX; ++x) {
          const px = x + 0.5, py = y + 0.5;
          const w0 = ((projected[1].sx - px) * (projected[2].sy - py) - (projected[2].sx - px) * (projected[1].sy - py)) / area;
          const w1 = ((projected[2].sx - px) * (projected[0].sy - py) - (projected[0].sx - px) * (projected[2].sy - py)) / area;
          const w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          const z = w0 * projected[0].depth + w1 * projected[1].depth + w2 * projected[2].depth;
          const texel = (view * size + y) * size + x;
          if (z >= depth[texel]) continue;
          depth[texel] = z;
          const offset = texel * 4;
          atlas[offset] = Math.min(255, Math.round(c0 * shade));
          atlas[offset + 1] = Math.min(255, Math.round(c1 * shade));
          atlas[offset + 2] = Math.min(255, Math.round(c2 * shade));
          atlas[offset + 3] = 255;
        }
      }
    }
  }
  // Coverage dilate: bilinear sampling at an impostor edge otherwise produces a
  // dark halo because the neighbouring texel is (0,0,0,0) rather than background.
  for (let view = 0; view < views; ++view) {
    for (let pass = 0; pass < 2; ++pass) {
      const copy = atlas.slice();
      for (let y = 1; y < size - 1; ++y) for (let x = 1; x < size - 1; ++x) {
        const offset = ((view * size + y) * size + x) * 4;
        if (copy[offset + 3] !== 0) continue;
        let r = 0, g = 0, b = 0, n = 0;
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const neighbour = ((view * size + y + dy) * size + x + dx) * 4;
          if (copy[neighbour + 3] === 0) continue;
          r += copy[neighbour]; g += copy[neighbour + 1]; b += copy[neighbour + 2]; ++n;
        }
        if (n === 0) continue;
        atlas[offset] = Math.round(r / n); atlas[offset + 1] = Math.round(g / n); atlas[offset + 2] = Math.round(b / n);
      }
    }
  }
  return { atlas, depth, views, size };
}

// ------------------------------------------------------------------- validation
function validateSpecies(spec) {
  keys(spec, ['schema', 'id', 'kind', 'profile', 'seed', 'dimensions', 'lod', 'wind', 'materials', 'budget', 'impostor'], ['textures', 'generators', 'components', 'stage'], 'species');
  requireValue(spec.schema === SPECIES_SCHEMA, `unsupported species schema ${spec.schema}`);
  // A generator graph is optional: without one the built-in recipe runs, which is what every
  // package cooked before the editor existed was built from.
  if (spec.generators !== undefined) normalizeGenerators(spec);
  // Components are optional in exactly the same way: a source without them takes the original
  // code path and produces the original bytes.
  if (spec.components !== undefined) normalizeComponents(spec);
  if (spec.stage !== undefined) requireValue(typeof spec.stage === 'string' && spec.stage.length <= 32, 'species.stage must be a short string');
  requireValue(typeof spec.id === 'string' && ID.test(spec.id), 'species.id is not a valid identifier');
  requireValue(SPECIES_KINDS.has(spec.kind), 'species.kind must be tree, shrub or grass');
  requireValue(SPECIES_PROFILES.has(spec.profile), 'species.profile must be broadleaf, conifer, shrub or grass');
  integer(spec.seed, 0, 0xFFFFFFFF, 'species.seed');
  keys(spec.dimensions, ['heightMetres', 'crownRadiusMetres', 'trunkRadiusMetres'], ['leafWidthMetres', 'leafHeightMetres', 'bladesPerClump'], 'dimensions');
  scalar(spec.dimensions.heightMetres, 0.05, 120, 'dimensions.heightMetres');
  scalar(spec.dimensions.crownRadiusMetres, 0.02, 40, 'dimensions.crownRadiusMetres');
  scalar(spec.dimensions.trunkRadiusMetres, 0.001, 4, 'dimensions.trunkRadiusMetres');
  if (spec.dimensions.leafWidthMetres !== undefined) scalar(spec.dimensions.leafWidthMetres, 0.01, 4, 'dimensions.leafWidthMetres');
  if (spec.dimensions.leafHeightMetres !== undefined) scalar(spec.dimensions.leafHeightMetres, 0.01, 4, 'dimensions.leafHeightMetres');
  if (spec.dimensions.bladesPerClump !== undefined) integer(spec.dimensions.bladesPerClump, 1, 512, 'dimensions.bladesPerClump');
  requireValue(spec.kind !== 'grass' || spec.dimensions.bladesPerClump !== undefined, 'grass species need dimensions.bladesPerClump');
  keys(spec.lod, ['levels', 'screenErrorPixels'], [], 'lod');
  integer(spec.lod.levels, 1, 4, 'lod.levels');
  requireValue(Array.isArray(spec.lod.screenErrorPixels) && spec.lod.screenErrorPixels.length === spec.lod.levels,
    'lod.screenErrorPixels must describe every level');
  spec.lod.screenErrorPixels.forEach((value, index) => scalar(value, 0, 64, `lod.screenErrorPixels[${index}]`));
  keys(spec.wind, ['amplitudeMetres', 'frequencyHz', 'stiffness'], [], 'wind');
  scalar(spec.wind.amplitudeMetres, 0, 4, 'wind.amplitudeMetres');
  scalar(spec.wind.frequencyHz, 0, 12, 'wind.frequencyHz');
  scalar(spec.wind.stiffness, 0, 1, 'wind.stiffness');
  requireValue(Array.isArray(spec.materials) && spec.materials.length >= 1 && spec.materials.length <= 8, 'species.materials needs 1–8 entries');
  const ids = new Set();
  for (const material of spec.materials) {
    // The contract validates the quantities; the species layer only checks identity and
    // consistency with the flags the runtime still reads.
    const contract = normalizeMaterial(material, ids.size);
    requireValue(typeof material.id === 'string' && ID.test(material.id), 'material.id is not a valid identifier');
    requireValue(!ids.has(material.id), `duplicate material id ${material.id}`); ids.add(material.id);
    if (material.alphaMasked !== undefined) {
      requireValue(typeof material.alphaMasked === 'boolean', 'material.alphaMasked must be a boolean');
      requireValue(material.alphaMasked === contract.alphaMasked,
        `material ${material.id} declares alphaMasked ${material.alphaMasked} but alphaMode ${contract.alphaMode}`);
    }
    if (material.doubleSided !== undefined) {
      requireValue(typeof material.doubleSided === 'boolean', 'material.doubleSided must be a boolean');
    }
    if (material.transmission !== undefined) vector(material.transmission, 3, 0, 1, 'material.transmission');
  }
  keys(spec.budget, ['maxVertices', 'maxTriangles'], [], 'budget');
  integer(spec.budget.maxVertices, 3, 400000, 'budget.maxVertices');
  integer(spec.budget.maxTriangles, 1, 800000, 'budget.maxTriangles');
  keys(spec.impostor, ['views', 'size'], [], 'impostor');
  integer(spec.impostor.views, 1, 16, 'impostor.views');
  requireValue([32, 64, 128, 256].includes(spec.impostor.size), 'impostor.size must be 32, 64, 128 or 256');
  // LOD0 must be the densest: a later level that adds geometry would make the error
  // table meaningless.
  requireValue(spec.lod.screenErrorPixels.every((value, index) => index === 0 || value > spec.lod.screenErrorPixels[index - 1]),
    'lod.screenErrorPixels must strictly increase');
}

// The reduction ladder only touches what a coarser view can afford to lose: radial
// tessellation and leaf/blade coverage. The skeleton is identical in every LOD, so the
// transition swaps detail instead of swapping the plant.
//
// `components` is a separate knob from `leaves` on purpose. Fruit and flowers are large, few and
// silhouette-carrying; dropping them at the same rate as leaves would make a tree shed its fruit
// one LOD before it sheds its foliage. The fractions below keep a recognisable fruit load at
// LOD1 and a token presence at LOD3.
const QUALITY = [
  { level: 0, radial: 1, leaves: 1, components: 1 },
  { level: 1, radial: 0.75, leaves: 0.62, components: 0.7 },
  { level: 2, radial: 0.6, leaves: 0.28, components: 0.35 },
  { level: 3, radial: 0.5, leaves: 0.1, components: 0.15 },
];

// Texture roles are resolved against the cooked texture library, never by file name: the
// package records the digest of the exact bytes it was cooked against, so replacing a PNG
// without re-cooking a species fails the load instead of silently changing its look.
//
// Slots are named after the material quantity they feed. `albedo` and `mask` are accepted as
// aliases for `baseColor` and `opacity` because older sources used them; `roughness` is
// rejected, because a standalone roughness image was exactly the slot that used to be filled
// with a height map. Roughness now comes from `orm`, where it has a defined channel.
const TEXTURE_SLOTS = ['baseColor', 'normal', 'orm', 'height', 'thickness', 'opacity'];
const TEXTURE_ALIASES = { albedo: 'baseColor', mask: 'opacity' };
const SLOT_SEMANTICS = {
  baseColor: 'baseColor', normal: 'normal', orm: 'orm',
  height: 'height', thickness: 'thickness', opacity: 'opacity',
};

function resolveTextures(spec, library) {
  if (!spec.textures) return null;
  requireValue(library && Array.isArray(library.roles), 'species.textures needs a cooked texture library');
  const byRole = new Map(library.roles.map((entry) => [entry.role, entry]));
  const resolved = { bark: null, leaf: null };
  for (const group of ['bark', 'leaf']) {
    const requested = spec.textures[group];
    if (!requested) continue;
    requireValue(requested.roughness === undefined,
      `textures.${group}.roughness is not a slot: roughness comes from orm (channel G)`);
    keys(requested, [], [...TEXTURE_SLOTS, ...Object.keys(TEXTURE_ALIASES)], `textures.${group}`);
    const entry = { roles: {}, baseColor: null, normal: null, orm: null, height: null, thickness: null, opacity: null };
    for (const [rawSlot, role] of Object.entries(requested)) {
      const slot = TEXTURE_ALIASES[rawSlot] ?? rawSlot;
      requireValue(TEXTURE_SLOTS.includes(slot), `textures.${group}.${rawSlot} is not a texture slot`);
      requireValue(entry[slot] === null, `textures.${group}.${slot} is declared twice`);
      const found = byRole.get(role);
      requireValue(found !== undefined, `textures.${group}.${slot} references unknown role ${role}`);
      // The role must actually be the quantity the slot asks for. This is the check that
      // would have caught a height map bound as roughness.
      requireValue(found.semantic === SLOT_SEMANTICS[slot],
        `textures.${group}.${slot} needs a ${SLOT_SEMANTICS[slot]} role but ${role} is ${found.semantic ?? 'unlabelled'}`);
      entry.roles[slot] = { role, digest: found.digest, semantic: found.semantic, colorSpace: found.colorSpace };
      entry[slot] = found;
    }
    requireValue(entry.baseColor !== null, `textures.${group}.baseColor is required when the group is present`);
    // Every atlas sheet in a group must share the grid, or a card samples one leaf's colour
    // with another leaf's mask.
    const atlasGrid = entry.baseColor.atlas;
    for (const slot of ['normal', 'orm', 'thickness', 'opacity']) {
      const sheet = entry[slot];
      if (!sheet) continue;
      if (atlasGrid) {
        requireValue(sheet.atlas && sheet.atlas.columns === atlasGrid.columns && sheet.atlas.rows === atlasGrid.rows,
          `textures.${group}.${slot} must share the baseColor atlas grid`);
      }
    }
    resolved[group] = entry;
  }
  requireValue(resolved.bark || resolved.leaf, 'species.textures declares no usable group');
  return resolved;
}

// Builds the atlas the leaf placement needs. The grid and pivot come from the cooked role, so
// the editor and the cook cannot disagree about which cell a leaf samples.
function buildLeafAtlas(spec, textures) {
  const sheet = textures?.leaf?.baseColor;
  if (!sheet || !sheet.atlas) return null;
  return buildAtlas({
    columns: sheet.atlas.columns,
    rows: sheet.atlas.rows,
    width: sheet.width,
    height: sheet.height,
    // Version and per-cell records come from the cooked sheet. A source that predates them is
    // migrated as legacy/v1 (bottom-edge pivot), not silently reinterpreted as canonical.
    schemaVersion: sheet.atlas.schemaVersion,
    cells: sheet.atlas.cells,
    pivot: sheet.atlas.pivot,
    padding: sheet.atlas.padding,
    leafWidthMetres: spec.dimensions.leafWidthMetres ?? 0,
    leafHeightMetres: spec.dimensions.leafHeightMetres ?? 0,
  });
}

export function compileSpecies(spec, options = {}) {
  validateSpecies(spec);
  const textures = resolveTextures(spec, options.textures);
  const atlas = buildLeafAtlas(spec, textures);
  // Normalized once and handed to every LOD: the component list is the same declaration at every
  // level, only the survival fraction changes.
  const components = normalizeComponents(spec);
  const componentOptions = { atlas, barkTiling: null, components, stage: spec.stage ?? null, assets: options.assets ?? null };
  const levels = [];
  let dense = null;
  const uv = {
    barkTiling: textures?.bark?.baseColor?.tileMetres ? { tileMetres: textures.bark.baseColor.tileMetres } : null,
    leafAtlas: atlas,
    atlas,
  };
  componentOptions.barkTiling = uv.barkTiling;
  let lastGraph = null;
  let lastLeaves = null;
  let lastPerFrond = null;
  let lastPerComponent = null;
  let lastLeafArea = 0;
  let lastSpacing = null;
  for (let level = 0; level < spec.lod.levels; ++level) {
    let mesh;
    if (spec.kind === 'grass') {
      const built = buildGrassMesh(spec, QUALITY[level], spec.seed, componentOptions);
      mesh = built.mesh;
      if (level === 0) { lastGraph = built.graph; lastLeaves = built.leaves; lastPerFrond = built.perFrond; lastPerComponent = built.perComponent; }
    } else {
      const built = buildStructuredMesh(spec, QUALITY[level], spec.seed, componentOptions);
      mesh = built.mesh;
      // The published structure is the LOD0 one. Reporting the coarsest level's leaf count
      // would understate the tree by the LOD ratio and make the report look better than it is.
      if (level === 0) {
        lastGraph = built.graph; lastLeaves = built.leaves; lastPerFrond = built.perFrond;
        lastPerComponent = built.perComponent;
        lastLeafArea = built.leafAreaMetresSquared; lastSpacing = built.minLeafSpacingMetres;
      }
    }
    const vertices = mesh.positions.length / 3, triangles = mesh.indices.length / 3;
    requireValue(vertices <= spec.budget.maxVertices, `lod${level} exceeds budget.maxVertices`);
    requireValue(triangles <= spec.budget.maxTriangles, `lod${level} exceeds budget.maxTriangles`);
    let bounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
    for (let index = 0; index < vertices; ++index) for (let axis = 0; axis < 3; ++axis) {
      bounds.min[axis] = Math.min(bounds.min[axis], mesh.positions[index * 3 + axis]);
      bounds.max[axis] = Math.max(bounds.max[axis], mesh.positions[index * 3 + axis]);
    }
    const error = level === 0 ? { maximum: 0, p95: 0, samples: 0 } : measureLodError(dense, mesh);
    levels.push({ level, mesh, vertices, triangles, bounds, error });
    if (level === 0) dense = mesh;
  }
  // A coarse LOD that is larger than the dense one is a cook bug, not a trade-off.
  for (let index = 1; index < levels.length; ++index) {
    requireValue(levels[index].triangles <= levels[index - 1].triangles,
      `lod${index} has more triangles than lod${index - 1}`);
    requireValue(Number.isFinite(levels[index].error.maximum), `lod${index} error measurement failed`);
    // Monotonicity is the property that makes the LOD table usable: a coarser level
    // that measures a *smaller* error means the two levels were not built from the
    // same plant, and the runtime would then pick the wrong one.
    requireValue(levels[index].error.maximum >= levels[index - 1].error.maximum * 0.98,
      `lod${index} error ${levels[index].error.maximum} is below lod${index - 1} error ${levels[index - 1].error.maximum}`);
  }

  const impostor = bakeImpostor(levels[Math.min(1, levels.length - 1)].mesh, {
    views: spec.impostor.views, size: spec.impostor.size,
    heightMetres: levels[0].bounds.max[1] - levels[0].bounds.min[1],
    radiusMetres: Math.max(0.05, Math.max(
      Math.abs(levels[0].bounds.max[0]), Math.abs(levels[0].bounds.min[0]),
      Math.abs(levels[0].bounds.max[2]), Math.abs(levels[0].bounds.min[2]))),
  });

  const sections = [];
  for (const level of levels) {
    sections.push({ kind: SectionKind.POSITION_F32X3, index: level.level, elements: level.mesh.positions });
    sections.push({ kind: SectionKind.NORMAL_F32X3, index: level.level, elements: level.mesh.normals });
    sections.push({ kind: SectionKind.UV_F32X2, index: level.level, elements: level.mesh.uvs });
    sections.push({ kind: SectionKind.COLOR_U8X4, index: level.level, elements: level.mesh.colors });
    sections.push({ kind: SectionKind.INDEX_U32, index: level.level, elements: level.mesh.indices });
    sections.push({
      kind: SectionKind.WIND_VERTEX, index: level.level,
      elements: Float32Array.from(level.mesh.weights, (weight) => weight),
    });
  }
  const primitives = [];
  for (const level of levels) for (const primitive of level.mesh.primitives) {
    primitives.push({ material: spec.materials.findIndex((material) => material.id === primitive.material), lod: level.level, firstIndex: primitive.firstIndex, indexCount: primitive.indexCount });
  }
  const { buffer: primitiveBytes } = packRecords(primitives, [
    { name: 'material', type: 'u16', bytes: 2 }, { name: 'lod', type: 'u16', bytes: 2 },
    { name: 'firstIndex', type: 'u32', bytes: 4 }, { name: 'indexCount', type: 'u32', bytes: 4 },
  ]);
  sections.push({ kind: SectionKind.PRIMITIVE, index: 0, data: primitiveBytes });

  const lodRecords = levels.map((level) => ({
    level: level.level, vertices: level.vertices, triangles: level.triangles,
    firstPrimitive: 0, primitiveCount: 0,
    worldErrorMetres: level.error.maximum, errorP95Metres: level.error.p95,
    screenErrorPixels: spec.lod.screenErrorPixels[level.level],
    heightMetres: level.bounds.max[1] - level.bounds.min[1],
    radiusMetres: Math.max(...level.bounds.max.map(Math.abs), ...level.bounds.min.map(Math.abs)),
  }));
  // Area goes into the JSON metadata, not into the packed record: the record layout is the native
  // ABI, and widening it to carry a reporting number would force the C++ reader to change in
  // lockstep for no runtime benefit. Tools, the report and the editor all read `meta`.
  const baseArea = surfaceArea(levels[0].mesh);
  for (const record of lodRecords) {
    const area = surfaceArea(levels[record.level].mesh);
    record.areaMetresSquared = Number(area.toFixed(4));
    record.areaRetention = baseArea > 0 ? Number((area / baseArea).toFixed(4)) : 1;
  }
  const { buffer: lodBytes } = packRecords(lodRecords, [
    { name: 'level', type: 'u32', bytes: 4 }, { name: 'vertices', type: 'u32', bytes: 4 },
    { name: 'triangles', type: 'u32', bytes: 4 }, { name: 'firstPrimitive', type: 'u32', bytes: 4 },
    { name: 'primitiveCount', type: 'u32', bytes: 4 }, { name: 'worldErrorMetres', type: 'f32', bytes: 4 },
    { name: 'errorP95Metres', type: 'f32', bytes: 4 }, { name: 'screenErrorPixels', type: 'f32', bytes: 4 },
    { name: 'heightMetres', type: 'f32', bytes: 4 }, { name: 'radiusMetres', type: 'f32', bytes: 4 },
  ]);
  sections.push({ kind: SectionKind.LOD, index: 0, data: lodBytes });

  // Bounds are the union over every LOD, not LOD0's box. A coarser LOD samples the same rings at
  // different angles, so a vertex can sit a centimetre or two further along an axis than any LOD0
  // vertex does — and a culler that trusts a box one branch pokes out of drops that branch from
  // the visible list. The measured overshoot on oak is 1.8 cm in Z, which is enough to fail the
  // native "every vertex inside species bounds" assertion.
  const overallBounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
  for (const level of levels) {
    for (let axis = 0; axis < 3; ++axis) {
      overallBounds.min[axis] = Math.min(overallBounds.min[axis], level.bounds.min[axis]);
      overallBounds.max[axis] = Math.max(overallBounds.max[axis], level.bounds.max[axis]);
    }
  }

  // Shadow proxy: one ellipsoid per structural level. It is deliberately not the
  // render mesh — a proxy that tracks every leaf would cost the same as drawing it.
  const centreX = (overallBounds.min[0] + overallBounds.max[0]) / 2;
  const centreY = (overallBounds.min[1] + overallBounds.max[1]) / 2;
  const centreZ = (overallBounds.min[2] + overallBounds.max[2]) / 2;
  const extentX = (overallBounds.max[0] - overallBounds.min[0]) / 2;
  const extentY = (overallBounds.max[1] - overallBounds.min[1]) / 2;
  const extentZ = (overallBounds.max[2] - overallBounds.min[2]) / 2;
  // Flat scalar fields, because packRecords writes one number per named field. An
  // array here would silently write NaN and produce a proxy nothing can shadow with.
  const proxy = [{ cx: centreX, cy: centreY, cz: centreZ, ex: extentX, ey: extentY, ez: extentZ }];
  const { buffer: proxyBytes } = packRecords(proxy, [
    { name: 'cx', type: 'f32', bytes: 4 }, { name: 'cy', type: 'f32', bytes: 4 }, { name: 'cz', type: 'f32', bytes: 4 },
    { name: 'ex', type: 'f32', bytes: 4 }, { name: 'ey', type: 'f32', bytes: 4 }, { name: 'ez', type: 'f32', bytes: 4 },
  ]);
  sections.push({ kind: SectionKind.SHADOW_PROXY, index: 0, data: proxyBytes });

  const collision = [{ ax: 0, ay: 0, az: 0, bx: 0, by: overallBounds.max[1] * 0.72, bz: 0, radius: spec.dimensions.trunkRadiusMetres, flags: spec.kind === 'grass' ? 0 : 1 }];
  const { buffer: collisionBytes } = packRecords(collision, [
    { name: 'ax', type: 'f32', bytes: 4 }, { name: 'ay', type: 'f32', bytes: 4 }, { name: 'az', type: 'f32', bytes: 4 },
    { name: 'bx', type: 'f32', bytes: 4 }, { name: 'by', type: 'f32', bytes: 4 }, { name: 'bz', type: 'f32', bytes: 4 },
    { name: 'radius', type: 'f32', bytes: 4 }, { name: 'flags', type: 'u32', bytes: 4 },
  ]);
  sections.push({ kind: SectionKind.COLLISION, index: 0, data: collisionBytes });

  sections.push({ kind: SectionKind.IMPOSTOR_RGBA8, index: 0, elements: impostor.atlas });
  sections.push({ kind: SectionKind.IMPOSTOR_DEPTH_F32, index: 0, elements: impostor.depth });

  const materials = spec.materials.map((material, index) => normalizeMaterial(material, index));
  const meta = {
    schema: SPECIES_COOK_SCHEMA,
    id: spec.id, kind: spec.kind, profile: spec.profile, seed: spec.seed,
    units: 'metres',
    sourceDigest: sha256Hex(canonicalJson(spec)),
    representations: ['mesh', 'impostor', 'shadowProxy', 'collision'],
    bounds: overallBounds,
    lod: lodRecords,
    materials,
    wind: { ...spec.wind, model: 'boundedProcedural', vertexWeights: true },
    impostor: { views: impostor.views, size: impostor.size, atlasBytes: impostor.atlas.length, depthBytes: impostor.depth.byteLength },
    primitiveCount: primitives.length,
    budget: spec.budget,
    // Structural report: branch count, depth, measured attachment gap. The cook publishes
    // these so a package can be audited for "is the tree actually connected" without
    // re-running the generator.
    structure: lastGraph ? {
      ...lastGraph.stats,
      // Whether the crown the generator grew agrees with the authored bound. Published so the
      // bake result and the editor can surface "this declaration is not what the tree does"
      // instead of only a report file knowing about it.
      crown: lastGraph.crownRadiusMetres > 0 ? {
        declared: lastGraph.crownRadiusMetres,
        measured: lastGraph.stats.crownRadialMetres,
        shape: lastGraph.stats.shapeCrownRadiusMetres,
        fraction: lastGraph.stats.crownFractionOfAuthored,
        overshoot: lastGraph.stats.crownOvershootFraction,
        shapeFraction: lastGraph.stats.crownFractionOfShape,
        mismatch: Math.abs((lastGraph.stats.crownFractionOfAuthored ?? 1) - 1) > 0.15,
      } : null,
      leaves: lastLeaves ? lastLeaves.length : 0,
      foliage: spec.kind === 'grass' ? null : foliageSummary(lastGraph, lastPerFrond, lastLeafArea, lastSpacing),
      // Per-generator counts, so "does every frond actually produce foliage" is answerable from
      // the package instead of from a screenshot. A grass clump reports its blade count under
      // `blade`, because its blades are the geometry rather than foliage on top of it.
      fronds: lastPerFrond ? Object.fromEntries(lastPerFrond.map((entry) => [entry.id, entry.leaves])) : {},
      // Only present when the source declares parts. A species without them keeps the exact meta
      // it had before this layer existed, so no existing package digest moves.
      ...(lastPerComponent && lastPerComponent.length ? {
        components: Object.fromEntries(lastPerComponent.map((entry) => [entry.id, {
          type: entry.type, parts: entry.parts, hosts: entry.hosts, triangles: entry.triangles,
          skippedStage: entry.skippedStage, skippedLod: entry.skippedLod,
        }])),
        stage: spec.stage ?? null,
      } : {}),
    } : null,
    // Only roles that were actually resolved are declared. A renderer that finds no
    // texture block keeps using vertex colour, which is what the fixtures without
    // textures still do.
    textures: textures ? {
      bark: textures.bark ? { roles: textures.bark.roles, tileMetres: textures.bark.baseColor.tileMetres ?? null } : null,
      leaf: textures.leaf ? {
        roles: textures.leaf.roles,
        atlas: textures.leaf.baseColor.atlas ?? null,
        // Cell rects and pivots are baked into the package so the runtime never has to
        // re-derive a grid from an image size. `coordinatesVersion` states which pivot contract
        // the rects follow; content bounds are null on a legacy sheet and a real rect otherwise.
        cells: atlas ? atlas.cells.map((cell) => ({
          index: cell.index, u0: cell.u0, v0: cell.v0, du: cell.du, dv: cell.dv,
          pivotU: cell.pivotU, pivotV: cell.pivotV,
          contentU0: cell.contentU0, contentV0: cell.contentV0,
          contentU1: cell.contentU1, contentV1: cell.contentV1,
          leafProfile: cell.leafProfile, sourceId: cell.sourceId, damageVariant: cell.damageVariant,
        })) : null,
        coordinatesVersion: atlas ? atlas.coordinatesVersion : null,
        atlasSchemaVersion: atlas ? atlas.schemaVersion : null,
        tileMetres: null,
      } : null,
      bindings: describeTextureBindings(textures),
    } : null,
  };
  const bytes = buildPackage({ schema: SPECIES_COOK_SCHEMA, meta, sections });
  return {
    id: spec.id, meta, bytes, hash: digestPackage(bytes),
    vertices: levels[0].vertices, triangles: levels[0].triangles,
    lodTriangles: levels.map((level) => level.triangles),
    lodErrorMetres: levels.map((level) => level.error.maximum),
    bytesTotal: bytes.byteLength,
  };
}

// --------------------------------------------------------------------- editor preview
// The editor needs the same geometry the cook would produce, without the package. Packing
// costs a hash over every section and produces bytes the viewport has no use for, so this
// entry point stops at the mesh and reports the numbers the inspector shows.
//
// `level` selects one LOD; `analysis` additionally builds every level to measure the error
// table, which is the expensive part and is therefore opt-in.
export function buildPreview(spec, options = {}) {
  validateSpecies(spec);
  const level = Math.round(Number(options.level ?? 0));
  requireValue(level >= 0 && level < spec.lod.levels, `preview level ${level} is not in the LOD table`);
  const quality = QUALITY[level];
  // The atlas may arrive already built (from the cooked library) or as a raw grid. Building
  // it here as well keeps the preview identical to the cook when the editor has no library.
  const atlas = options.atlas ?? (options.leafAtlas
    ? buildAtlas({
      ...options.leafAtlas,
      width: options.leafAtlas.width ?? 0,
      height: options.leafAtlas.height ?? 0,
      leafWidthMetres: spec.dimensions.leafWidthMetres ?? 0,
      leafHeightMetres: spec.dimensions.leafHeightMetres ?? 0,
    })
    : null);
  const uv = {
    barkTiling: options.barkTileMetres ? { tileMetres: Number(options.barkTileMetres) } : null,
    leafAtlas: atlas,
    atlas,
  };
  let mesh, structure = null, leaves = null;
  // The preview reads the same component declarations as the cook, so the editor's structure
  // panel and the baked package cannot disagree about how many fruit a species carries.
  const componentOptions = {
    atlas, barkTiling: uv.barkTiling,
    components: options.components ?? normalizeComponents(spec),
    stage: options.stage ?? spec.stage ?? null,
  };
  if (spec.kind === 'grass') {
    const built = buildGrassMesh(spec, quality, spec.seed, componentOptions);
    mesh = built.mesh;
    structure = { ...built.graph.stats, fronds: built.perFrond, components: built.perComponent };
    leaves = built.leaves.length;
  } else {
    const built = buildStructuredMesh(spec, quality, spec.seed, componentOptions);
    mesh = built.mesh;
    structure = {
      ...built.graph.stats,
      fronds: built.perFrond,
      components: built.perComponent,
      foliage: foliageSummary(built.graph, built.perFrond, built.leafAreaMetresSquared, built.minLeafSpacingMetres),
    };
    leaves = built.leaves.length;
  }
  const vertices = mesh.positions.length / 3, triangles = mesh.indices.length / 3;
  let bounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
  for (let index = 0; index < vertices; ++index) for (let axis = 0; axis < 3; ++axis) {
    const value = mesh.positions[index * 3 + axis];
    bounds.min[axis] = Math.min(bounds.min[axis], value);
    bounds.max[axis] = Math.max(bounds.max[axis], value);
  }
  const result = {
    id: spec.id,
    level,
    levels: spec.lod.levels,
    vertices,
    triangles,
    bounds,
    // The budget is a real constraint: the editor shows the delta instead of failing at
    // bake time with no explanation.
    budget: spec.budget,
    overBudget: vertices > spec.budget.maxVertices || triangles > spec.budget.maxTriangles,
    // The builder opens one primitive per tube and per card, which is what the cook wants
    // (it can address a single card). A viewport cannot: nine thousand draw calls per frame
    // is not a preview. Adjacent ranges that share a material are one range here.
    primitives: mergePrimitives(mesh.primitives.map((primitive) => {
      const material = Math.max(0, spec.materials.findIndex((entry) => entry.id === primitive.material));
      return {
        material,
        // Shading needs to know "leaf or wood", not "which index": grass has a blade material
        // at index 0, and an index-based test would shade it as bark.
        leaf: spec.materials[material]?.kind === 'bark' ? 0 : 1,
        firstIndex: primitive.firstIndex,
        indexCount: primitive.indexCount,
      };
    })),
    geometry: {
      positions: mesh.positions, normals: mesh.normals, uvs: mesh.uvs,
      windWeights: mesh.weights, colors: mesh.colors, indices: mesh.indices,
    },
    // What the editor's structure panel shows: branch count, depth, measured attachment gap,
    // leaf count. A preview that cannot report these is not a preview of this pipeline.
    structure,
    leaves,
    foliage: structure?.foliage ?? null,
    atlas: atlas ? { columns: atlas.columns, rows: atlas.rows, pivot: atlas.pivot, cells: atlas.cells.length } : null,
  };
  if (options.analysis) {
    const table = [];
    let dense = null;
    for (let index = 0; index < spec.lod.levels; ++index) {
      const built = index === level ? mesh : (spec.kind === 'grass'
        ? buildGrassMesh(spec, QUALITY[index], spec.seed).mesh
        : buildStructuredMesh(spec, QUALITY[index], spec.seed, { atlas, barkTiling: uv.barkTiling }).mesh);
      const error = index === 0 ? { maximum: 0, p95: 0 } : measureLodError(dense, built);
      // Area retention travels with the error row so the editor's LOD panel can show both: a level
      // can look cheap by distance while having thrown away a third of the crown.
      const referenceArea = index === 0 ? surfaceArea(built) : table[0].areaMetresSquared;
      const area = surfaceArea(built);
      table.push({
        level: index,
        vertices: built.positions.length / 3,
        triangles: built.indices.length / 3,
        worldErrorMetres: error.maximum,
        errorP95Metres: error.p95,
        screenErrorPixels: spec.lod.screenErrorPixels[index],
        areaMetresSquared: Number(area.toFixed(4)),
        areaRetention: referenceArea > 0 ? Number((area / referenceArea).toFixed(4)) : 1,
      });
      if (index === 0) dense = built;
    }
    result.lodTable = table;
  }
  return result;
}

// Coalesces contiguous index ranges that share a material. The cook keeps one range per tube
// and per card so a single leaf can be addressed; a preview draws by material, and nine
// thousand draw calls is not a preview.
function mergePrimitives(primitives) {
  const merged = [];
  for (const primitive of primitives) {
    const last = merged[merged.length - 1];
    if (last && last.material === primitive.material && last.firstIndex + last.indexCount === primitive.firstIndex) {
      last.indexCount += primitive.indexCount;
      continue;
    }
    merged.push({ ...primitive });
  }
  return merged;
}

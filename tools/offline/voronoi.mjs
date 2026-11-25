#!/usr/bin/env node
// Offline convex Voronoi bake for destructible props.
//
// Splits an axis-aligned box into convex Voronoi cells and emits, per cell:
// vertices, faces, volume, centroid, mass-proxy AABB, and per-pair bond areas.
// Everything is seeded and order-stable, so the same inputs produce byte-identical
// JSON. This is the "4a" stage of .developmentAgent/simulation-effects/PHYSICS-SCALE-PLAN.md: it does not touch the
// renderer or the runtime yet, it produces the asset contract they will consume.
//
// Usage:
//   node tools/offline/voronoi.mjs bake --half 1,1,1 --cells 24 --seed 7 --out build/voronoi/box.json
//   node tools/offline/voronoi.mjs verify --in build/voronoi/box.json

import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const EPS = 1e-7;
const PLANE_EPS = 1e-6;

export const SCHEMA = 'nativegpu.voronoi/1';

function rng(seed) {
  let state = (seed >>> 0) || 1;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
const length = (a) => Math.sqrt(dot(a, a));
function normalize(a) {
  const l = length(a);
  return l > EPS ? scale(a, 1 / l) : [0, 0, 0];
}
const round = (v) => Math.round(v * 1e6) / 1e6;

// Solve the 3x3 system of three plane equations n_i . x = d_i.
function intersectPlanes(p0, p1, p2) {
  const n1 = p0.normal, n2 = p1.normal, n3 = p2.normal;
  const c23 = cross(n2, n3);
  const det = dot(n1, c23);
  if (Math.abs(det) < 1e-9) return null;
  const c31 = cross(n3, n1);
  const c12 = cross(n1, n2);
  return scale(add(add(scale(c23, p0.offset), scale(c31, p1.offset)),
                   scale(c12, p2.offset)), 1 / det);
}


// Symmetric 3x3 eigen-decomposition by cyclic Jacobi rotations. Deterministic, and
// small enough that pulling in a linear algebra dependency would not be worth it.
function symmetricEigen(matrix) {
  const a = matrix.map((row) => row.slice());
  let v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  for (let sweep = 0; sweep < 24; ++sweep) {
    let off = 0;
    for (let i = 0; i < 3; ++i) {
      for (let j = i + 1; j < 3; ++j) off += a[i][j] * a[i][j];
    }
    if (off < 1e-14) break;
    for (let p = 0; p < 3; ++p) {
      for (let q = p + 1; q < 3; ++q) {
        if (Math.abs(a[p][q]) < 1e-15) continue;
        const theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const t = Math.sign(theta || 1) / (Math.abs(theta) + Math.sqrt(theta * theta + 1));
        const c = 1 / Math.sqrt(t * t + 1);
        const s = t * c;
        for (let k = 0; k < 3; ++k) {
          const akp = a[k][p];
          const akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (let k = 0; k < 3; ++k) {
          const apk = a[p][k];
          const aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (let k = 0; k < 3; ++k) {
          const vkp = v[k][p];
          const vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  const axes = [0, 1, 2].map((column) => normalize([v[0][column], v[1][column], v[2][column]]));
  return axes;
}

// Best-fit oriented box: covariance axes of the vertices, then the tightest extents
// along those axes. Used as the physics proxy until convex narrowphase exists.
function orientedBounds(vertices, centroid) {
  const mean = scale(vertices.reduce((acc, p) => add(acc, p), [0, 0, 0]), 1 / vertices.length);
  let cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
  for (const p of vertices) {
    const d = sub(p, mean);
    cxx += d[0] * d[0];
    cxy += d[0] * d[1];
    cxz += d[0] * d[2];
    cyy += d[1] * d[1];
    cyz += d[1] * d[2];
    czz += d[2] * d[2];
  }
  const axes = symmetricEigen([[cxx, cxy, cxz], [cxy, cyy, cyz], [cxz, cyz, czz]]);
  const lo = [Infinity, Infinity, Infinity];
  const hi = [-Infinity, -Infinity, -Infinity];
  for (const p of vertices) {
    for (let axis = 0; axis < 3; ++axis) {
      const d = dot(sub(p, mean), axes[axis]);
      lo[axis] = Math.min(lo[axis], d);
      hi[axis] = Math.max(hi[axis], d);
    }
  }
  const center = add(mean, add(scale(axes[0], (lo[0] + hi[0]) * 0.5),
                               add(scale(axes[1], (lo[1] + hi[1]) * 0.5),
                                   scale(axes[2], (lo[2] + hi[2]) * 0.5))));
  return {
    center: center.map(round),
    axes: axes.map((axis) => axis.map(round)),
    half: [0, 1, 2].map((axis) => round(Math.max(1e-3, (hi[axis] - lo[axis]) * 0.5))),
  };
}

function boxPlanes(half) {
  return [
    { normal: [1, 0, 0], offset: half[0] },
    { normal: [-1, 0, 0], offset: half[0] },
    { normal: [0, 1, 0], offset: half[1] },
    { normal: [0, -1, 0], offset: half[1] },
    { normal: [0, 0, 1], offset: half[2] },
    { normal: [0, 0, -1], offset: half[2] },
  ];
}

function insideAll(point, planes) {
  for (const plane of planes) {
    if (dot(plane.normal, point) > plane.offset + 1e-6) return false;
  }
  return true;
}

function planeBasis(normal) {
  const helper = Math.abs(normal[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  const u = normalize(cross(normal, helper));
  const v = normalize(cross(normal, u));
  return [u, v];
}

function buildCell(seedPoint, otherSeeds, half) {
  const planes = boxPlanes(half);
  for (const other of otherSeeds) {
    const normal = normalize(sub(other, seedPoint));
    if (length(normal) < EPS) continue;
    const midpoint = scale(add(seedPoint, other), 0.5);
    planes.push({ normal, offset: dot(normal, midpoint), bisector: other });
  }

  // Vertices are plane triples that survive every other half-space.
  const vertices = [];
  for (let i = 0; i < planes.length; ++i) {
    for (let j = i + 1; j < planes.length; ++j) {
      for (let k = j + 1; k < planes.length; ++k) {
        const point = intersectPlanes(planes[i], planes[j], planes[k]);
        if (!point) continue;
        if (!insideAll(point, planes)) continue;
        let duplicate = false;
        for (const existing of vertices) {
          if (length(sub(existing, point)) < 1e-5) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate) vertices.push(point);
      }
    }
  }
  if (vertices.length < 4) return null;

  const faces = [];
  for (const plane of planes) {
    const onPlane = vertices.filter(
      (v) => Math.abs(dot(plane.normal, v) - plane.offset) < 1e-5);
    if (onPlane.length < 3) continue;
    const [u, v] = planeBasis(plane.normal);
    const center = onPlane.reduce((acc, p) => add(acc, p), [0, 0, 0]);
    const c = scale(center, 1 / onPlane.length);
    const sorted = onPlane
      .map((p) => {
        const d = sub(p, c);
        return { p, angle: Math.atan2(dot(d, v), dot(d, u)) };
      })
      .sort((a, b) => a.angle - b.angle)
      .map((entry) => entry.p);
    // Faces are wound counter-clockwise around the outward normal.
    let area = 0;
    for (let i = 1; i + 1 < sorted.length; ++i) {
      const ab = sub(sorted[i], sorted[0]);
      const ac = sub(sorted[i + 1], sorted[0]);
      area += length(cross(ab, ac)) * 0.5;
    }
    faces.push({ normal: plane.normal, offset: plane.offset, points: sorted, area });
  }

  // Volume and centroid through the divergence theorem over the oriented surface.
  let volume = 0;
  let centroid = [0, 0, 0];
  for (const face of faces) {
    for (let i = 1; i + 1 < face.points.length; ++i) {
      const a = face.points[0];
      const b = face.points[i];
      const c = face.points[i + 1];
      const tetra = dot(a, cross(b, c)) / 6;
      volume += tetra;
      centroid = add(centroid, scale(add(add(a, b), c), tetra / 4));
    }
  }
  if (volume < 0) {
    volume = -volume;
    centroid = scale(centroid, -1);
  }
  if (volume <= 1e-9) return null;
  centroid = scale(centroid, 1 / volume);

  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const v of vertices) {
    for (let axis = 0; axis < 3; ++axis) {
      min[axis] = Math.min(min[axis], v[axis]);
      max[axis] = Math.max(max[axis], v[axis]);
    }
  }

  return {
    seed: seedPoint,
    vertices,
    faces: faces.map((face) => ({
      normal: face.normal.map(round),
      offset: round(face.offset),
      indices: face.points.map((p) => vertices.findIndex((v) => length(sub(v, p)) < 1e-9)),
      area: round(face.area),
    })),
    volume: round(volume),
    centroid: centroid.map(round),
    bounds: { min: min.map(round), max: max.map(round) },
    // Oriented proxy used by the OBB physics path until convex narrowphase (4c).
    obb: orientedBounds(vertices, centroid),
  };
}

export function bakeVoronoi({ half = [1, 1, 1], cells = 24, seed = 1 } = {}) {
  const count = Math.max(2, Math.min(256, Math.trunc(cells)));
  const random = rng(seed);
  const seeds = [];
  for (let i = 0; i < count; ++i) {
    seeds.push([
      round((random() * 2 - 1) * half[0] * 0.92),
      round((random() * 2 - 1) * half[1] * 0.92),
      round((random() * 2 - 1) * half[2] * 0.92),
    ]);
  }

  const baked = [];
  for (let i = 0; i < seeds.length; ++i) {
    const others = seeds.filter((_, index) => index !== i);
    const cell = buildCell(seeds[i], others, half);
    if (cell) baked.push({ index: baked.length, sourceSeed: i, ...cell });
  }

  // Bonds: a pair is adjacent when both cells own a face on their shared bisector.
  const bonds = [];
  for (let a = 0; a < baked.length; ++a) {
    for (let b = a + 1; b < baked.length; ++b) {
      const normal = normalize(sub(baked[b].seed, baked[a].seed));
      if (length(normal) < EPS) continue;
      const offset = dot(normal, scale(add(baked[a].seed, baked[b].seed), 0.5));
      const faceA = baked[a].faces.find(
        (face) => Math.abs(dot(face.normal, normal) - 1) < 1e-4 &&
                  Math.abs(face.offset - offset) < 1e-4);
      if (!faceA) continue;
      const faceB = baked[b].faces.find(
        (face) => Math.abs(dot(face.normal, scale(normal, -1)) - 1) < 1e-4 &&
                  Math.abs(face.offset + offset) < 1e-4);
      if (!faceB) continue;
      bonds.push({
        a,
        b,
        area: round(Math.min(faceA.area, faceB.area)),
      });
    }
  }

  const totalVolume = baked.reduce((sum, cell) => sum + cell.volume, 0);
  return {
    schema: SCHEMA,
    seed: seed >>> 0,
    half: half.map(round),
    cells: baked,
    bonds,
    totals: {
      cells: baked.length,
      bonds: bonds.length,
      volume: round(totalVolume),
      boxVolume: round(8 * half[0] * half[1] * half[2]),
    },
  };
}

export function validatePackage(pkg) {
  const errors = [];
  if (!pkg || pkg.schema !== SCHEMA) errors.push('schema mismatch');
  if (!Array.isArray(pkg?.cells) || pkg.cells.length === 0) errors.push('no cells');
  if (!Array.isArray(pkg?.bonds)) errors.push('no bonds array');
  const boxVolume = 8 * pkg.half[0] * pkg.half[1] * pkg.half[2];
  const total = pkg.totals?.volume ?? 0;
  if (Math.abs(total - boxVolume) > boxVolume * 0.005) {
    errors.push(`volume sum ${total} differs from box volume ${boxVolume}`);
  }
  for (const cell of pkg.cells ?? []) {
    if (!(cell.volume > 0)) errors.push(`cell ${cell.index} has no volume`);
    if (!cell.vertices || cell.vertices.length < 4) errors.push(`cell ${cell.index} degenerate`);
    for (const v of cell.vertices ?? []) {
      for (let axis = 0; axis < 3; ++axis) {
        if (Math.abs(v[axis]) > pkg.half[axis] + 1e-4) {
          errors.push(`cell ${cell.index} vertex outside bounds`);
          axis = 3;
          break;
        }
      }
    }
  }
  for (const bond of pkg.bonds ?? []) {
    if (!(bond.area > 0)) errors.push(`bond ${bond.a}-${bond.b} has no area`);
    if (!(bond.a < bond.b)) errors.push(`bond ${bond.a}-${bond.b} is not ordered`);
  }
  return errors;
}

function parseArgs(argv) {
  const args = { mode: argv[0] };
  for (let i = 1; i < argv.length; i += 2) {
    args[argv[i].replace(/^--/, '')] = argv[i + 1];
  }
  return args;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.mode === 'bake') {
    const half = (args.half ?? '1,1,1').split(',').map(Number);
    const pkg = bakeVoronoi({
      half,
      cells: Number(args.cells ?? 24),
      seed: Number(args.seed ?? 1),
    });
    const errors = validatePackage(pkg);
    if (errors.length) {
      console.error(JSON.stringify({ ok: false, errors }, null, 2));
      process.exit(1);
    }
    const out = resolve(args.out ?? 'build/voronoi/box.json');
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, `${JSON.stringify(pkg, null, 2)}\n`);
    console.log(JSON.stringify({
      ok: true,
      out,
      cells: pkg.totals.cells,
      bonds: pkg.totals.bonds,
      volume: pkg.totals.volume,
    }, null, 2));
    return;
  }
  if (args.mode === 'verify') {
    const pkg = JSON.parse(readFileSync(resolve(args.in), 'utf8'));
    const errors = validatePackage(pkg);
    console.log(JSON.stringify({ ok: errors.length === 0, errors }, null, 2));
    process.exit(errors.length ? 1 : 0);
  }
  if (args.mode === 'bake-scene') {
    // Re-bakes every destructible.voronoi reference in a scene from the entity's own
    // size, so changing seed/cells is a one-command loop instead of hand-editing
    // packages. Sizes come from the scene, which is why this cannot live in the loader.
    if (!args.scene) {
      console.error('bake-scene requires --scene <path>');
      process.exit(2);
    }
    const scenePath = resolve(args.scene);
    const scene = JSON.parse(readFileSync(scenePath, 'utf8'));
    const sceneDir = dirname(scenePath);
    const results = [];
    const errors = [];
    const seen = new Set();
    for (const entity of scene.entities ?? []) {
      const spec = entity.destructible;
      if (!spec || spec.mode !== 'bonded' || !spec.voronoi) continue;
      const outPath = resolve(sceneDir, spec.voronoi);
      if (seen.has(outPath)) continue;
      seen.add(outPath);
      const half = entity.scale.map((value) => Math.abs(value) * 0.5);
      const pkg = bakeVoronoi({
        half,
        cells: Number(spec.cells ?? 24),
        seed: Number(spec.seed ?? 1),
      });
      const packageErrors = validatePackage(pkg);
      if (packageErrors.length) {
        errors.push({ entity: entity.id, errors: packageErrors });
        continue;
      }
      mkdirSync(dirname(outPath), { recursive: true });
      writeFileSync(outPath, `${JSON.stringify(pkg, null, 2)}\n`);
      results.push({
        entity: entity.id,
        out: outPath,
        half,
        cells: pkg.totals.cells,
        bonds: pkg.totals.bonds,
        volume: pkg.totals.volume,
      });
    }
    if (!results.length && !errors.length) {
      errors.push('scene has no destructible.voronoi references to bake');
    }
    console.log(JSON.stringify({ ok: errors.length === 0, baked: results, errors }, null, 2));
    process.exit(errors.length ? 1 : 0);
  }
  console.error('usage: voronoi.mjs bake|verify|bake-scene ...');
  process.exit(2);
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) main();

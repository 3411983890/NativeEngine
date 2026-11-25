import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { colorConstraints } from '../cloth/topology.mjs';
import { poseSkeleton, restPose } from '../character/rig-mesh.mjs';
import { GarmentError } from './inspect.mjs';
import { DEFAULT_MAX_INFLUENCES } from './weights.mjs';

// Native garment simulation asset: the cooked form of "existing garment mesh + skin
// weights + physics contract + skeleton poses".
//
// Why this is a separate cooked schema rather than another cloth /2 variant: cloth /2 is
// a *grid* whose vertex indices mean (column, row), and every renderer-side assumption
// downstream — UV, membrane material frame, LOD resolution — reads that. A garment is an
// arbitrary triangle mesh with no grid, so folding skinning into the same document would
// make "is columns/rows meaningful?" a per-asset question. Here `mesh.type` says `mesh`,
// the vertex count comes from the baked arrays, and LOD is declared per triangle count.
//
// Everything a solver needs is baked: rest positions, membranes and hinges, unique edge
// rest lengths for strain limiting, per-vertex inverse mass, skin joints and weights,
// the four physics channels, bone-local collision proxies and the bone world matrices of
// every pose in the motion set. The runtime never measures geometry or solves an IK pose
// while it is integrating.

export const GARMENT_SCHEMA = 'nativegpu.garment/1';
export const COOKED_GARMENT_SCHEMA = 'nativegpu.cloth-cook/3';
export const GARMENT_MAX_INFLUENCES = DEFAULT_MAX_INFLUENCES;

// Fabric defaults are for a woven shirt, not for a flag: heavier, barely stretchy, and
// damped enough that a pose change does not ring for half a second.
export const GARMENT_MATERIAL = Object.freeze({
  arealDensityKgM2: 0.24, thicknessMetres: 0.0015,
  stretchCompliance: 1e-6, shearCompliance: 1e-5, bendAngleCompliance: 6,
  damping: 2.5, dragCoefficient: 0.8,
});

const fail = (ok, code, message) => { if (!ok) throw new GarmentError(code, message); };
const finiteIn = (value, low, high, label) => {
  fail(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high,
    'NG-E-GARMENT-COOK', `${label} must be finite and in [${low},${high}]`);
  return value;
};

// Column-vector convention, m[column*4 + row]. Identical to the character rig and to the
// native solver, so a baked matrix can be compared element by element across the boundary.
export function multiplyMatrix(a, b) {
  const out = new Array(16).fill(0);
  for (let column = 0; column < 4; ++column)
    for (let row = 0; row < 4; ++row) {
      let sum = 0;
      for (let k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
      out[column * 4 + row] = sum;
    }
  return out;
}

// Inverse of an affine matrix (no shear, uniform-or-axis scale). Bone matrices are
// affine by construction; a general inverse would only hide a bad matrix.
export function invertAffine(m) {
  const det3 = (a, b, c, d, e, f, g, h, i) => a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
  const determinant = det3(a, b, c, d, e, f, g, h, i);
  fail(Math.abs(determinant) > 1e-9, 'NG-E-GARMENT-COOK', 'bone matrix is singular');
  const inverse = new Array(16).fill(0);
  inverse[0] = (e * i - f * h) / determinant;
  inverse[4] = (c * h - b * i) / determinant;
  inverse[8] = (b * f - c * e) / determinant;
  inverse[1] = (f * g - d * i) / determinant;
  inverse[5] = (a * i - c * g) / determinant;
  inverse[9] = (c * d - a * f) / determinant;
  inverse[2] = (d * h - e * g) / determinant;
  inverse[6] = (b * g - a * h) / determinant;
  inverse[10] = (a * e - b * d) / determinant;
  inverse[3] = inverse[7] = inverse[11] = 0;
  inverse[15] = 1;
  // Translation: -R^-1 * t.
  const t = [m[12], m[13], m[14]];
  inverse[12] = -(inverse[0] * t[0] + inverse[4] * t[1] + inverse[8] * t[2]);
  inverse[13] = -(inverse[1] * t[0] + inverse[5] * t[1] + inverse[9] * t[2]);
  inverse[14] = -(inverse[2] * t[0] + inverse[6] * t[1] + inverse[10] * t[2]);
  return inverse;
}

export function transformPointMatrix(m, point) {
  return [
    m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12],
    m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13],
    m[2] * point[0] + m[6] * point[1] + m[10] * point[2] + m[14],
  ];
}

function triangleArea(positions, a, b, c) {
  const ux = positions[b][0] - positions[a][0], uy = positions[b][1] - positions[a][1], uz = positions[b][2] - positions[a][2];
  const vx = positions[c][0] - positions[a][0], vy = positions[c][1] - positions[a][1], vz = positions[c][2] - positions[a][2];
  return Math.hypot(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx) / 2;
}

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a) => Math.hypot(a[0], a[1], a[2]);
const normalize = (a) => { const l = norm(a); return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : a; };

// Membrane rest frame for an arbitrary 3D triangle mesh.
//
// `engine/cloth/topology.mjs` derives its inverse from the authored X/Y axes, which is
// correct for a grid lying in the XY plane and wrong for a garment wrapped around a body:
// the material axes would be the world axes projected onto a tilted triangle, so the rest
// lengths no longer read as unit strain and the native validator rejects the asset (and
// would otherwise have measured the wrong stretch). Here the frame is built in the
// triangle's own plane — u along the first edge, v orthogonal to it — and then expressed in
// the {e0, e1} basis, which is exactly the linear solve `u = e0*a0 + e1*a1` the native
// membrane code inverts.
export function bakeGarmentTopology(positions, triangles) {
  const membranes = [], hinges = [], edges = new Map();
  const adjacency = Array.from({ length: positions.length }, () => []);
  for (let id = 0; id < triangles.length; ++id) {
    const triangle = triangles[id];
    const [a, b, c] = triangle;
    const origin = positions[a];
    const e0 = sub(positions[b], origin), e1 = sub(positions[c], origin);
    const area = norm(cross(e0, e1)) / 2;
    fail(Number.isFinite(area) && area > 1e-12, 'NG-E-GARMENT-COOK', `triangle ${id} is degenerate`);
    const u = normalize(e0);
    const v = normalize(cross(normalize(cross(e0, e1)), u));
    const g00 = dot(e0, e0), g01 = dot(e0, e1), g11 = dot(e1, e1);
    const determinant = g00 * g11 - g01 * g01;
    fail(Math.abs(determinant) > 1e-16, 'NG-E-GARMENT-COOK', `triangle ${id} has no stable material frame`);
    // Least-squares coordinates of a plane direction w in the (e0, e1) basis.
    const solve = (w) => [
      (g11 * dot(w, e0) - g01 * dot(w, e1)) / determinant,
      (g00 * dot(w, e1) - g01 * dot(w, e0)) / determinant,
    ];
    const [a0, a1] = solve(u), [a2, a3] = solve(v);
    membranes.push({ inverse: [a0, a1, a2, a3], area });
    for (const vertex of triangle) adjacency[vertex].push(id);
    for (let i = 0; i < 3; ++i) {
      const from = triangle[i], to = triangle[(i + 1) % 3], opposite = triangle[(i + 2) % 3];
      const key = from < to ? `${from},${to}` : `${to},${from}`;
      const list = edges.get(key) ?? [];
      list.push({ a: from, b: to, opposite });
      fail(list.length <= 2, 'NG-E-GARMENT-COOK', 'non-manifold garment edge');
      edges.set(key, list);
    }
  }
  for (const adjacent of edges.values()) {
    if (adjacent.length !== 2) continue;
    const [first, second] = adjacent;
    fail(first.a === second.b && first.b === second.a, 'NG-E-GARMENT-COOK', 'inconsistent garment triangle winding');
    const vertices = [first.a, first.b, first.opposite, second.opposite];
    const p = vertices.map((vertex) => positions[vertex]);
    const e = sub(p[1], p[0]), n0 = cross(e, sub(p[2], p[0])), n1 = cross(sub(p[3], p[0]), e);
    const scale = norm(n0) * norm(n1);
    fail(scale > 1e-16, 'NG-E-GARMENT-COOK', 'garment hinge has a degenerate face');
    const sine = dot(cross(n0, n1), e) / (scale * norm(e));
    const cosine = dot(n0, n1) / scale;
    hinges.push({ vertices, angle: Math.atan2(sine, cosine) });
  }
  return {
    model: 'triangle-xpbd/1', membranes, hinges,
    membraneColors: colorConstraints(triangles, positions.length),
    hingeColors: colorConstraints(hinges.map((hinge) => hinge.vertices), positions.length),
    adjacency,
  };
}

// Unique triangle edges with their rest lengths. The membrane solve handles strain in
// the material frame; this list is for the strain *limiter*, which has to see a real
// rest length per edge or it has nothing to clamp against.
export function bakeEdgeRest(positions, indices) {
  const seen = new Map(), edges = [];
  for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
    const tri = [indices[triangle * 3], indices[triangle * 3 + 1], indices[triangle * 3 + 2]];
    for (let i = 0; i < 3; ++i) {
      const a = tri[i], b = tri[(i + 1) % 3];
      const key = a < b ? `${a},${b}` : `${b},${a}`;
      if (seen.has(key)) continue;
      seen.set(key, true);
      const rest = Math.hypot(
        positions[a][0] - positions[b][0], positions[a][1] - positions[b][1], positions[a][2] - positions[b][2]);
      if (rest <= 1e-9) continue;
      edges.push([Math.min(a, b), Math.max(a, b), rest]);
    }
  }
  edges.sort((left, right) => left[0] - right[0] || left[1] - right[1]);
  return edges;
}

function quantile(values, fraction) {
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.floor(sorted.length * fraction)))];
}

// Bone-local ellipsoid collision proxies, fitted from the character's own rest mesh.
//
// Why not the round capsules the hair path uses (`characterCollisionProxies`): a capsule
// has one radius, and this sample character's torso is roughly three times deeper in Z
// than it is wide in X. A round capsule whose radius covers the chest in Z stands 6 cm
// proud of the chest in X, so a fitted shirt *starts* 5–7 cm inside the collider and the
// solver has to push it off the body to make the penetration report clean. Measured on
// this character, the round-capsule set gives 71 mm of rest penetration at the chest; a
// per-bone ellipsoid gives 0 mm at the same height. The ellipsoid is also the better model
// for the collision response people actually expect from a shoulder or a thigh.
//
// Fitting: group the rest vertices by dominant bone, move them into that bone's space, take
// the mean as the centre and the per-axis 90th percentile of |offset| as the radii. The
// percentile drops stray vertices (a fingertip weighted to the forearm) that would otherwise
// inflate a whole segment.
export function bakeBodyEllipsoids(character, {
  minVertices = 24, minimumRadiusMetres = 0.02, maxProxies = 64, segmentsPerBone = 3,
} = {}) {
  const restWorld = restPose(character.bones);
  const dominant = new Int32Array(character.vertices).fill(-1);
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    let best = -1, bestWeight = 0;
    for (let slot = 0; slot < 4; ++slot) {
      const weight = character.weights[vertex * 4 + slot];
      if (weight > bestWeight) { bestWeight = weight; best = character.joints[vertex * 4 + slot]; }
    }
    dominant[vertex] = best;
  }
  const groups = new Map();
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    const bone = dominant[vertex];
    if (bone < 0) continue;
    if (!groups.has(bone)) groups.set(bone, []);
    groups.get(bone).push(vertex);
  }
  const fit = (points) => {
    const centre = [0, 0, 0];
    for (const point of points) for (let axis = 0; axis < 3; ++axis) centre[axis] += point[axis] / points.length;
    // Inscribed, not circumscribed: the 55th percentile keeps the collider inside the body
    // surface. A collider that wraps the widest vertices pushes a garment that is already
    // outside the body several centimetres off it, which reads as "cloth floats" and is the
    // single most common way a fitted garment looks wrong before any physics is involved.
    const radii = [0, 1, 2].map((axis) => quantile(points.map((point) => Math.abs(point[axis] - centre[axis])), 0.55));
    return { centre, radii };
  };
  const ellipsoids = [];
  for (const [bone, members] of groups) {
    if (members.length < minVertices) continue;
    const inverse = invertAffine(restWorld[bone]);
    const local = members.map((vertex) => transformPointMatrix(inverse, [
      character.positions[vertex * 3], character.positions[vertex * 3 + 1], character.positions[vertex * 3 + 2],
    ]));
    // One ellipsoid per bone is a poor fit for a long bone: a thigh is wide at the hip and
    // narrow at the knee, so a single ellipsoid that contains the hip end stands several
    // centimetres off the knee and pushes a trouser leg outward there. Split the bone along
    // its longest local axis instead — the collider becomes a short chain.
    const extent = [0, 1, 2].map((axis) => {
      const values = local.map((point) => point[axis]);
      return Math.max(...values) - Math.min(...values);
    });
    const axis = extent.indexOf(Math.max(...extent));
    const ordered = [...local].sort((left, right) => left[axis] - right[axis]);
    const wanted = Math.max(1, Math.min(segmentsPerBone, Math.round(extent[axis] / (4 * minimumRadiusMetres))));
    const pieces = Math.min(wanted, Math.max(1, Math.floor(members.length / minVertices)));
    const stride = Math.ceil(ordered.length / pieces);
    for (let piece = 0; piece < pieces; ++piece) {
      const chunk = ordered.slice(piece * stride, Math.min(ordered.length, (piece + 1) * stride));
      if (chunk.length < 8) continue;
      const { centre, radii } = fit(chunk);
      if (Math.max(...radii) < minimumRadiusMetres) continue;
      fail(Math.min(...radii) > 1e-3, 'NG-E-GARMENT-COOK',
        `bone ${character.bones[bone].name} fitted to a degenerate ellipsoid`);
      ellipsoids.push({
        bone,
        centre: centre.map((value) => round(value, 6)),
        radii: radii.map((value) => round(value, 6)),
      });
    }
  }
  fail(ellipsoids.length >= 3, 'NG-E-GARMENT-COOK', 'the character produced fewer than three collision ellipsoids');
  fail(ellipsoids.length <= maxProxies, 'NG-E-GARMENT-COOK', 'the character produced too many collision ellipsoids');
  ellipsoids.sort((left, right) => Math.max(...right.radii) - Math.max(...left.radii));
  return { spheres: [], capsules: [], ellipsoids };
}

export function bakeBoneProxies(character) {
  return bakeBodyEllipsoids(character);
}

export function bakePoseSet(character, poses) {
  const baked = [];
  for (const entry of poses) {
    const name = typeof entry === 'string' ? entry : entry.name;
    const pose = typeof entry === 'string' ? {} : (entry.pose ?? {});
    fail(typeof name === 'string' && /^[a-z][a-z0-9_]{0,31}$/.test(name),
      'NG-E-GARMENT-COOK', `pose name ${name} must match [a-z][a-z0-9_]*`);
    const world = poseSkeleton(character.bones, pose);
    fail(world.length === character.bones.length, 'NG-E-GARMENT-COOK', `pose ${name} has the wrong bone count`);
    for (const matrix of world) {
      fail(matrix.length === 16 && matrix.every(Number.isFinite),
        'NG-E-GARMENT-COOK', `pose ${name} produced a non-finite bone matrix`);
    }
    baked.push({ name, bones: world.map((matrix) => matrix.map((value) => round(value, 9))) });
  }
  return baked;
}

function round(value, digits) {
  const scale = 10 ** digits;
  return Math.round(value * scale) / scale;
}

// Cooks one bound garment into the native document. `bound` is the output of
// `bindGarment`, so the weights that reach the solver are exactly the ones the validator
// approved — no second transfer path.
export function cookGarment({
  id, revision = 0, seed = 0, character, bound, poses = [{ name: 'idle', pose: {} }],
  material = {}, budget = {}, strainLimit = 1.15, maxSpeed = 12, constraintsAxes = null,
} = {}) {
  fail(character && Array.isArray(character.bones) && character.bones.length > 0,
    'NG-E-GARMENT-COOK', 'a character with a skeleton is required');
  fail(bound?.garment && bound?.weights, 'NG-E-GARMENT-COOK', 'bindGarment output is required');
  fail(typeof id === 'string' && /^[a-z][a-z0-9_-]{0,63}$/.test(id), 'NG-E-GARMENT-COOK', 'invalid garment id');
  const { garment, weights, contract } = bound;
  const influences = weights.maximumInfluences ?? GARMENT_MAX_INFLUENCES;
  fail(influences === GARMENT_MAX_INFLUENCES,
    'NG-E-GARMENT-COOK', `native garment supports exactly ${GARMENT_MAX_INFLUENCES} influences per vertex`);

  const positions = [];
  for (let vertex = 0; vertex < garment.vertices; ++vertex)
    positions.push([garment.positions[vertex * 3], garment.positions[vertex * 3 + 1], garment.positions[vertex * 3 + 2]]);
  for (const point of positions)
    for (const axis of point) fail(Number.isFinite(axis) && Math.abs(axis) <= 1000,
      'NG-E-GARMENT-COOK', 'garment rest position is not finite or is outside the metre mapping bound');

  const triangles = [];
  for (let triangle = 0; triangle < garment.triangles; ++triangle) {
    const tri = [garment.indices[triangle * 3], garment.indices[triangle * 3 + 1], garment.indices[triangle * 3 + 2]];
    fail(new Set(tri).size === 3, 'NG-E-GARMENT-COOK', `triangle ${triangle} repeats a vertex`);
    for (const vertex of tri) fail(vertex < garment.vertices, 'NG-E-GARMENT-COOK', `triangle ${triangle} leaves the mesh`);
    triangles.push(tri);
  }

  const areas = new Array(garment.vertices).fill(0);
  let totalArea = 0;
  for (const [a, b, c] of triangles) {
    const area = triangleArea(positions, a, b, c);
    fail(Number.isFinite(area) && area > 1e-12, 'NG-E-GARMENT-COOK', 'garment has a degenerate triangle');
    totalArea += area;
    for (const vertex of [a, b, c]) areas[vertex] += area / 3;
  }
  fail(totalArea > 1e-6, 'NG-E-GARMENT-COOK', 'garment has no surface area');

  const topology = bakeGarmentTopology(positions, triangles);
  const edges = bakeEdgeRest(positions, garment.indices);

  const materialValues = { ...GARMENT_MATERIAL };
  for (const key of Object.keys(material)) {
    fail(Object.hasOwn(materialValues, key), 'NG-E-GARMENT-COOK', `unsupported material channel ${key}`);
    materialValues[key] = material[key];
  }
  finiteIn(materialValues.arealDensityKgM2, 0.001, 50, 'material.arealDensityKgM2');
  finiteIn(materialValues.thicknessMetres, 0.0001, 0.2, 'material.thicknessMetres');
  finiteIn(materialValues.stretchCompliance, 0, 1, 'material.stretchCompliance');
  finiteIn(materialValues.shearCompliance, 0, 1, 'material.shearCompliance');
  finiteIn(materialValues.bendAngleCompliance, 0, 1e8, 'material.bendAngleCompliance');
  finiteIn(materialValues.damping, 0, 50, 'material.damping');
  finiteIn(materialValues.dragCoefficient, 0, 10, 'material.dragCoefficient');

  const channel = (name, source, low, high) => {
    const values = [];
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      const value = source[vertex];
      fail(Number.isFinite(value) && value >= low && value <= high,
        'NG-E-GARMENT-COOK', `${name}[${vertex}] must be finite and in [${low},${high}]`);
      values.push(round(value, 6));
    }
    return values;
  };
  const blend = channel('blend', contract.blend, 0, 1);
  const maxDisplacement = channel('maxDisplacement', contract.maxDisplacement, 0, 0.5);
  const thickness = channel('thickness', contract.thickness, 0, 0.1);
  const anchor = channel('anchor', contract.anchors, 0, 1);
  const mass = channel('mass', contract.mass, 1e-5, 5);

  const joints = [], weightsOut = [];
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    let sum = 0;
    for (let slot = 0; slot < influences; ++slot) {
      const weight = weights.weights[vertex * influences + slot];
      const joint = weights.joints[vertex * influences + slot];
      fail(Number.isFinite(weight) && weight >= 0, 'NG-E-GARMENT-COOK', `vertex ${vertex} has a negative weight`);
      if (weight > 0) {
        fail(Number.isSafeInteger(joint) && joint >= 0 && joint < character.bones.length,
          'NG-E-GARMENT-COOK', `vertex ${vertex} references joint ${joint} outside the skeleton`);
      }
      sum += weight;
      joints.push(weight > 0 ? joint : 0);
      weightsOut.push(round(weight, 6));
    }
    fail(Math.abs(sum - 1) <= 1e-3, 'NG-E-GARMENT-COOK',
      `vertex ${vertex} weights sum to ${sum.toFixed(4)}; the native solver requires normalization`);
  }

  const proxies = bakeBodyEllipsoids(character);
  fail(proxies.ellipsoids.length > 0,
    'NG-E-GARMENT-COOK', 'the character produced no collision proxies; garment collision would be a no-op');

  const poseSet = bakePoseSet(character, poses);
  fail(poseSet.length >= 1, 'NG-E-GARMENT-COOK', 'at least one baked pose is required');

  const source = {
    schema: GARMENT_SCHEMA, id, revision, seed, kind: 'garment',
    mesh: { type: 'mesh', vertexCount: garment.vertices, triangleCount: triangles.length },
    material: materialValues,
    collision: { thicknessMetres: 0, plane: null, spheres: [], capsules: [] },
    bones: character.bones.map((bone) => ({
      name: String(bone.name), inverseBind: bone.inverseBind.map((value) => round(value, 9)),
    })),
    skin: { maximumInfluences: influences, joints, weights: weightsOut },
    physics: { blend, maxDisplacement, thickness, anchor, mass, strainLimit, maxSpeed },
    proxies,
    poses: poseSet,
    budget: {
      maxVertices: budget.maxVertices ?? 65536, maxSubsteps: budget.maxSubsteps ?? 8,
      iterations: budget.iterations ?? 8,
    },
    baked: {
      positions: positions.map((point) => point.map((value) => round(value, 6))),
      triangles,
      masses: mass,
      edges,
      totalAreaMetres2: round(totalArea, 6),
      topology,
    },
  };
  fail(source.baked.positions.length <= source.budget.maxVertices, 'NG-E-GARMENT-COOK', 'garment exceeds budget.maxVertices');
  void constraintsAxes;

  const payload = { schema: COOKED_GARMENT_SCHEMA, source };
  return {
    source, payload, hash: sha256Hex(canonicalJson(payload)),
    vertexCount: garment.vertices, triangleCount: triangles.length,
    edgeCount: edges.length, influenceCount: influences,
    boneCount: character.bones.length, poseCount: poseSet.length,
    proxyCount: proxies.spheres.length + proxies.capsules.length + proxies.ellipsoids.length,
    dynamicsVertices: blend.filter((value) => value > 0).length,
    anchoredVertices: anchor.filter((value) => value > 0).length,
    totalAreaMetres2: totalArea,
  };
}

export function compileGarmentLibrary(garments) {
  fail(Array.isArray(garments) && garments.length >= 1 && garments.length <= 256,
    'NG-E-GARMENT-COOK', 'garment library needs 1–256 assets');
  const cooked = garments.map((entry) => cookGarment(entry));
  const ids = new Set();
  for (const garment of cooked) {
    fail(!ids.has(garment.source.id), 'NG-E-GARMENT-COOK', `duplicate garment id ${garment.source.id}`);
    ids.add(garment.source.id);
  }
  const sorted = cooked.sort((left, right) => left.source.id.localeCompare(right.source.id));
  return { garments: sorted, hash: sha256Hex(canonicalJson(sorted.map((garment) => [garment.source.id, garment.hash]))) };
}

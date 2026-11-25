import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { Stream } from '../vegetation/random.mjs';
import {
  headVertexMask, extractSubmesh, skinPositions, skinPalette, restPose, poseSkeleton, findBone,
  triangleNormal, closestPointOnTriangle, transformPoint, transformDirection,
} from '../character/rig-mesh.mjs';
import { HairError, insideRegion, regionSeed } from './spec.mjs';

// Scalp binding on the character's real head geometry.
//
// A spherical cap is not a scalp. It has no ears, no brow, no nape, and its radius is
// a guess. Every root bound to it drifts the moment the character deforms, and the
// hairline cannot be matched to a reference photograph because there is no hairline.
// This module therefore builds the scalp out of the character's own triangles and
// stores roots as (triangle, barycentric, offset) frames, which is the only
// representation that survives the skull moving.

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

function barycentricPoint(positions, indices, triangle, bary) {
  const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
  return [0, 1, 2].map((axis) => bary[0] * positions[a * 3 + axis]
    + bary[1] * positions[b * 3 + axis] + bary[2] * positions[c * 3 + axis]);
}

// Interpolating the character's four skin influences over the triangle, then taking
// the four largest, is what makes a barycentric root follow the skull: using only the
// face's dominant bone would snap a root across a joint seam when the head turns.
// `scalp.indices` are compact submesh indices, so every lookup goes through
// `sourceVertex` — reading the character table with a compact index resolves the wrong
// vertices and produces a root that drifts by centimetres.
function interpolatedInfluences(character, scalp, triangle, bary) {
  const accumulated = new Map();
  for (let corner = 0; corner < 3; ++corner) {
    const local = scalp.indices[triangle * 3 + corner];
    const vertex = scalp.sourceVertex ? scalp.sourceVertex[local] : local;
    for (let influence = 0; influence < 4; ++influence) {
      const weight = character.weights[vertex * 4 + influence];
      if (weight <= 0) continue;
      const joint = character.joints[vertex * 4 + influence];
      accumulated.set(joint, (accumulated.get(joint) ?? 0) + weight * bary[corner]);
    }
  }
  const sorted = [...accumulated.entries()].sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, 4);
  const total = sorted.reduce((sum, entry) => sum + entry[1], 0) || 1;
  return sorted.map(([joint, weight]) => ({ joint, weight: weight / total }));
}

export function resolveBoundPoint(character, palette, binding, scalp, restPoint) {
  const influences = interpolatedInfluences(character, scalp, binding.triangle, binding.bary);
  let x = 0, y = 0, z = 0;
  for (const influence of influences) {
    const moved = transformPoint(palette[influence.joint], restPoint);
    x += moved[0] * influence.weight;
    y += moved[1] * influence.weight;
    z += moved[2] * influence.weight;
  }
  return [x, y, z];
}

export function resolveBoundNormal(character, palette, binding, scalp, restNormal) {
  const influences = interpolatedInfluences(character, scalp, binding.triangle, binding.bary);
  let x = 0, y = 0, z = 0;
  for (const influence of influences) {
    const moved = transformDirection(palette[influence.joint], restNormal);
    x += moved[0] * influence.weight;
    y += moved[1] * influence.weight;
    z += moved[2] * influence.weight;
  }
  const length = Math.hypot(x, y, z) || 1;
  return [x / length, y / length, z / length];
}

// ------------------------------------------------------------------ build
export function buildScalp(character, { headBone = 'head', poles = null } = {}) {
  const head = findBone(character.bones, headBone);
  fail(head, 'NG-E-HAIR-SCALP', `character has no ${headBone} bone; hair cannot be bound to a guessed surface`);
  const world = restPose(character.bones);
  const palette = skinPalette(character.bones, world);
  const restPositions = skinPositions(character, palette);
  const mask = headVertexMask(character, { headBone });
  const submesh = extractSubmesh(character, mask, restPositions);

  // Head frame: the skull joint is the centre, the crown direction is up. Both come
  // from the rig, not from the mesh bounds, so a character with a tall hat mesh or a
  // long neck is still parameterised around its own skull.
  const headOrigin = [world[head.index][12], world[head.index][13], world[head.index][14]];
  let crown = headOrigin;
  for (let vertex = 0; vertex < submesh.vertices; ++vertex) {
    if (submesh.positions[vertex * 3 + 1] > crown[1]) {
      crown = [submesh.positions[vertex * 3], submesh.positions[vertex * 3 + 1], submesh.positions[vertex * 3 + 2]];
    }
  }
  const centre = [headOrigin[0], crown[1] - (crown[1] - headOrigin[1]) * 0.55, headOrigin[2]];
  let radius = 0;
  for (let vertex = 0; vertex < submesh.vertices; ++vertex) {
    radius = Math.max(radius, Math.hypot(
      submesh.positions[vertex * 3] - centre[0],
      submesh.positions[vertex * 3 + 1] - centre[1],
      submesh.positions[vertex * 3 + 2] - centre[2],
    ));
  }
  fail(radius > 0.01, 'NG-E-HAIR-SCALP', 'the head region is degenerate; check the character rig');

  const triangles = submesh.triangles;
  const areas = new Float64Array(triangles);
  const centroids = new Float64Array(triangles * 3);
  const normals = new Float64Array(triangles * 3);
  const azimuths = new Float64Array(triangles);
  const polars = new Float64Array(triangles);
  let totalArea = 0;
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const a = submesh.indices[triangle * 3], b = submesh.indices[triangle * 3 + 1], c = submesh.indices[triangle * 3 + 2];
    const { normal, area } = triangleNormal(submesh.positions, a, b, c);
    areas[triangle] = area;
    totalArea += area;
    for (let axis = 0; axis < 3; ++axis) {
      centroids[triangle * 3 + axis] = (submesh.positions[a * 3 + axis]
        + submesh.positions[b * 3 + axis] + submesh.positions[c * 3 + axis]) / 3;
      normals[triangle * 3 + axis] = normal[axis];
    }
    azimuths[triangle] = (Math.atan2(centroids[triangle * 3] - centre[0],
      centroids[triangle * 3 + 2] - centre[2]) * 180) / Math.PI;
    const dy = centroids[triangle * 3 + 1] - centre[1];
    const distance = Math.hypot(centroids[triangle * 3] - centre[0], dy, centroids[triangle * 3 + 2] - centre[2]) || 1e-9;
    polars[triangle] = (Math.acos(Math.max(-1, Math.min(1, dy / distance))) * 180) / Math.PI;
  }

  const scalp = {
    positions: submesh.positions,
    indices: submesh.indices,
    sourceVertex: submesh.sourceVertex,
    vertices: submesh.vertices,
    triangles,
    areas, totalArea, centroids, normals, azimuths, polars,
    centre, radius, headBone, headBoneIndex: head.index, headOrigin,
    restPositions,
    characterHash: character.sourceSha256 ?? sha256Hex(canonicalJson({
      vertices: character.vertices, triangles: character.indices.length / 3,
    })),
    poles,
  };
  scalp.hash = scalpHash(scalp);
  return scalp;
}

export function scalpHash(scalp) {
  return sha256Hex(canonicalJson({
    version: 'scalp/1',
    vertices: scalp.vertices,
    triangles: scalp.triangles,
    headBone: scalp.headBone,
    centre: scalp.centre.map((value) => Math.round(value * 1e5) / 1e5),
    character: scalp.characterHash,
  })).slice(0, 32);
}

// Region membership is evaluated per triangle, not per vertex: sampling a vertex set
// leaves holes where a region boundary crosses a large triangle.
export function trianglesInRegion(scalp, region) {
  const selected = [];
  for (let triangle = 0; triangle < scalp.triangles; ++triangle) {
    if (scalp.areas[triangle] <= 0) continue;
    if (insideRegion(region, scalp.azimuths[triangle], scalp.polars[triangle])) selected.push(triangle);
  }
  return selected;
}

export function regionArea(scalp, region) {
  return trianglesInRegion(scalp, region).reduce((sum, triangle) => sum + scalp.areas[triangle], 0);
}

// Stratified area sampling with a cumulative-area table. Deterministic for a given
// region seed, and independent of every other region: adding a region cannot move
// another region's roots. That independence is what "修改刘海不重生成后脑" requires.
export function sampleRoots(scalp, region, count, { seed, jitterAzimuthDegrees = 0 } = {}) {
  const candidates = trianglesInRegion(scalp, region);
  fail(candidates.length > 0, 'NG-E-HAIR-SCALP',
    `region ${region.id} covers no scalp triangles; check its azimuth/polar sector`);
  const cumulative = new Float64Array(candidates.length);
  let running = 0;
  for (let index = 0; index < candidates.length; ++index) {
    running += scalp.areas[candidates[index]];
    cumulative[index] = running;
  }
  const stream = new Stream(seed, `hair:${region.id}`);
  const roots = [];
  for (let index = 0; index < count; ++index) {
    // Stratified: one sample per equal-area slab, shuffled inside the slab. Pure
    // uniform sampling clumps at low counts, which reads as bald patches.
    const slab = (index + stream.unit()) / count;
    const target = slab * running;
    let low = 0, high = candidates.length - 1;
    while (low < high) {
      const mid = (low + high) >> 1;
      if (cumulative[mid] < target) low = mid + 1; else high = mid;
    }
    const triangle = candidates[low];
    // Deterministic barycentric sample with sqrt warp so points are uniform on area.
    const u = stream.unit(), v = stream.unit();
    const su = Math.sqrt(u);
    let bary = [1 - su, su * (1 - v), su * v];
    bary = bary.map((value) => Math.max(0, Math.min(1, value)));
    const sum = bary[0] + bary[1] + bary[2];
    bary = bary.map((value) => value / sum);
    const point = barycentricPoint(scalp.positions, scalp.indices, triangle, bary);
    const normal = [scalp.normals[triangle * 3], scalp.normals[triangle * 3 + 1], scalp.normals[triangle * 3 + 2]];
    roots.push({
      triangle, bary, point, normal,
      azimuthDegrees: scalp.azimuths[triangle] + jitterAzimuthDegrees * (stream.unit() - 0.5),
      polarDegrees: scalp.polars[triangle],
      areaShare: scalp.areas[triangle] / running,
    });
  }
  return roots;
}

// Guide count from a real density. `densityPerCm2` is *hair* density (hairs per square
// centimetre, 100–300 for a human scalp); a guide stands for `streamsPerGuide` hairs,
// so guides = area × density / streamsPerGuide. Treating the field as guides-per-cm²
// produces thousands of guides on a fringe and none of the density variation the
// reference photograph actually shows.
export function guideCountForRegion(scalp, region, { streamsPerGuide = 40, maximum = 4096 } = {}) {
  const areaCm2 = regionArea(scalp, region) * 10000;
  const requested = Math.round((areaCm2 * region.densityPerCm2) / Math.max(1, streamsPerGuide));
  return Math.max(4, Math.min(maximum, requested));
}

// --------------------------------------------------------------- rebinding
// Root detachment is measured against the *deformed scalp surface*, not against a
// rigid skull rotation. A root at the nape is blended between head and neck by
// design; comparing it to a pure head rotation would report a 1.6 mm "detach" that is
// in fact correct skinning. The honest question is "is the root still on the scalp",
// so that is what this measures.
export function deformScalpPositions(character, palette, scalp) {
  const full = skinPositions(character, palette);
  const out = new Float32Array(scalp.vertices * 3);
  for (let vertex = 0; vertex < scalp.vertices; ++vertex) {
    const source = scalp.sourceVertex ? scalp.sourceVertex[vertex] : vertex;
    out[vertex * 3] = full[source * 3];
    out[vertex * 3 + 1] = full[source * 3 + 1];
    out[vertex * 3 + 2] = full[source * 3 + 2];
  }
  return out;
}

export function measureBindingDrift(character, bindings, scalp, { pose = {}, headBone = 'head', toleranceMetres = 5e-4 } = {}) {
  const head = findBone(character.bones, headBone);
  fail(head, 'NG-E-HAIR-BINDING', `character has no ${headBone} bone`);
  const posedPalette = skinPalette(character.bones, poseSkeleton(character.bones, pose));
  const posedScalp = deformScalpPositions(character, posedPalette, scalp);
  let maximum = 0, offSurface = 0;
  const samples = [];
  for (const binding of bindings) {
    // The root's rest position comes from the scalp submesh, not from the character
    // table: compact indices into a full vertex array resolve unrelated vertices.
    const restPoint = barycentricPoint(scalp.positions, scalp.indices, binding.triangle, binding.bary);
    const moved = resolveBoundPoint(character, posedPalette, binding, scalp, restPoint);
    const { distance } = nearestSurfacePoint(posedScalp, scalp.indices, moved, binding.triangle);
    maximum = Math.max(maximum, distance);
    if (distance > toleranceMetres) ++offSurface;
    if (samples.length < 8) samples.push(distance);
  }
  return { maximumSurfaceErrorMetres: maximum, rootsOffSurface: offSurface, samples };
}

// Nearest point on the deformed scalp, starting from the binding's own triangle so a
// correct binding costs one test instead of a full scan.
function nearestSurfacePoint(positions, indices, point, preferredTriangle = -1) {
  let best = { distance: Infinity, triangle: -1 };
  const test = (triangle) => {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    const hit = closestPointOnTriangle(point,
      [positions[a * 3], positions[a * 3 + 1], positions[a * 3 + 2]],
      [positions[b * 3], positions[b * 3 + 1], positions[b * 3 + 2]],
      [positions[c * 3], positions[c * 3 + 1], positions[c * 3 + 2]]);
    const distance = Math.hypot(hit.point[0] - point[0], hit.point[1] - point[1], hit.point[2] - point[2]);
    if (distance < best.distance) best = { distance, triangle, bary: hit.bary };
    return distance;
  };
  if (preferredTriangle >= 0 && test(preferredTriangle) <= 1e-6) return best;
  for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
    if (triangle === preferredTriangle) continue;
    test(triangle);
  }
  return best;
}

// 4x4 affine inverse, enough for inverse-bind matrices (no projection term).
export function invertAffine(matrix) {
  const a = matrix;
  const det = a[0] * (a[5] * a[10] - a[6] * a[9])
    - a[4] * (a[1] * a[10] - a[2] * a[9])
    + a[8] * (a[1] * a[6] - a[2] * a[5]);
  fail(Math.abs(det) > 1e-12, 'NG-E-HAIR-BINDING', 'character has a singular skeleton matrix');
  const inv = new Array(16).fill(0);
  inv[0] = (a[5] * a[10] - a[6] * a[9]) / det;
  inv[1] = (a[2] * a[9] - a[1] * a[10]) / det;
  inv[2] = (a[1] * a[6] - a[2] * a[5]) / det;
  inv[4] = (a[6] * a[8] - a[4] * a[10]) / det;
  inv[5] = (a[0] * a[10] - a[2] * a[8]) / det;
  inv[6] = (a[2] * a[4] - a[0] * a[6]) / det;
  inv[8] = (a[4] * a[9] - a[5] * a[8]) / det;
  inv[9] = (a[1] * a[8] - a[0] * a[9]) / det;
  inv[10] = (a[0] * a[5] - a[1] * a[4]) / det;
  inv[12] = -(a[12] * inv[0] + a[13] * inv[4] + a[14] * inv[8]);
  inv[13] = -(a[12] * inv[1] + a[13] * inv[5] + a[14] * inv[9]);
  inv[14] = -(a[12] * inv[2] + a[13] * inv[6] + a[14] * inv[10]);
  inv[15] = 1;
  return inv;
}

// Collision proxies that move with the character. The plan requires head, shoulder
// and chest proxies driven by bones, and it requires them to be derived from the rig
// rather than authored per hairstyle, so a hairstyle cannot pick a convenient head.
// Collision proxies that move with the character, sized from the *body mesh* rather
// than from bone lengths. A capsule whose radius is a fraction of a bone's length is
// wrong for every character that is not the one it was tuned on: the chest becomes a
// 5 cm tube and every strand that hangs down the back reports 45 mm of "penetration".
// Measuring the radius from vertices actually skinned to those bones gives a proxy the
// hair can rest on.
const PROXY_DEFINITIONS = Object.freeze([
  { kind: 'sphere', bone: 'head', members: ['head'] },
  { kind: 'capsule', bone: 'spine', endBone: 'chest', members: ['spine', 'chest', 'spine1'] },
  { kind: 'capsule', bone: 'leftshoulder', endBone: 'leftarm', members: ['leftshoulder', 'leftarm'] },
  { kind: 'capsule', bone: 'rightshoulder', endBone: 'rightarm', members: ['rightshoulder', 'rightarm'] },
]);

export function characterCollisionProxies(character, { pose = {}, extraBones = [] } = {}) {
  const world = poseSkeleton(character.bones, pose);
  const restPositions = skinPositions(character, skinPalette(character.bones, restPose(character.bones)));
  const byKey = new Map();
  character.bones.forEach((bone, index) => { if (!byKey.has(bone.key)) byKey.set(bone.key, index); });

  // Dominant bone per vertex, computed once: every proxy radius needs it.
  const dominant = new Int32Array(character.vertices).fill(-1);
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    let best = -1, bestWeight = 0;
    for (let influence = 0; influence < 4; ++influence) {
      const weight = character.weights[vertex * 4 + influence];
      if (weight > bestWeight) { bestWeight = weight; best = character.joints[vertex * 4 + influence]; }
    }
    dominant[vertex] = best;
  }

  const radiusFor = (memberIndices, axisA, axisB) => {
    const distances = [];
    const memberSet = new Set(memberIndices);
    for (let vertex = 0; vertex < character.vertices; ++vertex) {
      if (!memberSet.has(dominant[vertex])) continue;
      const point = [restPositions[vertex * 3], restPositions[vertex * 3 + 1], restPositions[vertex * 3 + 2]];
      if (axisB) {
        distances.push(distanceToSegment(point, axisA, axisB));
      } else {
        distances.push(Math.hypot(point[0] - axisA[0], point[1] - axisA[1], point[2] - axisA[2]));
      }
    }
    if (distances.length === 0) return null;
    distances.sort((a, b) => a - b);
    // P85, not max: a single stray vertex (a fingertip weighted to the forearm) must
    // not turn the arm into a barrel.
    return Math.max(0.01, distances[Math.min(distances.length - 1, Math.floor(distances.length * 0.85))]);
  };

  const spheres = [], capsules = [];
  for (const definition of PROXY_DEFINITIONS) {
    const startIndex = byKey.get(definition.bone);
    if (startIndex === undefined) continue;
    const start = [world[startIndex][12], world[startIndex][13], world[startIndex][14]];
    const restStart = [restPositions[0], restPositions[1], restPositions[2]];
    void restStart;
    const members = definition.members.map((key) => byKey.get(key)).filter((index) => index !== undefined);
    if (definition.kind === 'sphere') {
      const radius = radiusFor(members, start, null);
      if (radius) spheres.push({ bone: definition.bone, centre: start, radiusMetres: radius });
      continue;
    }
    const endIndex = byKey.get(definition.endBone);
    if (endIndex === undefined) continue;
    const end = [world[endIndex][12], world[endIndex][13], world[endIndex][14]];
    // Radius is measured in the rest pose against the rest axis, then the capsule is
    // placed by the posed axis: measuring after posing would let a twist change a
    // character's body thickness.
    const restWorld = restPose(character.bones);
    const restA = [restWorld[startIndex][12], restWorld[startIndex][13], restWorld[startIndex][14]];
    const restB = [restWorld[endIndex][12], restWorld[endIndex][13], restWorld[endIndex][14]];
    const radius = radiusFor(members, restA, restB);
    if (radius) capsules.push({ bone: definition.bone, a: start, b: end, radiusMetres: radius });
  }
  for (const name of extraBones) {
    const index = byKey.get(boneKeyOf(name));
    if (index === undefined) continue;
    const centre = [world[index][12], world[index][13], world[index][14]];
    const radius = radiusFor([index], centre, null);
    if (radius) spheres.push({ bone: name, centre, radiusMetres: radius });
  }
  return { spheres, capsules, source: 'characterMesh', pose };
}

function boneKeyOf(name) {
  const identifier = String(name).split(/[:|/]/).at(-1).toLowerCase();
  return identifier.replace(/^mixamorig/, '').replace(/[^a-z0-9]/g, '');
}

function distanceToSegment(point, a, b) {
  const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const ap = [point[0] - a[0], point[1] - a[1], point[2] - a[2]];
  const lengthSquared = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  const t = lengthSquared > 0
    ? Math.max(0, Math.min(1, (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / lengthSquared))
    : 0;
  const closest = [a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t];
  return Math.hypot(point[0] - closest[0], point[1] - closest[1], point[2] - closest[2]);
}

// Pushes curve points out of the proxies. Hair generation without this step produces
// guides that hang *inside* the torso, and then the first thing a reviewer sees is a
// penetration report describing the generator, not the physics.
//
// Smoothing is only for relaxing a projection: it runs when — and only where — a point
// was actually pushed. Running it over every guide unconditionally does two things the
// plan forbids: it shortens hair on every regeneration, and it dilutes an authored
// control-point edit (a 1 cm offset arrived as 4.4 mm smeared over five points), which
// makes "局部编辑" a parameter that does not do what its name says.
export function resolveProxyPenetration(points, proxies, { marginMetres = 0.004, smoothPasses = 2, keepRoot = true } = {}) {
  const moved = points.map(() => false);
  const push = (source) => source.map((point, index) => {
    if (keepRoot && index === 0) return point;
    let current = point;
    for (const sphere of proxies.spheres ?? []) {
      const offset = [current[0] - sphere.centre[0], current[1] - sphere.centre[1], current[2] - sphere.centre[2]];
      const distance = Math.hypot(offset[0], offset[1], offset[2]);
      const limit = sphere.radiusMetres + marginMetres;
      if (distance < limit) {
        const direction = distance > 1e-6 ? [offset[0] / distance, offset[1] / distance, offset[2] / distance] : [0, 1, 0];
        current = [sphere.centre[0] + direction[0] * limit,
          sphere.centre[1] + direction[1] * limit, sphere.centre[2] + direction[2] * limit];
      }
    }
    for (const capsule of proxies.capsules ?? []) {
      const ab = [capsule.b[0] - capsule.a[0], capsule.b[1] - capsule.a[1], capsule.b[2] - capsule.a[2]];
      const lengthSquared = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
      const apx = current[0] - capsule.a[0], apy = current[1] - capsule.a[1], apz = current[2] - capsule.a[2];
      const t = lengthSquared > 0
        ? Math.max(0, Math.min(1, (apx * ab[0] + apy * ab[1] + apz * ab[2]) / lengthSquared))
        : 0;
      const closest = [capsule.a[0] + ab[0] * t, capsule.a[1] + ab[1] * t, capsule.a[2] + ab[2] * t];
      const offset = [current[0] - closest[0], current[1] - closest[1], current[2] - closest[2]];
      const distance = Math.hypot(offset[0], offset[1], offset[2]);
      const limit = capsule.radiusMetres + marginMetres;
      if (distance < limit) {
        const direction = distance > 1e-6 ? [offset[0] / distance, offset[1] / distance, offset[2] / distance] : [0, 1, 0];
        current = [closest[0] + direction[0] * limit, closest[1] + direction[1] * limit, closest[2] + direction[2] * limit];
      }
    }
    if (current !== point) moved[index] = true;
    return current;
  });

  // Push, smooth, push again: smoothing after a projection pulls points back inside
  // the proxy, so the last operation must always be the projection. Otherwise the
  // reported penetration is a property of the smoother, not of the hairstyle.
  let resolved = push(points.map((point) => [point[0], point[1], point[2]]));
  if (!moved.some(Boolean)) return resolved;

  // Dilate the pushed indices so the relax pass has neighbours to average against, and
  // leave everything else exactly as authored.
  let band = moved.slice();
  for (let step = 0; step < smoothPasses + 1; ++step) {
    const next = band.slice();
    for (let index = 1; index < band.length - 1; ++index) {
      if (band[index - 1] || band[index + 1]) next[index] = true;
    }
    band = next;
  }
  const smooth = (source) => source.map((point, index) => {
    if (index === 0 || index === source.length - 1 || !band[index]) return point;
    const previous = source[index - 1], next = source[index + 1];
    return [
      point[0] * 0.6 + (previous[0] + next[0]) * 0.2,
      point[1] * 0.6 + (previous[1] + next[1]) * 0.2,
      point[2] * 0.6 + (previous[2] + next[2]) * 0.2,
    ];
  });
  for (let pass = 0; pass < smoothPasses; ++pass) resolved = push(smooth(resolved));
  resolved = push(resolved);
  if (keepRoot) resolved[0] = [points[0][0], points[0][1], points[0][2]];
  return resolved;
}


export { regionSeed, barycentricPoint, interpolatedInfluences };

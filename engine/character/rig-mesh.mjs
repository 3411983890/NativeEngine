import * as fs from 'node:fs/promises';
import path from 'node:path';
import { nodeMatrix, multiplyMatrix } from '../assets/gltf-document.mjs';
import { boneKey, sideOf, identifier } from '../assets/bone-semantics.mjs';

// Character geometry access for hair and garment authoring.
//
// The hair generator must bind roots to the *real* scalp triangle mesh of the user's
// character, and the garment binder must transfer weights from the *real* body mesh.
// Both need the same three things, so they share this reader:
//
//   * decoded vertices (position/normal/uv/joints/weights) and indices,
//   * a rest-pose skeleton contract (world matrices + skin palette),
//   * a way to pose the skeleton without loading an animation clip.
//
// Everything here is read-only and deterministic: no clock, no randomness.

export class CharacterMeshError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'CharacterMeshError';
    this.code = code;
  }
}

function fail(ok, code, message) {
  if (!ok) throw new CharacterMeshError(code, message);
}

const VERTEX_BYTES = 64;
const MAX_VERTICES = 100000;
const MAX_INDICES = 3000000;
const MAX_BONES = 256;

// --------------------------------------------------------------- matrix helpers
// Column-major 4x4, identical to the runtime and to glTF. Kept local so the module
// has no rendering dependency.

export function identityMatrix() {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

export function transformPoint(matrix, point) {
  return [
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  ];
}

export function transformDirection(matrix, direction) {
  return [
    matrix[0] * direction[0] + matrix[4] * direction[1] + matrix[8] * direction[2],
    matrix[1] * direction[0] + matrix[5] * direction[1] + matrix[9] * direction[2],
    matrix[2] * direction[0] + matrix[6] * direction[1] + matrix[10] * direction[2],
  ];
}

export function axisAngleQuaternion(axis, radians) {
  const length = Math.hypot(axis[0], axis[1], axis[2]);
  fail(length > 1e-9, 'NG-E-RIG-POSE', 'pose axis must be non-zero');
  const half = radians * 0.5, s = Math.sin(half) / length;
  return [axis[0] * s, axis[1] * s, axis[2] * s, Math.cos(half)];
}

export function multiplyQuaternion(a, b) {
  return [
    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  ];
}

// ------------------------------------------------------------------- decoding
// Vertex layout is frozen by the runtime loader: 3 f32 position, 3 f32 normal,
// 2 f32 uv, 4 u32 joints, 4 f32 weights, 64 bytes total.
export function decodeCharacter(header, binary) {
  fail(header && typeof header === 'object', 'NG-E-RIG-FORMAT', 'character header must be an object');
  fail(header.format === 'nativegpu.animation/1' || header.format === 'nativegpu.animation/2',
    'NG-E-RIG-FORMAT', `unsupported character format ${header.format}`);
  const mesh = header.mesh;
  fail(mesh && typeof mesh === 'object', 'NG-E-RIG-FORMAT', 'character header has no mesh');
  const vertices = mesh.vertices, indexCount = mesh.indices;
  fail(Number.isInteger(vertices) && vertices > 0 && vertices <= MAX_VERTICES, 'NG-E-RIG-FORMAT',
    `mesh.vertices must be an integer in [1,${MAX_VERTICES}]`);
  fail(Number.isInteger(indexCount) && indexCount > 0 && indexCount <= MAX_INDICES && indexCount % 3 === 0,
    'NG-E-RIG-FORMAT', `mesh.indices must be a triangle count in [3,${MAX_INDICES}]`);
  fail(Buffer.isBuffer(binary) || binary instanceof Uint8Array, 'NG-E-RIG-FORMAT', 'character binary must be bytes');
  const bytes = Buffer.isBuffer(binary) ? binary : Buffer.from(binary.buffer, binary.byteOffset, binary.byteLength);
  fail(mesh.vertexOffset === 0, 'NG-E-RIG-FORMAT', 'mesh.vertexOffset must be 0');
  fail(vertices * VERTEX_BYTES <= bytes.length, 'NG-E-RIG-FORMAT', 'vertex section exceeds the binary');
  const indexOffset = mesh.indexOffset ?? vertices * VERTEX_BYTES;
  fail(Number.isInteger(indexOffset) && indexOffset + indexCount * 4 <= bytes.length, 'NG-E-RIG-FORMAT',
    'index section exceeds the binary');

  const bones = Array.isArray(header.bones) ? header.bones : [];
  fail(bones.length > 0 && bones.length <= MAX_BONES, 'NG-E-RIG-SKELETON',
    `character needs 1–${MAX_BONES} bones`);
  bones.forEach((bone, index) => {
    fail(Number.isInteger(bone.parent) && bone.parent >= -1 && bone.parent < index, 'NG-E-RIG-SKELETON',
      `bone ${index} has an invalid parent`);
    fail(Array.isArray(bone.rest) && bone.rest.length === 10 && bone.rest.every(Number.isFinite),
      'NG-E-RIG-SKELETON', `bone ${index} has an invalid rest transform`);
    fail(Array.isArray(bone.inverseBind) && bone.inverseBind.length === 16
      && bone.inverseBind.every(Number.isFinite), 'NG-E-RIG-SKELETON', `bone ${index} has an invalid inverse bind`);
    fail(typeof bone.name === 'string' && bone.name.length > 0, 'NG-E-RIG-SKELETON', `bone ${index} has no name`);
  });

  const positions = new Float32Array(vertices * 3);
  const normals = new Float32Array(vertices * 3);
  const uvs = new Float32Array(vertices * 2);
  const joints = new Uint32Array(vertices * 4);
  const weights = new Float32Array(vertices * 4);
  for (let vertex = 0; vertex < vertices; ++vertex) {
    const at = vertex * VERTEX_BYTES;
    for (let axis = 0; axis < 3; ++axis) {
      positions[vertex * 3 + axis] = bytes.readFloatLE(at + axis * 4);
      normals[vertex * 3 + axis] = bytes.readFloatLE(at + 12 + axis * 4);
    }
    for (let axis = 0; axis < 2; ++axis) uvs[vertex * 2 + axis] = bytes.readFloatLE(at + 24 + axis * 4);
    let total = 0;
    for (let influence = 0; influence < 4; ++influence) {
      const joint = bytes.readUInt32LE(at + 32 + influence * 4);
      const weight = bytes.readFloatLE(at + 48 + influence * 4);
      fail(joint < bones.length, 'NG-E-RIG-SKIN', `vertex ${vertex} references bone ${joint} of ${bones.length}`);
      fail(Number.isFinite(weight) && weight >= 0, 'NG-E-RIG-SKIN', `vertex ${vertex} has an invalid skin weight`);
      joints[vertex * 4 + influence] = joint;
      weights[vertex * 4 + influence] = weight;
      total += weight;
    }
    // Weights are normalised here rather than trusted: a garment binder that projects a
    // vertex onto a body triangle inherits whatever this vertex claims, and an
    // unnormalised source silently scales every transferred weight.
    fail(Math.abs(total - 1) < 0.02, 'NG-E-RIG-SKIN', `vertex ${vertex} skin weights sum to ${total.toFixed(4)}`);
    if (Math.abs(total - 1) > 1e-6) {
      for (let influence = 0; influence < 4; ++influence) weights[vertex * 4 + influence] /= total;
    }
  }

  const indices = new Uint32Array(indexCount);
  for (let index = 0; index < indexCount; ++index) {
    const value = bytes.readUInt32LE(indexOffset + index * 4);
    fail(value < vertices, 'NG-E-RIG-FORMAT', `index ${index} references vertex ${value} of ${vertices}`);
    indices[index] = value;
  }

  const parts = Array.isArray(mesh.parts) && mesh.parts.length ? mesh.parts.map((part) => ({
    name: String(part.name ?? ''), material: String(part.material ?? ''),
    firstIndex: part.firstIndex, indexCount: part.indexCount,
  })) : [{ name: 'all', material: '', firstIndex: 0, indexCount }];

  return {
    format: header.format,
    coordinateSystem: header.coordinateSystem ?? 'left-handed-y-up',
    vertices, indices, positions, normals, uvs, joints, weights,
    bones: bones.map((bone, index) => ({
      index, name: bone.name, key: boneKey(bone.name), side: sideOf(bone.name),
      parent: bone.parent, rest: bone.rest.slice(0, 10), inverseBind: bone.inverseBind.slice(0, 16),
    })),
    parts,
    morphs: Array.isArray(header.morphs) ? header.morphs : [],
    sourceBytes: bytes.length,
  };
}

export async function loadCharacterAsset(file) {
  const header = JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
  const binaryName = header.binary ?? 'animation.bin';
  const binary = await fs.readFile(path.join(path.dirname(file), binaryName));
  const character = decodeCharacter(header, binary);
  character.file = file;
  character.directory = path.dirname(file);
  character.sourceSha256 = header.sourceSha256 ?? null;
  character.sourceHash = header.binaryHash ?? null;
  return character;
}

// ------------------------------------------------------------------ skeleton
// Rest world matrices in bone order. A pose is expressed as extra local rotations,
// not as animated clip data: diagnostics need "arm raised 60°", not a frame lookup.
export function restPose(bones) {
  const world = new Array(bones.length);
  for (let index = 0; index < bones.length; ++index) {
    const bone = bones[index];
    const [tx, ty, tz, qx, qy, qz, qw, sx, sy, sz] = bone.rest;
    const local = nodeMatrix({
      translation: [tx, ty, tz], rotation: [qx, qy, qz, qw], scale: [sx, sy, sz],
    });
    world[index] = bone.parent < 0 ? local : multiplyMatrix(world[bone.parent], local);
  }
  return world;
}

export function bonePositions(bones, world = restPose(bones)) {
  return world.map((matrix) => [matrix[12], matrix[13], matrix[14]]);
}

// `pose` maps a bone name or key to an extra rotation quaternion. The rotation is
// applied in the bone's local frame, after the rest orientation, which is what a
// diagnostic pose like "raise the left arm" means.
export function poseSkeleton(bones, pose = {}) {
  const byName = new Map(), byKey = new Map();
  bones.forEach((bone, index) => {
    byName.set(bone.name, index);
    byName.set(identifier(bone.name), index);
    if (!byKey.has(bone.key)) byKey.set(bone.key, index);
  });
  const overrides = new Map();
  for (const [name, rotation] of Object.entries(pose ?? {})) {
    const index = byName.get(name) ?? byKey.get(boneKey(name));
    fail(index !== undefined, 'NG-E-RIG-POSE', `pose references unknown bone ${name}`);
    fail(Array.isArray(rotation) && rotation.length === 4 && rotation.every(Number.isFinite),
      'NG-E-RIG-POSE', `pose rotation for ${name} must be a quaternion`);
    overrides.set(index, rotation);
  }
  const world = new Array(bones.length);
  const posed = bones.map((bone, index) => {
    const rotation = overrides.get(index);
    if (!rotation) return bone.rest;
    const rest = bone.rest;
    const combined = multiplyQuaternion([rest[3], rest[4], rest[5], rest[6]], rotation);
    return [rest[0], rest[1], rest[2], ...combined, rest[7], rest[8], rest[9]];
  });
  for (let index = 0; index < bones.length; ++index) {
    const [tx, ty, tz, qx, qy, qz, qw, sx, sy, sz] = posed[index];
    const local = nodeMatrix({ translation: [tx, ty, tz], rotation: [qx, qy, qz, qw], scale: [sx, sy, sz] });
    world[index] = bones[index].parent < 0 ? local : multiplyMatrix(world[bones[index].parent], local);
  }
  return world;
}

// palette[i] = world[i] * inverseBind[i]; the joint matrices the runtime uploads.
export function skinPalette(bones, world = restPose(bones)) {
  return world.map((matrix, index) => multiplyMatrix(matrix, bones[index].inverseBind));
}

export function skinPositions(character, palette) {
  const { vertices, positions, joints, weights } = character;
  const out = new Float32Array(vertices * 3);
  const point = [0, 0, 0];
  for (let vertex = 0; vertex < vertices; ++vertex) {
    const source = [positions[vertex * 3], positions[vertex * 3 + 1], positions[vertex * 3 + 2]];
    let x = 0, y = 0, z = 0;
    for (let influence = 0; influence < 4; ++influence) {
      const weight = weights[vertex * 4 + influence];
      if (weight === 0) continue;
      point[0] = source[0]; point[1] = source[1]; point[2] = source[2];
      const transformed = transformPoint(palette[joints[vertex * 4 + influence]], point);
      x += transformed[0] * weight; y += transformed[1] * weight; z += transformed[2] * weight;
    }
    out[vertex * 3] = x; out[vertex * 3 + 1] = y; out[vertex * 3 + 2] = z;
  }
  return out;
}

export function skinNormals(character, palette) {
  const { vertices, normals, joints, weights } = character;
  const out = new Float32Array(vertices * 3);
  for (let vertex = 0; vertex < vertices; ++vertex) {
    let x = 0, y = 0, z = 0;
    for (let influence = 0; influence < 4; ++influence) {
      const weight = weights[vertex * 4 + influence];
      if (weight === 0) continue;
      const transformed = transformDirection(palette[joints[vertex * 4 + influence]],
        [normals[vertex * 3], normals[vertex * 3 + 1], normals[vertex * 3 + 2]]);
      x += transformed[0] * weight; y += transformed[1] * weight; z += transformed[2] * weight;
    }
    const length = Math.hypot(x, y, z) || 1;
    out[vertex * 3] = x / length; out[vertex * 3 + 1] = y / length; out[vertex * 3 + 2] = z / length;
  }
  return out;
}

// --------------------------------------------------------------- bone lookup
export function findBone(bones, name) {
  const key = boneKey(name);
  const direct = bones.find((bone) => bone.name === name || identifier(bone.name) === identifier(name));
  if (direct) return direct;
  return bones.find((bone) => bone.key === key) ?? null;
}

export function boneChain(bones, name) {
  const bone = findBone(bones, name);
  if (!bone) return [];
  const chain = [bone.index];
  let parent = bone.parent;
  while (parent >= 0) { chain.push(parent); parent = bones[parent].parent; }
  return chain.reverse();
}

// ------------------------------------------------------------ region extract
// A region is a set of vertices plus the triangles whose three corners are all in
// the set. Hair binding and garment diagnosis both need "the head", so the rule is
// shared: dominant bone weight, in metres, in the character's own frame.
export function dominantBone(character, vertex) {
  let best = -1, bestWeight = 0;
  for (let influence = 0; influence < 4; ++influence) {
    const weight = character.weights[vertex * 4 + influence];
    if (weight > bestWeight) { bestWeight = weight; best = character.joints[vertex * 4 + influence]; }
  }
  return { bone: best, weight: bestWeight };
}

export function selectVertices(character, predicate) {
  const mask = new Uint8Array(character.vertices);
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    if (predicate(vertex, character)) mask[vertex] = 1;
  }
  return mask;
}

// Head-and-scalp selection. `head` is a bone name; everything weighted mostly to
// that bone above its origin is scalp. A hair generator that fakes this with a
// sphere cap produces a hairline that does not follow the character's skull.
export function headVertexMask(character, { headBone = 'head', neckBone = 'neck', minWeight = 0.5 } = {}) {
  const head = findBone(character.bones, headBone);
  fail(head, 'NG-E-RIG-SKELETON', `character has no ${headBone} bone; hair binding needs a real skull joint`);
  const headChain = new Set();
  // Descendants of the head bone (eyes, jaw, hair helpers) move with the skull.
  const children = new Map();
  for (const bone of character.bones) {
    if (bone.parent >= 0) {
      const list = children.get(bone.parent) ?? [];
      list.push(bone.index);
      children.set(bone.parent, list);
    }
  }
  const stack = [head.index];
  while (stack.length) {
    const index = stack.pop();
    headChain.add(index);
    for (const child of children.get(index) ?? []) stack.push(child);
  }
  const neck = findBone(character.bones, neckBone);
  const headOrigin = bonePositions(character.bones)[head.index][1];
  const neckOrigin = neck ? bonePositions(character.bones)[neck.index][1] : headOrigin;
  return selectVertices(character, (vertex) => {
    const { bone, weight } = dominantBone(character, vertex);
    if (headChain.has(bone) && weight >= minWeight) return true;
    // A vertex just above the neck joint that is only partly weighted to the head
    // still belongs to the scalp: rejecting it leaves a bald band at the nape.
    if (neck && bone === neck.index && weight >= minWeight) {
      return character.positions[vertex * 3 + 1] > (headOrigin + neckOrigin) * 0.5;
    }
    return false;
  });
}

// Converts a vertex mask into an indexed submesh. Triangles keep their winding, and
// `sourceVertex` maps the compact index back to the character so bindings survive.
export function extractSubmesh(character, mask, positions) {
  const remap = new Int32Array(character.vertices).fill(-1);
  let count = 0;
  for (let vertex = 0; vertex < character.vertices; ++vertex) if (mask[vertex]) remap[vertex] = count++;
  const triangles = [];
  const { indices } = character;
  for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    if (remap[a] < 0 || remap[b] < 0 || remap[c] < 0) continue;
    triangles.push(remap[a], remap[b], remap[c]);
  }
  fail(count > 0 && triangles.length >= 3, 'NG-E-RIG-REGION', 'the selected region has no closed triangles');
  const sourceVertex = new Uint32Array(count);
  const localPositions = new Float32Array(count * 3);
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    const local = remap[vertex];
    if (local < 0) continue;
    sourceVertex[local] = vertex;
    localPositions[local * 3] = positions[vertex * 3];
    localPositions[local * 3 + 1] = positions[vertex * 3 + 1];
    localPositions[local * 3 + 2] = positions[vertex * 3 + 2];
  }
  return {
    vertices: count, positions: localPositions,
    indices: Uint32Array.from(triangles), sourceVertex,
    triangles: triangles.length / 3,
  };
}

// ------------------------------------------------------------- mesh metrics
export function triangleNormal(positions, a, b, c) {
  const ab = [positions[b * 3] - positions[a * 3], positions[b * 3 + 1] - positions[a * 3 + 1], positions[b * 3 + 2] - positions[a * 3 + 2]];
  const ac = [positions[c * 3] - positions[a * 3], positions[c * 3 + 1] - positions[a * 3 + 1], positions[c * 3 + 2] - positions[a * 3 + 2]];
  const normal = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
  const length = Math.hypot(...normal);
  return { normal: length > 0 ? normal.map((v) => v / length) : [0, 1, 0], area: length * 0.5 };
}

export function closestPointOnTriangle(point, a, b, c) {
  const sub = (p, q) => [p[0] - q[0], p[1] - q[1], p[2] - q[2]];
  const dot = (p, q) => p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
  const add = (p, q) => [p[0] + q[0], p[1] + q[1], p[2] + q[2]];
  const scale = (p, s) => [p[0] * s, p[1] * s, p[2] * s];
  const ab = sub(b, a), ac = sub(c, a), ap = sub(point, a);
  const d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return { point: a, bary: [1, 0, 0] };
  const bp = sub(point, b), d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return { point: b, bary: [0, 1, 0] };
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const v = d1 / (d1 - d3);
    return { point: add(a, scale(ab, v)), bary: [1 - v, v, 0] };
  }
  const cp = sub(point, c), d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return { point: c, bary: [0, 0, 1] };
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const w = d2 / (d2 - d6);
    return { point: add(a, scale(ac, w)), bary: [1 - w, 0, w] };
  }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return { point: add(b, scale(sub(c, b), w)), bary: [0, 1 - w, w] };
  }
  const denominator = 1 / (va + vb + vc);
  const v = vb * denominator, w = vc * denominator;
  return { point: add(add(a, scale(ab, v)), scale(ac, w)), bary: [1 - v - w, v, w] };
}

// Brute-force nearest triangle, with an optional reject callback. Body meshes in
// this engine are ≤100k vertices, and both the scalp binder and the garment binder
// run once per authoring action, not per frame — an index would be premature.
export function nearestTriangle(positions, indices, point, { reject = null, stride = 1 } = {}) {
  let best = { distance: Infinity, triangle: -1, bary: [1, 0, 0], point: null };
  const triangles = indices.length / 3;
  for (let triangle = 0; triangle < triangles; triangle += stride) {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    const pa = [positions[a * 3], positions[a * 3 + 1], positions[a * 3 + 2]];
    const pb = [positions[b * 3], positions[b * 3 + 1], positions[b * 3 + 2]];
    const pc = [positions[c * 3], positions[c * 3 + 1], positions[c * 3 + 2]];
    const hit = closestPointOnTriangle(point, pa, pb, pc);
    const distance = Math.hypot(hit.point[0] - point[0], hit.point[1] - point[1], hit.point[2] - point[2]);
    if (distance >= best.distance) continue;
    if (reject && reject({ triangle, a, b, c, distance, point: hit.point, bary: hit.bary })) continue;
    best = { distance, triangle, bary: hit.bary, point: hit.point };
  }
  return best;
}

// Character frame convention: +Z forward, +X the character's own left, +Y up.
// Sides in hair regions and weight diagnostics are named from this, never from the
// viewer's left.
export function characterFrame(character, world = restPose(character.bones)) {
  const head = findBone(character.bones, 'head');
  const origin = head ? [world[head.index][12], world[head.index][13], world[head.index][14]] : [0, 0, 0];
  return { forward: [0, 0, 1], left: [1, 0, 0], up: [0, 1, 0], headOrigin: origin };
}

// Azimuth in degrees around the character's up axis: 0 is front, +90 the character's
// left, 180 back, -90 the character's right. Pitch is measured down from the crown.
export function sphericalOf(point, centre) {
  const dx = point[0] - centre[0], dy = point[1] - centre[1], dz = point[2] - centre[2];
  const radius = Math.hypot(dx, dy, dz) || 1e-9;
  return {
    radius,
    azimuthDegrees: (Math.atan2(dx, dz) * 180) / Math.PI,
    polarDegrees: (Math.acos(Math.max(-1, Math.min(1, dy / radius))) * 180) / Math.PI,
  };
}

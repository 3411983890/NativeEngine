#!/usr/bin/env node
// B7: minimal glTF 2.0 importer that feeds the content-addressed geometry package.
//
// Supported subset (documented in .developmentAgent/rendering/GEOMETRY-PACKAGE.md §8):
//   - .gltf JSON only; buffers embedded as base64 data URIs or a sibling .bin
//   - exactly one scene; transform-free node graph; one or more indexed
//     TRIANGLES primitives with POSITION and optional NORMAL
//   - accessor.byteOffset, bufferView.byteOffset and bufferView.byteStride are
//     honoured, so interleaved vertex layouts load without repacking
// Rejected with stable codes: skins, animation, Draco/KHR extensions, sparse
// accessors, morph targets, non-triangle modes and non-2.0 documents.
//
// The package adapter reuses geometry-package.mjs for canonical JSON, package
// digest hashing, manifest constants and manifest writing. Its cluster packing
// mirrors the rules documented in §3 of .developmentAgent/rendering/GEOMETRY-PACKAGE.md because
// geometry-package.mjs does not export its internal createPackageData(); the
// tests run the emitted package through the real verifyPackage() so the two
// cannot drift silently.
import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  FORMAT_VERSION, GeometryError, MANIFEST_NAME, MAX_CLUSTER_TRIANGLES,
  MAX_CLUSTER_VERTICES, PACKAGE_KIND, computePackageDigest, stableJSON,
} from './geometry-package.mjs';

export const TOOL_VERSION = '0.1.0';
export const MESH_FORMAT = 'nativegpu.gltf-mesh';
export const MESH_FORMAT_VERSION = 1;
export const GENERATOR_NAME = 'nativegpu-offline-gltf';
export const GLTF_GENERATOR = 'gltf-2.0-subset';
export const ERROR_CODES = Object.freeze({
  MISSING: 'NG-E-GLTF-MISSING',
  UNSUPPORTED: 'NG-E-GLTF-UNSUPPORTED',
  BUFFER: 'NG-E-GLTF-BUFFER',
  ACCESSOR: 'NG-E-GLTF-ACCESSOR',
  ARGUMENT: 'NG-E-GLTF-ARGUMENT',
  IO: 'NG-E-GLTF-IO',
});

export class GltfError extends GeometryError {
  constructor(code, message, details = []) {
    super(code, message, details);
    this.name = 'GltfError';
  }
}

const FLOAT = 5126;
const UNSIGNED_SHORT = 5123;
const UNSIGNED_INT = 5125;
const TRIANGLES = 4;
const DATA_URI = /^data:([^,]*?),(.*)$/s;
const IDENTITY_MATRIX = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const IDENTITY_TRS = Object.freeze({ translation: [0, 0, 0], rotation: [0, 0, 0, 1], scale: [1, 1, 1] });

const fail = (code, message, details = []) => { throw new GltfError(code, message, details); };
const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const sha256 = data => createHash('sha256').update(data).digest('hex');

const encodeFloat32 = values => {
  const array = values instanceof Float32Array ? values : Float32Array.from(values);
  return Buffer.from(array.buffer, array.byteOffset, array.byteLength);
};
const encodeUint32 = values => {
  const array = values instanceof Uint32Array ? values : Uint32Array.from(values);
  return Buffer.from(array.buffer, array.byteOffset, array.byteLength);
};

function interleave(positions, normals) {
  const count = positions.length / 3;
  const out = new Float32Array(count * 6);
  for (let i = 0; i < count; i++) {
    out[i * 6] = positions[i * 3];
    out[i * 6 + 1] = positions[i * 3 + 1];
    out[i * 6 + 2] = positions[i * 3 + 2];
    out[i * 6 + 3] = normals[i * 3];
    out[i * 6 + 4] = normals[i * 3 + 1];
    out[i * 6 + 5] = normals[i * 3 + 2];
  }
  return out;
}

function boundsOfFlat(positions) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < positions.length; i += 3) {
    for (let axis = 0; axis < 3; axis++) {
      min[axis] = Math.min(min[axis], positions[i + axis]);
      max[axis] = Math.max(max[axis], positions[i + axis]);
    }
  }
  return { min, max };
}

const unionBounds = list => ({
  min: [0, 1, 2].map(axis => Math.min(...list.map(bounds => bounds.min[axis]))),
  max: [0, 1, 2].map(axis => Math.max(...list.map(bounds => bounds.max[axis]))),
});

const sameDirectory = (left, right) => {
  const a = path.resolve(left);
  const b = path.resolve(right);
  return process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
};

// ---------------------------------------------------------------------------
// glTF document -> raw buffers
// ---------------------------------------------------------------------------

function decodeDataUri(uri, index) {
  const match = DATA_URI.exec(uri);
  if (!match) fail(ERROR_CODES.BUFFER, `buffers[${index}].uri is not a valid data URI`);
  if (!/;base64$/i.test(match[1])) fail(ERROR_CODES.BUFFER, `buffers[${index}].uri must be a base64 data URI`);
  const base64 = match[2].replace(/\s+/g, '');
  if (base64.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(base64)) {
    fail(ERROR_CODES.BUFFER, `buffers[${index}].uri contains invalid base64 data`);
  }
  return Buffer.from(base64, 'base64');
}

export async function readBuffers(document, gltfPath) {
  if (!Array.isArray(document.buffers) || document.buffers.length === 0) {
    fail(ERROR_CODES.UNSUPPORTED, 'glTF has no buffers');
  }
  const directory = path.dirname(gltfPath);
  const buffers = [];
  for (let index = 0; index < document.buffers.length; index++) {
    const buffer = document.buffers[index];
    if (!isObject(buffer)) fail(ERROR_CODES.BUFFER, `buffers[${index}] must be an object`);
    const byteLength = buffer.byteLength;
    if (!Number.isSafeInteger(byteLength) || byteLength < 1) {
      fail(ERROR_CODES.BUFFER, `buffers[${index}].byteLength must be a positive integer`);
    }
    const uri = buffer.uri;
    if (typeof uri !== 'string' || uri.length === 0) {
      fail(ERROR_CODES.BUFFER, `buffers[${index}] has no uri; GLB binary chunks are not supported`);
    }
    let bytes;
    let mode;
    let file = null;
    if (uri.startsWith('data:')) {
      bytes = decodeDataUri(uri, index);
      mode = 'embedded';
    } else {
      if (/^[a-z][a-z0-9+.-]*:/i.test(uri) || path.isAbsolute(uri)) {
        fail(ERROR_CODES.BUFFER, `buffers[${index}].uri must be a sibling .bin, not ${JSON.stringify(uri)}`);
      }
      let decoded;
      try {
        decoded = decodeURIComponent(uri);
      } catch {
        fail(ERROR_CODES.BUFFER, `buffers[${index}].uri is not a valid relative URI`);
      }
      const resolved = path.resolve(directory, decoded);
      if (!sameDirectory(path.dirname(resolved), directory)) {
        fail(ERROR_CODES.BUFFER, `buffers[${index}].uri must stay in the glTF directory`);
      }
      file = path.basename(resolved);
      try {
        bytes = await fs.readFile(resolved);
      } catch (error) {
        if (error.code === 'ENOENT' || error.code === 'ENOTDIR') {
          fail(ERROR_CODES.MISSING, `buffers[${index}] file is missing: ${file}`);
        }
        throw error;
      }
      mode = 'sibling';
    }
    if (bytes.length < byteLength) {
      fail(ERROR_CODES.BUFFER, `buffers[${index}] has ${bytes.length} bytes, byteLength is ${byteLength}`);
    }
    const declared = bytes.subarray(0, byteLength);
    buffers.push({ index, mode, file, byteLength, bytes: declared, sha256: sha256(declared) });
  }
  return buffers;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

function readElementSpec(accessor, view, label, role) {
  if (accessor.type === undefined) fail(ERROR_CODES.ACCESSOR, `${label}: accessor.type is required`);
  const componentType = accessor.componentType;
  if (role === 'indices') {
    if (accessor.type !== 'SCALAR') fail(ERROR_CODES.UNSUPPORTED, `${label}: index accessor must be SCALAR`);
    if (componentType !== UNSIGNED_SHORT && componentType !== UNSIGNED_INT) {
      fail(ERROR_CODES.UNSUPPORTED, `${label}: only UNSIGNED_SHORT/UNSIGNED_INT indices are supported`);
    }
    if (accessor.normalized === true) fail(ERROR_CODES.UNSUPPORTED, `${label}: normalized indices are not supported`);
    if (view.byteStride !== undefined) fail(ERROR_CODES.ACCESSOR, `${label}: bufferView.byteStride must not be defined for indices`);
    return { componentSize: componentType === UNSIGNED_SHORT ? 2 : 4, components: 1, indices: true };
  }
  if (accessor.type !== 'VEC3') fail(ERROR_CODES.UNSUPPORTED, `${label}: ${role} accessor must be VEC3`);
  if (componentType !== FLOAT) fail(ERROR_CODES.UNSUPPORTED, `${label}: only FLOAT ${role} attributes are supported`);
  if (accessor.normalized === true) fail(ERROR_CODES.UNSUPPORTED, `${label}: normalized attributes are not supported`);
  return { componentSize: 4, components: 3, indices: false };
}

function readAccessor(document, buffers, accessorIndex, label, role) {
  if (!Number.isSafeInteger(accessorIndex) || accessorIndex < 0) {
    fail(ERROR_CODES.ACCESSOR, `${label}: accessor index ${JSON.stringify(accessorIndex)} is invalid`);
  }
  const accessor = Array.isArray(document.accessors) ? document.accessors[accessorIndex] : undefined;
  if (!isObject(accessor)) fail(ERROR_CODES.ACCESSOR, `${label}: accessor ${accessorIndex} does not exist`);
  if (accessor.sparse !== undefined) fail(ERROR_CODES.UNSUPPORTED, `${label}: sparse accessors are not supported`);
  if (accessor.bufferView === undefined) fail(ERROR_CODES.UNSUPPORTED, `${label}: accessor ${accessorIndex} has no bufferView`);
  const viewIndex = accessor.bufferView;
  if (!Number.isSafeInteger(viewIndex) || viewIndex < 0) fail(ERROR_CODES.ACCESSOR, `${label}: bufferView index is invalid`);
  const view = Array.isArray(document.bufferViews) ? document.bufferViews[viewIndex] : undefined;
  if (!isObject(view)) fail(ERROR_CODES.ACCESSOR, `${label}: bufferView ${viewIndex} does not exist`);
  const bufferIndex = view.buffer;
  if (!Number.isSafeInteger(bufferIndex) || bufferIndex < 0 || bufferIndex >= buffers.length) {
    fail(ERROR_CODES.BUFFER, `${label}: bufferView ${viewIndex} references missing buffer ${JSON.stringify(bufferIndex)}`);
  }
  const buffer = buffers[bufferIndex];
  const count = accessor.count;
  if (!Number.isSafeInteger(count) || count < 1) fail(ERROR_CODES.ACCESSOR, `${label}: accessor.count must be a positive integer`);
  const byteOffset = accessor.byteOffset ?? 0;
  if (!Number.isSafeInteger(byteOffset) || byteOffset < 0) fail(ERROR_CODES.ACCESSOR, `${label}: accessor.byteOffset must be a non-negative integer`);
  const viewOffset = view.byteOffset ?? 0;
  if (!Number.isSafeInteger(viewOffset) || viewOffset < 0) fail(ERROR_CODES.ACCESSOR, `${label}: bufferView.byteOffset must be a non-negative integer`);
  const viewLength = view.byteLength;
  if (!Number.isSafeInteger(viewLength) || viewLength < 0) fail(ERROR_CODES.BUFFER, `${label}: bufferView.byteLength must be a non-negative integer`);
  if (viewOffset + viewLength > buffer.byteLength) {
    fail(ERROR_CODES.BUFFER, `${label}: bufferView ${viewIndex} exceeds buffer ${bufferIndex} (${buffer.byteLength} bytes)`);
  }
  const spec = readElementSpec(accessor, view, label, role);
  const elementSize = spec.componentSize * spec.components;
  const stride = view.byteStride ?? elementSize;
  if (!Number.isSafeInteger(stride) || stride < elementSize || stride % spec.componentSize !== 0) {
    fail(ERROR_CODES.ACCESSOR, `${label}: invalid byteStride ${JSON.stringify(view.byteStride)} for ${elementSize}-byte elements`);
  }
  if ((viewOffset + byteOffset) % spec.componentSize !== 0) {
    fail(ERROR_CODES.ACCESSOR, `${label}: accessor offset is not aligned to ${spec.componentSize} bytes`);
  }
  const required = byteOffset + (count - 1) * stride + elementSize;
  if (!Number.isSafeInteger(required) || required > viewLength) {
    fail(ERROR_CODES.ACCESSOR, `${label}: accessor ${accessorIndex} needs ${required} bytes but bufferView ${viewIndex} has ${viewLength}`);
  }
  if (spec.indices) {
    const values = new Uint32Array(count);
    for (let i = 0; i < count; i++) {
      values[i] = spec.componentSize === 2
        ? buffer.bytes.readUInt16LE(viewOffset + byteOffset + i * stride)
        : buffer.bytes.readUInt32LE(viewOffset + byteOffset + i * stride);
    }
    return values;
  }
  const values = new Float32Array(count * 3);
  for (let i = 0; i < count; i++) {
    for (let component = 0; component < 3; component++) {
      const value = buffer.bytes.readFloatLE(viewOffset + byteOffset + i * stride + component * 4);
      if (!Number.isFinite(value)) fail(ERROR_CODES.ACCESSOR, `${label}: accessor ${accessorIndex} contains a non-finite value`);
      values[i * 3 + component] = value;
    }
  }
  return values;
}

// ---------------------------------------------------------------------------
// Document / scene / primitive validation
// ---------------------------------------------------------------------------

function assertSupportedDocument(document) {
  if (!isObject(document)) fail(ERROR_CODES.ACCESSOR, 'glTF document must be a JSON object');
  if (!isObject(document.asset)) fail(ERROR_CODES.MISSING, 'glTF asset metadata is missing');
  if (document.asset.version !== '2.0') {
    fail(ERROR_CODES.UNSUPPORTED, `glTF asset.version ${JSON.stringify(document.asset.version)} is not 2.0`);
  }
  if (document.asset.minVersion !== undefined && document.asset.minVersion !== '2.0') {
    fail(ERROR_CODES.UNSUPPORTED, `glTF asset.minVersion ${JSON.stringify(document.asset.minVersion)} is not supported`);
  }
  if (document.extensions && Object.keys(document.extensions).length > 0) {
    fail(ERROR_CODES.UNSUPPORTED, 'Top-level glTF extensions are not supported');
  }
  for (const key of ['extensionsUsed', 'extensionsRequired']) {
    const value = document[key];
    if (value === undefined) continue;
    if (!Array.isArray(value) || value.some(item => typeof item !== 'string')) {
      fail(ERROR_CODES.UNSUPPORTED, `glTF ${key} must be an array of extension names`);
    }
    if (value.length > 0) {
      fail(ERROR_CODES.UNSUPPORTED, `glTF extensions are not supported: ${[...new Set(value)].join(', ')}`);
    }
  }
  if (document.skins !== undefined && (!Array.isArray(document.skins) || document.skins.length > 0)) {
    fail(ERROR_CODES.UNSUPPORTED, 'glTF skins are not supported');
  }
  if (document.animations !== undefined && (!Array.isArray(document.animations) || document.animations.length > 0)) {
    fail(ERROR_CODES.UNSUPPORTED, 'glTF animations are not supported');
  }
  if (!Array.isArray(document.meshes) || document.meshes.length === 0) {
    fail(ERROR_CODES.UNSUPPORTED, 'glTF has no meshes');
  }
}

function assertIdentityTransform(node, nodeIndex) {
  if (node.matrix !== undefined) {
    if (!Array.isArray(node.matrix) || node.matrix.length !== 16
      || !node.matrix.every((value, index) => value === IDENTITY_MATRIX[index])) {
      fail(ERROR_CODES.UNSUPPORTED, `nodes[${nodeIndex}].matrix is not the identity transform`);
    }
  }
  for (const [key, identity] of Object.entries(IDENTITY_TRS)) {
    const value = node[key];
    if (value === undefined) continue;
    if (!Array.isArray(value) || value.length !== identity.length
      || !value.every((item, index) => item === identity[index])) {
      fail(ERROR_CODES.UNSUPPORTED, `nodes[${nodeIndex}].${key} is not the identity transform`);
    }
  }
}

function collectSceneMeshes(document) {
  if (!Array.isArray(document.scenes) || document.scenes.length !== 1) {
    fail(ERROR_CODES.UNSUPPORTED, `exactly one scene is supported (found ${Array.isArray(document.scenes) ? document.scenes.length : 0})`);
  }
  if ((document.scene ?? 0) !== 0) fail(ERROR_CODES.UNSUPPORTED, `only scene 0 is supported (found ${document.scene})`);
  const scene = document.scenes[0];
  if (!isObject(scene)) fail(ERROR_CODES.UNSUPPORTED, 'scene 0 must be an object');
  if (!Array.isArray(document.nodes) || document.nodes.length === 0) fail(ERROR_CODES.UNSUPPORTED, 'glTF has no nodes');
  if (!Array.isArray(scene.nodes) || scene.nodes.length === 0) fail(ERROR_CODES.UNSUPPORTED, 'scene 0 references no nodes');
  const meshIndices = [];
  const visited = new Set();
  const walk = nodeIndex => {
    if (!Number.isSafeInteger(nodeIndex) || nodeIndex < 0 || nodeIndex >= document.nodes.length) {
      fail(ERROR_CODES.ACCESSOR, `scene references unknown node ${JSON.stringify(nodeIndex)}`);
    }
    if (visited.has(nodeIndex)) return;
    visited.add(nodeIndex);
    const node = document.nodes[nodeIndex];
    if (!isObject(node)) fail(ERROR_CODES.ACCESSOR, `nodes[${nodeIndex}] must be an object`);
    if (node.extensions && Object.keys(node.extensions).length > 0) {
      fail(ERROR_CODES.UNSUPPORTED, `nodes[${nodeIndex}].extensions are not supported`);
    }
    if (node.skin !== undefined) fail(ERROR_CODES.UNSUPPORTED, `nodes[${nodeIndex}].skin is not supported (skinning)`);
    assertIdentityTransform(node, nodeIndex);
    if (node.mesh !== undefined) {
      if (!Number.isSafeInteger(node.mesh) || node.mesh < 0 || node.mesh >= document.meshes.length) {
        fail(ERROR_CODES.ACCESSOR, `nodes[${nodeIndex}].mesh ${JSON.stringify(node.mesh)} does not exist`);
      }
      if (meshIndices.includes(node.mesh)) fail(ERROR_CODES.UNSUPPORTED, `mesh ${node.mesh} is instanced by multiple nodes`);
      meshIndices.push(node.mesh);
    }
    const children = node.children ?? [];
    if (!Array.isArray(children)) fail(ERROR_CODES.ACCESSOR, `nodes[${nodeIndex}].children must be an array`);
    for (const child of children) walk(child);
  };
  for (const nodeIndex of scene.nodes) walk(nodeIndex);
  if (meshIndices.length === 0) fail(ERROR_CODES.UNSUPPORTED, 'scene 0 contains no mesh nodes');
  return meshIndices;
}

function readMeshes(document, buffers, meshIndices) {
  const primitives = [];
  for (const meshIndex of meshIndices) {
    const mesh = document.meshes[meshIndex];
    if (!isObject(mesh)) fail(ERROR_CODES.ACCESSOR, `meshes[${meshIndex}] must be an object`);
    if (mesh.weights !== undefined) fail(ERROR_CODES.UNSUPPORTED, `meshes[${meshIndex}] uses morph target weights`);
    if (!Array.isArray(mesh.primitives) || mesh.primitives.length === 0) {
      fail(ERROR_CODES.UNSUPPORTED, `meshes[${meshIndex}] has no primitives`);
    }
    for (let primitiveIndex = 0; primitiveIndex < mesh.primitives.length; primitiveIndex++) {
      const primitive = mesh.primitives[primitiveIndex];
      const label = `meshes[${meshIndex}].primitives[${primitiveIndex}]`;
      if (!isObject(primitive)) fail(ERROR_CODES.ACCESSOR, `${label} must be an object`);
      if (primitive.extensions && Object.keys(primitive.extensions).length > 0) {
        fail(ERROR_CODES.UNSUPPORTED, `${label}.extensions are not supported (e.g. Draco)`);
      }
      if (primitive.targets !== undefined) fail(ERROR_CODES.UNSUPPORTED, `${label}.targets are not supported (morph targets)`);
      const mode = primitive.mode ?? TRIANGLES;
      if (mode !== TRIANGLES) fail(ERROR_CODES.UNSUPPORTED, `${label}.mode ${JSON.stringify(mode)} is not TRIANGLES (4)`);
      if (!isObject(primitive.attributes)) fail(ERROR_CODES.ACCESSOR, `${label}.attributes must be an object`);
      if (primitive.attributes.JOINTS_0 !== undefined || primitive.attributes.WEIGHTS_0 !== undefined) {
        fail(ERROR_CODES.UNSUPPORTED, `${label} has skinning attributes`);
      }
      if (primitive.attributes.POSITION === undefined) fail(ERROR_CODES.ACCESSOR, `${label} has no POSITION attribute`);
      if (primitive.indices === undefined) fail(ERROR_CODES.UNSUPPORTED, `${label} is not indexed`);
      const positions = readAccessor(document, buffers, primitive.attributes.POSITION, `${label}.POSITION`, 'position');
      const normals = primitive.attributes.NORMAL === undefined
        ? null
        : readAccessor(document, buffers, primitive.attributes.NORMAL, `${label}.NORMAL`, 'normal');
      const indices = readAccessor(document, buffers, primitive.indices, `${label}.indices`, 'indices');
      if (indices.length % 3 !== 0) {
        fail(ERROR_CODES.ACCESSOR, `${label} index count ${indices.length} is not a multiple of 3`);
      }
      const vertexCount = positions.length / 3;
      for (let index = 0; index < indices.length; index++) {
        if (indices[index] >= vertexCount) {
          fail(ERROR_CODES.ACCESSOR, `${label} index ${indices[index]} exceeds the ${vertexCount}-vertex POSITION accessor`);
        }
      }
      primitives.push({ label, positions, normals, indices });
    }
  }
  return primitives;
}

function assembleMesh(primitives) {
  let vertexCount = 0;
  let triangleCount = 0;
  for (const primitive of primitives) {
    vertexCount += primitive.positions.length / 3;
    triangleCount += primitive.indices.length / 3;
  }
  if (vertexCount === 0) fail(ERROR_CODES.ACCESSOR, 'glTF mesh has no vertices');
  if (vertexCount > 0xffffffff) fail(ERROR_CODES.UNSUPPORTED, 'more than 2^32-1 vertices are not supported');
  const hasNormals = primitives.every(primitive => primitive.normals !== null);
  const positions = new Float32Array(vertexCount * 3);
  const normals = hasNormals ? new Float32Array(vertexCount * 3) : null;
  const indices = new Uint32Array(triangleCount * 3);
  let vertexBase = 0;
  let indexBase = 0;
  for (const primitive of primitives) {
    positions.set(primitive.positions, vertexBase * 3);
    if (normals !== null) normals.set(primitive.normals, vertexBase * 3);
    for (let index = 0; index < primitive.indices.length; index++) {
      indices[indexBase + index] = primitive.indices[index] + vertexBase;
    }
    vertexBase += primitive.positions.length / 3;
    indexBase += primitive.indices.length;
  }
  return { positions, normals, indices, hasNormals };
}

// ---------------------------------------------------------------------------
// Public loader
// ---------------------------------------------------------------------------

export async function load(file) {
  if (typeof file !== 'string' || file.length === 0) fail(ERROR_CODES.MISSING, 'load() requires a .gltf path');
  const filePath = path.resolve(file);
  let fileBytes;
  try {
    fileBytes = await fs.readFile(filePath);
  } catch (error) {
    if (error.code === 'ENOENT' || error.code === 'ENOTDIR') fail(ERROR_CODES.MISSING, `glTF file not found: ${filePath}`);
    throw error;
  }
  if (fileBytes.length >= 4 && fileBytes.readUInt32LE(0) === 0x46546c67) {
    fail(ERROR_CODES.UNSUPPORTED, 'binary GLB containers are not supported; use .gltf with an embedded or sibling buffer');
  }
  if (path.extname(filePath).toLowerCase() === '.glb') {
    fail(ERROR_CODES.UNSUPPORTED, 'GLB containers are not supported; use .gltf with an embedded or sibling buffer');
  }
  let document;
  try {
    document = JSON.parse(fileBytes.toString('utf8').replace(/^\uFEFF/, ''));
  } catch (error) {
    fail(ERROR_CODES.ACCESSOR, `glTF JSON is invalid: ${error.message}`);
  }
  assertSupportedDocument(document);
  const buffers = await readBuffers(document, filePath);
  const meshIndices = collectSceneMeshes(document);
  const primitives = readMeshes(document, buffers, meshIndices);
  const assembled = assembleMesh(primitives);
  return {
    positions: assembled.positions,
    indices: assembled.indices,
    normals: assembled.normals,
    source: {
      kind: 'gltf',
      generator: GLTF_GENERATOR,
      file: path.basename(filePath),
      bytes: fileBytes.length,
      sha256: sha256(fileBytes),
      asset: {
        version: document.asset.version,
        generator: typeof document.asset.generator === 'string' ? document.asset.generator : null,
      },
      meshes: meshIndices.length,
      primitives: primitives.length,
      hasNormals: assembled.hasNormals,
      buffers: buffers.map(buffer => ({ mode: buffer.mode, file: buffer.file, bytes: buffer.byteLength, sha256: buffer.sha256 })),
    },
  };
}

// ---------------------------------------------------------------------------
// Mesh validation and cluster packaging (mirrors geometry-package §3)
// ---------------------------------------------------------------------------

function toFloat32Array(value, label) {
  const source = Array.isArray(value) ? value : value instanceof Float32Array ? value : null;
  if (source === null) fail(ERROR_CODES.ACCESSOR, `${label} must be an array or Float32Array`);
  if (Array.isArray(source)) {
    for (let index = 0; index < source.length; index++) {
      if (typeof source[index] !== 'number' || !Number.isFinite(source[index])) {
        fail(ERROR_CODES.ACCESSOR, `${label}[${index}] is not a finite number`);
      }
    }
  }
  const array = source instanceof Float32Array ? source : Float32Array.from(source);
  for (let index = 0; index < array.length; index++) {
    if (!Number.isFinite(array[index])) fail(ERROR_CODES.ACCESSOR, `${label}[${index}] is not a finite number`);
  }
  return array;
}

function toUint32Array(value, label) {
  const source = Array.isArray(value) ? value : value instanceof Uint32Array ? value : null;
  if (source === null) fail(ERROR_CODES.ACCESSOR, `${label} must be an array or Uint32Array`);
  if (Array.isArray(source)) {
    for (let index = 0; index < source.length; index++) {
      if (!Number.isSafeInteger(source[index]) || source[index] < 0 || source[index] > 0xffffffff) {
        fail(ERROR_CODES.ACCESSOR, `${label}[${index}] is not an unsigned 32-bit integer`);
      }
    }
  }
  const array = source instanceof Uint32Array ? source : Uint32Array.from(source);
  return array;
}

function canonicalizeFloat32(array) {
  let negativeZero = false;
  for (let index = 0; index < array.length; index++) {
    if (Object.is(array[index], -0)) { negativeZero = true; break; }
  }
  if (!negativeZero) return array;
  const copy = new Float32Array(array);
  for (let index = 0; index < copy.length; index++) {
    if (Object.is(copy[index], -0)) copy[index] = 0;
  }
  return copy;
}

function normalizeMesh(mesh) {
  if (!isObject(mesh)) fail(ERROR_CODES.ACCESSOR, 'mesh must be an object');
  const positions = canonicalizeFloat32(toFloat32Array(mesh.positions, 'positions'));
  const indices = toUint32Array(mesh.indices, 'indices');
  if (positions.length === 0 || positions.length % 3 !== 0) {
    fail(ERROR_CODES.ACCESSOR, 'positions must contain a non-zero multiple of 3 floats');
  }
  if (indices.length === 0 || indices.length % 3 !== 0) {
    fail(ERROR_CODES.ACCESSOR, 'indices must contain a non-zero multiple of 3 values');
  }
  const vertexCount = positions.length / 3;
  for (let index = 0; index < indices.length; index++) {
    if (indices[index] >= vertexCount) {
      fail(ERROR_CODES.ACCESSOR, `index ${indices[index]} exceeds the ${vertexCount} vertices`);
    }
  }
  let normals = null;
  if (mesh.normals !== null && mesh.normals !== undefined) {
    normals = canonicalizeFloat32(toFloat32Array(mesh.normals, 'normals'));
    if (normals.length !== positions.length) {
      fail(ERROR_CODES.ACCESSOR, 'normals must have the same length as positions');
    }
  }
  const hasNormals = normals !== null;
  if (normals === null) normals = new Float32Array(positions.length);
  return {
    positions, indices, normals, hasNormals,
    source: isObject(mesh.source) ? mesh.source : { kind: 'gltf' },
  };
}

// Pack one triangle range into local vertices/indices. Triangles that would
// exceed the cap are skipped (parents only; LOD0 never passes such a range).
function packTriangles(positions, normals, flatIndices, start, triangleCount, cap = MAX_CLUSTER_VERTICES) {
  const map = new Map();
  const outPositions = [];
  const outNormals = [];
  const outIndices = [];
  const key = index => `${positions[index * 3]},${positions[index * 3 + 1]},${positions[index * 3 + 2]}|${normals[index * 3]},${normals[index * 3 + 1]},${normals[index * 3 + 2]}`;
  for (let t = 0; t < triangleCount; t++) {
    const tri = [0, 1, 2].map(offset => flatIndices[start + t * 3 + offset]);
    const keys = tri.map(key);
    if (map.size + new Set(keys.filter(item => !map.has(item))).size > cap) {
      if (outIndices.length === 0) fail(ERROR_CODES.ACCESSOR, 'one triangle exceeds the cluster vertex limit');
      continue;
    }
    for (let offset = 0; offset < 3; offset++) {
      if (!map.has(keys[offset])) {
        map.set(keys[offset], outPositions.length / 3);
        outPositions.push(positions[tri[offset] * 3], positions[tri[offset] * 3 + 1], positions[tri[offset] * 3 + 2]);
        outNormals.push(normals[tri[offset] * 3], normals[tri[offset] * 3 + 1], normals[tri[offset] * 3 + 2]);
      }
      outIndices.push(map.get(keys[offset]));
    }
  }
  return {
    positions: outPositions, normals: outNormals, indices: outIndices,
    triangleCount: outIndices.length / 3, bounds: boundsOfFlat(outPositions),
  };
}

function combineChildren(children) {
  const positions = [];
  const normals = [];
  const indices = [];
  for (const child of children) {
    const base = positions.length / 3;
    positions.push(...child.positions);
    normals.push(...child.normals);
    for (const index of child.indices) indices.push(base + index);
  }
  return { positions, normals, indices };
}

// LOD0 is a disjoint cover of the source triangles. Every higher LOD groups two
// adjacent lower-LOD clusters and keeps every second child triangle as a proxy;
// this is a structural DAG, not a quality-guaranteed simplifier.
function buildClusterLevels(source) {
  const { positions, normals, indices } = source;
  const totalTriangles = indices.length / 3;
  const levels = [[]];
  let cursor = 0;
  while (cursor < totalTriangles) {
    const used = new Set();
    let count = 0;
    while (cursor + count < totalTriangles && count < MAX_CLUSTER_TRIANGLES) {
      const fresh = new Set();
      for (let k = 0; k < 3; k++) {
        const index = indices[(cursor + count) * 3 + k];
        if (!used.has(index)) fresh.add(index);
      }
      if (used.size + fresh.size > MAX_CLUSTER_VERTICES) break;
      for (const index of fresh) used.add(index);
      count++;
    }
    if (count === 0) fail(ERROR_CODES.ACCESSOR, 'a triangle exceeds the cluster vertex limit');
    const packed = packTriangles(positions, normals, indices, cursor * 3, count);
    levels[0].push({
      id: levels[0].length, lod: 0, parent: -1, children: [], ...packed,
      sourceTriangleStart: cursor, sourceTriangleCount: count,
    });
    cursor += count;
  }
  let previous = levels[0];
  let nextId = levels[0].length;
  while (previous.length > 1) {
    const level = levels.length;
    const next = [];
    for (let i = 0; i < previous.length; i += 2) {
      const group = previous.slice(i, i + 2);
      const combined = combineChildren(group);
      const selected = [];
      for (let t = 0; t < combined.indices.length / 3; t += 2) {
        selected.push(combined.indices[t * 3], combined.indices[t * 3 + 1], combined.indices[t * 3 + 2]);
      }
      const packed = packTriangles(combined.positions, combined.normals, selected, 0, selected.length / 3);
      const parent = {
        id: nextId++, lod: level, parent: -1, children: group.map(child => child.id),
        ...packed, bounds: unionBounds(group.map(child => child.bounds)),
        sourceTriangleStart: -1, sourceTriangleCount: 0,
      };
      for (const child of group) child.parent = parent.id;
      next.push(parent);
    }
    levels.push(next);
    previous = next;
  }
  return levels;
}

function createPackageDataFromMesh(mesh) {
  const source = normalizeMesh(mesh);
  const sourceVertexBytes = encodeFloat32(interleave(source.positions, source.normals));
  const sourceIndexBytes = encodeUint32(source.indices);
  const sourceContentHash = sha256(Buffer.concat([sourceVertexBytes, sourceIndexBytes]));
  const levels = buildClusterLevels(source);
  const clusters = levels.flat();
  const clusterVertexValues = [];
  const clusterIndexValues = [];
  for (const cluster of clusters) {
    cluster.vertexOffset = clusterVertexValues.length / 6;
    cluster.indexOffset = clusterIndexValues.length;
    clusterVertexValues.push(...interleave(cluster.positions, cluster.normals));
    clusterIndexValues.push(...cluster.indices);
  }
  const inventory = [
    ['vertices', sourceVertexBytes],
    ['indices', sourceIndexBytes],
    ['cluster-vertices', encodeFloat32(clusterVertexValues)],
    ['cluster-indices', encodeUint32(clusterIndexValues)],
  ].map(([name, bytes]) => ({ name, path: `sections/${name}.bin`, bytes, sha256: sha256(bytes) }))
    .sort((left, right) => (left.name < right.name ? -1 : left.name > right.name ? 1 : 0));
  const clusterRecords = clusters.map(cluster => ({
    id: cluster.id, lod: cluster.lod, parent: cluster.parent, children: cluster.children,
    vertexOffset: cluster.vertexOffset, vertexCount: cluster.positions.length / 3,
    indexOffset: cluster.indexOffset, triangleCount: cluster.triangleCount,
    bounds: cluster.bounds, sourceTriangleStart: cluster.sourceTriangleStart, sourceTriangleCount: cluster.sourceTriangleCount,
  }));
  const lodLevels = levels.map((list, level) => ({ level, clusterStart: list[0].id, clusterCount: list.length }));
  const vertexLayout = { fields: ['px', 'py', 'pz', 'nx', 'ny', 'nz'], format: 'float32le', strideBytes: 24 };
  const origin = source.source;
  const manifest = {
    formatVersion: FORMAT_VERSION,
    kind: PACKAGE_KIND,
    generator: { name: GENERATOR_NAME, version: TOOL_VERSION },
    source: {
      kind: origin.kind ?? 'gltf',
      generator: origin.generator ?? GLTF_GENERATOR,
      file: origin.file ?? null,
      sha256: origin.sha256 ?? null,
      hasNormals: source.hasNormals,
      primitives: origin.primitives ?? null,
      contentHash: sourceContentHash,
    },
    layout: {
      vertex: vertexLayout, index: { format: 'uint32le', strideBytes: 4 },
      clusterVertex: vertexLayout, clusterIndex: { format: 'uint32le', strideBytes: 4 },
    },
    counts: {
      sourceVertices: source.positions.length / 3, sourceTriangles: source.indices.length / 3,
      clusters: clusterRecords.length, lodLevels: lodLevels.length,
      clusterVertices: clusterVertexValues.length / 6,
      clusterTriangles: clusterRecords.reduce((sum, cluster) => sum + cluster.triangleCount, 0),
    },
    bounds: boundsOfFlat(source.positions),
    sections: inventory.map(({ name, path: sectionPath, bytes, sha256: hash }) => ({ name, path: sectionPath, bytes: bytes.length, sha256: hash })),
    clusters: clusterRecords,
    lodLevels,
  };
  manifest.packageDigest = computePackageDigest(manifest);
  return { manifest, sections: inventory, source };
}

export async function buildPackageFromMesh(outputDirectory, mesh) {
  const data = createPackageDataFromMesh(mesh);
  const root = path.resolve(outputDirectory);
  await fs.mkdir(path.join(root, 'sections'), { recursive: true });
  for (const section of data.sections) await fs.writeFile(path.join(root, section.path), section.bytes);
  const manifestPath = path.join(root, MANIFEST_NAME);
  await fs.writeFile(manifestPath, `${stableJSON(data.manifest)}\n`, 'utf8');
  return {
    manifestPath, manifest: data.manifest, packageDigest: data.manifest.packageDigest,
    counts: data.manifest.counts, lodLevels: data.manifest.lodLevels,
  };
}

// ---------------------------------------------------------------------------
// Mesh document (export format) and adapter back into a package
// ---------------------------------------------------------------------------

export function createMeshDocument(mesh) {
  const data = createPackageDataFromMesh(mesh);
  const source = data.source;
  return {
    format: MESH_FORMAT,
    formatVersion: MESH_FORMAT_VERSION,
    generator: { name: GENERATOR_NAME, version: TOOL_VERSION },
    source: source.source,
    counts: { vertices: source.positions.length / 3, triangles: source.indices.length / 3 },
    bounds: boundsOfFlat(source.positions),
    contentHash: data.manifest.source.contentHash,
    packageDigest: data.manifest.packageDigest,
    positions: Array.from(source.positions),
    normals: source.hasNormals ? Array.from(source.normals) : null,
    indices: Array.from(source.indices),
  };
}

export function meshFromDocument(document) {
  if (!isObject(document) || document.format !== MESH_FORMAT) {
    fail(ERROR_CODES.ARGUMENT, `not a ${MESH_FORMAT} document`);
  }
  if (document.formatVersion !== MESH_FORMAT_VERSION) {
    fail(ERROR_CODES.UNSUPPORTED, `unsupported mesh document formatVersion ${JSON.stringify(document.formatVersion)}`);
  }
  const mesh = {
    positions: toFloat32Array(document.positions, 'positions'),
    indices: toUint32Array(document.indices, 'indices'),
    normals: document.normals === null || document.normals === undefined
      ? null
      : toFloat32Array(document.normals, 'normals'),
    source: isObject(document.source) ? document.source : { kind: 'gltf' },
  };
  normalizeMesh(mesh);
  return mesh;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

const USAGE = `Minimal glTF 2.0 importer (B7, JSON output)
  node tools/offline/gltf.mjs load --in <file.gltf>
  node tools/offline/gltf.mjs export --in <file.gltf> --out <gltf.mesh.json>
`;

function parseOptions(args, allowed) {
  if (args.length % 2) fail(ERROR_CODES.ARGUMENT, 'Every option requires a value');
  const parsed = {};
  for (let index = 0; index < args.length; index += 2) {
    if (!allowed.includes(args[index]) || Object.hasOwn(parsed, args[index])) {
      fail(ERROR_CODES.ARGUMENT, `Unknown or duplicate option: ${args[index]}`);
    }
    parsed[args[index]] = args[index + 1];
  }
  return parsed;
}

function summarizeMesh(mesh) {
  return {
    file: mesh.source.file,
    counts: { vertices: mesh.positions.length / 3, triangles: mesh.indices.length / 3 },
    bounds: boundsOfFlat(mesh.positions),
    hasNormals: mesh.normals !== null,
    source: mesh.source,
  };
}

async function main(args) {
  const [command, ...rest] = args;
  if (!command || command === 'help' || command === '--help' || command === '-h') return USAGE;
  if (command === 'load') {
    const options = parseOptions(rest, ['--in']);
    if (!options['--in']) fail(ERROR_CODES.ARGUMENT, 'load requires --in <file.gltf>');
    return summarizeMesh(await load(options['--in']));
  }
  if (command === 'export') {
    const options = parseOptions(rest, ['--in', '--out']);
    if (!options['--in']) fail(ERROR_CODES.ARGUMENT, 'export requires --in <file.gltf>');
    const target = path.resolve(options['--out'] ?? path.join('build', 'offline', 'gltf.mesh.json'));
    const document = createMeshDocument(await load(options['--in']));
    const text = `${JSON.stringify(document, null, 2)}\n`;
    await fs.mkdir(path.dirname(target), { recursive: true });
    await fs.writeFile(target, text, 'utf8');
    return {
      out: target, bytes: Buffer.byteLength(text), counts: document.counts, bounds: document.bounds,
      contentHash: document.contentHash, packageDigest: document.packageDigest, source: document.source,
    };
  }
  fail(ERROR_CODES.ARGUMENT, `Unknown command: ${command}`);
}

const invoked = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (invoked) {
  main(process.argv.slice(2)).then(result => {
    if (typeof result === 'string') console.log(result);
    else if (result !== null) console.log(JSON.stringify({ ok: true, result }, null, 2));
  }).catch(error => {
    console.error(JSON.stringify({
      ok: false,
      error: { code: error.code ?? ERROR_CODES.IO, message: error.message, details: error.details ?? [] },
    }, null, 2));
    process.exitCode = 1;
  });
}

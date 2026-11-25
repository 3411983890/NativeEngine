import * as fs from 'node:fs/promises';
import path from 'node:path';
import { sha256Hex } from './canonical.mjs';

export class AssetError extends Error {
  constructor(code, message) { super(message); this.code = `NG-E-ASSET-${code}`; }
}
export function requireAsset(ok, message, code = 'FORMAT') {
  if (!ok) throw new AssetError(code, message);
}
export async function readRelative(root, uri) {
  requireAsset(typeof uri === 'string', 'Resource URI must be a string');
  if (uri.startsWith('data:')) {
    const match = /^data:([^;,]*);base64,([A-Za-z0-9+/]*={0,2})$/.exec(uri);
    requireAsset(match && match[2].length % 4 === 0, 'Invalid base64 resource');
    return Buffer.from(match[2], 'base64');
  }
  const decoded = decodeURIComponent(uri);
  requireAsset(!/[:\\\x00?#]/.test(decoded) && !path.isAbsolute(decoded), `Invalid resource URI: ${uri}`, 'PATH');
  const base = await fs.realpath(root), target = await fs.realpath(path.resolve(base, decoded));
  const relative = path.relative(base, target);
  requireAsset(relative && !relative.startsWith('..') && !path.isAbsolute(relative), `Resource escapes source directory: ${uri}`, 'PATH');
  const stat = await fs.stat(target);
  requireAsset(stat.size <= 256 * 1024 * 1024, 'Resource exceeds 256 MiB', 'BUDGET');
  return fs.readFile(target);
}

// Shared source reader: preserves the document for the static and animated importers.
export async function readGltf(file) {
  const source = await fs.readFile(file);
  requireAsset(source.length <= 256 * 1024 * 1024, 'Source exceeds 256 MiB', 'BUDGET');
  let document, binary;
  if (source.length >= 12 && source.readUInt32LE(0) === 0x46546c67) {
    requireAsset(source.readUInt32LE(4) === 2 && source.readUInt32LE(8) === source.length, 'Invalid GLB header');
    let at = 12, chunks = 0;
    while (at < source.length) {
      requireAsset(at + 8 <= source.length, 'Truncated GLB chunk');
      const length = source.readUInt32LE(at), type = source.readUInt32LE(at + 4); at += 8;
      requireAsset(length % 4 === 0 && at + length <= source.length, 'Invalid GLB chunk size');
      const bytes = source.subarray(at, at + length); at += length;
      if (chunks++ === 0) {
        requireAsset(type === 0x4e4f534a, 'First GLB chunk must be JSON');
        document = JSON.parse(bytes.toString('utf8').trimEnd());
      } else if (type === 0x004e4942) {
        requireAsset(!binary, 'Duplicate GLB binary chunk'); binary = bytes;
      }
    }
  } else document = JSON.parse(source.toString('utf8').replace(/^\uFEFF/,''));
  requireAsset(document?.asset?.version === '2.0', 'Expected glTF 2.0');
  const buffers = [];let sourceBytes=0;
  requireAsset((document.buffers??[]).length<=1024,'glTF buffer count exceeds budget','BUDGET');
  for (const [i, spec] of (document.buffers ?? []).entries()) {
    if(spec.uri===undefined&&!binary&&(spec.extensions?.EXT_meshopt_compression?.fallback||spec.extensions?.KHR_meshopt_compression?.fallback)) {
      requireAsset(Number.isSafeInteger(spec.byteLength)&&spec.byteLength>=0&&spec.byteLength<=256*1024*1024,'Invalid meshopt fallback buffer','BUDGET');
      buffers.push(Buffer.alloc(0));continue;
    }
    const bytes = spec.uri === undefined ? binary : await readRelative(path.dirname(file), spec.uri);
    sourceBytes+=bytes?.length??0;requireAsset(sourceBytes<=256*1024*1024,'Combined source buffers exceed 256 MiB','BUDGET');
    requireAsset(bytes && Number.isSafeInteger(spec.byteLength) && spec.byteLength >= 0 && spec.byteLength <= bytes.length,
      `Invalid buffer ${i}`);
    if (spec.uri === undefined) requireAsset(i === 0 && bytes.length - spec.byteLength <= 3, 'Invalid GLB binary buffer');
    buffers.push(bytes.subarray(0, spec.byteLength));
  }
  const dependencies = buffers.map(sha256Hex);
  requireAsset(buffers.reduce((sum,buffer)=>sum+buffer.length,0)<=256*1024*1024,'Combined glTF buffers exceed 256 MiB','BUDGET');
  const asset={ document, buffers, source, dependencies, file: path.resolve(file) };
  if(document.bufferViews?.some(v=>v.extensions?.EXT_meshopt_compression||v.extensions?.KHR_meshopt_compression)||document.meshes?.some(m=>m.primitives?.some(p=>p.extensions?.KHR_draco_mesh_compression))) {
    const {decodeGltfCompression}=await import('./gltf-compression.mjs');return decodeGltfCompression(asset);
  }
  return asset;
}

const TYPES = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
const COMPONENTS = { 5120: [1, 'getInt8', 127], 5121: [1, 'getUint8', 255], 5122: [2, 'getInt16', 32767],
  5123: [2, 'getUint16', 65535], 5125: [4, 'getUint32', 4294967295], 5126: [4, 'getFloat32', 0] };
export function readAccessor(asset, index, expectedType) {
  const { document: doc, buffers } = asset, accessor = doc.accessors?.[index];
  requireAsset(Number.isSafeInteger(index) && index >= 0 && accessor, `Missing accessor ${index}`);
  const components = TYPES[accessor.type], spec = COMPONENTS[accessor.componentType];
  requireAsset(components && spec && (!expectedType || accessor.type === expectedType), `Invalid accessor type ${index}`);
  requireAsset(Number.isSafeInteger(accessor.count) && accessor.count > 0 && accessor.count <= 1000000, 'Accessor count exceeds budget', 'BUDGET');
  requireAsset(accessor.normalized === undefined || typeof accessor.normalized === 'boolean', 'Accessor normalized must be boolean');
  requireAsset(!accessor.normalized || (accessor.componentType !== 5126 && accessor.componentType !== 5125), 'Invalid normalized accessor');
  function values(viewIndex, offset, count, countComponents, componentSpec, interleaved) {
    const view = doc.bufferViews?.[viewIndex], buffer = buffers[view?.buffer];
    requireAsset(view && buffer, 'Missing accessor bufferView');
    const start = view.byteOffset ?? 0, local = offset ?? 0;
    const stride = interleaved ? view.byteStride ?? countComponents * componentSpec[0] : countComponents * componentSpec[0];
    requireAsset([start, local, view.byteLength, stride].every(Number.isSafeInteger) && start >= 0 && local >= 0 &&
      stride >= countComponents * componentSpec[0] && stride <= 252 && stride % componentSpec[0] === 0 &&
      (start + local) % componentSpec[0] === 0 && start + view.byteLength <= buffer.length &&
      local + (count - 1) * stride + countComponents * componentSpec[0] <= view.byteLength, 'Accessor exceeds bufferView');
    const data = new DataView(buffer.buffer, buffer.byteOffset, buffer.length);
    return Array.from({ length: count }, (_, i) => Array.from({ length: countComponents }, (_, c) => {
      const value = data[componentSpec[1]](start + local + i * stride + c * componentSpec[0], true);
      requireAsset(Number.isFinite(value), 'Non-finite accessor value'); return value;
    }));
  }
  let result = accessor.bufferView === undefined ? Array.from({ length: accessor.count }, () => Array(components).fill(0))
    : values(accessor.bufferView, accessor.byteOffset, accessor.count, components, spec, true);
  if (accessor.sparse) {
    const sparse = accessor.sparse;
    requireAsset(Number.isSafeInteger(sparse.count) && sparse.count > 0 && sparse.count <= accessor.count &&
      [5121, 5123, 5125].includes(sparse.indices?.componentType), 'Invalid sparse accessor');
    const indices = values(sparse.indices.bufferView, sparse.indices.byteOffset, sparse.count, 1, COMPONENTS[sparse.indices.componentType], false);
    const replacements = values(sparse.values.bufferView, sparse.values.byteOffset, sparse.count, components, spec, false);
    indices.forEach(([i], n) => {
      requireAsset(i < accessor.count && (n === 0 || i > indices[n - 1][0]), 'Sparse indices must increase'); result[i] = replacements[n];
    });
  }
  if (accessor.normalized) result = result.map(v => v.map(x => Math.max(-1, x / spec[2])));
  return result;
}

export const identityMatrix = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
export function multiplyMatrix(a, b) {
  return Array.from({ length: 16 }, (_, k) => Array.from({ length: 4 }, (_, i) => a[(k % 4) + i * 4] * b[i + Math.floor(k / 4) * 4]).reduce((s, v) => s + v, 0));
}
export function nodeMatrix(node) {
  if (node.matrix) {
    requireAsset(node.matrix.length === 16 && node.matrix.every(Number.isFinite), 'Invalid node matrix');
    requireAsset(!node.translation && !node.rotation && !node.scale, 'Matrix and TRS cannot coexist');
    requireAsset(node.matrix[3] === 0 && node.matrix[7] === 0 && node.matrix[11] === 0 && node.matrix[15] === 1, 'Expected affine matrix');
    return node.matrix;
  }
  const t = node.translation ?? [0, 0, 0], s = node.scale ?? [1, 1, 1], r = node.rotation ?? [0, 0, 0, 1];
  requireAsset(t.length === 3 && s.length === 3 && r.length === 4 && [...t, ...s, ...r].every(Number.isFinite), 'Invalid node TRS');
  const length = Math.hypot(...r); requireAsset(length > 1e-8, 'Invalid rotation');
  const [x, y, z, w] = r.map(v => v / length);
  return [(1-2*y*y-2*z*z)*s[0], (2*x*y+2*w*z)*s[0], (2*x*z-2*w*y)*s[0], 0,
    (2*x*y-2*w*z)*s[1], (1-2*x*x-2*z*z)*s[1], (2*y*z+2*w*x)*s[1], 0,
    (2*x*z+2*w*y)*s[2], (2*y*z-2*w*x)*s[2], (1-2*x*x-2*y*y)*s[2], 0, ...t, 1];
}

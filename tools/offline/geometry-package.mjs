#!/usr/bin/env node
// INN-14: content-addressed geometry package (offline, CPU only).
// One procedural source mesh -> vertices/indices + clusters + LOD DAG, for
// later Nanite-style rendering, Lumen SDF tracing and DXR BLAS builds.
import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const TOOL_VERSION = '0.1.0';
export const FORMAT_VERSION = 1;
export const PACKAGE_KIND = 'nativegpu.geometry-package';
export const MANIFEST_NAME = 'geometry.package.json';
export const MAX_CLUSTER_VERTICES = 128;
export const MAX_CLUSTER_TRIANGLES = 128;
export const ERROR_CODES = Object.freeze({
  TAMPERED: 'NG-E-GEOMETRY-TAMPERED',
  MISSING: 'NG-E-GEOMETRY-MISSING',
  MANIFEST: 'NG-E-GEOMETRY-MANIFEST',
  PATH: 'NG-E-GEOMETRY-PATH',
  INPUT: 'NG-E-GEOMETRY-INPUT',
  ARGUMENT: 'NG-E-GEOMETRY-ARGUMENT',
});

export class GeometryError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'GeometryError';
    this.code = code;
    this.details = details;
  }
}

// Canonical JSON: recursively sorted keys, compact separators, array order kept.
export function stableJSON(value) {
  if (Array.isArray(value)) return `[${value.map(stableJSON).join(',')}]`;
  if (value !== null && typeof value === 'object') {
    return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${stableJSON(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}

const sha256 = data => createHash('sha256').update(data).digest('hex');

export function computePackageDigest(manifest) {
  const body = {};
  for (const key of Object.keys(manifest)) if (key !== 'packageDigest') body[key] = manifest[key];
  return sha256(Buffer.from(`${stableJSON(body)}\n`, 'utf8'));
}

const encodeFloat32 = values => { const array = Float32Array.from(values); return Buffer.from(array.buffer, array.byteOffset, array.byteLength); };
const encodeUint32 = values => { const array = Uint32Array.from(values); return Buffer.from(array.buffer, array.byteOffset, array.byteLength); };

function interleave(positions, normals) {
  const out = [];
  for (let i = 0; i < positions.length; i += 3) {
    out.push(positions[i], positions[i + 1], positions[i + 2], normals[i], normals[i + 1], normals[i + 2]);
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

const FACES = [
  { n: [1, 0, 0], u: [0, 1, 0], v: [0, 0, 1] },
  { n: [-1, 0, 0], u: [0, 0, 1], v: [0, 1, 0] },
  { n: [0, 1, 0], u: [0, 0, 1], v: [1, 0, 0] },
  { n: [0, -1, 0], u: [1, 0, 0], v: [0, 0, 1] },
  { n: [0, 0, 1], u: [1, 0, 0], v: [0, 1, 0] },
  { n: [0, 0, -1], u: [0, 1, 0], v: [1, 0, 0] },
];

// Procedural, inspectable input: a quad grid plus four boxes. No external
// binary asset, RNG or clock, so identical parameters rebuild identical bytes.
export function proceduralSource(parameters = {}) {
  const gridQuads = parameters.gridQuads ?? 32;
  const cellSize = parameters.cellSize ?? 0.5;
  const boxSize = parameters.boxSize ?? 1;
  const positions = [];
  const normals = [];
  const indices = [];
  const seen = new Map();
  const vertex = (p, n) => {
    const key = `${p[0]},${p[1]},${p[2]}|${n[0]},${n[1]},${n[2]}`;
    let index = seen.get(key);
    if (index === undefined) {
      index = positions.length / 3;
      seen.set(key, index);
      positions.push(...p);
      normals.push(...n);
    }
    return index;
  };
  const half = (gridQuads * cellSize) / 2;
  const gridBase = positions.length / 3;
  for (let z = 0; z <= gridQuads; z++) {
    for (let x = 0; x <= gridQuads; x++) vertex([x * cellSize - half, 0, z * cellSize - half], [0, 1, 0]);
  }
  for (let z = 0; z < gridQuads; z++) {
    for (let x = 0; x < gridQuads; x++) {
      const i = gridBase + z * (gridQuads + 1) + x;
      indices.push(i, i + gridQuads + 1, i + 1, i + 1, i + gridQuads + 1, i + gridQuads + 2);
    }
  }
  const h = boxSize / 2;
  const offset = half + boxSize;
  const centers = [[-offset, h, -offset], [offset, h, -offset], [offset, h, offset], [-offset, h, offset]];
  for (const center of centers) {
    for (const face of FACES) {
      const corners = [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([su, sv]) => [0, 1, 2].map(axis =>
        center[axis] + face.n[axis] * h + (face.u[axis] * su + face.v[axis] * sv) * h));
      const base = positions.length / 3;
      for (const corner of corners) vertex(corner, face.n);
      indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
    }
  }
  return { parameters: { gridQuads, cellSize, boxSize, boxCount: centers.length }, positions, normals, indices };
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
      if (outIndices.length === 0) throw new GeometryError(ERROR_CODES.INPUT, 'One triangle exceeds the cluster vertex limit');
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
  return { positions: outPositions, normals: outNormals, indices: outIndices, triangleCount: outIndices.length / 3, bounds: boundsOfFlat(outPositions) };
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
    if (count === 0) throw new GeometryError(ERROR_CODES.INPUT, 'A triangle exceeds the cluster vertex limit');
    const packed = packTriangles(positions, normals, indices, cursor * 3, count);
    levels[0].push({ id: levels[0].length, lod: 0, parent: -1, children: [], ...packed, sourceTriangleStart: cursor, sourceTriangleCount: count });
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

function createPackageData(options = {}) {
  const source = proceduralSource(options.parameters ?? {});
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
  const manifest = {
    formatVersion: FORMAT_VERSION,
    kind: PACKAGE_KIND,
    generator: { name: 'nativegpu-offline-geometry', version: TOOL_VERSION },
    source: { kind: 'procedural', generator: 'quad-grid+boxes', parameters: source.parameters, contentHash: sourceContentHash },
    layout: { vertex: vertexLayout, index: { format: 'uint32le', strideBytes: 4 },
      clusterVertex: vertexLayout, clusterIndex: { format: 'uint32le', strideBytes: 4 } },
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
  return { manifest, sections: inventory };
}

export async function buildPackage(outputDirectory, options = {}) {
  const data = createPackageData(options);
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

const fail = (code, message, details = []) => { throw new GeometryError(code, message, details); };
const isHash = value => typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
const countKeys = ['sourceVertices', 'sourceTriangles', 'clusters', 'lodLevels', 'clusterVertices', 'clusterTriangles'];

function resolveInside(root, relativePath) {
  if (typeof relativePath !== 'string' || relativePath.length === 0 || path.isAbsolute(relativePath)) {
    fail(ERROR_CODES.PATH, `Section path is not relative: ${relativePath}`);
  }
  const full = path.resolve(root, relativePath);
  const relative = path.relative(root, full);
  if (relative.startsWith('..') || path.isAbsolute(relative)) fail(ERROR_CODES.PATH, `Section path escapes the package: ${relativePath}`);
  return full;
}

function assertManifest(manifest) {
  const bad = message => fail(ERROR_CODES.MANIFEST, message);
  if (manifest === null || typeof manifest !== 'object' || Array.isArray(manifest)) bad('Manifest must be a JSON object');
  if (manifest.formatVersion !== FORMAT_VERSION) bad(`formatVersion must be ${FORMAT_VERSION}`);
  if (manifest.kind !== PACKAGE_KIND) bad(`kind must be ${PACKAGE_KIND}`);
  if (!isHash(manifest.packageDigest)) bad('packageDigest must be a lowercase SHA-256 hex string');
  if (!Array.isArray(manifest.sections) || !Array.isArray(manifest.clusters) || !Array.isArray(manifest.lodLevels)) {
    bad('sections, clusters and lodLevels must be arrays');
  }
  if (!manifest.counts || countKeys.some(key => !Number.isSafeInteger(manifest.counts[key]) || manifest.counts[key] < 0)) {
    bad('counts must contain non-negative integers');
  }
  if (manifest.clusters.some(cluster => cluster === null || typeof cluster !== 'object' || Array.isArray(cluster))) bad('Cluster entries must be objects');
  if (manifest.lodLevels.some(level => level === null || typeof level !== 'object' || Array.isArray(level))) bad('LOD entries must be objects');
  const names = new Set();
  for (const section of manifest.sections) {
    if (!section || typeof section.name !== 'string' || typeof section.path !== 'string'
      || !Number.isSafeInteger(section.bytes) || section.bytes < 0 || !isHash(section.sha256)) {
      bad('Malformed section entry');
    }
    if (names.has(section.name)) bad(`Duplicate section name: ${section.name}`);
    names.add(section.name);
  }
}

function validateLayout(manifest) {
  const bad = message => fail(ERROR_CODES.MANIFEST, message);
  if (manifest.clusters.length !== manifest.counts.clusters) bad('counts.clusters does not match clusters.length');
  if (manifest.lodLevels.length !== manifest.counts.lodLevels) bad('counts.lodLevels does not match lodLevels.length');
  manifest.clusters.forEach((cluster, index) => {
    if (cluster.id !== index) bad(`Cluster ids must equal their array index (${index})`);
    if (!Number.isSafeInteger(cluster.vertexOffset) || !Number.isSafeInteger(cluster.vertexCount)
      || !Number.isSafeInteger(cluster.indexOffset) || !Number.isSafeInteger(cluster.triangleCount)
      || cluster.vertexCount < 1 || cluster.triangleCount < 1
      || cluster.vertexCount > MAX_CLUSTER_VERTICES || cluster.triangleCount > MAX_CLUSTER_TRIANGLES) {
      bad(`Cluster ${index} has invalid ranges`);
    }
    if (cluster.vertexOffset + cluster.vertexCount > manifest.counts.clusterVertices) bad(`Cluster ${index} exceeds the vertex section`);
    if (cluster.indexOffset + cluster.triangleCount * 3 > manifest.counts.clusterTriangles * 3) bad(`Cluster ${index} exceeds the index section`);
    if (cluster.parent !== -1) {
      if (!Number.isSafeInteger(cluster.parent) || cluster.parent < 0 || cluster.parent >= manifest.clusters.length) bad(`Cluster ${index} has an unknown parent`);
    }
    if (!Array.isArray(cluster.children)) bad(`Cluster ${index} children must be an array`);
    for (const child of cluster.children) {
      if (!Number.isSafeInteger(child) || child < 0 || child >= manifest.clusters.length) bad(`Cluster ${index} has an unknown child ${child}`);
      if (manifest.clusters[child].parent !== cluster.id) bad(`Child ${child} does not point back to cluster ${index}`);
      if (manifest.clusters[child].lod !== cluster.lod - 1) bad(`Child ${child} is not one LOD below cluster ${index}`);
    }
    const seen = new Set();
    let current = cluster;
    while (current.parent !== -1) {
      if (seen.has(current.id)) bad(`Cycle detected at cluster ${current.id}`);
      seen.add(current.id);
      current = manifest.clusters[current.parent];
    }
  });
  const covered = manifest.clusters.filter(cluster => cluster.lod === 0)
    .reduce((sum, cluster) => sum + cluster.sourceTriangleCount, 0);
  if (covered !== manifest.counts.sourceTriangles) bad('LOD0 clusters do not cover every source triangle');
  let start = 0;
  manifest.lodLevels.forEach((level, index) => {
    if (level.level !== index || level.clusterStart !== start || level.clusterCount < 1
      || (index > 0 && level.clusterCount >= manifest.lodLevels[index - 1].clusterCount)) {
      bad('LOD ranges must be contiguous, ordered and strictly shrinking');
    }
    start += level.clusterCount;
  });
  if (start !== manifest.clusters.length) bad('LOD ranges do not cover every cluster');
}

export async function verifyPackage(manifestPath) {
  const file = path.resolve(manifestPath);
  let raw;
  try {
    raw = await fs.readFile(file, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') fail(ERROR_CODES.MISSING, `Package manifest not found: ${file}`);
    throw error;
  }
  let manifest;
  try {
    manifest = JSON.parse(raw);
  } catch {
    fail(ERROR_CODES.MANIFEST, `Package manifest is not valid JSON: ${file}`);
  }
  assertManifest(manifest);
  const root = path.dirname(file);
  const sections = [];
  for (const section of manifest.sections) {
    const sectionFile = resolveInside(root, section.path);
    let data;
    try {
      data = await fs.readFile(sectionFile);
    } catch (error) {
      if (error.code === 'ENOENT') fail(ERROR_CODES.MISSING, `Section is missing: ${section.name}`, [section.path]);
      throw error;
    }
    if (data.length !== section.bytes) {
      fail(ERROR_CODES.TAMPERED, `Section size mismatch: ${section.name}`, [section.path, `expected ${section.bytes}`, `actual ${data.length}`]);
    }
    const hash = sha256(data);
    if (hash !== section.sha256) {
      fail(ERROR_CODES.TAMPERED, `Section hash mismatch: ${section.name}`, [section.path, `expected ${section.sha256}`, `actual ${hash}`]);
    }
    sections.push({ name: section.name, path: section.path, bytes: data.length, sha256: hash });
  }
  const digest = computePackageDigest(manifest);
  if (digest !== manifest.packageDigest) {
    fail(ERROR_CODES.TAMPERED, 'Package digest mismatch', [`expected ${manifest.packageDigest}`, `actual ${digest}`]);
  }
  validateLayout(manifest);
  return { ok: true, manifestPath: file, manifest, packageDigest: digest, sections };
}

export function summarizeLodLevels(manifest) {
  return manifest.lodLevels.map(level => {
    const clusters = manifest.clusters.slice(level.clusterStart, level.clusterStart + level.clusterCount);
    return {
      level: level.level, clusterStart: level.clusterStart, clusterCount: level.clusterCount,
      triangleCount: clusters.reduce((sum, cluster) => sum + cluster.triangleCount, 0),
      vertexCount: clusters.reduce((sum, cluster) => sum + cluster.vertexCount, 0),
      bounds: unionBounds(clusters.map(cluster => cluster.bounds)),
    };
  });
}

async function packageBytes(manifestPath) {
  const manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
  const root = path.dirname(manifestPath);
  const chunks = [await fs.readFile(manifestPath)];
  for (const section of manifest.sections) chunks.push(await fs.readFile(path.join(root, section.path)));
  return Buffer.concat(chunks);
}

export async function selfTest() {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-geometry-selftest-'));
  try {
    const first = await buildPackage(path.join(root, 'a'));
    const second = await buildPackage(path.join(root, 'b'));
    const tampered = await buildPackage(path.join(root, 'tampered'));
    const verified = await verifyPackage(first.manifestPath);
    const deterministic = first.packageDigest === second.packageDigest
      && (await packageBytes(first.manifestPath)).equals(await packageBytes(second.manifestPath));
    const section = tampered.manifest.sections.find(item => item.name === 'vertices');
    const tamperedFile = path.join(root, 'tampered', section.path);
    const bytes = await fs.readFile(tamperedFile);
    bytes[0] ^= 0xff;
    await fs.writeFile(tamperedFile, bytes);
    let tamperCode = null;
    try {
      await verifyPackage(tampered.manifestPath);
    } catch (error) {
      tamperCode = error.code ?? 'UNKNOWN';
    }
    const coverage = first.manifest.clusters.filter(cluster => cluster.lod === 0)
      .reduce((sum, cluster) => sum + cluster.sourceTriangleCount, 0) === first.manifest.counts.sourceTriangles;
    const roots = first.manifest.clusters.filter(cluster => cluster.parent === -1).length;
    return {
      ok: deterministic && verified.ok && tamperCode === ERROR_CODES.TAMPERED && coverage && roots === 1,
      deterministic, packageDigest: first.packageDigest, counts: first.counts,
      lodLevels: summarizeLodLevels(first.manifest), coverage, roots,
      tamper: { detected: tamperCode === ERROR_CODES.TAMPERED, code: tamperCode },
    };
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
}

const USAGE = `Offline geometry package (INN-14, JSON output)
  node tools/offline/geometry-package.mjs build --out <directory>
  node tools/offline/geometry-package.mjs verify --package <geometry.package.json>
  node tools/offline/geometry-package.mjs self-test
`;

function parseOptions(args, allowed) {
  if (args.length % 2) fail(ERROR_CODES.ARGUMENT, 'Every option requires a value');
  const parsed = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!allowed.includes(args[i]) || Object.hasOwn(parsed, args[i])) fail(ERROR_CODES.ARGUMENT, `Unknown or duplicate option: ${args[i]}`);
    parsed[args[i]] = args[i + 1];
  }
  return parsed;
}

async function main(args) {
  const [command, ...rest] = args;
  if (!command || command === 'help' || command === '--help') return USAGE;
  if (command === 'build') {
    const options = parseOptions(rest, ['--out']);
    const result = await buildPackage(options['--out'] ?? path.join('build', 'offline'));
    return { manifestPath: result.manifestPath, packageDigest: result.packageDigest, counts: result.counts, lodLevels: result.lodLevels };
  }
  if (command === 'verify') {
    const options = parseOptions(rest, ['--package']);
    if (!options['--package']) fail(ERROR_CODES.ARGUMENT, 'verify requires --package');
    const result = await verifyPackage(options['--package']);
    return { manifestPath: result.manifestPath, packageDigest: result.packageDigest, counts: result.manifest.counts, sections: result.sections };
  }
  if (command === 'self-test') {
    if (rest.length) fail(ERROR_CODES.ARGUMENT, 'self-test takes no options');
    const result = await selfTest();
    console.log([
      `self-test: ${result.ok ? 'PASS' : 'FAIL'}`,
      `package digest: ${result.packageDigest}`,
      `source: ${result.counts.sourceVertices} vertices, ${result.counts.sourceTriangles} triangles`,
      `clusters: ${result.counts.clusters} across ${result.counts.lodLevels} LOD levels`,
      ...result.lodLevels.map(level => `lod ${level.level}: ${level.clusterCount} clusters, ${level.triangleCount} triangles, ${level.vertexCount} vertices`),
      `coverage: ${result.coverage ? 'every LOD0 triangle in exactly one cluster' : 'BROKEN'}`,
      `roots: ${result.roots}`,
      `determinism: ${result.deterministic ? 'byte-identical rebuild' : 'MISMATCH'}`,
      `tamper: ${result.tamper.detected ? `detected (${result.tamper.code})` : 'NOT DETECTED'}`,
    ].join('\n'));
    if (!result.ok) process.exitCode = 1;
    return null;
  }
  fail(ERROR_CODES.ARGUMENT, `Unknown command: ${command}`);
}

const invoked = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (invoked) {
  main(process.argv.slice(2)).then(result => {
    if (result !== null) console.log(JSON.stringify({ ok: true, result }, null, 2));
  }).catch(error => {
    console.error(JSON.stringify({ ok: false, error: { code: error.code ?? 'NG-E-GEOMETRY-IO', message: error.message, details: error.details ?? [] } }, null, 2));
    process.exitCode = 1;
  });
}

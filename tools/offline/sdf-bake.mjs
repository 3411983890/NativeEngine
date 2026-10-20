#!/usr/bin/env node
// L0: offline narrow-band signed distance bake (CPU only, deterministic).
// One mesh (geometry package or simple JSON) -> chunked float32 SDF volume +
// manifest, so a later Lumen SWRT slice has content-addressed distance data.
// No GPU tracing, no global distance field, no surface cache cards here.
import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { stableJSON, verifyPackage } from './geometry-package.mjs';

export const TOOL_VERSION = '0.1.0';
export const FORMAT_VERSION = 1;
export const VOLUME_KIND = 'nativegpu.sdf-volume';
export const MANIFEST_NAME = 'sdf.volume.json';
export const DATA_NAME = 'sdf.volume.bin';
export const DEFAULT_RESOLUTION = 32;
export const DEFAULT_BAND_VOXELS = 2;
export const CHUNK_EDGE = 8;
export const CHUNK_VOXELS = CHUNK_EDGE * CHUNK_EDGE * CHUNK_EDGE;
export const CHUNK_BYTES = CHUNK_VOXELS * 4;
export const DEFAULT_PACKAGE_PATH = path.join('build', 'offline', 'geometry.package.json');
export const SELF_TEST_MAX_ERROR_VOXELS = 0.75;
export const ERROR_CODES = Object.freeze({
  TAMPERED: 'NG-E-SDF-TAMPERED',
  MISSING: 'NG-E-SDF-MISSING',
  MANIFEST: 'NG-E-SDF-MANIFEST',
  PATH: 'NG-E-SDF-PATH',
  INPUT: 'NG-E-SDF-INPUT',
  ARGUMENT: 'NG-E-SDF-ARGUMENT',
  IO: 'NG-E-SDF-IO',
});

export class SdfError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'SdfError';
    this.code = code;
    this.details = details;
  }
}

const sha256 = data => createHash('sha256').update(data).digest('hex');
const fail = (code, message, details = []) => { throw new SdfError(code, message, details); };
const isHash = value => typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
const isVector3 = value => Array.isArray(value) && value.length === 3 && value.every(item => Number.isFinite(item));

const encodeFloat32 = values => { const array = Float32Array.from(values); return Buffer.from(array.buffer, array.byteOffset, array.byteLength); };
const encodeUint32 = values => { const array = Uint32Array.from(values); return Buffer.from(array.buffer, array.byteOffset, array.byteLength); };

// Same canonical JSON convention as the geometry package: sorted keys, no
// whitespace, array order kept, one trailing newline on disk.
export function computeVolumeDigest(manifest) {
  const body = {};
  for (const key of Object.keys(manifest)) if (key !== 'volumeDigest') body[key] = manifest[key];
  return sha256(Buffer.from(`${stableJSON(body)}\n`, 'utf8'));
}

export function meshContentHash(positions, indices) {
  return sha256(Buffer.concat([encodeFloat32(positions), encodeUint32(indices)]));
}

function boundsOfFlat(positions) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let index = 0; index < positions.length; index += 3) {
    for (let axis = 0; axis < 3; axis++) {
      min[axis] = Math.min(min[axis], positions[index + axis]);
      max[axis] = Math.max(max[axis], positions[index + axis]);
    }
  }
  return { min, max };
}

function assertMeshArrays(positions, indices) {
  if (!Array.isArray(positions) || positions.length < 9 || positions.length % 3 !== 0) {
    fail(ERROR_CODES.INPUT, 'positions must be a flat array of at least three vertices (length a multiple of 3)');
  }
  if (!Array.isArray(indices) || indices.length < 3 || indices.length % 3 !== 0) {
    fail(ERROR_CODES.INPUT, 'indices must be a flat array with a multiple of three entries');
  }
  const vertexCount = positions.length / 3;
  for (const value of positions) {
    if (typeof value !== 'number' || !Number.isFinite(value)) fail(ERROR_CODES.INPUT, 'positions must contain only finite numbers');
  }
  for (const index of indices) {
    if (!Number.isSafeInteger(index) || index < 0 || index >= vertexCount) {
      fail(ERROR_CODES.INPUT, `Triangle index ${index} is outside [0, ${vertexCount})`);
    }
  }
}

// Simple JSON mesh: { "positions": [x,y,z, ...], "indices": [i0,i1,i2, ...] }.
// "positions" may also be an array of [x,y,z] triples.
export function parseMesh(value, label = 'mesh') {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) fail(ERROR_CODES.INPUT, `${label} must be a JSON object`);
  let positions = value.positions;
  if (Array.isArray(positions) && positions.length > 0 && Array.isArray(positions[0])) positions = positions.flat(1);
  assertMeshArrays(positions, value.indices);
  return { positions: Array.from(positions), indices: Array.from(value.indices) };
}

export async function loadSimpleMesh(file) {
  const target = path.resolve(file);
  let raw;
  try {
    raw = await fs.readFile(target, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') fail(ERROR_CODES.MISSING, `Mesh file not found: ${target}`);
    throw error;
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch {
    fail(ERROR_CODES.INPUT, `Mesh file is not valid JSON: ${target}`);
  }
  const mesh = parseMesh(parsed, 'mesh file');
  const bounds = boundsOfFlat(mesh.positions);
  return {
    positions: mesh.positions,
    indices: mesh.indices,
    source: {
      kind: 'mesh-json',
      contentHash: meshContentHash(mesh.positions, mesh.indices),
      vertices: mesh.positions.length / 3,
      triangles: mesh.indices.length / 3,
      bounds,
    },
  };
}

// Reuse the INN-14 package reader: verifyPackage checks the manifest digest and
// every section hash, then we decode the source vertices/indices as float32le
// (stride 24, positions in the first three floats) and uint32le.
export async function readMeshFromPackage(manifestPath) {
  const verified = await verifyPackage(manifestPath);
  const manifest = verified.manifest;
  const root = path.dirname(verified.manifestPath);
  const vertexSection = manifest.sections.find(section => section.name === 'vertices');
  const indexSection = manifest.sections.find(section => section.name === 'indices');
  if (!vertexSection || !indexSection) fail(ERROR_CODES.MANIFEST, 'Geometry package has no vertices/indices sections');
  const vertexBuffer = await fs.readFile(path.join(root, vertexSection.path));
  const indexBuffer = await fs.readFile(path.join(root, indexSection.path));
  if (vertexBuffer.length % 24 !== 0) fail(ERROR_CODES.MANIFEST, 'vertices section is not a multiple of the 24-byte stride');
  if (indexBuffer.length % 12 !== 0) fail(ERROR_CODES.MANIFEST, 'indices section is not a multiple of three uint32 values');
  const vertexView = new DataView(vertexBuffer.buffer, vertexBuffer.byteOffset, vertexBuffer.byteLength);
  const vertexCount = vertexBuffer.length / 24;
  const positions = new Array(vertexCount * 3);
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    positions[vertex * 3] = vertexView.getFloat32(vertex * 24, true);
    positions[vertex * 3 + 1] = vertexView.getFloat32(vertex * 24 + 4, true);
    positions[vertex * 3 + 2] = vertexView.getFloat32(vertex * 24 + 8, true);
  }
  const indexView = new DataView(indexBuffer.buffer, indexBuffer.byteOffset, indexBuffer.byteLength);
  const indices = new Array(indexBuffer.length / 4);
  for (let index = 0; index < indices.length; index++) indices[index] = indexView.getUint32(index * 4, true);
  assertMeshArrays(positions, indices);
  return {
    positions,
    indices,
    source: {
      kind: 'geometry-package',
      contentHash: isHash(manifest.source?.contentHash) ? manifest.source.contentHash : meshContentHash(positions, indices),
      packageDigest: manifest.packageDigest,
      vertices: positions.length / 3,
      triangles: indices.length / 3,
      bounds: manifest.bounds,
    },
  };
}

function createGrid(bounds, resolution, bandVoxels) {
  const extent = Math.max(
    bounds.max[0] - bounds.min[0],
    bounds.max[1] - bounds.min[1],
    bounds.max[2] - bounds.min[2],
  );
  if (!(extent > 0)) fail(ERROR_CODES.INPUT, 'Mesh bounds have zero extent; cannot choose a voxel size');
  const interior = resolution - 2 * bandVoxels;
  const voxelSize = extent / interior;
  if (!(voxelSize > 0)) fail(ERROR_CODES.INPUT, 'Voxel size underflowed');
  const origin = [0, 1, 2].map(axis => (bounds.min[axis] + bounds.max[axis]) / 2 - (resolution - 1) * voxelSize / 2);
  return {
    dimensions: [resolution, resolution, resolution],
    origin,
    voxelSize,
    bandRadius: bandVoxels * voxelSize,
  };
}

function squaredDistance(px, py, pz, out) {
  const dx = px - out[0];
  const dy = py - out[1];
  const dz = pz - out[2];
  return dx * dx + dy * dy + dz * dz;
}

function pointSegmentSquared(px, py, pz, ax, ay, az, bx, by, bz, out) {
  const abx = bx - ax;
  const aby = by - ay;
  const abz = bz - az;
  const apx = px - ax;
  const apy = py - ay;
  const apz = pz - az;
  const lengthSquared = abx * abx + aby * aby + abz * abz;
  let t = lengthSquared > 0 ? (apx * abx + apy * aby + apz * abz) / lengthSquared : 0;
  t = Math.min(1, Math.max(0, t));
  out[0] = ax + abx * t;
  out[1] = ay + aby * t;
  out[2] = az + abz * t;
  return squaredDistance(px, py, pz, out);
}

// Ericson, Real-Time Collision Detection, closest point on triangle.
function closestPointTriangleSquared(px, py, pz, ax, ay, az, bx, by, bz, cx, cy, cz, out) {
  const abx = bx - ax;
  const aby = by - ay;
  const abz = bz - az;
  const acx = cx - ax;
  const acy = cy - ay;
  const acz = cz - az;
  const apx = px - ax;
  const apy = py - ay;
  const apz = pz - az;
  const d1 = abx * apx + aby * apy + abz * apz;
  const d2 = acx * apx + acy * apy + acz * apz;
  if (d1 <= 0 && d2 <= 0) {
    out[0] = ax; out[1] = ay; out[2] = az;
    return apx * apx + apy * apy + apz * apz;
  }
  const bpx = px - bx;
  const bpy = py - by;
  const bpz = pz - bz;
  const d3 = abx * bpx + aby * bpy + abz * bpz;
  const d4 = acx * bpx + acy * bpy + acz * bpz;
  if (d3 >= 0 && d4 <= d3) {
    out[0] = bx; out[1] = by; out[2] = bz;
    return bpx * bpx + bpy * bpy + bpz * bpz;
  }
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const v = d1 / (d1 - d3);
    out[0] = ax + abx * v; out[1] = ay + aby * v; out[2] = az + abz * v;
    return squaredDistance(px, py, pz, out);
  }
  const cpx = px - cx;
  const cpy = py - cy;
  const cpz = pz - cz;
  const d5 = abx * cpx + aby * cpy + abz * cpz;
  const d6 = acx * cpx + acy * cpy + acz * cpz;
  if (d6 >= 0 && d5 <= d6) {
    out[0] = cx; out[1] = cy; out[2] = cz;
    return cpx * cpx + cpy * cpy + cpz * cpz;
  }
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const w = d2 / (d2 - d6);
    out[0] = ax + acx * w; out[1] = ay + acy * w; out[2] = az + acz * w;
    return squaredDistance(px, py, pz, out);
  }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
    const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    out[0] = bx + (cx - bx) * w; out[1] = by + (cy - by) * w; out[2] = bz + (cz - bz) * w;
    return squaredDistance(px, py, pz, out);
  }
  const denominator = va + vb + vc;
  if (!(denominator > 0)) {
    // Degenerate triangle: fall back to its three edges.
    const scratch = [0, 0, 0];
    let best = pointSegmentSquared(px, py, pz, ax, ay, az, bx, by, bz, out);
    let candidate = pointSegmentSquared(px, py, pz, bx, by, bz, cx, cy, cz, scratch);
    if (candidate < best) { best = candidate; out[0] = scratch[0]; out[1] = scratch[1]; out[2] = scratch[2]; }
    candidate = pointSegmentSquared(px, py, pz, cx, cy, cz, ax, ay, az, scratch);
    if (candidate < best) { best = candidate; out[0] = scratch[0]; out[1] = scratch[1]; out[2] = scratch[2]; }
    return best;
  }
  const v = vb / denominator;
  const w = vc / denominator;
  out[0] = ax + abx * v + acx * w;
  out[1] = ay + aby * v + acy * w;
  out[2] = az + abz * v + acz * w;
  return squaredDistance(px, py, pz, out);
}

// Broad phase: for every triangle, visit only voxel centres inside its AABB
// expanded by the band radius. Narrow phase: exact point-triangle distance.
// This is not a BVH; a single huge triangle still visits the whole grid.
function computeDistances(positions, indices, grid) {
  const [nx, ny, nz] = grid.dimensions;
  const [ox, oy, oz] = grid.origin;
  const h = grid.voxelSize;
  const bandRadius = grid.bandRadius;
  const total = nx * ny * nz;
  const squared = new Float64Array(total).fill(Infinity);
  const nearest = new Int32Array(total).fill(-1);
  const out = new Float64Array(3);
  const triangleCount = indices.length / 3;
  for (let triangle = 0; triangle < triangleCount; triangle++) {
    const ia = indices[triangle * 3] * 3;
    const ib = indices[triangle * 3 + 1] * 3;
    const ic = indices[triangle * 3 + 2] * 3;
    const ax = positions[ia], ay = positions[ia + 1], az = positions[ia + 2];
    const bx = positions[ib], by = positions[ib + 1], bz = positions[ib + 2];
    const cx = positions[ic], cy = positions[ic + 1], cz = positions[ic + 2];
    const ix0 = Math.max(0, Math.floor((Math.min(ax, bx, cx) - bandRadius - ox) / h));
    const ix1 = Math.min(nx - 1, Math.ceil((Math.max(ax, bx, cx) + bandRadius - ox) / h));
    const iy0 = Math.max(0, Math.floor((Math.min(ay, by, cy) - bandRadius - oy) / h));
    const iy1 = Math.min(ny - 1, Math.ceil((Math.max(ay, by, cy) + bandRadius - oy) / h));
    const iz0 = Math.max(0, Math.floor((Math.min(az, bz, cz) - bandRadius - oz) / h));
    const iz1 = Math.min(nz - 1, Math.ceil((Math.max(az, bz, cz) + bandRadius - oz) / h));
    if (ix0 > ix1 || iy0 > iy1 || iz0 > iz1) continue;
    for (let iz = iz0; iz <= iz1; iz++) {
      const pz = oz + iz * h;
      for (let iy = iy0; iy <= iy1; iy++) {
        const py = oy + iy * h;
        const row = (iz * ny + iy) * nx;
        for (let ix = ix0; ix <= ix1; ix++) {
          const d2 = closestPointTriangleSquared(ox + ix * h, py, pz, ax, ay, az, bx, by, bz, cx, cy, cz, out);
          const voxel = row + ix;
          if (d2 < squared[voxel]) {
            squared[voxel] = d2;
            nearest[voxel] = triangle;
          }
        }
      }
    }
  }
  const distance = new Float64Array(total);
  for (let voxel = 0; voxel < total; voxel++) {
    distance[voxel] = squared[voxel] === Infinity ? Infinity : Math.sqrt(squared[voxel]);
  }
  return { distance, nearest };
}

// A closed, consistently oriented two-manifold gets a global inside/outside
// test by counting +X ray crossings per (y,z) row. Edges shared by exactly two
// triangles in opposite directions; position keys (not indices) weld seams.
function analyzeWatertight(positions, indices) {
  const vertexCount = positions.length / 3;
  const keys = new Array(vertexCount);
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    keys[vertex] = `${positions[vertex * 3]},${positions[vertex * 3 + 1]},${positions[vertex * 3 + 2]}`;
  }
  const edges = new Map();
  const directed = [[0, 1], [1, 2], [2, 0]];
  for (let triangle = 0; triangle < indices.length / 3; triangle++) {
    for (const [from, to] of directed) {
      const keyA = keys[indices[triangle * 3 + from]];
      const keyB = keys[indices[triangle * 3 + to]];
      if (keyA === keyB) return { watertight: false, reason: 'degenerate-edge' };
      const forward = keyA < keyB;
      const key = forward ? `${keyA}|${keyB}` : `${keyB}|${keyA}`;
      let entry = edges.get(key);
      if (entry === undefined) {
        entry = { count: 0, forward: 0, backward: 0 };
        edges.set(key, entry);
      }
      entry.count++;
      if (forward) entry.forward++; else entry.backward++;
    }
  }
  for (const entry of edges.values()) {
    if (entry.count !== 2 || entry.forward !== 1 || entry.backward !== 1) {
      return { watertight: false, reason: 'open-or-non-manifold' };
    }
  }
  return { watertight: true, reason: 'closed-two-manifold' };
}

function computeParityInside(positions, indices, grid) {
  const [nx, ny, nz] = grid.dimensions;
  const [ox, oy, oz] = grid.origin;
  const h = grid.voxelSize;
  const total = nx * ny * nz;
  const inside = new Uint8Array(total);
  const triangleCount = indices.length / 3;
  const crossings = new Float64Array(triangleCount);
  // Fixed, axis-dependent sub-voxel offsets so rays cannot land exactly on a
  // shared projected edge (equal offsets would keep y == z and hit the diagonal
  // of an axis-aligned box). They are deterministic and only affect voxels
  // within 1e-6 voxels of the surface.
  const offsetY = h * 1e-7;
  const offsetZ = h * 2.7e-7;
  for (let iz = 0; iz < nz; iz++) {
    const z = oz + iz * h + offsetZ;
    for (let iy = 0; iy < ny; iy++) {
      const y = oy + iy * h + offsetY;
      let count = 0;
      for (let triangle = 0; triangle < triangleCount; triangle++) {
        const ia = indices[triangle * 3] * 3;
        const ib = indices[triangle * 3 + 1] * 3;
        const ic = indices[triangle * 3 + 2] * 3;
        const ay = positions[ia + 1], az = positions[ia + 2];
        const by = positions[ib + 1], bz = positions[ib + 2];
        const cy = positions[ic + 1], cz = positions[ic + 2];
        const area = (by - ay) * (cz - az) - (bz - az) * (cy - ay);
        if (area === 0) continue;
        const weightA = ((by - y) * (cz - z) - (bz - z) * (cy - y)) / area;
        if (weightA <= 0) continue;
        const weightB = ((cy - y) * (az - z) - (cz - z) * (ay - y)) / area;
        if (weightB <= 0) continue;
        const weightC = 1 - weightA - weightB;
        if (weightC <= 0) continue;
        crossings[count++] = weightA * positions[ia] + weightB * positions[ib] + weightC * positions[ic];
      }
      const list = crossings.subarray(0, count);
      list.sort();
      const row = (iz * ny + iy) * nx;
      let passed = 0;
      for (let ix = 0; ix < nx; ix++) {
        const px = ox + ix * h;
        while (passed < count && list[passed] < px) passed++;
        if (((count - passed) & 1) === 1) inside[row + ix] = 1;
      }
    }
  }
  return inside;
}

function computeSigns(positions, indices, grid, nearest, watertight) {
  const total = grid.dimensions[0] * grid.dimensions[1] * grid.dimensions[2];
  const signs = new Int8Array(total).fill(1);
  if (watertight) {
    const inside = computeParityInside(positions, indices, grid);
    for (let voxel = 0; voxel < total; voxel++) if (inside[voxel]) signs[voxel] = -1;
    return signs;
  }
  // Non-watertight input has no global interior. Use the local face normal of
  // the closest triangle only; untouched far voxels stay positive.
  const [nx, ny, nz] = grid.dimensions;
  const [ox, oy, oz] = grid.origin;
  const h = grid.voxelSize;
  const out = new Float64Array(3);
  for (let voxel = 0; voxel < total; voxel++) {
    const triangle = nearest[voxel];
    if (triangle < 0) continue;
    const ix = voxel % nx;
    const iy = Math.floor(voxel / nx) % ny;
    const iz = Math.floor(voxel / (nx * ny));
    const px = ox + ix * h;
    const py = oy + iy * h;
    const pz = oz + iz * h;
    const ia = indices[triangle * 3] * 3;
    const ib = indices[triangle * 3 + 1] * 3;
    const ic = indices[triangle * 3 + 2] * 3;
    const ax = positions[ia], ay = positions[ia + 1], az = positions[ia + 2];
    const bx = positions[ib], by = positions[ib + 1], bz = positions[ib + 2];
    const cx = positions[ic], cy = positions[ic + 1], cz = positions[ic + 2];
    closestPointTriangleSquared(px, py, pz, ax, ay, az, bx, by, bz, cx, cy, cz, out);
    const abx = bx - ax, aby = by - ay, abz = bz - az;
    const acx = cx - ax, acy = cy - ay, acz = cz - az;
    const normalX = aby * acz - abz * acy;
    const normalY = abz * acx - abx * acz;
    const normalZ = abx * acy - aby * acx;
    const dot = (px - out[0]) * normalX + (py - out[1]) * normalY + (pz - out[2]) * normalZ;
    if (dot < 0) signs[voxel] = -1;
  }
  return signs;
}

function buildValues(distance, signs, farValue) {
  const values = new Float32Array(distance.length);
  for (let voxel = 0; voxel < distance.length; voxel++) {
    const distanceValue = distance[voxel];
    const magnitude = distanceValue === Infinity || distanceValue > farValue ? farValue : distanceValue;
    values[voxel] = signs[voxel] < 0 ? -magnitude : magnitude;
  }
  return values;
}

// Occupancy metadata for a future surface-cache step. Classified from the
// quantized float32 values with thresholds that verify recomputes identically.
function classifyValues(values, voxelSize, farValue) {
  // Thresholds are float32 so bake and verify classify the quantized values
  // identically (a voxel exactly half a voxel from the surface would otherwise
  // round up in float32 and never count as a surface voxel).
  const halfVoxel = Math.fround(voxelSize / 2);
  const far32 = Math.fround(farValue);
  let voxelsInBand = 0;
  let surfaceVoxels = 0;
  let insideVoxels = 0;
  for (let voxel = 0; voxel < values.length; voxel++) {
    const value = values[voxel];
    const magnitude = Math.abs(value);
    if (magnitude < far32) voxelsInBand++;
    if (magnitude <= halfVoxel) surfaceVoxels++;
    if (value < 0) insideVoxels++;
  }
  return { voxelsInBand, surfaceVoxels, insideVoxels, outsideVoxels: values.length - insideVoxels };
}

// Tile the volume x-fastest, then y, then z into 8^3 chunks. Chunk bytes are
// contiguous slices of the data file so a loader can page one chunk at a time.
function buildChunks(values, grid) {
  const [nx, ny, nz] = grid.dimensions;
  const chunksPerAxis = nx / CHUNK_EDGE;
  const data = Buffer.alloc(values.length * 4);
  const chunks = [];
  const tile = new Float32Array(CHUNK_VOXELS);
  const halfVoxel = Math.fround(grid.voxelSize / 2);
  let chunkIndex = 0;
  for (let cz = 0; cz < chunksPerAxis; cz++) {
    for (let cy = 0; cy < chunksPerAxis; cy++) {
      for (let cx = 0; cx < chunksPerAxis; cx++) {
        let cursor = 0;
        let surfaceVoxels = 0;
        for (let lz = 0; lz < CHUNK_EDGE; lz++) {
          for (let ly = 0; ly < CHUNK_EDGE; ly++) {
            const row = ((cz * CHUNK_EDGE + lz) * ny + (cy * CHUNK_EDGE + ly)) * nx + cx * CHUNK_EDGE;
            for (let lx = 0; lx < CHUNK_EDGE; lx++) {
              const value = values[row + lx];
              tile[cursor++] = value;
              if (Math.abs(value) <= halfVoxel) surfaceVoxels++;
            }
          }
        }
        const bytes = Buffer.from(tile.buffer, tile.byteOffset, tile.byteLength);
        const offset = chunkIndex * CHUNK_BYTES;
        const hash = sha256(bytes);
        bytes.copy(data, offset);
        chunks.push({
          index: chunkIndex,
          origin: [cx * CHUNK_EDGE, cy * CHUNK_EDGE, cz * CHUNK_EDGE],
          dimensions: [CHUNK_EDGE, CHUNK_EDGE, CHUNK_EDGE],
          offset,
          bytes: CHUNK_BYTES,
          sha256: hash,
          surfaceVoxels,
        });
        chunkIndex++;
      }
    }
  }
  return { data, chunks };
}

export async function bakeSdf(mesh, outputDirectory, options = {}) {
  if (!outputDirectory) fail(ERROR_CODES.ARGUMENT, 'bake requires an output directory');
  const resolution = options.resolution ?? DEFAULT_RESOLUTION;
  const bandVoxels = options.bandVoxels ?? DEFAULT_BAND_VOXELS;
  if (!Number.isSafeInteger(resolution) || resolution < CHUNK_EDGE || resolution % CHUNK_EDGE !== 0) {
    fail(ERROR_CODES.ARGUMENT, `resolution must be a multiple of ${CHUNK_EDGE} (got ${resolution})`);
  }
  if (!Number.isSafeInteger(bandVoxels) || bandVoxels < 1) fail(ERROR_CODES.ARGUMENT, `band must be a positive integer (got ${bandVoxels})`);
  if (resolution - 2 * bandVoxels < 1) fail(ERROR_CODES.ARGUMENT, `resolution ${resolution} leaves no interior voxels for band ±${bandVoxels}`);
  const { positions, indices } = mesh;
  assertMeshArrays(positions, indices);
  const meshBounds = boundsOfFlat(positions);
  const grid = createGrid(meshBounds, resolution, bandVoxels);
  const { distance, nearest } = computeDistances(positions, indices, grid);
  const { watertight, reason } = analyzeWatertight(positions, indices);
  const signs = computeSigns(positions, indices, grid, nearest, watertight);
  const values = buildValues(distance, signs, grid.bandRadius);
  const surface = classifyValues(values, grid.voxelSize, grid.bandRadius);
  const { data, chunks } = buildChunks(values, grid);
  const source = options.source ?? mesh.source ?? {
    kind: 'mesh',
    contentHash: meshContentHash(positions, indices),
    vertices: positions.length / 3,
    triangles: indices.length / 3,
    bounds: meshBounds,
  };
  if (source === null || typeof source !== 'object' || Array.isArray(source) || !isHash(source.contentHash)) {
    fail(ERROR_CODES.INPUT, 'source.contentHash must be a lowercase SHA-256 hex string');
  }
  if (source.vertices !== positions.length / 3 || source.triangles !== indices.length / 3) {
    fail(ERROR_CODES.INPUT, 'source vertex/triangle counts do not match the baked mesh');
  }
  if (!source.bounds || !isVector3(source.bounds.min) || !isVector3(source.bounds.max)) {
    fail(ERROR_CODES.INPUT, 'source.bounds must be {min:[x,y,z],max:[x,y,z]}');
  }
  const manifest = {
    formatVersion: FORMAT_VERSION,
    kind: VOLUME_KIND,
    generator: { name: 'nativegpu-offline-sdf-bake', version: TOOL_VERSION },
    units: 'metres',
    signedConvention: {
      inside: 'negative',
      outside: 'positive',
      note: 'Stored value is the distance in metres to the triangle mesh; the far field is clamped to band.farValue and keeps its sign.',
    },
    source,
    dimensions: [...grid.dimensions],
    origin: [...grid.origin],
    voxelSize: grid.voxelSize,
    bounds: meshBounds,
    band: { voxels: bandVoxels, farValue: grid.bandRadius, clamped: true },
    sign: {
      method: watertight ? 'ray-parity' : 'closest-triangle-normal',
      watertight,
      farField: watertight ? 'parity' : 'positive',
      reason,
    },
    layout: {
      format: 'float32le',
      endianness: 'little',
      voxelOrder: 'x-fastest, then y, then z',
      chunkSize: [CHUNK_EDGE, CHUNK_EDGE, CHUNK_EDGE],
      samplePosition: 'origin + voxelSize * (ix, iy, iz)',
    },
    chunks,
    data: { path: DATA_NAME, bytes: data.length, sha256: sha256(data) },
    surface,
  };
  manifest.volumeDigest = computeVolumeDigest(manifest);
  const root = path.resolve(outputDirectory);
  await fs.mkdir(root, { recursive: true });
  await fs.writeFile(path.join(root, DATA_NAME), data);
  await fs.writeFile(path.join(root, MANIFEST_NAME), `${stableJSON(manifest)}\n`, 'utf8');
  return {
    manifestPath: path.join(root, MANIFEST_NAME),
    dataPath: path.join(root, DATA_NAME),
    manifest,
    volumeDigest: manifest.volumeDigest,
    dimensions: manifest.dimensions,
    voxelSize: manifest.voxelSize,
    chunks: chunks.length,
    bytes: data.length,
    surface,
  };
}

function resolveInside(root, relativePath) {
  if (typeof relativePath !== 'string' || relativePath.length === 0 || path.isAbsolute(relativePath)) {
    fail(ERROR_CODES.PATH, `Data path is not relative: ${relativePath}`);
  }
  const full = path.resolve(root, relativePath);
  const relative = path.relative(root, full);
  if (relative.startsWith('..') || path.isAbsolute(relative)) fail(ERROR_CODES.PATH, `Data path escapes the volume directory: ${relativePath}`);
  return full;
}

function assertVolumeManifest(manifest) {
  const bad = message => fail(ERROR_CODES.MANIFEST, message);
  if (manifest === null || typeof manifest !== 'object' || Array.isArray(manifest)) bad('Volume manifest must be a JSON object');
  if (manifest.formatVersion !== FORMAT_VERSION) bad(`formatVersion must be ${FORMAT_VERSION}`);
  if (manifest.kind !== VOLUME_KIND) bad(`kind must be ${VOLUME_KIND}`);
  if (manifest.units !== 'metres') bad('units must be metres');
  if (!isHash(manifest.volumeDigest)) bad('volumeDigest must be a lowercase SHA-256 hex string');
  const dimensions = manifest.dimensions;
  if (!Array.isArray(dimensions) || dimensions.length !== 3
    || dimensions.some(value => !Number.isSafeInteger(value) || value < CHUNK_EDGE || value % CHUNK_EDGE !== 0)) {
    bad(`dimensions must be three multiples of ${CHUNK_EDGE}`);
  }
  if (dimensions[0] !== dimensions[1] || dimensions[0] !== dimensions[2]) bad('dimensions must describe a cubic grid');
  if (!isVector3(manifest.origin)) bad('origin must be three finite numbers');
  if (!Number.isFinite(manifest.voxelSize) || manifest.voxelSize <= 0) bad('voxelSize must be a positive number');
  if (!manifest.signedConvention || manifest.signedConvention.inside !== 'negative' || manifest.signedConvention.outside !== 'positive') {
    bad('signedConvention must declare negative inside and positive outside');
  }
  const band = manifest.band;
  if (band === null || typeof band !== 'object' || Array.isArray(band)) bad('band must be an object');
  if (!Number.isSafeInteger(band.voxels) || band.voxels < 1) bad('band.voxels must be a positive integer');
  if (!Number.isFinite(band.farValue) || band.farValue <= 0) bad('band.farValue must be a positive number');
  if (band.farValue !== band.voxels * manifest.voxelSize) bad('band.farValue must equal band.voxels * voxelSize');
  if (band.clamped !== true) bad('band.clamped must be true (the far field is clamped)');
  const sign = manifest.sign;
  if (sign === null || typeof sign !== 'object' || Array.isArray(sign)) bad('sign must be an object');
  if (sign.method !== 'ray-parity' && sign.method !== 'closest-triangle-normal') bad('sign.method is unsupported');
  if (typeof sign.watertight !== 'boolean') bad('sign.watertight must be a boolean');
  if (sign.watertight !== (sign.method === 'ray-parity')) bad('sign.method and sign.watertight disagree');
  const layout = manifest.layout;
  if (layout === null || typeof layout !== 'object' || Array.isArray(layout)) bad('layout must be an object');
  if (layout.format !== 'float32le') bad('layout.format must be float32le');
  if (layout.endianness !== 'little') bad('layout.endianness must be little');
  if (layout.voxelOrder !== 'x-fastest, then y, then z') bad('layout.voxelOrder is unsupported');
  if (!Array.isArray(layout.chunkSize) || layout.chunkSize.length !== 3 || layout.chunkSize.some(value => value !== CHUNK_EDGE)) {
    bad(`layout.chunkSize must be [${CHUNK_EDGE},${CHUNK_EDGE},${CHUNK_EDGE}]`);
  }
  if (layout.samplePosition !== 'origin + voxelSize * (ix, iy, iz)') bad('layout.samplePosition is unsupported');
  const source = manifest.source;
  if (source === null || typeof source !== 'object' || Array.isArray(source)) bad('source must be an object');
  if (!isHash(source.contentHash)) bad('source.contentHash must be a lowercase SHA-256 hex string');
  if (!Number.isSafeInteger(source.vertices) || source.vertices < 3) bad('source.vertices must be an integer >= 3');
  if (!Number.isSafeInteger(source.triangles) || source.triangles < 1) bad('source.triangles must be a positive integer');
  if (!source.bounds || !isVector3(source.bounds.min) || !isVector3(source.bounds.max)) bad('source.bounds must be {min:[x,y,z],max:[x,y,z]}');
  const data = manifest.data;
  if (data === null || typeof data !== 'object' || Array.isArray(data)) bad('data must be an object');
  if (typeof data.path !== 'string' || data.path.length === 0 || path.isAbsolute(data.path)) bad('data.path must be a relative path');
  const total = dimensions[0] * dimensions[1] * dimensions[2];
  if (data.bytes !== total * 4) bad(`data.bytes must be ${total * 4}`);
  if (!isHash(data.sha256)) bad('data.sha256 must be a lowercase SHA-256 hex string');
  const chunksPerAxis = dimensions[0] / CHUNK_EDGE;
  const expectedChunks = chunksPerAxis ** 3;
  if (!Array.isArray(manifest.chunks) || manifest.chunks.length !== expectedChunks) bad(`chunks must contain ${expectedChunks} entries`);
  const seenOrigins = new Set();
  manifest.chunks.forEach((chunk, index) => {
    if (chunk === null || typeof chunk !== 'object' || Array.isArray(chunk)) bad(`Chunk ${index} must be an object`);
    if (chunk.index !== index) bad('Chunk indices must equal their array position');
    if (!Array.isArray(chunk.origin) || chunk.origin.length !== 3
      || chunk.origin.some(value => !Number.isSafeInteger(value) || value < 0 || value % CHUNK_EDGE !== 0 || value + CHUNK_EDGE > dimensions[0])) {
      bad(`Chunk ${index} has an invalid origin`);
    }
    if (!Array.isArray(chunk.dimensions) || chunk.dimensions.length !== 3 || chunk.dimensions.some(value => value !== CHUNK_EDGE)) {
      bad(`Chunk ${index} has invalid dimensions`);
    }
    if (chunk.offset !== index * CHUNK_BYTES) bad(`Chunk ${index} has a non-contiguous offset`);
    if (chunk.bytes !== CHUNK_BYTES) bad(`Chunk ${index} has the wrong byte count`);
    if (!isHash(chunk.sha256)) bad(`Chunk ${index} has an invalid sha256`);
    if (!Number.isSafeInteger(chunk.surfaceVoxels) || chunk.surfaceVoxels < 0 || chunk.surfaceVoxels > CHUNK_VOXELS) {
      bad(`Chunk ${index} has an invalid surfaceVoxels count`);
    }
    const originKey = chunk.origin.join(',');
    if (seenOrigins.has(originKey)) bad(`Chunk ${index} repeats an origin`);
    seenOrigins.add(originKey);
  });
  const surface = manifest.surface;
  if (surface === null || typeof surface !== 'object' || Array.isArray(surface)) bad('surface must be an object');
  for (const key of ['voxelsInBand', 'surfaceVoxels', 'insideVoxels', 'outsideVoxels']) {
    if (!Number.isSafeInteger(surface[key]) || surface[key] < 0 || surface[key] > total) bad(`surface.${key} is not a valid voxel count`);
  }
  if (surface.surfaceVoxels > surface.voxelsInBand) bad('surface.surfaceVoxels must not exceed surface.voxelsInBand');
  if (surface.outsideVoxels !== total - surface.insideVoxels) bad('surface.outsideVoxels must equal dims volume minus insideVoxels');
}

// Reads and fully verifies a volume: manifest structure, volume digest, data
// size/hash, every chunk hash, chunk surface counts and volume-wide counts.
async function openVolume(manifestPath) {
  const file = path.resolve(manifestPath);
  let raw;
  try {
    raw = await fs.readFile(file, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') fail(ERROR_CODES.MISSING, `Volume manifest not found: ${file}`);
    throw error;
  }
  let manifest;
  try {
    manifest = JSON.parse(raw);
  } catch {
    fail(ERROR_CODES.MANIFEST, `Volume manifest is not valid JSON: ${file}`);
  }
  assertVolumeManifest(manifest);
  const digest = computeVolumeDigest(manifest);
  if (digest !== manifest.volumeDigest) {
    fail(ERROR_CODES.TAMPERED, 'Volume digest mismatch', [`expected ${manifest.volumeDigest}`, `actual ${digest}`]);
  }
  const root = path.dirname(file);
  const dataPath = resolveInside(root, manifest.data.path);
  let data;
  try {
    data = await fs.readFile(dataPath);
  } catch (error) {
    if (error.code === 'ENOENT') fail(ERROR_CODES.MISSING, `Volume data not found: ${dataPath}`);
    throw error;
  }
  const [nx, ny, nz] = manifest.dimensions;
  const total = nx * ny * nz;
  if (data.length !== total * 4 || data.length !== manifest.data.bytes) {
    fail(ERROR_CODES.TAMPERED, 'Volume data size mismatch', [`expected ${total * 4}`, `actual ${data.length}`]);
  }
  const dataHash = sha256(data);
  if (dataHash !== manifest.data.sha256) {
    fail(ERROR_CODES.TAMPERED, 'Volume data hash mismatch', [`expected ${manifest.data.sha256}`, `actual ${dataHash}`]);
  }
  const values = new Float32Array(total);
  const stats = { voxelsInBand: 0, surfaceVoxels: 0, insideVoxels: 0 };
  const halfVoxel = Math.fround(manifest.voxelSize / 2);
  const far32 = Math.fround(manifest.band.farValue);
  for (const chunk of manifest.chunks) {
    const slice = data.subarray(chunk.offset, chunk.offset + CHUNK_BYTES);
    const hash = sha256(slice);
    if (hash !== chunk.sha256) {
      fail(ERROR_CODES.TAMPERED, `Chunk hash mismatch at ${chunk.index}`, [`expected ${chunk.sha256}`, `actual ${hash}`]);
    }
    const view = new DataView(slice.buffer, slice.byteOffset, slice.byteLength);
    const [ox, oy, oz] = chunk.origin;
    let surfaceInChunk = 0;
    let cursor = 0;
    for (let lz = 0; lz < CHUNK_EDGE; lz++) {
      for (let ly = 0; ly < CHUNK_EDGE; ly++) {
        for (let lx = 0; lx < CHUNK_EDGE; lx++) {
          const value = view.getFloat32(cursor * 4, true);
          cursor++;
          const magnitude = Math.abs(value);
          if (magnitude < far32) stats.voxelsInBand++;
          if (magnitude <= halfVoxel) { stats.surfaceVoxels++; surfaceInChunk++; }
          if (value < 0) stats.insideVoxels++;
          values[((oz + lz) * ny + (oy + ly)) * nx + (ox + lx)] = value;
        }
      }
    }
    if (surfaceInChunk !== chunk.surfaceVoxels) {
      fail(ERROR_CODES.TAMPERED, `Chunk surfaceVoxels mismatch at ${chunk.index}`, [`expected ${chunk.surfaceVoxels}`, `actual ${surfaceInChunk}`]);
    }
  }
  stats.outsideVoxels = total - stats.insideVoxels;
  const manifestSurface = manifest.surface;
  if (stats.voxelsInBand !== manifestSurface.voxelsInBand
    || stats.surfaceVoxels !== manifestSurface.surfaceVoxels
    || stats.insideVoxels !== manifestSurface.insideVoxels
    || stats.outsideVoxels !== manifestSurface.outsideVoxels) {
    fail(ERROR_CODES.TAMPERED, 'Surface occupancy metadata does not match the volume data');
  }
  return { manifestPath: file, dataPath, manifest, values };
}

export async function loadVolume(manifestPath) {
  return openVolume(manifestPath);
}

export async function verifyVolume(manifestPath) {
  const volume = await openVolume(manifestPath);
  const manifest = volume.manifest;
  return {
    ok: true,
    manifestPath: volume.manifestPath,
    dataPath: volume.dataPath,
    volumeDigest: manifest.volumeDigest,
    dimensions: manifest.dimensions,
    origin: manifest.origin,
    voxelSize: manifest.voxelSize,
    band: manifest.band,
    sign: manifest.sign,
    chunks: manifest.chunks.length,
    bytes: manifest.data.bytes,
    surface: manifest.surface,
    source: manifest.source,
  };
}

// Trilinear sample at a world-space point (metres). Points outside the grid are
// clamped to the boundary and reported as clamped.
export function sampleTrilinear(volume, point) {
  if (!isVector3(point)) fail(ERROR_CODES.ARGUMENT, 'point must be three finite numbers');
  const { manifest, values } = volume;
  const dimensions = manifest.dimensions;
  const origin = manifest.origin;
  const h = manifest.voxelSize;
  let clamped = false;
  const continuous = [0, 1, 2].map(axis => (point[axis] - origin[axis]) / h);
  const gridPoint = continuous.map((value, axis) => {
    const maximum = dimensions[axis] - 1;
    if (value < 0 || value > maximum) clamped = true;
    return Math.min(maximum, Math.max(0, value));
  });
  const low = gridPoint.map(value => Math.floor(value));
  const high = low.map((value, axis) => Math.min(value + 1, dimensions[axis] - 1));
  const fraction = gridPoint.map((value, axis) => value - Math.floor(value));
  const at = (ix, iy, iz) => values[(iz * dimensions[1] + iy) * dimensions[0] + ix];
  const x0 = low[0], y0 = low[1], z0 = low[2];
  const x1 = high[0], y1 = high[1], z1 = high[2];
  const tx = fraction[0], ty = fraction[1], tz = fraction[2];
  const c00 = at(x0, y0, z0) * (1 - tx) + at(x1, y0, z0) * tx;
  const c10 = at(x0, y1, z0) * (1 - tx) + at(x1, y1, z0) * tx;
  const c01 = at(x0, y0, z1) * (1 - tx) + at(x1, y0, z1) * tx;
  const c11 = at(x0, y1, z1) * (1 - tx) + at(x1, y1, z1) * tx;
  const c0 = c00 * (1 - ty) + c10 * ty;
  const c1 = c01 * (1 - ty) + c11 * ty;
  return { value: c0 * (1 - tz) + c1 * tz, clamped, grid: gridPoint };
}

export function sphereMesh(options = {}) {
  const radius = options.radius ?? 1;
  const segments = options.segments ?? 48;
  const rings = options.rings ?? 24;
  const center = options.center ?? [0, 0, 0];
  if (!(radius > 0) || !Number.isSafeInteger(segments) || segments < 3 || !Number.isSafeInteger(rings) || rings < 2) {
    fail(ERROR_CODES.INPUT, 'sphereMesh needs radius > 0, segments >= 3 and rings >= 2');
  }
  const positions = [];
  const indices = [];
  const addVertex = point => { positions.push(point[0], point[1], point[2]); return positions.length / 3 - 1; };
  const north = addVertex([center[0], center[1] + radius, center[2]]);
  const rows = [];
  for (let ring = 1; ring < rings; ring++) {
    const theta = Math.PI * ring / rings;
    const y = Math.cos(theta);
    const r = Math.sin(theta);
    const row = [];
    for (let segment = 0; segment < segments; segment++) {
      const phi = 2 * Math.PI * segment / segments;
      row.push(addVertex([
        center[0] + radius * r * Math.sin(phi),
        center[1] + radius * y,
        center[2] + radius * r * Math.cos(phi),
      ]));
    }
    rows.push(row);
  }
  const south = addVertex([center[0], center[1] - radius, center[2]]);
  for (let segment = 0; segment < segments; segment++) {
    indices.push(north, rows[0][segment], rows[0][(segment + 1) % segments]);
  }
  for (let ring = 0; ring < rows.length - 1; ring++) {
    for (let segment = 0; segment < segments; segment++) {
      const next = (segment + 1) % segments;
      const a = rows[ring][segment];
      const b = rows[ring][next];
      const c = rows[ring + 1][next];
      const d = rows[ring + 1][segment];
      indices.push(a, c, b, a, d, c);
    }
  }
  const last = rows.length - 1;
  for (let segment = 0; segment < segments; segment++) {
    indices.push(south, rows[last][(segment + 1) % segments], rows[last][segment]);
  }
  return { positions, indices };
}

export function boxMesh(options = {}) {
  const half = options.half ?? 1;
  const center = options.center ?? [0, 0, 0];
  const halfX = Array.isArray(half) ? half[0] : half;
  const halfY = Array.isArray(half) ? half[1] : half;
  const halfZ = Array.isArray(half) ? half[2] : half;
  if (!(halfX > 0) || !(halfY > 0) || !(halfZ > 0)) fail(ERROR_CODES.INPUT, 'boxMesh needs positive half extents');
  const signs = [
    [-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1],
    [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1],
  ];
  const positions = [];
  for (const [sx, sy, sz] of signs) positions.push(center[0] + sx * halfX, center[1] + sy * halfY, center[2] + sz * halfZ);
  const quads = [
    [1, 2, 6, 5],
    [0, 4, 7, 3],
    [3, 7, 6, 2],
    [0, 1, 5, 4],
    [4, 5, 6, 7],
    [0, 3, 2, 1],
  ];
  const indices = [];
  for (const [i0, i1, i2, i3] of quads) indices.push(i0, i1, i2, i0, i2, i3);
  return { positions, indices };
}

function measureAccuracy(volume, analytic, bandVoxels) {
  const { manifest, values } = volume;
  const [nx, ny, nz] = manifest.dimensions;
  const [ox, oy, oz] = manifest.origin;
  const h = manifest.voxelSize;
  const centerLimit = (bandVoxels - 0.5) * h;
  let centerMax = 0;
  let centerSum = 0;
  let centerCount = 0;
  for (let iz = 0; iz < nz; iz++) {
    for (let iy = 0; iy < ny; iy++) {
      for (let ix = 0; ix < nx; ix++) {
        const reference = analytic([ox + ix * h, oy + iy * h, oz + iz * h]);
        if (Math.abs(reference) > centerLimit) continue;
        const stored = values[(iz * ny + iy) * nx + ix];
        const error = Math.abs(stored - reference) / h;
        if (error > centerMax) centerMax = error;
        centerSum += error;
        centerCount++;
      }
    }
  }
  const midpointLimit = Math.max(0, bandVoxels - 1) * h;
  let midpointMax = 0;
  let midpointSum = 0;
  let midpointCount = 0;
  for (let iz = 0; iz < nz - 1; iz++) {
    for (let iy = 0; iy < ny - 1; iy++) {
      for (let ix = 0; ix < nx - 1; ix++) {
        const point = [ox + (ix + 0.5) * h, oy + (iy + 0.5) * h, oz + (iz + 0.5) * h];
        const reference = analytic(point);
        if (Math.abs(reference) > midpointLimit) continue;
        const error = Math.abs(sampleTrilinear(volume, point).value - reference) / h;
        if (error > midpointMax) midpointMax = error;
        midpointSum += error;
        midpointCount++;
      }
    }
  }
  return {
    voxelSize: h,
    toleranceVoxels: 1.5 * Math.sqrt(3),
    center: {
      count: centerCount,
      maxVoxels: centerMax,
      meanVoxels: centerCount === 0 ? 0 : centerSum / centerCount,
      limitMetres: centerLimit,
    },
    midpoint: {
      count: midpointCount,
      maxVoxels: midpointMax,
      meanVoxels: midpointCount === 0 ? 0 : midpointSum / midpointCount,
      limitMetres: midpointLimit,
    },
  };
}

function checkSigns(volume, analytic, points) {
  const threshold = volume.manifest.voxelSize * 0.5;
  let mismatches = 0;
  const samples = [];
  for (const point of points) {
    const reference = analytic(point);
    const sample = sampleTrilinear(volume, point);
    const expected = reference < 0 ? 'inside' : 'outside';
    const actual = sample.value < 0 ? 'inside' : 'outside';
    const ok = Math.abs(reference) > threshold && expected === actual;
    if (!ok) mismatches++;
    samples.push({ point, reference, value: sample.value, expected, actual, clamped: sample.clamped, ok });
  }
  return { threshold, mismatches, samples };
}

const sphereAnalytic = point => Math.hypot(point[0], point[1], point[2]) - 1;
const boxAnalytic = point => {
  const qx = Math.abs(point[0]) - 1;
  const qy = Math.abs(point[1]) - 1;
  const qz = Math.abs(point[2]) - 1;
  const outside = Math.hypot(Math.max(qx, 0), Math.max(qy, 0), Math.max(qz, 0));
  return outside + Math.min(Math.max(qx, qy, qz), 0);
};

export async function selfTest() {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-sdf-selftest-'));
  try {
    const options = { resolution: DEFAULT_RESOLUTION, bandVoxels: DEFAULT_BAND_VOXELS };
    const sphere = sphereMesh({ radius: 1, segments: 48, rings: 24 });
    const box = boxMesh({ half: 1 });
    const sphereBake = await bakeSdf(sphere, path.join(root, 'sphere-a'), options);
    const sphereRepeat = await bakeSdf(sphere, path.join(root, 'sphere-b'), options);
    const boxBake = await bakeSdf(box, path.join(root, 'box'), options);
    const sphereVolume = await loadVolume(sphereBake.manifestPath);
    const boxVolume = await loadVolume(boxBake.manifestPath);
    const sphereAccuracy = measureAccuracy(sphereVolume, sphereAnalytic, DEFAULT_BAND_VOXELS);
    const boxAccuracy = measureAccuracy(boxVolume, boxAnalytic, DEFAULT_BAND_VOXELS);
    const sphereSigns = checkSigns(sphereVolume, sphereAnalytic, [
      [0, 0, 0], [0.3, 0, 0], [0, 0.6, 0], [0.5, 0.5, 0], [0, 0, 0.8],
      [1.05, 0, 0], [1.1, 0.1, 0], [0, 1.1, 0], [2.5, 0, 0],
    ]);
    const boxSigns = checkSigns(boxVolume, boxAnalytic, [
      [0, 0, 0], [0.5, 0.5, 0.5], [0.9, 0, 0], [0, 0.95, 0],
      [1.05, 0, 0], [1.06, 0.5, 0], [0, 0, 1.05], [2.5, 0, 0],
    ]);
    const manifestBytesEqual = (await fs.readFile(sphereBake.manifestPath))
      .equals(await fs.readFile(sphereRepeat.manifestPath));
    const dataBytesEqual = (await fs.readFile(sphereBake.dataPath))
      .equals(await fs.readFile(sphereRepeat.dataPath));
    const deterministic = sphereBake.volumeDigest === sphereRepeat.volumeDigest && manifestBytesEqual && dataBytesEqual;
    const tamperedDataDirectory = path.join(root, 'tampered-data');
    await fs.cp(path.dirname(boxBake.manifestPath), tamperedDataDirectory, { recursive: true });
    const tamperedDataPath = path.join(tamperedDataDirectory, DATA_NAME);
    const tamperedBytes = await fs.readFile(tamperedDataPath);
    tamperedBytes[0] ^= 0xff;
    await fs.writeFile(tamperedDataPath, tamperedBytes);
    let dataTamperCode = null;
    try {
      await verifyVolume(path.join(tamperedDataDirectory, MANIFEST_NAME));
    } catch (error) {
      dataTamperCode = error.code ?? 'UNKNOWN';
    }
    const tamperedManifestDirectory = path.join(root, 'tampered-manifest');
    await fs.cp(path.dirname(boxBake.manifestPath), tamperedManifestDirectory, { recursive: true });
    const tamperedManifestPath = path.join(tamperedManifestDirectory, MANIFEST_NAME);
    const edited = JSON.parse(await fs.readFile(tamperedManifestPath, 'utf8'));
    edited.origin[0] += 1;
    await fs.writeFile(tamperedManifestPath, `${stableJSON(edited)}\n`, 'utf8');
    let manifestTamperCode = null;
    try {
      await verifyVolume(tamperedManifestPath);
    } catch (error) {
      manifestTamperCode = error.code ?? 'UNKNOWN';
    }
    const boxVerified = await verifyVolume(boxBake.manifestPath);
    const accuracyOk = [sphereAccuracy, boxAccuracy].every(result =>
      result.center.maxVoxels <= SELF_TEST_MAX_ERROR_VOXELS && result.midpoint.maxVoxels <= SELF_TEST_MAX_ERROR_VOXELS);
    const signsOk = sphereSigns.mismatches === 0 && boxSigns.mismatches === 0;
    const tamperOk = dataTamperCode === ERROR_CODES.TAMPERED && manifestTamperCode === ERROR_CODES.TAMPERED;
    return {
      ok: deterministic && accuracyOk && signsOk && tamperOk && boxVerified.ok,
      deterministic,
      sphere: {
        volumeDigest: sphereBake.volumeDigest,
        dimensions: sphereBake.dimensions,
        voxelSize: sphereBake.voxelSize,
        band: sphereBake.manifest.band,
        sign: sphereBake.manifest.sign,
        surface: sphereBake.surface,
        accuracy: sphereAccuracy,
        signs: sphereSigns,
      },
      box: {
        volumeDigest: boxBake.volumeDigest,
        dimensions: boxBake.dimensions,
        voxelSize: boxBake.voxelSize,
        band: boxBake.manifest.band,
        sign: boxBake.manifest.sign,
        surface: boxBake.surface,
        accuracy: boxAccuracy,
        signs: boxSigns,
      },
      verify: { ok: boxVerified.ok, chunks: boxVerified.chunks, bytes: boxVerified.bytes },
      tamper: { data: dataTamperCode, manifest: manifestTamperCode },
    };
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
}

const USAGE = `Offline narrow-band SDF bake (L0, deterministic, content-addressed)
  node tools/offline/sdf-bake.mjs bake --out <directory> [--package <geometry.package.json>] [--mesh <mesh.json>] [--resolution 32] [--band 2]
  node tools/offline/sdf-bake.mjs query --volume <sdf.volume.json> --point x,y,z
  node tools/offline/sdf-bake.mjs verify --volume <sdf.volume.json>
  node tools/offline/sdf-bake.mjs self-test
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

function integerOption(value, name) {
  if (!/^-?\d+$/.test(value ?? '')) fail(ERROR_CODES.ARGUMENT, `${name} must be an integer`);
  return Number(value);
}

const fmt = (value, digits = 4) => Number(value).toFixed(digits);

async function main(args) {
  const [command, ...rest] = args;
  if (!command || command === 'help' || command === '--help') return USAGE;
  if (command === 'bake') {
    const options = parseOptions(rest, ['--out', '--package', '--mesh', '--resolution', '--band']);
    if (options['--package'] && options['--mesh']) fail(ERROR_CODES.ARGUMENT, 'bake accepts either --package or --mesh, not both');
    const output = options['--out'] ?? path.join('build', 'offline', 'sdf');
    const resolution = options['--resolution'] === undefined ? DEFAULT_RESOLUTION : integerOption(options['--resolution'], '--resolution');
    const bandVoxels = options['--band'] === undefined ? DEFAULT_BAND_VOXELS : integerOption(options['--band'], '--band');
    const mesh = options['--mesh']
      ? await loadSimpleMesh(options['--mesh'])
      : await readMeshFromPackage(options['--package'] ?? DEFAULT_PACKAGE_PATH);
    const result = await bakeSdf(mesh, output, { resolution, bandVoxels });
    return {
      manifestPath: result.manifestPath,
      dataPath: result.dataPath,
      volumeDigest: result.volumeDigest,
      dimensions: result.dimensions,
      origin: result.manifest.origin,
      voxelSize: result.voxelSize,
      band: result.manifest.band,
      sign: result.manifest.sign,
      chunks: result.chunks,
      bytes: result.bytes,
      surface: result.surface,
      source: result.manifest.source,
    };
  }
  if (command === 'query') {
    const options = parseOptions(rest, ['--volume', '--point']);
    if (!options['--volume']) fail(ERROR_CODES.ARGUMENT, 'query requires --volume');
    if (!options['--point']) fail(ERROR_CODES.ARGUMENT, 'query requires --point');
    const point = options['--point'].split(',').map(part => Number(part.trim()));
    if (point.length !== 3 || point.some(value => !Number.isFinite(value))) fail(ERROR_CODES.ARGUMENT, '--point must be x,y,z');
    const volume = await loadVolume(options['--volume']);
    const sample = sampleTrilinear(volume, point);
    return {
      volume: volume.manifestPath,
      point,
      value: sample.value,
      inside: sample.value < 0,
      clamped: sample.clamped,
      units: volume.manifest.units,
      band: volume.manifest.band,
      grid: sample.grid,
    };
  }
  if (command === 'verify') {
    const options = parseOptions(rest, ['--volume']);
    if (!options['--volume']) fail(ERROR_CODES.ARGUMENT, 'verify requires --volume');
    return verifyVolume(options['--volume']);
  }
  if (command === 'self-test') {
    if (rest.length) fail(ERROR_CODES.ARGUMENT, 'self-test takes no options');
    const result = await selfTest();
    const accuracyLine = (label, data) => [
      `${label}: dims ${data.dimensions.join('x')}, voxelSize ${data.voxelSize} m, band ±${data.band.voxels} voxels (far ${data.band.farValue} m), sign ${data.sign.method} (watertight: ${data.sign.watertight})`,
      `${label}: centres max ${fmt(data.accuracy.center.maxVoxels)} / mean ${fmt(data.accuracy.center.meanVoxels)} voxels over ${data.accuracy.center.count} samples`,
      `${label}: trilinear max ${fmt(data.accuracy.midpoint.maxVoxels)} / mean ${fmt(data.accuracy.midpoint.meanVoxels)} voxels over ${data.accuracy.midpoint.count} midpoints`,
      `${label}: sign mismatches ${data.signs.mismatches}, surface ${data.surface.surfaceVoxels} voxels, in-band ${data.surface.voxelsInBand}, digest ${data.volumeDigest}`,
    ];
    console.log([
      `self-test: ${result.ok ? 'PASS' : 'FAIL'}`,
      ...accuracyLine('sphere', result.sphere),
      ...accuracyLine('box', result.box),
      `determinism: ${result.deterministic ? 'byte-identical rebuild' : 'MISMATCH'}`,
      `tamper: data ${result.tamper.data}, manifest ${result.tamper.manifest}`,
      `verify: ${result.verify.ok ? 'ok' : 'FAILED'} (${result.verify.chunks} chunks, ${result.verify.bytes} bytes)`,
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
    console.error(JSON.stringify({ ok: false, error: { code: error.code ?? 'NG-E-SDF-IO', message: error.message, details: error.details ?? [] } }, null, 2));
    process.exitCode = 1;
  });
}

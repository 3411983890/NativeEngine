// Cooked world manifest and chunk container.
//
// A cooked world is a directory of immutable, content-addressed files plus one manifest.
// The runtime only ever needs the manifest and the files it names; the authoring recipe,
// the layer files and the component libraries can all be absent from a published build.
//
// The chunk container is deliberately boring: a fixed 64-byte header, a segment table, and
// little-endian typed payloads. Every length is validated against the remaining byte count
// before it is trusted, because a truncated or hostile chunk must produce a diagnostic, not
// an out-of-bounds read in the host.

import { CHUNK_FORMAT, CHUNK_VERSION, MANIFEST_SCHEMA, validateRecipe } from './schema.mjs';
import { WORLD_LIMITS } from './chunk.mjs';
import { isSha256, sha256Canonical } from './hash.mjs';
import { WORLD_ERROR_CODES, WorldError } from './errors.mjs';

export const CHUNK_HEADER_BYTES = 64;
export const CHUNK_SEGMENT_BYTES = 16;

export const SEGMENT = Object.freeze({
  HEIGHTS: 1,
  MATERIAL_WEIGHTS: 2,
  NORMALS: 3,
  INSTANCES: 4,
  ROAD_MESH: 5,
  RIVER_BED: 6,
  COLLISION: 7,
  BUILDINGS: 8,
  QUERY: 9,
});

export const SEGMENT_NAMES = Object.freeze(
  Object.fromEntries(Object.entries(SEGMENT).map(([name, id]) => [id, name])),
);

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

/**
 * Reads a chunk header without decoding any payload. The runtime uses this to validate a
 * chunk before it is queued for upload; the tests use it to build a corrupt chunk on purpose.
 *
 * Layout (little endian):
 *   0   char[4]  'NGRC'
 *   4   u16      version
 *   6   u16      lod
 *   8   i32      chunkX
 *   12  i32      chunkZ
 *   16  u32      chunkSizeMetres
 *   20  u32      sampleStepCentimetres
 *   24  u32      vertexCount
 *   28  u32      instanceCount
 *   32  u32      segmentCount
 *   36  u32      flags
 *   40  u64      contentHash (FNV-1a 64 of the payload bytes)
 *   48  u64      inputDigestPrefix
 *   56  u32      payloadBytes
 *   60  u32      reserved (0)
 */
export function parseChunkHeader(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < CHUNK_HEADER_BYTES) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, 'chunk is shorter than its header', { stage: 'decode' });
  }
  const magic = buffer.toString('ascii', 0, 4);
  if (magic !== CHUNK_FORMAT) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, `chunk magic is '${magic}', expected '${CHUNK_FORMAT}'`, { stage: 'decode' });
  }
  const version = buffer.readUInt16LE(4);
  if (version !== CHUNK_VERSION) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
      `chunk version ${version} is not supported (expected ${CHUNK_VERSION})`, { stage: 'decode' });
  }
  const header = {
    version,
    lod: buffer.readUInt16LE(6),
    chunkX: buffer.readInt32LE(8),
    chunkZ: buffer.readInt32LE(12),
    chunkSizeMetres: buffer.readUInt32LE(16),
    sampleStepCentimetres: buffer.readUInt32LE(20),
    vertexCount: buffer.readUInt32LE(24),
    instanceCount: buffer.readUInt32LE(28),
    segmentCount: buffer.readUInt32LE(32),
    flags: buffer.readUInt32LE(36),
    contentHash: buffer.readBigUInt64LE(40).toString(16).padStart(16, '0'),
    inputDigestPrefix: buffer.readBigUInt64LE(48).toString(16).padStart(16, '0'),
    payloadBytes: buffer.readUInt32LE(56),
    reserved: buffer.readUInt32LE(60),
  };

  if (header.lod > 3) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, `chunk LOD ${header.lod} is outside 0..3`, { stage: 'decode' });
  }
  if (header.vertexCount > WORLD_LIMITS.verticesPerAxis * WORLD_LIMITS.verticesPerAxis) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, 'chunk vertex count exceeds the contract limit', { stage: 'decode' });
  }
  if (header.instanceCount > WORLD_LIMITS.instancesPerChunk) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, 'chunk instance count exceeds the contract limit', { stage: 'decode' });
  }
  if (header.segmentCount > 32) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, 'chunk segment count exceeds the contract limit', { stage: 'decode' });
  }

  const tableBytes = header.segmentCount * CHUNK_SEGMENT_BYTES;
  const expected = CHUNK_HEADER_BYTES + tableBytes + header.payloadBytes;
  if (buffer.length !== expected) {
    throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
      `chunk is ${buffer.length} bytes, header declares ${expected}`, { stage: 'decode' });
  }

  const segments = [];
  let offset = CHUNK_HEADER_BYTES + tableBytes;
  for (let index = 0; index < header.segmentCount; index += 1) {
    const base = CHUNK_HEADER_BYTES + index * CHUNK_SEGMENT_BYTES;
    const id = buffer.readUInt16LE(base);
    const elementBytes = buffer.readUInt16LE(base + 2);
    const count = buffer.readUInt32LE(base + 4);
    const byteOffset = buffer.readUInt32LE(base + 8);
    const byteLength = buffer.readUInt32LE(base + 12);
    if (byteOffset + byteLength > header.payloadBytes) {
      throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
        `segment ${SEGMENT_NAMES[id] ?? id} runs past the payload`, { stage: 'decode' });
    }
    if (elementBytes > 0 && count * elementBytes !== byteLength) {
      throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
        `segment ${SEGMENT_NAMES[id] ?? id} declares ${count}×${elementBytes} bytes but carries ${byteLength}`,
        { stage: 'decode' });
    }
    segments.push({
      id,
      name: SEGMENT_NAMES[id] ?? `unknown-${id}`,
      elementBytes,
      count,
      offset: offset + byteOffset,
      byteLength,
    });
  }

  return { ...header, segments };
}

export function readChunkSegment(buffer, header, id) {
  const segment = header.segments.find(entry => entry.id === id);
  if (!segment) return null;
  return buffer.subarray(segment.offset, segment.offset + segment.byteLength);
}

// ---------------------------------------------------------------- manifest

const MANIFEST_REQUIRED = ['schema', 'worldId', 'generatorVersion', 'inputDigest', 'manifestHash',
  'extentMetres', 'originMetres', 'chunkSizeMetres', 'sampleStepMetres', 'chunkRange', 'chunks', 'budget'];

export function validateManifest(manifest, { errors = [] } = {}) {
  if (!isObject(manifest)) {
    errors.push('manifest: must be an object');
    return errors;
  }
  for (const key of MANIFEST_REQUIRED) {
    if (!Object.hasOwn(manifest, key)) errors.push(`manifest.${key}: is required`);
  }
  for (const key of Object.keys(manifest)) {
    if (!MANIFEST_REQUIRED.includes(key) && !['stats', 'layers', 'libraries', 'savedAt', 'sourceRecipe', 'components'].includes(key)) {
      errors.push(`manifest.${key}: key is not supported`);
    }
  }
  if (manifest.schema !== MANIFEST_SCHEMA) errors.push(`manifest.schema: must be '${MANIFEST_SCHEMA}'`);
  if (typeof manifest.worldId !== 'string' || manifest.worldId.length === 0 || manifest.worldId.length > 64) {
    errors.push('manifest.worldId: must be 1-64 characters');
  }
  if (typeof manifest.generatorVersion !== 'string' || !/^worldgen-\d+$/.test(manifest.generatorVersion)) {
    errors.push('manifest.generatorVersion: must look like worldgen-1');
  }
  for (const key of ['inputDigest', 'manifestHash']) {
    if (!isSha256(manifest[key])) errors.push(`manifest.${key}: must be 64 lowercase hex digits`);
  }
  if (!Array.isArray(manifest.extentMetres) || manifest.extentMetres.length !== 2) {
    errors.push('manifest.extentMetres: must be two numbers');
  }
  if (!Array.isArray(manifest.originMetres) || manifest.originMetres.length !== 3) {
    errors.push('manifest.originMetres: must be three numbers');
  }
  if (!finite(manifest.chunkSizeMetres) || manifest.chunkSizeMetres <= 0) errors.push('manifest.chunkSizeMetres: must be positive');
  if (!finite(manifest.sampleStepMetres) || manifest.sampleStepMetres <= 0) errors.push('manifest.sampleStepMetres: must be positive');

  const range = manifest.chunkRange;
  if (!isObject(range) || !['minCx', 'minCz', 'maxCx', 'maxCz'].every(key => Number.isInteger(range[key]))) {
    errors.push('manifest.chunkRange: must be four integers');
  }

  if (!Array.isArray(manifest.chunks)) {
    errors.push('manifest.chunks: must be an array');
  } else {
    if (manifest.chunks.length > WORLD_LIMITS.chunksTotal) errors.push('manifest.chunks: exceeds the chunk limit');
    const seen = new Set();
    manifest.chunks.forEach((chunk, index) => {
      const where = `manifest.chunks[${index}]`;
      if (!isObject(chunk)) { errors.push(`${where}: must be an object`); return; }
      for (const key of ['cx', 'cz', 'file', 'sha256', 'bytes', 'lod', 'instanceCount', 'vertexCount', 'bounds']) {
        if (!Object.hasOwn(chunk, key)) errors.push(`${where}.${key}: is required`);
      }
      for (const key of Object.keys(chunk)) {
        if (!['cx', 'cz', 'file', 'sha256', 'bytes', 'lod', 'instanceCount', 'vertexCount', 'bounds', 'minHeight', 'maxHeight'].includes(key)) {
          errors.push(`${where}.${key}: key is not supported`);
        }
      }
      if (!Number.isInteger(chunk.cx) || !Number.isInteger(chunk.cz)) errors.push(`${where}: chunk coordinates must be integers`);
      else {
        const key = `${chunk.cx},${chunk.cz}`;
        if (seen.has(key)) errors.push(`${where}: duplicates chunk ${key}`);
        else seen.add(key);
      }
      if (typeof chunk.file !== 'string' || !/^chunks\/-?\d+_-?\d+\.ngrc$/.test(chunk.file)) {
        errors.push(`${where}.file: must be 'chunks/<cx>_<cz>.ngrc'`);
      }
      if (!isSha256(chunk.sha256)) errors.push(`${where}.sha256: must be 64 lowercase hex digits`);
      if (!Number.isInteger(chunk.bytes) || chunk.bytes <= CHUNK_HEADER_BYTES || chunk.bytes > WORLD_LIMITS.fileBytes) {
        errors.push(`${where}.bytes: must be a plausible byte count`);
      }
      if (!Number.isInteger(chunk.lod) || chunk.lod < 0 || chunk.lod > 3) errors.push(`${where}.lod: must be 0..3`);
      if (!Number.isInteger(chunk.instanceCount) || chunk.instanceCount < 0) errors.push(`${where}.instanceCount: must be a non-negative integer`);
      if (!Number.isInteger(chunk.vertexCount) || chunk.vertexCount < 0) errors.push(`${where}.vertexCount: must be a non-negative integer`);
      const bounds = chunk.bounds;
      if (!isObject(bounds) || !['minX', 'minZ', 'maxX', 'maxZ'].every(key => finite(bounds[key]))) {
        errors.push(`${where}.bounds: must be four numbers`);
      }
    });
  }

  if (manifest.components !== undefined) {
    if (!Array.isArray(manifest.components) || manifest.components.length > 4096) {
      errors.push('manifest.components: must be up to 4096 entries');
    } else manifest.components.forEach((component, index) => {
      const where = `manifest.components[${index}]`;
      if (!isObject(component)) { errors.push(`${where}: must be an object`); return; }
      if (typeof component.id !== 'string' || component.id.length === 0 || component.id.length > 64) {
        errors.push(`${where}.id: must be 1-64 characters`);
      }
      if (typeof component.shape !== 'string') errors.push(`${where}.shape: must be a string`);
      if (!Array.isArray(component.sizeMetres) || component.sizeMetres.length !== 3
          || !component.sizeMetres.every(value => finite(value) && value > 0)) {
        errors.push(`${where}.sizeMetres: must be three positive numbers`);
      }
      if (!Array.isArray(component.albedo) || component.albedo.length !== 3
          || !component.albedo.every(value => finite(value) && value >= 0 && value <= 1)) {
        errors.push(`${where}.albedo: must be three channels in [0, 1]`);
      }
    });
  }

  const budget = manifest.budget;
  if (!isObject(budget)) {
    errors.push('manifest.budget: must be an object');
  } else {
    for (const key of ['profile', 'totalInstances', 'residentChunks', 'cookMillis']) {
      if (!Object.hasOwn(budget, key)) errors.push(`manifest.budget.${key}: is required`);
    }
    if (!['debug', 'desktop-balanced', 'desktop-quality', 'stress'].includes(budget.profile)) {
      errors.push('manifest.budget.profile: is not a known profile');
    }
    if (!Number.isInteger(budget.totalInstances) || budget.totalInstances < 0) errors.push('manifest.budget.totalInstances: must be a non-negative integer');
    if (!Number.isInteger(budget.residentChunks) || budget.residentChunks < 0) errors.push('manifest.budget.residentChunks: must be a non-negative integer');
    if (!finite(budget.cookMillis) || budget.cookMillis < 0) errors.push('manifest.budget.cookMillis: must be a non-negative number');
  }

  return errors;
}

export function assertManifest(manifest) {
  const errors = validateManifest(manifest);
  if (errors.length) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `cooked manifest is invalid: ${errors[0]}`, {
      stage: 'manifest',
      cause: errors.slice(0, 32),
    });
  }
  return manifest;
}

// The manifest hash identifies the *content* of a cooked world, so it must not depend on
// anything that changes between two identical cooks. Two fields do:
//
//   * `savedAt` is a timestamp;
//   * `budget.cookMillis` and the `stats` block are measurements.
//
// Hashing the whole document would give the same world a different address every time it
// was cooked, which defeats the content-addressed cache, makes the scene reference drift
// after a no-op recook, and would make "is this the manifest the scene points at?" always
// answer no. The projection below is the contract: adding a field to the manifest does not
// change its hash unless it is added here on purpose.
const HASHED_FIELDS = [
  'schema', 'worldId', 'generatorVersion', 'inputDigest',
  'extentMetres', 'originMetres', 'chunkSizeMetres', 'sampleStepMetres',
  'chunkRange', 'chunks', 'components',
];

export function manifestHashInput(manifest) {
  const projection = {};
  for (const key of HASHED_FIELDS) {
    if (manifest[key] !== undefined) projection[key] = manifest[key];
  }
  return projection;
}

export function manifestHashOf(manifest) {
  return sha256Canonical(manifestHashInput(manifest));
}

export function manifestChunkKey(cx, cz) {
  return `${cx},${cz}`;
}

export function manifestChunkPath(cx, cz) {
  return `chunks/${cx}_${cz}.ngrc`;
}

// Resolves the chunk that contains a world-space metre position, using the manifest's own
// range. Returns null outside the world rather than clamping: a query outside the world is
// "not loaded", not "the nearest edge".
export function manifestChunkAt(manifest, x, z) {
  const size = manifest.chunkSizeMetres;
  const cx = Math.floor(x / size);
  const cz = Math.floor(z / size);
  const range = manifest.chunkRange;
  if (cx < range.minCx || cx > range.maxCx || cz < range.minCz || cz > range.maxCz) return null;
  return manifest.chunks.find(chunk => chunk.cx === cx && chunk.cz === cz) ?? null;
}

export function chunkFileName(cx, cz) {
  return `${cx}_${cz}.ngrc`;
}

export { CHUNK_FORMAT, CHUNK_VERSION, MANIFEST_SCHEMA, validateRecipe };

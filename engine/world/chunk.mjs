// Chunk coordinate contract.
//
// Chunk coordinates are signed integers derived with floor(), not truncation toward zero.
// The difference only shows up at negative world coordinates, which is exactly where a
// 2 km world centred on the origin lives: a brush at x = -0.5 m must land in chunk -1, and
// truncation would put it in chunk 0 and split the seam.
//
// CPU addressing is integer chunk + local metres. The GPU never sees chunk coordinates:
// it sees camera-relative metres. Anything that converts between the two lives here so
// the JS authoring side and the C++ compiler cannot disagree.

export const DEFAULT_CHUNK_SIZE_METRES = 128;
export const DEFAULT_SAMPLE_STEP_METRES = 1;

// Hard limits. They are contract, not tuning: the recipe validator rejects anything above
// them so a 64 km world cannot be requested and then fail halfway through a cook.
export const WORLD_LIMITS = Object.freeze({
  extentMetres: [16, 8192],
  chunkSizeMetres: [16, 512],
  sampleStepMetres: [0.25, 8],
  chunksPerAxis: 512,
  chunksTotal: 262144,
  verticesPerAxis: 257,
  layers: 256,
  libraries: 64,
  overrides: 4096,
  pathsPerLayer: 512,
  pointsPerPath: 4096,
  brushStrokesPerOperation: 4096,
  instancesPerChunk: 262144,
  fileBytes: 256 * 1024 * 1024,
  recipeBytes: 4 * 1024 * 1024,
  libraryBytes: 8 * 1024 * 1024,
});

export function chunkIndexOf(metres, chunkSizeMetres) {
  // Math.floor on the quotient, not bit tricks: 8192 m at 16 m chunks is 512, and a
  // bitwise shift would silently wrap above 2^31 only after the contract already failed.
  return Math.floor(metres / chunkSizeMetres);
}

export function chunkKey(cx, cz) {
  return `${cx},${cz}`;
}

export function parseChunkKey(key) {
  const match = /^(-?\d+),(-?\d+)$/.exec(String(key ?? ''));
  if (!match) return null;
  const cx = Number(match[1]);
  const cz = Number(match[2]);
  if (!Number.isSafeInteger(cx) || !Number.isSafeInteger(cz)) return null;
  return { cx, cz };
}

export function chunkOrigin(cx, cz, chunkSizeMetres) {
  return { x: cx * chunkSizeMetres, z: cz * chunkSizeMetres };
}

export function chunkBounds(cx, cz, chunkSizeMetres) {
  const x = cx * chunkSizeMetres;
  const z = cz * chunkSizeMetres;
  return { minX: x, minZ: z, maxX: x + chunkSizeMetres, maxZ: z + chunkSizeMetres };
}

// Chunk range covering an axis-aligned box. maxX/maxZ are exclusive: a brush that ends
// exactly on a chunk border does not dirty the next chunk.
export function chunkRangeForBounds(bounds, chunkSizeMetres) {
  const minCx = chunkIndexOf(bounds.minX, chunkSizeMetres);
  const minCz = chunkIndexOf(bounds.minZ, chunkSizeMetres);
  const maxCx = Math.max(minCx, chunkIndexOf(bounds.maxX - 1e-6, chunkSizeMetres));
  const maxCz = Math.max(minCz, chunkIndexOf(bounds.maxZ - 1e-6, chunkSizeMetres));
  return { minCx, minCz, maxCx, maxCz };
}

export function worldChunkCount(extentMetres, chunkSizeMetres) {
  const [width, depth] = extentMetres;
  return {
    x: Math.ceil(width / chunkSizeMetres),
    z: Math.ceil(depth / chunkSizeMetres),
    total: Math.ceil(width / chunkSizeMetres) * Math.ceil(depth / chunkSizeMetres),
  };
}

// Vertices per chunk edge for a given sample step. A 128 m chunk at 1 m is 129 vertices:
// the +1 is the shared edge, which is what makes neighbouring chunks meet without a gap.
export function verticesPerAxis(chunkSizeMetres, sampleStepMetres) {
  return Math.round(chunkSizeMetres / sampleStepMetres) + 1;
}

// LOD ring for a chunk centre distance. 0 = near field (full sample step), 1 = mid,
// 2 = far. The renderer and the collision prefetch both read this so they cannot disagree
// about which chunks are allowed to be coarse.
export function lodForDistance(distanceMetres, thresholds = [256, 768]) {
  if (distanceMetres <= thresholds[0]) return 0;
  if (distanceMetres <= thresholds[1]) return 1;
  return 2;
}

export function lodSampleStep(sampleStepMetres, lod) {
  return sampleStepMetres * (1 << Math.max(0, Math.min(3, lod)));
}

// Stable feature id. It is derived from the *source* of a feature, never from its index in
// a list: adding one tree at the front of a list must not renumber every later tree, or
// every hand edit in the world would silently rebind to a different object.
export function featureId(layerId, kind, cellX, cellZ, ordinal) {
  return `${layerId}:${kind}:${cellX}:${cellZ}:${ordinal}`;
}

// Stable object id used by overrides (lock / delete / move). Same reasoning as featureId,
// but this is what the override file stores so it must not change format casually.
export function objectId(layerId, kind, cellX, cellZ, ordinal) {
  return `${layerId}/${kind}/${cellX}/${cellZ}/${ordinal}`;
}

export function parseObjectId(id) {
  const parts = String(id ?? '').split('/');
  if (parts.length !== 5) return null;
  const [layerId, kind, cx, cz, ordinal] = parts;
  if (!layerId || !kind) return null;
  const cellX = Number(cx);
  const cellZ = Number(cz);
  const index = Number(ordinal);
  if (!Number.isInteger(cellX) || !Number.isInteger(cellZ) || !Number.isInteger(index)) return null;
  return { layerId, kind, cellX, cellZ, ordinal: index };
}

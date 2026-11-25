// Public surface of the world authoring contract.
//
// Consumers should import from here rather than from the individual modules: the editor
// service, the AI tool surface, the cook CLI and the tests all need to be looking at the
// same names, and a barrel is the cheapest way to make that true.

export {
  WORLD_ERROR_CODES, WorldError, isWorldError, diagnostic, fail,
} from './errors.mjs';

export {
  RECIPE_SCHEMA, LIBRARY_SCHEMA, LAYER_SCHEMA, OVERRIDES_SCHEMA, MANIFEST_SCHEMA,
  CHUNK_FORMAT, CHUNK_VERSION, GENERATOR_VERSION,
  TERRAIN_PRESETS, BUDGET_PROFILES, LAYER_KINDS, COMPONENT_CATEGORIES, PATH_KINDS, BRUSH_SHAPES,
  BUDGETS, budgetFor,
  validateRecipe, validateLibrary, validateLayer, validateOverrides,
  assertRecipe, assertLibrary, assertLayer, assertOverrides,
  normalizeRecipe, recipeChunkRange, makeRecipe,
} from './schema.mjs';

export {
  DEFAULT_CHUNK_SIZE_METRES, DEFAULT_SAMPLE_STEP_METRES, WORLD_LIMITS,
  chunkIndexOf, chunkKey, parseChunkKey, chunkOrigin, chunkBounds, chunkRangeForBounds,
  worldChunkCount, verticesPerAxis, lodForDistance, lodSampleStep,
  featureId, objectId, parseObjectId,
} from './chunk.mjs';

export {
  canonicalNumber, canonicalize, sha256Hex, sha256Canonical, shortDigest, inputDigest,
  SHA256_PATTERN, SHORT_DIGEST_PATTERN, isSha256, validSeed,
} from './hash.mjs';

export {
  CHUNK_HEADER_BYTES, CHUNK_SEGMENT_BYTES, SEGMENT, SEGMENT_NAMES,
  parseChunkHeader, readChunkSegment, validateManifest, assertManifest, manifestHashOf, manifestHashInput,
  manifestChunkKey, manifestChunkPath, manifestChunkAt, chunkFileName,
} from './manifest.mjs';

export {
  WORLD_LAYOUT, WORLD_WRITABLE_DIRECTORIES, resolveProjectPath, isInside, isWritableWorldPath,
  recipePath, layerPath, overridePath, libraryPath, rulesPath, cookedPath, cachePath, reportPath,
  sanitizeId,
} from './paths.mjs';

export {
  STAMP_SPACING_FRACTION, distance2D, resampleStroke, strokeLength, strokeBounds,
  boundsUnion, boundsIntersect, boundsContains, pointInBounds, pointInPolygon, polygonBounds,
  brushWeight, sampleCatmullRom, distanceToPolyline, distanceToSegment, pathDiagnostics,
} from './brush.mjs';

export {
  DEFAULT_COMPONENTS, defaultLibrary, componentById, componentsByCategory,
  summarizeLibrary, adaptLibraryToRecipe,
} from './library.mjs';

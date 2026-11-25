// World authoring contract tests.
//
// These run without a GPU and without a project: they are the JS half of the world
// contract, and the native half lives in native_world_tests.cpp. Between them, every rule
// the plan calls out — floor-based chunk indices, no placeholder hashes, distance-based
// brush resampling, path confinement, corruption detection — is pinned by a test that
// fails loudly instead of a comment that says it should.

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs/promises';

import {
  CHUNK_HEADER_BYTES, WORLD_LIMITS, WorldError, assertLayer, assertLibrary, assertRecipe,
  budgetFor, canonicalize, chunkIndexOf, chunkRangeForBounds, defaultLibrary, featureId,
  inputDigest, makeRecipe, parseChunkHeader, pathDiagnostics, pointInPolygon, recipeChunkRange,
  resampleStroke, resolveProjectPath, sampleCatmullRom, summarizeLibrary, validateManifest,
  validateRecipe, brushWeight, objectId, parseObjectId, lodForDistance, verticesPerAxis,
} from '../../../engine/world/index.mjs';

test('chunk indices use floor so negative coordinates do not collapse to chunk 0', () => {
  assert.equal(chunkIndexOf(-0.5, 128), -1);
  assert.equal(chunkIndexOf(-128, 128), -1);
  assert.equal(chunkIndexOf(-128.1, 128), -2);
  assert.equal(chunkIndexOf(0, 128), 0);
  assert.equal(chunkIndexOf(127.999, 128), 0);
  assert.equal(chunkIndexOf(128, 128), 1);
});

test('a 256 m world centred on the origin is four chunks, not one', () => {
  const range = chunkRangeForBounds({ minX: -128, minZ: -128, maxX: 128, maxZ: 128 }, 128);
  assert.deepEqual(range, { minCx: -1, minCz: -1, maxCx: 0, maxCz: 0 });
});

test('a bounds edge exactly on a chunk border does not dirty the next chunk', () => {
  const range = chunkRangeForBounds({ minX: 0, minZ: 0, maxX: 128, maxZ: 128 }, 128);
  assert.equal(range.maxCx, 0);
  assert.equal(range.maxCz, 0);
});

test('vertex count shares the edge: 128 m at 1 m is 129 vertices', () => {
  assert.equal(verticesPerAxis(128, 1), 129);
  assert.equal(verticesPerAxis(128, 2), 65);
});

test('LOD bands are monotone', () => {
  assert.equal(lodForDistance(0), 0);
  assert.equal(lodForDistance(255), 0);
  assert.equal(lodForDistance(300), 1);
  assert.equal(lodForDistance(5000), 2);
});

test('a valid recipe passes and an unknown key is refused', () => {
  const recipe = makeRecipe({ worldId: 'valley', extentMetres: [256, 256], seed: '42' });
  assert.deepEqual(validateRecipe(recipe), []);
  const broken = { ...recipe, chunkSizeMeters: 128 };
  assert.ok(validateRecipe(broken).some(message => message.includes('not supported')));
});

test('a placeholder hash never passes validation', () => {
  const recipe = makeRecipe({ worldId: 'valley' });
  const broken = {
    ...recipe,
    libraries: [{ id: 'kit', file: 'assets/world-libraries/kit/library.json', sha256: '<64 hex>' }],
  };
  assert.ok(validateRecipe(broken).some(message => message.includes('sha256')));
});

test('the seed must be a decimal string, not a number', () => {
  const recipe = makeRecipe({ worldId: 'valley' });
  assert.ok(validateRecipe({ ...recipe, seed: 947210 }).length > 0);
  assert.deepEqual(validateRecipe({ ...recipe, seed: '18446744073709551615' }), []);
});

test('world extents outside the contract are refused', () => {
  const recipe = makeRecipe({ worldId: 'valley' });
  assert.ok(validateRecipe({ ...recipe, extentMetres: [4, 4] }).length > 0);
  assert.ok(validateRecipe({ ...recipe, extentMetres: [16384, 16384] }).length > 0);
});

test('recipe chunk range matches the extent and origin', () => {
  const recipe = makeRecipe({ worldId: 'valley', extentMetres: [2048, 2048] });
  const range = recipeChunkRange(recipe);
  assert.equal(range.minCx, -8);
  assert.equal(range.maxCx, 7);
});

test('a layer with an unsupported rule key is refused', () => {
  const layer = { schema: 'nativegpu.world-layer/1', id: 'forest', kind: 'scatter', rules: { species: 'pine', bogus: 1 } };
  assert.throws(() => assertLayer(layer), WorldError);
  assert.doesNotThrow(() => assertLayer({ schema: 'nativegpu.world-layer/1', id: 'forest', kind: 'scatter', rules: { species: 'pine', densityPerHectare: 100 } }));
});

test('the default library is valid and reports stand-in geometry honestly', () => {
  const library = defaultLibrary();
  assert.doesNotThrow(() => assertLibrary(library));
  const summary = summarizeLibrary(library);
  assert.equal(summary.imported, 0);
  assert.equal(summary.standIn, library.components.length);
  assert.ok(summary.total >= 10);
});

test('a component cannot be both an imported asset and procedural geometry', () => {
  const library = defaultLibrary();
  const broken = JSON.parse(JSON.stringify(library));
  broken.components[0].asset = { file: 'assets/x.glb', sha256: 'a'.repeat(64) };
  assert.ok(validateLibraryBroken(broken).some(message => message.includes('not both')));
});

function validateLibraryBroken(library) {
  const errors = [];
  try {
    assertLibrary(library);
  } catch (error) {
    errors.push(...(error.details?.cause ?? [error.message]));
  }
  return errors;
}

test('the input digest is stable under key order and changes with the seed', () => {
  const recipe = makeRecipe({ worldId: 'valley', seed: '42' });
  const reordered = Object.fromEntries(Object.entries(recipe).reverse());
  assert.equal(canonicalize(recipe), canonicalize(reordered));
  const a = inputDigest({ recipe, generatorVersion: 'worldgen-1' });
  const b = inputDigest({ recipe: { ...recipe, seed: '43' }, generatorVersion: 'worldgen-1' });
  assert.notEqual(a, b);
  assert.equal(a.length, 64);
});

test('floats are quantized so a rounding difference does not change the digest', () => {
  assert.equal(canonicalize({ a: 1.0000000001 }), canonicalize({ a: 1.0 }));
  assert.notEqual(canonicalize({ a: 1.001 }), canonicalize({ a: 1.0 }));
});

test('the manifest validator rejects a truncated chunk list entry', () => {
  const manifest = {
    schema: 'nativegpu.world-manifest/1',
    worldId: 'valley',
    generatorVersion: 'worldgen-1',
    inputDigest: 'a'.repeat(64),
    manifestHash: 'b'.repeat(64),
    extentMetres: [256, 256],
    originMetres: [-128, 0, -128],
    chunkSizeMetres: 128,
    sampleStepMetres: 1,
    chunkRange: { minCx: -1, minCz: -1, maxCx: 0, maxCz: 0 },
    chunks: [{
      cx: -1, cz: -1, file: 'chunks/-1_-1.ngrc', sha256: 'c'.repeat(64),
      bytes: 1024, lod: 0, instanceCount: 10, vertexCount: 129,
      bounds: { minX: -128, minZ: -128, maxX: 0, maxZ: 0 },
    }],
    budget: { profile: 'debug', totalInstances: 10, residentChunks: 1, cookMillis: 12 },
  };
  assert.deepEqual(validateManifest(manifest), []);
  const broken = { ...manifest, chunks: [{ ...manifest.chunks[0], file: 'chunks/evil.ngrc' }] };
  assert.ok(validateManifest(broken).some(message => message.includes('chunks/')));
  const missingHash = { ...manifest, chunks: [{ ...manifest.chunks[0], sha256: 'nope' }] };
  assert.ok(validateManifest(missingHash).length > 0);
});

test('the chunk header reader refuses a truncated or mislabelled file', () => {
  // Exactly a header: no segment table, no payload. The reader checks the byte count
  // against the declared payload, so an over-long buffer is a corruption too.
  const buffer = Buffer.alloc(CHUNK_HEADER_BYTES, 0);
  buffer.write('NGRC', 0, 'ascii');
  buffer.writeUInt16LE(1, 4);
  buffer.writeUInt16LE(0, 6);
  buffer.writeInt32LE(-1, 8);
  buffer.writeInt32LE(0, 12);
  buffer.writeUInt32LE(128, 16);
  buffer.writeUInt32LE(100, 20);
  buffer.writeUInt32LE(129, 24);
  buffer.writeUInt32LE(0, 28);
  buffer.writeUInt32LE(0, 32);
  buffer.writeUInt32LE(0, 36);
  buffer.writeUInt32LE(0, 56);
  const header = parseChunkHeader(buffer);
  assert.equal(header.chunkX, -1);
  assert.equal(header.vertexCount, 129);

  assert.throws(() => parseChunkHeader(buffer.subarray(0, 32)), WorldError);
  const wrongMagic = Buffer.from(buffer);
  wrongMagic[0] = 'X'.charCodeAt(0);
  assert.throws(() => parseChunkHeader(wrongMagic), WorldError);
  const wrongVersion = Buffer.from(buffer);
  wrongVersion.writeUInt16LE(9, 4);
  assert.throws(() => parseChunkHeader(wrongVersion), WorldError);
});

test('brush resampling is distance based, not event based', () => {
  const sparse = resampleStroke([[0, 0], [10, 0]], 1);
  const dense = resampleStroke([[0, 0], [5, 0], [10, 0]], 1);
  assert.equal(sparse.length, 11);
  assert.equal(dense.length, 11);
  const short = resampleStroke([[0, 0], [0.5, 0]], 1);
  assert.equal(short.length, 2);
});

test('a closed path spline stays inside its control hull', () => {
  const samples = sampleCatmullRom([[0, 0], [10, 0], [10, 10], [0, 10]], 1, { closed: true });
  assert.ok(samples.length > 30);
  for (const [x, z] of samples) {
    assert.ok(x >= -1.5 && x <= 11.5, `x ${x} escaped the hull`);
    assert.ok(z >= -1.5 && z <= 11.5, `z ${z} escaped the hull`);
  }
});

test('point in polygon handles the concave case', () => {
  const polygon = [[0, 0], [10, 0], [10, 10], [6, 10], [6, 4], [4, 4], [4, 10], [0, 10]];
  assert.equal(pointInPolygon(polygon, 2, 5), true);
  assert.equal(pointInPolygon(polygon, 5, 8), false);
  assert.equal(pointInPolygon(polygon, 5, 2), true);
});

test('brush falloff is 1 in the core and 0 at the rim', () => {
  assert.equal(brushWeight(0, 10, 4), 1);
  assert.equal(brushWeight(6, 10, 4), 1);
  assert.equal(brushWeight(10, 10, 4), 0);
  assert.ok(brushWeight(8, 10, 4) > 0 && brushWeight(8, 10, 4) < 1);
});

test('path diagnostics catch duplicate nodes, hairpins and self-intersection', () => {
  const duplicated = pathDiagnostics({ id: 'p', points: [[0, 0], [0, 0], [5, 0]] });
  assert.ok(duplicated.some(entry => entry.code === 'PATH_DUPLICATE_POINT'));
  const hairpin = pathDiagnostics({ id: 'p', points: [[0, 0], [10, 0], [0, 1]] });
  assert.ok(hairpin.some(entry => entry.code === 'PATH_SHARP_TURN'));
  const crossing = pathDiagnostics({ id: 'p', points: [[0, 0], [10, 10], [10, 0], [0, 10]] });
  assert.ok(crossing.some(entry => entry.code === 'PATH_SELF_INTERSECTS'));
});

test('stable object ids round trip and reject malformed input', () => {
  const id = objectId('forest', 'pine', -3, 7, 12);
  assert.equal(id, 'forest/pine/-3/7/12');
  assert.deepEqual(parseObjectId(id), { layerId: 'forest', kind: 'pine', cellX: -3, cellZ: 7, ordinal: 12 });
  assert.equal(parseObjectId('forest/pine'), null);
  assert.notEqual(featureId('a', 'pine', 1, 1, 1), featureId('a', 'pine', 1, 1, 2));
});

test('project paths refuse traversal and absolute paths', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'ng-world-'));
  try {
    await assert.doesNotReject(() => resolveProjectPath(root, '.screen/world/main.world.json'));
    await assert.rejects(() => resolveProjectPath(root, '../escape.json'), WorldError);
    await assert.rejects(() => resolveProjectPath(root, 'C:/Windows/system32/x.json'), WorldError);
    await assert.rejects(() => resolveProjectPath(root, '/etc/passwd'), WorldError);
    await assert.rejects(() => resolveProjectPath(root, 'a/../../b.json'), WorldError);
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('the manifest hash covers content only, so a recook keeps the same address', async () => {
  const { manifestHashOf } = await import('../../../engine/world/manifest.mjs');
  const base = {
    schema: 'nativegpu.world-manifest/1',
    worldId: 'valley',
    generatorVersion: 'worldgen-1',
    inputDigest: 'a'.repeat(64),
    manifestHash: '',
    extentMetres: [256, 256],
    originMetres: [-128, 0, -128],
    chunkSizeMetres: 128,
    sampleStepMetres: 1,
    chunkRange: { minCx: -1, minCz: -1, maxCx: 0, maxCz: 0 },
    chunks: [],
    budget: { profile: 'debug', totalInstances: 10, residentChunks: 0, cookMillis: 12 },
    stats: { vertices: 1 },
  };
  const first = manifestHashOf(base);
  const recooked = { ...base, savedAt: new Date().toISOString(), budget: { ...base.budget, cookMillis: 987 }, stats: { vertices: 999 } };
  assert.equal(manifestHashOf(recooked), first);
  const changed = { ...base, chunks: [{ cx: 0, cz: 0 }] };
  assert.notEqual(manifestHashOf(changed), first);
});

test('budget profiles are declared and bounded', () => {
  for (const profile of ['debug', 'desktop-balanced', 'desktop-quality', 'stress']) {
    const budget = budgetFor(profile);
    assert.ok(budget.instancesPerChunk > 0);
    assert.ok(budget.totalInstances >= budget.instancesPerChunk);
  }
  assert.equal(budgetFor('unknown'), budgetFor('desktop-balanced'));
});

test('world limits are the ones the native compiler enforces', () => {
  assert.deepEqual(WORLD_LIMITS.extentMetres, [16, 8192]);
  assert.deepEqual(WORLD_LIMITS.chunkSizeMetres, [16, 512]);
  assert.equal(WORLD_LIMITS.chunksTotal, 262144);
});

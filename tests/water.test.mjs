import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { compileWater, compileWaterLibrary, WaterError } from '../engine/water/asset.mjs';
import { cookWaterLibrary } from '../tools/water.mjs';

const root = path.resolve(import.meta.dirname, '..');
const fixture = async (name = 'pond_still') =>
  JSON.parse(await fs.readFile(path.join(root, `assets/water/${name}.water.json`), 'utf8'));

test('water compile derives the solver grid and keeps a stable identity', async () => {
  const asset = await fixture('river_shallow'), before = structuredClone(asset);
  const compiled = compileWater(asset);
  assert.deepEqual(asset, before, 'compile mutated the authored source');
  assert.equal(compiled.columns, 240);
  assert.equal(compiled.rows, 80);
  assert.equal(compiled.cells, 19200);
  assert.equal(compiled.payload.source.derived.extentMetres[0], 24);
  assert.equal(compiled.payload.source.derived.extentMetres[1], 8);
  assert.equal(compiled.fft, false, 'the spectrum backend must not claim an FFT');
  assert.equal(compiled.hash, compileWater(Object.fromEntries(Object.entries(asset).reverse())).hash);
});

test('water assets reject malformed grids, budgets, wave directions and unimplemented backends', async () => {
  const invalid = [
    (a) => { a.unknown = true; },
    (a) => { a.extentMetres = [0.1, 10]; },
    (a) => { a.cellMetres = 4; },
    (a) => { a.budget.maxCells = 100; },
    (a) => { a.waves.direction = [0, 0]; },
    (a) => { a.boundaries.inflowCubicMetresPerSecond = -1; },
    (a) => { a.bathymetry = { type: 'noise', heightMetres: 0 }; },
    (a) => { a.material.refractionIndex = 0.5; },
  ];
  for (const mutate of invalid) {
    const asset = await fixture(); mutate(asset);
    assert.throws(() => compileWater(asset), undefined, mutate.toString());
  }
  const pbf = await fixture(); pbf.backend = 'pbf';
  assert.throws(() => compileWater(pbf), (error) =>
    error instanceof WaterError && error.code === 'NG-E-WATER-UNSUPPORTED');
  const unknown = await fixture(); unknown.backend = 'raytraced';
  assert.throws(() => compileWater(unknown), /not declared/);
});

test('water assets accept local flow, channel terrain and obstacles with strict validation', async () => {
  const base = await fixture();
  assert.equal(Object.hasOwn(compileWater(base).payload.source, 'sources'), false,
    'absent optional fields stay absent so existing cooked identities do not move');
  const flowing = await fixture();
  flowing.sources = [{ x: -3, z: 0, radiusMetres: 0.6, flowCubicMetresPerSecond: 0.02, speedMetresPerSecond: 0.5, direction: [1, 0] }];
  flowing.drains = [{ x: 3, z: 0, radiusMetres: 0.6, flowCubicMetresPerSecond: 0.02 }];
  flowing.obstacles = [{ x: 0, z: 0, radiusMetres: 0.5, topHeightMetres: 0.6 }];
  const compiled = compileWater(flowing);
  assert.equal(compiled.payload.source.sources[0].flowCubicMetresPerSecond, 0.02);
  assert.equal(compiled.payload.source.drains[0].radiusMetres, 0.6);
  assert.equal(compiled.payload.source.obstacles[0].topHeightMetres, 0.6);
  for (const bathymetry of [
    { type: 'channel', heightMetres: -0.3, widthMetres: 4, depthMetres: 0.2, bankSlope: 1 },
    { type: 'mounds', heightMetres: 0, mounds: [{ x: 0, z: 0, radiusMetres: 1, heightMetres: 0.2 }] },
  ]) {
    const asset = await fixture(); asset.bathymetry = bathymetry;
    assert.equal(compileWater(asset).payload.source.bathymetry.type, bathymetry.type);
  }
  const invalid = [
    (a) => { a.sources = [{ x: 0, z: 0, radiusMetres: 0.5, flowCubicMetresPerSecond: -1 }]; },
    (a) => { a.sources = [{ x: 0, z: 0, radiusMetres: 0.5, flowCubicMetresPerSecond: 1, direction: [0, 0] }]; },
    (a) => { a.drains = [{ x: 0, z: 0, radiusMetres: 0.5, flowCubicMetresPerSecond: 1, unknown: 1 }]; },
    (a) => { a.obstacles = [{ x: 0, z: 0, radiusMetres: 0.5, topHeightMetres: 500 }]; },
    (a) => { a.obstacles = [{ x: 0, z: 0, radiusMetres: 0 }]; },
    (a) => { a.bathymetry = { type: 'channel', heightMetres: 0, widthMetres: 0, depthMetres: 0.2 }; },
    (a) => { a.bathymetry = { type: 'mounds', heightMetres: 0, mounds: [] }; },
  ];
  for (const mutate of invalid) {
    const asset = await fixture(); mutate(asset);
    assert.throws(() => compileWater(asset), undefined, mutate.toString());
  }
});

test('every authored water asset compiles', async () => {
  const directory = path.join(root, 'assets/water');
  const names = (await fs.readdir(directory)).filter((name) => name.endsWith('.water.json'));
  assert.ok(names.length >= 4);
  for (const name of names) {
    const asset = JSON.parse(await fs.readFile(path.join(directory, name), 'utf8'));
    const compiled = compileWater(asset);
    assert.equal(compiled.source.id, asset.id);
    assert.ok(compiled.columns >= 4 && compiled.rows >= 4, name);
  }
});

test('water library cooking is deterministic and content addressed', async () => {
  const directory = await fs.mkdtemp(path.join(tmpdir(), 'ng-water-'));
  try {
    const source = path.join(directory, 'source'), out = path.join(directory, 'cooked');
    await fs.mkdir(source);
    for (const name of ['pond_still', 'coast_spectrum'])
      await fs.writeFile(path.join(source, `${name}.water.json`), JSON.stringify(await fixture(name)));
    const first = await cookWaterLibrary({ sourceDirectory: source, outputDirectory: out });
    const second = await cookWaterLibrary({ sourceDirectory: source, outputDirectory: out });
    assert.equal(first.packageHash, second.packageHash);
    assert.equal(first.runtimeIntegrated, false);
    assert.equal(first.bodies.length, 2);
    for (const entry of first.bodies) {
      const container = JSON.parse(await fs.readFile(path.join(out, entry.file), 'utf8'));
      assert.equal(container.schema, 'nativegpu.water-cook/1');
      assert.equal(container.hash, entry.hash);
      const payload = JSON.parse(container.payloadJson);
      assert.equal(payload.source.id, entry.id);
      assert.ok(payload.source.derived.totalCells >= 16);
    }
    const published = await fs.readFile(path.join(out, 'library.json'), 'utf8');
    const bad = await fixture(); bad.backend = 'pbf';
    await fs.writeFile(path.join(source, 'invalid.water.json'), JSON.stringify(bad));
    await assert.rejects(cookWaterLibrary({ sourceDirectory: source, outputDirectory: out }));
    assert.equal(await fs.readFile(path.join(out, 'library.json'), 'utf8'), published,
      'a rejected backend replaced the published library');
  } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test('water library rejects duplicate ids', async () => {
  const asset = await fixture();
  assert.throws(() => compileWaterLibrary([asset, structuredClone(asset)]), /duplicate water id/);
  assert.throws(() => compileWaterLibrary([]));
});

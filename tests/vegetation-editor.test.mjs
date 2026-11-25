import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createVegetationService } from '../editor/vegetation-service.mjs';
import { writeVegetationFixtures } from '../tools/vegetation-fixtures.mjs';

// Incremental coverage for the editor's server half. These tests never write: saving and
// baking are exercised against the real project, so a test that wrote would mutate content
// every run. The write path is verified once by hand (see .developmentAgent/vegetation/VEGETATION-EDITOR.md).

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const service = createVegetationService({ root });
const ATLAS = { columns: 3, rows: 4 };

const bytesOf = (base64) => Buffer.from(base64, 'base64').length;

test('editor service lists the species the cook can see', async () => {
  const { ok, result } = await service.call('vegetation.list', {});
  assert.equal(ok, true, JSON.stringify(result));
  const ids = result.species.map((entry) => entry.id);
  for (const id of ['oak_broadleaf', 'pine_conifer', 'hazel_shrub']) assert.ok(ids.includes(id), `missing ${id}`);
  // Every entry must say whether it came from the editor or from a build fixture: the tree
  // panel labels them differently and "where did this file come from" is not guessable.
  for (const entry of result.species) assert.equal(typeof entry.authored, 'boolean');
});

test('load hands the editor a complete generator graph for a legacy source', async () => {
  const { ok, result } = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  assert.equal(ok, true, JSON.stringify(result));
  const generators = result.spec.generators;
  assert.equal(generators.levels[0].type, 'trunk');
  assert.ok(generators.levels.length >= 3, 'a broadleaf needs several branch levels to edit');
  // Every frond generator is real, not just the first one. The old builder consumed
  // `fronds[0]` and silently dropped the rest, so "add a second foliage layer" did nothing;
  // the assertion is therefore per frond and checks that each declares its own host levels.
  assert.ok(generators.fronds.length >= 1, 'a broadleaf needs at least one frond');
  for (const frond of generators.fronds) {
    assert.ok(frond.parentLevel >= 1 && frond.parentLevel < generators.levels.length);
    assert.ok(frond.cardsPerSegment > 0);
    assert.ok(Array.isArray(frond.hostLevels) && frond.hostLevels.length > 0);
    for (const level of frond.hostLevels) {
      assert.ok(level >= 1 && level < generators.levels.length, `frond ${frond.id} hosts a nonexistent level ${level}`);
    }
    assert.ok(frond.petioleLengthMetres >= 0, 'a frond must state its petiole length in metres');
  }
  // Two fronds that claim the same host level would produce the same tree from different
  // sliders, which is the defect this test exists to catch.
  const hosts = generators.fronds.map((frond) => frond.hostLevels.join(','));
  assert.equal(new Set(hosts).size, hosts.length, `fronds share a host selector: ${hosts}`);
});

test('every frond generator contributes leaves, and dropping one keeps the others', async () => {
  const load = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  const spec = structuredClone(load.result.spec);
  assert.ok(spec.generators.fronds.length >= 2, 'this test needs a species with more than one frond');
  const before = await service.call('vegetation.preview', { spec, level: 0, leafAtlas: ATLAS });
  assert.equal(before.ok, true, JSON.stringify(before.result ?? before.error));
  const counts = Object.fromEntries(before.result.structure.fronds.map((entry) => [entry.id, entry.leaves]));
  for (const frond of spec.generators.fronds) {
    assert.ok((counts[frond.id] ?? 0) > 0, `frond ${frond.id} produced no leaves: ${JSON.stringify(counts)}`);
  }
  // Removing one frond must remove exactly its own foliage and leave the other's host count
  // intact — that is what "each frond acts only on its declared hosts" means in numbers.
  const removed = spec.generators.fronds[0];
  spec.generators.fronds = spec.generators.fronds.slice(1);
  const after = await service.call('vegetation.preview', { spec, level: 0, leafAtlas: ATLAS });
  assert.equal(after.ok, true, JSON.stringify(after.result ?? after.error));
  const remaining = Object.fromEntries(after.result.structure.fronds.map((entry) => [entry.id, entry.leaves]));
  assert.equal(remaining[removed.id] ?? 0, 0, `frond ${removed.id} still produced leaves after removal`);
  for (const frond of spec.generators.fronds) {
    assert.equal(remaining[frond.id], counts[frond.id], `frond ${frond.id} changed when a different frond was removed`);
  }
});

test('save is revision-checked, atomic, and undo restores the exact bytes', async () => {
  // A throw-away project root: the write path must not touch the user's authored species.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'veg-forge-'));
  try {
    await writeVegetationFixtures(path.join(root, 'build/vegetation/source'));
    const sandbox = createVegetationService({ root });
    const loaded = await sandbox.call('vegetation.load', { id: 'oak_broadleaf' });
    assert.equal(loaded.ok, true, JSON.stringify(loaded));
    const first = loaded.result.revision;
    // The revision is content-addressed, so it is a full digest and not a counter.
    assert.match(first, /^[0-9a-f]{64}$/);

    const spec = structuredClone(loaded.result.spec);
    spec.generators.levels[1].count += 1;
    const saved = await sandbox.call('vegetation.save', { spec, expectedRevision: first });
    assert.equal(saved.ok, true, JSON.stringify(saved));
    assert.notEqual(saved.result.revision, first);

    // A second writer that still believes it is at `first` must be refused, not silently win.
    const stale = await sandbox.call('vegetation.save', { spec, expectedRevision: first });
    assert.equal(stale.ok, false);
    assert.equal(stale.error.code, 'REVISION_CONFLICT');
    assert.equal(stale.error.details[0].currentRevision, saved.result.revision);

    // No temporary file may be left behind, or the next cook sees two candidate sources.
    const directory = path.join(root, 'assets/vegetation/species');
    const leftovers = (await fs.readdir(directory)).filter((name) => name.endsWith('.tmp'));
    assert.deepEqual(leftovers, []);

    // Undo restores the previous document byte for byte, which is also the previous revision.
    const undo = await sandbox.call('vegetation.undo', { id: 'oak_broadleaf' });
    assert.equal(undo.ok, true, JSON.stringify(undo));
    assert.equal(undo.result.revision, first);
    const reloaded = await sandbox.call('vegetation.load', { id: 'oak_broadleaf' });
    assert.equal(reloaded.result.revision, first, 'undo did not restore the same content revision');
    assert.equal(reloaded.result.spec.generators.levels[1].count, spec.generators.levels[1].count - 1);

    const redo = await sandbox.call('vegetation.redo', { id: 'oak_broadleaf' });
    assert.equal(redo.ok, true, JSON.stringify(redo));
    assert.equal(redo.result.revision, saved.result.revision);

    // Nothing to redo a second time: an empty stack is a coded refusal, not a no-op success.
    const again = await sandbox.call('vegetation.redo', { id: 'oak_broadleaf' });
    assert.equal(again.ok, false);
    assert.equal(again.error.code, 'NOTHING_TO_RESTORE');
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('load hands the editor the cooked texture roles instead of a placeholder', async () => {
  const { ok, result } = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  assert.equal(ok, true, JSON.stringify(result));
  // No cooked texture library in this checkout means the field is null, not a fabricated table.
  if (result.textureRoles === null) return;
  assert.ok(Array.isArray(result.textureRoles.roles) && result.textureRoles.roles.length > 0);
  for (const role of result.textureRoles.roles) {
    assert.equal(typeof role.digest, 'string');
    assert.ok(role.semantic, `${role.role} has no semantic`);
    assert.ok(role.colorSpace, `${role.role} has no colour space`);
  }
  const bark = result.spec.textures?.bark;
  assert.ok(bark && bark.baseColor, 'the species must declare which role fills each slot');
});

test('a second bake of an unchanged document is served from the cache, and a change invalidates it', async () => {
  // A temp root so the real content library is never touched. Grass is used because it needs no
  // texture library: the cache must be provable without cooking textures first.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'veg-cache-'));
  try {
    await writeVegetationFixtures(path.join(root, 'build/vegetation/source'));
    const sandbox = createVegetationService({ root });
    const loaded = await sandbox.call('vegetation.load', { id: 'meadow_grass' });
    assert.equal(loaded.ok, true, JSON.stringify(loaded));
    const spec = loaded.result.spec;

    const first = await sandbox.call('vegetation.cook', { spec });
    assert.equal(first.ok, true, JSON.stringify(first));
    assert.equal(first.result.cached, false, 'the first bake must not claim a cache hit');
    assert.equal(typeof first.result.fingerprint, 'string');
    assert.match(first.result.fingerprint, /^[0-9a-f]{32}$/);

    // Nothing changed: same source, same textures (none), same cook code.
    const second = await sandbox.call('vegetation.cook', { spec });
    assert.equal(second.ok, true, JSON.stringify(second));
    assert.equal(second.result.cached, true, 'an unchanged document re-cooked instead of using the cache');
    assert.equal(second.result.hash, first.result.hash);

    // One authored number changes: the cached bytes no longer describe this document.
    const edited = structuredClone(spec);
    edited.dimensions.heightMetres = spec.dimensions.heightMetres * 1.3;
    const third = await sandbox.call('vegetation.cook', { spec: edited });
    assert.equal(third.ok, true, JSON.stringify(third));
    assert.equal(third.result.cached, false, 'the cache survived a source change');
    assert.notEqual(third.result.fingerprint, first.result.fingerprint);

    // `force` is the explicit way to re-bake without touching the document.
    const forced = await sandbox.call('vegetation.cook', { spec, force: true });
    assert.equal(forced.ok, true);
    assert.equal(forced.result.cached, false);
    assert.equal(forced.result.hash, first.result.hash, 'a forced re-bake produced different bytes');
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('preview geometry agrees with its own counts and fits the budget', async () => {
  const load = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  const { ok, result } = await service.call('vegetation.preview', {
    spec: load.result.spec, level: 1, leafAtlas: ATLAS,
  });
  assert.equal(ok, true, JSON.stringify(result));
  const positions = bytesOf(result.geometry.positions) / 4;
  const normals = bytesOf(result.geometry.normals) / 4;
  const uvs = bytesOf(result.geometry.uvs) / 4;
  const wind = bytesOf(result.geometry.windWeights) / 4;
  const colors = bytesOf(result.geometry.colors) / 4;
  const indices = bytesOf(result.geometry.indices) / 4;
  assert.equal(positions, result.vertices * 3);
  assert.equal(normals, result.vertices * 3);
  assert.equal(uvs, result.vertices * 2);
  assert.equal(wind, result.vertices);
  // Colour is Uint8x4 per vertex, so its byte length is already four values per vertex.
  assert.equal(bytesOf(result.geometry.colors), result.vertices * 4);
  assert.equal(indices, result.triangles * 3);
  assert.equal(result.overBudget, false);
  assert.ok(result.triangles > 1000);
});

test('changing a generator changes the geometry it claims to drive', async () => {
  const load = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  const spec = load.result.spec;
  const before = await service.call('vegetation.preview', { spec, level: 1, leafAtlas: ATLAS });
  const level = spec.generators.levels[1];
  const witness = level.count;
  level.count = witness + 2;
  const after = await service.call('vegetation.preview', { spec, level: 1, leafAtlas: ATLAS });
  level.count = witness;
  assert.equal(before.ok && after.ok, true);
  assert.notEqual(after.result.triangles, before.result.triangles,
    'branch count did not reach the builder');
  // Wind weights are baked per vertex. Dropping every level plus the frond must drop the peak
  // in the vertex buffer, otherwise the per-level wind sliders are decoration.
  const peak = (preview) => {
    const values = new Float32Array(Buffer.from(preview.result.geometry.windWeights, 'base64').buffer.slice(0));
    let max = 0;
    for (const value of values) max = Math.max(max, value);
    return max;
  };
  const windPeakBefore = peak(before);
  assert.ok(windPeakBefore > 0.9, `the fixture should carry a near-1 wind weight, got ${windPeakBefore}`);
  for (const level of spec.generators.levels) level.wind = 0.05;
  for (const frond of spec.generators.fronds) frond.wind = 0.05;
  const quiet = await service.call('vegetation.preview', { spec, level: 1, leafAtlas: ATLAS });
  assert.ok(peak(quiet) < 0.2, `per-level wind weights did not reach the vertex buffer: ${peak(quiet)}`);
});

test('analysis reports one LOD row per declared level with rising error', async () => {
  const load = await service.call('vegetation.load', { id: 'pine_conifer' });
  const { ok, result } = await service.call('vegetation.preview', {
    spec: load.result.spec, level: 0, analysis: true, leafAtlas: ATLAS,
  });
  assert.equal(ok, true, JSON.stringify(result));
  assert.equal(result.lodTable.length, load.result.spec.lod.levels);
  assert.equal(result.lodTable[0].worldErrorMetres, 0);
  for (let index = 1; index < result.lodTable.length; index++) {
    assert.ok(result.lodTable[index].worldErrorMetres > 0, `lod${index} error must be measured`);
    assert.ok(result.lodTable[index].triangles <= result.lodTable[index - 1].triangles, `lod${index} grew`);
  }
});

test('an impossible generator graph is refused with a message, not a stack trace', async () => {
  const load = await service.call('vegetation.load', { id: 'oak_broadleaf' });
  const spec = structuredClone(load.result.spec);
  spec.generators.levels[1].count = 400;
  const { ok, error } = await service.call('vegetation.preview', { spec, level: 0, leafAtlas: ATLAS });
  // 400 is clamped to 16 by the normalizer, so this one still builds; the unbounded case is
  // a graph that is structurally wrong, which must be a coded refusal.
  assert.equal(ok, true);
  spec.generators.levels = [{ type: 'branch', level: 2, count: 2 }];
  const broken = await service.call('vegetation.preview', { spec, level: 0, leafAtlas: ATLAS });
  assert.equal(broken.ok, false);
  assert.equal(broken.error.code, 'NG-E-VEG-SPECIES');
  assert.match(broken.error.message, /trunk/);
});

test('publishing a region turns baked species into a package the runtime can load', async () => {
  // Temp root: publishing writes a package, a source and a manifest entry, so it must never run
  // against the real content library. Grass is used because it bakes without a texture library.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'veg-publish-'));
  try {
    await writeVegetationFixtures(path.join(root, 'build/vegetation/source'));
    const sandbox = createVegetationService({ root });
    const region = {
      schema: 'nativegpu.vegetation-region/1', id: 'editor_meadow', seed: 7,
      bounds: { minX: -16, minZ: -16, maxX: 16, maxZ: 16 },
      cellSizeMetres: 8, pageCellMetres: 16, pageSize: 256,
      surface: { kind: 'plane', heightMetres: 0, version: 'test-plane/1' },
      species: [{ species: 'meadow_grass', densityPerHectare: 400, minSpacingMetres: 0.4, scaleRange: [0.9, 1.1] }],
      budget: { maxInstances: 20000 },
    };

    // Nothing is baked yet, so the region would reference packages that do not exist. This must be
    // a coded refusal rather than an empty page set that looks like a successful publish.
    const tooEarly = await sandbox.call('vegetation.publish', { region });
    assert.equal(tooEarly.ok, false);
    assert.equal(tooEarly.error.code, 'NG-E-VEG-SPECIES');

    const loaded = await sandbox.call('vegetation.load', { id: 'meadow_grass' });
    assert.equal(loaded.ok, true, JSON.stringify(loaded));
    const baked = await sandbox.call('vegetation.cook', { spec: loaded.result.spec });
    assert.equal(baked.ok, true, JSON.stringify(baked));

    const first = await sandbox.call('vegetation.publish', { region });
    assert.equal(first.ok, true, JSON.stringify(first));
    assert.equal(first.result.id, 'editor_meadow');
    assert.match(first.result.hash, /^[0-9a-f]{64}$/);
    assert.ok(first.result.instances > 0, 'the published region scattered no instances');
    assert.ok(first.result.pages >= 1, 'a populated region produced no pages');
    assert.equal(first.result.file, `regions/${first.result.hash}.ngvrg`);

    // The package sits on disk under the hash it claims, and the manifest points at it.
    const cooked = path.join(root, 'build/vegetation/cooked');
    const bytes = await fs.readFile(path.join(cooked, first.result.file));
    assert.equal(bytes.byteLength, first.result.totalBytes);
    const library = JSON.parse(await fs.readFile(path.join(cooked, 'library.json'), 'utf8'));
    assert.equal(library.regions.length, 1);
    assert.equal(library.regions[0].id, 'editor_meadow');
    assert.equal(library.regions[0].hash, first.result.hash);
    // Publishing must not disturb the species entries the Bake button owns.
    assert.deepEqual(library.species.map((entry) => entry.id), [baked.result.id]);

    // The authored source is written back, so the region can be reopened for editing.
    const source = JSON.parse(await fs.readFile(
      path.join(root, 'assets/vegetation/regions/editor_meadow.region.json'), 'utf8'));
    assert.equal(source.seed, region.seed);
    assert.equal(source.species[0].species, 'meadow_grass');
    assert.equal(source.bounds.maxX, region.bounds.maxX);

    // Same document, same seed: the compile is deterministic, so the hash does not move and the
    // region is replaced rather than appended.
    const again = await sandbox.call('vegetation.publish', { region });
    assert.equal(again.ok, true, JSON.stringify(again));
    assert.equal(again.result.hash, first.result.hash, 'republishing the same region changed its bytes');
    assert.equal(again.result.regions, 1, 'republishing duplicated the region entry');

    // A stale expectedRevision is a conflict, not a silent overwrite of another author's publish.
    const stale = await sandbox.call('vegetation.publish', { region, expectedRevision: 'f'.repeat(64) });
    assert.equal(stale.ok, false);
    assert.equal(stale.error.code, 'NG-E-VEG-CONFLICT');
    const fresh = await sandbox.call('vegetation.publish', { region, expectedRevision: first.result.hash });
    assert.equal(fresh.ok, true, JSON.stringify(fresh));

    // A region naming a species that was never baked is refused before anything is written.
    const unknown = await sandbox.call('vegetation.publish', {
      region: { ...region, id: 'editor_unknown', species: [{ ...region.species[0], species: 'never_baked' }] },
    });
    assert.equal(unknown.ok, false);
    const after = JSON.parse(await fs.readFile(path.join(cooked, 'library.json'), 'utf8'));
    assert.equal(after.regions.length, 1, 'a refused publish still wrote a manifest entry');
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('a scene description maps to the region the panel actually asked for', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'veg-scene-'));
  try {
    await writeVegetationFixtures(path.join(root, 'build/vegetation/source'));
    const sandbox = createVegetationService({ root });
    const loaded = await sandbox.call('vegetation.load', { id: 'meadow_grass' });
    const baked = await sandbox.call('vegetation.cook', { spec: loaded.result.spec });
    assert.equal(baked.ok, true, JSON.stringify(baked));

    // Only the numbers a person sets: extent, density, spacing. The schema plumbing must be
    // filled in by the service, not by the panel.
    const scene = {
      id: 'editor_meadow', seed: 3, halfExtentMetres: 10,
      species: [{ species: 'meadow_grass', densityPerHectare: 200, minSpacingMetres: 0.5 }],
    };
    const first = await sandbox.call('vegetation.publish', { scene });
    assert.equal(first.ok, true, JSON.stringify(first));
    assert.ok(first.result.instances > 0, 'the scene scattered nothing');

    const source = JSON.parse(await fs.readFile(
      path.join(root, 'assets/vegetation/regions/editor_meadow.region.json'), 'utf8'));
    assert.equal(source.bounds.minX, -10);
    assert.equal(source.bounds.maxX, 10);
    assert.equal(source.bounds.minZ, -10);
    assert.equal(source.seed, 3);
    assert.equal(source.schema, 'nativegpu.vegetation-region/1');
    assert.equal(source.surface.kind, 'plane');
    assert.equal(source.cellSizeMetres, 8);
    assert.equal(source.pageCellMetres, 16);
    assert.equal(source.pageSize, 256);
    assert.equal(source.budget.maxInstances, 20000);
    assert.equal(source.species[0].densityPerHectare, 200);
    assert.equal(source.species[0].minSpacingMetres, 0.5);
    // A rule that did not state a scale range gets the documented default, not undefined.
    assert.deepEqual(source.species[0].scaleRange, [0.85, 1.15]);

    // Density is the control that matters: four times the density must scatter clearly more.
    const dense = await sandbox.call('vegetation.publish', {
      scene: { ...scene, species: [{ ...scene.species[0], densityPerHectare: 800 }] },
    });
    assert.equal(dense.ok, true, JSON.stringify(dense));
    assert.ok(dense.result.instances > first.result.instances * 2,
      `density 4x produced ${dense.result.instances} vs ${first.result.instances} instances`);

    // Every out-of-range number is refused and names the field, so the panel can point at it.
    const refusals = [
      [{ halfExtentMetres: 1 }, 'halfExtentMetres'],
      [{ seed: -1 }, 'seed'],
      [{ species: [] }, 'scene.species'],
      [{ species: [{ species: 'meadow_grass', densityPerHectare: 0 }] }, 'densityPerHectare'],
      [{ species: [{ species: 'meadow_grass', densityPerHectare: 100, minSpacingMetres: 0 }] }, 'minSpacingMetres'],
      [{ species: [{ species: 'meadow_grass', densityPerHectare: 100, scaleRange: [1.2, 0.8] }] }, 'scaleRange'],
      [{ id: 'Not A Valid Id' }, 'scene.id'],
    ];
    for (const [patch, field] of refusals) {
      const bad = await sandbox.call('vegetation.publish', { scene: { ...scene, ...patch } });
      assert.equal(bad.ok, false, `${field} was accepted`);
      assert.equal(bad.error.code, 'NG-E-VEG-SCATTER');
      assert.match(bad.error.message, new RegExp(field.replace('.', '\\.')), `${field}: ${bad.error.message}`);
    }
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

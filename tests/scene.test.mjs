import test from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {
  SceneError, TOOL_VERSION, validateScene, stableJSON, sceneHash, queryScene,
  previewTransaction, applyTransaction, saveScene, loadScene, commitScene,
  undoSceneFile, recoverScene, buildScene,
} from '../engine/scene/index.mjs';

const PROJECT = 'a1e00000-0000-4000-8000-000000000001';
const PLAYER = 'a1e00000-0000-4000-8000-000000000002';
const GROUND = 'a1e00000-0000-4000-8000-000000000003';
const PROP = 'a1e00000-0000-4000-8000-000000000004';
const KEY = 'a1e00000-0000-4000-8000-000000000005';
const DOOR = 'a1e00000-0000-4000-8000-000000000006';
const GOAL = 'a1e00000-0000-4000-8000-000000000007';

function entity(id, name, kind, position, scale, color = [0.5, 0.5, 0.5], collider = false) {
  return { id, name, kind, position, scale, color, collider };
}

function makeScene(overrides = {}) {
  return {
    schemaVersion: 1,
    projectId: PROJECT,
    name: 'Test courtyard',
    revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities: [
      entity(PLAYER, 'Player spawn', 'player', [0, 0, 5], [0.6, 1.8, 0.6], [0.2, 0.8, 0.95], true),
      entity(GROUND, 'Ground', 'ground', [0, -0.2, 0], [20, 0.4, 20], [0.23, 0.29, 0.31], true),
      entity(PROP, 'Crate', 'prop', [2, 0.6, 2], [1.2, 1.2, 1.2], [0.72, 0.45, 0.22], true),
      entity(KEY, 'Key', 'key', [4, 0.8, 1], [0.4, 0.4, 0.4], [1, 0.8, 0.2]),
      entity(DOOR, 'Door', 'door', [0, 1.5, -8], [2, 3, 0.4], [0.8, 0.35, 0.2], true),
      entity(GOAL, 'Goal', 'goal', [0, 0, -9], [1.5, 0.1, 1.5], [0.3, 1, 0.6]),
    ],
    ...overrides,
  };
}

function transaction(scene, commands) {
  return { projectId: scene.projectId, expectedRevision: scene.revision, commands };
}

function throwsCode(fn, code) {
  assert.throws(fn, error => {
    assert.ok(error instanceof SceneError, `expected SceneError, got ${error}`);
    assert.equal(error.code, code);
    return true;
  });
}

async function rejectsCode(promise, code) {
  await assert.rejects(promise, error => {
    assert.ok(error instanceof SceneError, `expected SceneError, got ${error}`);
    assert.equal(error.code, code);
    return true;
  });
}

async function temporaryProject(t) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-scene-test-'));
  t.after(() => fs.rm(directory, { recursive: true, force: true }));
  return { directory, file: path.join(directory, 'courtyard.scene.json') };
}

test('scene validation accepts the canonical scene and rejects contract violations', () => {
  const scene = makeScene();
  assert.deepEqual(validateScene(scene), { valid: true, errors: [] });

  const noPlayer = makeScene();
  noPlayer.entities = noPlayer.entities.filter(item => item.kind !== 'player');
  assert.equal(validateScene(noPlayer).valid, false);
  assert.match(validateScene(noPlayer).errors.join('\n'), /exactly one player/);

  const twoPlayers = makeScene();
  twoPlayers.entities.push(entity(randomUUID(), 'Second player', 'player', [1, 0, 1], [1, 1, 1]));
  assert.equal(validateScene(twoPlayers).valid, false);

  const noGround = makeScene();
  noGround.entities = noGround.entities.filter(item => item.kind !== 'ground');
  assert.equal(validateScene(noGround).valid, false);
  assert.match(validateScene(noGround).errors.join('\n'), /at least one ground/);

  const duplicate = makeScene();
  duplicate.entities[2].id = duplicate.entities[1].id.toUpperCase();
  assert.equal(validateScene(duplicate).valid, false);
  assert.match(validateScene(duplicate).errors.join('\n'), /duplicated/);

  const unknownTopLevel = makeScene({ extra: true });
  assert.equal(validateScene(unknownTopLevel).valid, false);
  assert.match(validateScene(unknownTopLevel).errors.join('\n'), /not supported/);

  const unknownEntityField = makeScene();
  unknownEntityField.entities[0].script = 'evil.js';
  assert.equal(validateScene(unknownEntityField).valid, false);

  const badName = makeScene({ name: 'x'.repeat(129) });
  assert.equal(validateScene(badName).valid, false);
  const controlName = makeScene({ name: 'bad\nname' });
  assert.equal(validateScene(controlName).valid, false);
  const blankName = makeScene({ name: '   ' });
  assert.equal(validateScene(blankName).valid, false);

  const lit = makeScene();
  lit.entities.push({
    ...entity(randomUUID(), 'Lamp', 'light', [-4, 3, 1], [0.25, 0.25, 0.25], [1, 0.7, 0.3]),
    material: { albedo: [1, 0.7, 0.3], roughness: 0.2, metalness: 0, pattern: 'metal' },
    light: { color: [1, 0.7, 0.3], intensity: 3, range: 8 },
  });
  assert.equal(validateScene(lit).valid, true);
  const noLightObject = structuredClone(lit);
  delete noLightObject.entities.at(-1).light;
  assert.equal(validateScene(noLightObject).valid, false);
  const badLight = structuredClone(lit);
  badLight.entities.at(-1).light.intensity = 0;
  assert.equal(validateScene(badLight).valid, false);
  const lightOnProp = structuredClone(lit);
  lightOnProp.entities.at(-1).kind = 'prop';
  assert.equal(validateScene(lightOnProp).valid, false);
  const badMaterial = structuredClone(lit);
  badMaterial.entities.at(-1).material.pattern = 'glow';
  assert.equal(validateScene(badMaterial).valid, false);
  const tooManyLights = structuredClone(lit);
  const extraLight = structuredClone(lit.entities.at(-1));
  for (let i = 0; i < 4; ++i) {
    tooManyLights.entities.push({
      ...structuredClone(extraLight),
      id: randomUUID(),
      name: `Extra lamp ${i}`,
    });
  }
  assert.equal(validateScene(tooManyLights).valid, false);

  const badScale = makeScene();
  badScale.entities[2].scale[0] = 0;
  assert.equal(validateScene(badScale).valid, false);

  const nonFinite = makeScene();
  nonFinite.entities[2].position[0] = Infinity;
  assert.equal(validateScene(nonFinite).valid, false);

  const badSettings = makeScene();
  badSettings.settings.walkSpeed = 0;
  assert.equal(validateScene(badSettings).valid, false);
  const reversedSpeed = makeScene();
  reversedSpeed.settings.walkSpeed = 10;
  reversedSpeed.settings.runSpeed = 5;
  assert.equal(validateScene(reversedSpeed).valid, false);

  assert.equal(validateScene(null).valid, false);
  assert.equal(validateScene([]).valid, false);
});

test('stable JSON and scene hashes ignore object key order and reject invalid scenes', () => {
  const scene = makeScene();
  const reordered = Object.fromEntries(Object.entries(scene).reverse());
  assert.equal(stableJSON(scene), stableJSON(reordered));
  assert.equal(sceneHash(scene), sceneHash(reordered));
  assert.match(sceneHash(scene), /^[0-9a-f]{64}$/);
  assert.throws(() => sceneHash({ ...scene, revision: -1 }), SceneError);
});

test('query returns filtered clones and validates query fields', () => {
  const scene = makeScene();
  const props = queryScene(scene, { kind: 'prop' });
  assert.equal(props.revision, 0);
  assert.equal(props.entities.length, 1);
  assert.equal(props.entities[0].name, 'Crate');
  props.entities[0].name = 'Mutated';
  assert.equal(scene.entities[2].name, 'Crate');

  assert.equal(queryScene(scene, { name: 'cRaTe' }).entities.length, 1);
  assert.equal(queryScene(scene, { entityId: PROP.toUpperCase() }).entities.length, 1);
  assert.equal(queryScene(scene, {}).entities.length, scene.entities.length);
  throwsCode(() => queryScene(scene, { kind: 'npc' }), 'INVALID_QUERY');
  throwsCode(() => queryScene(scene, { entityId: 'not-a-uuid' }), 'INVALID_QUERY');
  throwsCode(() => queryScene(scene, { name: 3 }), 'INVALID_QUERY');
  throwsCode(() => queryScene(scene, { path: '../secret' }), 'INVALID_QUERY');
});

test('preview applies a batch to a clone and reports ordered changes', () => {
  const scene = makeScene();
  const tx = transaction(scene, [
    { op: 'set', entityId: PROP, changes: { position: [-4, 0.6, 1], color: [1, 0, 0] } },
    { op: 'create', entity: entity(randomUUID(), 'Lamp', 'prop', [1, 1, 1], [0.2, 2, 0.2]) },
    { op: 'remove', entityId: KEY },
  ]);
  const result = previewTransaction(scene, tx);
  assert.equal(result.baseRevision, 0);
  assert.equal(result.revision, 1);
  assert.equal(result.changes.length, 3);
  assert.equal(result.changes[0].op, 'set');
  assert.deepEqual(result.changes[0].after.position, [-4, 0.6, 1]);
  assert.equal(result.changes[1].op, 'create');
  assert.equal(result.changes[2].op, 'remove');
  assert.equal(result.scene.revision, 1);
  assert.equal(result.scene.entities.length, scene.entities.length);
  assert.equal(scene.revision, 0);
  assert.equal(scene.entities[2].position[0], 2);
  assert.equal(validateScene(result.scene).valid, true);
});

test('transaction failures are explicit and never half-apply', () => {
  const scene = makeScene();
  const original = structuredClone(scene);

  throwsCode(() => previewTransaction(scene, transaction(scene, [])), 'INVALID_TRANSACTION');
  throwsCode(() => previewTransaction(scene, { ...transaction(scene, [
    { op: 'remove', entityId: KEY },
  ]), expectedRevision: 4 }), 'REVISION_CONFLICT');
  throwsCode(() => previewTransaction(scene, {
    projectId: randomUUID(), expectedRevision: 0,
    commands: [{ op: 'remove', entityId: KEY }],
  }), 'PROJECT_MISMATCH');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'create', entity: scene.entities[2] },
  ])), 'DUPLICATE_ENTITY');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'remove', entityId: PLAYER },
  ])), 'INVALID_SCENE');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { id: randomUUID() } },
  ])), 'INVALID_COMMAND');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { script: 'evil.js' } },
  ])), 'INVALID_COMMAND');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { position: [Infinity, 0, 0] } },
  ])), 'INVALID_SCENE');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'remove', entityId: PROP },
    { op: 'set', entityId: PROP, changes: { position: [0, 0, 0] } },
  ])), 'ENTITY_NOT_FOUND');
  throwsCode(() => previewTransaction(scene, transaction(scene, [
    { op: 'explode', entityId: PROP },
  ])), 'INVALID_COMMAND');
  throwsCode(() => previewTransaction(scene, {
    ...transaction(scene, [{ op: 'remove', entityId: KEY }]),
    extra: true,
  }), 'INVALID_TRANSACTION');
  throwsCode(() => previewTransaction(scene, transaction(scene,
    Array.from({ length: 1001 }, () => ({ op: 'remove', entityId: KEY })))), 'INVALID_TRANSACTION');

  // A failure in the second command must not leak the first command's change.
  assert.deepEqual(scene, original);
  assert.equal(validateScene(scene).valid, true);
  const applied = applyTransaction(scene, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { position: [1, 2, 3] } },
  ]));
  assert.equal(applied.scene.revision, 1);
  assert.equal(applied.undoRecord.before.revision, 0);
  assert.equal(applied.undoRecord.before.entities[2].position[0], 2);
});

test('save, commit, undo and conflict detection persist atomically', async t => {
  const { directory, file } = await temporaryProject(t);
  const scene = makeScene();
  assert.deepEqual(await saveScene(file, scene), scene);
  await rejectsCode(saveScene(file, scene), 'ALREADY_EXISTS');
  assert.deepEqual(await loadScene(file), scene);

  const first = await commitScene(file, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { position: [-4, 0.6, 1] } },
  ]));
  assert.equal(first.revision, 1);
  assert.equal(await loadScene(file).then(value => value.revision), 1);
  assert.equal((await fs.readFile(`${file}.pending.json`).catch(error => error)).code, 'ENOENT');
  await fs.access(`${file}.snapshot.json`);
  const history = JSON.parse(await fs.readFile(`${file}.history.json`, 'utf8'));
  assert.equal(history.headRevision, 1);
  assert.equal(history.undoStack.length, 1);

  await rejectsCode(commitScene(file, {
    ...transaction(first.scene, [{ op: 'remove', entityId: KEY }]),
    expectedRevision: 0,
  }), 'REVISION_CONFLICT');
  // A failed transaction leaves both scene and history untouched.
  assert.equal((await loadScene(file)).entities.length, scene.entities.length);

  const undone = await undoSceneFile(file, { projectId: PROJECT, expectedRevision: 1 });
  assert.equal(undone.revision, 2);
  assert.equal(undone.scene.entities[2].position[2], 2);
  assert.equal((await loadScene(file)).revision, 2);
  await rejectsCode(undoSceneFile(file, { projectId: PROJECT, expectedRevision: 2 }), 'NOTHING_TO_UNDO');

  const lock = `${file}.lock`;
  await fs.writeFile(lock, JSON.stringify({ pid: process.pid }));
  await rejectsCode(commitScene(file, transaction(undone.scene, [
    { op: 'remove', entityId: KEY },
  ])), 'PROJECT_BUSY');
  await fs.rm(lock);
  const afterLock = await commitScene(file, transaction(undone.scene, [
    { op: 'remove', entityId: KEY },
  ]));
  assert.equal(afterLock.revision, 3);
  assert.equal((await fs.access(`${file}.lock`).catch(error => error)).code, 'ENOENT');

  await fs.mkdir(path.join(directory, 'other'), { recursive: true });
  const invalid = makeScene({ entities: [] });
  await rejectsCode(saveScene(path.join(directory, 'other', 'bad.json'), invalid), 'INVALID_SCENE');
});

test('recover completes an interrupted commit and restores a corrupt scene', async t => {
  const { file } = await temporaryProject(t);
  const scene = makeScene();
  await saveScene(file, scene);

  // Simulate a crash after the new scene landed but before history did.
  const pendingResult = applyTransaction(scene, transaction(scene, [
    { op: 'set', entityId: PROP, changes: { position: [9, 0.6, 9] } },
  ]));
  const pendingHistory = {
    version: 1,
    projectId: PROJECT,
    headRevision: 1,
    headHash: sceneHash(pendingResult.scene),
    undoStack: [pendingResult.undoRecord],
  };
  await fs.writeFile(file, `${stableJSON(pendingResult.scene)}\n`);
  await fs.writeFile(`${file}.snapshot.json`, `${stableJSON({
    version: 1, scene, history: JSON.parse(await fs.readFile(`${file}.history.json`, 'utf8')),
  })}\n`);
  await fs.writeFile(`${file}.pending.json`, `${stableJSON({
    version: 1,
    previousHash: sceneHash(scene),
    scene: pendingResult.scene,
    history: pendingHistory,
  })}\n`);
  await rejectsCode(loadScene(file), 'RECOVERY_REQUIRED');
  const completed = await recoverScene(file);
  assert.equal(completed.recovery, 'completed-interrupted-save');
  assert.equal(completed.scene.revision, 1);
  assert.equal((await fs.access(`${file}.pending.json`).catch(error => error)).code, 'ENOENT');
  assert.equal((await loadScene(file)).entities[2].position[0], 9);

  // Corrupt the scene after a commit; recovery must use the immutable snapshot.
  await fs.writeFile(file, '{"broken":');
  const restored = await recoverScene(file);
  assert.equal(restored.recovery, 'restored-last-snapshot');
  assert.ok(restored.scene.revision >= 2);
  assert.equal(validateScene(restored.scene).valid, true);
  assert.equal((await loadScene(file)).revision, restored.scene.revision);

  const fresh = await temporaryProject(t);
  await fs.writeFile(fresh.file, '{"broken":');
  await rejectsCode(recoverScene(fresh.file), 'NO_SNAPSHOT');
});

test('content-addressed builds are deterministic, cacheable and tamper-evident', async t => {
  const { directory, file } = await temporaryProject(t);
  const scene = makeScene();
  await saveScene(file, scene);
  const output = path.join(directory, 'packages');
  const first = await buildScene(file, output);
  assert.equal(first.cacheHit, false);
  assert.match(first.manifest.buildKey, /^[0-9a-f]{64}$/);
  assert.equal(first.manifest.sceneRevision, 0);
  assert.equal(first.manifest.toolVersion, undefined);
  assert.equal(first.manifest.tool.version, TOOL_VERSION);
  const manifest = JSON.parse(await fs.readFile(path.join(first.packageDirectory, 'manifest.json'), 'utf8'));
  assert.deepEqual(manifest, first.manifest);
  assert.doesNotMatch(JSON.stringify(manifest), /timestamp|C:\\\\/i);

  const second = await buildScene(file, output);
  assert.equal(second.cacheHit, true);
  assert.equal(second.packageDirectory, first.packageDirectory);

  const sceneCopy = structuredClone(scene);
  const other = await buildScene(sceneCopy, path.join(directory, 'other'));
  assert.equal(other.manifest.buildKey, first.manifest.buildKey);

  await fs.writeFile(path.join(first.packageDirectory, 'scene.json'), '{}\n');
  await rejectsCode(buildScene(file, output), 'BUILD_CONFLICT');
  await rejectsCode(buildScene(scene, output, { target: 'linux-x64' }), 'UNSUPPORTED_TARGET');

  const invalid = makeScene();
  invalid.entities[0].kind = 'npc';
  await rejectsCode(buildScene(invalid, path.join(directory, 'invalid')), 'INVALID_SCENE');
});

test('schema file documents the same limits as the runtime validator', async () => {
  const schema = JSON.parse(await fs.readFile(new URL('../assets/scene.schema.json', import.meta.url), 'utf8'));
  assert.equal(schema.$schema, 'https://json-schema.org/draft/2020-12/schema');
  assert.equal(schema.additionalProperties, false);
  assert.equal(schema.properties.entities.maxItems, 10000);
  assert.equal(schema.properties.entities.allOf[0].minContains, 1);
  assert.equal(schema.properties.entities.allOf[0].maxContains, 1);
  assert.equal(schema.$defs.settings.properties.walkSpeed.maximum, 30);
  assert.equal(schema.$defs.settings.properties.gravity.maximum, 100);
  assert.ok(schema.$defs.entity.properties.kind.enum.includes('light'));
  assert.equal(schema.$defs.material.properties.pattern.enum.join(','), 'checker,brick,metal');
  assert.equal(schema.$defs.light.properties.intensity.maximum, 100);
  assert.equal(schema.$defs.uuid.pattern, '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$');
});

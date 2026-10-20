import { createHash, randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';

export const TOOL_VERSION = '0.3.0';
export const ENTITY_KINDS = Object.freeze(['ground', 'wall', 'prop', 'player', 'key', 'door', 'goal', 'light']);
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const SCENE_KEYS = ['schemaVersion', 'projectId', 'name', 'revision', 'entities', 'settings'];
const ENTITY_KEYS = ['id', 'name', 'kind', 'position', 'scale', 'color', 'collider'];
const ENTITY_OPTIONAL_KEYS = ['material', 'light'];
const MATERIAL_KEYS = ['albedo', 'roughness', 'metalness', 'pattern'];
const LIGHT_KEYS = ['color', 'intensity', 'range'];
const MATERIAL_PATTERNS = Object.freeze(['checker', 'brick', 'metal']);
const SETTINGS_KEYS = ['walkSpeed', 'runSpeed', 'jumpSpeed', 'gravity'];
const MAX_ENTITIES = 10000;
const MAX_FILE_BYTES = 16 * 1024 * 1024;
const SETTINGS_RANGE = Object.freeze({
  walkSpeed: [0.1, 30],
  runSpeed: [0.1, 60],
  jumpSpeed: [0.1, 30],
  gravity: [0.1, 100],
});

export class SceneError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'SceneError';
    this.code = code;
    this.details = details;
  }
}

function object(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function exactKeys(value, keys, location, errors, optional = []) {
  if (!object(value)) {
    errors.push(`${location} must be an object`);
    return false;
  }
  const allowed = [...keys, ...optional];
  for (const key of Object.keys(value)) {
    if (!allowed.includes(key)) errors.push(`${location}.${key} is not supported`);
  }
  for (const key of keys) {
    if (!Object.hasOwn(value, key)) errors.push(`${location}.${key} is required`);
  }
  return true;
}

function text(value) {
  return typeof value === 'string' && [...value].length > 0 && [...value].length <= 128
    && !/[\u0000-\u001f]/.test(value) && /\S/.test(value);
}

function vector(value, predicate) {
  return Array.isArray(value) && value.length === 3
    && value.every(number => Number.isFinite(number) && predicate(number));
}

function validateEntity(entity, location, errors) {
  if (!exactKeys(entity, ENTITY_KEYS, location, errors, ENTITY_OPTIONAL_KEYS)) return;
  if (typeof entity.id !== 'string' || !UUID.test(entity.id)) errors.push(`${location}.id must be a UUID`);
  if (!text(entity.name)) errors.push(`${location}.name must contain 1–128 printable characters`);
  if (!ENTITY_KINDS.includes(entity.kind)) errors.push(`${location}.kind is unsupported`);
  if (!vector(entity.position, n => Math.abs(n) <= 100000)) errors.push(`${location}.position must contain three finite coordinates within ±100000`);
  if (!vector(entity.scale, n => n >= 0.001 && n <= 10000)) errors.push(`${location}.scale must contain three dimensions between 0.001 and 10000`);
  if (!vector(entity.color, n => n >= 0 && n <= 1)) errors.push(`${location}.color must contain three channels in [0, 1]`);
  if (typeof entity.collider !== 'boolean') errors.push(`${location}.collider must be boolean`);
  if (entity.material !== undefined) {
    if (!object(entity.material)) {
      errors.push(`${location}.material must be an object`);
    } else {
      exactKeys(entity.material, MATERIAL_KEYS, `${location}.material`, errors);
      if (!vector(entity.material.albedo, n => n >= 0 && n <= 1)) {
        errors.push(`${location}.material.albedo must contain three channels in [0, 1]`);
      }
      for (const key of ['roughness', 'metalness']) {
        if (!Number.isFinite(entity.material[key]) || entity.material[key] < 0 || entity.material[key] > 1) {
          errors.push(`${location}.material.${key} must be a number in [0, 1]`);
        }
      }
      if (!MATERIAL_PATTERNS.includes(entity.material.pattern)) {
        errors.push(`${location}.material.pattern must be checker, brick or metal`);
      }
    }
  }
  if (entity.kind === 'light') {
    if (!object(entity.light)) {
      errors.push(`${location}.light is required for a light entity`);
    } else {
      exactKeys(entity.light, LIGHT_KEYS, `${location}.light`, errors);
      if (!vector(entity.light.color, n => n >= 0 && n <= 1)) {
        errors.push(`${location}.light.color must contain three channels in [0, 1]`);
      }
      if (!Number.isFinite(entity.light.intensity) || entity.light.intensity <= 0 || entity.light.intensity > 100) {
        errors.push(`${location}.light.intensity must be a finite number in (0, 100]`);
      }
      if (!Number.isFinite(entity.light.range) || entity.light.range <= 0 || entity.light.range > 1000) {
        errors.push(`${location}.light.range must be a finite number in (0, 1000]`);
      }
    }
  } else if (entity.light !== undefined) {
    errors.push(`${location}.light is only supported on a light entity`);
  }
}

export function validateScene(scene) {
  const errors = [];
  if (exactKeys(scene, SCENE_KEYS, 'scene', errors)) {
    if (scene.schemaVersion !== 1) errors.push('scene.schemaVersion must be 1');
    if (typeof scene.projectId !== 'string' || !UUID.test(scene.projectId)) errors.push('scene.projectId must be a UUID');
    if (!text(scene.name)) errors.push('scene.name must contain 1–128 printable characters');
    if (!Number.isSafeInteger(scene.revision) || scene.revision < 0) errors.push('scene.revision must be a non-negative safe integer');
    if (!Array.isArray(scene.entities) || scene.entities.length > MAX_ENTITIES) {
      errors.push(`scene.entities must be an array of at most ${MAX_ENTITIES} entities`);
    } else {
      const ids = new Set();
      for (const [index, entity] of scene.entities.entries()) {
        validateEntity(entity, `scene.entities[${index}]`, errors);
        if (object(entity) && typeof entity.id === 'string') {
          const id = entity.id.toLowerCase();
          if (ids.has(id)) errors.push(`scene.entities[${index}].id is duplicated`);
          ids.add(id);
        }
      }
      if (scene.entities.filter(entity => entity?.kind === 'player').length !== 1) {
        errors.push('scene must contain exactly one player spawn');
      }
      if (scene.entities.filter(entity => entity?.kind === 'ground').length < 1) {
        errors.push('scene must contain at least one ground');
      }
      if (scene.entities.filter(entity => entity?.kind === 'light').length > 4) {
        errors.push('scene supports at most 4 lights');
      }
    }
    if (exactKeys(scene.settings, SETTINGS_KEYS, 'scene.settings', errors)) {
      for (const key of SETTINGS_KEYS) {
        const [low, high] = SETTINGS_RANGE[key];
        if (!Number.isFinite(scene.settings[key])
            || scene.settings[key] < low || scene.settings[key] > high) {
          errors.push(`scene.settings.${key} must be a finite number between ${low} and ${high}`);
        }
      }
      if (scene.settings.runSpeed < scene.settings.walkSpeed) errors.push('scene.settings.runSpeed must be at least walkSpeed');
    }
  }
  return { valid: errors.length === 0, errors };
}

function assertScene(scene) {
  const result = validateScene(scene);
  if (!result.valid) throw new SceneError('INVALID_SCENE', 'Scene validation failed', result.errors);
}

export function stableJSON(value) {
  if (Array.isArray(value)) return `[${value.map(stableJSON).join(',')}]`;
  if (object(value)) {
    return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${stableJSON(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}

export function sceneHash(scene) {
  assertScene(scene);
  return createHash('sha256').update(stableJSON(scene)).digest('hex');
}

function sameId(a, b) {
  return typeof a === 'string' && typeof b === 'string' && a.toLowerCase() === b.toLowerCase();
}

function checkRevision(scene, projectId, expectedRevision) {
  if (!sameId(projectId, scene.projectId)) throw new SceneError('PROJECT_MISMATCH', 'The command belongs to a different project');
  if (!Number.isSafeInteger(expectedRevision) || expectedRevision !== scene.revision) {
    throw new SceneError('REVISION_CONFLICT', `Expected revision ${expectedRevision}; current revision is ${scene.revision}`);
  }
  if (scene.revision === Number.MAX_SAFE_INTEGER) throw new SceneError('REVISION_EXHAUSTED', 'Revision counter is exhausted');
}

export function queryScene(scene, query = {}) {
  assertScene(scene);
  const allowed = ['kind', 'entityId', 'name'];
  if (!object(query) || Object.keys(query).some(key => !allowed.includes(key))) {
    throw new SceneError('INVALID_QUERY', 'Query accepts kind, entityId and name only');
  }
  if (query.kind !== undefined && !ENTITY_KINDS.includes(query.kind)) throw new SceneError('INVALID_QUERY', 'Unknown entity kind');
  if (query.entityId !== undefined && (typeof query.entityId !== 'string' || !UUID.test(query.entityId))) {
    throw new SceneError('INVALID_QUERY', 'entityId must be a UUID');
  }
  if (query.name !== undefined && typeof query.name !== 'string') throw new SceneError('INVALID_QUERY', 'name must be a string');
  const entities = scene.entities.filter(entity =>
    (query.kind === undefined || entity.kind === query.kind)
    && (query.entityId === undefined || sameId(entity.id, query.entityId))
    && (query.name === undefined || entity.name.toLowerCase().includes(query.name.toLowerCase())));
  return { projectId: scene.projectId, revision: scene.revision, entities: structuredClone(entities) };
}

export function previewTransaction(scene, transaction) {
  assertScene(scene);
  const errors = [];
  if (!exactKeys(transaction, ['projectId', 'expectedRevision', 'commands'], 'transaction', errors)) {
    throw new SceneError('INVALID_TRANSACTION', 'Transaction must be an object', errors);
  }
  if (errors.length) throw new SceneError('INVALID_TRANSACTION', 'Transaction contains unsupported or missing fields', errors);
  checkRevision(scene, transaction.projectId, transaction.expectedRevision);
  if (!Array.isArray(transaction.commands) || transaction.commands.length === 0 || transaction.commands.length > 1000) {
    throw new SceneError('INVALID_TRANSACTION', 'A transaction must contain between 1 and 1000 commands');
  }
  const candidate = structuredClone(scene);
  const changes = [];
  for (const [index, command] of transaction.commands.entries()) {
    const commandErrors = [];
    if (!object(command) || !['create', 'set', 'remove'].includes(command.op)) {
      throw new SceneError('INVALID_COMMAND', `Command ${index} has an unsupported operation`);
    }
    const keys = command.op === 'create' ? ['op', 'entity']
      : command.op === 'set' ? ['op', 'entityId', 'changes'] : ['op', 'entityId'];
    exactKeys(command, keys, `commands[${index}]`, commandErrors);
    if (commandErrors.length) throw new SceneError('INVALID_COMMAND', `Command ${index} has invalid fields`, commandErrors);
    if (command.op === 'create') {
      validateEntity(command.entity, `commands[${index}].entity`, commandErrors);
      if (commandErrors.length) throw new SceneError('INVALID_COMMAND', `Command ${index} has an invalid entity`, commandErrors);
      if (candidate.entities.some(entity => sameId(entity.id, command.entity.id))) {
        throw new SceneError('DUPLICATE_ENTITY', `Entity ${command.entity.id} already exists`);
      }
      candidate.entities.push(structuredClone(command.entity));
      changes.push({ op: 'create', entityId: command.entity.id, name: command.entity.name });
      continue;
    }
    if (typeof command.entityId !== 'string' || !UUID.test(command.entityId)) throw new SceneError('INVALID_COMMAND', `Command ${index} entityId must be a UUID`);
    const entityIndex = candidate.entities.findIndex(entity => sameId(entity.id, command.entityId));
    if (entityIndex < 0) throw new SceneError('ENTITY_NOT_FOUND', `Entity ${command.entityId} does not exist`);
    const entity = candidate.entities[entityIndex];
    if (command.op === 'remove') {
      candidate.entities.splice(entityIndex, 1);
      changes.push({ op: 'remove', entityId: entity.id, name: entity.name });
    } else {
      if (!object(command.changes) || Object.keys(command.changes).length === 0
          || Object.keys(command.changes).some(key => !ENTITY_KEYS.includes(key) || key === 'id')) {
        throw new SceneError('INVALID_COMMAND', 'set changes must contain supported mutable entity fields; id cannot change');
      }
      candidate.entities[entityIndex] = { ...entity, ...structuredClone(command.changes) };
      changes.push({ op: 'set', entityId: entity.id, before: structuredClone(entity), after: structuredClone(candidate.entities[entityIndex]) });
    }
  }
  candidate.revision += 1;
  assertScene(candidate);
  return { projectId: scene.projectId, baseRevision: scene.revision, revision: candidate.revision, changes, scene: candidate };
}

export function applyTransaction(scene, transaction) {
  const result = previewTransaction(scene, transaction);
  return { ...result, undoRecord: { projectId: scene.projectId, before: structuredClone(scene) } };
}

export function undoScene(scene, undoRecord, expectedRevision) {
  assertScene(scene);
  if (!object(undoRecord) || !object(undoRecord.before)) throw new SceneError('INVALID_HISTORY', 'Undo record is invalid');
  checkRevision(scene, undoRecord.projectId, expectedRevision);
  assertScene(undoRecord.before);
  if (!sameId(undoRecord.before.projectId, scene.projectId)) throw new SceneError('PROJECT_MISMATCH', 'Undo snapshot belongs to another project');
  if (undoRecord.before.revision >= scene.revision) throw new SceneError('INVALID_HISTORY', 'Undo snapshot must precede the current revision');
  return { ...structuredClone(undoRecord.before), revision: scene.revision + 1 };
}

async function readJSON(file) {
  try {
    const stat = await fs.stat(file);
    if (stat.size > MAX_FILE_BYTES) throw new SceneError('FILE_TOO_LARGE', `JSON file exceeds ${MAX_FILE_BYTES} bytes`);
    return JSON.parse(await fs.readFile(file, 'utf8'));
  } catch (error) {
    if (error instanceof SyntaxError) throw new SceneError('INVALID_JSON', `Invalid JSON in ${path.basename(file)}`);
    throw error;
  }
}

async function exists(file) {
  try { await fs.access(file); return true; }
  catch (error) { if (error.code === 'ENOENT') return false; throw error; }
}

async function atomicWrite(file, value) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  const temporary = `${file}.${randomUUID()}.tmp`;
  let handle;
  try {
    handle = await fs.open(temporary, 'wx');
    await handle.writeFile(`${stableJSON(value)}\n`, 'utf8');
    await handle.sync();
    await handle.close();
    handle = null;
    await fs.rename(temporary, file);
  } finally {
    await handle?.close();
    await fs.rm(temporary, { force: true });
  }
}

async function locked(file, action) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  const lockPath = `${file}.lock`;
  let handle;
  try {
    handle = await fs.open(lockPath, 'wx');
  } catch (error) {
    if (error.code === 'EEXIST') throw new SceneError('PROJECT_BUSY', 'Project is locked. If a process crashed, verify it has stopped before removing the .lock file.');
    throw error;
  }
  try {
    await handle.writeFile(JSON.stringify({ pid: process.pid }));
    return await action();
  } finally {
    await handle.close();
    await fs.rm(lockPath, { force: true });
  }
}

function emptyHistory(scene) {
  return { version: 1, projectId: scene.projectId, headRevision: scene.revision, headHash: sceneHash(scene), undoStack: [] };
}

function assertHistory(history, scene) {
  if (!object(history) || history.version !== 1 || !sameId(history.projectId, scene.projectId)
      || history.headRevision !== scene.revision || history.headHash !== sceneHash(scene)
      || !Array.isArray(history.undoStack) || history.undoStack.length > 100) {
    throw new SceneError('HISTORY_CONFLICT', 'History does not match the saved scene; use recover or restore a matching backup');
  }
  for (const record of history.undoStack) {
    if (!object(record) || !sameId(record.projectId, scene.projectId)
        || !sameId(record.before?.projectId, scene.projectId) || !validateScene(record.before).valid
        || record.before.revision >= scene.revision) {
      throw new SceneError('INVALID_HISTORY', 'Undo history contains an invalid snapshot');
    }
  }
}

async function readState(file) {
  const scene = await readJSON(file);
  assertScene(scene);
  const history = await exists(`${file}.history.json`) ? await readJSON(`${file}.history.json`) : emptyHistory(scene);
  assertHistory(history, scene);
  return { scene, history };
}

function validateEnvelope(envelope) {
  if (!object(envelope) || envelope.version !== 1 || !object(envelope.scene) || !object(envelope.history)) {
    throw new SceneError('INVALID_RECOVERY', 'Recovery record is invalid');
  }
  assertScene(envelope.scene);
  assertHistory(envelope.history, envelope.scene);
}

async function reconcilePending(file) {
  const pendingPath = `${file}.pending.json`;
  if (!await exists(pendingPath)) return;
  const pending = await readJSON(pendingPath);
  validateEnvelope(pending);
  const scene = await readJSON(file);
  assertScene(scene);
  const hash = sceneHash(scene);
  if (hash === sceneHash(pending.scene)) {
    // Scene replacement is the commit point. Complete an interrupted history write.
    await atomicWrite(`${file}.history.json`, pending.history);
  } else if (hash !== pending.previousHash) {
    throw new SceneError('RECOVERY_REQUIRED', 'Interrupted save conflicts with the current scene; explicit recovery is required');
  }
  await fs.rm(pendingPath);
}

async function persistState(file, previous, next) {
  if (previous) {
    await atomicWrite(`${file}.snapshot.json`, { version: 1, ...previous });
    await atomicWrite(`${file}.pending.json`, { version: 1, previousHash: sceneHash(previous.scene), ...next });
  }
  await atomicWrite(file, next.scene);
  await atomicWrite(`${file}.history.json`, next.history);
  await fs.rm(`${file}.pending.json`, { force: true });
}

export async function loadScene(file) {
  file = path.resolve(file);
  if (await exists(`${file}.pending.json`)) {
    throw new SceneError('RECOVERY_REQUIRED', 'An interrupted save was detected. Run recover before opening the scene.');
  }
  return (await readState(file)).scene;
}

export async function saveScene(file, scene) {
  assertScene(scene);
  file = path.resolve(file);
  return locked(file, async () => {
    if (await exists(file)) throw new SceneError('ALREADY_EXISTS', 'saveScene creates a new project; edit an existing project with commitScene');
    await persistState(file, null, { scene, history: emptyHistory(scene) });
    return structuredClone(scene);
  });
}

export async function commitScene(file, transaction) {
  file = path.resolve(file);
  return locked(file, async () => {
    await reconcilePending(file);
    const previous = await readState(file);
    const result = applyTransaction(previous.scene, transaction);
    const history = {
      ...emptyHistory(result.scene),
      undoStack: [...previous.history.undoStack, result.undoRecord].slice(-100),
    };
    await persistState(file, previous, { scene: result.scene, history });
    return result;
  });
}

export async function undoSceneFile(file, { projectId, expectedRevision }) {
  file = path.resolve(file);
  return locked(file, async () => {
    await reconcilePending(file);
    const previous = await readState(file);
    checkRevision(previous.scene, projectId, expectedRevision);
    const record = previous.history.undoStack.at(-1);
    if (!record) throw new SceneError('NOTHING_TO_UNDO', 'The project has no saved undo history');
    const scene = undoScene(previous.scene, record, expectedRevision);
    const history = { ...emptyHistory(scene), undoStack: previous.history.undoStack.slice(0, -1) };
    await persistState(file, previous, { scene, history });
    return { scene, revision: scene.revision };
  });
}

export async function recoverScene(file) {
  file = path.resolve(file);
  return locked(file, async () => {
    if (await exists(`${file}.pending.json`)) {
      try {
        await reconcilePending(file);
        return { scene: (await readState(file)).scene, recovery: 'completed-interrupted-save' };
      } catch (error) {
        if (!(error instanceof SceneError) && error.code !== 'ENOENT') throw error;
      }
    }
    // The snapshot is immutable during recovery so another failure remains recoverable.
    if (!await exists(`${file}.snapshot.json`)) throw new SceneError('NO_SNAPSHOT', 'No valid recovery snapshot exists');
    const snapshot = await readJSON(`${file}.snapshot.json`);
    validateEnvelope(snapshot);
    let greatestRevision = snapshot.scene.revision;
    for (const candidate of [file, `${file}.history.json`, `${file}.pending.json`]) {
      try {
        const data = await readJSON(candidate);
        const revision = data.revision ?? data.headRevision ?? data.scene?.revision;
        if (Number.isSafeInteger(revision) && revision >= 0) greatestRevision = Math.max(greatestRevision, revision);
      } catch { /* A corrupt candidate does not invalidate a verified snapshot. */ }
    }
    if (greatestRevision === Number.MAX_SAFE_INTEGER) throw new SceneError('REVISION_EXHAUSTED', 'Cannot increment revision during recovery');
    const scene = { ...structuredClone(snapshot.scene), revision: greatestRevision + 1 };
    const history = { ...emptyHistory(scene), undoStack: snapshot.history.undoStack };
    await atomicWrite(`${file}.pending.json`, { version: 1, previousHash: sceneHash(snapshot.scene), scene, history });
    await atomicWrite(file, scene);
    await atomicWrite(`${file}.history.json`, history);
    await fs.rm(`${file}.pending.json`, { force: true });
    return { scene, recovery: 'restored-last-snapshot' };
  });
}

export async function buildScene(sceneOrFile, outputDirectory, { target = 'windows-x64' } = {}) {
  if (target !== 'windows-x64') throw new SceneError('UNSUPPORTED_TARGET', 'Only windows-x64 packages are supported');
  const scene = typeof sceneOrFile === 'string' ? await loadScene(sceneOrFile) : structuredClone(sceneOrFile);
  assertScene(scene);
  const sceneData = `${stableJSON(scene)}\n`;
  const contentHash = createHash('sha256').update(sceneData).digest('hex');
  const buildKey = createHash('sha256').update(stableJSON({ contentHash, toolVersion: TOOL_VERSION, target })).digest('hex');
  const manifest = {
    formatVersion: 1,
    tool: { name: 'nativegpu-scene-build', version: TOOL_VERSION },
    target,
    projectId: scene.projectId,
    sceneRevision: scene.revision,
    buildKey,
    entryScene: 'scene.json',
    files: [{ path: 'scene.json', sha256: contentHash, bytes: Buffer.byteLength(sceneData) }],
    dependencies: [],
    entityCount: scene.entities.length,
    includedAssetTypes: ['procedural-box'],
  };
  const root = path.resolve(outputDirectory);
  const packageDirectory = path.join(root, buildKey);
  await fs.mkdir(root, { recursive: true });
  if (await exists(packageDirectory)) {
    const prior = await readJSON(path.join(packageDirectory, 'manifest.json'));
    const priorScene = await fs.readFile(path.join(packageDirectory, 'scene.json'), 'utf8');
    const entries = (await fs.readdir(packageDirectory)).sort();
    if (stableJSON(prior) !== stableJSON(manifest) || priorScene !== sceneData
        || entries.join(',') !== 'manifest.json,scene.json') {
      throw new SceneError('BUILD_CONFLICT', 'An existing content-addressed package differs from its expected contents');
    }
    return { packageDirectory, manifest, cacheHit: true };
  }
  const staging = path.join(root, `.scene-build-${randomUUID()}`);
  await fs.mkdir(staging);
  try {
    await atomicWrite(path.join(staging, 'scene.json'), scene);
    await atomicWrite(path.join(staging, 'manifest.json'), manifest);
    await fs.rename(staging, packageDirectory);
  } finally {
    // Only remove the exact staging directory constructed under the resolved output root.
    if (path.dirname(staging) !== root || !path.basename(staging).startsWith('.scene-build-')) {
      throw new SceneError('UNSAFE_PATH', 'Unexpected staging path');
    }
    await fs.rm(staging, { recursive: true, force: true });
  }
  return { packageDirectory, manifest, cacheHit: false };
}

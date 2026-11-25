// World service: the single controlled API behind the editor UI and the AI tools.
//
// Everything the user (or a model) can do to a world goes through this class, which is what
// makes the two paths interchangeable: the panel calls `world_preview`, the AI calls
// `world_preview`, and both get the same task identity, the same validation and the same
// commit rules.
//
// The model is draft-then-commit:
//
//   open      -> recipe + layers loaded into an in-memory draft
//   preview   -> mutates the draft, cooks it in the background, returns a taskId
//   status    -> progress and diagnostics for that task
//   apply     -> publishes the cooked result and switches the scene's world reference
//   undo/redo -> restores a draft snapshot; nothing is copied but the recipe and the hash
//
// Three invariants are enforced here rather than in the UI, because a UI cannot be trusted
// to enforce them and an AI surface certainly cannot:
//
//   * A task belongs to an epoch. Opening another world, closing the project or resetting
//     bumps the epoch, and a task from an older epoch can never be applied.
//   * Applying checks the expected revision *and* the input hash. A stale cook is refused
//     with REVISION_CONFLICT instead of silently overwriting newer work.
//   * A preview never writes to the scene. It writes into build/world-cache and stops.

import fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

import {
  DEFAULT_WORKER, cookWorld, readManifest, readRecipe, releaseClosure, resolveWorld,
  writeLayer, writeLibrary,
} from '../engine/world/cook.mjs';
import { WORLD_ERROR_CODES, WorldError, diagnostic } from '../engine/world/errors.mjs';
import { inputDigest, sha256Hex } from '../engine/world/hash.mjs';
import { defaultLibrary, summarizeLibrary } from '../engine/world/library.mjs';
import { cookedPath, resolveProjectPath, WORLD_LAYOUT, isWritableWorldPath } from '../engine/world/paths.mjs';
import {
  BUDGETS, GENERATOR_VERSION, LAYER_SCHEMA, RECIPE_SCHEMA, budgetFor, normalizeRecipe,
  recipeChunkRange, validateRecipe,
} from '../engine/world/schema.mjs';
import { chunkRangeForBounds, chunkBounds } from '../engine/world/chunk.mjs';
import {
  boundsUnion, resampleStroke, sampleCatmullRom, strokeBounds, pathDiagnostics,
} from '../engine/world/brush.mjs';

const TASK_STATES = Object.freeze([
  'queued', 'validating', 'generating', 'cooking', 'ready', 'committing', 'committed',
  'failed', 'cancelled', 'stale',
]);

function clone(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function nowIso() {
  return new Date().toISOString();
}

/**
 * @param {object} options
 * @param {string} options.root project root
 * @param {string} [options.worker] native cook worker
 * @param {Function} [options.commitScene] async (scenePath, worldRef, expectedRevision) => result
 */
export function createWorldService({ root, worker = DEFAULT_WORKER, commitScene = null } = {}) {
  const projectRoot = path.resolve(root);

  let epoch = 1;
  // `revision` counts commits; `draftSeq` counts previews. They are separate on purpose:
  // a preview must not move the revision the scene is holding, or the conflict check on
  // apply would fail against the very revision the preview was based on.
  let revision = 0;
  let draftSeq = 0;
  let worldId = null;
  let recipe = null;
  let draft = null;          // { recipe, layers: Map<id, layer> }
  let lastCook = null;       // { taskId, manifest, manifestHash, digest, directory }
  let closed = false;

  const tasks = new Map();
  const history = [];
  let historyIndex = -1;
  const locks = new Set();
  const tombstones = new Set();
  const moves = new Map();

  function fail(code, message, details) {
    throw new WorldError(code, message, details);
  }

  function assertOpen() {
    if (closed) fail(WORLD_ERROR_CODES.TASK_STALE, 'the world service has been closed');
    if (!draft) fail(WORLD_ERROR_CODES.TASK_NOT_FOUND, 'no world is open', {
      suggestion: 'Call world_open or world_create first.',
    });
  }

  function snapshot(label) {
    // A snapshot stores the recipe and the layer documents, not the cooked result. Undo is
    // a reference switch: the previous manifest is still on disk and still immutable.
    history.splice(historyIndex + 1);
    history.push({
      label,
      revision,
      draftSeq,
      worldId,
      recipe: clone(draft.recipe),
      layers: [...draft.layers.entries()].map(([id, layer]) => [id, clone(layer)]),
    });
    if (history.length > 64) history.shift();
    historyIndex = history.length - 1;
  }

  function restore(entry) {
    draft = {
      recipe: clone(entry.recipe),
      layers: new Map(entry.layers.map(([id, layer]) => [id, clone(layer)])),
    };
    draftSeq = entry.draftSeq + 1;
  }

  function layerById(id) {
    const layer = draft.layers.get(id);
    if (!layer) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `unknown layer '${id}'`, {
      suggestion: `Available layers: ${[...draft.layers.keys()].join(', ')}`,
    });
    return layer;
  }

  function markDirty() {
    draftSeq += 1;
  }

  // ---------------------------------------------------------------- task helpers

  function createTask(kind, extra = {}) {
    const taskId = randomUUID();
    const task = {
      taskId,
      kind,
      state: 'queued',
      epoch,
      baseRevision: revision,
      draftSeq,
      inputHash: null,
      createdAt: nowIso(),
      updatedAt: nowIso(),
      progress: { stage: 'queued', done: 0, total: null },
      diagnostics: [],
      result: null,
      error: null,
      ...extra,
    };
    tasks.set(taskId, task);
    return task;
  }

  function setTaskState(task, state, stage) {
    if (task.epoch !== epoch) {
      task.state = 'stale';
      return;
    }
    task.state = state;
    task.updatedAt = nowIso();
    task.progress = { ...task.progress, stage: stage ?? state };
  }

  function publicTask(task) {
    if (!task) return null;
    // A task from an older epoch is reported as stale even if its own state says ready: the
    // caller must not be able to act on it just because it finished before the switch.
    const state = task.epoch !== epoch ? 'stale' : task.state;
    return {
      taskId: task.taskId,
      kind: task.kind,
      state,
      epoch: task.epoch,
      baseRevision: task.baseRevision,
      inputHash: task.inputHash,
      progress: task.progress,
      diagnostics: task.diagnostics,
      error: task.error,
      createdAt: task.createdAt,
      updatedAt: task.updatedAt,
      result: task.result,
    };
  }

  // ---------------------------------------------------------------- open / create

  async function openWorld(id) {
    const loaded = await readRecipe(projectRoot, id);
    const layers = new Map();
    for (const reference of loaded.layers) {
      const resolved = await resolveProjectPath(projectRoot, reference.file, { allowMissing: false, mustBeFile: true });
      const layer = JSON.parse(await fs.readFile(resolved.absolute, 'utf8'));
      layers.set(layer.id, layer);
    }
    epoch += 1;
    revision = 0;
    draftSeq = 0;
    worldId = id;
    recipe = loaded;
    draft = { recipe: clone(loaded), layers };
    history.length = 0;
    historyIndex = -1;
    snapshot('open');
    lastCook = null;
    return {
      worldId,
      epoch,
      revision,
      recipe: clone(recipe),
      layers: [...layers.values()].map(layer => ({ ...layer })),
    };
  }

  async function createWorld({ worldId: id, extentMetres = 256, preset = 'mountain-valley', seed = '947210', budgetProfile = 'desktop-balanced' }) {
    if (!/^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$/.test(String(id ?? ''))) {
      fail(WORLD_ERROR_CODES.SCHEMA_INVALID, 'worldId must be 1-64 identifier characters');
    }
    const half = extentMetres / 2;
    const library = defaultLibrary('ancient-kit');
    const libraryRef = await writeLibrary(projectRoot, library);

    const baseLayers = [
      {
        schema: LAYER_SCHEMA, id: 'terrain-base', kind: 'terrain',
        rules: { preset, amplitudeMetres: Math.max(20, extentMetres * 0.06), baseHeightMetres: 4, ridgeStrength: 0.55 },
        regions: [],
      },
      {
        schema: LAYER_SCHEMA, id: 'river-main', kind: 'water',
        rules: { widthMetres: Math.max(10, extentMetres * 0.012), bedDepthMetres: 1.4, flowMetresPerSecond: 1.6 },
        paths: [{
          id: 'river-main', kind: 'river',
          points: [[-half * 0.85, -half * 0.9], [-half * 0.3, -half * 0.45], [0, 0], [half * 0.28, half * 0.42], [half * 0.8, half * 0.85]],
          widthMetres: Math.max(10, extentMetres * 0.012), bedDepthMetres: 1.4,
        }],
      },
      {
        schema: LAYER_SCHEMA, id: 'road-main', kind: 'road',
        rules: { widthMetres: 5, shoulderMetres: 0.8, maxSlopeDegrees: 18 },
        paths: [{
          id: 'road-main', kind: 'road',
          points: [[-half * 0.9, half * 0.08], [-half * 0.3, half * 0.03], [0, 0], [half * 0.35, -half * 0.05], [half * 0.9, -half * 0.12]],
          widthMetres: 5, maxSlopeDegrees: 18,
        }],
      },
      {
        schema: LAYER_SCHEMA, id: 'forest-east', kind: 'scatter',
        rules: { species: 'pine-ancient', densityPerHectare: 140, maxSlopeDegrees: 36, minSpacingMetres: 5, clumping: 0.35, scaleRange: [0.85, 1.25] },
        regions: [{ shape: 'circle', center: [half * 0.35, 0], radiusMetres: half * 0.6, falloffMetres: half * 0.18, strength: 1 }],
      },
      {
        schema: LAYER_SCHEMA, id: 'meadow', kind: 'scatter',
        rules: { species: 'grass-tuft', densityPerHectare: 9000, maxSlopeDegrees: 34, alignToSlope: true, scaleRange: [0.8, 1.3] },
        regions: [],
      },
      {
        schema: LAYER_SCHEMA, id: 'boulders', kind: 'scatter',
        rules: { species: 'granite-boulder', densityPerHectare: 26, maxSlopeDegrees: 46, minSpacingMetres: 4, scaleRange: [0.7, 1.6] },
        regions: [],
      },
      {
        schema: LAYER_SCHEMA, id: 'village', kind: 'settlement',
        rules: {
          components: ['courtyard-house', 'two-storey-inn'], density: 0.72, lotSizeMetres: 22, setbackMetres: 2.5,
          roadWidthMetres: 5, maxBuildings: 48, entranceClearanceMetres: 3.5,
        },
        regions: [{ shape: 'circle', center: [0, 0], radiusMetres: Math.max(60, extentMetres * 0.22) }],
      },
    ];

    const layerRefs = [];
    for (const layer of baseLayers) layerRefs.push(await writeLayer(projectRoot, layer));

    const created = normalizeRecipe({
      schema: RECIPE_SCHEMA,
      worldId: id,
      generatorVersion: GENERATOR_VERSION,
      seed: String(seed),
      extentMetres: [extentMetres, extentMetres],
      originMetres: [-half, 0, -half],
      chunkSizeMetres: 128,
      terrain: { sampleStepMetres: 1, preset, amplitudeMetres: Math.max(20, extentMetres * 0.06), baseHeightMetres: 4 },
      libraries: [libraryRef],
      layers: layerRefs,
      overrides: [],
      budgetProfile,
    });
    const errors = validateRecipe(created);
    if (errors.length) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `the new world is invalid: ${errors[0]}`, { cause: errors });

    const resolved = await resolveProjectPath(projectRoot, `${WORLD_LAYOUT.worldDir}/${id}.world.json`);
    await fs.mkdir(path.dirname(resolved.absolute), { recursive: true });
    await fs.writeFile(resolved.absolute, JSON.stringify(created, null, 2), 'utf8');

    return openWorld(id);
  }

  // ---------------------------------------------------------------- edits

  function applyStroke(stroke) {
    const layer = layerById(stroke.layerId);
    const radius = Number(stroke.radiusMetres ?? 8);
    if (!(radius > 0) || radius > 8192) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, 'brush radius must be in (0, 8192] metres');
    const spacing = Math.max(0.5, radius * 0.35);
    const points = Array.isArray(stroke.points) ? stroke.points : [];
    if (points.length === 0) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, 'a stroke needs at least one point');

    // The stamp count follows world distance, never the number of pointer events.
    const stamps = stroke.shape === 'path'
      ? sampleCatmullRom(points, spacing, { closed: stroke.closed === true })
      : resampleStroke(points, spacing);

    if (layer.kind === 'terrain') {
      for (const stamp of stamps) {
        layer.regions = layer.regions ?? [];
        if (layer.regions.length >= 4096) break;
        layer.regions.push({
          shape: 'circle',
          center: [stamp.x, stamp.z],
          radiusMetres: radius,
          falloffMetres: stroke.falloffMetres ?? radius * 0.5,
          strength: (stroke.strength ?? 1) * (stroke.tone ?? 1),
          erase: stroke.erase === true,
        });
      }
      if (stroke.mode) layer.rules = { ...(layer.rules ?? {}), brush: stroke.mode };
      return { stamps: stamps.length, layerId: layer.id };
    }

    if (layer.kind === 'scatter' || layer.kind === 'decor') {
      for (const stamp of stamps) {
        layer.regions = layer.regions ?? [];
        if (layer.regions.length >= 4096) break;
        layer.regions.push({
          shape: 'circle',
          center: [stamp.x, stamp.z],
          radiusMetres: radius,
          falloffMetres: stroke.falloffMetres ?? radius * 0.6,
          strength: stroke.strength ?? 1,
          erase: stroke.erase === true,
        });
      }
      return { stamps: stamps.length, layerId: layer.id };
    }

    if (layer.kind === 'settlement') {
      layer.regions = layer.regions ?? [];
      layer.regions.push({
        shape: 'circle',
        center: [points[0][0], points[0][1]],
        radiusMetres: radius,
      });
      return { stamps: 1, layerId: layer.id };
    }

    if (layer.kind === 'water' || layer.kind === 'road') {
      // A path stroke replaces the control polygon of a new path rather than stamping
      // circles: a river drawn as a chain of discs is not a river.
      layer.paths = layer.paths ?? [];
      if (layer.paths.length >= 512) fail(WORLD_ERROR_CODES.BUDGET_EXCEEDED, 'this layer already has the maximum number of paths');
      const id = stroke.pathId ?? `${layer.id}-${layer.paths.length + 1}`;
      layer.paths.push({
        id,
        kind: layer.kind === 'water' ? 'river' : 'road',
        points: points.map(point => [Number(point[0]), Number(point[1])]),
        widthMetres: stroke.widthMetres ?? layer.rules?.widthMetres ?? (layer.kind === 'water' ? 12 : 4),
        bedDepthMetres: layer.rules?.bedDepthMetres ?? 1,
      });
      return { stamps: 1, layerId: layer.id, pathId: id, diagnostics: pathDiagnostics(layer.paths[layer.paths.length - 1]) };
    }

    fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `layer '${layer.id}' does not accept brush strokes`);
    return null;
  }

  function applyPathEdit(edit) {
    const layer = layerById(edit.layerId);
    if (layer.kind !== 'water' && layer.kind !== 'road') {
      fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `layer '${layer.id}' does not hold paths`);
    }
    layer.paths = layer.paths ?? [];
    if (edit.remove) {
      layer.paths = layer.paths.filter(entry => entry.id !== edit.pathId);
      return { removed: edit.pathId };
    }
    let entry = layer.paths.find(candidate => candidate.id === edit.pathId);
    if (!entry) {
      entry = { id: edit.pathId, kind: layer.kind === 'water' ? 'river' : 'road', points: [] };
      layer.paths.push(entry);
    }
    if (Array.isArray(edit.points)) entry.points = edit.points.map(point => [Number(point[0]), Number(point[1])]);
    if (Number.isFinite(edit.widthMetres)) entry.widthMetres = edit.widthMetres;
    if (Number.isFinite(edit.bedDepthMetres)) entry.bedDepthMetres = edit.bedDepthMetres;
    return { path: entry, diagnostics: pathDiagnostics(entry) };
  }

  function setLayerFlag(id, key, value) {
    const layer = layerById(id);
    if (!['visible', 'locked'].includes(key)) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `'${key}' is not a layer flag`);
    layer[key] = value === true;
    return { layerId: id, [key]: layer[key] };
  }

  function deleteObjects(layerId, objectIds) {
    const layer = layerById(layerId);
    layer.deletedObjects = layer.deletedObjects ?? [];
    for (const id of objectIds) {
      if (typeof id !== 'string' || id.length === 0 || id.length > 256) continue;
      if (!layer.deletedObjects.includes(id)) layer.deletedObjects.push(id);
      tombstones.add(id);
    }
    return { deleted: layer.deletedObjects.length };
  }

  function moveObjects(layerId, entries) {
    const layer = layerById(layerId);
    layer.movedObjects = layer.movedObjects ?? [];
    for (const entry of entries) {
      if (!entry || typeof entry.id !== 'string') continue;
      const record = { id: entry.id };
      if (Array.isArray(entry.position)) record.position = entry.position.map(Number);
      if (Number.isFinite(entry.yaw)) record.yaw = Number(entry.yaw);
      if (Number.isFinite(entry.scale)) record.scale = Number(entry.scale);
      const index = layer.movedObjects.findIndex(candidate => candidate.id === record.id);
      if (index >= 0) layer.movedObjects[index] = record;
      else layer.movedObjects.push(record);
      moves.set(record.id, record);
    }
    return { moved: layer.movedObjects.length };
  }

  // ---------------------------------------------------------------- preview

  async function preview(options = {}) {
    assertOpen();
    const task = createTask('preview');
    setTaskState(task, 'validating', 'validating');

    try {
      if (Array.isArray(options.strokes)) {
        if (options.strokes.length > 4096) fail(WORLD_ERROR_CODES.BUDGET_EXCEEDED, 'a preview accepts at most 4096 strokes');
        for (const stroke of options.strokes) applyStroke(stroke);
      }
      if (Array.isArray(options.paths)) for (const edit of options.paths) applyPathEdit(edit);
      if (Array.isArray(options.layers)) {
        for (const change of options.layers) {
          if (change.visible !== undefined) setLayerFlag(change.id, 'visible', change.visible);
          if (change.locked !== undefined) setLayerFlag(change.id, 'locked', change.locked);
        }
      }
      if (Array.isArray(options.delete)) for (const entry of options.delete) deleteObjects(entry.layerId, entry.objectIds ?? []);
      if (Array.isArray(options.move)) for (const entry of options.move) moveObjects(entry.layerId, entry.objects ?? []);
      if (options.recipe) {
        const merged = normalizeRecipe({ ...draft.recipe, ...options.recipe });
        const errors = validateRecipe(merged);
        if (errors.length) fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `recipe edit is invalid: ${errors[0]}`, { cause: errors });
        draft.recipe = merged;
      }
      markDirty();
      snapshot(options.label ?? 'preview');

      // The draft is staged, not saved. A preview that wrote the layer files would leave a
      // cancelled stroke on disk, and "preview does not change the world" has to be true of
      // the files as well as of the scene.
      const cacheDir = path.join(projectRoot, WORLD_LAYOUT.cache, task.taskId);
      const stageDir = path.join(cacheDir, 'layers');
      await fs.mkdir(stageDir, { recursive: true });
      const stagedRecipe = clone(draft.recipe);
      stagedRecipe.layers = [];
      for (const layer of draft.layers.values()) {
        const file = `${WORLD_LAYOUT.cache}/${task.taskId}/layers/${layer.id}.json`;
        const absolute = path.join(projectRoot, file);
        const text = JSON.stringify({ ...layer, schema: LAYER_SCHEMA }, null, 2);
        await fs.writeFile(absolute, text, 'utf8');
        stagedRecipe.layers.push({ id: layer.id, file, sha256: sha256Hex(Buffer.from(text, 'utf8')) });
      }

      setTaskState(task, 'generating', 'generating');
      const resolved = await resolveWorld(projectRoot, stagedRecipe);
      task.inputHash = resolved.digest;

      await fs.mkdir(cacheDir, { recursive: true });
      const inputPath = path.join(cacheDir, 'input.json');
      await fs.writeFile(inputPath, JSON.stringify(resolved.compileInput, null, 2), 'utf8');

      setTaskState(task, 'cooking', 'cooking');
      const { runWorker } = await import('../engine/world/cook.mjs');
      const summary = await runWorker({
        worker, inputPath, outputDir: cacheDir, inputDigest: resolved.digest,
      });
      if (task.epoch !== epoch) {
        task.state = 'stale';
        return publicTask(task);
      }

      const partialPath = path.join(cacheDir, 'manifest.partial.json');
      const partial = JSON.parse(await fs.readFile(partialPath, 'utf8'));
      partial.schema = 'nativegpu.world-manifest/1';
      partial.inputDigest = resolved.digest;
      const { manifestHashOf } = await import('../engine/world/manifest.mjs');
      const manifestHash = manifestHashOf(partial);
      const manifest = { ...partial, manifestHash };

      task.result = {
        manifestHash,
        inputHash: resolved.digest,
        directory: path.relative(projectRoot, cacheDir).split(path.sep).join('/'),
        chunks: manifest.chunks.length,
        chunkRange: manifest.chunkRange,
        instances: manifest.budget.totalInstances,
        buildings: manifest.stats?.buildings ?? 0,
        cookMillis: Math.round(manifest.budget.cookMillis),
        warnings: summary.warnings ?? [],
        budgetScale: summary.budgetScale ?? 1,
      };
      task.diagnostics = [
        ...(summary.pathDiagnostics ?? []).map(entry => diagnostic('WORLD_CONSTRAINT_UNSATISFIED', entry.message, {
          layerId: entry.pathId, stage: 'paths', taskId: task.taskId,
        })),
        ...(summary.settlementDiagnostics ?? []).map(entry => diagnostic('WORLD_CONSTRAINT_UNSATISFIED', entry.message, {
          stage: 'settlement', taskId: task.taskId, chunk: { x: entry.x, z: entry.z },
        })),
        ...(summary.warnings ?? []).map(message => diagnostic('WORLD_BUDGET_EXCEEDED', message, {
          stage: 'budget', taskId: task.taskId,
        })),
      ];
      setTaskState(task, 'ready', 'ready');
      return publicTask(task);
    } catch (error) {
      task.error = { code: error.code ?? 'WORLD_TASK_FAILED', message: error.message, details: error.details ?? null };
      task.state = task.epoch !== epoch ? 'stale' : 'failed';
      task.updatedAt = nowIso();
      return publicTask(task);
    }
  }

  // ---------------------------------------------------------------- apply

  async function publish(task) {
    const cacheDir = path.join(projectRoot, task.result.directory);
    const partial = JSON.parse(await fs.readFile(path.join(cacheDir, 'manifest.partial.json'), 'utf8'));
    partial.schema = 'nativegpu.world-manifest/1';
    partial.inputDigest = task.result.inputHash;
    const { manifestHashOf } = await import('../engine/world/manifest.mjs');
    const manifestHash = manifestHashOf(partial);
    const manifest = { ...partial, manifestHash };

    const relative = cookedPath(manifestHash);
    const target = path.join(projectRoot, relative);
    await fs.mkdir(target, { recursive: true });
    await fs.cp(path.join(cacheDir, 'chunks'), path.join(target, 'chunks'), { recursive: true });
    await fs.writeFile(path.join(target, 'manifest.json'), JSON.stringify(manifest, null, 2), 'utf8');
    const manifestBytes = await fs.readFile(path.join(target, 'manifest.json'));
    return {
      manifest,
      manifestHash,
      // `relative` is project-relative (for the viewport host and the CLI); `assetsRelative`
      // is what the scene stores, because every other scene dependency is relative to the
      // project's assets directory and a second convention would be a bug waiting to happen.
      relative,
      assetsRelative: relative.replace(/^assets\//, ''),
      manifestSha256: sha256Hex(manifestBytes),
    };
  }

  async function apply({ taskId, expectedRevision, scenePath = null, sceneRevision = null, allowStale = false } = {}) {
    assertOpen();
    const task = tasks.get(taskId);
    if (!task) fail(WORLD_ERROR_CODES.TASK_NOT_FOUND, `unknown task '${taskId}'`);
    if (task.epoch !== epoch) {
      fail(WORLD_ERROR_CODES.TASK_STALE, 'this task belongs to an earlier world session', {
        suggestion: 'Run world_preview again in the current session.',
      });
    }
    if (task.state !== 'ready' && !(allowStale && task.result)) {
      fail(WORLD_ERROR_CODES.CONSTRAINT_UNSATISFIED, `task ${taskId} is ${task.state}, not ready`, {
        cause: task.error,
      });
    }
    if (Number.isSafeInteger(expectedRevision) && expectedRevision !== revision) {
      fail(WORLD_ERROR_CODES.REVISION_CONFLICT,
        `expected revision ${expectedRevision}, current revision is ${revision}`, {
          suggestion: 'Preview again from the current revision; the draft is preserved.',
        });
    }

    task.state = 'committing';
    task.updatedAt = nowIso();

    // Commit the draft to the project: layer files first, then the recipe, then the cooked
    // manifest. A crash between the two leaves unreferenced layer files, which the next
    // preview overwrites; it never leaves a scene pointing at a manifest that is not there.
    const layerRefs = [];
    for (const layer of draft.layers.values()) layerRefs.push(await writeLayer(projectRoot, layer));
    draft.recipe.layers = layerRefs;
    const recipePath = await resolveProjectPath(projectRoot, `${WORLD_LAYOUT.worldDir}/${draft.recipe.worldId}.world.json`);
    await fs.mkdir(path.dirname(recipePath.absolute), { recursive: true });
    const recipeText = JSON.stringify(draft.recipe, null, 2);
    await fs.writeFile(recipePath.absolute, recipeText, 'utf8');
    const recipeSha = sha256Hex(Buffer.from(recipeText, 'utf8'));

    const published = await publish(task);

    let sceneResult = null;
    if (scenePath && typeof commitScene === 'function') {
      const worldRef = {
        worldId: draft.recipe.worldId,
        generatorVersion: draft.recipe.generatorVersion,
        manifestHash: published.manifestHash,
        inputDigest: task.result.inputHash,
        cookedManifest: {
          file: `${published.assetsRelative}/manifest.json`,
          sha256: published.manifestSha256,
        },
        recipe: {
          file: `${WORLD_LAYOUT.worldDir}/${draft.recipe.worldId}.world.json`,
          sha256: recipeSha,
        },
      };
      // `expectedRevision` guards the *world* draft; the scene has its own revision and
      // its own history. Passing the world revision to the scene transaction would fail on
      // every second apply, because the two counters advance independently. A caller that
      // wants the stricter check passes `sceneRevision` from scene_validate.
      sceneResult = await commitScene(scenePath, worldRef,
        Number.isSafeInteger(sceneRevision) ? sceneRevision : null);
    }

    revision += 1;
    task.state = 'committed';
    task.result = {
      ...task.result,
      manifestHash: published.manifestHash,
      publishedTo: published.relative,
    };
    task.updatedAt = nowIso();
    lastCook = {
      taskId, manifest: published.manifest, manifestHash: published.manifestHash,
      digest: task.result.inputHash, directory: published.relative,
    };
    return {
      task: publicTask(task),
      world: {
        worldId: draft.recipe.worldId,
        generatorVersion: draft.recipe.generatorVersion,
        manifestHash: published.manifestHash,
        inputDigest: task.result.inputHash,
        // `directory` is project-relative and is what the native viewport is told to open.
        directory: published.relative,
        cookedManifest: { file: `${published.assetsRelative}/manifest.json`, sha256: published.manifestSha256 },
      },
      scene: sceneResult,
      revision,
    };
  }

  // ---------------------------------------------------------------- query / locks

  function query({ region = null } = {}) {
    assertOpen();
    const chunkSize = draft.recipe.chunkSizeMetres;
    const range = region
      ? chunkRangeForBounds(region, chunkSize)
      : recipeChunkRange(draft.recipe);
    const chunks = [];
    for (let cz = range.minCz; cz <= range.maxCz; cz += 1) {
      for (let cx = range.minCx; cx <= range.maxCx; cx += 1) {
        chunks.push({ cx, cz, ...chunkBounds(cx, cz, chunkSize) });
      }
    }
    const layers = [...draft.layers.values()].map(layer => ({
      id: layer.id,
      kind: layer.kind,
      visible: layer.visible !== false,
      locked: layer.locked === true,
      paths: layer.paths?.length ?? 0,
      regions: layer.regions?.length ?? 0,
      deleted: layer.deletedObjects?.length ?? 0,
      moved: layer.movedObjects?.length ?? 0,
    }));
    return {
      worldId: draft.recipe.worldId,
      revision,
      draftSeq,
      epoch,
      extentMetres: draft.recipe.extentMetres,
      originMetres: draft.recipe.originMetres,
      chunkSizeMetres: chunkSize,
      chunkRange: range,
      chunkCount: chunks.length,
      chunks,
      layers,
      budget: budgetFor(draft.recipe.budgetProfile),
      locks: [...locks],
      tombstones: tombstones.size,
      lastCook: lastCook ? {
        manifestHash: lastCook.manifestHash,
        directory: lastCook.directory,
        chunks: lastCook.manifest.chunks.length,
        instances: lastCook.manifest.budget.totalInstances,
      } : null,
    };
  }

  function lock({ region = null, objectIds = [], unlock = false } = {}) {
    assertOpen();
    if (region) {
      const key = [region.minX, region.minZ, region.maxX, region.maxZ].map(value => Number(value).toFixed(3)).join(',');
      if (unlock) locks.delete(key);
      else locks.add(key);
      draft.recipe.locks = [...locks].map(text => {
        const [minX, minZ, maxX, maxZ] = text.split(',').map(Number);
        return { minX, minZ, maxX, maxZ };
      });
      markDirty();
      return { locks: [...locks] };
    }
    if (objectIds.length) {
      for (const id of objectIds) {
        if (unlock) tombstones.delete(id);
        else tombstones.add(id);
      }
      markDirty();
      return { lockedObjects: [...tombstones] };
    }
    return { locks: [...locks], lockedObjects: [...tombstones] };
  }

  // ---------------------------------------------------------------- history

  function undo() {
    assertOpen();
    if (historyIndex <= 0) return { ok: false, reason: 'nothing to undo', revision };
    historyIndex -= 1;
    restore(history[historyIndex]);
    return { ok: true, revision, label: history[historyIndex].label };
  }

  function redo() {
    assertOpen();
    if (historyIndex >= history.length - 1) return { ok: false, reason: 'nothing to redo', revision };
    historyIndex += 1;
    restore(history[historyIndex]);
    return { ok: true, revision, label: history[historyIndex].label };
  }

  // ---------------------------------------------------------------- listing

  async function listWorlds() {
    const directory = path.join(projectRoot, WORLD_LAYOUT.worldDir);
    let entries = [];
    try {
      entries = await fs.readdir(directory, { withFileTypes: true });
    } catch {
      return { worlds: [] };
    }
    const worlds = [];
    for (const entry of entries) {
      if (!entry.isFile() || !entry.name.endsWith('.world.json')) continue;
      try {
        const recipe = JSON.parse(await fs.readFile(path.join(directory, entry.name), 'utf8'));
        worlds.push({
          worldId: recipe.worldId,
          file: `${WORLD_LAYOUT.worldDir}/${entry.name}`,
          extentMetres: recipe.extentMetres,
          generatorVersion: recipe.generatorVersion,
          layers: recipe.layers?.length ?? 0,
        });
      } catch {
        worlds.push({ worldId: entry.name.replace(/\.world\.json$/, ''), file: `${WORLD_LAYOUT.worldDir}/${entry.name}`, broken: true });
      }
    }
    return { worlds };
  }

  async function inspectLibrary() {
    const libraries = [];
    for (const reference of draft?.recipe?.libraries ?? []) {
      const resolved = await resolveProjectPath(projectRoot, reference.file, { allowMissing: false, mustBeFile: true })
        .catch(() => null);
      if (!resolved) {
        libraries.push({ id: reference.id, missing: true });
        continue;
      }
      const library = JSON.parse(await fs.readFile(resolved.absolute, 'utf8'));
      libraries.push({ ...summarizeLibrary(library), file: reference.file });
    }
    return { libraries };
  }

  function capabilities() {
    return {
      available: true,
      generatorVersion: GENERATOR_VERSION,
      worker,
      chunkFormats: ['NGRC/1'],
      terrainPresets: ['flat', 'mountain-valley', 'rolling-hills', 'ridge-basin', 'river-plain'],
      budgetProfiles: Object.keys(BUDGETS),
      limits: {
        extentMetres: [16, 8192],
        chunkSizeMetres: [16, 512],
        sampleStepMetres: [0.25, 8],
      },
      // Honest capability labels. The plan is explicit that a stand-in shell must not be
      // presented as a finished building.
      buildingInteriors: 'not-implemented',
      notes: [
        'Buildings are stand-in shells with reserved footprints and entrances.',
        'Rivers carry a static surface; full fluid simulation is a separate feature.',
        'The native viewport draws terrain, roads, rivers and instanced components.',
      ],
    };
  }

  function close() {
    closed = true;
    epoch += 1;
    draft = null;
    tasks.clear();
  }

  async function call(action, args = {}) {
    if (closed) fail(WORLD_ERROR_CODES.TASK_STALE, 'the world service has been closed');
    switch (action) {
      case 'world_capabilities':
        return { ok: true, capabilities: capabilities() };
      case 'world_list':
        return { ok: true, ...(await listWorlds()) };
      case 'world_open':
        return { ok: true, world: await openWorld(args.worldId) };
      case 'world_create':
        return { ok: true, world: await createWorld(args) };
      case 'world_library_inspect':
        return { ok: true, ...(await inspectLibrary()) };
      case 'world_preview':
        return { ok: true, task: await preview(args) };
      case 'world_job_status': {
        const task = tasks.get(args.taskId);
        if (!task) fail(WORLD_ERROR_CODES.TASK_NOT_FOUND, `unknown task '${args.taskId}'`);
        return { ok: true, task: publicTask(task) };
      }
      case 'world_cancel': {
        const task = tasks.get(args.taskId);
        if (!task) fail(WORLD_ERROR_CODES.TASK_NOT_FOUND, `unknown task '${args.taskId}'`);
        if (['committed', 'failed', 'cancelled'].includes(task.state)) {
          return { ok: true, task: publicTask(task) };
        }
        task.state = 'cancelled';
        task.updatedAt = nowIso();
        return { ok: true, task: publicTask(task) };
      }
      case 'world_apply':
        return { ok: true, ...(await apply(args)) };
      case 'world_query':
        return { ok: true, ...query(args) };
      case 'world_regenerate': {
        // A regeneration is a preview with an explicit scope. The scope is reported back so
        // the caller can see the dependency closure rather than assume "only this brush".
        const task = await preview({ ...args, label: 'regenerate' });
        return { ok: true, task, scope: task.result?.chunkRange ?? null };
      }
      case 'world_lock':
        return { ok: true, ...lock(args) };
      case 'world_undo':
        return { ok: true, ...undo() };
      case 'world_redo':
        return { ok: true, ...redo() };
      case 'world_build': {
        const task = tasks.get(args.taskId) ?? null;
        if (task && task.result) {
          const published = await publish(task);
          return { ok: true, manifestHash: published.manifestHash, directory: published.relative };
        }
        return { ok: true, ...(await preview(args)) };
      }
      case 'world_release': {
        if (!lastCook) fail(WORLD_ERROR_CODES.TASK_NOT_FOUND, 'nothing has been cooked yet');
        const directory = path.join(projectRoot, lastCook.directory);
        const manifest = await readManifest(directory);
        const closure = await releaseClosure(directory, manifest);
        return { ok: true, chunks: closure.chunks, bytes: closure.bytes };
      }
      default:
        fail(WORLD_ERROR_CODES.SCHEMA_INVALID, `unknown world action '${action}'`);
        return null;
    }
  }

  return {
    handles(action) {
      return typeof action === 'string' && action.startsWith('world_');
    },
    call,
    close,
    get epoch() { return epoch; },
    get revision() { return revision; },
    get worldId() { return worldId; },
  };
}

export { TASK_STATES };

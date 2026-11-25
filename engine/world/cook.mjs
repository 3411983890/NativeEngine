// World cook pipeline.
//
// One function owns the whole path from "a recipe on disk" to "a directory of immutable
// chunk files plus a manifest", so the editor service, the CLI and the tests cannot drift:
//
//   recipe + libraries + layers + overrides
//        -> resolved compile request (JSON, one file)
//        -> native worker (world_cook.exe)
//        -> chunks/*.ngrc + manifest.partial.json
//        -> manifest.json (hash computed here, not in C++)
//
// Two rules are load-bearing:
//
//   * Every referenced file is hashed and the hash is compared with the one in the recipe.
//     A library that changed under a cooked world must not silently produce a different
//     world under the same input digest.
//   * The input digest is computed from the *resolved* request, not from the recipe. Two
//     recipes that reference the same content therefore share a cooked directory, and a
//     layer edit invalidates exactly the cooks that used it.

import fs from 'node:fs/promises';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

import { WORLD_ERROR_CODES, WorldError } from './errors.mjs';
import { canonicalize, inputDigest, isSha256, sha256Hex, sha256Canonical } from './hash.mjs';
import { manifestHashOf, validateManifest } from './manifest.mjs';
import {
  LAYER_SCHEMA, MANIFEST_SCHEMA, assertLayer, assertLibrary, assertOverrides, assertRecipe,
  budgetFor, normalizeRecipe, recipeChunkRange,
} from './schema.mjs';
import {
  WORLD_LAYOUT, cookedPath, layerPath, libraryPath, overridePath, resolveProjectPath,
} from './paths.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = path.resolve(here, '..', '..');
export const DEFAULT_WORKER = path.join(PROJECT_ROOT, 'build', 'world-host', 'world_cook.exe');

async function readJson(absolute, what) {
  let text;
  try {
    text = await fs.readFile(absolute, 'utf8');
  } catch (error) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `${what} cannot be read: ${error.code ?? error.message}`, {
      stage: 'resolve',
      suggestion: `Check that the file exists at ${absolute}.`,
    });
  }
  try {
    return JSON.parse(text);
  } catch (error) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `${what} is not valid JSON: ${error.message}`, { stage: 'resolve' });
  }
}

/**
 * Reads a file, verifies its SHA-256 against the reference and returns both.
 * The hash is computed over the exact bytes that were parsed, so a file cannot change
 * between the check and the use.
 */
async function readVerified(projectRoot, reference, what) {
  const resolved = await resolveProjectPath(projectRoot, reference.file, { allowMissing: false, mustBeFile: true });
  const bytes = await fs.readFile(resolved.absolute);
  const hash = sha256Hex(bytes);
  if (!isSha256(reference.sha256)) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `${what} reference has no valid SHA-256`, { stage: 'resolve' });
  }
  if (hash !== reference.sha256) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID,
      `${what} at ${reference.file} does not match its recorded hash`, {
        stage: 'resolve',
        cause: { expected: reference.sha256, actual: hash },
        suggestion: 'Re-import or re-save the file, then update the recipe reference.',
      });
  }
  let value;
  try {
    value = JSON.parse(bytes.toString('utf8'));
  } catch (error) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `${what} at ${reference.file} is not valid JSON: ${error.message}`, { stage: 'resolve' });
  }
  return { value, hash, bytes };
}

function layerToCompile(layer) {
  const rules = layer.rules ?? {};
  const result = { id: layer.id, kind: layer.kind, visible: layer.visible !== false, locked: layer.locked === true };

  if (layer.kind === 'terrain') {
    result.edits = (layer.regions ?? []).map(region => ({
      mode: rules.brush ?? 'raise',
      shape: region.shape,
      center: region.center,
      radiusMetres: region.radiusMetres,
      bounds: region.bounds ? [region.bounds.minX, region.bounds.minZ, region.bounds.maxX, region.bounds.maxZ] : undefined,
      points: region.points,
      falloffMetres: region.falloffMetres ?? 0,
      strength: region.strength ?? 1,
      deltaMetres: rules.amplitudeMetres ? rules.amplitudeMetres / 8 : 1,
      targetHeight: rules.baseHeightMetres ?? 0,
      erase: region.erase === true,
    }));
  }

  if (layer.kind === 'water' || layer.kind === 'road') {
    result.paths = (layer.paths ?? []).map(entry => ({
      id: entry.id,
      kind: layer.kind === 'water' ? 'river' : 'road',
      points: entry.points,
      widthMetres: entry.widthMetres ?? rules.widthMetres ?? (layer.kind === 'water' ? 12 : 4),
      bedDepthMetres: entry.bedDepthMetres ?? rules.bedDepthMetres ?? 1,
      shoulderMetres: rules.shoulderMetres ?? 0.6,
      maxSlopeDegrees: rules.maxSlopeDegrees ?? 20,
      flowMetresPerSecond: rules.flowMetresPerSecond ?? 1,
      closed: entry.closed === true,
      bridge: entry.bridge !== false,
      visible: layer.visible !== false,
    }));
  }

  if (layer.kind === 'scatter' || layer.kind === 'decor') {
    // The exclusion category comes from the component, not from the layer: a rule that
    // names a tree component must be blocked by tree exclusions even if the authoring layer
    // forgot to say "category": "tree".
    result.scatter = {
      layerId: layer.id,
      component: rules.species,
      densityPerHectare: rules.densityPerHectare ?? 100,
      maxSlopeDegrees: rules.maxSlopeDegrees ?? 35,
      minSpacingMetres: rules.minSpacingMetres ?? 0,
      exclusionRadiusMetres: rules.exclusionRadiusMetres ?? 0,
      edgeFalloffMetres: rules.edgeFalloffMetres ?? 0,
      clumping: rules.clumping ?? 0,
      scaleMin: rules.scaleRange?.[0] ?? 0.9,
      scaleMax: rules.scaleRange?.[1] ?? 1.1,
      alignToSlope: rules.alignToSlope === true,
      avoidWater: true,
      avoidRoads: true,
    };
    result.regions = (layer.regions ?? []).map(region => ({
      shape: region.shape,
      center: region.center,
      radiusMetres: region.radiusMetres,
      bounds: region.bounds ? [region.bounds.minX, region.bounds.minZ, region.bounds.maxX, region.bounds.maxZ] : undefined,
      points: region.points,
      falloffMetres: region.falloffMetres ?? 0,
      strength: region.strength ?? 1,
      erase: region.erase === true,
    }));
  }

  if (layer.kind === 'settlement') {
    result.settlement = {
      layerId: layer.id,
      components: Array.isArray(rules.components) ? rules.components : (rules.species ? [rules.species] : []),
      density: rules.density ?? 0.7,
      lotSizeMetres: rules.lotSizeMetres ?? 20,
      setbackMetres: rules.setbackMetres ?? 2.5,
      roadWidthMetres: rules.roadWidthMetres ?? 5,
      entranceClearanceMetres: rules.entranceClearanceMetres ?? 3,
      maxSlopeDegrees: rules.maxSlopeDegrees ?? 8,
      buildingSpacingMetres: rules.buildingSpacingMetres ?? 4,
      maxBuildings: rules.maxBuildings ?? 256,
      scaleMin: rules.scaleRange?.[0] ?? 0.92,
      scaleMax: rules.scaleRange?.[1] ?? 1.08,
    };
    result.settlementRegions = (layer.regions ?? []).map(region => ({
      shape: region.shape,
      center: region.center,
      radiusMetres: region.radiusMetres,
      bounds: region.bounds ? [region.bounds.minX, region.bounds.minZ, region.bounds.maxX, region.bounds.maxZ] : undefined,
      points: region.points,
    }));
  }

  result.deleted = layer.deletedObjects ?? [];
  result.moved = layer.movedObjects ?? [];
  result.lockedObjects = layer.lockedObjects ?? [];
  return result;
}

function componentToCompile(component) {
  const geometry = component.geometry ?? {};
  return {
    id: component.id,
    category: component.category,
    shape: geometry.shape ?? 'box',
    sizeMetres: component.sizeMetres,
    albedo: component.material?.albedo ?? [0.7, 0.7, 0.7],
    roughness: component.material?.roughness ?? 0.85,
    metalness: component.material?.metalness ?? 0,
    collision: component.collision ? component.collision.mode !== 'none' : false,
    collisionHalfExtents: component.collision?.halfExtentsMetres ?? [
      component.sizeMetres[0] / 2, component.sizeMetres[1] / 2, component.sizeMetres[2] / 2,
    ],
  };
}

/**
 * Resolves a recipe and every file it references into one self-contained compile request.
 * This is the only place that knows how a layer kind maps onto compiler fields.
 */
export async function resolveWorld(projectRoot, recipeInput, { overrides = [] } = {}) {
  const recipe = normalizeRecipe(recipeInput);
  assertRecipe(recipe);

  const components = [];
  const componentCategories = new Map();
  const libraryHashes = [];
  for (const reference of recipe.libraries) {
    const { value, hash } = await readVerified(projectRoot, reference, `world library '${reference.id}'`);
    assertLibrary(value);
    libraryHashes.push(`${reference.id}:${hash}`);
    for (const component of value.components) {
      components.push(componentToCompile(component));
      componentCategories.set(component.id, component.category);
    }
  }

  const paths = [];
  const terrainEdits = [];
  const scatter = [];
  const densityRegions = [];
  const settlements = [];
  const settlementRegions = [];
  const deleted = [];
  const moved = [];
  const locks = [...(recipe.locks ?? [])];
  const layerHashes = [];

  for (const reference of recipe.layers) {
    const { value, hash } = await readVerified(projectRoot, reference, `world layer '${reference.id}'`);
    assertLayer(value);
    if (value.id !== reference.id) {
      throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID,
        `layer file ${reference.file} declares id '${value.id}' but the recipe calls it '${reference.id}'`, { stage: 'resolve' });
    }
    layerHashes.push(`${reference.id}:${hash}`);
    if (value.visible === false) continue;
    const compiled = layerToCompile(value);
    if (compiled.paths) paths.push(...compiled.paths);
    if (compiled.edits) terrainEdits.push(...compiled.edits);
    if (compiled.scatter) {
      const category = componentCategories.get(compiled.scatter.component) ?? 'grass';
      scatter.push({ ...compiled.scatter, category: category === 'rock' ? 'rock' : (category === 'tree' ? 'tree' : 'grass') });
    }
    if (compiled.regions) densityRegions.push(...compiled.regions);
    if (compiled.settlement) settlements.push(compiled.settlement);
    if (compiled.settlementRegions) settlementRegions.push(...compiled.settlementRegions);
    deleted.push(...compiled.deleted);
    moved.push(...compiled.moved);
    if (value.locked === true) {
      locks.push({ minX: -100000, minZ: -100000, maxX: 100000, maxZ: 100000, layerId: value.id });
    }
  }

  // Override files are region-scoped hand edits. They are merged into the same three lists
  // the layers write to, so the compiler has one place to look.
  const overrideDigests = [];
  for (const reference of [...recipe.overrides, ...overrides]) {
    if (!reference?.file) continue;
    const { value, hash } = await readVerified(projectRoot, reference, 'world override');
    assertOverrides(value);
    overrideDigests.push(hash);
    deleted.push(...(value.deleted ?? []));
    moved.push(...(value.moved ?? []));
    if (value.region) locks.push(value.region);
  }

  const compileInput = {
    schema: 'nativegpu.world-compile/1',
    worldId: recipe.worldId,
    generatorVersion: recipe.generatorVersion,
    seed: recipe.seed,
    extentMetres: recipe.extentMetres,
    originMetres: recipe.originMetres,
    chunkSizeMetres: recipe.chunkSizeMetres,
    sampleStepMetres: recipe.terrain.sampleStepMetres,
    budgetProfile: recipe.budgetProfile,
    terrain: {
      preset: recipe.terrain.preset,
      amplitudeMetres: recipe.terrain.amplitudeMetres,
      baseHeightMetres: recipe.terrain.baseHeightMetres,
      ridgeStrength: recipe.terrain.ridgeStrength,
      erosionPasses: recipe.terrain.erosionPasses,
    },
    components,
    paths,
    terrainEdits,
    scatter,
    densityRegions,
    settlements,
    settlementRegions,
    deleted: [...new Set(deleted)],
    moved: dedupeMoved(moved),
    locks: locks.map(lock => [lock.minX, lock.minZ, lock.maxX, lock.maxZ]),
  };

  const digest = inputDigest({
    recipe,
    generatorVersion: recipe.generatorVersion,
    libraryHashes,
    layerHashes,
    ruleHashes: [],
    overridesDigest: sha256Canonical(overrideDigests.sort()),
  });

  return { recipe, compileInput, digest, libraryHashes, layerHashes, overrideDigests };
}

function dedupeMoved(moved) {
  // Last write wins: a hand move applied twice is still one move, and the later layer in
  // the recipe order is the one the user edited most recently.
  const byId = new Map();
  for (const entry of moved) {
    if (!entry || typeof entry.id !== 'string') continue;
    byId.set(entry.id, entry);
  }
  return [...byId.values()];
}

/**
 * Runs the native worker. It is a separate process so a crash in the compiler is a failed
 * cook rather than a dead editor, and so the cook can run on a machine with no GPU.
 */
export async function runWorker({ worker = DEFAULT_WORKER, inputPath, outputDir, inputDigest: digest, maxChunks, timeoutMs = 300000 }) {
  await fs.access(worker).catch(() => {
    throw new WorldError(WORLD_ERROR_CODES.NATIVE_UNAVAILABLE,
      'the native world cook worker is missing', {
        stage: 'cook',
        suggestion: 'Run `npm run world:build` (tools/build-world.ps1) to build build/world-host/world_cook.exe.',
      });
  });

  const args = ['--input', inputPath, '--out', outputDir];
  if (digest) args.push('--input-digest', digest);
  if (maxChunks) args.push('--max-chunks', String(maxChunks));

  return new Promise((resolve, reject) => {
    const child = spawn(worker, args, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill();
      reject(new WorldError(WORLD_ERROR_CODES.CONSTRAINT_UNSATISFIED, 'the world cook timed out', { stage: 'cook' }));
    }, timeoutMs);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.on('error', error => {
      clearTimeout(timer);
      reject(new WorldError(WORLD_ERROR_CODES.NATIVE_UNAVAILABLE, `cannot start the world cook worker: ${error.message}`, { stage: 'cook' }));
    });
    child.on('close', code => {
      clearTimeout(timer);
      const line = stdout.trim().split('\n').filter(Boolean).pop() ?? '';
      let parsed = null;
      try {
        parsed = JSON.parse(line);
      } catch {
        parsed = null;
      }
      if (code !== 0 || !parsed || parsed.ok !== true) {
        reject(new WorldError(WORLD_ERROR_CODES.CONSTRAINT_UNSATISFIED,
          parsed?.error ?? stderr.trim() ?? `the world cook exited with code ${code}`, {
            stage: 'cook',
            cause: stderr.slice(0, 4096),
          }));
        return;
      }
      resolve(parsed);
    });
  });
}

/**
 * Full cook: resolve -> write the request -> run the worker -> finalize the manifest.
 * `cacheDir` is where the worker writes; `finalDir` is the content-addressed home of the
 * finished world. They are the same directory when cooking straight into the project.
 */
export async function cookWorld({
  projectRoot,
  recipe,
  worker = DEFAULT_WORKER,
  cacheDir,
  finalDir,
  overrides = [],
  keepCache = true,
  maxChunks,
}) {
  const resolved = await resolveWorld(projectRoot, recipe, { overrides });
  const cache = cacheDir ?? path.join(projectRoot, WORLD_LAYOUT.cache, `${resolved.digest.slice(0, 16)}`);
  await fs.mkdir(cache, { recursive: true });
  const inputPath = path.join(cache, 'input.json');
  await fs.writeFile(inputPath, JSON.stringify(resolved.compileInput, null, 2), 'utf8');

  const summary = await runWorker({
    worker, inputPath, outputDir: cache, inputDigest: resolved.digest, maxChunks,
  });

  const partialPath = path.join(cache, 'manifest.partial.json');
  const partial = JSON.parse(await fs.readFile(partialPath, 'utf8'));
  partial.schema = MANIFEST_SCHEMA;
  partial.inputDigest = resolved.digest;
  partial.savedAt = new Date().toISOString();
  const manifestHash = manifestHashOf(partial);
  const manifest = { ...partial, manifestHash };
  delete manifest.savedAt;

  const errors = validateManifest(manifest);
  if (errors.length) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `the cooked manifest is invalid: ${errors[0]}`, {
      stage: 'manifest',
      cause: errors.slice(0, 16),
    });
  }

  const target = finalDir ?? path.join(projectRoot, cookedPath(manifestHash));
  if (path.resolve(target) !== path.resolve(cache)) {
    await fs.mkdir(target, { recursive: true });
    await copyTree(path.join(cache, 'chunks'), path.join(target, 'chunks'));
    await fs.copyFile(inputPath, path.join(target, 'input.json')).catch(() => {});
  }
  await fs.writeFile(path.join(target, 'manifest.json'), JSON.stringify(manifest, null, 2), 'utf8');
  if (!keepCache && path.resolve(target) !== path.resolve(cache)) {
    await fs.rm(cache, { recursive: true, force: true }).catch(() => {});
  }

  return {
    manifest,
    manifestHash,
    digest: resolved.digest,
    directory: target,
    cache,
    summary,
    relativeDirectory: path.relative(projectRoot, target).split(path.sep).join('/'),
  };
}

async function copyTree(from, to) {
  await fs.mkdir(to, { recursive: true });
  const entries = await fs.readdir(from, { withFileTypes: true });
  for (const entry of entries) {
    const source = path.join(from, entry.name);
    const target = path.join(to, entry.name);
    if (entry.isDirectory()) await copyTree(source, target);
    else await fs.copyFile(source, target);
  }
}

/** Reads a recipe from the project. */
export async function readRecipe(projectRoot, worldId) {
  const resolved = await resolveProjectPath(projectRoot, `${WORLD_LAYOUT.worldDir}/${worldId}.world.json`, {
    allowMissing: false, mustBeFile: true,
  });
  const recipe = await readJson(resolved.absolute, `world recipe '${worldId}'`);
  assertRecipe(recipe);
  return recipe;
}

/**
 * Reads a cooked manifest and verifies that every chunk file still matches its hash.
 * `verifyFiles = false` skips the byte check for callers that only need the shape.
 */
export async function readManifest(directory, { verifyFiles = true } = {}) {
  const manifest = await readJson(path.join(directory, 'manifest.json'), 'cooked manifest');
  const errors = validateManifest(manifest);
  if (errors.length) {
    throw new WorldError(WORLD_ERROR_CODES.SCHEMA_INVALID, `cooked manifest is invalid: ${errors[0]}`, {
      stage: 'manifest', cause: errors.slice(0, 16),
    });
  }
  if (verifyFiles) {
    for (const chunk of manifest.chunks) {
      const file = path.join(directory, chunk.file);
      let bytes;
      try {
        bytes = await fs.readFile(file);
      } catch {
        throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT, `chunk ${chunk.file} is missing`, {
          stage: 'manifest', chunk: { cx: chunk.cx, cz: chunk.cz },
        });
      }
      if (bytes.length !== chunk.bytes) {
        throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
          `chunk ${chunk.file} is ${bytes.length} bytes, manifest says ${chunk.bytes}`, {
            stage: 'manifest', chunk: { cx: chunk.cx, cz: chunk.cz },
          });
      }
      if (sha256Hex(bytes) !== chunk.sha256) {
        throw new WorldError(WORLD_ERROR_CODES.CHUNK_CORRUPT,
          `chunk ${chunk.file} does not match its recorded hash`, {
            stage: 'manifest', chunk: { cx: chunk.cx, cz: chunk.cz },
          });
      }
    }
  }
  return manifest;
}

/** The dependency closure a published build needs: the manifest plus every chunk file. */
export async function releaseClosure(directory, manifest) {
  const files = [{ source: path.join(directory, 'manifest.json'), relative: 'manifest.json' }];
  for (const chunk of manifest.chunks) {
    files.push({ source: path.join(directory, chunk.file), relative: chunk.file });
  }
  let bytes = 0;
  for (const file of files) {
    const stat = await fs.stat(file.source);
    file.bytes = stat.size;
    bytes += stat.size;
  }
  return { files, bytes, chunks: manifest.chunks.length };
}

/** Writes a layer file and returns the reference the recipe should carry. */
export async function writeLayer(projectRoot, layer) {
  assertLayer({ ...layer, schema: LAYER_SCHEMA });
  const relative = layerPath(layer.id);
  const resolved = await resolveProjectPath(projectRoot, relative);
  await fs.mkdir(path.dirname(resolved.absolute), { recursive: true });
  const payload = { ...layer, schema: LAYER_SCHEMA, savedAt: new Date().toISOString() };
  delete payload.savedAt;
  const text = JSON.stringify(payload, null, 2);
  await fs.writeFile(resolved.absolute, text, 'utf8');
  return { id: layer.id, file: relative, sha256: sha256Hex(Buffer.from(text, 'utf8')) };
}

export async function writeLibrary(projectRoot, library) {
  assertLibrary(library);
  const relative = libraryPath(library.id);
  const resolved = await resolveProjectPath(projectRoot, relative);
  await fs.mkdir(path.dirname(resolved.absolute), { recursive: true });
  const text = JSON.stringify(library, null, 2);
  await fs.writeFile(resolved.absolute, text, 'utf8');
  return { id: library.id, file: relative, sha256: sha256Hex(Buffer.from(text, 'utf8')) };
}

export async function writeOverrides(projectRoot, overrides) {
  assertOverrides({ ...overrides, schema: 'nativegpu.world-overrides/1' });
  const relative = overridePath(overrides.regionId ?? 'default');
  const resolved = await resolveProjectPath(projectRoot, relative);
  await fs.mkdir(path.dirname(resolved.absolute), { recursive: true });
  const text = JSON.stringify({ ...overrides, schema: 'nativegpu.world-overrides/1' }, null, 2);
  await fs.writeFile(resolved.absolute, text, 'utf8');
  return { id: overrides.regionId ?? 'default', file: relative, sha256: sha256Hex(Buffer.from(text, 'utf8')) };
}

export { budgetFor, recipeChunkRange, canonicalize };

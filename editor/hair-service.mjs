import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { loadCharacterAsset } from '../engine/character/rig-mesh.mjs';
import { HairError, normalizeHairstyle, hairstyleRevision, REGION_PRESETS } from '../engine/hair/spec.mjs';
import { buildScalp, scalpHash } from '../engine/hair/scalp.mjs';
import { generateHairstyle, rootTracking } from '../engine/hair/generate.mjs';
import { cookHairstyle, cookCompatibleGroom, readHairPackage } from '../engine/hair/cook.mjs';
import { applyPatch, hairRevision } from '../engine/hair/patch.mjs';
import {
  registerReferenceFile, reconcileAnalyses, regionsFromReconciliation, ANALYSIS_SCHEMA,
} from '../engine/hair/reference.mjs';
import { buildCapturePlan, captureMetadata, resolveSelection, CAPTURE_VIEWS, DEBUG_CHANNELS } from '../engine/hair/capture.mjs';
import { validateHairstyle as validateHairstyleReport, HAIR_TEST_POSES } from '../engine/hair/validate.mjs';
import {
  transparencyBudget, strandCoveragePixels, calibrateForAlbedo, albedoFromPigment,
} from '../engine/hair/shading.mjs';
import { parseMesh, bodyFromCharacter, inspectGarment, GarmentError } from '../engine/garment/inspect.mjs';
import { partitionBody } from '../engine/garment/partition.mjs';
import { bindGarment, validateGarmentPhysics } from '../engine/garment/validate.mjs';
import { validateWeights, weightsSnapshot, patchWeights, topologyHash, DEFAULT_MAX_INFLUENCES, influenceTotals } from '../engine/garment/weights.mjs';
import { setRegionPhysics, defaultPhysicsContract, updateSkinTarget, teleport, reset, setPaused } from '../engine/garment/physics.mjs';
import { renderWeightsView, exportWeightsBundle, encodeWeightsBinary, EXPORT_CHANNELS, VIEWS } from '../engine/weights/export.mjs';
import { getPlugin, PLUGINS } from '../engine/plugins/registry.mjs';

// Hair and garment authoring service: the single mutation channel for the two workflows.
//
// Everything the AI tools call lands here, and this module owns the four properties the
// plan asks for:
//
//   * one write path (no second channel that bypasses revision checks),
//   * content revisions with `expectedRevision`, dry-run and undo,
//   * idempotent retries by `requestId`,
//   * assets and reports confined to the project directory.
//
// The heavy lifting stays in `engine/`; this file is orchestration, path safety and state.

const DEFAULT_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const HAIR_SOURCE_DIRECTORY = 'assets/hair/source';
const HAIR_COOKED_DIRECTORY = 'assets/hair/cooked';
const HAIR_REFERENCE_DIRECTORY = 'assets/hair/references';
const GARMENT_DIRECTORY = 'assets/garments';
const HAIR_REPORT_DIRECTORY = 'reports/hair';
const GARMENT_REPORT_DIRECTORY = 'reports/garments';
const WEIGHTS_REPORT_DIRECTORY = 'reports/weights';
const MAX_HISTORY = 64;
const ID = /^[a-z][a-z0-9_-]{0,63}$/;

function fail(code, message, details) {
  const error = new HairError(code, message, details);
  return error;
}

const readJson = (file) => fs.readFile(file, 'utf8').then((text) => JSON.parse(text.replace(/^\uFEFF/, '')));

async function writeAtomic(file, data) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.${Date.now().toString(36)}.tmp`;
  try {
    await fs.writeFile(temporary, data);
    await fs.rename(temporary, file);
  } catch (error) {
    await fs.unlink(temporary).catch(() => {});
    throw error;
  }
}

export function createHairService({ root = DEFAULT_ROOT } = {}) {
  const projectRoot = path.resolve(root);
  // Session state: undo/redo stacks and the character cache. Both are deliberately
  // per-session — an undo history that survives a restart would let a user "undo" into a
  // document another tool has since replaced.
  const undoStacks = new Map();
  const redoStacks = new Map();
  const characterCache = new Map();
  const scalpCache = new Map();
  const generationCache = new Map();
  const garmentBindings = new Map();
  const requestLog = new Map();

  async function insideProject(relative, label) {
    const absolute = path.resolve(projectRoot, relative);
    const rootReal = await fs.realpath(projectRoot);
    let ancestor = absolute;
    for (;;) {
      try {
        const real = await fs.realpath(ancestor);
        const relation = path.relative(rootReal, real);
        if (relation.startsWith('..') || path.isAbsolute(relation)) {
          throw fail('NG-E-HAIR-PATH', `${label} resolves outside the project: ${relative}`);
        }
        break;
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        const parent = path.dirname(ancestor);
        if (parent === ancestor) break;
        ancestor = parent;
      }
    }
    return absolute;
  }

  async function character(assetId = 'assets/builtin/robot/character.json') {
    const cached = characterCache.get(assetId);
    if (cached) return cached;
    const file = await insideProject(assetId, 'character');
    const loaded = await loadCharacterAsset(file);
    characterCache.set(assetId, loaded);
    return loaded;
  }

  function scalpFor(loaded, headBone) {
    const key = `${loaded.sourceSha256 ?? loaded.file}:${headBone}`;
    const cached = scalpCache.get(key);
    if (cached) return cached;
    const scalp = buildScalp(loaded, { headBone });
    scalpCache.set(key, scalp);
    return scalp;
  }

  function resolveHairSource(id) {
    if (typeof id !== 'string' || !ID.test(id)) throw fail('NG-E-HAIR-FORMAT', 'hairstyle id must be a lowercase identifier');
    return path.join(projectRoot, HAIR_SOURCE_DIRECTORY, `${id}.hairstyle.json`);
  }

  function pushHistory(map, id, entry) {
    const stack = map.get(id) ?? [];
    stack.push(entry);
    while (stack.length > MAX_HISTORY) stack.shift();
    map.set(id, stack);
  }

  async function loadSource(id) {
    const file = resolveHairSource(id);
    const spec = await readJson(file).catch((error) => {
      if (error.code === 'ENOENT') throw fail('NG-E-HAIR-NOT-FOUND', `hairstyle ${id} does not exist`);
      throw error;
    });
    const normalized = normalizeHairstyle(spec);
    return { spec: normalized, revision: hairstyleRevision(normalized), file };
  }

  async function saveSource(spec, { expectedRevision = null } = {}) {
    const normalized = normalizeHairstyle(spec);
    const revision = hairstyleRevision(normalized);
    const file = resolveHairSource(normalized.id);
    let previousText = null;
    try { previousText = await fs.readFile(file, 'utf8'); } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
    if (expectedRevision !== null && expectedRevision !== revision && previousText !== null) {
      // expectedRevision is checked by the caller before patching; this guards a direct
      // save path that raced another writer.
      const current = hairstyleRevision(normalizeHairstyle(JSON.parse(previousText)));
      if (current !== expectedRevision) {
        throw fail('NG-E-HAIR-REVISION',
          `hairstyle ${normalized.id} changed while the patch was being applied`,
          [{ currentRevision: current, expectedRevision }]);
      }
    }
    if (previousText !== null) pushHistory(undoStacks, normalized.id, { revision, text: previousText });
    redoStacks.delete(normalized.id);
    await writeAtomic(file, `${JSON.stringify(normalized, null, 2)}\n`);
    return { id: normalized.id, revision, path: path.relative(projectRoot, file).split(path.sep).join('/') };
  }

  // ------------------------------------------------------------- hair actions
  async function capabilities() {
    const { ANALYSIS_FIELDS } = await import('../engine/hair/reference.mjs');
    return {
      schema: 'nativegpu.hair-capabilities/1',
      plugins: [...PLUGINS.values()].map((plugin) => ({
        id: plugin.id, version: plugin.version, enabled: true, capabilities: plugin.capabilities,
      })),
      hairstyleSchema: 'nativegpu.hairstyle/1',
      cookedSchema: 'nativegpu.hair-cook/1',
      regions: Object.keys(REGION_PRESETS),
      captureViews: Object.keys(CAPTURE_VIEWS),
      channels: DEBUG_CHANNELS,
      poses: Object.keys(HAIR_TEST_POSES),
      weightChannels: EXPORT_CHANNELS,
      weightViews: VIEWS,
      analysisFields: Object.keys(ANALYSIS_FIELDS),
      limits: {
        regions: 16, guides: 4096, strands: 120000, captureResolution: 4096,
      },
      notImplemented: [
        'visual analysis is provided by the caller; the engine does not run a vision model',
        'garment design, 2D pattern making and sewing are out of scope',
        'cloth self-collision and layer collision are not supported',
      ],
    };
  }

  async function registerReference(args) {
    const file = await insideProject(args.path, 'reference image');
    const reference = await registerReferenceFile(file, {
      id: args.id, view: args.view, crop: args.crop ?? null, subject: args.subject ?? '',
      hairlineVisible: args.hairlineVisible ?? null, confidence: args.confidence ?? 0.5,
      authorized: args.authorized === true, license: args.license ?? null,
    });
    const directory = path.join(projectRoot, HAIR_REFERENCE_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const record = { ...reference, path: path.relative(projectRoot, file).split(path.sep).join('/'), registeredAt: new Date().toISOString() };
    await writeAtomic(path.join(directory, `${reference.id}.reference.json`), `${JSON.stringify(record, null, 2)}\n`);
    return record;
  }

  async function reconcile(args) {
    const references = [];
    for (const entry of args.references ?? []) {
      if (entry.hash) { references.push(entry); continue; }
      const file = await insideProject(entry.path, 'reference image');
      references.push(await registerReferenceFile(file, {
        id: entry.id, view: entry.view, authorized: true, confidence: entry.confidence ?? 0.5,
      }));
    }
    const analyses = (args.analyses ?? []).map((analysis) => ({ schema: ANALYSIS_SCHEMA, ...analysis }));
    const reconciliation = reconcileAnalyses(references, analyses);
    const derived = regionsFromReconciliation(reconciliation, { style: args.style ?? null });
    return { reconciliation, derived };
  }

  function defaultSpec({ id, character: characterRef, scalp, reconciliation, regions, seed, views }) {
    const colour = reconciliation?.fields?.colorAlbedo ?? [0.09, 0.06, 0.04];
    const groups = [{
      id: 'hair',
      albedo: colour,
      roughness: reconciliation?.fields?.roughness ?? 0.3,
      radialRoughness: 0.28, longitudinalRoughness: 0.3,
      absorption: [0.6, 0.42, 0.3],
      melanin: Math.min(0.98, Math.max(0.02, 1 - colour.reduce((sum, value) => sum + value, 0) / 3)),
      redness: 0.25,
    }];
    return {
      schema: 'nativegpu.hairstyle/1',
      id,
      seed: seed ?? 1,
      character: {
        headBone: characterRef.headBone ?? 'head',
        assetId: characterRef.assetId ?? 'assets/builtin/robot/character.json',
        scalpHash: scalpHash(scalp),
        characterHash: scalp.characterHash,
        units: 'metres',
        coordinateSystem: 'left-handed-y-up',
      },
      references: views ?? [],
      analysis: reconciliation ?? null,
      regions: regions ?? [
        { id: 'bangs', ...REGION_PRESETS.bangs, densityPerCm2: 140, lengthMetres: 0.14, comb: [0, -0.25, 1] },
        { id: 'crown', ...REGION_PRESETS.crown, densityPerCm2: 140, lengthMetres: 0.18, comb: [0, -0.35, 0.5] },
        { id: 'left_side', ...REGION_PRESETS.left_side, densityPerCm2: 140, lengthMetres: 0.2, comb: [0.4, -0.5, 0.2] },
        { id: 'right_side', ...REGION_PRESETS.right_side, densityPerCm2: 140, lengthMetres: 0.2, comb: [-0.4, -0.5, 0.2] },
        { id: 'back', ...REGION_PRESETS.back, densityPerCm2: 140, lengthMetres: 0.22, comb: [0, -0.5, -0.4] },
      ],
      strands: { widthMetres: 0.00007, rootJitterMetres: 0.0012, streamsPerGuide: 40 },
      guides: { segments: 12, radiusMetres: 0.00007, maxGuides: 4096 },
      material: { groups },
      physics: { gravityMetresPerSecond2: 9.81 },
      lod: [
        { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 4 },
        { level: 1, strandFraction: 0.5, segmentScale: 0.7, maxDistanceMetres: 12 },
        { level: 2, strandFraction: 0.18, segmentScale: 0.5, maxDistanceMetres: 24 },
      ],
      budget: { maxStrands: 120000, maxPoints: 6000000 },
      locks: [],
      guideOverrides: [],
    };
  }

  async function create(args) {
    if (typeof args.id !== 'string' || !ID.test(args.id)) throw fail('NG-E-HAIR-FORMAT', 'id must be a lowercase identifier');
    const loaded = await character(args.characterAsset ?? 'assets/builtin/robot/character.json');
    const headBone = args.headBone ?? 'head';
    const scalp = scalpFor(loaded, headBone);
    const spec = args.spec
      ? normalizeHairstyle({ ...args.spec, id: args.id })
      : normalizeHairstyle(defaultSpec({
        id: args.id,
        character: { headBone, assetId: args.characterAsset ?? 'assets/builtin/robot/character.json' },
        scalp,
        reconciliation: args.reconciliation ?? null,
        regions: args.regions ?? null,
        seed: args.seed ?? 1,
        views: args.references ?? [],
      }));
    const generated = generateHairstyle(spec, loaded, { scalp });
    // Binding diagnostics are part of creating an asset, not an optional extra: a
    // hairstyle whose roots do not sit on the scalp must be refused at creation time.
    const binding = validateHairstyleReport({ generated, character: loaded, poses: { rest: {} } });
    const cooked = cookHairstyle(generated, {
      binding: {
        maximumSurfaceErrorMetres: binding.poses[0].maxSurfaceErrorMetres,
        rootsOffSurface: binding.poses[0].rootsOffSurface,
        checkPoses: Object.keys(HAIR_TEST_POSES),
      },
      analysis: args.reconciliation ? { digest: args.reconciliation.digest, inferredFields: args.reconciliation.inferredFields } : null,
    });
    const saved = await saveSource(spec, { expectedRevision: null });
    const directory = path.join(projectRoot, HAIR_COOKED_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const packageFile = path.join(directory, `${args.id}.${cooked.hash.slice(0, 12)}.nghair`);
    await writeAtomic(packageFile, cooked.bytes);
    return {
      id: args.id,
      revision: saved.revision,
      assetHash: cooked.hash,
      sourcePath: saved.path,
      packagePath: path.relative(projectRoot, packageFile).split(path.sep).join('/'),
      counts: cooked.counts,
      lod: cooked.lod,
      regions: cooked.regions,
      binding: binding.poses[0],
      validation: { passes: binding.passes, failures: binding.failures },
      inferredFields: args.reconciliation?.inferredFields ?? [],
    };
  }

  async function load(args) {
    const { spec, revision, file } = await loadSource(args.id);
    const loaded = await character(spec.character.assetId ?? 'assets/builtin/robot/character.json');
    const scalp = scalpFor(loaded, spec.character.headBone);
    return {
      id: spec.id,
      revision,
      path: path.relative(projectRoot, file).split(path.sep).join('/'),
      spec,
      scalp: { hash: scalp.hash, vertices: scalp.vertices, triangles: scalp.triangles, headBone: scalp.headBone },
      guides: (spec.guideOverrides ?? []).length,
      lockedRegions: spec.regions.filter((region) => (region.lockedFields ?? []).length > 0).map((region) => ({
        region: region.id, fields: region.lockedFields,
      })),
      canUndo: (undoStacks.get(spec.id) ?? []).length > 0,
      canRedo: (redoStacks.get(spec.id) ?? []).length > 0,
    };
  }

  async function generateCached(spec) {
    const revision = hairstyleRevision(spec);
    const cached = generationCache.get(`${spec.id}:${revision}`);
    if (cached) return cached;
    const loaded = await character(spec.character.assetId ?? 'assets/builtin/robot/character.json');
    const scalp = scalpFor(loaded, spec.character.headBone);
    const generated = generateHairstyle(spec, loaded, { scalp });
    generationCache.set(`${spec.id}:${revision}`, generated);
    return generated;
  }

  async function patch(args) {
    const { spec, revision } = await loadSource(args.id);
    const generated = await generateCached(spec);
    const knownGuides = new Set(generated.guides.map((guide) => guide.stableId));
    const { spec: candidate, diff } = applyPatch(spec, {
      expectedRevision: args.expectedRevision ?? revision,
      operations: args.operations,
      requestId: args.requestId,
      note: args.note,
    }, { knownGuides });
    if (args.dryRun === true) {
      return {
        dryRun: true, id: args.id, diff,
        wouldRegenerate: diff.regeneratedRegions,
        untouched: diff.untouchedRegions,
      };
    }
    const saved = await saveSource(candidate, { expectedRevision: revision });
    // Prove the claim instead of asserting it: regenerate and diff the untouched regions.
    const regenerated = await generateCached(candidate);
    const changed = [];
    for (const regionId of diff.untouchedRegions) {
      const before = generated.guides.filter((guide) => guide.region === regionId)
        .map((guide) => [guide.stableId, guide.points]).flat(2);
      const after = regenerated.guides.filter((guide) => guide.region === regionId)
        .map((guide) => [guide.stableId, guide.points]).flat(2);
      if (canonicalJson(before) !== canonicalJson(after)) changed.push(regionId);
    }
    if (changed.length > 0) {
      throw fail('NG-E-HAIR-REGION-LEAK',
        `a patch meant for ${diff.changedRegions.join(', ')} also moved ${changed.join(', ')}; the edit was not committed`,
        [{ changed, intended: diff.changedRegions }]);
    }
    return {
      dryRun: false,
      id: args.id,
      revision: saved.revision,
      previousRevision: revision,
      diff,
      verifiedUntouchedRegions: diff.untouchedRegions,
      canUndo: true,
    };
  }

  async function undo(args) {
    const stack = undoStacks.get(args.id) ?? [];
    if (!stack.length) throw fail('NG-E-HAIR-NOTHING-TO-UNDO', `hairstyle ${args.id} has no undo history`);
    const file = resolveHairSource(args.id);
    const current = await fs.readFile(file, 'utf8').catch(() => null);
    const entry = stack.pop();
    if (current !== null) pushHistory(redoStacks, args.id, { revision: entry.revision, text: current });
    await writeAtomic(file, entry.text);
    const spec = normalizeHairstyle(JSON.parse(entry.text));
    generationCache.clear();
    return {
      id: args.id, revision: hairstyleRevision(spec), restoredRevision: entry.revision,
      canUndo: (undoStacks.get(args.id) ?? []).length > 0,
      canRedo: (redoStacks.get(args.id) ?? []).length > 0,
    };
  }

  async function redo(args) {
    const stack = redoStacks.get(args.id) ?? [];
    if (!stack.length) throw fail('NG-E-HAIR-NOTHING-TO-REDO', `hairstyle ${args.id} has no redo history`);
    const file = resolveHairSource(args.id);
    const current = await fs.readFile(file, 'utf8').catch(() => null);
    const entry = stack.pop();
    if (current !== null) pushHistory(undoStacks, args.id, { entry: null, revision: entry.revision, text: current });
    await writeAtomic(file, entry.text);
    const spec = normalizeHairstyle(JSON.parse(entry.text));
    generationCache.clear();
    return {
      id: args.id, revision: hairstyleRevision(spec),
      canUndo: (undoStacks.get(args.id) ?? []).length > 0,
      canRedo: (redoStacks.get(args.id) ?? []).length > 0,
    };
  }

  async function capture(args) {
    const { spec, revision } = await loadSource(args.id);
    const loaded = await character(spec.character.assetId);
    const scalp = scalpFor(loaded, spec.character.headBone);
    const generated = await generateCached(spec);
    const cooked = cookHairstyle(generated);
    const plan = buildCapturePlan({
      views: args.views ?? ['front', 'back', 'left', 'right', 'three_quarter_left', 'hairline_closeup'],
      channels: args.channels ?? ['beauty', 'silhouette', 'region_id', 'root_binding'],
      lighting: args.lighting ?? ['neutral'],
      resolution: args.resolution ?? { width: 1024, height: 1024 },
      headCentre: scalp.centre,
      headRadiusMetres: scalp.radius,
      revision,
      assetHash: cooked.hash,
      seed: spec.seed,
      pose: args.pose ?? {},
      frame: args.frame ?? 3,
      exposure: args.exposure ?? 0,
    });
    const directory = path.join(projectRoot, HAIR_REPORT_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, `capture-plan-${args.id}-${cooked.hash.slice(0, 8)}.json`);
    // Each entry carries the metadata that makes a screenshot addressable later: camera,
    // pose, revision, seed, exposure, frame. Without it a selection cannot be validated.
    const entries = plan.entries.map((entry) => ({ ...entry, metadata: captureMetadata(entry) }));
    await writeAtomic(file, `${JSON.stringify({ ...plan, entries }, null, 2)}\n`);
    return {
      id: args.id,
      revision,
      assetHash: cooked.hash,
      planPath: path.relative(projectRoot, file).split(path.sep).join('/'),
      entries: entries.map((entry) => ({
        id: entry.id, view: entry.view, channel: entry.channel, lighting: entry.lighting,
        requiresIdBuffer: entry.requiresIdBuffer, requiresDepthBuffer: entry.requiresDepthBuffer,
        metadata: entry.metadata,
      })),
      count: entries.length,
      note: 'Plan only: rendering these views requires the native capture path; the metadata here is what '
        + 'a selection is validated against.',
    };
  }

  async function validate(args) {
    const { spec, revision } = await loadSource(args.id);
    const loaded = await character(spec.character.assetId);
    const scalp = scalpFor(loaded, spec.character.headBone);
    const generated = await generateCached(spec);
    const report = validateHairstyleReport({ generated, character: loaded, poses: HAIR_TEST_POSES });
    const budget = transparencyBudget({
      strandsOnScreen: generated.stats.strands,
      averageSegmentsPerStrand: generated.stats.segments,
      averageCoverage: args.averageCoverage ?? 0.25,
      layersPerTile: args.layersPerTile ?? 8,
      resolution: args.resolution ?? { width: 1920, height: 1080 },
    });
    const directory = path.join(projectRoot, HAIR_REPORT_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, `validation-${args.id}-${revision.slice(0, 8)}.json`);
    const payload = {
      ...report, revision, budget,
      transparency: budget,
      performance: {
        measured: false,
        generationMs: null,
        note: 'GPU generation/simulation/draw timings are not collected by the service; run a native capture to time them',
      },
    };
    await writeAtomic(file, `${JSON.stringify(payload, null, 2)}\n`);
    return { ...payload, reportPath: path.relative(projectRoot, file).split(path.sep).join('/') };
  }

  async function exportPackage(args) {
    const { spec, revision } = await loadSource(args.id);
    const loaded = await character(spec.character.assetId);
    const generated = await generateCached(spec);
    const cooked = cookHairstyle(generated);
    const directory = path.join(projectRoot, HAIR_COOKED_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, `${spec.id}.nghair`);
    await writeAtomic(file, cooked.bytes);
    const read = readHairPackage(cooked.bytes);
    const result = {
      id: spec.id, revision, assetHash: cooked.hash,
      path: path.relative(projectRoot, file).split(path.sep).join('/'),
      bytes: cooked.bytesTotal,
      counts: read.meta.counts,
      scalpVertices: read.scalp.vertices, scalpTriangles: read.scalp.triangles,
      guideIds: read.meta.guideIds.length,
    };
    if (args.includeCompatibleGroom === true) {
      const compatible = cookCompatibleGroom(spec);
      const compatFile = path.join(directory, `${spec.id}.compat.nggroom`);
      await writeAtomic(compatFile, compatible.bytes);
      result.compatibleGroom = {
        path: path.relative(projectRoot, compatFile).split(path.sep).join('/'),
        hash: compatible.hash, losses: compatible.losses,
      };
    }
    return result;
  }

  function selection(args) {
    const metadata = args.metadata;
    const currentRevision = args.currentRevision;
    const currentAssetHash = args.currentAssetHash;
    const guides = args.guides ?? [];
    return resolveSelection({
      selection: args.selection, metadata, currentRevision, currentAssetHash,
      idBuffer: args.idBuffer ? {
        width: args.idBuffer.width, height: args.idBuffer.height,
        data: new Uint32Array(Buffer.from(args.idBuffer.dataBase64, 'base64').buffer.slice(0)),
      } : null,
      depthBuffer: args.depthBuffer ? {
        width: args.depthBuffer.width, height: args.depthBuffer.height,
        data: new Float32Array(Buffer.from(args.depthBuffer.dataBase64, 'base64').buffer.slice(0)),
      } : null,
      guides, maxDepthMetres: args.maxDepthMetres ?? 0.05,
    });
  }

  // ----------------------------------------------------------- garment actions
  async function garmentInspect(args) {
    const loaded = await character(args.characterAsset ?? 'assets/builtin/robot/character.json');
    const body = bodyFromCharacter(loaded);
    const garment = args.garmentPath
      ? parseMesh(await readJson(await insideProject(args.garmentPath, 'garment')), { label: 'garment' })
      : parseMesh(args.garment, { label: 'garment' });
    const report = inspectGarment({ garment, body, character: loaded, thresholds: args.thresholds ?? {} });
    const directory = path.join(projectRoot, GARMENT_REPORT_DIRECTORY);
    await fs.mkdir(directory, { recursive: true });
    const key = args.garmentId ?? `garment-${topologyHash(garment).slice(0, 8)}`;
    const file = path.join(directory, `inspection-${key}.json`);
    await writeAtomic(file, `${JSON.stringify(report, null, 2)}\n`);
    return {
      ...report,
      garmentId: key,
      topologyHash: topologyHash(garment),
      reportPath: path.relative(projectRoot, file).split(path.sep).join('/'),
    };
  }

  function garmentMesh(args) {
    if (args.garmentPath) return null;
    return parseMesh(args.garment, { label: 'garment' });
  }

  async function garmentBind(args) {
    const loaded = await character(args.characterAsset ?? 'assets/builtin/robot/character.json');
    const garment = args.garmentPath
      ? parseMesh(await readJson(await insideProject(args.garmentPath, 'garment')), { label: 'garment' })
      : garmentMesh(args);
    const bound = bindGarment({
      garment, character: loaded,
      physics: args.physics ?? {},
      material: args.material ?? {},
      maximumInfluences: args.maximumInfluences ?? DEFAULT_MAX_INFLUENCES,
      smooth: args.smooth ?? { iterations: 2, factor: 0.5 },
      regionPhysics: args.regionPhysics ?? null,
      allowedRegions: args.allowedRegions ?? null,
      influenceRegions: args.influenceRegions ?? null,
      locks: args.locks ?? null,
    });
    const key = args.garmentId ?? `garment-${bound.topologyHash.slice(0, 8)}`;
    garmentBindings.set(key, bound);
    const directory = path.join(projectRoot, GARMENT_DIRECTORY, key);
    await fs.mkdir(directory, { recursive: true });
    const bindingFile = path.join(directory, 'binding.json');
    const payload = {
      schema: 'nativegpu.garment-binding/1',
      garmentId: key,
      topologyHash: bound.topologyHash,
      characterAsset: args.characterAsset ?? 'assets/builtin/robot/character.json',
      characterHash: loaded.sourceSha256 ?? null,
      maximumInfluences: bound.weights.maximumInfluences,
      // Weights are stored by stable vertex id, not by array index: an exporter that
      // reorders vertices must not silently reassign weights.
      vertices: Array.from(bound.garment.vertexIds).map((id, index) => ({
        vertexId: id,
        region: bound.partition.regions[index],
        confidence: Number(bound.partition.confidence[index].toFixed(4)),
        influences: Array.from({ length: bound.weights.maximumInfluences }, (_, slot) => ({
          bone: bound.weights.joints[index * bound.weights.maximumInfluences + slot],
          boneName: loaded.bones[bound.weights.joints[index * bound.weights.maximumInfluences + slot]]?.name ?? null,
          weight: Number(bound.weights.weights[index * bound.weights.maximumInfluences + slot].toFixed(6)),
        })).filter((entry) => entry.weight > 0),
      })),
      physics: {
        blend: Array.from(bound.contract.blend),
        maxDisplacementMetres: Array.from(bound.contract.maxDisplacement),
        collisionThicknessMetres: Array.from(bound.contract.thickness),
        anchorStiffness: Array.from(bound.contract.anchors),
      },
      statistics: bound.stats,
      generatedAt: new Date().toISOString(),
    };
    await writeAtomic(bindingFile, `${JSON.stringify(payload)}\n`);
    return {
      garmentId: key,
      topologyHash: bound.topologyHash,
      path: path.relative(projectRoot, bindingFile).split(path.sep).join('/'),
      validation: bound.validation,
      statistics: bound.stats,
      diagnostics: bound.diagnostics,
      snapshotDigest: bound.snapshot.digest,
      canPatch: true,
    };
  }

  function requireBinding(args) {
    const entry = garmentBindings.get(args.garmentId);
    if (!entry) {
      throw fail('NG-E-GARMENT-NOT-BOUND',
        `garment ${args.garmentId} is not bound in this session; call garment_bind first`);
    }
    return entry;
  }

  function garmentPhysicsPatch(args) {
    const bound = requireBinding(args);
    // A dry run clones the contract: "preview the physics change" must not be a hidden
    // edit that the next validation silently uses.
    const contract = args.dryRun === true
      ? {
        ...bound.contract,
        blend: Float32Array.from(bound.contract.blend),
        maxDisplacement: Float32Array.from(bound.contract.maxDisplacement),
        thickness: Float32Array.from(bound.contract.thickness),
        anchors: Float32Array.from(bound.contract.anchors),
        mass: Float32Array.from(bound.contract.mass),
      }
      : bound.contract;
    const applied = [];
    for (const change of args.changes ?? []) {
      if (change.regions) {
        applied.push(setRegionPhysics(contract, bound.garment, bound.partition.regions, change.regions, change.values ?? {}));
      } else if (change.vertexIds) {
        const byId = new Map([...bound.garment.vertexIds].map((id, index) => [id, index]));
        let touched = 0;
        for (const id of change.vertexIds) {
          const vertex = byId.get(id);
          if (vertex === undefined) continue;
          ++touched;
          if (change.values?.blend !== undefined) contract.blend[vertex] = change.values.blend;
          if (change.values?.maxDisplacement !== undefined) contract.maxDisplacement[vertex] = change.values.maxDisplacement;
          if (change.values?.thickness !== undefined) contract.thickness[vertex] = change.values.thickness;
          if (change.values?.anchors !== undefined) contract.anchors[vertex] = change.values.anchors;
        }
        applied.push({ touched, vertices: change.vertexIds.length });
      }
    }
    return {
      garmentId: args.garmentId,
      applied,
      contract: {
        meanBlend: Array.from(contract.blend).reduce((sum, value) => sum + value, 0) / bound.garment.vertices,
        minThicknessMetres: Math.min(...contract.thickness),
        maxDisplacementMetres: Math.max(...contract.maxDisplacement),
      },
    };
  }

  function garmentValidate(args) {
    const bound = requireBinding(args);
    const simulation = validateGarmentPhysics(bound, {
      poses: args.poses ?? undefined,
      ticks: args.ticks ?? 48,
      penetrationToleranceMetres: args.penetrationToleranceMetres ?? 0.002,
      stretchTolerance: args.stretchTolerance ?? 1.8,
    });
    return {
      garmentId: args.garmentId,
      topologyHash: bound.topologyHash,
      ...simulation,
      notes: [
        'proxy collision uses bone-driven capsules sized from the body mesh',
        'mesh penetration is measured against the character triangle mesh',
        'self-collision is not implemented; a long dress or a wide sleeve cannot be certified',
      ],
    };
  }

  function weightsExport(args) {
    const bound = requireBinding(args);
    const channels = args.channels ?? ['skin_weight'];
    for (const channel of channels) {
      if (!EXPORT_CHANNELS.includes(channel)) {
        throw fail('NG-E-WEIGHTS-EXPORT', `unknown channel ${channel}`);
      }
    }
    const bundle = exportWeightsBundle({
      garment: bound.garment, weights: bound.weights, snapshot: bound.snapshot,
      contract: bound.contract, bones: bound.character.bones,
      penetration: args.penetration ?? null, partition: bound.partition,
      views: args.views ?? ['front'], channels, bone: args.bone ?? null,
      resolution: args.resolution ?? 512, wireframe: args.wireframe !== false,
    });
    return { garmentId: args.garmentId, ...bundle };
  }

  function weightsPatch(args) {
    const bound = requireBinding(args);
    const result = patchWeights(bound.garment, bound.weights, args.operations ?? [], {
      character: bound.character,
      expectedTopology: args.expectedTopology ?? bound.topologyHash,
    });
    // A dry run must not leave the bound state mutated; that is the difference between a
    // preview and an edit, and the plan requires the same diff for both.
    if (args.dryRun === true) {
      const validation = validateWeights(result.weights, { maximumInfluences: bound.weights.maximumInfluences });
      return {
        dryRun: true, garmentId: args.garmentId, changes: result.changes,
        validation, topologyHash: result.topologyHash,
      };
    }
    bound.weights = result.weights;
    bound.snapshot = weightsSnapshot(bound.garment, bound.weights, { character: bound.character });
    bound.validation = validateWeights(bound.weights, { maximumInfluences: bound.weights.maximumInfluences });
    return {
      dryRun: false, garmentId: args.garmentId, changes: result.changes,
      validation: bound.validation, snapshotDigest: bound.snapshot.digest,
      topologyHash: bound.topologyHash,
      influences: influenceTotals(bound.weights, bound.character.bones).slice(0, 6),
    };
  }

  async function simulationControl(args) {
    const bound = requireBinding(args);
    if (!bound.state) {
      const physics = await requirePhysics();
      bound.state = physics.createSimulation(bound.garment, bound.character, bound.weights, {
        contract: bound.contract, material: bound.material,
      });
    }
    if (args.action === 'pause') return setPaused(bound.state, true);
    if (args.action === 'resume') return setPaused(bound.state, false);
    if (args.action === 'reset') return reset(bound.state);
    if (args.action === 'teleport') return teleport(bound.state, args.pose ?? {});
    if (args.action === 'retarget') return { target: Array.from(updateSkinTarget(bound.state, args.pose ?? {})).slice(0, 9) };
    throw fail('NG-E-GARMENT-PHYSICS', `unknown simulation action ${args.action}`);
  }

  // Loaded on first use: the physics module is the heaviest import and most service
  // calls never need a simulation.
  let physicsModulePromise = null;
  function requirePhysics() {
    physicsModulePromise = physicsModulePromise ?? import('../engine/garment/physics.mjs');
    return physicsModulePromise;
  }

  const actions = {
    'hair.capabilities': () => capabilities(),
    'hair.reference.register': (args) => registerReference(args),
    'hair.analysis.reconcile': (args) => reconcile(args),
    'hair.create': (args) => create(args),
    'hair.load': (args) => load(args),
    'hair.patch': (args) => patch(args),
    'hair.undo': (args) => undo(args),
    'hair.redo': (args) => redo(args),
    'hair.capture': (args) => capture(args),
    'hair.validate': (args) => validate(args),
    'hair.export': (args) => exportPackage(args),
    'hair.selection': (args) => selection(args),
    'garment.inspect': (args) => garmentInspect(args),
    'garment.bind': (args) => garmentBind(args),
    'garment.physics': (args) => garmentPhysicsPatch(args),
    'garment.validate': (args) => garmentValidate(args),
    'garment.simulate': (args) => simulationControl(args),
    'weights.export': (args) => weightsExport(args),
    'weights.patch': (args) => weightsPatch(args),
    'weights.binary': (args) => {
      const bundle = weightsExport(args);
      const binary = encodeWeightsBinary(bundle);
      return { garmentId: args.garmentId, base64: binary.toString('base64'), bytes: binary.length, digest: bundle.digest };
    },
    'plugin.list': () => [...PLUGINS.values()],
    'plugin.info': (args) => getPlugin(args.id),
  };

  return {
    root: projectRoot,
    async init() {
      await requirePhysics();
      return this;
    },
    handles(action) { return Object.hasOwn(actions, action); },
    listActions() { return Object.keys(actions); },
    async call(action, args = {}) {
      try {
        const handler = actions[action];
        if (!handler) throw fail('UNKNOWN_ACTION', `Unknown hair action: ${action}`);
        if (action !== 'hair.capabilities' && action !== 'plugin.list' && action !== 'plugin.info') {
          // Idempotency: a retry with the same requestId returns the first result and does
          // not regenerate a package or bump a revision.
          const requestId = args.requestId;
          if (typeof requestId === 'string' && requestId.length > 0) {
            const key = `${action}:${requestId}`;
            if (requestLog.has(key)) return { ...requestLog.get(key), replayed: true };
            const result = await handler(args);
            requestLog.set(key, result);
            return result;
          }
        }
        return await handler(args);
      } catch (error) {
        return { ok: false, error: {
          code: error.code ?? 'HAIR_FAILURE', message: error.message ?? String(error), details: error.details ?? [],
        } };
      }
    },
  };
}

// Errors from the engine modules are distinct classes; the service normalises the shape
// so the tool layer and the editor both see {ok, error:{code,message,details}}.
export function asToolResult(value) {
  if (value && value.ok === false) return { ok: false, ...value };
  return { ok: true, result: value };
}

export { GarmentError, HairError };

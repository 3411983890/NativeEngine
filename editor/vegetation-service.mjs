import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildPreview, compileSpecies, SPECIES_COOK_SCHEMA } from '../engine/vegetation/species.mjs';
import { defaultGenerators, withDefaults } from '../engine/vegetation/generators.mjs';
import { compileRegion, REGION_SCHEMA } from '../engine/vegetation/region.mjs';
import { readPackage } from '../engine/assets/binary-package.mjs';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';

// Vegetation Forge service.
//
// The editor never re-implements the generator: every preview and every package comes out of
// `engine/vegetation/species.mjs`, the same module the cook runs. A viewport that agreed with
// itself but not with the baked asset would be the single most expensive bug this tool could
// have, so the round trip is the design.
//
// What the service adds on top:
//   - authored sources live in assets/vegetation/species/, seeded from the build fixtures;
//   - preview stops before packing (no hash of 3 MB of sections per keystroke);
//   - cook writes the package into the same library the runtime reads.

const SOURCE_DIRECTORY = 'assets/vegetation/species';
const REGION_DIRECTORY = 'assets/vegetation/regions';
const FIXTURE_DIRECTORY = 'build/vegetation/source';
const COOKED_DIRECTORY = 'build/vegetation/cooked';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;

function fail(code, message) {
  const error = new Error(message);
  error.code = code;
  return error;
}

const base64 = (typed) => Buffer.from(typed.buffer, typed.byteOffset, typed.byteLength).toString('base64');

function readJson(file) {
  return fs.readFile(file, 'utf8').then((text) => JSON.parse(text.replace(/^\uFEFF/, '')));
}

// A revision identifies *content*, not a clock: two saves that produce the same bytes are the
// same revision, and a save that changes one number is a different one. That is what makes
// `expectedRevision` a conflict check rather than a timestamp comparison.
function revisionOf(spec) {
  const payload = { ...spec };
  delete payload.__file;
  delete payload.__revision;
  return sha256Hex(canonicalJson(payload));
}

// Write through a temporary file in the same directory and rename. A crash between the two
// steps leaves the previous file intact instead of a half-written source that the next load
// cannot parse.
async function writeAtomic(file, text) {
  const temporary = `${file}.${process.pid}.${Date.now().toString(36)}.tmp`;
  try {
    await fs.writeFile(temporary, text, 'utf8');
    await fs.rename(temporary, file);
  } catch (error) {
    await fs.unlink(temporary).catch(() => {});
    throw error;
  }
}

const MAX_HISTORY = 64;

// Compact scene description → region document. The panel sends the numbers a person actually
// sets (extent, density, spacing); everything the region schema demands but nobody wants to type
// (cell sizes, page size, budget, plane surface) is filled in here, so the UI cannot drift from
// the schema and the mapping is testable without a browser.
const SCENE_LIMITS = {
  halfExtentMetres: [4, 512],
  densityPerHectare: [1, 5000],
  minSpacingMetres: [0.05, 50],
  seed: [0, 0xFFFFFFFF],
};

function bounded(value, [min, max], name) {
  const number = Number(value);
  if (!Number.isFinite(number) || number < min || number > max) {
    throw fail('NG-E-VEG-SCATTER', `${name} must be within [${min}, ${max}], got ${JSON.stringify(value)}`);
  }
  return number;
}

function regionFromScene(scene) {
  if (!scene || typeof scene !== 'object') throw fail('INVALID_ARGUMENTS', 'region or scene is required');
  if (!ID.test(String(scene.id))) throw fail('NG-E-VEG-SCATTER', 'scene.id is not a valid identifier');
  const rules = scene.species;
  if (!Array.isArray(rules) || rules.length === 0) {
    throw fail('NG-E-VEG-SCATTER', 'scene.species needs at least one {species, densityPerHectare} rule');
  }
  const half = bounded(scene.halfExtentMetres ?? 16, SCENE_LIMITS.halfExtentMetres, 'scene.halfExtentMetres');
  const seed = bounded(scene.seed ?? 1, SCENE_LIMITS.seed, 'scene.seed');
  return {
    schema: REGION_SCHEMA,
    id: scene.id,
    seed,
    bounds: { minX: -half, minZ: -half, maxX: half, maxZ: half },
    cellSizeMetres: 8, pageCellMetres: 16, pageSize: 256,
    surface: { kind: 'plane', heightMetres: 0, version: 'editor-plane/1' },
    species: rules.map((rule, index) => {
      if (!ID.test(String(rule?.species))) {
        throw fail('NG-E-VEG-SCATTER', `scene.species[${index}].species is not a valid identifier`);
      }
      const scale = rule.scaleRange ?? [0.85, 1.15];
      if (!Array.isArray(scale) || scale.length !== 2) {
        throw fail('NG-E-VEG-SCATTER', `scene.species[${index}].scaleRange must be [min, max]`);
      }
      const [low, high] = scale.map((value) => bounded(value, [0.01, 100], `scene.species[${index}].scaleRange`));
      if (!(low <= high)) throw fail('NG-E-VEG-SCATTER', `scene.species[${index}].scaleRange is inverted`);
      const mapped = {
        species: rule.species,
        densityPerHectare: bounded(rule.densityPerHectare, SCENE_LIMITS.densityPerHectare,
          `scene.species[${index}].densityPerHectare`),
        minSpacingMetres: bounded(rule.minSpacingMetres ?? 1, SCENE_LIMITS.minSpacingMetres,
          `scene.species[${index}].minSpacingMetres`),
        scaleRange: [low, high],
      };
      if (rule.lodBias !== undefined) mapped.lodBias = bounded(rule.lodBias, [0, 8], `scene.species[${index}].lodBias`);
      return mapped;
    }),
    budget: { maxInstances: 20000 },
  };
}

// Files whose contents decide what a cook produces. Hashing them means the build cache invalidates
// when the geometry/material code changes, without anyone remembering to bump a version constant.
const COOK_SOURCES = [
  'engine/vegetation/species.mjs',
  'engine/vegetation/branch-graph.mjs',
  'engine/vegetation/branch-mesh.mjs',
  'engine/vegetation/leaf-placement.mjs',
  'engine/vegetation/atlas.mjs',
  'engine/vegetation/material-contract.mjs',
  'engine/vegetation/generators.mjs',
  'engine/vegetation/texture.mjs',
];

export function createVegetationService({ root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..') } = {}) {
  const projectRoot = path.resolve(root);
  const sources = path.join(projectRoot, SOURCE_DIRECTORY);
  const fixtures = path.join(projectRoot, FIXTURE_DIRECTORY);
  const cooked = path.join(projectRoot, COOKED_DIRECTORY);

  // Undo/redo stacks of raw source text, per species. They are process state on purpose: an
  // editor session's undo history should not survive a restart and quietly restore a document
  // the user has since overwritten from another tool.
  const undoHistory = new Map();
  const redoHistory = new Map();
  const pushHistory = (map, id, entry) => {
    const stack = map.get(id) ?? [];
    stack.push(entry);
    while (stack.length > MAX_HISTORY) stack.shift();
    map.set(id, stack);
  };

  function resolveSpeciesFile(id) {
    if (typeof id !== 'string' || !ID.test(id)) throw fail('INVALID_ARGUMENTS', 'species id is not a valid identifier');
    return path.join(sources, `${id}.species.json`);
  }

  async function readSource(id) {
    const authored = resolveSpeciesFile(id);
    try {
      const spec = await readJson(authored);
      return { spec, authored: true, revision: revisionOf(spec), file: authored };
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
    const fixture = path.join(fixtures, `${id}.species.json`);
    try {
      const spec = await readJson(fixture);
      return { spec, authored: false, revision: revisionOf(spec), file: fixture };
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
    throw fail('NOT_FOUND', `找不到物种 ${id}`);
  }

  async function listSources() {
    const found = new Map();
    for (const [directory, authored] of [[fixtures, false], [sources, true]]) {
      let entries = [];
      try { entries = await fs.readdir(directory); } catch (error) { if (error.code !== 'ENOENT') throw error; }
      for (const name of entries) {
        if (!name.endsWith('.species.json')) continue;
        const id = name.slice(0, -'.species.json'.length);
        if (!ID.test(id)) continue;
        const stat = await fs.stat(path.join(directory, name)).catch(() => null);
        // Authored sources win over fixtures: the editor shows what it last saved.
        if (found.has(id) && !authored) continue;
        found.set(id, { id, authored, modified: stat ? stat.mtime.toISOString() : null });
      }
    }
    return [...found.values()].sort((a, b) => a.id.localeCompare(b.id));
  }

  async function cookedLibrary() {
    try { return await readJson(path.join(cooked, 'library.json')); } catch (error) {
      if (error.code === 'ENOENT') return null;
      throw error;
    }
  }

  async function speciesList() {
    const [list, library] = await Promise.all([listSources(), cookedLibrary()]);
    const baked = new Map((library?.species ?? []).map((entry) => [entry.id, entry]));
    return {
      species: list.map((entry) => {
        const cookedEntry = baked.get(entry.id) ?? null;
        return {
          ...entry,
          baked: cookedEntry ? {
            triangles: cookedEntry.triangles,
            lodTriangles: cookedEntry.lodTriangles,
            lodErrorMetres: cookedEntry.lodErrorMetres,
            bytes: cookedEntry.bytes,
            hash: cookedEntry.hash,
          } : null,
        };
      }),
      cookedLibraryHash: library?.packageHash ?? null,
    };
  }

  // Merging one entry into the cooked library is what makes the Bake button mean something:
  // the runtime reads this file. Only the same id is replaced; nothing else is dropped.
  async function mergeCookedLibrary(entry) {
    await fs.mkdir(cooked, { recursive: true });
    const file = path.join(cooked, 'library.json');
    let library = null;
    try { library = await readJson(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const species = (library?.species ?? []).filter((candidate) => candidate.id !== entry.id);
    species.push(entry);
    species.sort((a, b) => String(a.id).localeCompare(String(b.id)));
    const next = {
      schema: library?.schema ?? 'nativegpu.vegetation-library/1',
      species,
      regions: library?.regions ?? [],
      outputDirectory: cooked,
    };
    await fs.writeFile(file, JSON.stringify(next, null, 2));
    return next;
  }

  // Regions live in the same manifest as species but under their own key: publishing one must
  // not disturb the species entries the Bake button owns, and vice versa.
  async function mergeCookedRegion(entry) {
    await fs.mkdir(cooked, { recursive: true });
    const file = path.join(cooked, 'library.json');
    let library = null;
    try { library = await readJson(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const regions = (library?.regions ?? []).filter((candidate) => candidate.id !== entry.id);
    regions.push(entry);
    regions.sort((a, b) => String(a.id).localeCompare(String(b.id)));
    const next = {
      schema: library?.schema ?? 'nativegpu.vegetation-library/1',
      species: library?.species ?? [],
      regions,
      outputDirectory: cooked,
    };
    await writeAtomic(file, `${JSON.stringify(next, null, 2)}\n`);
    return next;
  }

  // Publishing a region is the step between "the species is baked" and "there is a scene": until
  // it runs, the cooked library holds packages that no page set references. It writes the
  // authored source, the `.ngvrg` package and the manifest entry, and it refuses to reference a
  // species whose bytes no longer match their digest — the same rule the CLI cook enforces, so a
  // published region never points at a package that changed underneath it.
  async function publishRegion(args) {
    // Two ways in: a full region document (what the CLI cooks) or a compact scene description
    // that the editor's panel can produce without knowing the region schema. Keeping the builder
    // here rather than in the UI is what makes the numbers testable at all.
    const region = args.region ?? regionFromScene(args.scene);
    if (region.schema !== REGION_SCHEMA) throw fail('NG-E-VEG-SCATTER', `region schema must be ${REGION_SCHEMA}`);
    if (!ID.test(String(region.id))) throw fail('NG-E-VEG-SCATTER', 'region.id is not a valid identifier');
    const started = Date.now();
    const library = await cookedLibrary();
    if (!Array.isArray(library?.species) || library.species.length === 0) {
      throw fail('NG-E-VEG-SPECIES', 'no cooked species to publish against; bake a species first');
    }
    const species = [];
    for (const entry of library.species) {
      const bytes = await fs.readFile(path.join(cooked, entry.file));
      const digest = sha256Hex(bytes);
      if (digest !== entry.hash) throw fail('NG-E-VEG-HASH', `species ${entry.id} bytes do not match its manifest digest`);
      const parsed = readPackage(bytes);
      if (parsed.schema !== SPECIES_COOK_SCHEMA) throw fail('NG-E-VEG-FORMAT', `species ${entry.id} has schema ${parsed.schema}`);
      if (parsed.meta.id !== entry.id) throw fail('NG-E-VEG-FORMAT', `species ${entry.id} package identifies itself as ${parsed.meta.id}`);
      species.push({ id: entry.id, hash: entry.hash, meta: parsed.meta });
    }

    const previous = (library.regions ?? []).find((candidate) => candidate.id === region.id) ?? null;
    // Same contract as saving a species: the caller echoes the revision it edited from, and a
    // mismatch is a conflict rather than a silent overwrite of somebody else's publish.
    if (args.expectedRevision !== undefined && args.expectedRevision !== null
        && args.expectedRevision !== (previous?.hash ?? null)) {
      throw fail('NG-E-VEG-CONFLICT', `region ${region.id} is at revision ${previous?.hash ?? 'unpublished'}`
        + `; expected ${args.expectedRevision}`);
    }

    const compiled = compileRegion({ region, species });
    const directory = path.join(cooked, 'regions');
    await fs.mkdir(directory, { recursive: true });
    const file = `${compiled.hash}.ngvrg`;
    const target = path.join(directory, file);
    // Content-addressed, so a re-publish of the same region lands on the same name; the temp file
    // keeps a crash from leaving a half-written package under a hash that claims success.
    const temporary = `${target}.${process.pid}.tmp`;
    await fs.writeFile(temporary, compiled.bytes);
    await fs.rename(temporary, target);

    const entry = {
      id: compiled.id,
      hash: compiled.hash,
      file: path.posix.join('regions', file),
      instances: compiled.instances,
      pages: compiled.pages,
      instanceBytes: compiled.instanceBytes,
      totalBytes: compiled.bytes.byteLength,
      density: compiled.meta.density,
      counts: compiled.meta.counts,
      quantization: compiled.meta.quantization,
    };
    const next = await mergeCookedRegion(entry);
    // Source last: if this step fails the package is already consistent and re-publishing the
    // same document is idempotent, whereas a source without a package would look authored but
    // be unloadable.
    await fs.mkdir(path.join(projectRoot, REGION_DIRECTORY), { recursive: true });
    await writeAtomic(path.join(projectRoot, REGION_DIRECTORY, `${region.id}.region.json`),
      `${JSON.stringify(region, null, 2)}\n`);
    return { ...entry, revision: entry.hash, elapsedMs: Date.now() - started, regions: next.regions.length };
  }

  async function loadSpecies(id) {
    const { spec, authored, revision } = await readSource(id);
    const [library, textures] = await Promise.all([cookedLibrary(), textureLibrary()]);
    const baked = (library?.species ?? []).find((entry) => entry.id === id) ?? null;
    return {
      authored,
      // The revision the editor must echo back on save. Without it, two windows editing the same
      // species silently overwrite each other.
      revision,
      spec: {
        ...spec,
        // The editor always sees a full graph, even for a species cooked from the legacy
        // recipe — otherwise the tree panel would be empty for most existing content.
        generators: withDefaults(spec),
      },
      baked: baked ? { hash: baked.hash, triangles: baked.triangles, lodTriangles: baked.lodTriangles } : null,
      // Real material data for the inspector: the cooked roles (with digest, semantic, colour
      // space, channels, mip count) and the slots this species binds. The previous value was
      // `library ? null : null`, which is a placeholder that could never be filled.
      textureRoles: textures ? {
        packageHash: textures.packageHash ?? null,
        roles: (textures.roles ?? []).map((role) => ({
          role: role.role, semantic: role.semantic ?? null, colorSpace: role.colorSpace ?? null,
          channels: role.channels ?? null, normalConvention: role.normalConvention ?? null,
          digest: role.digest, width: role.width, height: role.height, mipCount: role.mipCount,
          filter: role.filter ?? null, coverageCutoff: role.coverageCutoff ?? null,
          source: role.source ?? null,
        })),
        slots: spec.textures ?? null,
      } : null,
    };
  }

  function preview(args) {
    const spec = args.spec;
    if (!spec || typeof spec !== 'object') throw fail('INVALID_ARGUMENTS', 'spec is required');
    const level = Number(args.level ?? 0);
    const result = buildPreview(spec, {
      level,
      analysis: args.analysis === true,
      leafAtlas: args.leafAtlas ?? null,
    });
    const geometry = result.geometry;
    return {
      id: result.id,
      level: result.level,
      levels: result.levels,
      vertices: result.vertices,
      triangles: result.triangles,
      bounds: result.bounds,
      budget: result.budget,
      overBudget: result.overBudget,
      primitives: result.primitives,
      lodTable: result.lodTable ?? null,
      // Structure is what makes a preview auditable: branch count, measured attachment gap and
      // per-frond leaf counts. The viewport cannot show "the fork is attached"; these numbers
      // can. `null` means the preview did not produce a graph, never "fine".
      structure: result.structure ?? null,
      leaves: result.leaves ?? null,
      atlas: result.atlas ?? null,
      byteLength: geometry.positions.byteLength + geometry.normals.byteLength + geometry.uvs.byteLength
        + geometry.windWeights.byteLength + geometry.colors.byteLength + geometry.indices.byteLength,
      geometry: {
        positions: base64(geometry.positions),
        normals: base64(geometry.normals),
        uvs: base64(geometry.uvs),
        windWeights: base64(geometry.windWeights),
        colors: base64(geometry.colors),
        indices: base64(geometry.indices),
      },
    };
  }

  async function saveSpecies(args) {
    const spec = args.spec;
    if (!spec || typeof spec !== 'object') throw fail('INVALID_ARGUMENTS', 'spec is required');
    // Validate through the generator so an impossible graph is refused here, with a message,
    // rather than at bake time.
    buildPreview(spec, { level: 0 });
    await fs.mkdir(sources, { recursive: true });
    const file = resolveSpeciesFile(spec.id);
    const previousText = await fs.readFile(file, 'utf8').catch((error) => {
      if (error.code === 'ENOENT') return null;
      throw error;
    });
    // A species that has never been authored lives as a build fixture. The editor loaded that
    // fixture, so its revision is the base this save is checked against and the content undo
    // returns to. `removeAuthored` tells undo that "back to the fixture" means "delete this file",
    // not "write these bytes".
    let baseText = previousText;
    let baseRevision = previousText === null ? null : revisionOf(JSON.parse(previousText.replace(/^\uFEFF/, '')));
    let firstAuthoring = false;
    if (baseText === null) {
      const fixtureText = await fs.readFile(path.join(fixtures, `${spec.id}.species.json`), 'utf8').catch(() => null);
      if (fixtureText !== null) {
        baseText = fixtureText;
        baseRevision = revisionOf(JSON.parse(fixtureText.replace(/^\uFEFF/, '')));
        firstAuthoring = true;
      }
    }

    // Conflict detection. `expectedRevision: null` means "this is a deliberate overwrite"; any
    // other value must match the content the editor actually loaded, or the write is refused and
    // the caller is told which revision the project has.
    if (args.expectedRevision !== undefined && args.expectedRevision !== null
      && args.expectedRevision !== baseRevision) {
      const conflict = fail('REVISION_CONFLICT',
        `物种 ${spec.id} 已被其他编辑写入：期望 ${String(args.expectedRevision).slice(0, 12)}，当前 ${String(baseRevision ?? 'none').slice(0, 12)}`);
      conflict.details = [{ currentRevision: baseRevision, expectedRevision: args.expectedRevision }];
      throw conflict;
    }

    const payload = { ...spec };
    delete payload.__file;
    delete payload.__revision;
    const text = `${JSON.stringify(payload, null, 2)}\n`;
    const revision = revisionOf(payload);
    // The previous bytes go on the undo stack before they are replaced. A save that changes
    // nothing is not an undo step: pressing Ctrl+Z after a no-op save should not appear to do
    // nothing at all.
    if (baseText !== null && baseRevision !== revision) {
      pushHistory(undoHistory, spec.id, { revision: baseRevision, text: baseText, remove: firstAuthoring });
      redoHistory.delete(spec.id);
    }
    await writeAtomic(file, text);
    return {
      id: spec.id,
      path: path.relative(projectRoot, file).split(path.sep).join('/'),
      revision,
      previousRevision: baseRevision,
      firstAuthoring,
      bytes: Buffer.byteLength(text),
      canUndo: (undoHistory.get(spec.id) ?? []).length > 0,
      canRedo: (redoHistory.get(spec.id) ?? []).length > 0,
    };
  }

  // Undo/redo are content snapshots, not command logs: restoring the exact bytes that were on
  // disk before the last save is the only way "undo then reload" reproduces the previous document
  // bit for bit. The stacks live in this process, so they are per editor session by design.
  async function restoreFrom(history, other, args) {
    const id = args.id;
    if (typeof id !== 'string' || !ID.test(id)) throw fail('INVALID_ARGUMENTS', 'species id is not a valid identifier');
    const stack = history.get(id) ?? [];
    if (!stack.length) throw fail('NOTHING_TO_RESTORE', `物种 ${id} 没有可恢复的历史`);
    const file = resolveSpeciesFile(id);
    const currentText = await fs.readFile(file, 'utf8').catch(() => null);
    const entry = stack.pop();
    const currentRevision = currentText === null
      ? null
      : revisionOf(JSON.parse(currentText.replace(/^\uFEFF/, '')));
    if (currentText !== null) pushHistory(other, id, { revision: currentRevision, text: currentText });
    if (entry.remove) {
      // Undoing the very first save puts the species back to "not authored": the fixture is the
      // document again, and leaving a copy behind would make the next load disagree with undo.
      await fs.unlink(file).catch((error) => { if (error.code !== 'ENOENT') throw error; });
    } else {
      await writeAtomic(file, entry.text);
    }
    const spec = JSON.parse(entry.text.replace(/^\uFEFF/, ''));
    return {
      id,
      revision: entry.revision,
      spec: { ...spec, generators: withDefaults(spec) },
      restoredRevision: entry.revision,
      discardedRevision: currentRevision,
      canUndo: (history.get(id) ?? []).length > 0,
      canRedo: (other.get(id) ?? []).length > 0,
    };
  }

  const undoSpecies = (args) => restoreFrom(undoHistory, redoHistory, args);
  const redoSpecies = (args) => restoreFrom(redoHistory, undoHistory, args);

  async function textureLibrary() {
    // Optional, exactly like the CLI: a species with no `textures` block stays vertex
    // coloured, and one that declares roles without a library to resolve them against is a
    // real error rather than a silently untextured bake.
    try { return await readJson(path.join(projectRoot, 'build/vegetation/textures/library.json')); }
    catch (error) { if (error.code === 'ENOENT') return null; throw error; }
  }

  // Fingerprint of the cook code. Computed once per service instance: the files do not change
  // while the editor is running, and hashing eight files per Bake would be wasted I/O on a
  // button the user presses repeatedly.
  let toolFingerprint = null;
  async function computeToolFingerprint() {
    if (toolFingerprint) return toolFingerprint;
    const digests = [];
    for (const relative of COOK_SOURCES) {
      try {
        digests.push(sha256Hex(await fs.readFile(path.join(projectRoot, relative), 'utf8')));
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        // A missing cook source is not a cache problem; record it so the fingerprint still
        // changes if the file appears later.
        digests.push(`missing:${relative}`);
      }
    }
    toolFingerprint = sha256Hex(digests.join(':')).slice(0, 32);
    return toolFingerprint;
  }

  // What a cooked package depends on: the source document, the cooked texture library and the
  // code that turns them into bytes. Anything else (the clock, the editor session) must not
  // change it, or the cache would never hit.
  async function cookFingerprint(spec, textures) {
    return sha256Hex(canonicalJson({
      source: revisionOf(spec),
      textures: textures?.packageHash ?? null,
      tool: await computeToolFingerprint(),
    })).slice(0, 32);
  }

  async function cookSpecies(args) {
    const spec = args.spec;
    if (!spec || typeof spec !== 'object') throw fail('INVALID_ARGUMENTS', 'spec is required');
    const started = Date.now();
    const textures = await textureLibrary();
    const fingerprint = await cookFingerprint(spec, textures);

    // Cache hit: the bytes on disk already correspond to this source, this texture library and
    // this cook code, so re-baking would produce the same file under the same name.
    if (args.force !== true) {
      const library = await cookedLibrary();
      const existing = (library?.species ?? []).find((entry) => entry.id === spec.id);
      if (existing && existing.fingerprint === fingerprint) {
        const file = path.join(cooked, existing.file);
        const present = await fs.stat(file).then((stat) => stat.size > 0).catch(() => false);
        if (present) {
          return { ...existing, cached: true, fingerprint, elapsedMs: Date.now() - started };
        }
      }
    }

    const result = compileSpecies(spec, { textures });
    await fs.mkdir(cooked, { recursive: true });
    const file = `${result.hash}.ngveg`;
    await fs.writeFile(path.join(cooked, file), result.bytes);
    const entry = {
      id: result.id,
      hash: result.hash,
      file,
      vertices: result.vertices,
      triangles: result.triangles,
      lodTriangles: result.lodTriangles,
      lodErrorMetres: result.lodErrorMetres,
      bytes: result.bytesTotal,
      // Published so the next Bake can tell whether anything that matters changed.
      fingerprint,
    };
    const library = await mergeCookedLibrary(entry);
    return {
      ...entry,
      cached: false,
      elapsedMs: Date.now() - started,
      libraryHash: library.species.length,
      impostor: result.meta.impostor,
      // Surfaced from the package so the Bake button can say "the crown the tree grew disagrees
      // with its declaration by N%". The generator no longer clamps onto the declaration, so this
      // is the only place an author would otherwise never hear about the disagreement.
      crown: result.meta.structure?.crown ?? null,
      representations: result.meta.representations,
      representationsNote: '图集在包内已烘焙；视口里的 4 视图是同一网格的多机位预览，不是包内图集。',
    };
  }

  async function impostorInfo(args) {
    const spec = args.spec;
    if (!spec || typeof spec !== 'object') throw fail('INVALID_ARGUMENTS', 'spec is required');
    const result = buildPreview(spec, { level: Math.min(1, (spec.lod?.levels ?? 1) - 1) });
    return {
      views: spec.impostor?.views ?? 8,
      size: spec.impostor?.size ?? 128,
      sourceTriangles: result.triangles,
      // Declared, not produced: the atlas itself is baked by compileSpecies. Saying so is the
      // difference between a preview and a lie.
      atlas: '烘焙时由 compileSpecies 生成，视口预览为多机位渲染',
    };
  }

  const actions = {
    'vegetation.list': () => speciesList(),
    'vegetation.load': (args) => loadSpecies(args.id),
    'vegetation.preview': (args) => preview(args),
    'vegetation.save': (args) => saveSpecies(args),
    'vegetation.undo': (args) => undoSpecies(args),
    'vegetation.redo': (args) => redoSpecies(args),
    'vegetation.cook': (args) => cookSpecies(args),
    'vegetation.publish': (args) => publishRegion(args),
    'vegetation.impostor': (args) => impostorInfo(args),
    'vegetation.defaults': (args) => defaultGenerators(args.spec ?? {}),
  };

  return {
    root: projectRoot,
    handles(action) {
      return Object.hasOwn(actions, action);
    },
    async call(action, args = {}) {
      try {
        const handler = actions[action];
        if (!handler) throw fail('UNKNOWN_ACTION', `Unknown vegetation action: ${action}`);
        return { ok: true, result: await handler(args) };
      } catch (error) {
        return {
          ok: false,
          error: {
            code: error.code ?? 'EDITOR_FAILURE',
            message: error.message ?? String(error),
            details: error.details ?? [],
          },
        };
      }
    },
  };
}

#!/usr/bin/env node
// World production entry: create, cook, inspect and package a world.
//
// This is a *production* command, not a test script. The incremental test entry points live
// under .incrementalTesting/; this file is what a user or a build machine runs.
//
//   node tools/world.mjs new     --project <dir> --world <id> [--extent 256] [--preset mountain-valley]
//   node tools/world.mjs cook    --project <dir> --world <id> [--worker <exe>] [--no-cache]
//   node tools/world.mjs info    --project <dir> --world <id>
//   node tools/world.mjs verify  --project <dir> --world <id>
//   node tools/world.mjs package --project <dir> --world <id> --out <dir>

import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

import {
  DEFAULT_WORKER, cookWorld, readManifest, readRecipe, releaseClosure, writeLayer, writeLibrary,
} from '../engine/world/cook.mjs';
import { defaultLibrary } from '../engine/world/library.mjs';
import { GENERATOR_VERSION, LAYER_SCHEMA, RECIPE_SCHEMA, normalizeRecipe } from '../engine/world/schema.mjs';
import { WORLD_LAYOUT, recipePath, resolveProjectPath } from '../engine/world/paths.mjs';
import { WorldError } from '../engine/world/errors.mjs';
import { sha256Hex } from '../engine/world/hash.mjs';

function parseArguments(argv) {
  const options = { command: argv[0] ?? 'help' };
  for (let index = 1; index < argv.length; index += 1) {
    const token = argv[index];
    if (!token.startsWith('--')) continue;
    const key = token.slice(2);
    const next = argv[index + 1];
    if (next === undefined || next.startsWith('--')) {
      options[key] = true;
    } else {
      options[key] = next;
      index += 1;
    }
  }
  return options;
}

function requireOption(options, key) {
  const value = options[key];
  if (typeof value !== 'string' || value.length === 0) {
    throw new WorldError('WORLD_SCHEMA_INVALID', `--${key} is required`, { suggestion: `Pass --${key} <value>.` });
  }
  return value;
}

function printHelp() {
  console.log(`NativeEngine world tool

  node tools/world.mjs new     --project <dir> --world <id> [--extent 256] [--preset mountain-valley]
  node tools/world.mjs cook    --project <dir> --world <id> [--worker <exe>] [--no-cache]
  node tools/world.mjs info    --project <dir> --world <id>
  node tools/world.mjs verify  --project <dir> --world <id>
  node tools/world.mjs package --project <dir> --world <id> --out <dir>

The world lives in <project>/.screen/world/. Cooked chunks live in
<project>/assets/world-cooked/<manifestHash>/ and are immutable and content addressed.`);
}

// A complete, cookable first world: mountains and a valley, a river down the middle, a road
// that crosses it on a bridge, a forest on the east slope and a small settlement where the
// road meets the water. Every number here is a starting point the editor then edits.
async function createWorld(projectRoot, worldId, extent, preset) {
  const half = extent / 2;
  const library = defaultLibrary('ancient-kit');
  const libraryRef = await writeLibrary(projectRoot, library);

  const terrainLayer = {
    schema: LAYER_SCHEMA,
    id: 'terrain-base',
    kind: 'terrain',
    rules: { preset, amplitudeMetres: Math.max(20, extent * 0.06), baseHeightMetres: 4, ridgeStrength: 0.55 },
    regions: [],
  };
  const waterLayer = {
    schema: LAYER_SCHEMA,
    id: 'river-main',
    kind: 'water',
    rules: { widthMetres: Math.max(10, extent * 0.012), bedDepthMetres: 1.4, flowMetresPerSecond: 1.6 },
    paths: [{
      id: 'river-main',
      kind: 'river',
      points: [
        [-half * 0.85, -half * 0.9],
        [-half * 0.3, -half * 0.45],
        [0, 0],
        [half * 0.28, half * 0.42],
        [half * 0.8, half * 0.85],
      ],
      widthMetres: Math.max(10, extent * 0.012),
      bedDepthMetres: 1.4,
    }],
  };
  const roadLayer = {
    schema: LAYER_SCHEMA,
    id: 'road-main',
    kind: 'road',
    rules: { widthMetres: 5, shoulderMetres: 0.8, maxSlopeDegrees: 18 },
    paths: [{
      id: 'road-main',
      kind: 'road',
      points: [[-half * 0.9, half * 0.08], [-half * 0.3, half * 0.03], [0, 0], [half * 0.35, -half * 0.05], [half * 0.9, -half * 0.12]],
      widthMetres: 5,
      maxSlopeDegrees: 18,
    }],
  };
  const forestLayer = {
    schema: LAYER_SCHEMA,
    id: 'forest-east',
    kind: 'scatter',
    rules: {
      species: 'pine-ancient', densityPerHectare: 140,
      maxSlopeDegrees: 36, minSpacingMetres: 5, clumping: 0.35, scaleRange: [0.85, 1.25],
    },
    regions: [{
      shape: 'circle', center: [half * 0.35, 0], radiusMetres: half * 0.6, falloffMetres: half * 0.18, strength: 1,
    }],
  };
  const meadowLayer = {
    schema: LAYER_SCHEMA,
    id: 'meadow',
    kind: 'scatter',
    rules: {
      species: 'grass-tuft', densityPerHectare: 18000,
      maxSlopeDegrees: 34, alignToSlope: true, scaleRange: [0.8, 1.3],
    },
    regions: [],
  };
  const rockLayer = {
    schema: LAYER_SCHEMA,
    id: 'boulders',
    kind: 'scatter',
    rules: {
      species: 'granite-boulder', densityPerHectare: 26,
      maxSlopeDegrees: 46, minSpacingMetres: 4, scaleRange: [0.7, 1.6],
    },
    regions: [],
  };
  const villageLayer = {
    schema: LAYER_SCHEMA,
    id: 'village',
    kind: 'settlement',
    rules: {
      components: ['courtyard-house', 'two-storey-inn'], density: 0.72, lotSizeMetres: 22, setbackMetres: 2.5,
      roadWidthMetres: 5, maxBuildings: 48, entranceClearanceMetres: 3.5,
    },
    regions: [{
      shape: 'circle', center: [0, 0], radiusMetres: Math.max(60, extent * 0.22),
    }],
  };

  const layers = [];
  for (const layer of [terrainLayer, waterLayer, roadLayer, forestLayer, meadowLayer, rockLayer, villageLayer]) {
    layers.push(await writeLayer(projectRoot, layer));
  }

  const recipe = normalizeRecipe({
    schema: RECIPE_SCHEMA,
    worldId,
    generatorVersion: GENERATOR_VERSION,
    seed: '947210',
    extentMetres: [extent, extent],
    originMetres: [-half, 0, -half],
    chunkSizeMetres: extent <= 512 ? 128 : 128,
    terrain: { sampleStepMetres: 1, preset, amplitudeMetres: Math.max(20, extent * 0.06), baseHeightMetres: 4 },
    libraries: [libraryRef],
    layers,
    overrides: [],
    budgetProfile: 'desktop-balanced',
  });

  const relative = recipePath(worldId);
  const resolved = await resolveProjectPath(projectRoot, relative);
  await fs.mkdir(path.dirname(resolved.absolute), { recursive: true });
  const text = JSON.stringify(recipe, null, 2);
  await fs.writeFile(resolved.absolute, text, 'utf8');

  return { recipe, relative, sha256: sha256Hex(Buffer.from(text, 'utf8')), libraryRef, layers };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options.command === 'help' || options['--help']) {
    printHelp();
    return 0;
  }

  const projectRoot = path.resolve(requireOption(options, 'project'));
  const worldId = requireOption(options, 'world');

  if (options.command === 'new') {
    const extent = Number(options.extent ?? 256);
    const preset = typeof options.preset === 'string' ? options.preset : 'mountain-valley';
    const created = await createWorld(projectRoot, worldId, extent, preset);
    console.log(`created ${created.relative} (${extent} m, ${preset})`);
    console.log(`  library ${created.libraryRef.id} -> ${created.libraryRef.file}`);
    for (const layer of created.layers) console.log(`  layer   ${layer.id} -> ${layer.file}`);
    return 0;
  }

  if (options.command === 'cook') {
    const recipe = await readRecipe(projectRoot, worldId);
    const result = await cookWorld({
      projectRoot,
      recipe,
      worker: typeof options.worker === 'string' ? options.worker : DEFAULT_WORKER,
      keepCache: options['no-cache'] !== true,
    });
    console.log(`cooked ${worldId}`);
    console.log(`  manifest ${result.manifestHash}`);
    console.log(`  input    ${result.digest}`);
    console.log(`  chunks   ${result.manifest.chunks.length} in ${result.relativeDirectory}`);
    console.log(`  instances ${result.manifest.budget.totalInstances}, buildings ${result.manifest.stats?.buildings ?? 0}`);
    console.log(`  cook     ${Math.round(result.manifest.budget.cookMillis)} ms`);
    for (const warning of result.summary.warnings ?? []) console.log(`  warning  ${warning}`);
    for (const diagnostic of result.summary.pathDiagnostics ?? []) console.log(`  path     ${diagnostic.code}: ${diagnostic.message}`);
    for (const diagnostic of result.summary.settlementDiagnostics ?? []) {
      console.log(`  lot      ${diagnostic.code}: ${diagnostic.message}`);
    }
    return 0;
  }

  if (options.command === 'info') {
    const recipe = await readRecipe(projectRoot, worldId);
    console.log(`world ${recipe.worldId} (${recipe.generatorVersion})`);
    console.log(`  extent   ${recipe.extentMetres[0]} x ${recipe.extentMetres[1]} m`);
    console.log(`  origin   ${recipe.originMetres.join(', ')}`);
    console.log(`  chunks   ${recipe.chunkSizeMetres} m`);
    console.log(`  seed     ${recipe.seed}`);
    console.log(`  budget   ${recipe.budgetProfile}`);
    console.log(`  library  ${recipe.libraries.map(entry => entry.id).join(', ') || '(none)'}`);
    console.log(`  layers   ${recipe.layers.map(entry => entry.id).join(', ') || '(none)'}`);
    return 0;
  }

  if (options.command === 'verify') {
    const recipe = await readRecipe(projectRoot, worldId);
    const result = await cookWorld({ projectRoot, recipe, keepCache: true });
    const manifest = await readManifest(result.directory);
    const closure = await releaseClosure(result.directory, manifest);
    console.log(`verified ${worldId}`);
    console.log(`  manifest ${manifest.manifestHash}`);
    console.log(`  chunks   ${closure.chunks}`);
    console.log(`  bytes    ${closure.bytes}`);
    let instances = 0;
    for (const chunk of manifest.chunks) instances += chunk.instanceCount;
    console.log(`  instances ${instances}`);
    return 0;
  }

  if (options.command === 'package') {
    const outDir = path.resolve(requireOption(options, 'out'));
    const recipe = await readRecipe(projectRoot, worldId);
    const result = await cookWorld({ projectRoot, recipe, keepCache: true });
    const manifest = await readManifest(result.directory);
    const closure = await releaseClosure(result.directory, manifest);
    await fs.mkdir(outDir, { recursive: true });
    for (const file of closure.files) {
      const target = path.join(outDir, file.relative);
      await fs.mkdir(path.dirname(target), { recursive: true });
      await fs.copyFile(file.source, target);
    }
    // The release carries the manifest and the chunks only. No recipe, no layers, no
    // libraries: a published world must not need the authoring sources to run.
    const summary = {
      schema: 'nativegpu.world-package/1',
      worldId: manifest.worldId,
      manifestHash: manifest.manifestHash,
      chunks: closure.chunks,
      bytes: closure.bytes,
      generatorVersion: manifest.generatorVersion,
    };
    await fs.writeFile(path.join(outDir, 'package.json'), JSON.stringify(summary, null, 2), 'utf8');
    console.log(`packaged ${worldId} -> ${outDir}`);
    console.log(`  chunks ${closure.chunks}, ${closure.bytes} bytes`);
    return 0;
  }

  printHelp();
  return 2;
}

main().then(code => process.exit(code)).catch(error => {
  if (error instanceof WorldError) {
    console.error(`world: ${error.code}: ${error.message}`);
    if (error.details?.suggestion) console.error(`  ${error.details.suggestion}`);
    for (const cause of error.details?.cause ?? []) console.error(`  - ${cause}`);
  } else {
    console.error(error.stack ?? error.message);
  }
  process.exit(1);
});

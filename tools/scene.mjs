#!/usr/bin/env node
import * as fs from 'node:fs/promises';
import {
  SceneError, validateScene, loadScene, queryScene, previewTransaction,
  commitScene, undoSceneFile, buildScene, recoverScene,
} from '../engine/scene/index.mjs';
import { lintScene } from '../engine/scene/lint.mjs';
import { queryScene as querySceneLanguage } from '../engine/scene/query.mjs';

const USAGE = `Scene tools (JSON output)
  node tools/scene.mjs validate <scene.json>
  node tools/scene.mjs query <scene.json> [--kind prop] [--name text] [--id uuid]
  node tools/scene.mjs query <scene.json> --query '<json>' | --file <query.json>
  node tools/scene.mjs preview <scene.json> <transaction.json>
  node tools/scene.mjs apply <scene.json> <transaction.json>
  node tools/scene.mjs undo <scene.json> --project <uuid> --revision <integer>
  node tools/scene.mjs recover <scene.json>
  node tools/scene.mjs build <scene.json> <output-directory>
  node tools/scene.mjs lint <scene.json> [--json] [--max-entities N]
Transactions: {projectId, expectedRevision, commands: [{op:"set", entityId, changes:{position:[0,1,0]}}]}
lint: --json is the default (and only) output format; exits 1 if any error-severity finding exists.
query: --query/--file uses the typed query language (docs/SCENE-QUERY.md); exits 1 on NG-E-QUERY-* errors.
`;

function fail(message) { throw new SceneError('INVALID_ARGUMENT', message); }
function options(args, allowed) {
  if (args.length % 2) fail('Every option requires a value');
  const parsed = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!allowed.includes(args[i]) || Object.hasOwn(parsed, args[i])) fail(`Unknown or duplicate option: ${args[i]}`);
    parsed[args[i]] = args[i + 1];
  }
  return parsed;
}
function lintOptions(args) {
  const parsed = {};
  for (let i = 0; i < args.length; i += 1) {
    if (args[i] === '--json') {
      if (Object.hasOwn(parsed, '--json')) fail('Duplicate option: --json');
      parsed['--json'] = true;
      continue;
    }
    if (args[i] === '--max-entities') {
      if (Object.hasOwn(parsed, '--max-entities')) fail('Duplicate option: --max-entities');
      const value = args[i + 1];
      if (value === undefined) fail('--max-entities requires a value');
      parsed['--max-entities'] = value;
      i += 1;
      continue;
    }
    fail(`Unknown option: ${args[i]}`);
  }
  if (parsed['--max-entities'] !== undefined
      && (!/^[1-9]\d*$/.test(parsed['--max-entities']) || !Number.isSafeInteger(Number(parsed['--max-entities'])))) {
    fail('--max-entities must be a positive integer');
  }
  return parsed;
}
async function json(file) {
  const stat = await fs.stat(file);
  if (stat.size > 16 * 1024 * 1024) throw new SceneError('FILE_TOO_LARGE', 'Input JSON exceeds 16 MiB');
  try { return JSON.parse(await fs.readFile(file, 'utf8')); }
  catch (error) {
    if (error instanceof SyntaxError) throw new SceneError('INVALID_JSON', `Invalid JSON in ${file}`);
    throw error;
  }
}
async function main() {
  const [command, file, ...args] = process.argv.slice(2);
  if (command === '--help' || command === 'help' || !command) return { help: USAGE };
  if (!file) fail('A scene path is required');
  if (command === 'validate') {
    if (args.length) fail('validate takes only a scene path');
    const scene = await json(file);
    const result = validateScene(scene);
    if (!result.valid) throw new SceneError('INVALID_SCENE', 'Scene validation failed', result.errors);
    return { valid: true, projectId: scene.projectId, revision: scene.revision, entityCount: scene.entities.length };
  }
  if (command === 'lint') {
    const opt = lintOptions(args);
    const result = lintScene(await json(file), opt['--max-entities'] === undefined
      ? {} : { maxEntities: Number(opt['--max-entities']) });
    // Findings are a successful command with a failing gate: keep the envelope
    // shape stable for callers and let the process exit code signal CI.
    if (!result.ok) process.exitCode = 1;
    return result;
  }
  if (command === 'query' && args.some((arg, index) => index % 2 === 0
      && (arg === '--query' || arg === '--file'))) {
    const opt = options(args, ['--query', '--file']);
    if (opt['--query'] !== undefined && opt['--file'] !== undefined) {
      fail('Provide either --query or --file, not both');
    }
    let query;
    if (opt['--query'] !== undefined) {
      try { query = JSON.parse(opt['--query']); }
      catch { throw new SceneError('INVALID_JSON', '--query must contain valid JSON'); }
    } else {
      query = await json(opt['--file']);
    }
    return querySceneLanguage(await loadScene(file), query);
  }
  if (command === 'query') {
    const opt = options(args, ['--kind', '--name', '--id']);
    const query = {};
    if (opt['--kind'] !== undefined) query.kind = opt['--kind'];
    if (opt['--name'] !== undefined) query.name = opt['--name'];
    if (opt['--id'] !== undefined) query.entityId = opt['--id'];
    return queryScene(await loadScene(file), query);
  }
  if (command === 'preview' || command === 'apply') {
    if (args.length !== 1) fail(`${command} takes a scene path and a transaction path`);
    const transaction = await json(args[0]);
    const result = command === 'preview'
      ? previewTransaction(await loadScene(file), transaction) : await commitScene(file, transaction);
    return { projectId: result.projectId, baseRevision: result.baseRevision, revision: result.revision, changes: result.changes, scene: result.scene };
  }
  if (command === 'undo') {
    const opt = options(args, ['--project', '--revision']);
    if (!opt['--project'] || !/^(0|[1-9]\d*)$/.test(opt['--revision'] ?? '')) fail('undo requires --project and a non-negative --revision');
    return undoSceneFile(file, { projectId: opt['--project'], expectedRevision: Number(opt['--revision']) });
  }
  if (command === 'recover') {
    if (args.length) fail('recover takes only a scene path');
    return recoverScene(file);
  }
  if (command === 'build') {
    if (args.length !== 1) fail('build takes a scene path and output directory');
    return buildScene(file, args[0]);
  }
  fail(`Unknown command: ${command}`);
}

try {
  console.log(JSON.stringify({ ok: true, result: await main() }, null, 2));
} catch (error) {
  console.error(JSON.stringify({ ok: false, error: { code: error.code ?? 'IO_ERROR', message: error.message, details: error.details ?? [] } }, null, 2));
  process.exitCode = 1;
}

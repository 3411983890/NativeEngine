#!/usr/bin/env node
import * as fs from 'node:fs/promises';
import {
  SceneError, validateScene, loadScene, queryScene, previewTransaction,
  commitScene, undoSceneFile, buildScene, recoverScene,
} from '../engine/scene/index.mjs';

const USAGE = `Scene tools (JSON output)
  node tools/scene.mjs validate <scene.json>
  node tools/scene.mjs query <scene.json> [--kind prop] [--name text] [--id uuid]
  node tools/scene.mjs preview <scene.json> <transaction.json>
  node tools/scene.mjs apply <scene.json> <transaction.json>
  node tools/scene.mjs undo <scene.json> --project <uuid> --revision <integer>
  node tools/scene.mjs recover <scene.json>
  node tools/scene.mjs build <scene.json> <output-directory>
Transactions: {projectId, expectedRevision, commands: [{op:"set", entityId, changes:{position:[0,1,0]}}]}
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

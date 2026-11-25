import test from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { saveScene, loadScene } from '../engine/scene/index.mjs';
import { createToolService, listTools } from '../ai/tool-service.mjs';
import { createMcpHandler, SERVER_VERSION } from '../ai/mcp-server.mjs';

const PROJECT = 'a1e00000-0000-4000-8000-000000000001';
const PLAYER = 'a1e00000-0000-4000-8000-000000000002';
const GROUND = 'a1e00000-0000-4000-8000-000000000003';
const PROP = 'a1e00000-0000-4000-8000-000000000004';

function scene() {
  return {
    schemaVersion: 1,
    projectId: PROJECT,
    name: 'MCP test',
    revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities: [
      { id: PLAYER, name: 'Player', kind: 'player', position: [0, 0, 5], scale: [0.6, 1.8, 0.6], color: [0.2, 0.8, 0.95], collider: true },
      { id: GROUND, name: 'Ground', kind: 'ground', position: [0, -0.2, 0], scale: [20, 0.4, 20], color: [0.2, 0.2, 0.2], collider: true },
      { id: PROP, name: 'Crate', kind: 'prop', position: [2, 0.6, 2], scale: [1.2, 1.2, 1.2], color: [0.7, 0.4, 0.2], collider: true },
    ],
  };
}

async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-mcp-test-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const file = path.join(root, 'assets', 'courtyard.scene.json');
  await saveScene(file, scene());
  const service = createToolService({ root, executable: path.join(root, 'build', 'NativeGPU.exe') });
  return { root, file, service };
}

test('tool catalog is stable and includes the complete scene/runtime surface', () => {
  const names = listTools().map(tool => tool.name);
  assert.deepEqual(names, [
    'gameplay_catalog', 'gameplay_validate', 'gameplay_inspect',
    'scene_validate', 'scene_query', 'scene_preview', 'scene_apply', 'material_patch',
    'scene_undo', 'scene_build', 'runtime_smoke', 'engine_probe',
  ]);
  for (const tool of listTools()) {
    assert.equal(typeof tool.description, 'string');
    assert.equal(tool.inputSchema.type, 'object');
    assert.equal(tool.inputSchema.additionalProperties, false);
  }
  const copy = listTools();
  copy[0].name = 'mutated';
  assert.equal(listTools()[0].name, 'gameplay_catalog');
});

test('scene tools validate, preview, apply, undo and build without escaping the project', async t => {
  const { root, file, service } = await fixture(t);

  const validated = await service.callTool('scene_validate', { scenePath: 'assets/courtyard.scene.json' });
  assert.equal(validated.ok, true);
  assert.equal(validated.result.valid, true);
  assert.equal(validated.result.entityCount, 3);

  const query = await service.callTool('scene_query', {
    scenePath: 'assets/courtyard.scene.json', kind: 'prop',
  });
  assert.equal(query.ok, true);
  assert.equal(query.result.entities[0].name, 'Crate');

  const tx = {
    projectId: PROJECT,
    expectedRevision: 0,
    commands: [{ op: 'set', entityId: PROP, changes: { position: [-3, 0.6, 1] } }],
  };
  const preview = await service.callTool('scene_preview', {
    scenePath: 'assets/courtyard.scene.json', transaction: tx,
  });
  assert.equal(preview.ok, true);
  assert.equal(preview.result.revision, 1);
  assert.equal(preview.result.scene, undefined);
  assert.equal((await loadScene(file)).revision, 0);

  const applied = await service.callTool('scene_apply', {
    scenePath: 'assets/courtyard.scene.json', transaction: tx,
  });
  assert.equal(applied.ok, true);
  assert.equal(applied.result.revision, 1);
  assert.match(applied.result.sceneHash, /^[0-9a-f]{64}$/);
  assert.equal((await loadScene(file)).entities[2].position[0], -3);

  const stale = await service.callTool('scene_apply', {
    scenePath: 'assets/courtyard.scene.json', transaction: tx,
  });
  assert.equal(stale.ok, false);
  assert.equal(stale.error.code, 'REVISION_CONFLICT');

  const undone = await service.callTool('scene_undo', {
    scenePath: 'assets/courtyard.scene.json', projectId: PROJECT, expectedRevision: 1,
  });
  assert.equal(undone.ok, true);
  assert.equal(undone.result.revision, 2);
  assert.equal((await loadScene(file)).entities[2].position[0], 2);

  const built = await service.callTool('scene_build', {
    scenePath: 'assets/courtyard.scene.json', outputDirectory: 'outputs/packages',
  });
  assert.equal(built.ok, true);
  assert.match(built.result.packageDirectory, /^outputs\/packages\/[0-9a-f]{64}$/);
  assert.equal(built.result.manifest.target, 'windows-x64');
  await fs.access(path.join(root, ...built.result.packageDirectory.split('/'), 'manifest.json'));

  const traversal = await service.callTool('scene_validate', { scenePath: '../outside.scene.json' });
  assert.equal(traversal.ok, false);
  assert.equal(traversal.error.code, 'INVALID_PATH');
  const absoluteOutside = await service.callTool('scene_validate', { scenePath: path.join(os.tmpdir(), 'outside.scene.json') });
  assert.equal(absoluteOutside.ok, false);
  assert.equal(absoluteOutside.error.code, 'INVALID_PATH');
  const outsideOutput = await service.callTool('scene_build', {
    scenePath: 'assets/courtyard.scene.json', outputDirectory: 'build/packages',
  });
  assert.equal(outsideOutput.ok, false);
  assert.equal(outsideOutput.error.code, 'INVALID_PATH');

  const missing = await service.callTool('runtime_smoke', { scenePath: 'assets/courtyard.scene.json' });
  assert.equal(missing.ok, false);
  assert.equal(missing.error.code, 'NATIVE_BINARY_MISSING');
  const missingProbe = await service.callTool('engine_probe', { scenePath: 'assets/courtyard.scene.json' });
  assert.equal(missingProbe.ok, false);
  assert.equal(missingProbe.error.code, 'NATIVE_BINARY_MISSING');
  const unknown = await service.callTool('scene_delete_everything', {});
  assert.equal(unknown.ok, false);
  assert.equal(unknown.error.code, 'UNKNOWN_TOOL');
});

test('tool service reports invalid scenes as structured validation results', async t => {
  const { root, service } = await fixture(t);
  const invalid = scene();
  invalid.entities[1].scale[0] = 0;
  await fs.writeFile(path.join(root, 'assets', 'invalid.scene.json'), `${JSON.stringify(invalid)}\n`);
  const checked = await service.callTool('scene_validate', { scenePath: 'assets/invalid.scene.json' });
  assert.equal(checked.ok, true);
  assert.equal(checked.result.valid, false);
  assert.equal(checked.result.projectId, null);
  assert.ok(checked.result.errors.some(error => /scale/.test(error)));
});

test('MCP protocol maps initialize, tools and failures without leaking stdout', async t => {
  const { service } = await fixture(t);
  const handle = createMcpHandler({ service });

  const initialized = await handle({
    jsonrpc: '2.0', id: 1, method: 'initialize',
    params: { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'test', version: '1' } },
  });
  assert.equal(initialized.id, 1);
  assert.equal(initialized.result.serverInfo.name, 'nativegpu-scene');
  assert.equal(initialized.result.serverInfo.version, SERVER_VERSION);
  assert.equal(initialized.result.capabilities.tools.listChanged, false);

  assert.equal(await handle({ jsonrpc: '2.0', method: 'notifications/initialized' }), undefined);
  const ping = await handle({ jsonrpc: '2.0', id: 'p', method: 'ping' });
  assert.deepEqual(ping, { jsonrpc: '2.0', id: 'p', result: {} });

  const listed = await handle({ jsonrpc: '2.0', id: 2, method: 'tools/list' });
  assert.equal(listed.result.tools.length, 12);

  const called = await handle({
    jsonrpc: '2.0', id: 3, method: 'tools/call',
    params: { name: 'scene_validate', arguments: { scenePath: 'assets/courtyard.scene.json' } },
  });
  assert.equal(called.result.isError, false);
  assert.equal(JSON.parse(called.result.content[0].text).valid, true);

  const failed = await handle({
    jsonrpc: '2.0', id: 4, method: 'tools/call',
    params: { name: 'scene_query', arguments: { scenePath: '../secret.json' } },
  });
  assert.equal(failed.result.isError, true);
  assert.equal(JSON.parse(failed.result.content[0].text).code, 'INVALID_PATH');

  const missingMethod = await handle({ jsonrpc: '2.0', id: 5, method: 'resources/read' });
  assert.equal(missingMethod.error.code, -32601);
  const invalidParams = await handle({ jsonrpc: '2.0', id: 6, method: 'tools/call', params: {} });
  assert.equal(invalidParams.error.code, -32602);
  const invalidRequest = await handle({ jsonrpc: '2.0', id: 7, method: 42 });
  assert.equal(invalidRequest.error.code, -32600);
});

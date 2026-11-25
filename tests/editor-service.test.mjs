import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createEditorService } from '../editor/service.mjs';
import { loadScene, saveScene } from '../engine/scene/index.mjs';

const PROJECT = 'a1e00000-0000-4000-8000-000000000001';
const PLAYER = 'a1e00000-0000-4000-8000-000000000002';
const GROUND = 'a1e00000-0000-4000-8000-000000000003';
const PROP = 'a1e00000-0000-4000-8000-000000000004';
const SCENE = 'assets/test.scene.json';

function scene() {
  return {
    schemaVersion: 1, projectId: PROJECT, name: 'Editor test', revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities: [
      { id: PLAYER, name: 'Player', kind: 'player', position: [0, 0, 5], scale: [0.6, 1.8, 0.6], color: [0.2, 0.8, 0.95], collider: true },
      { id: GROUND, name: 'Ground', kind: 'ground', position: [0, -0.2, 0], scale: [20, 0.4, 20], color: [0.2, 0.2, 0.2], collider: true },
      { id: PROP, name: 'Crate', kind: 'prop', position: [2, 0.6, 2], scale: [1.2, 1.2, 1.2], color: [0.7, 0.4, 0.2], collider: true },
    ],
  };
}

function fakeNative({ failStart = false, capture = false, exitCode = 0 } = {}) {
  const calls = [];
  function spawnProcess(executable, args, options) {
    const child = new EventEmitter();
    child.pid = 1000 + calls.length;
    child.killCalls = 0;
    child.kill = () => { ++child.killCalls; queueMicrotask(() => child.emit('exit', null, 'SIGTERM')); return true; };
    calls.push({ executable, args, options, child });
    process.nextTick(async () => {
      if (failStart) { child.emit('error', new Error('Native test launch failed')); return; }
      child.emit('spawn');
      if (capture && args.includes('--screenshot')) {
        const image = args[args.indexOf('--screenshot') + 1];
        await fs.writeFile(image, Buffer.from([137, 80, 78, 71, 13, 10, 26, 10, 0]));
        child.emit('exit', exitCode, null);
      }
    });
    return child;
  }
  return { calls, spawnProcess };
}

async function fixture(t, options = {}) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-editor-test-'));
  const file = path.join(root, SCENE);
  await saveScene(file, scene());
  await fs.writeFile(path.join(root, 'VERSION'), '0.10.0\n');
  const executable = path.join(root, 'build', 'NativeGPU.exe');
  if (options.binary !== false) {
    await fs.mkdir(path.dirname(executable), { recursive: true });
    await fs.writeFile(executable, 'fake');
  }
  const native = fakeNative(options);
  const service = createEditorService({ root, executable, spawnProcess: native.spawnProcess });
  t.after(async () => { await service.close(); await fs.rm(root, { recursive: true, force: true }); });
  return { root, file, service, native };
}

function transaction(revision, changes = { position: [3, 0.6, 2] }) {
  return { projectId: PROJECT, expectedRevision: revision, commands: [{ op: 'set', entityId: PROP, changes }] };
}

test('editor opens and catalogs scenes without mutating files or creating history', async t => {
  const { service, file } = await fixture(t);
  const before = await fs.readFile(file, 'utf8');
  const info = await service.call('project_info');
  assert.equal(info.ok, true);
  assert.equal(info.result.nativeAvailable, true);
  assert.deepEqual(info.result.scenes, [{ path: SCENE, name: 'Editor test' }]);
  const opened = await service.call('scene_open', { scenePath: SCENE });
  assert.equal(opened.result.scene.revision, 0);
  assert.equal(opened.result.canUndo, false);
  const preview = await service.call('scene_preview', { scenePath: SCENE, transaction: transaction(0) });
  assert.equal(preview.ok, true);
  assert.equal(preview.result.scene.entities[2].position[0], 3);
  assert.equal(preview.result.revision, 1);
  assert.equal(await fs.readFile(file, 'utf8'), before);
});

test('editor commits and undoes through the shared revision-controlled scene tools', async t => {
  const { service, file } = await fixture(t);
  const applied = await service.call('scene_apply', { scenePath: SCENE, transaction: transaction(0) });
  assert.equal(applied.ok, true);
  assert.equal(applied.result.scene.revision, 1);
  assert.equal(applied.result.canUndo, true);
  assert.equal(applied.result.history.length, 1);
  const saved = await fs.readFile(file, 'utf8');
  const conflict = await service.call('scene_apply', { scenePath: SCENE, transaction: transaction(0, { name: 'Stale' }) });
  assert.equal(conflict.error.code, 'REVISION_CONFLICT');
  assert.equal(await fs.readFile(file, 'utf8'), saved);
  const invalid = await service.call('scene_apply', { scenePath: SCENE, transaction: transaction(1, { scale: [0, 1, 1] }) });
  assert.equal(invalid.ok, false);
  assert.equal(await fs.readFile(file, 'utf8'), saved);
  const undone = await service.call('scene_undo', { scenePath: SCENE, projectId: PROJECT, expectedRevision: 1 });
  assert.equal(undone.ok, true);
  assert.equal(undone.result.scene.revision, 2);
  assert.equal(undone.result.scene.entities[2].position[0], 2);
  assert.equal(undone.result.canUndo, false);
  assert.equal((await loadScene(file)).revision, 2);
});

test('editor rejects paths outside assets, traversal, non-scene files and symlink escapes', async t => {
  const { service, root } = await fixture(t);
  for (const input of ['../outside.scene.json', 'assets/../assets/test.scene.json', 'assets/test.json', path.join(root, SCENE), 'reports/test.scene.json']) {
    const opened = await service.call('scene_open', { scenePath: input });
    assert.equal(opened.ok, false, input);
    assert.equal(opened.error.code, 'INVALID_PATH', input);
  }
  const outside = path.join(root, 'outside');
  await fs.mkdir(outside);
  await fs.writeFile(path.join(outside, 'escape.scene.json'), JSON.stringify(scene()));
  const link = path.join(root, 'assets', 'link');
  try { await fs.symlink(outside, link, process.platform === 'win32' ? 'junction' : 'dir'); }
  catch (error) { if (error.code === 'EPERM') { t.diagnostic('Symlink creation unavailable'); return; } throw error; }
  const opened = await service.call('scene_open', { scenePath: 'assets/link/escape.scene.json' });
  assert.equal(opened.ok, false);
  assert.equal(opened.error.code, 'INVALID_PATH');
});

test('editor owns one native preview and starts fixed arguments without a shell', async t => {
  const { service, native, file, root } = await fixture(t);
  const started = await service.call('runtime_start', { scenePath: SCENE, executable: 'evil.exe', args: ['bad'] });
  assert.equal(started.ok, true);
  assert.equal(started.result.running, true);
  assert.equal(native.calls.length, 1);
  assert.equal(native.calls[0].options.shell, false);
  assert.equal(native.calls[0].options.cwd, root);
  assert.equal(native.calls[0].args[native.calls[0].args.indexOf('--scene') + 1], file);
  assert.ok(native.calls[0].args.includes('--watch'));
  assert.ok(!native.calls[0].args.includes('bad'));
  await service.call('runtime_start', { scenePath: SCENE });
  assert.equal(native.calls.length, 1);
  native.calls[0].child.emit('error', new Error('A later process operation failed'));
  assert.equal((await service.call('runtime_status')).result.running, true);
  await service.call('runtime_start', { scenePath: SCENE });
  assert.equal(native.calls.length, 1);
  const stopped = await service.call('runtime_stop');
  assert.equal(stopped.result.running, false);
  assert.equal(native.calls[0].child.killCalls, 1);
  await service.call('runtime_stop');
  assert.equal(native.calls[0].child.killCalls, 1);
  await service.call('runtime_start', { scenePath: SCENE });
  native.calls[1].child.emit('exit', 7, null);
  const status = await service.call('runtime_status');
  assert.equal(status.result.running, false);
  assert.equal(status.result.lastExit.code, 7);
});

test('editor reports missing binaries and failed starts, and close cleans only owned processes', async t => {
  const absent = await fixture(t, { binary: false });
  assert.equal((await absent.service.call('runtime_start', { scenePath: SCENE })).error.code, 'NATIVE_BINARY_MISSING');
  assert.equal((await absent.service.call('project_info')).result.nativeAvailable, false);
  const failed = await fixture(t, { failStart: true });
  assert.equal((await failed.service.call('runtime_start', { scenePath: SCENE })).error.code, 'NATIVE_START_FAILED');
  assert.equal((await failed.service.call('runtime_status')).result.running, false);
  const active = await fixture(t);
  await active.service.call('runtime_start', { scenePath: SCENE });
  await active.service.close();
  assert.equal(active.native.calls[0].child.killCalls, 1);
  assert.equal((await active.service.call('runtime_status')).error.code, 'EDITOR_CLOSED');
  await active.service.close();
});

test('native captures use bounded hidden rendering and return only the image URL', async t => {
  const { service, native } = await fixture(t, { capture: true });
  const captured = await service.call('runtime_capture', { scenePath: SCENE });
  assert.equal(captured.ok, true);
  assert.match(captured.result.imagePath, /^\/captures\/[a-f0-9-]+\.png$/);
  assert.equal(captured.result.revision, 0);
  assert.equal(native.calls[0].options.windowsHide, true);
  const snapshot = native.calls[0].args[native.calls[0].args.indexOf('--scene') + 1];
  assert.ok(snapshot.startsWith(service.captureDirectory));
  assert.equal(JSON.parse(await fs.readFile(snapshot, 'utf8')).revision, 0);
  assert.ok(native.calls[0].args.includes('--no-watch'));
  assert.ok(native.calls[0].args.includes('--smoke'));
  assert.equal((await service.call('runtime_status')).result.running, false);
});

test('concurrent captures are bounded and native failure remains an error', async t => {
  const { service } = await fixture(t, { capture: true });
  const first = service.call('runtime_capture', { scenePath: SCENE });
  const second = await service.call('runtime_capture', { scenePath: SCENE });
  assert.equal(second.error.code, 'CAPTURE_BUSY');
  assert.equal((await first).ok, true);
  const failed = await fixture(t, { capture: true, exitCode: 7 });
  assert.equal((await failed.service.call('runtime_capture', { scenePath: SCENE })).error.code, 'CAPTURE_FAILED');
  const startFailed = await fixture(t, { failStart: true });
  assert.equal((await startFailed.service.call('runtime_capture', { scenePath: SCENE })).error.code, 'NATIVE_START_FAILED');
});

test('closing while a capture is being prepared cannot launch a process afterwards', async t => {
  const { service, native } = await fixture(t, { capture: true });
  const pending = service.call('runtime_capture', { scenePath: SCENE });
  await service.close();
  assert.equal((await pending).error.code, 'EDITOR_CLOSED');
  assert.equal(native.calls.length, 0);
});

test('closing waits for an already queued native start and stops its process', async t => {
  const { service, native } = await fixture(t);
  const pending = service.call('runtime_start', { scenePath: SCENE });
  const closing = service.close();
  assert.equal((await pending).ok, true);
  await closing;
  assert.equal(native.calls.length, 1);
  assert.equal(native.calls[0].child.killCalls, 1);
});

test('unknown actions and malformed arguments remain structured errors', async t => {
  const { service } = await fixture(t);
  assert.equal((await service.call('shell_execute')).error.code, 'UNKNOWN_ACTION');
  assert.equal((await service.call('scene_open', null)).error.code, 'INVALID_ARGUMENTS');
});

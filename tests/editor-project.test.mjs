import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createProjectManager } from '../editor/project-manager.mjs';
import { createEditorService } from '../editor/service.mjs';
import { startEditorServer } from '../editor/server.mjs';
import { createHash } from 'node:crypto';

const engineRoot = path.resolve(import.meta.dirname, '..');

async function fixture(t) {
  const projectsRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'nativeengine-projects-'));
  t.after(() => fs.rm(projectsRoot, { recursive: true, force: true }));
  return { projectsRoot, manager: createProjectManager({ engineRoot, projectsRoot, version: '0.11.0' }) };
}

test('projects are self-contained folders under the managed projects root', async t => {
  const { projectsRoot, manager } = await fixture(t);
  const created = await manager.create({ name: '空白项目 A', template: 'blank', pipeline: 'standard' });
  assert.equal(created.folder, '空白项目 A');
  assert.equal(path.dirname(created.path), projectsRoot);
  assert.equal(await fs.readFile(path.join(created.path, 'VERSION'), 'utf8'), '0.11.0\n');
  await fs.access(path.join(created.path, 'assets/gameplay/components.json'));
  await fs.access(path.join(created.path, '.screen/main.scene.json'));
  await fs.access(path.join(created.path, 'nativeengine.project.json'));
  await assert.rejects(fs.access(path.join(engineRoot, '空白项目 A')));

  const projects = await manager.list();
  assert.deepEqual(projects.map(project => project.folder), ['空白项目 A']);
  assert.equal((await manager.latest()).folder, '空白项目 A');
  await assert.rejects(manager.create({ name: '空白项目 A' }), error => error.code === 'PROJECT_EXISTS');
  await assert.rejects(manager.create({ name: '../escape' }), error => error.code === 'INVALID_PROJECT_NAME');
  await assert.rejects(manager.create({ name: 'CON' }), error => error.code === 'INVALID_PROJECT_NAME');
  await assert.rejects(manager.create({ name: '模板项目', template: 'character' }), error => error.code === 'UNSUPPORTED_TEMPLATE');
  await assert.rejects(manager.create({ name: '光追项目', pipeline: 'raytracing' }), error => error.code === 'UNSUPPORTED_PIPELINE');
});

test('the editor service opens the blank project template without reading engine scenes', async t => {
  const { manager } = await fixture(t);
  const project = await manager.create({ name: '编辑项目' });
  const service = createEditorService({ root: project.path, executable: path.join(engineRoot, 'build', 'NativeGPU.exe') });
  t.after(() => service.close());
  const info = await service.call('project_info', {});
  assert.equal(info.ok, true);
  assert.deepEqual(info.result.scenes.map(scene => scene.path), ['.screen/main.scene.json']);
  const opened = await service.call('scene_open', { scenePath: '.screen/main.scene.json' });
  assert.equal(opened.ok, true, JSON.stringify(opened));
  assert.equal(opened.result.scene.name, '编辑项目');
  const scene = opened.result.scene;
  const player = scene.entities.find(entity => entity.kind === 'player');
  assert.equal(player.components.camera.mode, 'thirdPerson');
  assert.equal(player.character.anchor, 'feet');
  assert.equal(player.character.file, 'builtin/robot/character.json');
  assert.ok(Math.abs(player.scale[1] - 1.8) < 1e-6);
  const headerBytes = await fs.readFile(path.join(project.path, 'assets', player.character.file));
  const binary = await fs.readFile(path.join(project.path, 'assets/builtin/robot/animation.bin'));
  assert.equal(createHash('sha256').update(headerBytes).digest('hex'), player.character.sha256);
  assert.equal(createHash('sha256').update(binary).digest('hex'), player.character.binarySha256);
  assert.deepEqual(JSON.parse(headerBytes).controller, ['idle', 'walk.001', 'run.001', 'jump.001', 'fall']);
  assert.equal(scene.materials.default.albedo, 'engine_grid');
  assert.equal(scene.materials.default.uvScale, 5);
  assert.equal(scene.entities.filter(e => e.physics?.mode === 'dynamic').length, 3);
  assert.ok(scene.entities.filter(e => e.physics).every(e => e.collider));
  await fs.access(path.join(project.path, 'assets/textures/engine_grid.albedo.ngtex'));
  assert.equal(scene.entities.some(e => e.kind === 'goal'), false);
  const workspace = await service.call('workspace_data', { scenePath: '.screen/main.scene.json' });
  assert.equal(workspace.ok, true, JSON.stringify(workspace));
  assert.equal(workspace.result.characters[0].clips.length, 5);
  const built = await service.call('scene_build', { scenePath: '.screen/main.scene.json' });
  assert.equal(built.ok, true, JSON.stringify(built));
  assert.ok(built.result.packageDirectory ?? built.result.outputDirectory);
  await fs.access(path.join(project.path, 'reports', 'editor-builds'));
  await assert.rejects(fs.access(path.join(engineRoot, 'reports', 'editor-builds', project.folder)));
  const packageDirectory = built.result.packageDirectory ?? built.result.outputDirectory;
  await fs.access(path.join(packageDirectory, 'textures/engine_grid.albedo.ngtex'));
});

test('new starter projects have independent scene and asset ownership', async t => {
  const { manager } = await fixture(t);
  const a = await manager.create({ name: '项目 A' });
  const b = await manager.create({ name: '项目 B' });
  const readScene = project => fs.readFile(path.join(project.path, '.screen/main.scene.json'), 'utf8').then(JSON.parse);
  const first = await readScene(a), second = await readScene(b);
  assert.notEqual(first.projectId, second.projectId);
  assert.ok(first.entities.every(entity => !second.entities.some(other => other.id === entity.id)));
  const asset = 'assets/builtin/robot/animation.bin';
  const original = await fs.readFile(path.join(b.path, asset));
  await fs.writeFile(path.join(a.path, asset), 'project-owned edit');
  assert.deepEqual(await fs.readFile(path.join(b.path, asset)), original);
  assert.deepEqual(await fs.readFile(path.join(engineRoot, asset)), original);
});

test('server project actions create and activate a managed project', async t => {
  const { projectsRoot } = await fixture(t);
  const server = await startEditorServer({ root: engineRoot, projectsRoot });
  t.after(() => server.close());
  const session = await fetch(`${server.url}/api/session`).then(response => response.json());
  const call = async (action, args = {}) => {
    const response = await fetch(`${server.url}/api/call`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-NativeGPU-Token': session.token, Origin: server.url },
      body: JSON.stringify({ action, args }),
    });
    return response.json();
  };
  const before = await call('project_info');
  assert.equal(before.ok, true);
  assert.equal(before.result.activeProject, null);
  assert.equal(before.result.projectsRoot, projectsRoot);
  const created = await call('project_create', { name: '服务项目', template: 'blank', pipeline: 'standard' });
  assert.equal(created.ok, true, JSON.stringify(created));
  assert.equal(created.result.activeProject.folder, '服务项目');
  assert.equal(path.dirname(created.result.activeProject.location), projectsRoot);
  const after = await call('project_info');
  assert.equal(after.result.activeProject.folder, '服务项目');
  assert.deepEqual(after.result.scenes.map(scene => scene.path), ['.screen/main.scene.json']);
  assert.equal((await call('project_open', { folder: '服务项目' })).ok, true);
});

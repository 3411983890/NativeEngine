import * as fs from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import { createProjectManager } from '../../editor/project-manager.mjs';
import { buildScene, loadScene } from '../../engine/scene/index.mjs';

const root = path.resolve(import.meta.dirname, '../..');
const run = promisify(execFile);
const options = { cwd: root, windowsHide: true, timeout: 180000, maxBuffer: 65536 };
const host = path.join(root, 'build/assets-host');
const output = path.join(host, 'starter_controller_tests.exe');
try {
  if (!process.argv.includes('--skip-build')) await run('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools/build-effects-host.ps1', '-OutputDirectory', 'build/assets-host'], { ...options, timeout: 1200000 });
  const sources = ['engine_native_game.cpp.o', 'engine_native_physics.cpp.o', 'engine_native_animation.cpp.o', 'engine_native_animation_features.cpp.o', 'engine_native_animation_asset.cpp.o', 'engine_native_determinism.cpp.o', 'host_scene_loader.cpp.o', 'engine_native_assets_sha256.cpp.o', 'engine_audio_audio.cpp.o', 'engine_audio_audio_asset.cpp.o'];
  await run(path.join(root, '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'), ['-std=c++17', '-O2', '-static', '.incrementalTesting/editor/starter-controller.cpp', ...sources.map(name => path.join(host, 'objects', name)), path.join(host, 'gameplay/nativegpu-gameplay.a'), '-lbcrypt', '-o', output], options);
  const manager = createProjectManager({ engineRoot: root, version: '0.11.0' });
  const projectOption = process.argv.indexOf('--project');
  const project = projectOption >= 0 ? await manager.open(process.argv[projectOption + 1]) : await manager.create({ name: `StarterAcceptance-${randomUUID().slice(0, 8)}` });
  const scenePath = path.join(project.path, project.scene);
  const reports = path.join(project.path, 'reports/starter');
  await fs.mkdir(reports, { recursive: true });
  const contracts = JSON.parse((await run(output, [scenePath], { ...options, cwd: project.path })).stdout);
  const executable = path.join(host, 'NativeGPU.exe');
  const common = ['--smoke', '--no-stress', '--animation-features', 'off', '--audio', 'off'];
  const render = async (name, scene, textures, extra = []) => {
    const report = path.join(reports, name + '.json');
    await run(executable, [...common, '--scene', scene, '--textures', textures, '--frames', '180', '--screenshot-frame', '5', '--screenshot', path.join(reports, name + '.png'), '--report', report, ...extra], { ...options, cwd: project.path });
    const data = JSON.parse(await fs.readFile(report, 'utf8'));
    assert.equal(data.success, true, JSON.stringify(data));
    assert.equal(data.animation.instances.length, 1);
    assert.ok(data.probes.skinning.draws > 0);
    return data;
  };
  const original = await render('starter', scenePath, path.join(project.path, 'assets/textures'));
  const scene = await loadScene(scenePath);
  // Exercise fallback selection independently from a scene's named 'default'.
  const fallback = structuredClone(scene); delete fallback.materials.default;
  const fallbackPath = path.join(project.path, '.screen/fallback.scene.json');
  try {
    await fs.writeFile(fallbackPath, JSON.stringify(fallback));
    await render('fallback', fallbackPath, path.join(project.path, 'assets/textures'));
    const a = await sharp(path.join(reports, 'starter.png')).raw().toBuffer();
    const b = await sharp(path.join(reports, 'fallback.png')).raw().toBuffer();
    assert.deepEqual(a, b, 'Missing material must use the same grid as the named default');
  } finally { await fs.rm(fallbackPath, { force: true }); }
  const bundle = await buildScene(scenePath, path.join(project.path, 'build/starter'));
  await render('packaged', path.join(bundle.packageDirectory, 'scene.json'), path.join(bundle.packageDirectory, 'textures'));
  const evidence = { success: true, project: project.path, contracts, animation: original.animation, physics: original.physics, fallbackMatchesDefault: true, packagedScene: true };
  await fs.writeFile(path.join(reports, 'acceptance.json'), JSON.stringify(evidence, null, 2) + '\n');
  console.log(JSON.stringify({ success: true, project: project.path, contracts, evidence: path.join(reports, 'acceptance.json'), screenshot: path.join(reports, 'starter.png') }));
} catch (error) { console.error(error.stderr ?? error.stack); process.exitCode = 1; }

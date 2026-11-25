import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../..');
const compiler = path.join(root, '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');

test('heightfield sampling, step retention and resting contact', { skip: process.platform !== 'win32' || !existsSync(compiler) }, () => {
  const directory = mkdtempSync(path.join(tmpdir(), 'ng-heightfield-'));
  try {
    const exe = path.join(directory, 'heightfield.exe');
    execFileSync(compiler, ['-std=c++17', '-O2', '-static',
      '.incrementalTesting/simulation/heightfield-collision.cpp', 'engine/native/physics.cpp', '-o', exe],
      { cwd: root, windowsHide: true, timeout: 120000, stdio: 'pipe' });
    const output = execFileSync(exe, [], { windowsHide: true, timeout: 120000, encoding: 'utf8' });
    assert.match(output, /Heightfield collision: .*passed/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('heightfield character ground contacts, slope following and jumps during stepping', { skip: process.platform !== 'win32' || !existsSync(compiler) }, () => {
  const directory = mkdtempSync(path.join(tmpdir(), 'ng-heightfield-character-'));
  try {
    const exe = path.join(directory, 'heightfield-character.exe');
    execFileSync(compiler, ['-std=c++17', '-O1', '-static',
      '.incrementalTesting/simulation/heightfield-character.cpp',
      'engine/native/game.cpp', 'engine/native/physics.cpp',
      'engine/native/animation.cpp', 'engine/native/animation_features.cpp',
      'engine/native/animation_asset.cpp', 'engine/native/determinism.cpp',
      'engine/native/water/body.cpp', 'engine/native/water/asset.cpp',
      'engine/native/water/coupling.cpp', 'engine/native/water/transfer.cpp',
      'engine/native/assets/sha256.cpp', 'build/gameplay/nativegpu-gameplay.a',
      'host/scene_loader.cpp',
      '-lbcrypt', '-o', exe], { cwd: root, windowsHide: true, timeout: 180000, stdio: 'pipe' });
    const args = [path.join(root, 'assets/builtin/robot/character.json')];
    if (process.env.NG_HEIGHTFIELD_SCENE) {
      args.push(path.resolve(process.env.NG_HEIGHTFIELD_SCENE));
      if (process.env.NG_HEIGHTFIELD_REPORT) args.push(path.resolve(process.env.NG_HEIGHTFIELD_REPORT));
    }
    const output = execFileSync(exe, args,
      { cwd: root, windowsHide: true, timeout: 30000, encoding: 'utf8' });
    assert.match(output, /Heightfield character: .*checks passed/);
  } finally { rmSync(directory, { recursive: true, force: true }); }
});

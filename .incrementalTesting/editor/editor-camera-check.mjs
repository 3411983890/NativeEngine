import * as fs from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { execFile } from 'node:child_process';
import assert from 'node:assert/strict';

// The camera is C++ and header-only on purpose, so this check compiles one small
// executable and runs it without D3D12 or the editor. It guards the conventions that a
// screenshot test would only reveal indirectly: eye placement, handedness, pan plane,
// focus distance and the orthographic w component.
const root = path.resolve(import.meta.dirname, '../..');
const run = promisify(execFile);
const compiler = path.join(root, '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
const output = path.join(root, 'build/viewport-host', 'editor-camera-tests.exe');
const options = { cwd: root, windowsHide: true, timeout: 180000, maxBuffer: 65536 };

try {
  await fs.mkdir(path.dirname(output), { recursive: true });
  await run(compiler, ['-std=c++17', '-O2', '-static', '-Wall', '-Wextra', '-Wpedantic',
    '.incrementalTesting/editor/editor-camera.cpp', '-o', output], options);
  const { stdout, stderr } = await run(output, [], options);
  if (stderr.trim()) process.stderr.write(stderr);
  const report = JSON.parse(stdout.trim().split('\n').at(-1));
  assert.equal(report.success, true, stdout);
  assert.ok(report.checks >= 20, JSON.stringify(report));
  console.log(JSON.stringify({ success: true, executable: output, checks: report.checks }));
} catch (error) {
  console.error(error.stderr ?? error.stack);
  process.exitCode = 1;
}

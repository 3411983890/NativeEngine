// Isolated GPU contract for the water residual and draw binding.
//
// The full `water:verify` path builds the gameplay host first. That host also owns
// unrelated in-progress modules, so this entry point compiles only the shader blobs and
// the two water renderer translation units. It still executes real D3D12: the residual
// heights are read back and the draw pass is sampled, not simulated on the CPU.
import fs from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { execFile } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../..');
const run = promisify(execFile);
const options = { cwd: root, windowsHide: true, timeout: 600000, maxBuffer: 131072 };
const output = path.join(root, 'build/water-gpu');
await fs.mkdir(output, { recursive: true });
try {
  await run('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools/compile-shaders.ps1',
    '-OutputDirectory', path.relative(root, output), '-Quiet'], options);
  const compiler = path.join(root, '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
  const binary = path.join(output, 'water-ripple-gpu.exe');
  await run(compiler, ['-std=c++17', '-O2', '-static', '-DNOMINMAX', '-DUNICODE', '-D_UNICODE',
    `-I${path.join(output, 'generated')}`,
    '.incrementalTesting/simulation/water-ripple-gpu.cpp', 'renderer/water_ripple.cpp',
    'renderer/water_draw.cpp', 'renderer/matrix_math.cpp',
    '-ld3d12', '-ldxgi', '-ldxguid', '-ld3dcompiler', '-o', binary], options);
  const result = JSON.parse((await run(binary, [], options)).stdout);
  if (result.success !== true) throw new Error('GPU water contract did not succeed');
  console.log(JSON.stringify({
    success: true,
    scope: 'water GPU residual readback, advection, event delivery capacity and draw binding',
    binary: path.relative(root, binary),
    shaderHeader: path.relative(root, path.join(output, 'generated/nativegpu_shaders.h')),
    checks: result.checks,
    squareBias: result.squareBias,
    directedAspect: result.directedAspect,
    flowShift: result.flowShift,
  }));
} catch (error) {
  console.error(error.stdout ?? '', error.stderr ?? '', error.message);
  process.exitCode = 1;
}

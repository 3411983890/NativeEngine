// Reproducible layered-material budget run. Not part of CI: it measures this
// machine's GPU time so the plan document can cite real numbers.
// Usage: node .incrementalTesting/rendering/layered-budget.mjs [--layers 1,2,3,4]
import { mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { makeMips, writeNgtex } from '../../tools/textures.mjs';
import { normalizeLayeredMaterial, LAYERED_SCHEMA } from '../../engine/material/layered.mjs';

const root = path.resolve(import.meta.dirname, '../..');
const outDir = path.join(root, 'build/layered-budget');
const work = path.join(outDir, 'scenes');
rmSync(work, { recursive: true, force: true });
mkdirSync(path.join(work, 'textures'), { recursive: true });

const size = 64;
const noise = (seed) => {
  const data = Buffer.alloc(size * size * 4);
  let state = seed >>> 0;
  for (let i = 0; i < size * size; i += 1) {
    state = (state * 1664525 + 1013904223) >>> 0;
    const v = state & 0xff;
    data[i * 4] = v; data[i * 4 + 1] = (v * 3) & 0xff; data[i * 4 + 2] = (v * 7) & 0xff; data[i * 4 + 3] = 255;
  }
  return { width: size, height: size, data };
};
const write = (name, semantic, image, srgb = false) =>
  writeFileSync(path.join(work, 'textures', `${name}.${semantic}.ngtex`), writeNgtex({ mips: makeMips(image, semantic), semantic, srgb }));
for (const layer of ['a', 'b', 'c', 'd']) {
  write(`${layer}-albedo`, 'albedo', noise(layer.charCodeAt(0)), true);
  write(`${layer}-normal`, 'normal', noise(layer.charCodeAt(0) + 7));
  write(`${layer}-height`, 'height', noise(layer.charCodeAt(0) + 13));
}
write('mask', 'mask', noise(99));
write('fallback', 'albedo', { width: 1, height: 1, data: Buffer.from([255, 255, 255, 255]) }, true);
write('fallback', 'normal', { width: 1, height: 1, data: Buffer.from([128, 128, 128, 255]) });
write('fallback', 'orm', { width: 1, height: 1, data: Buffer.from([255, 255, 255, 255]) });

const requested = (() => {
  const index = process.argv.indexOf('--layers');
  if (index > 0 && process.argv[index + 1]) return process.argv[index + 1].split(',').map(Number);
  return [1, 2, 3, 4];
})();

const sceneFor = (count) => {
  const slots = ['a', 'b', 'c', 'd'].slice(0, count);
  const layers = slots.map((slot, index) => ({
    id: `layer-${slot}`,
    baseColor: [0.8, 0.45 + index * 0.1, 0.2],
    textures: { albedo: { name: `${slot}-albedo` }, normal: { name: `${slot}-normal` }, height: { name: `${slot}-height` } },
    ...(index === 0 ? {} : { blend: { mode: 'height', source: 'textureMask', texture: { name: 'mask' }, channel: 'r', weight: 0.6, width: 0.3 } }),
  }));
  const layered = normalizeLayeredMaterial({ schema: LAYERED_SCHEMA, name: `budget-${count}`, layers });
  const entity = (id, kind, position, scale) => ({ id, kind, name: kind, position, scale, color: [1, 1, 1], collider: true });
  return {
    schemaVersion: 2, projectId: 'a1e00000-0000-4000-8000-000000000001', name: `budget ${count}`, revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities: [
      entity('a1e00000-0000-4000-8000-000000000002', 'player', [-20, 0, 0], [0.6, 1.8, 0.6]),
      { ...entity('a1e00000-0000-4000-8000-000000000003', 'ground', [0, -0.2, 0], [12, 0.4, 12]), material: 'test' },
    ],
    materials: { test: { albedo: 'fallback', layered } },
  };
};

const rows = [];
for (const count of requested) {
  const sceneFile = path.join(work, `scene-${count}.json`);
  const reportFile = path.join(work, `report-${count}.json`);
  writeFileSync(sceneFile, JSON.stringify(sceneFor(count)));
  execFileSync(path.join(root, 'build/assets-host/NativeGPU.exe'), [
    '--smoke', '--no-stress', '--scene', sceneFile, '--textures', path.join(work, 'textures'),
    '--frames', '240', '--render-size', process.env.NG_BUDGET_SIZE ?? '1280x720', '--camera', '0,10,0.001,0,0,0',
    '--mesh-shaders', 'off', '--report', reportFile, '--audio', 'off',
  ], { cwd: root, windowsHide: true, timeout: 300000, stdio: 'pipe' });
  const report = JSON.parse(readFileSync(reportFile, 'utf8').replace(/^\uFEFF/, ''));
  if (!report.success) throw new Error(`budget scene ${count} failed: ${report.error}`);
  rows.push({
    layers: count,
    activeLayers: report.capabilities.layeredMaterials.activeLayers,
    samples: report.capabilities.layeredMaterials.textureSamples,
    uniqueTextures: report.capabilities.layeredMaterials.uniqueTextures,
    textureBytes: report.capabilities.layeredMaterials.textureBytes,
    gpuP50Ms: report.gpuP50Ms, gpuP95Ms: report.gpuP95Ms,
    cpuSubmitP50Ms: report.cpuSubmitP50Ms, frameIntervalP95Ms: report.frameIntervalP95Ms,
    frames: report.frames,
  });
}
const summary = { adapter: rows[0] && JSON.parse(readFileSync(path.join(work, 'report-1.json'), 'utf8').replace(/^\uFEFF/, '')).capabilities.adapter, resolution: process.env.NG_BUDGET_SIZE ?? '1280x720', frames: 240, rows };
writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 2));
console.table(rows);
console.log(`wrote ${path.join(outDir, 'summary.json')}`);

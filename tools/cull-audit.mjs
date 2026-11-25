#!/usr/bin/env node
// Offline view-frustum audit for a scene file.
//
// The renderer culls draw items against six planes extracted from the view-projection
// matrix (renderer/frustum.hpp). This tool answers the same question ahead of time,
// straight from a .scene.json and a camera, so two things are possible without a GPU:
//
//   1. Quantify how much of a map the hero camera actually sees. For level design that
//      is the "is the content on screen" number; for the renderer it is the ceiling the
//      culling pass can reach.
//   2. Regression-check the plane math. The self-test below pins the same cases as
//      tests/native_frustum.cpp; if the two ever disagree, one of them is wrong and the
//      audit is worthless.
//
// Conventions are the engine's, not OpenGL's: left-handed view, z clipped to [0,1],
// column-major matrices with column vectors (clip = viewProjection * model * p).

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const FOV_DEFAULT = 1.05;      // host/main.cpp: third-person camera fov
const NEAR_DEFAULT = 0.1;
const FAR_DEFAULT = 120;       // host/main.cpp: far = max(120, distance + ...)

function perspectiveLH(fovY, aspect, near, far) {
  const f = 1 / Math.tan(fovY * 0.5);
  const m = new Float64Array(16);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = far / (far - near);
  m[11] = 1;
  m[14] = -near * far / (far - near);
  return m;
}

function lookAtLH(eye, target, up = [0, 1, 0]) {
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const norm = (v) => { const l = Math.hypot(...v) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
  const z = norm(sub(target, eye));
  const x = norm(cross(up, z));
  const y = cross(z, x);
  // Column-major: m[column * 4 + row].
  const m = new Float64Array(16);
  m[0] = x[0]; m[1] = y[0]; m[2] = z[0]; m[3] = 0;
  m[4] = x[1]; m[5] = y[1]; m[6] = z[1]; m[7] = 0;
  m[8] = x[2]; m[9] = y[2]; m[10] = z[2]; m[11] = 0;
  m[12] = -dot(x, eye); m[13] = -dot(y, eye); m[14] = -dot(z, eye); m[15] = 1;
  return m;
}

function multiply(a, b) {
  const out = new Float64Array(16);
  for (let c = 0; c < 4; ++c) {
    for (let r = 0; r < 4; ++r) {
      let sum = 0;
      for (let k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
  return out;
}

export function frustumPlanes(vp) {
  const m = vp;
  const row = (r) => [m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]];
  const r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  const make = (a, sign, addW) => {
    const n = [
      (addW ? r3[0] : 0) + sign * a[0],
      (addW ? r3[1] : 0) + sign * a[1],
      (addW ? r3[2] : 0) + sign * a[2],
    ];
    let d = (addW ? r3[3] : 0) + sign * a[3];
    const length = Math.hypot(...n);
    if (length > 1e-20) { for (let i = 0; i < 3; ++i) n[i] /= length; d /= length; }
    return { n, d };
  };
  return [
    make(r0, 1, true), make(r0, -1, true),
    make(r1, 1, true), make(r1, -1, true),
    make(r2, 1, false), make(r2, -1, true),
  ];
}

export function outsideFrustum(planes, min, max) {
  for (const plane of planes) {
    const x = plane.n[0] >= 0 ? max[0] : min[0];
    const y = plane.n[1] >= 0 ? max[1] : min[1];
    const z = plane.n[2] >= 0 ? max[2] : min[2];
    if (plane.n[0] * x + plane.n[1] * y + plane.n[2] * z + plane.d < 0) return true;
  }
  return false;
}

function selfTest() {
  // Same cases as tests/native_frustum.cpp. A silent disagreement between the two
  // implementations would make every number below meaningless.
  const vp = multiply(perspectiveLH(1.0472, 1, 0.1, 100), lookAtLH([0, 0, 0], [0, 0, 1]));
  const planes = frustumPlanes(vp);
  const visible = (cx, cy, cz, half) =>
    !outsideFrustum(planes, [cx - half, cy - half, cz - half], [cx + half, cy + half, cz + half]);
  const cases = [
    ['ahead', visible(0, 0, 10, 1), true],
    ['behind', visible(0, 0, -10, 1), false],
    ['beyond far', visible(0, 0, 200, 1), false],
    ['far to the side', visible(1000, 0, 10, 1), false],
    ['straddling near', visible(0, 0, 0.5, 0.5), true],
    ['large box crossing planes', visible(0, 0, 10, 40), true],
  ];
  for (const [name, got, want] of cases) {
    if (got !== want) throw new Error(`frustum self-test failed: ${name} (got ${got}, want ${want})`);
  }
}

function parseArgs(argv) {
  const options = { scene: null, camera: null, width: 1600, height: 900, fov: FOV_DEFAULT, json: false };
  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--camera') options.camera = argv[++i];
    else if (arg === '--size') {
      const [w, h] = argv[++i].split('x').map(Number);
      options.width = w; options.height = h;
    } else if (arg === '--fov') options.fov = Number(argv[++i]);
    else if (arg === '--json') options.json = true;
    else if (!arg.startsWith('--')) options.scene = arg;
    else throw new Error(`unknown option ${arg}`);
  }
  if (!options.scene) throw new Error('usage: node tools/cull-audit.mjs <scene.json> --camera ex,ey,ez,tx,ty,tz [--size WxH] [--fov rad] [--json]');
  if (!options.camera) throw new Error('--camera ex,ey,ez,tx,ty,tz is required');
  return options;
}

function main() {
  selfTest();
  const options = parseArgs(process.argv.slice(2));
  const scene = JSON.parse(readFileSync(options.scene, 'utf8'));
  const eye = options.camera.split(',').slice(0, 3).map(Number);
  const target = options.camera.split(',').slice(3, 6).map(Number);
  const aspect = options.width / options.height;
  const distance = Math.hypot(target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]);
  const far = Math.max(FAR_DEFAULT, distance + 6);
  const vp = multiply(perspectiveLH(options.fov, aspect, NEAR_DEFAULT, far), lookAtLH(eye, target));
  const planes = frustumPlanes(vp);

  const byKind = new Map();
  const byMaterial = new Map();
  const totals = { submitted: 0, visible: 0, culled: 0 };
  for (const entity of scene.entities ?? []) {
    const [x, y, z] = entity.position ?? [0, 0, 0];
    const [sx, sy, sz] = entity.scale ?? [1, 1, 1];
    const min = [x - sx / 2, y - sy / 2, z - sz / 2];
    const max = [x + sx / 2, y + sy / 2, z + sz / 2];
    const culled = outsideFrustum(planes, min, max);
    const bucket = byKind.get(entity.kind) ?? { submitted: 0, visible: 0, culled: 0 };
    bucket.submitted += 1;
    bucket[culled ? 'culled' : 'visible'] += 1;
    byKind.set(entity.kind, bucket);
    const material = entity.material ?? 'default';
    byMaterial.set(material, (byMaterial.get(material) ?? 0) + 1);
    totals.submitted += 1;
    totals[culled ? 'culled' : 'visible'] += 1;
  }

  const result = {
    schema: 'nativegpu.cull-audit/1',
    scene: options.scene,
    camera: { eye, target, fov: options.fov, aspect, near: NEAR_DEFAULT, far },
    totals,
    culledFraction: totals.submitted ? totals.culled / totals.submitted : 0,
    byKind: Object.fromEntries([...byKind].sort((a, b) => b[1].submitted - a[1].submitted)),
    // Draw-call projection. Every scene item is currently its own DrawIndexedInstanced;
    // grouping by material (the mesh is the unit cube for all of them) is the number an
    // instanced path would submit instead.
    batching: {
      materials: byMaterial.size,
      currentDraws: totals.submitted,
      instancedDraws: byMaterial.size,
      perMaterial: Object.fromEntries([...byMaterial].sort((a, b) => b[1] - a[1])),
    },
  };

  if (options.json) {
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
    return;
  }
  const percent = (value) => `${(value * 100).toFixed(1)}%`;
  console.log(`${options.scene}  camera ${options.camera}  ${options.width}x${options.height}  far ${far.toFixed(1)} m`);
  console.log(`  submitted ${totals.submitted}  visible ${totals.visible}  culled ${totals.culled} (${percent(result.culledFraction)})`);
  for (const [kind, bucket] of Object.entries(result.byKind)) {
    console.log(`    ${kind.padEnd(8)} ${String(bucket.submitted).padStart(4)} -> visible ${String(bucket.visible).padStart(4)}  culled ${String(bucket.culled).padStart(4)}`);
  }
  const batching = result.batching;
  console.log(`  draws ${batching.currentDraws} -> ${batching.instancedDraws} with per-material instancing (${batching.materials} materials, ${(batching.currentDraws / Math.max(1, batching.instancedDraws)).toFixed(1)}x fewer)`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main();

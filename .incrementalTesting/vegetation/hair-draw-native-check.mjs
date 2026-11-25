import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { buildHairFixture } from '../../tools/hair-style-fixture.mjs';
import { cookHairstyle, readHairPackage } from '../../engine/hair/cook.mjs';
import { packHairForGpu, encodeHairPack, decodeHairPack, HAIR_PACK_FILE_SCHEMA } from '../../engine/hair/gpu-pack.mjs';
import { cameraForView, viewMatrix } from '../../engine/hair/capture.mjs';

// The strand draw on the actual D3D12 device.
//
// `hair-render-check.mjs` measures the CPU reference and `hair-gpu-pack-check.mjs` the
// upload contract. This entry is the third leg: it compiles `tests/native_hair_draw.cpp`,
// runs a real offscreen frame through `renderer/hair_draw.cpp`, and asserts the pixel and
// pipeline-statistics evidence the plan asks for — a draw that executes VS/PS work,
// coverage that behaves like strands, self-shadowing that depends on the volume, backlight
// that depends on albedo, and a strict refusal of an edited pack.
//
// It still is not the host scene frame: the pass has no scene integration, no GPU guide
// solve and no order-independent transparency, and the evidence names those as missing.

const ROOT = process.cwd();
const COMPILER = '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';
const BUILD = 'build/hair-draw';
const WIDTH = 256, HEIGHT = 256;
const SOURCES = [
  'tests/native_hair_draw.cpp',
  'renderer/hair_draw.cpp',
  'renderer/png_writer.cpp',
  'renderer/matrix_math.cpp',
  'engine/native/hair/gpu_pack.cpp',
  'engine/native/assets/sha256.cpp',
];

const { spec, generated, scalp } = await buildHairFixture();
const cooked = cookHairstyle(generated);
const pack = packHairForGpu(readHairPackage(cooked.bytes), { level: 0 });
const packFile = encodeHairPack(pack);

mkdirSync(BUILD, { recursive: true });
writeFileSync(join(BUILD, 'hair-pack.hgp'), packFile);
const reread = decodeHairPack(readFileSync(join(BUILD, 'hair-pack.hgp')));
assert.equal(reread.schema, HAIR_PACK_FILE_SCHEMA);
assert.equal(reread.hash, pack.hash);

// Camera math mirrors engine/hair/capture.mjs and the renderer's left-handed 0..1 depth
// projection; a view-projection built with a different convention would silently frame
// the hairstyle outside the target.
function perspective(fovYDegrees, aspect, nearPlane, farPlane) {
  const f = 1 / Math.tan((fovYDegrees * Math.PI) / 360);
  const out = new Array(16).fill(0);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = farPlane / (farPlane - nearPlane);
  out[11] = 1;
  out[14] = (-nearPlane * farPlane) / (farPlane - nearPlane);
  return out;
}
function multiply(a, b) {
  const out = new Array(16).fill(0);
  for (let column = 0; column < 4; ++column) {
    for (let row = 0; row < 4; ++row) {
      let sum = 0;
      for (let k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
      out[column * 4 + row] = sum;
    }
  }
  return out;
}
const views = ['front', 'back', 'three_quarter_left', 'hairline_closeup'].map((name) => {
  const camera = cameraForView(name, {
    headCentre: scalp.centre, headRadiusMetres: scalp.radius, targetOffsetMetres: 0.42,
  });
  const projection = perspective(camera.fovYDegrees, WIDTH / HEIGHT, camera.nearMetres, camera.farMetres);
  return {
    name,
    position: camera.position,
    viewProjection: multiply(projection, viewMatrix(camera)),
    tanHalfFovY: Math.tan((camera.fovYDegrees * Math.PI) / 360),
    nearMetres: camera.nearMetres,
  };
});

const material = {
  ...spec.material.groups[0],
  albedo: [0.055, 0.036, 0.024], roughness: 0.34,
  radialRoughness: 0.25, shiftRadians: 0.12, rootToTipTint: [1, 1, 1],
};
const keyLight = { direction: [0.35, 0.55, 0.75], colour: [1, 0.97, 0.92], intensity: 3.6 };
const rimLight = { direction: [-0.25, 0.1, -0.96], colour: [1, 0.95, 0.88], intensity: 4.2 };
const fixture = {
  schema: 'nativegpu.hair-draw-fixture/1',
  width: WIDTH, height: HEIGHT,
  tanHalfFovY: views[0].tanHalfFovY, nearMetres: views[0].nearMetres,
  views: views.map(({ name, position, viewProjection }) => ({ name, position, viewProjection })),
  material, keyLight, rimLight,
  ambient: [0.05, 0.052, 0.06],
  shadowStrength: 0.9, shadowSigmaPerMetre: 2400, shadowDensityScale: 1,
  pack: {
    strands: pack.counts.strands, points: pack.counts.points,
    segmentsPerStrand: pack.draw.segmentsPerStrand, guides: pack.counts.guides,
    drawVertices: pack.draw.vertices, uploadBytes: pack.uploadBytes,
    hash: pack.hash, hairHash: pack.hairHash,
  },
};
writeFileSync(join(BUILD, 'fixture.json'), `${JSON.stringify(fixture, null, 2)}\n`);

test('the pack and fixture the native draw consumes are the cooked hairstyle', () => {
  assert.equal(pack.counts.strands, generated.levels[0].curves.length);
  assert.equal(pack.counts.points, pack.counts.strands * (pack.draw.segmentsPerStrand + 1));
  assert.equal(packFile.toString('ascii', 0, 4), 'HGP1');
  assert.equal(packFile.length % 256, 0);
  assert.equal(fixture.views.length, 4);
  assert.equal(fixture.views[0].viewProjection.length, 16);
});

test('the D3D12 strand draw executes real VS/PS work and produces the claimed evidence', () => {
  if (!existsSync(COMPILER)) {
    assert.fail(`compiler missing at ${COMPILER}; the pinned LLVM toolchain is required`);
  }
  const shaderBuild = spawnSync('powershell', [
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools/compile-shaders.ps1',
    '-RequireDxc', '-Quiet', '-Source', 'shaders/hair.hlsl', '-OutputDirectory', BUILD,
  ], { cwd: ROOT, encoding: 'utf8' });
  assert.equal(shaderBuild.status, 0, `hair shader build failed:\n${shaderBuild.stdout}\n${shaderBuild.stderr}`);
  assert.ok(existsSync(join(BUILD, 'generated/nativegpu_shaders.h')), 'the shader header was not generated');

  const exe = join(BUILD, 'native_hair_draw_tests.exe');
  const compile = spawnSync(COMPILER, [
    '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-static', '-DNOMINMAX',
    `-I${join(BUILD, 'generated')}`,
    ...SOURCES, '-o', exe,
    '-ld3d12', '-ldxgi', '-ld3dcompiler', '-lbcrypt', '-lole32', '-luuid',
  ], { cwd: ROOT, encoding: 'utf8' });
  assert.equal(compile.status, 0, `native build failed:\n${compile.stdout}\n${compile.stderr}`);

  const adapter = process.env.HAIR_GPU_ADAPTER ?? 'any';
  const run = spawnSync(exe, [
    '--pack', join(BUILD, 'hair-pack.hgp'),
    '--fixture', join(BUILD, 'fixture.json'),
    '--out', BUILD,
    '--adapter', adapter,
  ], { cwd: ROOT, encoding: 'utf8' });
  assert.equal(run.status, 0, `native hair draw failed:\n${run.stderr}`);
  const evidence = JSON.parse(run.stdout);
  writeFileSync(join(BUILD, 'evidence.json'), `${JSON.stringify(evidence, null, 2)}\n`);

  assert.equal(evidence.ok, true);
  assert.equal(evidence.schema, 'nativegpu.hair-draw-evidence/1');
  const { pack: drawn, draw } = evidence.evidence;
  assert.equal(drawn.strands, pack.counts.strands);
  assert.equal(drawn.drawVertices, pack.draw.vertices);
  assert.equal(draw.backend, 'd3d12');
  assert.equal(draw.transparency, 'depth-sorted-alpha');
  assert.equal(draw.depthSorted, true);
  assert.ok(draw.sortingChangedPixels > 100, 'depth sorting did not change the offscreen composite');
  assert.ok(draw.drawnVertices >= pack.draw.vertices, 'the evidence did not draw a full hairstyle');
  assert.ok(draw.shadedPixels > 0, 'no pixel shader work was recorded');
  assert.ok(draw.coverageFraction > 0.03 && draw.coverageFraction < 0.8,
    `coverage ${draw.coverageFraction} is outside the plausible strand band`);
  assert.ok(draw.shadow.coveredTexels > 0, 'the deep-opacity grid received no strands');
  assert.ok(draw.shadow.maxOpticalDepthMetres > 0, 'the deep-opacity grid has no optical depth');
  assert.ok(draw.unshadowedLuminance > draw.shadowedLuminance,
    'self-shadowing did not darken the covered pixels');
  assert.equal(evidence.evidence.capabilities.gpuGuideSolve, false);
  assert.equal(evidence.evidence.capabilities.depthSortedTransparency, true);
  assert.equal(evidence.evidence.capabilities.orderIndependentTransparency, false);
  assert.equal(evidence.evidence.capabilities.sceneShadowCast, false);
  // The harness asserts the pack rejections itself; a run that reached the end must have
  // covered all of them.
  assert.ok(evidence.groups >= 8, `only ${evidence.groups} evidence groups ran`);
  assert.ok(evidence.checks >= 40, `only ${evidence.checks} checks ran`);
  for (const image of ['front', 'back', 'three-quarter', 'debug-coverage', 'debug-shadow',
    'shadow-on', 'shadow-off', 'backlight-light', 'backlight-dark', 'unsorted-front', 'closeup']) {
    assert.ok(existsSync(join(BUILD, `${image}.png`)), `missing evidence image ${image}.png`);
  }
  console.log(`native hair draw: ${evidence.checks} checks, ${evidence.groups} groups, `
    + `${drawn.strands} strands, ${draw.drawnVertices} vertices, coverage ${draw.coverageFraction.toFixed(3)}, `
    + `shadow ${draw.shadow.maxOpticalDepthMetres.toFixed(3)} m over ${draw.shadow.coveredTexels} texels`);
});

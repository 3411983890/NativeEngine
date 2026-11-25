import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import sharp from 'sharp';
import { buildHairFixture } from '../../tools/hair-style-fixture.mjs';
import { cookHairstyle, readHairPackage } from '../../engine/hair/cook.mjs';
import { packHairForGpu, encodeHairPack } from '../../engine/hair/gpu-pack.mjs';

// The strand pass inside the host scene frame.
//
// `hair-draw-native-check.mjs` proves the draw module against a standalone offscreen
// target. This entry proves the integration the plan actually asks for: the host loads a
// cooked HGP1 pack, the renderer uploads it, and the hairstyle is drawn into the same
// linear HDR target and depth buffer as the scene, lit by the scene Environment, then
// captured through the normal screenshot path.
//
// The control frame (same camera, same scene, no --groom) is the evidence that the extra
// pixels are the hairstyle and not a changed camera or exposure. Still not covered:
// order-independent transparency, hair casting onto the scene, GPU guide simulation and
// motion/flicker sequences.

const ROOT = process.cwd();
const BUILD = 'build/hair-host';
const SCENE = '.screen/hair-preview.scene.json';
const WIDTH = 640, HEIGHT = 360;

const { generated } = await buildHairFixture();
const cooked = cookHairstyle(generated);
const pack = packHairForGpu(readHairPackage(cooked.bytes), { level: 0 });
mkdirSync(BUILD, { recursive: true });
const packPath = join(BUILD, 'hair-pack.hgp');
writeFileSync(packPath, encodeHairPack(pack));
const min = pack.bounds.min, max = pack.bounds.max;
const centre = [0, 1, 2].map((axis) => (min[axis] + max[axis]) * 0.5);
const extent = Math.max(max[0] - min[0], max[1] - min[1], max[2] - min[2]);
const distance = Math.max(0.25, extent * 2.2);
const camera = [
  centre[0], centre[1] + extent * 0.05, centre[2] + distance,
  centre[0], centre[1], centre[2],
].join(',');

function runHost(name, { groom = false, wind = null, headTurn = 0 } = {}) {
  const report = join(BUILD, `${name}.json`);
  const image = join(BUILD, `${name}.png`);
  const args = [
    '--smoke', '--validation', '--no-watch', '--no-stress', '--audio', 'off',
    '--frames', '120', '--render-size', `${WIDTH}x${HEIGHT}`,
    '--scene', SCENE, '--camera', camera,
    '--report', report, '--screenshot', image, '--screenshot-frame', '90',
  ];
  if (groom) args.push('--groom', packPath);
  if (wind) args.push('--groom-wind', wind.join(','));
  if (headTurn) args.push('--groom-head-turn', String(headTurn));
  const run = spawnSync(join(BUILD, 'NativeGPU.exe'), args, { cwd: ROOT, encoding: 'utf8' });
  assert.equal(run.status, 0, `${name} host run failed:\n${run.stderr}\n${run.stdout}`);
  assert.ok(existsSync(report), `${name} report is missing`);
  assert.ok(existsSync(image), `${name} screenshot is missing`);
  return { report: JSON.parse(readFileSync(report, 'utf8')), image };
}

async function pixels(path) {
  const { data, info } = await sharp(path).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  return { data, width: info.width, height: info.height };
}

function compare(a, b) {
  let changed = 0, darkened = 0;
  for (let index = 0; index < a.data.length; index += 4) {
    const dr = Math.abs(a.data[index] - b.data[index]);
    const dg = Math.abs(a.data[index + 1] - b.data[index + 1]);
    const db = Math.abs(a.data[index + 2] - b.data[index + 2]);
    if (Math.max(dr, dg, db) > 6) ++changed;
    const lumA = 0.2126 * a.data[index] + 0.7152 * a.data[index + 1] + 0.0722 * a.data[index + 2];
    const lumB = 0.2126 * b.data[index] + 0.7152 * b.data[index + 1] + 0.0722 * b.data[index + 2];
    if (lumA < lumB - 5) ++darkened;
  }
  return { changed, darkened };
}

test('the host scene frame draws the resident hairstyle over the character', async () => {
  // The build is part of the evidence by default. HAIR_HOST_SKIP_BUILD exists for a
  // locally built binary (for example when another process holds the workspace files);
  // it must not be used to publish evidence.
  if (process.env.HAIR_HOST_SKIP_BUILD !== '1') {
    const build = spawnSync('powershell', [
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools/build-effects-host.ps1',
      '-OutputDirectory', BUILD,
    ], { cwd: ROOT, encoding: 'utf8' });
    assert.equal(build.status, 0, `host build failed:\n${build.stdout}\n${build.stderr}`);
  }
  assert.ok(existsSync(join(BUILD, 'NativeGPU.exe')), 'host executable is missing');

  const withGroom = runHost('with-groom', { groom: true });
  const withWind = runHost('with-groom-wind', { groom: true, wind: [5, 0, 2] });
  const withHeadTurn = runHost('with-groom-head-turn', { groom: true, headTurn: 25 });
  const withoutGroom = runHost('without-groom', {});
  for (const run of [withGroom, withWind, withHeadTurn, withoutGroom])
    assert.equal(run.report.success, true, `host reported: ${run.report.error}`);

  const groom = withGroom.report.groomRuntime;
  const windGroom = withWind.report.groomRuntime;
  const headGroom = withHeadTurn.report.groomRuntime;
  assert.equal(groom.requested, true);
  assert.equal(groom.configured, true, 'the hairstyle was not resident');
  assert.equal(groom.backend, 'd3d12');
  assert.equal(groom.transparency, 'depth-sorted-alpha');
  assert.equal(groom.depthSorted, true, 'the host frames were not depth sorted');
  assert.ok(groom.sortedStrands >= 60 * pack.counts.strands, 'the sort did not cover every frame');
  assert.equal(groom.orderStaleUploads, 0, 'a sorted frame could not upload its order');
  assert.ok(groom.frames >= 60, `only ${groom.frames} scene frames drew hair`);
  assert.ok(groom.drawnStrands >= 1000, `only ${groom.drawnStrands} strands were submitted`);
  // The draw uploads the point and strand buffers only; the guide and attribute tables
  // stay in the pack file for the solver, so the GPU figure is smaller than the file.
  assert.equal(groom.uploadBytes, pack.counts.points * 32 + pack.counts.strands * 32);
  assert.ok(groom.shadowCoveredTexels > 0, 'the deep-opacity grid received no strands');
  assert.ok(groom.shadowMaxOpticalDepthMetres > 0);
  assert.equal(withoutGroom.report.groomRuntime.configured, false);

  // The guide solve is the CPU authority: fixed 60 Hz tick, roots pinned, no residual
  // penetration, and no buffer left unwritten because the previous frame still read it.
  assert.equal(groom.simulating, true);
  assert.ok(groom.simulationTicks >= 60, `only ${groom.simulationTicks} simulation ticks ran`);
  assert.ok(groom.simulationMaxStretch <= 1.05, `guides stretched to ${groom.simulationMaxStretch}`);
  assert.ok(groom.simulationMaxPenetrationMetres <= 0.001,
    `guides ended ${groom.simulationMaxPenetrationMetres} m inside the head`);
  assert.equal(groom.simulationMaxRootDriftMetres, 0, 'a pinned root moved');
  assert.equal(groom.simulationStaleUploads, 0, 'a frame could not write its simulated points');
  assert.equal(windGroom.wind[0], 5);
  assert.ok(windGroom.simulationMaxSpeedMetresPerSecond > groom.simulationMaxSpeedMetresPerSecond,
    'wind did not increase the strand speed');
  // Head attachment: the strands use the player's head skin matrix every frame; the
  // turn run proves the same matrix also moves them with the posed head bone.
  assert.equal(groom.headAttached, true, 'the hairstyle was not attached to the head bone');
  assert.equal(headGroom.headAttached, true);
  assert.equal(headGroom.headTurnDegrees, 25);

  const groomed = await pixels(withGroom.image);
  const windPixels = await pixels(withWind.image);
  const headPixels = await pixels(withHeadTurn.image);
  const control = await pixels(withoutGroom.image);
  assert.equal(groomed.width, WIDTH);
  assert.equal(groomed.height, HEIGHT);
  const { changed, darkened } = compare(groomed, control);
  const windDelta = compare(windPixels, groomed);
  const headDelta = compare(headPixels, groomed);
  assert.ok(changed > 1000, `the hairstyle changed only ${changed} pixels`);
  assert.ok(darkened > 500, `the hairstyle darkened only ${darkened} pixels`);
  assert.ok(windDelta.changed > 1000, `wind changed only ${windDelta.changed} hairstyle pixels`);
  assert.ok(headDelta.changed > 1000, `a 25 degree head turn moved only ${headDelta.changed} pixels`);
  assert.notEqual(createHash('sha256').update(readFileSync(withGroom.image)).digest('hex'),
    createHash('sha256').update(readFileSync(withoutGroom.image)).digest('hex'));

  writeFileSync(join(BUILD, 'evidence.json'), `${JSON.stringify({
    schema: 'nativegpu.hair-host-evidence/1',
    pack: {
      strands: pack.counts.strands, points: pack.counts.points, hash: pack.hash,
      fileBytes: pack.uploadBytes, gpuBytes: groom.uploadBytes,
    },
    camera, scene: SCENE, resolution: { width: WIDTH, height: HEIGHT },
    groom: {
      frames: groom.frames, drawnStrands: groom.drawnStrands, drawnVertices: groom.drawnVertices,
      triangles: groom.triangles, shadowCoveredTexels: groom.shadowCoveredTexels,
      shadowDroppedLayers: groom.shadowDroppedLayers,
      shadowMaxOpticalDepthMetres: groom.shadowMaxOpticalDepthMetres,
      transparency: groom.transparency, backend: groom.backend,
      depthSorted: groom.depthSorted, sortedStrands: groom.sortedStrands,
      orderStaleUploads: groom.orderStaleUploads, headAttached: groom.headAttached,
      simulation: {
        backend: 'cpu-guide', ticks: groom.simulationTicks,
        timeSeconds: groom.simulationTimeSeconds, maxStretch: groom.simulationMaxStretch,
        maxPenetrationMetres: groom.simulationMaxPenetrationMetres,
        maxSpeedMetresPerSecond: groom.simulationMaxSpeedMetresPerSecond,
        maxRootDriftMetres: groom.simulationMaxRootDriftMetres,
        staleUploads: groom.simulationStaleUploads,
      },
    },
    wind: {
      direction: windGroom.wind, maxSpeedMetresPerSecond: windGroom.simulationMaxSpeedMetresPerSecond,
      changedPixels: windDelta.changed,
    },
    headTurn: {
      degrees: headGroom.headTurnDegrees, attached: headGroom.headAttached,
      changedPixels: headDelta.changed,
    },
    pixels: { changed, darkened },
    capabilities: {
      sceneFrame: true, sceneDepthTest: true, environmentLighting: true,
      guideSimulation: true, gpuGuideSolve: false, headBoneAttachment: true,
      depthSortedTransparency: true, orderIndependentTransparency: false, sceneShadowCast: false,
    },
  }, null, 2)}\n`);
  console.log(`hair host: ${groom.frames} frames, ${groom.drawnStrands} strands, `
    + `stretch ${groom.simulationMaxStretch.toFixed(3)}, wind ${windDelta.changed} pixels, `
    + `${changed} changed / ${darkened} darkened`);
});

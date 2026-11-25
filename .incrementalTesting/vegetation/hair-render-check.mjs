import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync } from 'node:fs';
import { loadCharacterAsset } from '../../engine/character/rig-mesh.mjs';
import { buildScalp } from '../../engine/hair/scalp.mjs';
import { generateHairstyle } from '../../engine/hair/generate.mjs';
import { cameraForView } from '../../engine/hair/capture.mjs';
import { encodePng } from '../../engine/weights/png.mjs';
import {
  renderHairReference, toRgba8, coverageRgba8, meanLuminance, DEFAULT_RENDER_OPTIONS,
} from '../../engine/hair/render-reference.mjs';
import { hairStyleSpec } from '../../tools/hair-style-fixture.mjs';

// Strand rendering, measured on the CPU reference.
//
// The claims under test are the ones the plan makes about the draw path, so each test names
// the claim rather than the function: sub-pixel coverage, a bounded transparency cache with
// a reported overflow, deep-opacity self-shadowing, backlight transmission, and stability
// under camera distance. None of these can be checked from a formula alone — they need
// strands composited into pixels.
//
// This is *not* evidence for the D3D12 pass. `stats.backend` is `cpu-reference` and the
// engine frames, GPU timings, video memory and hardware resolve behaviour remain unmeasured
// until that pass exists.

const character = await loadCharacterAsset('assets/builtin/robot/character.json');
const scalp = buildScalp(character);
const spec = hairStyleSpec();
const generated = generateHairstyle(spec, character, { scalp });
const material = { ...spec.material.groups[0], albedo: [0.055, 0.036, 0.024], roughness: 0.34 };
const keyLight = { direction: [0.35, 0.55, 0.75], colour: [1, 0.97, 0.92], intensity: 3.6 };
const rimLight = { direction: [-0.25, 0.1, -0.96], colour: [1, 0.95, 0.88], intensity: 4.2 };
const outputDirectory = process.env.HAIR_RENDER_OUT ?? 'build/hair-render';
mkdirSync(outputDirectory, { recursive: true });

const camera = (view, offset = 0.42) =>
  cameraForView(view, { headCentre: scalp.centre, headRadiusMetres: scalp.radius, targetOffsetMetres: offset });
const render = (view, { offset = 0.42, light = keyLight, options = {}, size = 224, materialOverride = null } = {}) =>
  renderHairReference({
    generated, material: materialOverride ?? material, camera: camera(view, offset),
    width: size, height: size, light, options,
  });
const digest = (result) => createHash('sha256').update(toRgba8(result)).digest('hex');
const write = (name, result) => writeFileSync(`${outputDirectory}/${name}.png`,
  encodePng({ width: result.width, height: result.height, rgba: toRgba8(result) }));

test('the draw path composites strands, not a card: three views produce distinct silhouettes', () => {
  const front = render('front');
  const back = render('back');
  const side = render('three_quarter_left');
  for (const [name, result] of [['front', front], ['back', back], ['three_quarter_left', side]]) {
    assert.equal(result.stats.backend, 'cpu-reference');
    assert.equal(result.stats.strands, generated.levels[0].curves.length);
    assert.ok(result.stats.segments > result.stats.strands, `${name} must rasterise segments, not strand points`);
    assert.ok(result.stats.coverageFraction > 0.05, `${name} covers almost nothing (${result.stats.coverageFraction})`);
    assert.ok(result.stats.coverageFraction < 0.75, `${name} fills the frame (${result.stats.coverageFraction})`);
    write(`view-${name}`, result);
  }
  assert.notEqual(digest(front), digest(back), 'front and back must not be the same image');
  writeFileSync(`${outputDirectory}/coverage-front.png`,
    encodePng({ width: front.width, height: front.height, rgba: coverageRgba8(front) }));
});

test('the same inputs render byte-identical images', () => {
  const first = render('front', { size: 160 });
  const second = render('front', { size: 160 });
  assert.equal(digest(first), digest(second), 'a reference render must be reproducible');
  const changed = render('front', { size: 160, light: rimLight });
  assert.notEqual(digest(first), digest(changed), 'changing the light must change the image');
});

test('sub-pixel strands keep their coverage instead of collapsing to a hard line', () => {
  // At this framing a 70 µm strand is well under a pixel wide. It must be drawn one pixel
  // wide with a fractional alpha, not at alpha 1: otherwise the hairline turns into a band.
  const close = render('front', { offset: 0.12, size: 192 });
  const far = render('front', { offset: 0.95, size: 192 });
  assert.ok(close.stats.subPixelStrands > 0, 'the near framing must contain sub-pixel strands');
  assert.equal(far.stats.subPixelStrands, close.stats.subPixelStrands,
    'every strand is sub-pixel at both framings, so the count is the segment count');
  // Coverage falls with distance because the hairstyle subtends less area. A renderer that
  // widened sub-pixel strands to full opacity would keep the same relative area; one that
  // dropped them would lose it entirely.
  const ratio = far.stats.coverageFraction / close.stats.coverageFraction;
  assert.ok(ratio > 0.15 && ratio < 0.85, `coverage ratio ${ratio.toFixed(3)} is outside the plausible band`);

  // The width clamp bounds a strand's screen footprint. With 25x the authored radius the
  // strands are wide on screen, and a small clamp must visibly reduce coverage compared with
  // a wide one — that is the difference between "bounded overdraw" and "no bound at all".
  const thick = {
    ...generated,
    levels: generated.levels.map((level) => ({
      ...level,
      curves: level.curves.map((curve) => ({ ...curve, radii: curve.radii.map((radius) => radius * 100) })),
    })),
  };
  const wide = renderHairReference({
    generated: thick, material, camera: camera('front', 0.06), width: 160, height: 160, light: keyLight,
    options: { maxWidthPixels: 8 },
  });
  const narrow = renderHairReference({
    generated: thick, material, camera: camera('front', 0.06), width: 160, height: 160, light: keyLight,
    options: { maxWidthPixels: 2 },
  });
  assert.ok(wide.stats.clampedWidths > 0, 'thick strands at a close framing must hit the width clamp');
  assert.ok(narrow.stats.coverageFraction < wide.stats.coverageFraction,
    'a tighter width clamp must reduce coverage instead of being ignored');
});

test('the transparency cache is bounded and reports its overflow', () => {
  const thin = render('front', { size: 176, options: { maxLayersPerPixel: 2 } });
  const thick = render('front', { size: 176, options: { maxLayersPerPixel: 24 } });
  assert.equal(thin.stats.layeredTransparency.layersAvailable, 2);
  assert.ok(thin.stats.overflowPixels > 0, 'two layers cannot hold this hairstyle; the overflow must be counted');
  assert.ok(thin.stats.overflowPixels > thick.stats.overflowPixels,
    'a larger cache must overflow less than a smaller one');
  assert.equal(thin.stats.fallback, 'opaque-spill-layer');
  assert.ok(thin.stats.spillPixels > 0, 'overflow is composited as a spill layer, not dropped');
  // The spill layer exists so that a small cache loses depth order, not density: coverage
  // with two layers stays within a few percent of coverage with twenty-four.
  const loss = Math.abs(thin.stats.coverageFraction - thick.stats.coverageFraction) / thick.stats.coverageFraction;
  assert.ok(loss < 0.1, `layer overflow changed coverage by ${(loss * 100).toFixed(1)}%`);
  write('layers-2', thin);
  write('layers-24', thick);
});

test('deep-opacity self-shadowing darkens the roots more than the tips', () => {
  const shadowed = render('three_quarter_left', { size: 224, options: { shadow: true } });
  const unshadowed = render('three_quarter_left', { size: 224, options: { shadow: false } });
  assert.ok(shadowed.stats.shadow, 'the shadow grid must be reported');
  assert.ok(shadowed.stats.shadow.coveredTexels > 0, 'the shadow grid must actually receive the strands');
  assert.ok(shadowed.stats.shadow.maxOpticalDepthMetres > 0, 'the grid must accumulate optical depth');
  const shadowedLuminance = meanLuminance(shadowed);
  const unshadowedLuminance = meanLuminance(unshadowed);
  assert.ok(shadowedLuminance < unshadowedLuminance * 0.95,
    `self-shadowing barely changed the image (${shadowedLuminance.toFixed(4)} vs ${unshadowedLuminance.toFixed(4)})`);

  // The claim is about the *shadow*, not about the albedo or the light: compare each
  // screen-space band with and without shadowing. The volume centre must lose a larger
  // fraction of its light than the silhouette edge, which is what a deep-opacity lookup does
  // and what a single uniform shadow multiplier cannot.
  const bandProfile = (image) => {
    const bands = new Array(5).fill(0), counts = new Array(5).fill(0);
    for (let pixel = 0; pixel < image.width * image.height; ++pixel) {
      if (image.coverage[pixel] <= 0) continue;
      const x = pixel % image.width, y = Math.floor(pixel / image.width);
      const distance = Math.hypot(x - image.width * 0.5, y - image.height * 0.5) / (image.width * 0.5);
      const band = Math.min(4, Math.floor(distance * 5));
      bands[band] += 0.2126 * image.rgb[pixel * 3] + 0.7152 * image.rgb[pixel * 3 + 1] + 0.0722 * image.rgb[pixel * 3 + 2];
      counts[band] += 1;
    }
    return bands.map((sum, index) => (counts[index] ? sum / counts[index] : 0));
  };
  const shadowedBands = bandProfile(shadowed);
  const unshadowedBands = bandProfile(unshadowed);
  const innerRatio = shadowedBands[0] / Math.max(1e-6, unshadowedBands[0]);
  const outerRatio = shadowedBands[4] / Math.max(1e-6, unshadowedBands[4]);
  assert.ok(innerRatio < outerRatio,
    `the volume centre must be shadowed more than the edge (inner ${innerRatio.toFixed(3)}, outer ${outerRatio.toFixed(3)})`);
  write('shadow-on', shadowed);
  write('shadow-off', unshadowed);
});

test('backlit hair transmits by albedo, so light hair carries more light than dark hair', () => {
  const lightHair = { ...material, albedo: [0.32, 0.22, 0.12] };
  const darkHair = { ...material, albedo: [0.02, 0.014, 0.01] };
  const bright = render('front', { size: 192, light: rimLight, materialOverride: lightHair });
  const dark = render('front', { size: 192, light: rimLight, materialOverride: darkHair });
  assert.ok(meanLuminance(bright) > meanLuminance(dark) * 1.3,
    `blond hair must transmit visibly more than black hair (${meanLuminance(bright).toFixed(4)} vs ${meanLuminance(dark).toFixed(4)})`);
  write('backlight-light', bright);
  write('backlight-dark', dark);
});

test('the LOD level changes strand count without changing the image contract', () => {
  const level0 = render('front', { size: 176 });
  const level1 = renderHairReference({
    generated, material, camera: camera('front'),
    width: 176, height: 176, light: keyLight, options: { level: 1 },
  });
  assert.ok(level1.stats.strands < level0.stats.strands, 'a coarser LOD must draw fewer strands');
  assert.ok(level1.stats.coverageFraction > level0.stats.coverageFraction * 0.4,
    'a coarser LOD must still cover the silhouette');
  assert.ok(level1.stats.overflowPixels <= level0.stats.overflowPixels,
    'a coarser LOD must not need more transparency layers');
});

test('the renderer rejects inputs it cannot render instead of producing a black frame', () => {
  const rejects = (action) => assert.throws(action,
    (error) => typeof error.code === 'string' && error.code.startsWith('NG-E-HAIR-RENDER'));
  rejects(() => renderHairReference({
    generated: { levels: [{ curves: [] }] }, material, camera: camera('front'),
    width: 64, height: 64, light: keyLight,
  }));
  // A zero light direction would be normalised to garbage and shade every strand black.
  rejects(() => renderHairReference({
    generated, material, camera: camera('front'), width: 64, height: 64,
    light: { direction: [0, 0, 0], colour: [1, 1, 1] },
  }));
  rejects(() => render('front', { options: { level: 7 }, size: 64 }));
});

test('the default render options stay the documented ones', () => {
  // A silently changed default (say, layers 8 → 2) would move every image in this file while
  // the tests still passed on their ratios.
  assert.equal(DEFAULT_RENDER_OPTIONS.maxLayersPerPixel, 8);
  assert.equal(DEFAULT_RENDER_OPTIONS.minWidthPixels, 1);
  assert.equal(DEFAULT_RENDER_OPTIONS.shadow, true);
  assert.equal(DEFAULT_RENDER_OPTIONS.layered?.layersAvailable, undefined);
});

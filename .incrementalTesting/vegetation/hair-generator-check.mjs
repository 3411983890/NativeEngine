import test from 'node:test';
import assert from 'node:assert/strict';
import { loadCharacterAsset, axisAngleQuaternion } from '../../engine/character/rig-mesh.mjs';
import {
  normalizeHairstyle, REGION_PRESETS, regionSeed, hairstyleRevision, HairError,
} from '../../engine/hair/spec.mjs';
import { buildScalp, measureBindingDrift, regionArea, characterCollisionProxies } from '../../engine/hair/scalp.mjs';
import { generateHairstyle, rootTracking } from '../../engine/hair/generate.mjs';
import { cookHairstyle, readHairPackage, cookCompatibleGroom, strandIdentity } from '../../engine/hair/cook.mjs';
import { applyPatch, hairRevision } from '../../engine/hair/patch.mjs';
import { validateHairstyle, HAIR_TEST_POSES } from '../../engine/hair/validate.mjs';
import {
  strandCoveragePixels, transparencyBudget, fibreResponse, shadeSample, albedoFromPigment,
  calibrateForAlbedo, stabilityMetric, lodTransitionDelta, deepOpacityAlpha,
} from '../../engine/hair/shading.mjs';
import { buildCapturePlan, resolveSelection, unprojectPixel } from '../../engine/hair/capture.mjs';
import {
  reconcileAnalyses, registerReferenceFile, ANALYSIS_SCHEMA,
} from '../../engine/hair/reference.mjs';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { encodePng } from '../../engine/weights/png.mjs';

const character = await loadCharacterAsset('assets/builtin/robot/character.json');
const scalp = buildScalp(character);

// Low density on purpose: these tests check identity and binding, not hair volume. A test
// suite that takes 30 s to generate 60k strands is a suite nobody runs.
const baseSpec = (overrides = {}) => ({
  schema: 'nativegpu.hairstyle/1',
  id: 'test_style',
  seed: 4242,
  character: { headBone: 'head', assetId: 'assets/builtin/robot/character.json' },
  regions: [
    { id: 'bangs', ...REGION_PRESETS.bangs, densityPerCm2: 40, lengthMetres: 0.12, comb: [0, -0.25, 1],
      parting: { azimuthDegrees: 0, widthDegrees: 6 } },
    { id: 'crown', ...REGION_PRESETS.crown, densityPerCm2: 40, lengthMetres: 0.16, comb: [0, -0.35, 0.5] },
    { id: 'back', ...REGION_PRESETS.back, densityPerCm2: 40, lengthMetres: 0.18, comb: [0, -0.5, -0.4] },
  ],
  strands: { widthMetres: 0.00007, rootJitterMetres: 0.0012, streamsPerGuide: 8 },
  guides: { segments: 8, radiusMetres: 0.00007 },
  material: { groups: [{ id: 'hair', albedo: [0.09, 0.06, 0.04], roughness: 0.3 }] },
  physics: { gravityMetresPerSecond2: 9.81 },
  lod: [
    { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 4 },
    { level: 1, strandFraction: 0.5, segmentScale: 0.7, maxDistanceMetres: 12 },
  ],
  budget: { maxStrands: 40000, maxPoints: 2000000 },
  ...overrides,
});

const generate = (overrides = {}) => generateHairstyle(baseSpec(overrides), character, { scalp });

test('scalp is built from the character head mesh, not a sphere', () => {
  assert.ok(scalp.triangles > 1000, `scalp has only ${scalp.triangles} triangles`);
  assert.equal(scalp.headBone, 'head');
  assert.ok(scalp.radius > 0.05 && scalp.radius < 0.2, `implausible head radius ${scalp.radius}`);
  // A spherical cap would have every triangle centroid at the same distance from the
  // centre. A real head has a face, a nape and ears, so the spread must be visible.
  let minimum = Infinity, maximum = 0;
  for (let triangle = 0; triangle < scalp.triangles; ++triangle) {
    const distance = Math.hypot(
      scalp.centroids[triangle * 3] - scalp.centre[0],
      scalp.centroids[triangle * 3 + 1] - scalp.centre[1],
      scalp.centroids[triangle * 3 + 2] - scalp.centre[2]);
    minimum = Math.min(minimum, distance);
    maximum = Math.max(maximum, distance);
  }
  assert.ok(maximum - minimum > 0.01, `scalp radius varies by only ${(maximum - minimum) * 1000} mm`);
});

test('generation is deterministic and guide ids are unique within a region', () => {
  const first = generate();
  const second = generate();
  assert.equal(first.stats.guides, second.stats.guides);
  assert.deepEqual(
    first.guides.map((guide) => [guide.stableId, guide.points[3]]),
    second.guides.map((guide) => [guide.stableId, guide.points[3]]));
  for (const region of first.regions) {
    const ids = first.guides.filter((guide) => guide.region === region.id).map((guide) => guide.stableId);
    assert.equal(new Set(ids).size, ids.length, `region ${region.id} has duplicate stable ids`);
  }
});

test('editing one region does not move any other region', () => {
  const before = generate();
  const shorter = generate({
    regions: [
      { id: 'bangs', ...REGION_PRESETS.bangs, densityPerCm2: 40, lengthMetres: 0.09, comb: [0, -0.25, 1] },
      { id: 'crown', ...REGION_PRESETS.crown, densityPerCm2: 40, lengthMetres: 0.16, comb: [0, -0.35, 0.5] },
      { id: 'back', ...REGION_PRESETS.back, densityPerCm2: 40, lengthMetres: 0.18, comb: [0, -0.5, -0.4] },
    ],
  });
  const pick = (generated, region) => generated.guides
    .filter((guide) => guide.region === region)
    .map((guide) => [guide.stableId, guide.points, guide.radii]);
  assert.deepEqual(pick(before, 'back'), pick(shorter, 'back'));
  assert.deepEqual(pick(before, 'crown'), pick(shorter, 'crown'));
  assert.notDeepEqual(pick(before, 'bangs'), pick(shorter, 'bangs'));
});

test('every guide root sits on the scalp and follows the skull under a pose', () => {
  const generated = generate();
  const bindings = generated.guides.map((guide) => ({ triangle: guide.triangle, bary: guide.bary }));
  const rest = measureBindingDrift(character, bindings, scalp, {});
  assert.equal(rest.rootsOffSurface, 0, `${rest.rootsOffSurface} roots are off the scalp at rest`);
  const turned = measureBindingDrift(character, bindings, scalp, {
    pose: { head: axisAngleQuaternion([0, 1, 0], 0.9) },
  });
  assert.equal(turned.rootsOffSurface, 0, `${turned.rootsOffSurface} roots left the scalp when the head turned`);
  assert.ok(turned.maximumSurfaceErrorMetres < 5e-4,
    `binding error ${turned.maximumSurfaceErrorMetres * 1000} mm exceeds 0.5 mm`);
  const tracking = rootTracking(character, generated, { pose: { head: axisAngleQuaternion([0, 1, 0], 0.9) } });
  assert.ok(tracking.maximumMetres > 0.01, 'roots did not move with the head at all');
});

test('a hairstyle without a real skull joint is refused', () => {
  assert.throws(() => generateHairstyle(baseSpec({ character: { headBone: 'not_a_bone' } }), character),
    (error) => error instanceof HairError || error.code === 'NG-E-HAIR-SCALP');
});

test('cook is byte-deterministic and re-reads its own package', () => {
  const generated = generate();
  const first = cookHairstyle(generated, { binding: { rootsOffSurface: 0 } });
  const second = cookHairstyle(generate(), { binding: { rootsOffSurface: 0 } });
  assert.equal(first.hash, second.hash);
  assert.deepEqual(Buffer.compare(first.bytes, second.bytes), 0);
  const read = readHairPackage(first.bytes);
  assert.equal(read.meta.counts.guides, generated.stats.guides);
  assert.equal(read.scalp.triangles, scalp.triangles);
  assert.equal(read.guides.length, generated.stats.guides);
  assert.equal(read.levels[0].curves.length, generated.stats.strands);
  assert.equal(read.bindings.count, generated.stats.guides);
  // Strand identity resolves back to the guide it belongs to.
  const identity = strandIdentity(read, 0, 3);
  assert.ok(identity.stableId.startsWith(read.meta.guideIds[identity.guideIndex]));
});

test('a coarser LOD keeps the same roots and only drops strands', () => {
  const generated = generate();
  const fine = generated.levels[0].curves;
  const coarse = generated.levels[1].curves;
  assert.ok(coarse.length < fine.length);
  const byId = new Map(fine.map((curve) => [curve.stableId, curve]));
  let compared = 0;
  for (const curve of coarse) {
    const reference = byId.get(curve.stableId);
    assert.ok(reference, `LOD1 strand ${curve.stableId} does not exist at LOD0`);
    for (let axis = 0; axis < 3; ++axis) {
      assert.ok(Math.abs(curve.points[0][axis] - reference.points[0][axis]) < 1e-6,
        `LOD1 moved the root of ${curve.stableId}`);
    }
    assert.ok(curve.points.length < reference.points.length);
    ++compared;
  }
  assert.ok(compared > 10);
  const transition = lodTransitionDelta(
    { roots: fine.map((curve) => curve.points[0]) },
    { roots: coarse.map((curve) => curve.points[0]) });
  // Index-wise comparison is expected to differ: that is why identity matters.
  assert.ok(transition.maxRootDriftMetres > 0);
});

test('validation reports binding, penetration, LOD identity and what it did not measure', () => {
  const generated = generate();
  const report = validateHairstyle({ generated, character });
  assert.equal(report.poses.length, Object.keys(HAIR_TEST_POSES).length);
  for (const pose of report.poses) {
    assert.equal(pose.rootsOffSurface, 0);
    // Only the rest pose is a hard penetration gate. A posed pose compares the *static*
    // hairstyle against moved collision proxies; that gap is what the solver exists to
    // close, so it must be reported (finite, attributed) rather than silently passed or
    // failed. Asserting a small number for it would be claiming a simulation result a
    // static check cannot produce.
    assert.ok(Number.isFinite(pose.penetration.p99Metres));
    if (pose.pose === 'rest') {
      assert.equal(pose.enforced, true);
      assert.ok(pose.penetration.p99Metres <= 0.002,
        `penetration ${(pose.penetration.p99Metres * 1000).toFixed(2)} mm at rest`);
    } else {
      assert.equal(pose.enforced, false, `${pose.pose} must not be enforced as a static gate`);
      assert.equal(pose.requiresSolver, true, `${pose.pose} must say the solver is needed`);
      if (pose.penetration.p99Metres > 0.002) {
        assert.ok(pose.penetration.worst.guideId, 'a penetration failure must name the worst guide');
      }
    }
  }
  assert.ok(report.lod.every((entry) => entry.maxRootDriftMetres === 0));
  assert.ok(report.unmeasured.includes('GPU draw timing and video memory'));
  assert.equal(report.timings.measured, false);
  assert.equal(report.passes, true, report.failures.map((failure) => failure.message).join('; '));
});

test('patch requires the current revision, honours locks and regenerates only its region', () => {
  const spec = normalizeHairstyle(baseSpec());
  const revision = hairstyleRevision(spec);
  const known = new Set(generate().guides.map((guide) => guide.stableId));
  const { diff } = applyPatch(spec, {
    expectedRevision: revision,
    operations: [{ kind: 'region', region: 'bangs', changes: { lengthMetres: 0.1 } }],
  }, { knownGuides: known });
  assert.deepEqual(diff.regeneratedRegions.sort(), ['bangs']);
  assert.ok(diff.untouchedRegions.includes('back'));

  assert.throws(() => applyPatch(spec, {
    expectedRevision: 'deadbeefdeadbeef',
    operations: [{ kind: 'region', region: 'bangs', changes: { lengthMetres: 0.1 } }],
  }), (error) => error.code === 'NG-E-HAIR-REVISION');

  const locked = applyPatch(spec, {
    expectedRevision: revision,
    operations: [{ kind: 'lock', region: 'crown', fields: ['lengthMetres'] }],
  }).spec;
  const lockedRevision = hairRevision(locked);
  assert.throws(() => applyPatch(locked, {
    expectedRevision: lockedRevision,
    operations: [{ kind: 'region', region: 'crown', changes: { lengthMetres: 0.3 } }],
  }), (error) => error.code === 'NG-E-HAIR-LOCKED');

  assert.throws(() => applyPatch(spec, {
    expectedRevision: revision,
    operations: [{ kind: 'guide', guide: 'bangs#000000', changes: { lengthScale: 0.5 } }],
  }, { knownGuides: known }), (error) => error.code === 'NG-E-HAIR-UNKNOWN-GUIDE');
});

test('guide overrides edit the curve but never the root', () => {
  const generated = generate();
  const target = generated.guides.find((guide) => guide.region === 'bangs');
  const override = (extra) => normalizeHairstyle(baseSpec({
    guideOverrides: [{ guide: target.stableId, region: 'bangs', lengthScale: 0.6, ...extra }],
  }));
  // The two edits are generated separately so each one is measured on its own: a
  // length scale drags every point towards the root, which would mask (and here did
  // mask) the effect of a control-point offset applied on top of it.
  const shortened = generateHairstyle(override({}), character, { scalp })
    .guides.find((guide) => guide.stableId === target.stableId);
  const after = generateHairstyle(override({ pointOffsets: [{ index: 4, delta: [0, 0.01, 0] }] }), character, { scalp })
    .guides.find((guide) => guide.stableId === target.stableId);
  assert.ok(shortened && after, 'the edited guide disappeared');
  for (let axis = 0; axis < 3; ++axis) {
    assert.ok(Math.abs(shortened.points[0][axis] - target.points[0][axis]) < 1e-9, 'the root moved');
    assert.ok(Math.abs(after.points[0][axis] - target.points[0][axis]) < 1e-9, 'the root moved');
  }
  assert.ok(shortened.lengthMetres < target.lengthMetres * 0.75, 'lengthScale had no effect');
  assert.ok(after.points[4][1] > shortened.points[4][1] + 0.005, 'the control point offset had no effect');
  // Points the patch did not name are untouched by it.
  assert.ok(Math.abs(after.points[7][1] - shortened.points[7][1]) < 1e-9, 'the offset leaked to another point');
  assert.equal(after.edited, true);
});

test('coverage, transparency budget and fibre response behave as documented', () => {
  // A 70 µm strand at 30 cm on a 1080p view with a 32° vertical FOV is a fraction of a pixel.
  const coverage = strandCoveragePixels(0.00007, 0.3, 1080, Math.tan((16 * Math.PI) / 180));
  assert.ok(coverage > 0 && coverage < 1, `coverage ${coverage}`);
  const budget = transparencyBudget({
    strandsOnScreen: 40000, averageSegmentsPerStrand: 12, averageCoverage: 0.3,
    layersPerTile: 8, resolution: { width: 1280, height: 720 },
  });
  assert.equal(budget.algorithm, 'bounded-tile-layers');
  assert.ok(budget.layersNeeded >= 1);
  assert.equal(budget.overflowLayers > 0, budget.fallback === 'opaque-approximation');

  const dark = albedoFromPigment(0.95, 0.1);
  const light = albedoFromPigment(0.12, 0.4);
  assert.ok(dark.reduce((sum, value) => sum + value, 0) < light.reduce((sum, value) => sum + value, 0));
  const darkCalibration = calibrateForAlbedo(dark);
  const lightCalibration = calibrateForAlbedo(light);
  assert.equal(darkCalibration.category, 'dark');
  assert.equal(lightCalibration.category, 'light');
  // Dark hair must show a relatively stronger, tighter highlight and far less transmission.
  assert.ok(darkCalibration.specularScale > lightCalibration.specularScale);
  assert.ok(darkCalibration.transmissionScale < lightCalibration.transmissionScale);

  const grazing = fibreResponse({
    tangent: [0, 1, 0], normal: [0, 0, 1], viewDirection: [0, 0, -1], lightDirection: [0, 0, 1],
  });
  const front = shadeSample({
    tangent: [0, 1, 0], normal: [0, 0, 1], viewDirection: [0, 0, 1], lightDirection: [0, 0, 1],
    material: { albedo: [0.2, 0.15, 0.1], roughness: 0.3 },
  });
  const back = shadeSample({
    tangent: [0, 1, 0], normal: [0, 0, 1], viewDirection: [0, 0, 1], lightDirection: [0, 0, -1],
    material: { albedo: [0.2, 0.15, 0.1], roughness: 0.3 },
  });
  assert.ok(grazing.transmission > 0.1, 'a back-lit fibre has no transmission');
  assert.ok(front.transmission[0] < back.transmission[0], 'transmission is not directional');
  for (const value of back.radiance) assert.ok(Number.isFinite(value) && value >= 0);

  const steady = stabilityMetric([
    { coverage: [0.5, 0.5, 0.5] }, { coverage: [0.5, 0.51, 0.5] }, { coverage: [0.5, 0.5, 0.5] },
  ]);
  assert.equal(steady.flickering, false);
  const flicker = stabilityMetric([
    { coverage: [0.2, 0.2, 0.2] }, { coverage: [0.9, 0.9, 0.9] },
  ]);
  assert.equal(flicker.flickering, true);
  assert.ok(deepOpacityAlpha([4, 8, 12]) > 0.1);
});

test('capture plan is reproducible and a stale screenshot cannot be selected', () => {
  // Revisions and asset hashes are content digests in production; the fixtures use the
  // same shape so the plan's "a capture without a revision is useless" guard stays in
  // force instead of being relaxed to accept any string.
  const revision = 'a1b2c3d4e5f60718';
  const assetHash = '9f8e7d6c5b4a3921';
  const plan = buildCapturePlan({
    views: ['front', 'back'], channels: ['beauty', 'root_binding'], lighting: ['neutral', 'backlight'],
    resolution: { width: 256, height: 256 }, headCentre: scalp.centre, headRadiusMetres: scalp.radius,
    revision, assetHash, seed: 7,
  });
  assert.equal(plan.count, 8);
  const again = buildCapturePlan({
    views: ['front', 'back'], channels: ['beauty', 'root_binding'], lighting: ['neutral', 'backlight'],
    resolution: { width: 256, height: 256 }, headCentre: scalp.centre, headRadiusMetres: scalp.radius,
    revision, assetHash, seed: 7,
  });
  assert.deepEqual(plan.entries.map((entry) => entry.id), again.entries.map((entry) => entry.id));
  const entry = plan.entries[0];
  const metadata = { ...entry, resolution: { width: 256, height: 256 } };
  // Depth-buffer selection against the same revision resolves guides near the pixels.
  const depth = new Float32Array(256 * 256);
  const guide = generate().guides[0];
  for (let index = 0; index < depth.length; ++index) depth[index] = 0.3;
  const selection = resolveSelection({
    selection: { mode: 'visible', rect: [0, 0, 256, 256] },
    metadata, currentRevision: revision, currentAssetHash: assetHash,
    depthBuffer: { width: 256, height: 256, data: depth },
    guides: [guide],
    maxDepthMetres: 2,
  });
  assert.equal(selection.mode, 'visible');
  assert.ok(selection.guides.length >= 1, 'a 2 m radius over the whole frame matched no guide');
  // A capture without a revision cannot be planned at all.
  assert.throws(() => buildCapturePlan({ revision: '', assetHash, headCentre: scalp.centre, headRadiusMetres: scalp.radius }),
    (error) => error.code === 'NG-E-HAIR-CAPTURE');

  assert.throws(() => resolveSelection({
    selection: { mode: 'visible', rect: [0, 0, 32, 32] },
    metadata, currentRevision: 'ffffffffffffffff', currentAssetHash: assetHash,
    depthBuffer: { width: 256, height: 256, data: depth },
  }), (error) => error.code === 'NG-E-HAIR-SCREENSHOT-STALE');

  assert.throws(() => resolveSelection({
    selection: { mode: 'through', rect: [0, 0, 32, 32] },
    metadata, currentRevision: revision, currentAssetHash: assetHash,
  }), (error) => error.code === 'NG-E-HAIR-SELECTION');
  void unprojectPixel;
});

test('multi-view analysis refuses to average a conflict and flags inferred fields', async () => {
  const directory = mkdtempSync(join(tmpdir(), 'hair-ref-'));
  // The PNG is built through the same encoder the weights export uses, so the reference
  // reader sees a real file rather than a hand-written byte string.
  const png = encodePng({ width: 2, height: 2, rgba: new Uint8Array(16).fill(200) });
  const frontPath = join(directory, 'front.png');
  const backPath = join(directory, 'back.png');
  writeFileSync(frontPath, png);
  writeFileSync(backPath, png);
  const front = await registerReferenceFile(frontPath, { id: 'front', view: 'front', authorized: true, confidence: 0.9 });
  const back = await registerReferenceFile(backPath, { id: 'back', view: 'back', authorized: true, confidence: 0.8 });
  const analysis = (referenceId, length, extra = []) => ({
    schema: ANALYSIS_SCHEMA,
    referenceId,
    observations: [
      { field: 'lengthMetres', value: length, confidence: 0.8, provenance: 'visible' },
      { field: 'curlTurnsPerMetre', value: 0, confidence: 0.7, provenance: 'visible' },
      ...extra,
    ],
  });
  const agreeing = reconcileAnalyses([front, back], [analysis('front', 0.2), analysis('back', 0.21)]);
  assert.equal(agreeing.conflicts.length, 0);
  assert.equal(agreeing.fields.lengthMetres, 0.2);

  const conflicting = reconcileAnalyses([front, back], [analysis('front', 0.2), analysis('back', 0.34)]);
  assert.equal(conflicting.conflicts.length, 1);
  assert.equal(conflicting.conflicts[0].field, 'lengthMetres');
  assert.equal(conflicting.provenance.lengthMetres.provenance, 'conflict');

  const inferred = reconcileAnalyses([front], [analysis('front', 0.2, [
    { field: 'backVolumeMetres', value: 0.03, confidence: 0.4, provenance: 'inferred' },
  ])]);
  assert.deepEqual(inferred.inferredFields, ['backVolumeMetres']);
  assert.equal(inferred.requiresBackView, true);
  assert.ok(inferred.diagnostics.some((entry) => entry.code === 'NG-E-HAIR-BACK-VIEW-REQUIRED'));

  // A front view claiming to see the back of the head is ignored, not trusted.
  const illegal = reconcileAnalyses([front], [analysis('front', 0.2, [
    { field: 'backVolumeMetres', value: 0.02, confidence: 0.9, provenance: 'visible' },
  ])]);
  assert.ok(illegal.diagnostics.some((entry) => entry.code === 'NG-E-HAIR-VIEW-MISMATCH'));
  assert.equal(illegal.fields.backVolumeMetres, undefined);
});

test('the compatibility groom export states what it loses', () => {
  const spec = normalizeHairstyle(baseSpec());
  const compatible = cookCompatibleGroom(spec);
  assert.ok(compatible.guides > 0 && compatible.strands > 0);
  assert.ok(compatible.losses.some((loss) => loss.includes('spherical cap')));
  assert.ok(compatible.losses.some((loss) => loss.includes('region identity')));
});

test('region area and collision proxies come from the character', () => {
  assert.ok(regionArea(scalp, normalizeHairstyle(baseSpec()).regions[0]) > 0);
  const proxies = characterCollisionProxies(character);
  assert.ok(proxies.spheres.some((sphere) => sphere.bone === 'head'));
  assert.ok(proxies.capsules.length >= 2);
  for (const sphere of proxies.spheres) {
    assert.ok(sphere.radiusMetres > 0.01 && sphere.radiusMetres < 0.3,
      `head sphere radius ${sphere.radiusMetres} m is implausible`);
  }
  void regionSeed;

});

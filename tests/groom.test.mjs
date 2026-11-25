import test from 'node:test';
import assert from 'node:assert/strict';
import { readPackage, SectionKind } from '../engine/assets/binary-package.mjs';
import { compileGroom, GROOM_COOK_SCHEMA, GUIDE_SECTION_INDEX, SCALP_SECTION_INDEX } from '../engine/groom/asset.mjs';
import { arcLength, resampleCurve, validateCurve, GroomError } from '../engine/groom/curves.mjs';

const source = (overrides = {}) => ({
  schema: 'nativegpu.groom/1', id: 'test_bob', style: 'scalp', seed: 4242,
  scalp: { radiusMetres: 0.095, coverageDegrees: 110, rings: 8, segments: 20 },
  groups: [{
    id: 'hair',
    material: {
      albedo: [0.1, 0.07, 0.05], roughness: 0.3, radialRoughness: 0.25, longitudinalRoughness: 0.3,
      absorption: [0.6, 0.42, 0.3], melanin: 0.8, redness: 0.3,
    },
    simulation: 'guide', shadow: 'deepOpacity',
  }],
  guides: { count: 96, segments: 8, lengthMetres: 0.2, rootRadiusMetres: 0.00006, gravity: 0.8, comb: [0, -0.2, -0.6] },
  strands: { count: 1200, frizzMetres: 0.002, widthMetres: 0.00007, rootJitterMetres: 0.0015 },
  lod: [
    { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 4 },
    { level: 1, strandFraction: 0.5, segmentScale: 0.75, maxDistanceMetres: 12 },
  ],
  budget: { maxStrands: 40000, maxPoints: 4000000 },
  ...overrides,
});

function decodeCurves(pkg, level) {
  const headers = pkg.require(SectionKind.CURVE_HEADER, level);
  const points = pkg.require(SectionKind.CURVE_POINT_F32X3, level);
  const radii = pkg.require(SectionKind.CURVE_RADIUS_F32, level);
  const pointsView = new Float32Array(points.data.buffer, points.data.byteOffset, points.bytes / 4);
  const radiiView = new Float32Array(radii.data.buffer, radii.data.byteOffset, radii.bytes / 4);
  const curves = [];
  for (let index = 0; index < headers.count; ++index) {
    const first = headers.data.readUInt32LE(index * 16);
    const count = headers.data.readUInt32LE(index * 16 + 4);
    const group = headers.data.readUInt32LE(index * 16 + 8);
    curves.push({
      firstPoint: first, group,
      points: Array.from({ length: count }, (_, point) => [pointsView[(first + point) * 3], pointsView[(first + point) * 3 + 1], pointsView[(first + point) * 3 + 2]]),
      radii: Array.from({ length: count }, (_, point) => radiiView[first + point]),
    });
  }
  return curves;
}

test('groom cook is byte-deterministic and re-reads its own package', () => {
  const first = compileGroom(source());
  const second = compileGroom(source());
  assert.equal(first.hash, second.hash);
  assert.deepEqual(first.bytes, second.bytes);
  const pkg = readPackage(first.bytes);
  assert.equal(pkg.schema, GROOM_COOK_SCHEMA);
  assert.equal(pkg.meta.counts.strands, first.strands);
  assert.equal(pkg.meta.counts.guides, first.guides);
  assert.deepEqual(pkg.meta.representations, ['strands', 'cards', 'guides']);
  // Section counts must agree with the manifest, not with whatever the writer felt like.
  const headers = pkg.require(SectionKind.CURVE_HEADER, 0);
  assert.equal(headers.count, first.strands);
  const points = pkg.require(SectionKind.CURVE_POINT_F32X3, 0);
  assert.equal(points.count, first.meta.lod[0].points);
});

test('every strand root sits on the scalp and every curve has a positive radius', () => {
  const cooked = compileGroom(source());
  const pkg = readPackage(cooked.bytes);
  const scalpPositions = pkg.require(SectionKind.POSITION_F32X3, SCALP_SECTION_INDEX);
  const positions = new Float32Array(scalpPositions.data.buffer, scalpPositions.data.byteOffset, scalpPositions.bytes / 4);
  const curves = decodeCurves(pkg, 0);
  assert.equal(curves.length, cooked.strands);
  const radius = source().scalp.radiusMetres;
  let checked = 0;
  for (const curve of curves.slice(0, 200)) {
    const root = curve.points[0];
    // The scalp is a spherical cap, so a root belongs on it when its distance from the
    // head centre is the cap radius. Root jitter is 1.5 mm and the chord error of the
    // tessellated cap is ~1 mm, so 4 mm is the honest tolerance.
    const distanceFromCentre = Math.hypot(root[0], root[1], root[2]);
    assert.ok(Math.abs(distanceFromCentre - radius) < 0.004,
      `strand root is ${Math.abs(distanceFromCentre - radius)} m off the scalp (radius ${radius})`);
    for (const value of curve.radii) assert.ok(value > 0 && value <= 0.05);
    ++checked;
  }
  assert.ok(checked >= 100);
});

test('guide binding is a barycentric frame that reproduces the root', () => {
  const cooked = compileGroom(source());
  const pkg = readPackage(cooked.bytes);
  const bindings = pkg.require(SectionKind.BINDING, GUIDE_SECTION_INDEX);
  const guides = decodeCurves(pkg, GUIDE_SECTION_INDEX);
  const scalpPositions = pkg.require(SectionKind.POSITION_F32X3, SCALP_SECTION_INDEX);
  const positions = new Float32Array(scalpPositions.data.buffer, scalpPositions.data.byteOffset, scalpPositions.bytes / 4);
  const scalpIndices = pkg.require(SectionKind.INDEX_U32, SCALP_SECTION_INDEX);
  const indices = new Uint32Array(scalpIndices.data.buffer, scalpIndices.data.byteOffset, scalpIndices.bytes / 4);
  for (let index = 0; index < bindings.count; ++index) {
    const base = index * 32;
    const triangle = bindings.data.readUInt32LE(base);
    const bary = [0, 1, 2].map((component) => bindings.data.readFloatLE(base + 4 + component * 4));
    assert.ok(Math.abs(bary[0] + bary[1] + bary[2] - 1) < 1e-4, 'barycentric weights must sum to 1');
    for (const weight of bary) assert.ok(weight >= -1e-4 && weight <= 1 + 1e-4);
    const root = guides[index].points[0];
    const i0 = indices[triangle * 3], i1 = indices[triangle * 3 + 1], i2 = indices[triangle * 3 + 2];
    const reconstructed = [0, 1, 2].map((axis) =>
      bary[0] * positions[i0 * 3 + axis] + bary[1] * positions[i1 * 3 + axis] + bary[2] * positions[i2 * 3 + axis]);
    // The reconstructed point is the closest point on the *mesh*, and offsetMetres is
    // the distance the cook measured between that point and the root. They must agree:
    // that is what proves the stored frame belongs to this root.
    const distance = Math.hypot(reconstructed[0] - root[0], reconstructed[1] - root[1], reconstructed[2] - root[2]);
    const recorded = bindings.data.readFloatLE(base + 24);
    assert.ok(Math.abs(distance - recorded) < 2e-5, `binding ${index} records ${recorded} m but reconstructs ${distance} m`);
  }
});

test('guide weights are normalised and reference real guides', () => {
  const cooked = compileGroom(source());
  const pkg = readPackage(cooked.bytes);
  const weights = pkg.require(SectionKind.GUIDE_WEIGHT, 0);
  assert.equal(weights.count, cooked.strands);
  const guideCount = cooked.guides;
  for (let index = 0; index < weights.count; ++index) {
    const base = index * 32;
    const guides = [0, 1, 2].map((component) => weights.data.readUInt32LE(base + component * 4));
    const values = [0, 1, 2].map((component) => weights.data.readFloatLE(base + 16 + component * 4));
    assert.ok(guides.every((guide) => guide < guideCount), 'guide index outside the guide pool');
    assert.ok(Math.abs(values[0] + values[1] + values[2] - 1) < 1e-4, 'strand weights must sum to 1');
    assert.ok(values.every((value) => value >= -1e-5 && value <= 1 + 1e-5));
  }
});

test('a coarser LOD keeps the same strands and only drops points', () => {
  const cooked = compileGroom(source());
  const pkg = readPackage(cooked.bytes);
  const fine = decodeCurves(pkg, 0), coarse = decodeCurves(pkg, 1);
  assert.ok(coarse.length < fine.length);
  const fineWeights = pkg.require(SectionKind.GUIDE_WEIGHT, 0);
  const coarseWeights = pkg.require(SectionKind.GUIDE_WEIGHT, 1);
  for (let index = 0; index < coarse.length; index += 37) {
    const fineBase = index * 32, coarseBase = index * 32;
    // Same strand index, same guide blend: only the point count changed.
    assert.equal(coarseWeights.data.readUInt32LE(coarseBase), fineWeights.data.readUInt32LE(fineBase));
    assert.ok(Math.abs(coarseWeights.data.readFloatLE(coarseBase + 16) - fineWeights.data.readFloatLE(fineBase + 16)) < 1e-6);
    const root = coarse[index].points[0], fineRoot = fine[index].points[0];
    assert.ok(Math.hypot(root[0] - fineRoot[0], root[1] - fineRoot[1], root[2] - fineRoot[2]) < 1e-6, 'LOD must not move a strand root');
    assert.ok(coarse[index].points.length < fine[index].points.length);
  }
});

test('cards reference real curve ranges and preserve coverage', () => {
  const cooked = compileGroom(source());
  const pkg = readPackage(cooked.bytes);
  const cards = pkg.require(SectionKind.CARD, 0);
  const total = cooked.strands;
  let covered = 0;
  for (let index = 0; index < cards.count; ++index) {
    const base = index * 32;
    const first = cards.data.readUInt32LE(base), count = cards.data.readUInt32LE(base + 4);
    const width = cards.data.readFloatLE(base + 8), coverage = cards.data.readFloatLE(base + 12);
    assert.equal(first, covered, 'cards must tile the strand range without gaps');
    assert.ok(count >= 1 && count <= 8);
    assert.ok(width > 0 && width < 0.05);
    assert.ok(coverage > 0 && coverage <= 1);
    covered += count;
  }
  assert.equal(covered, total);
});

test('curve maths rejects degenerate input and preserves endpoints when resampling', () => {
  assert.throws(() => validateCurve([[0, 0, 0], [0, 0, 0]], [0.001, 0.001]), /zero arc length/);
  assert.throws(() => validateCurve([[0, 0, 0], [0, 0.1, 0]], [0.001, 0]), /radius/);
  assert.throws(() => validateCurve([[0, 0, 0], [0, Number.NaN, 0]], [0.001, 0.001]), /finite/);
  const points = [[0, 0, 0], [0, 0.1, 0], [0.02, 0.2, 0], [0.05, 0.3, 0]];
  const radii = [0.002, 0.0018, 0.0015, 0.001];
  const resampled = resampleCurve(points, radii, 17);
  assert.equal(resampled.points.length, 17);
  assert.ok(Math.hypot(...resampled.points[0].map((value, axis) => value - points[0][axis])) < 1e-9);
  const last = resampled.points[resampled.points.length - 1];
  assert.ok(Math.hypot(...last.map((value, axis) => value - points[3][axis])) < 1e-6);
  const original = arcLength(points).total, resampledLength = arcLength(resampled.points).total;
  assert.ok(Math.abs(original - resampledLength) / original < 0.05, `resample changed arc length ${original} -> ${resampledLength}`);
  assert.ok(resampled.radii.every((value) => value > 0));
});

test('groom validation rejects unit and budget mistakes', () => {
  assert.throws(() => compileGroom(source({ lod: [{ level: 0, strandFraction: 0.5, segmentScale: 1, maxDistanceMetres: 4 }] })), /lod0/);
  assert.throws(() => compileGroom(source({ budget: { maxStrands: 100, maxPoints: 4000000 } })), /maxStrands/);
  assert.throws(() => compileGroom(source({ guides: { ...source().guides, rootRadiusMetres: 0.5 } })), /rootRadiusMetres/);
  assert.throws(() => compileGroom(source({ groups: [] })), /1–8/);
  assert.throws(() => compileGroom(source({ style: 'mane' })), /style/);
  assert.throws(() => validateCurve([[0, 0, 0]], [0.001]), GroomError);
});

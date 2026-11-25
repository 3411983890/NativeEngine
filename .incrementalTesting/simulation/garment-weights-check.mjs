import test from 'node:test';
import assert from 'node:assert/strict';
import { loadCharacterAsset, axisAngleQuaternion } from '../../engine/character/rig-mesh.mjs';
import { inspectGarment, bodyFromCharacter, parseMesh, GarmentError } from '../../engine/garment/inspect.mjs';
import {
  transferWeights, smoothWeights, sparsifyWeights, validateWeights, weightsSnapshot,
  patchWeights, mirrorBoneMap, mirrorWeights, topologyHash, applyLocks, influenceTotals,
  DEFAULT_MAX_INFLUENCES,
} from '../../engine/garment/weights.mjs';
import {
  bindGarment, validateGarmentPhysics, validateGarment,
} from '../../engine/garment/validate.mjs';
import { GARMENT_TEST_POSES, SUPPORTED, measureMeshPenetration } from '../../engine/garment/physics.mjs';

// Garment weight transfer, physics contract and simulation, on the sample character.
//
// The garments here are generated, not imported: a fitted sleeve is the character's own
// cross-section at that height plus a 12 mm clearance. That matters for what this file
// can claim. It proves the transfer, the side separation, the lock behaviour, the
// influence limit, the stale-topology guard and the simulation contract on a mesh whose
// ground truth is measurable. It does **not** prove that a specific imported shirt from
// an artist will bind correctly, and it does not prove anything about self-collision —
// `SUPPORTED.selfCollision` is false and every report says so.

const character = await loadCharacterAsset('assets/builtin/robot/character.json');
const boneIndex = (name) => {
  const bone = character.bones.find((entry) => entry.name === name);
  assert.ok(bone, `the sample character has no bone ${name}`);
  return bone.index;
};
const boneName = (index) => character.bones[index]?.name ?? '';

// ---------------------------------------------------------------- mesh fixtures
// Cross-section of the body at a height: centroid plus the half-extent along x and z, from
// the vertices dominated by `boneNames`. Building the garment from this is what makes
// "fitted" a measurement instead of a guessed number.
function sectionOf(boneNames, y, halfHeight = 0.025) {
  const bones = new Set(boneNames.map(boneIndex));
  let centreX = 0, centreZ = 0, count = 0, radiusX = 0, radiusZ = 0;
  const points = [];
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    if (!bones.has(character.joints[vertex * 4]) || character.weights[vertex * 4] < 0.5) continue;
    if (Math.abs(character.positions[vertex * 3 + 1] - y) > halfHeight) continue;
    points.push([character.positions[vertex * 3], character.positions[vertex * 3 + 2]]);
    centreX += character.positions[vertex * 3];
    centreZ += character.positions[vertex * 3 + 2];
    ++count;
  }
  assert.ok(count > 0, `no body vertices near y=${y} for ${boneNames.join('+')}`);
  centreX /= count; centreZ /= count;
  for (const [x, z] of points) {
    radiusX = Math.max(radiusX, Math.abs(x - centreX));
    radiusZ = Math.max(radiusZ, Math.abs(z - centreZ));
  }
  return { centreX, centreZ, radiusX, radiusZ };
}

// Open elliptical tube through a list of rings. Elliptical rather than circular because a
// torso is roughly twice as wide as it is deep: a circle that clears the sides is 5 cm off
// the chest, and every one of those vertices reports as low confidence.
function tube(stations, { segments = 24, flatten = 1 } = {}) {
  const positions = [], indices = [];
  for (const station of stations) {
    for (let segment = 0; segment <= segments; ++segment) {
      const angle = (segment / segments) * Math.PI * 2;
      positions.push(
        station.centre[0] + Math.cos(angle) * station.radiusX,
        station.centre[1],
        station.centre[2] + Math.sin(angle) * station.radiusZ * flatten);
    }
  }
  const stride = segments + 1;
  for (let ring = 0; ring < stations.length - 1; ++ring) {
    for (let segment = 0; segment < segments; ++segment) {
      const a = ring * stride + segment, b = a + 1, c = a + stride, d = c + 1;
      indices.push(a, b, c, b, d, c);
    }
  }
  // Stations may run up or down the body, which flips the winding. The fixture is supposed
  // to be a correctly wound garment, so orient it here instead of making every caller
  // remember which way its heights go.
  let volume = 0;
  for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
    const [i0, i1, i2] = [indices[triangle * 3], indices[triangle * 3 + 1], indices[triangle * 3 + 2]];
    const p0 = [positions[i0 * 3], positions[i0 * 3 + 1], positions[i0 * 3 + 2]];
    const p1 = [positions[i1 * 3], positions[i1 * 3 + 1], positions[i1 * 3 + 2]];
    const p2 = [positions[i2 * 3], positions[i2 * 3 + 1], positions[i2 * 3 + 2]];
    volume += (p0[0] * (p1[1] * p2[2] - p1[2] * p2[1])
      - p0[1] * (p1[0] * p2[2] - p1[2] * p2[0])
      + p0[2] * (p1[0] * p2[1] - p1[1] * p2[0])) / 6;
  }
  if (volume < 0) {
    for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
      const swap = indices[triangle * 3 + 1];
      indices[triangle * 3 + 1] = indices[triangle * 3 + 2];
      indices[triangle * 3 + 2] = swap;
    }
  }
  return { positions, indices };
}

function fittedSleeve(boneNames, heights, clearanceMetres, segments = 24) {
  return tube(heights.map((y) => {
    const section = sectionOf(boneNames, y);
    return {
      centre: [section.centreX, y, section.centreZ],
      radiusX: section.radiusX + clearanceMetres,
      radiusZ: section.radiusZ + clearanceMetres,
    };
  }), { segments });
}

function mergeMeshes(...meshes) {
  const positions = [], indices = [];
  for (const mesh of meshes) {
    const offset = positions.length / 3;
    positions.push(...mesh.positions);
    indices.push(...mesh.indices.map((index) => index + offset));
  }
  return { positions, indices };
}

const TORSO_BONES = ['mixamorig:Hips', 'mixamorig:Spine', 'mixamorig:Spine1', 'mixamorig:Spine2'];
const LEFT_LEG = ['mixamorig:LeftUpLeg', 'mixamorig:LeftLeg'];
const RIGHT_LEG = ['mixamorig:RightUpLeg', 'mixamorig:RightLeg'];

// A tank top: torso only, stops below the armpit so the arms are not inside it.
const tankTop = fittedSleeve(TORSO_BONES, [0.56, 0.62, 0.68, 0.72], 0.012);
const pants = mergeMeshes(
  fittedSleeve(LEFT_LEG, [0.58, 0.50, 0.42, 0.34, 0.26, 0.18], 0.012, 18),
  fittedSleeve(RIGHT_LEG, [0.58, 0.50, 0.42, 0.34, 0.26, 0.18], 0.012, 18));
const waist = sectionOf(TORSO_BONES, 0.56);
const skirt = tube([
  { centre: [waist.centreX, 0.56, waist.centreZ], radiusX: waist.radiusX + 0.012, radiusZ: waist.radiusZ + 0.012 },
  { centre: [waist.centreX, 0.48, waist.centreZ], radiusX: 0.150, radiusZ: 0.120 },
  { centre: [waist.centreX, 0.40, waist.centreZ], radiusX: 0.185, radiusZ: 0.150 },
  { centre: [waist.centreX, 0.32, waist.centreZ], radiusX: 0.210, radiusZ: 0.170 },
], { segments: 32 });

const TANK_REGIONS = ['torso', 'hips'];
const PANTS_REGIONS = ['hips', 'left_thigh', 'left_calf', 'left_foot', 'right_thigh', 'right_calf', 'right_foot'];
const SKIRT_REGIONS = ['hips', 'torso'];

const body = bodyFromCharacter(character);

function dominantBone(weights, vertex) {
  let best = -1, bestWeight = 0;
  for (let slot = 0; slot < weights.maximumInfluences; ++slot) {
    const weight = weights.weights[vertex * weights.maximumInfluences + slot];
    if (weight > bestWeight) { bestWeight = weight; best = weights.joints[vertex * weights.maximumInfluences + slot]; }
  }
  return { bone: best, weight: bestWeight, name: boneName(best) };
}

// --------------------------------------------------------------- inspection
test('inspection measures the generated garment and reports what is wrong with it', () => {
  const report = inspectGarment({ garment: tankTop, body, character });
  assert.equal(report.schema, 'nativegpu.garment-inspection/1');
  assert.ok(report.topology.signedVolumeM3 > 0, 'the fixture must not be inside-out');
  assert.equal(report.topology.nonManifoldEdges, 0);
  assert.equal(report.topology.degenerateTriangles, 0);
  assert.equal(report.perVertexSignedDistance.length, report.garment.vertices);
  // The measurement is signed and populated: a garment wrapped around the body has most of
  // its vertices outside it. A sign error here (parity voting on a multi-part body used to
  // report every point as outside) shows up as `outsideVertices === vertices`.
  assert.ok(report.fit.outsideVertices > report.garment.vertices * 0.5,
    `only ${report.fit.outsideVertices} of ${report.garment.vertices} vertices are outside the body`);

  // A garment built inside the torso must be reported, and the depth must be measured
  // rather than flagged. This is the check that catches an inverted sign: with parity
  // voting it reported zero penetration for a tube buried in the chest.
  const buried = tube([0.62, 0.68].map((y) => {
    const section = sectionOf(TORSO_BONES, y);
    return {
      centre: [section.centreX, y, section.centreZ],
      radiusX: 0.025, radiusZ: 0.02,
    };
  }), { segments: 12 });
  const bad = inspectGarment({ garment: buried, body, character });
  // Not literally every vertex: the torso is only ~5 cm deep, so a 25 mm tube pokes out at
  // the front. "Almost all of it is inside" is the claim being checked, and a sign error
  // fails it outright.
  assert.ok(bad.fit.penetratingVertices > bad.garment.vertices * 0.8,
    `a garment inside the body reported only ${bad.fit.penetratingVertices} of `
    + `${bad.garment.vertices} penetrating vertices`);
  assert.ok(bad.fit.penetration.maximumMetres > 0.02);
  const codes = bad.diagnostics.map((entry) => entry.code);
  assert.ok(codes.includes('NG-E-GARMENT-PENETRATION'), `expected a penetration diagnostic, got ${codes.join(',')}`);
  assert.equal(bad.valid, false);
  // Shrinking is not offered as a fix: the verdict names the geometry, not the weights.
  assert.match(bad.verdict, /geometry problems must be fixed before weights/);

  // Penetration is monotone in clearance: pull the same garment 30 mm inwards and the
  // reported overlap must grow. A number that does not respond to the geometry is a
  // number nobody should act on.
  const tight = fittedSleeve(TORSO_BONES, [0.56, 0.62, 0.68, 0.72], -0.03);
  const tightReport = inspectGarment({ garment: tight, body, character });
  assert.ok(tightReport.fit.penetratingVertices > report.fit.penetratingVertices,
    `penetration did not increase when the garment was shrunk `
    + `(${report.fit.penetratingVertices} → ${tightReport.fit.penetratingVertices})`);
  assert.ok(tightReport.fit.penetration.maximumMetres > report.fit.penetration.maximumMetres);
});

test('a two-leg garment is reported as two shells and an inside-out one is refused', () => {
  const report = inspectGarment({ garment: pants, body, character });
  assert.equal(report.topology.components, 2);
  assert.ok(report.diagnostics.some((entry) => entry.code === 'NG-E-GARMENT-SHELLS'));
  // Both shells have positive volume; the trousers are not accidentally mirrored.
  assert.ok(report.topology.signedVolumeM3 > 0);

  const flipped = { positions: pants.positions, indices: [] };
  for (let triangle = 0; triangle < pants.indices.length / 3; ++triangle) {
    flipped.indices.push(pants.indices[triangle * 3], pants.indices[triangle * 3 + 2], pants.indices[triangle * 3 + 1]);
  }
  const insideOut = inspectGarment({ garment: flipped, body, character });
  assert.ok(insideOut.diagnostics.some((entry) => entry.code === 'NG-E-GARMENT-MIRROR'),
    'an inside-out garment must be reported as a negative-volume mesh');
});

// ----------------------------------------------------------------- bindings
test('every bound vertex has legal, normalised weights within the influence limit', () => {
  for (const [label, garment, regions] of [
    ['tank top', tankTop, TANK_REGIONS], ['pants', pants, PANTS_REGIONS], ['skirt', skirt, SKIRT_REGIONS],
  ]) {
    const bound = bindGarment({ garment, character, allowedRegions: regions });
    const validation = bound.validation;
    assert.equal(validation.invalid.negative, 0, `${label}: negative weights`);
    assert.equal(validation.invalid.nonFinite, 0, `${label}: non-finite weights`);
    assert.equal(validation.invalid.zeroTotal, 0, `${label}: unweighted vertices`);
    assert.equal(validation.invalid.overLimit, 0, `${label}: more than ${DEFAULT_MAX_INFLUENCES} influences`);
    assert.ok(validation.invalid.sumError <= 1e-4, `${label}: sum error ${validation.invalid.sumError}`);
    assert.equal(validation.valid, true, `${label}: ${JSON.stringify(validation.invalid)}`);
    assert.equal(bound.stats.unresolvedVertices, 0, `${label}: unresolved vertices`);
    for (const count of Object.keys(validation.influenceHistogram)) {
      assert.ok(Number(count) <= DEFAULT_MAX_INFLUENCES);
    }
  }
});

test('a shirt binds only to the regions it is allowed to, and never across the body', () => {
  const bound = bindGarment({ garment: tankTop, character, allowedRegions: TANK_REGIONS });
  const used = new Set(bound.partition.regions);
  for (const region of used) assert.ok(TANK_REGIONS.includes(region), `tank top bound to ${region}`);
  assert.ok(used.has('torso'), 'a tank top with no torso vertices is not binding to the torso');

  // Dominant bone must be a torso bone. The bottom ring sits at the hip crease, where the
  // body surface is already dominated by the thigh, so a hem vertex weighted to the thigh
  // is correct; what must never happen is the torso being driven by an arm, a hand or the
  // head, which is what a nearest-triangle transfer does when the arms hang beside the body.
  let torsoDominant = 0;
  for (let vertex = 0; vertex < bound.weights.vertices; ++vertex) {
    const dominant = dominantBone(bound.weights, vertex);
    assert.ok(!/Arm|Hand|Head|Neck|Calf|Foot/.test(dominant.name),
      `tank top vertex ${vertex} is dominated by ${dominant.name}`);
    if (/Spine|Hips|Thigh/.test(dominant.name)) ++torsoDominant;
  }
  assert.ok(torsoDominant > bound.weights.vertices * 0.85,
    `only ${torsoDominant} of ${bound.weights.vertices} vertices are dominated by a torso bone`);
});

test('trousers keep left and right apart through transfer and smoothing', () => {
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  const centreX = bound.partition.centreX;
  let wrongSide = 0;
  let leftVertices = 0, rightVertices = 0;
  for (let vertex = 0; vertex < bound.weights.vertices; ++vertex) {
    const x = bound.garment.positions[vertex * 3];
    const dominant = dominantBone(bound.weights, vertex);
    const isRightBone = /Right/.test(dominant.name);
    const isLeftBone = /Left/.test(dominant.name);
    if (x > centreX + 0.02) {
      ++leftVertices;
      if (isRightBone) ++wrongSide;
    } else if (x < centreX - 0.02) {
      ++rightVertices;
      if (isLeftBone) ++wrongSide;
    }
  }
  assert.equal(wrongSide, 0, `${wrongSide} trouser vertices are dominated by the opposite leg`);
  // Both legs are actually present in the binding; a symmetric split is the expected shape
  // for a symmetric fixture, and a large imbalance means one shell took the other's bones.
  assert.ok(leftVertices > 50 && rightVertices > 50, `${leftVertices} left / ${rightVertices} right vertices`);
  assert.ok(Math.abs(leftVertices - rightVertices) / Math.max(leftVertices, rightVertices) < 0.15,
    `left/right split is ${leftVertices}/${rightVertices}`);
});

test('smoothing does not let a far-side influence spread across the body', () => {
  // The invariant behind the previous test: a vertex visibly on one side must not gain the
  // other side's bone from its neighbours, however many smoothing passes run. The input is
  // deliberately contaminated, because a correctly transferred garment has nothing to
  // clean up — and a guard that is never exercised is not a guard.
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  const planted = {
    ...bound.weights,
    joints: Uint32Array.from(bound.weights.joints),
    weights: Float32Array.from(bound.weights.weights),
  };
  const rightBone = boneIndex('mixamorig:RightUpLeg');
  const centreX = bound.partition.centreX;
  // Find a vertex clearly on the right and give it 100% left-leg weight.
  let target = -1;
  for (let vertex = 0; vertex < planted.vertices; ++vertex) {
    if (bound.garment.positions[vertex * 3] < centreX - 0.03) { target = vertex; break; }
  }
  assert.ok(target >= 0);
  for (let slot = 0; slot < planted.maximumInfluences; ++slot) {
    planted.joints[target * planted.maximumInfluences + slot] = slot === 0 ? boneIndex('mixamorig:LeftUpLeg') : 0;
    planted.weights[target * planted.maximumInfluences + slot] = slot === 0 ? 1 : 0;
  }

  const smoothed = smoothWeights(planted, bound.garment, {
    iterations: 4, factor: 0.7, regions: bound.partition.regions,
    sides: bound.partition.sides, character,
  });
  // The contaminated vertex keeps its (wrong) own weight, but it must not hand that weight
  // to its neighbours: the far side is filtered out of the neighbour average. A small
  // far-side weight that the *binding* put there is allowed — a crotch vertex legitimately
  // shares both legs — so the comparison is before-versus-after, not against zero.
  const farWeight = (weights, vertex) => {
    let total = 0;
    for (let slot = 0; slot < weights.maximumInfluences; ++slot) {
      if (weights.joints[vertex * weights.maximumInfluences + slot] === boneIndex('mixamorig:LeftUpLeg')) {
        total += weights.weights[vertex * weights.maximumInfluences + slot];
      }
    }
    return total;
  };
  for (const neighbour of [target - 1, target + 1]) {
    if (neighbour < 0 || neighbour >= smoothed.weights.vertices || neighbour === target) continue;
    assert.ok(farWeight(smoothed.weights, neighbour) <= farWeight(bound.weights, neighbour) + 1e-6,
      `neighbour ${neighbour} gained left-leg weight from smoothing`);
  }
  assert.ok(rightBone !== undefined);
});

test('a loose skirt hangs from the hips instead of snapping to the nearest leg', () => {
  // `influenceRegions` is the author's statement that the skirt hangs from the pelvis: a
  // hips region is compatible with the thighs by design, so without this the hem takes
  // thigh weight from whatever body triangle is underneath it.
  const bound = bindGarment({
    garment: skirt, character, allowedRegions: SKIRT_REGIONS, influenceRegions: SKIRT_REGIONS,
  });
  assert.ok(bound.stats.filteredRegionInfluences > 0, 'the skirt never had leg influence to remove');
  assert.ok(bound.partition.propagated > 0,
    'a skirt hem 20 cm from the body should inherit its region, not be measured against a leg');
  for (const region of new Set(bound.partition.regions)) {
    assert.ok(SKIRT_REGIONS.includes(region), `skirt bound to ${region}`);
  }
  // Nothing on a skirt may be dominated by a leg bone: that is the failure this test
  // exists for, and it is the reason the free hem is weighted to the hips.
  for (let vertex = 0; vertex < bound.weights.vertices; ++vertex) {
    const dominant = dominantBone(bound.weights, vertex);
    assert.ok(!/Thigh|Leg|Foot/.test(dominant.name),
      `skirt vertex ${vertex} is dominated by ${dominant.name}`);
  }
  // Propagated vertices are reported as low confidence by construction (0.35), so the
  // count of low-confidence vertices is at least the count of propagated ones.
  assert.ok(bound.stats.lowConfidenceVertices >= bound.partition.propagated);
  assert.ok(bound.diagnostics.some((entry) => entry.code === 'NG-E-GARMENT-PROPAGATED'));
});

test('without a declared region set the binder does misassign, and says so', () => {
  // This is the negative control for `allowedRegions`. It documents the limitation instead
  // of claiming the nearest-triangle rule is always right: with the hands hanging beside
  // the hips, a trouser waist vertex is close enough to a hand.
  const unconstrained = bindGarment({ garment: pants, character, inspect: false });
  const regions = new Set(unconstrained.partition.regions);
  const unexpected = [...regions].filter((region) => !PANTS_REGIONS.includes(region));
  assert.ok(unexpected.length > 0,
    'the unconstrained binder assigned no out-of-set region; the negative control no longer demonstrates the risk');
});

// ------------------------------------------------------------- locks, limits
test('a locked vertex keeps its exact weights through smoothing and reduction', () => {
  const spine = boneIndex('mixamorig:Spine2');
  const hips = boneIndex('mixamorig:Hips');
  const lock = { 0: [{ bone: spine, weight: 0.75 }, { bone: hips, weight: 0.25 }] };
  const bound = bindGarment({ garment: tankTop, character, allowedRegions: TANK_REGIONS, locks: lock });
  const read = (vertex) => {
    const entries = [];
    for (let slot = 0; slot < bound.weights.maximumInfluences; ++slot) {
      const weight = bound.weights.weights[vertex * bound.weights.maximumInfluences + slot];
      if (weight > 0) entries.push({ bone: bound.weights.joints[vertex * bound.weights.maximumInfluences + slot], weight });
    }
    return entries;
  };
  const entries = read(0);
  assert.equal(entries.length, 2);
  assert.equal(entries[0].bone, spine);
  assert.ok(Math.abs(entries[0].weight - 0.75) < 1e-3, `locked weight is ${entries[0].weight}`);
  assert.ok(Math.abs(entries[1].weight - 0.25) < 1e-3);
  assert.equal(bound.validation.invalid.lockedViolations, 0);
  assert.equal(bound.validation.valid, true);

  // A lock that does not fit the influence limit cannot be honoured, and that is an error
  // with a diagnostic rather than a silent trim.
  const tooMany = {
    0: [
      { bone: boneIndex('mixamorig:Spine2'), weight: 0.4 },
      { bone: boneIndex('mixamorig:Spine1'), weight: 0.2 },
      { bone: boneIndex('mixamorig:Spine'), weight: 0.15 },
      { bone: hips, weight: 0.15 },
      { bone: boneIndex('mixamorig:Neck'), weight: 0.1 },
    ],
  };
  const overflowing = bindGarment({ garment: tankTop, character, allowedRegions: TANK_REGIONS, locks: tooMany });
  assert.ok(overflowing.validation.invalid.lockedViolations > 0);
  assert.ok(overflowing.diagnostics.some((entry) => entry.code === 'NG-E-GARMENT-LOCK'));
  assert.equal(overflowing.validation.valid, false);
});

test('influence reduction keeps normalisation and records what it dropped', () => {
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  const reduced = sparsifyWeights(bound.weights, { maximumInfluences: 2 });
  const validation = validateWeights(reduced.weights, { maximumInfluences: 2 });
  assert.equal(validation.valid, true, JSON.stringify(validation.invalid));
  assert.ok(validation.invalid.sumError <= 1e-4);
  for (const count of Object.keys(validation.influenceHistogram)) assert.ok(Number(count) <= 2);
  assert.ok(reduced.droppedInfluences > 0, 'no influence was dropped, so the limit was not exercised');

  // Smoothing with a lock map passed as documented must actually honour the mask: this is
  // the call shape that used to be silently ignored.
  const vertex = 12;
  const entries = [];
  for (let slot = 0; slot < bound.weights.maximumInfluences; ++slot) {
    const weight = bound.weights.weights[vertex * bound.weights.maximumInfluences + slot];
    if (weight > 0) entries.push({ bone: bound.weights.joints[vertex * bound.weights.maximumInfluences + slot], weight });
  }
  const locks = new Map([[vertex, entries]]);
  const smoothed = smoothWeights(applyLocks(bound.weights, locks).weights, bound.garment, {
    iterations: 3, factor: 0.6, regions: bound.partition.regions, locked: locks,
  });
  const check = validateWeights(smoothed.weights, { maximumInfluences: DEFAULT_MAX_INFLUENCES, locked: locks });
  assert.equal(check.invalid.lockedViolations, 0, 'smoothing changed a locked vertex');
  assert.equal(check.valid, true);
});

// --------------------------------------------------------------- patch guard
test('a weight patch written for another topology is refused', () => {
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  const moved = parseMesh({
    positions: bound.garment.positions.map((value, index) => (index % 3 === 0 ? value + 2e-3 : value)),
    indices: bound.garment.indices,
  }, { label: 'moved' });
  assert.notEqual(topologyHash(moved), bound.topologyHash);

  assert.throws(() => patchWeights(moved, bound.weights, [
    { kind: 'set', vertexId: 0, bone: boneIndex('mixamorig:Hips'), value: 1 },
  ], { character, expectedTopology: bound.topologyHash }), (error) => {
    assert.ok(error instanceof GarmentError);
    assert.equal(error.code, 'NG-E-GARMENT-TOPOLOGY');
    assert.match(error.message, /re-inspect before patching/);
    return true;
  });

  // The same patch against the mesh it was written for succeeds and reports the change.
  const applied = patchWeights(bound.garment, bound.weights, [
    { kind: 'set', vertexId: 0, bone: boneIndex('mixamorig:Hips'), value: 1 },
  ], { character, expectedTopology: bound.topologyHash });
  assert.equal(applied.changes.length, 1);
  assert.equal(applied.changes[0].after, 1);
  assert.ok(applied.changes[0].before >= 0);

  // A raw position/index object is a caller mistake, not a topology change.
  assert.throws(() => patchWeights({ positions: moved.positions, indices: moved.indices }, bound.weights, [
    { kind: 'set', vertexId: 0, bone: 0, value: 0.5 },
  ]), (error) => error.code === 'NG-E-GARMENT-MESH');
});

test('mirroring uses an explicit bone map, not a negated coordinate', () => {
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  assert.throws(() => mirrorWeights(bound.weights, bound.garment, {}),
    (error) => error.code === 'NG-E-GARMENT-WEIGHTS');

  const map = mirrorBoneMap(character);
  const leftThigh = boneIndex('mixamorig:LeftUpLeg');
  const rightThigh = boneIndex('mixamorig:RightUpLeg');
  assert.equal(map.get(leftThigh), rightThigh);
  assert.equal(map.get(rightThigh), leftThigh);
  // Central bones map to themselves rather than disappearing from the map.
  assert.equal(map.get(boneIndex('mixamorig:Hips')), boneIndex('mixamorig:Hips'));

  const mirrored = mirrorWeights(bound.weights, bound.garment, { boneMap: map, side: 'left' });
  assert.ok(mirrored.mirroredVertices > 50, `only ${mirrored.mirroredVertices} vertices were mirrored`);
  assert.equal(mirrored.pairs, mirrored.mirroredVertices);
});

// -------------------------------------------------------------- simulation
test('the physics report names its failures and its unsupported cases', () => {
  const bound = bindGarment({ garment: pants, character, allowedRegions: PANTS_REGIONS });
  const report = validateGarmentPhysics(bound, { ticks: 24 });
  assert.equal(report.schema, 'nativegpu.garment-simulation/1');
  assert.equal(report.poses.length, Object.keys(GARMENT_TEST_POSES).length + 1);
  assert.ok(report.poses.some((pose) => pose.pose === 'after_teleport'));

  // Every pose is reported separately: a garment that fails while running has not passed
  // because it passed while standing.
  for (const pose of report.poses) {
    assert.equal(pose.nonFiniteEvents, 0, `${pose.pose} produced non-finite values`);
    assert.ok(Number.isFinite(pose.worstPenetrationP99Metres));
    assert.ok(pose.maxStretchRatio < 3, `${pose.pose} stretched ${pose.maxStretchRatio}×`);
  }
  // Penetration failures, when they happen, name the pose and the code instead of being
  // folded into a single average.
  for (const failure of report.failures) {
    assert.ok(failure.pose, `${failure.code} has no pose`);
    assert.match(failure.code, /^NG-E-GARMENT-/);
  }
  assert.equal(report.passes, report.failures.length === 0);
  // Self-collision is not implemented, and the report says so rather than implying the
  // result covers a long dress.
  assert.equal(SUPPORTED.selfCollision, false);
  assert.ok(report.unsupportedClaims.includes('self-collision'));
  assert.match(report.verdict, /self-collision is not supported|self-collision, which this solver does not implement/);
});

test('mesh penetration is measured against the body, not against the proxies', () => {
  const bound = bindGarment({ garment: skirt, character, allowedRegions: SKIRT_REGIONS });
  const report = validateGarmentPhysics(bound, { ticks: 8, poses: { idle: GARMENT_TEST_POSES.idle } });
  const measurement = report.poses[0].meshPenetration;
  assert.ok(measurement, 'the mesh penetration measurement is missing');
  assert.ok(Number.isFinite(measurement.p99Metres));
  assert.ok(measurement.penetratingVertices >= 0);
  // Proxy and mesh measurement are separate numbers, and both are reported: the proxies
  // decide what the solver pushed against, the mesh decides whether the visible surface
  // actually left the body. Reporting only one of them is how a garment passes physics
  // while the drawn surface is inside the character.
  assert.ok(Number.isFinite(report.poses[0].worstPenetrationP99Metres));
  assert.ok(Number.isFinite(measurement.p95Metres));
  assert.ok(measurement.maximumMetres >= measurement.p99Metres);
  assert.equal(typeof measureMeshPenetration, 'function');
});

test('the end-to-end validation composes geometry, weights and physics', () => {
  const report = validateGarment({
    garment: tankTop, character, allowedRegions: TANK_REGIONS,
    poses: { idle: GARMENT_TEST_POSES.idle }, ticks: 8,
  });
  assert.equal(report.schema, 'nativegpu.garment-validation/1');
  assert.ok(report.inspection);
  assert.equal(report.weights.valid, true);
  assert.ok(report.simulation);
  assert.equal(report.unsupported.selfCollision, false);
  assert.equal(report.passes, report.failures.length === 0);
  for (const failure of report.failures) assert.ok(failure.code, 'a failure with no code cannot be acted on');
});

test('the heatmap and the numbers come from one snapshot', () => {
  const bound = bindGarment({ garment: tankTop, character, allowedRegions: TANK_REGIONS });
  const snapshot = weightsSnapshot(bound.garment, bound.weights, { character });
  assert.equal(snapshot.schema, 'nativegpu.weights-snapshot/1');
  // The snapshot carries the topology it was taken from, so a heatmap drawn from it cannot
  // be paired with numbers from a different mesh.
  assert.equal(snapshot.topologyHash, bound.topologyHash);
  assert.equal(snapshot.vertexCount, bound.weights.vertices);
  assert.equal(snapshot.records.length, bound.weights.vertices);
  // The numeric export and the picture are the same data: every record sums to one, and
  // the per-vertex bone list matches what the heatmap would colour.
  let maximumResidual = 0;
  for (const record of snapshot.records) {
    const sum = record.influences.reduce((total, entry) => total + entry.weight, 0);
    maximumResidual = Math.max(maximumResidual, Math.abs(sum - 1), record.residual);
    assert.ok(record.influences.length <= snapshot.maximumInfluences);
  }
  assert.ok(maximumResidual <= 1e-4, `snapshot residual ${maximumResidual}`);
  // The digest changes when the weights change, which is what makes an exported report
  // checkable against a later re-export.
  const rebound = bindGarment({ garment: tankTop, character, allowedRegions: TANK_REGIONS, smooth: { iterations: 4, factor: 0.6 } });
  const other = weightsSnapshot(rebound.garment, rebound.weights, { character });
  assert.notEqual(other.digest, snapshot.digest);
  assert.equal(other.topologyHash, snapshot.topologyHash, 'smoothing must not change the topology hash');
  const totals = influenceTotals(bound.weights, character.bones);
  const sum = totals.reduce((total, entry) => total + entry.total, 0);
  assert.ok(Math.abs(sum - bound.weights.vertices) < 1, `influence totals sum to ${sum} for ${bound.weights.vertices} vertices`);
});

test('the pose set covers the motion the plan asks for', () => {
  for (const required of ['idle', 'walk_contact', 'run_stride', 'turn_fast', 'arms_up', 'crouch', 'jump']) {
    assert.ok(GARMENT_TEST_POSES[required], `the pose set has no ${required}`);
  }
  // Poses are described the way a person describes them and resolved to quaternions only
  // when the solver needs them.
  const walk = GARMENT_TEST_POSES.walk_contact;
  const first = Object.values(walk)[0];
  assert.ok(Array.isArray(first.axis) && Number.isFinite(first.radians));
  const quaternion = axisAngleQuaternion(first.axis, first.radians);
  assert.equal(quaternion.length, 4);
});

import { HairError } from './spec.mjs';
import { measureBindingDrift, characterCollisionProxies, deformScalpPositions } from './scalp.mjs';
import { skinPalette, poseSkeleton, axisAngleQuaternion } from '../character/rig-mesh.mjs';
import { lodTransitionDelta } from './shading.mjs';

// Hairstyle validation.
//
// The rule this module follows: it reports what it measured, and it lists what it did
// not measure. A validation that says "performance: ok" without a single GPU timestamp
// is worse than no validation, because it turns an unknown into an approval.
//
// Measured here:
//   * root binding on the character's scalp under a set of poses,
//   * guide penetration against bone-driven collision proxies,
//   * LOD identity (roots do not move) and strand-volume change,
//   * budget limits.
// Not measured here, and reported as such:
//   * GPU generation, simulation and draw timings, video memory,
//   * frame-level transparency overflow on real hardware.

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

function percentile(sorted, fraction) {
  if (sorted.length === 0) return 0;
  const index = Math.min(sorted.length - 1, Math.max(0, Math.floor(sorted.length * fraction)));
  return sorted[index];
}

function distanceToSegment(point, a, b) {
  const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const ap = [point[0] - a[0], point[1] - a[1], point[2] - a[2]];
  const lengthSquared = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  const t = lengthSquared > 0
    ? Math.max(0, Math.min(1, (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / lengthSquared))
    : 0;
  const closest = [a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t];
  return Math.hypot(point[0] - closest[0], point[1] - closest[1], point[2] - closest[2]);
}

// Penetration of a guide point cloud against bone-driven proxies. Points near the root
// are skipped: a root sits *on* the scalp, so counting it as penetrating the head
// sphere reports a failure for every valid hairstyle.
export function penetrationReport(guides, proxies, { skipRootFraction = 0.25 } = {}) {
  const depths = [];
  let worst = { guideId: null, depthMetres: 0, point: null };
  for (const guide of guides) {
    const points = guide.points ?? [];
    const start = Math.floor(points.length * skipRootFraction);
    for (let index = start; index < points.length; ++index) {
      const point = points[index];
      for (const sphere of proxies.spheres ?? []) {
        const distance = Math.hypot(point[0] - sphere.centre[0], point[1] - sphere.centre[1], point[2] - sphere.centre[2]);
        const depth = sphere.radiusMetres - distance;
        if (depth > 0) {
          depths.push(depth);
          if (depth > worst.depthMetres) worst = { guideId: guide.stableId, depthMetres: depth, point, proxy: sphere.bone };
        }
      }
      for (const capsule of proxies.capsules ?? []) {
        const depth = capsule.radiusMetres - distanceToSegment(point, capsule.a, capsule.b);
        if (depth > 0) {
          depths.push(depth);
          if (depth > worst.depthMetres) worst = { guideId: guide.stableId, depthMetres: depth, point, proxy: capsule.bone };
        }
      }
    }
  }
  depths.sort((a, b) => a - b);
  return {
    samples: depths.length,
    p95Metres: percentile(depths, 0.95),
    p99Metres: percentile(depths, 0.99),
    maximumMetres: depths.length ? depths[depths.length - 1] : 0,
    worst,
    penetratingGuides: new Set(depths.length ? [worst.guideId] : []).size,
  };
}

// Poses the plan names for hair: a head turn, a neck bend and a raised shoulder. They
// exist because a hairstyle that survives only the rest pose is not a hairstyle.
export const HAIR_TEST_POSES = Object.freeze({
  rest: {},
  head_turn: { head: { axis: [0, 1, 0], radians: 0.9 } },
  neck_bend: { neck: { axis: [1, 0, 0], radians: 0.45 } },
  head_tilt: { head: { axis: [0, 0, 1], radians: 0.4 }, neck: { axis: [0, 1, 0], radians: 0.3 } },
  shoulder_raise: { leftshoulder: { axis: [0, 0, 1], radians: -0.35 }, rightshoulder: { axis: [0, 0, 1], radians: 0.35 } },
});

function quaternionFor(entry, axisAngle) {
  return axisAngle(entry.axis, entry.radians);
}

export function validateHairstyle({
  generated, character, poses = HAIR_TEST_POSES, penetrationToleranceMetres = 0.002,
  bindingToleranceMetres = 0.0005, timings = null,
}) {
  fail(generated && generated.guides, 'NG-E-HAIR-VALIDATE', 'a generated hairstyle is required');
  fail(character, 'NG-E-HAIR-VALIDATE', 'the character is required: binding cannot be validated without it');
  const bindings = generated.guides.map((guide) => ({ triangle: guide.triangle, bary: guide.bary }));
  const failures = [];
  const poseReports = [];

  for (const [name, pose] of Object.entries(poses)) {
    const resolved = Object.fromEntries(Object.entries(pose).map(([bone, entry]) => [bone, quaternionFor(entry, axisAngleQuaternion)]));
    const drift = measureBindingDrift(character, bindings, generated.scalp, {
      pose: resolved, headBone: generated.spec.character.headBone, toleranceMetres: bindingToleranceMetres,
    });
    const proxies = characterCollisionProxies(character, {
      bones: ['head', 'neck', 'leftshoulder', 'rightshoulder', 'chest'], pose: resolved,
    });
    // The guides are generated at rest; moving the proxies under a posed skeleton is
    // what the solver sees when the character turns. It is a static approximation of a
    // dynamic solve, and it is reported as such.
    const penetration = penetrationReport(generated.guides, proxies);
    const strict = name === 'rest';
    poseReports.push({
      pose: name,
      rootsOffSurface: drift.rootsOffSurface,
      maxSurfaceErrorMetres: drift.maximumSurfaceErrorMetres,
      penetration: {
        samples: penetration.samples, p95Metres: penetration.p95Metres,
        p99Metres: penetration.p99Metres, maximumMetres: penetration.maximumMetres,
        worst: penetration.worst,
      },
      // Only the rest pose is a hard failure. A posed check compares a *static*
      // hairstyle against moved proxies; that difference is what the solver exists to
      // resolve, so failing it here would be claiming a simulation result a static
      // check cannot produce.
      enforced: strict,
      requiresSolver: !strict,
    });
    if (drift.rootsOffSurface > 0) {
      failures.push({
        code: 'NG-E-HAIR-ROOT-DETACHED', pose: name,
        message: `${drift.rootsOffSurface} roots left the scalp under pose ${name} (max ${(drift.maximumSurfaceErrorMetres * 1000).toFixed(3)} mm)`,
      });
    }
    if (strict && penetration.p99Metres > penetrationToleranceMetres) {
      failures.push({
        code: 'NG-E-HAIR-PENETRATION', pose: name,
        message: `P99 penetration ${(penetration.p99Metres * 1000).toFixed(2)} mm exceeds `
          + `${(penetrationToleranceMetres * 1000).toFixed(2)} mm at rest `
          + `(worst guide ${penetration.worst.guideId} against ${penetration.worst.proxy})`,
      });
    }
  }

  // LOD identity: the same strands, same roots, only fewer of them.
  const lodChecks = [];
  const level0 = generated.levels[0];
  for (const level of generated.levels.slice(1)) {
    const a = { roots: level0.curves.map((curve) => curve.points[0]) };
    const b = { roots: level.curves.map((curve) => curve.points[0]) };
    const transition = lodTransitionDelta(a, b);
    // Compare only the strands both levels contain, matched by stable id.
    const byId = new Map(level0.curves.map((curve) => [curve.stableId, curve]));
    let maximum = 0, compared = 0;
    for (const curve of level.curves) {
      const reference = byId.get(curve.stableId);
      if (!reference) continue;
      ++compared;
      maximum = Math.max(maximum, Math.hypot(
        curve.points[0][0] - reference.points[0][0],
        curve.points[0][1] - reference.points[0][1],
        curve.points[0][2] - reference.points[0][2]));
    }
    lodChecks.push({
      level: level.level, compared, maxRootDriftMetres: maximum,
      strands: level.curves.length, sourceStrands: level0.curves.length,
      sharedIdentity: compared === level.curves.length,
      naiveIndexDrift: transition.maxRootDriftMetres,
    });
    if (maximum > 1e-6) {
      failures.push({
        code: 'NG-E-HAIR-LOD-DRIFT', level: level.level,
        message: `LOD ${level.level} moves strand roots by up to ${(maximum * 1000).toFixed(3)} mm`,
      });
    }
  }

  const budget = generated.spec.budget;
  if (generated.stats.strands > budget.maxStrands) {
    failures.push({ code: 'NG-E-HAIR-BUDGET', message: `strand count exceeds budget.maxStrands` });
  }
  if (generated.stats.strandPoints > budget.maxPoints) {
    failures.push({ code: 'NG-E-HAIR-BUDGET', message: `point count exceeds budget.maxPoints` });
  }

  const unmeasured = [
    'GPU strand generation timing',
    'GPU guide simulation timing and stability over time',
    'GPU draw timing and video memory',
    'per-frame transparency layer overflow on target hardware',
    'shimmer under real camera motion (needs a rendered sequence, not a static pose)',
    'back-lit transmission and shadowed self-occlusion (needs a rendered frame)',
  ];
  return {
    schema: 'nativegpu.hair-validation/1',
    hairstyle: generated.spec.id,
    revision: generated.spec.__revision ?? null,
    passes: failures.length === 0,
    failures,
    poses: poseReports,
    lod: lodChecks,
    counts: generated.stats,
    scalp: {
      hash: generated.scalpHash, vertices: generated.scalp.vertices,
      triangles: generated.scalp.triangles, headBone: generated.scalp.headBone,
    },
    timings: timings ?? {
      measured: false,
      note: 'no GPU or native timings were collected for this validation run',
    },
    unmeasured,
    verdict: failures.length === 0
      ? 'geometry and binding checks passed; rendering and performance claims still require a native capture'
      : 'failed; see failures — do not publish this hairstyle',
  };
}

// Kept exported so callers do not re-implement the same reduction.
export { deformScalpPositions, skinPalette, poseSkeleton };

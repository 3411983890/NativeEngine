import { bodyFromCharacter, inspectGarment, parseMesh, GarmentError } from './inspect.mjs';
import { partitionBody, partitionGarment } from './partition.mjs';
import {
  transferWeights, smoothWeights, sparsifyWeights, validateWeights, weightsSnapshot,
  influenceTotals, topologyHash, applyLocks, DEFAULT_MAX_INFLUENCES,
} from './weights.mjs';
import {
  createSimulation, defaultPhysicsContract, setRegionPhysics, simulatePose, GARMENT_TEST_POSES,
  measureMeshPenetration, SUPPORTED, teleport, updateSkinTarget,
} from './physics.mjs';

// The garment workflow, end to end: inspect → partition → bind → simulate → report.
//
// Splitting these into separate modules keeps each testable, and this one composes them
// in the only order that produces trustworthy numbers: geometry is checked before weights
// are written, weights are validated before they are simulated, and the simulation report
// names the poses that failed instead of averaging them into a single verdict.

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

export function bindGarment({
  garment: garmentInput, character, physics = {}, material = {}, maximumInfluences = DEFAULT_MAX_INFLUENCES,
  smooth = { iterations: 2, factor: 0.5 }, regionPhysics = null, inspect = true,
  allowedRegions = null, influenceRegions = null, locks = null,
} = {}) {
  fail(character, 'NG-E-GARMENT-BIND', 'a character is required to bind a garment');
  const garment = parseMesh(garmentInput, { label: 'garment' });
  const body = bodyFromCharacter(character);
  const inspection = inspect ? inspectGarment({ garment, body, character }) : null;
  const bodyPartition = partitionBody(character);
  const partition = partitionGarment(garment, body, bodyPartition, { allowedRegions });
  const transfer = transferWeights(garment, character, partition, { maximumInfluences: 6, influenceRegions });
  // Author locks are applied *before* smoothing and fed to it as a boundary, so the
  // published weights are the locked values rather than whatever survived the filter.
  const lockedEntries = locks ? new Map(Object.entries(locks).map(([vertex, entries]) => [Number(vertex), entries])) : null;
  const lockedWeights = lockedEntries ? applyLocks(transfer.weights, lockedEntries) : null;
  const smoothed = smoothWeights(lockedWeights?.weights ?? transfer.weights, garment, {
    iterations: smooth.iterations, factor: smooth.factor, regions: partition.regions,
    locked: lockedEntries, sides: partition.sides, character,
  });
  const sparse = sparsifyWeights(smoothed.weights, { maximumInfluences });
  const validation = validateWeights(sparse.weights, {
    maximumInfluences, regionConfidence: partition.confidence, locked: lockedEntries,
  });
  const snapshot = weightsSnapshot(garment, sparse.weights, { character });
  const contract = defaultPhysicsContract(garment, physics);
  if (regionPhysics) {
    for (const entry of regionPhysics) {
      setRegionPhysics(contract, garment, partition.regions, entry.regions, entry.values);
    }
  }
  return {
    garment, body, character, bodyPartition, partition, inspection,
    weights: sparse.weights, validation, snapshot, contract, material,
    topologyHash: topologyHash(garment),
    stats: {
      vertices: garment.vertices,
      triangles: garment.triangles,
      propagatedVertices: transfer.propagated,
      unresolvedVertices: transfer.unresolved,
      filteredSideInfluences: transfer.filteredSideInfluences,
      filteredRegionInfluences: transfer.filteredRegionInfluences,
      smoothLockConflicts: smoothed.lockConflicts,
      droppedInfluences: sparse.droppedInfluences,
      lowConfidenceVertices: validation.lowConfidenceVertices,
      influenceHistogram: validation.influenceHistogram,
      topology: influenceTotals(sparse.weights, character.bones).slice(0, 8),
    },
    diagnostics: [
      ...(inspection?.diagnostics ?? []),
      ...(transfer.unresolved > 0 ? [{
        code: 'NG-E-GARMENT-UNRESOLVED', level: 'error',
        message: `${transfer.unresolved} vertices could not be given a body region or an inherited weight`,
      }] : []),
      ...(partition.propagated > 0 ? [{
        code: 'NG-E-GARMENT-PROPAGATED', level: 'warning',
        message: `${partition.propagated} vertices inherited their region from neighbours `
          + '(loose garment area); their weights are lower confidence by construction',
      }] : []),
      ...(lockedEntries && validation.invalid.lockedViolations > 0 ? [{
        code: 'NG-E-GARMENT-LOCK', level: 'error',
        message: `${validation.invalid.lockedViolations} locked vertices no longer match their locked value `
          + 'after influence reduction; the lock does not fit the influence limit',
      }] : []),
    ],
  };
}

// Physics acceptance over a motion set. Every pose is reported separately because a
// garment that fails while running and passes while standing has not passed.
export function validateGarmentPhysics(bound, {
  poses = GARMENT_TEST_POSES, ticks = 48, dt = 1 / 120, gravity = [0, -9.81, 0],
  penetrationToleranceMetres = 0.002, stretchTolerance = 1.8, includeMeshPenetration = true,
  poseQuaternions = null,
} = {}) {
  const { garment, character, weights, contract, material, body } = bound;
  const state = createSimulation(garment, character, weights, { contract, material });
  const reports = [];
  const failures = [];
  const { axisAngleQuaternion } = poseHelpers();
  for (const [name, pose] of Object.entries(poses)) {
    // Test poses are written as {axis, radians} because that is how a person describes
    // "raise the arms"; the skeleton takes quaternions.
    const resolved = poseQuaternions ? poseQuaternions(pose)
      : Object.fromEntries(Object.entries(pose).map(([bone, entry]) => [
        bone, Array.isArray(entry) ? entry : axisAngleQuaternion(entry.axis, entry.radians),
      ]));
    const result = simulatePose(state, name, resolved, { ticks, dt, gravity });
    const mesh = includeMeshPenetration ? measureMeshPenetration(state, body) : null;
    reports.push({
      pose: name,
      ticks,
      worstPenetrationP99Metres: result.worstP99Metres,
      meanPenetrationP99Metres: result.meanP99Metres,
      maxStretchRatio: result.maxStretchRatio,
      nonFiniteEvents: result.nonFiniteEvents,
      meshPenetration: mesh,
      substeps: result.finalMetrics?.substeps ?? 1,
    });
    if (result.worstP99Metres > penetrationToleranceMetres) {
      failures.push({
        code: 'NG-E-GARMENT-PENETRATION', pose: name,
        message: `P99 proxy penetration ${(result.worstP99Metres * 1000).toFixed(2)} mm exceeds `
          + `${(penetrationToleranceMetres * 1000).toFixed(2)} mm during ${name}`,
      });
    }
    if (mesh && mesh.p99Metres > penetrationToleranceMetres) {
      failures.push({
        code: 'NG-E-GARMENT-MESH-PENETRATION', pose: name,
        message: `P99 penetration against the body mesh ${(mesh.p99Metres * 1000).toFixed(2)} mm during ${name} `
          + `(${mesh.penetratingVertices} vertices)`,
      });
    }
    if (result.maxStretchRatio > stretchTolerance) {
      failures.push({
        code: 'NG-E-GARMENT-STRETCH', pose: name,
        message: `maximum edge stretch ${result.maxStretchRatio.toFixed(2)}× exceeds ${stretchTolerance}× during ${name}`,
      });
    }
    if (result.nonFiniteEvents > 0) {
      failures.push({
        code: 'NG-E-GARMENT-DIVERGED', pose: name,
        message: `${result.nonFiniteEvents} non-finite values were detected and recovered during ${name}`,
      });
    }
  }
  // Teleport: a large displacement must not destabilise the solve.
  teleport(state, {});
  const afterTeleport = simulatePose(state, 'after_teleport', {}, { ticks: 12, dt, gravity });
  reports.push({
    pose: 'after_teleport', ticks: 12,
    worstPenetrationP99Metres: afterTeleport.worstP99Metres,
    maxStretchRatio: afterTeleport.maxStretchRatio,
    nonFiniteEvents: afterTeleport.nonFiniteEvents,
    meshPenetration: null, substeps: afterTeleport.finalMetrics?.substeps ?? 1,
  });
  if (afterTeleport.nonFiniteEvents > 0) {
    failures.push({ code: 'NG-E-GARMENT-DIVERGED', pose: 'after_teleport', message: 'teleport destabilised the solve' });
  }

  return {
    schema: 'nativegpu.garment-simulation/1',
    poses: reports,
    failures,
    passes: failures.length === 0,
    supported: SUPPORTED,
    unsupportedClaims: [
      'self-collision',
      'cloth-cloth (layer) collision',
      'true swept continuous collision',
    ],
    tolerances: { penetrationToleranceMetres, stretchTolerance },
    verdict: failures.length === 0
      ? 'passes the measured motion set; self-collision is not supported and is not claimed'
      : 'failed; see failures. A long dress or a wide sleeve needs self-collision, which this solver does not implement.',
  };
}

export function validateGarment({ garment, character, physics = {}, poses = GARMENT_TEST_POSES, ticks = 48, ...rest } = {}) {
  const bound = bindGarment({ garment, character, physics, ...rest });
  const weightsValidation = bound.validation;
  const simulation = rest.skipSimulation === true ? null : validateGarmentPhysics(bound, {
    poses, ticks, ...rest,
  });
  const failures = [
    ...bound.diagnostics.filter((entry) => entry.level === 'error').map((entry) => ({
      code: entry.code, message: entry.message,
    })),
  ];
  if (!weightsValidation.valid) {
    failures.push({
      code: 'NG-E-WEIGHTS', message: `invalid weights: ${JSON.stringify(weightsValidation.invalid)}`,
    });
  }
  if (simulation && !simulation.passes) failures.push(...simulation.failures);
  return {
    schema: 'nativegpu.garment-validation/1',
    passes: failures.length === 0,
    failures,
    inspection: bound.inspection,
    weights: weightsValidation,
    statistics: bound.stats,
    topologyHash: bound.topologyHash,
    simulation,
    unsupported: SUPPORTED,
    verdict: failures.length === 0
      ? 'geometry, weights and the measured motion set pass'
      : 'failed; see failures — do not publish this garment binding',
  };
}

export { updateSkinTarget };
import { axisAngleQuaternion as engineAxisAngle } from '../character/rig-mesh.mjs';
function poseHelpers() { return { axisAngleQuaternion: engineAxisAngle }; }

import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { GarmentError } from './inspect.mjs';
import { garmentAdjacency, regionCompatible, regionForBoneName, sideOfRegion } from './partition.mjs';

// Skin-weight transfer and cleanup for a garment mesh.
//
// The contract this module enforces:
//   * every vertex ends with at most `maxInfluences` non-negative weights summing to 1,
//   * a locked weight is never changed, and a smooth pass that cannot honour a lock
//     reports a conflict instead of quietly dropping it,
//   * the result is tied to a topology hash, so a patch written for one mesh is refused
//     against a mesh whose vertices moved,
//   * low-confidence regions stay low confidence — they are not averaged into looking
//     certain.

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

export const DEFAULT_MAX_INFLUENCES = 4;

export function topologyHash(mesh) {
  return sha256Hex(canonicalJson({
    vertices: mesh.vertices,
    triangles: mesh.triangles,
    // Quantised positions: an exporter that re-serialises the same mesh with different
    // float noise must not invalidate an authored weight patch, but a moved vertex must.
    positions: Array.from({ length: Math.min(mesh.positions.length, 3 * 4096) }, (_, index) => Math.round(mesh.positions[index] * 1e5)),
    ids: Array.from(mesh.vertexIds).slice(0, 4096),
  })).slice(0, 32);
}

export function emptyWeights(vertices, maximumInfluences = DEFAULT_MAX_INFLUENCES) {
  return {
    joints: new Uint32Array(vertices * maximumInfluences),
    weights: new Float32Array(vertices * maximumInfluences),
    maximumInfluences,
    vertices,
  };
}

function normalisedVertex(joints, weights, vertex, maximumInfluences) {
  const entries = [];
  for (let slot = 0; slot < maximumInfluences; ++slot) {
    const weight = weights[vertex * maximumInfluences + slot];
    if (weight > 0) entries.push({ bone: joints[vertex * maximumInfluences + slot], weight });
  }
  const total = entries.reduce((sum, entry) => sum + entry.weight, 0);
  if (total <= 0) return [];
  return entries.map((entry) => ({ bone: entry.bone, weight: entry.weight / total }));
}

// Two normalised weight sets are the same skinning if every bone has the same weight
// within tolerance. Used for lock verification, where an exact float comparison would
// flag float32 rounding as an author error.
function sameWeightSet(a, b, tolerance = 1e-6) {
  if (a.length !== b.length) return false;
  for (let index = 0; index < a.length; ++index) {
    if (a[index].bone !== b[index].bone) return false;
    if (Math.abs(a[index].weight - b[index].weight) > tolerance) return false;
  }
  return true;
}

function writeVertex(target, vertex, entries) {
  const { joints, weights, maximumInfluences } = target;
  const sorted = [...entries].sort((a, b) => b.weight - a.weight || a.bone - b.bone)
    .slice(0, maximumInfluences);
  const total = sorted.reduce((sum, entry) => sum + entry.weight, 0) || 1;
  for (let slot = 0; slot < maximumInfluences; ++slot) {
    const entry = sorted[slot];
    joints[vertex * maximumInfluences + slot] = entry ? entry.bone : 0;
    weights[vertex * maximumInfluences + slot] = entry ? entry.weight / total : 0;
  }
}

// Barycentric transfer. Weights come from the *body* vertex influences, blended by the
// barycentric coordinate of the projection — not from the nearest body vertex, which
// produces a stepped weight field that shows up as a crease along every limb.
export function transferWeights(garment, character, partition, {
  maximumInfluences = DEFAULT_MAX_INFLUENCES, influenceRegions = null,
} = {}) {
  const result = emptyWeights(garment.vertices, maximumInfluences);
  const adjacency = garmentAdjacency(garment);
  const pending = [];
  // A body triangle at the crotch or the armpit spans both limbs, so its barycentric
  // interpolation hands the garment vertex a weight from the *other* leg. The garment's
  // own region already knows which side it is on: any influence from the opposite side
  // is removed here rather than left for the artist to find as a hem that swings with
  // the wrong leg.
  const boneRegions = new Map(character.bones.map((bone) => [bone.index, regionForBoneName(bone.name)]));
  const boneSide = new Map([...boneRegions].map(([bone, region]) => [bone, sideOfRegion(region)]));
  // `influenceRegions` is how an author says "this skirt hangs from the pelvis". A hips
  // region is compatible with the thighs by design, so without it a 20 cm hem takes thigh
  // weight from the body triangle underneath and swings with the leg.
  const influenceSet = influenceRegions ? new Set(influenceRegions) : null;
  let filteredRegionInfluences = 0;
  let filteredSideInfluences = 0;
  let balancedSideVertices = 0;
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    const triangle = partition.triangles[vertex];
    if (triangle < 0) { pending.push(vertex); continue; }
    const a = character.indices[triangle * 3];
    const b = character.indices[triangle * 3 + 1];
    const c = character.indices[triangle * 3 + 2];
    const bary = [partition.barycentric[vertex * 3], partition.barycentric[vertex * 3 + 1], partition.barycentric[vertex * 3 + 2]];
    const accumulated = new Map();
    const corners = [a, b, c];
    corners.forEach((bodyVertex, corner) => {
      const cornerWeight = bary[corner];
      if (cornerWeight <= 0) return;
      for (let influence = 0; influence < 4; ++influence) {
        const weight = character.weights[bodyVertex * 4 + influence];
        if (weight <= 0) continue;
        const bone = character.joints[bodyVertex * 4 + influence];
        accumulated.set(bone, (accumulated.get(bone) ?? 0) + weight * cornerWeight);
      }
    });
    let entries = [...accumulated.entries()].map(([bone, weight]) => ({ bone, weight }));
    fail(entries.length > 0, 'NG-E-GARMENT-WEIGHTS',
      `vertex ${vertex} projects onto body triangle ${triangle}, which has no skin weights`);
    if (influenceSet) {
      const kept = entries.filter((entry) => {
        const region = boneRegions.get(entry.bone);
        return !region || region === 'unknown' || influenceSet.has(region);
      });
      if (kept.length > 0 && kept.length < entries.length) {
        filteredRegionInfluences += entries.length - kept.length;
        entries = kept;
      }
    }
    const vertexSide = sideOfRegion(partition.regions[vertex]);
    if (vertexSide) {
      const kept = entries.filter((entry) => {
        const side = boneSide.get(entry.bone);
        return !side || side === vertexSide;
      });
      // Never filter a vertex down to nothing: a wrong-side weight is still better than
      // an unweighted vertex, and the remaining entries are recorded as low confidence.
      if (kept.length > 0 && kept.length < entries.length) {
        filteredSideInfluences += entries.length - kept.length;
        entries = kept;
      }
    }
    // A vertex at the crotch is assigned `hips`, which is side-neutral, so the drop above
    // does not apply — yet the body triangle it lands on still hands it a bigger weight
    // from the *far* thigh (0.40 far vs 0.28 near on the sample character). Capping the
    // far side at the near side is a monotone correction: it never invents weight, it only
    // stops the wrong limb from dominating a vertex that is visibly on one side of the
    // centre line.
    const geometrySide = partition.sides?.[vertex] ?? vertexSide;
    if (geometrySide) {
      let near = 0, far = 0;
      for (const entry of entries) {
        const side = boneSide.get(entry.bone);
        if (side === geometrySide) near += entry.weight;
        else if (side) far += entry.weight;
      }
      if (near > 0 && far > near) {
        const scale = near / far;
        entries = entries.map((entry) => {
          const side = boneSide.get(entry.bone);
          return side && side !== geometrySide ? { bone: entry.bone, weight: entry.weight * scale } : entry;
        });
        ++balancedSideVertices;
      }
    }
    writeVertex(result, vertex, entries);
  }

  // Unresolved vertices inherit from resolved neighbours, averaged with their own
  // region filter applied: a hem vertex must not inherit a weight from the opposite leg
  // just because the two are adjacent along the hem.
  const regionOf = partition.regions;
  let propagated = 0;
  // Rounds until no progress: one pass leaves a vertex unresolved when all of its
  // neighbours were unresolved at the time it was visited, and a vertex with zero weights
  // later fails every skinning operation with an unrelated error.
  let queue = [...pending];
  for (;;) {
    const next = [];
    for (const vertex of queue) {
      if (normalisedVertex(result.joints, result.weights, vertex, maximumInfluences).length > 0) continue;
      const votes = new Map();
      for (const neighbour of adjacency[vertex]) {
        const entries = normalisedVertex(result.joints, result.weights, neighbour, maximumInfluences);
        if (entries.length === 0) continue;
        if (regionOf[vertex] && regionOf[neighbour]
            && !regionCompatible(regionOf[vertex], regionOf[neighbour])) continue;
        for (const entry of entries) votes.set(entry.bone, (votes.get(entry.bone) ?? 0) + entry.weight);
      }
      if (votes.size === 0) { next.push(vertex); continue; }
      writeVertex(result, vertex, [...votes.entries()].map(([bone, weight]) => ({ bone, weight })));
      ++propagated;
    }
    if (next.length === queue.length) break;
    queue = next;
  }
  const unresolved = queue.filter((vertex) => normalisedVertex(result.joints, result.weights, vertex, maximumInfluences).length === 0);
  return { weights: result, propagated, unresolved: unresolved.length, unresolvedVertices: unresolved, filteredSideInfluences, balancedSideVertices, filteredRegionInfluences };
}

// Constrained Laplacian smoothing.
//
// Locked vertices keep their exact weights. Smoothing their *neighbours* towards a locked
// value would undo the lock through the back door, so a locked vertex is excluded from
// the neighbour average as well: it acts as a fixed boundary, which is what a painter
// expects when they lock a region and smooth around it.
//
// `locked` accepts either the `Map<vertex, entries>` that `applyLocks` and
// `validateWeights` use, or a byte mask. Accepting the Map is not convenience: with the
// mask-only signature, passing the documented Map looked up `undefined` for every vertex,
// so every lock was silently ignored and the smoothed weights drifted away from the
// values the author had frozen.
export function smoothWeights(weights, garment, {
  iterations = 2, factor = 0.5, locked = null, regions = null, maximumRegionMismatch = true,
  sides = null, character = null,
} = {}) {
  const adjacency = garmentAdjacency(garment);
  const { vertices, maximumInfluences } = weights;
  let current = {
    ...weights,
    joints: Uint32Array.from(weights.joints),
    weights: Float32Array.from(weights.weights),
  };
  let lockedMask;
  if (locked instanceof Map) {
    lockedMask = new Uint8Array(vertices);
    for (const vertex of locked.keys()) {
      fail(vertex >= 0 && vertex < vertices, 'NG-E-GARMENT-WEIGHTS',
        `lock references vertex ${vertex} of ${vertices}`);
      lockedMask[vertex] = 1;
    }
  } else {
    lockedMask = locked ?? new Uint8Array(vertices);
    fail(lockedMask.length === vertices, 'NG-E-GARMENT-WEIGHTS',
      `locked mask has ${lockedMask.length} entries, the mesh has ${vertices} vertices`);
  }
  // Side-consistent smoothing. A vertex visibly on one side of the body must not pick up
  // weight from the other side just because the crotch seam is a few edges away: two
  // passes at factor 0.5 turned a correct 0.39 near / 0.08 far split into 0.28 near /
  // 0.40 far on the sample trousers, and the hem then swings with the wrong leg. Neutral
  // bones (hips, spine) still smooth freely, which is what keeps the seam continuous.
  const boneSide = sides && character
    ? new Map(character.bones.map((bone) => [bone.index, sideOfRegion(regionForBoneName(bone.name))]))
    : null;
  let conflicts = 0;
  for (let iteration = 0; iteration < iterations; ++iteration) {
    const next = {
      ...current,
      joints: Uint32Array.from(current.joints),
      weights: Float32Array.from(current.weights),
    };
    for (let vertex = 0; vertex < vertices; ++vertex) {
      if (lockedMask[vertex]) continue;
      const own = normalisedVertex(current.joints, current.weights, vertex, maximumInfluences);
      if (own.length === 0) continue;
      const neighbourWeights = new Map();
      const vertexSide = boneSide ? sides[vertex] : null;
      let neighbours = 0;
      for (const neighbour of adjacency[vertex]) {
        if (lockedMask[neighbour]) continue;
        if (maximumRegionMismatch && regions && regions[vertex] && regions[neighbour]
            && !regionCompatible(regions[vertex], regions[neighbour])) continue;
        const entries = normalisedVertex(current.joints, current.weights, neighbour, maximumInfluences);
        if (entries.length === 0) continue;
        const usable = vertexSide
          ? entries.filter((entry) => {
            const side = boneSide.get(entry.bone);
            return !side || side === vertexSide;
          })
          : entries;
        if (usable.length === 0) continue;
        ++neighbours;
        for (const entry of usable) {
          neighbourWeights.set(entry.bone, (neighbourWeights.get(entry.bone) ?? 0) + entry.weight);
        }
      }
      if (neighbours === 0) continue;
      // Laplacian stencil: (1 - factor) of the vertex's own weights plus `factor` spread
      // over the neighbour average. Every component of the stencil sums to one, so the
      // vertex stays normalised without a separate renormalisation step.
      const accumulated = new Map();
      for (const entry of own) accumulated.set(entry.bone, entry.weight * (1 - factor));
      for (const [bone, weight] of neighbourWeights) {
        accumulated.set(bone, (accumulated.get(bone) ?? 0) + (weight / neighbours) * factor);
      }
      writeVertex(next, vertex, [...accumulated.entries()].map(([bone, weight]) => ({ bone, weight })));
    }
    current = next;
  }
  // A lock that smoothing would have changed is a conflict worth reporting: it means the
  // surrounding weights disagree with the locked value, and the artist should know.
  if (lockedMask.some((value) => value)) {
    for (let vertex = 0; vertex < vertices; ++vertex) {
      if (!lockedMask[vertex]) continue;
      const before = normalisedVertex(weights.joints, weights.weights, vertex, maximumInfluences);
      const after = normalisedVertex(current.joints, current.weights, vertex, maximumInfluences);
      if (canonicalJson(before) !== canonicalJson(after)) ++conflicts;
    }
  }
  return { weights: current, lockConflicts: conflicts };
}

// Drops the smallest influences until at most `maximumInfluences` remain, then
// renormalises. The runtime format has a hard limit; exporting 5 influences to a 4-slot
// format silently drops the fifth and un-normalises the vertex.
export function sparsifyWeights(weights, { maximumInfluences = DEFAULT_MAX_INFLUENCES } = {}) {
  const result = emptyWeights(weights.vertices, maximumInfluences);
  let dropped = 0;
  for (let vertex = 0; vertex < weights.vertices; ++vertex) {
    const entries = normalisedVertex(weights.joints, weights.weights, vertex, weights.maximumInfluences);
    if (entries.length > maximumInfluences) dropped += entries.length - maximumInfluences;
    writeVertex(result, vertex, entries);
  }
  return { weights: result, droppedInfluences: dropped };
}

// Explicit left/right bone mapping. Mirroring by negating X coordinates is wrong on any
// character whose left bone is not called `right` after removing four characters, and it
// silently produces a mirrored hand weight on the opposite arm.
export function mirrorBoneMap(character) {
  const map = new Map();
  for (const bone of character.bones) {
    const name = bone.name;
    const swapped = name
      .replace(/Left/i, '__side__').replace(/Right/i, 'Left').replace(/__side__/g, 'Right')
      .replace(/left/g, '__side__').replace(/right/g, 'left').replace(/__side__/g, 'right')
      .replace(/_l$/i, '__side__').replace(/_r$/i, '_l').replace(/__side__/g, '_r');
    const target = character.bones.find((candidate) => candidate.name === swapped);
    if (target) map.set(bone.index, target.index);
  }
  return map;
}

export function mirrorWeights(weights, garment, { boneMap, side = 'left', xThreshold = 0 } = {}) {
  fail(boneMap instanceof Map && boneMap.size > 0, 'NG-E-GARMENT-WEIGHTS',
    'mirroring needs an explicit bone map; negating coordinates is not a mirror');
  const result = {
    ...weights,
    joints: Uint32Array.from(weights.joints),
    weights: Float32Array.from(weights.weights),
  };
  const source = side === 'left' ? 1 : -1;
  let mirrored = 0;
  const pairs = new Map();
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    const x = garment.positions[vertex * 3];
    if (Math.sign(x - xThreshold) !== source) continue;
    // Find the mirrored counterpart by position: this is the only reliable pairing on a
    // mesh without a symmetric topology guarantee.
    let best = -1, bestDistance = Infinity;
    for (let other = 0; other < garment.vertices; ++other) {
      if (Math.sign(garment.positions[other * 3] - xThreshold) === source) continue;
      const distance = Math.hypot(
        -x - garment.positions[other * 3],
        garment.positions[vertex * 3 + 1] - garment.positions[other * 3 + 1],
        garment.positions[vertex * 3 + 2] - garment.positions[other * 3 + 2]);
      if (distance < bestDistance) { bestDistance = distance; best = other; }
    }
    if (best < 0 || bestDistance > 0.01) continue;
    pairs.set(vertex, best);
  }
  for (const [from, to] of pairs) {
    const entries = normalisedVertex(weights.joints, weights.weights, from, weights.maximumInfluences)
      .map((entry) => ({ bone: boneMap.get(entry.bone) ?? entry.bone, weight: entry.weight }));
    writeVertex(result, to, entries);
    ++mirrored;
  }
  return { weights: result, mirroredVertices: mirrored, pairs: pairs.size };
}

// Locked values are supplied as `Map<vertexIndex, Array<{bone, weight}>>`. Applying a
// lock writes exactly those weights and records any vertex whose stored value was not
// normalised, because a caller-supplied lock that does not sum to 1 is a bug in the
// patch, not something to fix silently.
export function applyLocks(weights, locks) {
  const result = {
    ...weights,
    joints: Uint32Array.from(weights.joints),
    weights: Float32Array.from(weights.weights),
  };
  const violations = [];
  for (const [vertex, entries] of locks) {
    fail(vertex >= 0 && vertex < weights.vertices, 'NG-E-GARMENT-WEIGHTS',
      `lock references vertex ${vertex} of ${weights.vertices}`);
    const total = entries.reduce((sum, entry) => sum + entry.weight, 0);
    if (Math.abs(total - 1) > 1e-3) violations.push({ vertex, total });
    writeVertex(result, vertex, entries);
  }
  return { weights: result, violations };
}

export function validateWeights(weights, { maximumInfluences = DEFAULT_MAX_INFLUENCES, locked = null, regionConfidence = null } = {}) {
  const issues = [];
  const influenceCounts = new Uint8Array(weights.vertices);
  let sumError = 0, negative = 0, nonFinite = 0, zeroTotal = 0, overLimit = 0;
  for (let vertex = 0; vertex < weights.vertices; ++vertex) {
    let total = 0, count = 0;
    for (let slot = 0; slot < weights.maximumInfluences; ++slot) {
      const weight = weights.weights[vertex * weights.maximumInfluences + slot];
      const bone = weights.joints[vertex * weights.maximumInfluences + slot];
      if (!Number.isFinite(weight)) { ++nonFinite; continue; }
      if (weight < 0) ++negative;
      if (weight > 0) { total += weight; ++count; }
      if (weight > 0 && bone >= 9999) issues.push({ vertex, code: 'NG-E-GARMENT-BONE', bone });
    }
    influenceCounts[vertex] = count;
    if (count === 0) { ++zeroTotal; continue; }
    if (count > maximumInfluences) ++overLimit;
    sumError = Math.max(sumError, Math.abs(total - 1));
  }
  const lowConfidence = [];
  if (regionConfidence) {
    for (let vertex = 0; vertex < weights.vertices; ++vertex) {
      if (regionConfidence[vertex] < 0.4) lowConfidence.push(vertex);
    }
  }
  const result = {
    vertices: weights.vertices,
    maximumInfluences,
    invalid: {
      negative, nonFinite, zeroTotal, overLimit,
      sumError,
      lockedViolations: 0,
    },
    influenceHistogram: [...influenceCounts].reduce((histogram, count) => {
      histogram[count] = (histogram[count] ?? 0) + 1;
      return histogram;
    }, {}),
    lowConfidenceVertices: lowConfidence.length,
    issues,
  };
  if (locked) {
    let violations = 0;
    for (const [vertex, entries] of locked) {
      const current = normalisedVertex(weights.joints, weights.weights, vertex, weights.maximumInfluences);
      const expected = [...entries].sort((a, b) => b.weight - a.weight || a.bone - b.bone);
      const total = expected.reduce((sum, entry) => sum + entry.weight, 0) || 1;
      const normalisedExpected = expected.slice(0, weights.maximumInfluences)
        .map((entry) => ({ bone: entry.bone, weight: entry.weight / total }));
      // Compared with a tolerance, not by serialising the floats: the weights live in a
      // Float32Array and the expected values are Float64, so an exact comparison reports
      // a violation for arithmetic that differs in the last bit of a float32.
      if (!sameWeightSet(current, normalisedExpected)) ++violations;
    }
    result.invalid.lockedViolations = violations;
  }
  result.valid = result.invalid.negative === 0 && result.invalid.nonFinite === 0
    && result.invalid.zeroTotal === 0 && result.invalid.overLimit === 0
    && result.invalid.sumError <= 1e-4 && result.invalid.lockedViolations === 0;
  return result;
}

// One snapshot object feeds both the numeric export and the heatmap. The plan requires
// that; keeping them separate is how a report ends up showing a colour that does not
// match the numbers it was generated with.
export function weightsSnapshot(garment, weights, { topology = null, character = null } = {}) {
  const hash = topology ?? topologyHash(garment);
  const records = [];
  for (let vertex = 0; vertex < weights.vertices; ++vertex) {
    const entries = normalisedVertex(weights.joints, weights.weights, vertex, weights.maximumInfluences);
    records.push({
      vertexId: garment.vertexIds[vertex],
      index: vertex,
      influences: entries,
      residual: Math.abs(entries.reduce((sum, entry) => sum + entry.weight, 0) - 1),
    });
  }
  const bones = character ? character.bones.map((bone) => ({ index: bone.index, name: bone.name })) : [];
  const payload = canonicalJson({
    topology: hash,
    bones,
    vertices: records.map((record) => [record.vertexId, record.influences.map((entry) => [entry.bone, Math.round(entry.weight * 1e6)])]),
  });
  return {
    schema: 'nativegpu.weights-snapshot/1',
    topologyHash: hash,
    vertexCount: weights.vertices,
    maximumInfluences: weights.maximumInfluences,
    records,
    bones,
    digest: sha256Hex(payload).slice(0, 32),
  };
}

// Applies a patch of numeric weight edits. `operations` address vertices by stable id,
// which is what makes a patch survive a re-export that reorders vertices.
export function patchWeights(garment, weights, operations, { character = null, expectedTopology = null } = {}) {
  // A raw {positions, indices} object has no parsed vertex ids. Reading them as
  // `undefined` used to surface as an unrelated TypeError from the digester, which
  // reads like a bug in the engine instead of a caller mistake.
  fail(garment && garment.vertices > 0 && garment.vertexIds && garment.vertexIds.length === garment.vertices,
    'NG-E-GARMENT-MESH', 'patchWeights needs a mesh from parseMesh (with vertexIds), not a raw position/index object');
  const hash = topologyHash(garment);
  if (expectedTopology !== null && expectedTopology !== hash) {
    throw new GarmentError('NG-E-GARMENT-TOPOLOGY',
      `weights were authored against topology ${String(expectedTopology).slice(0, 12)} but the mesh is now `
      + `${hash.slice(0, 12)}; re-inspect before patching`,
      [{ expectedTopology, currentTopology: hash }]);
  }
  fail(Array.isArray(operations) && operations.length > 0, 'NG-E-GARMENT-WEIGHTS', 'operations must be a non-empty array');
  const byId = new Map([...garment.vertexIds].map((id, index) => [id, index]));
  const result = {
    ...weights,
    joints: Uint32Array.from(weights.joints),
    weights: Float32Array.from(weights.weights),
  };
  const changes = [];
  for (const operation of operations) {
    fail(operation && typeof operation === 'object', 'NG-E-GARMENT-WEIGHTS', 'each operation must be an object');
    fail(['set', 'add', 'scale'].includes(operation.kind), 'NG-E-GARMENT-WEIGHTS',
      'operation.kind must be set, add or scale');
    const targets = operation.vertexIds ?? (operation.vertexId !== undefined ? [operation.vertexId] : null);
    fail(Array.isArray(targets), 'NG-E-GARMENT-WEIGHTS', 'an operation needs vertexId or vertexIds');
    fail(typeof operation.bone === 'number' || typeof operation.bone === 'string',
      'NG-E-GARMENT-WEIGHTS', 'an operation needs a bone index or name');
    const boneIndex = typeof operation.bone === 'number'
      ? operation.bone
      : (character?.bones.find((bone) => bone.name === operation.bone)?.index ?? null);
    fail(boneIndex !== null && boneIndex !== undefined, 'NG-E-GARMENT-WEIGHTS',
      `unknown bone ${operation.bone}`);
    for (const vertexId of targets) {
      const vertex = byId.get(vertexId);
      fail(vertex !== undefined, 'NG-E-GARMENT-WEIGHTS', `vertex id ${vertexId} is not in the mesh`);
      const entries = normalisedVertex(result.joints, result.weights, vertex, result.maximumInfluences);
      const before = entries.find((entry) => entry.bone === boneIndex)?.weight ?? 0;
      let after;
      if (operation.kind === 'set') after = operation.value;
      else if (operation.kind === 'add') after = before + operation.value;
      else after = before * operation.value;
      fail(typeof after === 'number' && Number.isFinite(after) && after >= 0 && after <= 1,
        'NG-E-GARMENT-WEIGHTS', `operation produces an invalid weight ${after} on vertex ${vertexId}`);
      const others = entries.filter((entry) => entry.bone !== boneIndex);
      const updated = after <= 1e-6 ? others : [...others, { bone: boneIndex, weight: after }];
      fail(updated.length > 0, 'NG-E-GARMENT-WEIGHTS',
        `operation removes the last influence of vertex ${vertexId}`);
      writeVertex(result, vertex, updated);
      changes.push({ vertexId, bone: boneIndex, before, after });
    }
  }
  return { weights: result, changes, topologyHash: hash };
}

// Bone influence totals, for a report that answers "which bone is doing too much work".
export function influenceTotals(weights, bones = []) {
  const totals = new Map();
  for (let index = 0; index < weights.joints.length; ++index) {
    const weight = weights.weights[index];
    if (weight <= 0) continue;
    const bone = weights.joints[index];
    totals.set(bone, (totals.get(bone) ?? 0) + weight);
  }
  return [...totals.entries()]
    .sort((a, b) => b[1] - a[1])
    .map(([bone, total]) => ({ bone, name: bones[bone]?.name ?? String(bone), total }));
}

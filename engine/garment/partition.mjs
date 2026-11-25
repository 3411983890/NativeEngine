import { TriangleGrid } from './grid.mjs';
import { GarmentError } from './inspect.mjs';
import { boneKey, identifier } from '../assets/bone-semantics.mjs';

// Semantic partitioning of a body and a garment.
//
// The failure this module exists to prevent: a skirt vertex near the crotch picking the
// nearest body triangle, which belongs to the *other* leg, and then pulling half the
// skirt sideways whenever the character walks. Distance alone cannot solve that — the
// nearest point to the inside of a left thigh is often the right thigh. So the mapping
// is constrained by region identity, side, and surface orientation, and whatever cannot
// be decided confidently is reported as low confidence instead of guessed.

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

// Bone key → body region. `boneKey` normalises Mixamo/UE/Blender names, so this table
// works for a rig that calls the upper arm `LeftArm`, `upperarm_l` or `mixamorig:LeftArm`.
const BONE_REGIONS = new Map([
  ['hips', 'hips'],
  ['spine', 'torso'], ['chest', 'torso'], ['neck', 'neck'], ['head', 'head'], ['headend', 'head'],
  ['leftshoulder', 'left_shoulder'], ['rightshoulder', 'right_shoulder'],
  ['leftarm', 'left_upper_arm'], ['rightarm', 'right_upper_arm'],
  ['leftforearm', 'left_forearm'], ['rightforearm', 'right_forearm'],
  ['lefthand', 'left_hand'], ['righthand', 'right_hand'],
  ['leftthigh', 'left_thigh'], ['rightthigh', 'right_thigh'],
  ['leftcalf', 'left_calf'], ['rightcalf', 'right_calf'],
  ['leftfoot', 'left_foot'], ['rightfoot', 'right_foot'],
  ['lefttoe', 'left_foot'], ['righttoe', 'right_foot'],
]);

// Which regions may transfer weight to which. A garment vertex classified as part of the
// left sleeve may take weight from the left arm and the torso, never from the right arm.
const REGION_NEIGHBOURS = new Map([
  ['torso', ['torso', 'hips', 'neck', 'left_shoulder', 'right_shoulder', 'left_upper_arm', 'right_upper_arm']],
  ['hips', ['hips', 'torso', 'left_thigh', 'right_thigh']],
  ['neck', ['neck', 'torso', 'head']],
  ['head', ['head', 'neck']],
  ['left_shoulder', ['left_shoulder', 'torso', 'left_upper_arm']],
  ['right_shoulder', ['right_shoulder', 'torso', 'right_upper_arm']],
  ['left_upper_arm', ['left_upper_arm', 'left_shoulder', 'left_forearm', 'torso']],
  ['right_upper_arm', ['right_upper_arm', 'right_shoulder', 'right_forearm', 'torso']],
  ['left_forearm', ['left_forearm', 'left_upper_arm', 'left_hand']],
  ['right_forearm', ['right_forearm', 'right_upper_arm', 'right_hand']],
  ['left_hand', ['left_hand', 'left_forearm']],
  ['right_hand', ['right_hand', 'right_forearm']],
  ['left_thigh', ['left_thigh', 'hips', 'left_calf']],
  ['right_thigh', ['right_thigh', 'hips', 'right_calf']],
  ['left_calf', ['left_calf', 'left_thigh', 'left_foot']],
  ['right_calf', ['right_calf', 'right_thigh', 'right_foot']],
  ['left_foot', ['left_foot', 'left_calf']],
  ['right_foot', ['right_foot', 'right_calf']],
]);

export const SIDE_REGIONS = Object.freeze({
  left: new Set(['left_shoulder', 'left_upper_arm', 'left_forearm', 'left_hand', 'left_thigh', 'left_calf', 'left_foot']),
  right: new Set(['right_shoulder', 'right_upper_arm', 'right_forearm', 'right_hand', 'right_thigh', 'right_calf', 'right_foot']),
});

export function regionForBoneName(name) {
  const key = boneKey(name);
  if (BONE_REGIONS.has(key)) return BONE_REGIONS.get(key);
  // Numbered chains: Spine1, Chest_02, UpperArm_L_01. Without this, a three-segment spine
  // reports everything above the first spine joint as `unknown`, and `unknown` is
  // compatible with every region — so the region filter goes permissive exactly on the
  // chest, which is where a sleeve is most likely to be given to the wrong limb.
  const base = key.replace(/[0-9]+$/, '');
  if (base && base !== key && BONE_REGIONS.has(base)) return BONE_REGIONS.get(base);
  // Finger bones and any other leaf under a hand belong to that hand.
  const id = identifier(name);
  if (/^left(hand|thumb|index|middle|pinky|ring)/.test(id)) return 'left_hand';
  if (/^right(hand|thumb|index|middle|pinky|ring)/.test(id)) return 'right_hand';
  return 'unknown';
}

export function sideOfRegion(region) {
  for (const [side, regions] of Object.entries(SIDE_REGIONS)) {
    if (regions.has(region)) return side;
  }
  return null;
}

export function regionCompatible(a, b) {
  if (a === 'unknown' || b === 'unknown') return true;
  if (a === b) return true;
  return (REGION_NEIGHBOURS.get(a) ?? []).includes(b);
}

// Body partition: every vertex gets the region of its dominant bone. This is reliable
// because the body already has correct weights — it is the garment that does not.
export function partitionBody(character) {
  const regions = new Array(character.vertices);
  const regionVertices = new Map();
  const boneRegions = character.bones.map((bone) => regionForBoneName(bone.name));
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    let best = -1, bestWeight = 0;
    for (let influence = 0; influence < 4; ++influence) {
      const weight = character.weights[vertex * 4 + influence];
      if (weight > bestWeight) { bestWeight = weight; best = character.joints[vertex * 4 + influence]; }
    }
    const region = best >= 0 ? boneRegions[best] : 'unknown';
    regions[vertex] = region;
    const list = regionVertices.get(region) ?? [];
    list.push(vertex);
    regionVertices.set(region, list);
  }
  const counts = Object.fromEntries([...regionVertices.entries()].map(([region, list]) => [region, list.length]));
  return {
    regions, regionVertices, counts,
    boneRegions,
    summary: Object.entries(counts).sort((a, b) => b[1] - a[1]).map(([region, count]) => ({ region, vertices: count })),
  };
}

// Garment vertex → body region.
//
// Three signals, in order of authority:
//  1. the garment's own geometry relative to the body's centre line decides the side,
//     which is the only reliable signal for a loose skirt or a cape,
//  2. the nearest compatible body triangle decides the region,
//  3. if no body triangle is close enough, the vertex is *unresolved*: its weights are
//     propagated from resolved neighbours along the garment's own connectivity rather
//     than snapped to the nearest bone.
export function partitionGarment(garment, body, bodyPartition, {
  bodyGrid = null, maxDistanceMetres = 0.06, sideBiasMetres = 0.02, allowedRegions = null,
} = {}) {
  // `allowedRegions` is how an author says "this is a cape, it does not follow the upper
  // arm". Without it, the nearest-triangle rule attaches a cape's shoulder edge to the
  // arm, and the sheet tears the moment the arm lifts — the exact failure the plan
  // describes for loose garments.
  const allowed = allowedRegions ? new Set(allowedRegions) : null;
  const grid = bodyGrid ?? new TriangleGrid(body.positions, body.indices);
  const regions = new Array(garment.vertices).fill(null);
  // The geometric side of every vertex, independent of the region it ends up with. A
  // crotch vertex is `hips`, which has no side, but it is still visibly on one side of
  // the centre line — and that is the signal the weight transfer needs to stop the far
  // leg from dominating it.
  const sides = new Array(garment.vertices).fill(null);
  const confidence = new Float32Array(garment.vertices);
  const distances = new Float32Array(garment.vertices).fill(Infinity);
  const triangles = new Int32Array(garment.vertices).fill(-1);
  const barycentric = new Float32Array(garment.vertices * 3);
  const lowConfidence = [];

  // Body centre line, from the hips: the split between left and right.
  const hipsVertices = bodyPartition.regionVertices.get('hips') ?? [];
  let centreX = 0;
  for (const vertex of hipsVertices) centreX += body.positions[vertex * 3];
  centreX = hipsVertices.length ? centreX / hipsVertices.length : 0;

  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    const point = [garment.positions[vertex * 3], garment.positions[vertex * 3 + 1], garment.positions[vertex * 3 + 2]];
    const garmentSide = point[0] > centreX + sideBiasMetres ? 'left'
      : point[0] < centreX - sideBiasMetres ? 'right' : null;
    sides[vertex] = garmentSide;

    // Collect several candidates: the single nearest triangle is exactly the failure
    // mode this module exists to avoid, so the choice is made with a region filter.
    let best = null;
    // The radius escalates geometrically: a loose cape sits 8–20 cm off the shoulders, so
    // a fixed 6 cm search finds nothing and the whole sheet ends up unresolved. Searching
    // further is not the same as believing the result — confidence still decays with
    // distance, and anything beyond the confident radius is reported as propagated.
    // Three steps (6/12/24 cm by default) cover that case. Escalating to 1.9 m would let a
    // hem a metre from the body be "measured" against whatever limb happened to be there;
    // past 24 cm propagation from the garment's own connectivity is the correct answer.
    for (let attempt = 0; attempt < 3 && !best; ++attempt) {
      const radius = maxDistanceMetres * Math.pow(2, attempt);
      const candidates = [];
      const nearest = grid.nearest(point, { maxDistanceMetres: radius });
      // `continue`, not `break`: an empty query at 6 cm says nothing about 12 cm, and
      // breaking here made the escalation dead code — a vertex just outside the first
      // radius was declared unresolved and inherited its neighbour's weights instead of
      // being measured, which is how a waist vertex ended up 17% weighted to an arm.
      if (nearest.triangle < 0) continue;
      candidates.push(nearest);
      // Probe the candidate's neighbours by re-querying with a filter that excludes the
      // already-rejected triangle; this keeps the query cheap while still considering
      // alternatives on the correct side of a limb.
      const rejected = new Set([nearest.triangle]);
      for (let probe = 0; probe < 6; ++probe) {
        const alternative = grid.nearest(point, {
          maxDistanceMetres: radius,
          filter: ({ triangle }) => rejected.has(triangle),
        });
        if (alternative.triangle < 0) break;
        rejected.add(alternative.triangle);
        candidates.push(alternative);
      }
      const scored = candidates.map((candidate) => {
        const region = dominantRegionOfTriangle(bodyPartition, candidate.a, candidate.b, candidate.c);
        const candidateSide = sideOfRegion(region);
        const sideOk = !garmentSide || !candidateSide || candidateSide === garmentSide;
        const allowedOk = !allowed || allowed.has(region);
        return { candidate, region, sideOk, allowedOk, distance: candidate.distance };
      }).filter((entry) => entry.sideOk && entry.allowedOk);
      scored.sort((a, b) => a.distance - b.distance);
      best = scored[0] ?? null;
    }

    if (!best) {
      lowConfidence.push(vertex);
      continue;
    }
    regions[vertex] = best.region;
    distances[vertex] = best.candidate.distance;
    triangles[vertex] = best.candidate.triangle;
    barycentric[vertex * 3] = best.candidate.bary[0];
    barycentric[vertex * 3 + 1] = best.candidate.bary[1];
    barycentric[vertex * 3 + 2] = best.candidate.bary[2];
    // Confidence falls with distance and, for a *sided* region, with how close the
    // vertex is to the body's centre line: both make the assignment less certain.
    // A central region (torso, hips) has no side to get wrong, so proximity to the
    // centre line must not lower its confidence — doing that flagged half of a fitted
    // shirt as doubtful and made the low-confidence report useless.
    const distanceScore = Math.max(0, 1 - best.candidate.distance / maxDistanceMetres);
    const sideScore = !sideOfRegion(best.region) || garmentSide ? 1 : 0.6;
    confidence[vertex] = Math.min(1, distanceScore * sideScore);
  }

  // Unresolved vertices: propagate from resolved neighbours through the garment's own
  // connectivity. A skirt hem far from the legs inherits the region of the waist it
  // hangs from, which is the correct answer and cannot be obtained from proximity.
  const adjacency = garmentAdjacency(garment);
  let propagated = 0;
  // Rounds, not a single pass: a vertex whose neighbours are all unresolved in the first
  // round is skipped forever by a one-pass queue, which is how a single isolated vertex
  // ends up with no region at all.
  let pending = [...lowConfidence];
  for (;;) {
    const next = [];
    for (const vertex of pending) {
      if (regions[vertex]) continue;
      let bestRegion = null, bestScore = 0;
      for (const [candidate, score] of votesFor(adjacency, regions, confidence, vertex)) {
        if (score > bestScore) { bestScore = score; bestRegion = candidate; }
      }
      if (!bestRegion) { next.push(vertex); continue; }
      regions[vertex] = bestRegion;
      // Propagated regions are lower confidence than measured ones, and they are counted
      // separately so a report can say "37% of this cape's vertices were propagated".
      confidence[vertex] = 0.35;
      distances[vertex] = maxDistanceMetres;
      ++propagated;
    }
    if (next.length === pending.length) break;
    pending = next;
  }

  const remaining = regions.filter((region) => !region).length;
  const summary = new Map();
  for (const region of regions) {
    if (!region) continue;
    summary.set(region, (summary.get(region) ?? 0) + 1);
  }
  return {
    regions, sides, confidence, distances, triangles, barycentric,
    propagated,
    unresolved: remaining,
    summary: [...summary.entries()].sort((a, b) => b[1] - a[1]).map(([region, count]) => ({ region, count })),
    centreX,
  };
}

function votesFor(adjacency, regions, confidence, vertex) {
  const votes = new Map();
  for (const neighbour of adjacency[vertex]) {
    if (!regions[neighbour]) continue;
    votes.set(regions[neighbour], (votes.get(regions[neighbour]) ?? 0) + confidence[neighbour] + 1e-3);
  }
  return [...votes.entries()].sort((a, b) => b[1] - a[1]);
}

function dominantRegionOfTriangle(bodyPartition, a, b, c) {
  const counts = new Map();
  for (const vertex of [a, b, c]) {
    const region = bodyPartition.regions[vertex];
    counts.set(region, (counts.get(region) ?? 0) + 1);
  }
  return [...counts.entries()].sort((left, right) => right[1] - left[1])[0][0];
}

export function garmentAdjacency(mesh) {
  const adjacency = Array.from({ length: mesh.vertices }, () => new Set());
  for (let triangle = 0; triangle < mesh.triangles; ++triangle) {
    const a = mesh.indices[triangle * 3], b = mesh.indices[triangle * 3 + 1], c = mesh.indices[triangle * 3 + 2];
    adjacency[a].add(b); adjacency[a].add(c);
    adjacency[b].add(a); adjacency[b].add(c);
    adjacency[c].add(a); adjacency[c].add(b);
  }
  return adjacency.map((set) => [...set]);
}

// Explicit anchors for hanging parts: vertices close enough to the body to be pinned.
// A cape or a long skirt is attached at its top edge, and proximity never finds that
// edge — it has to be named, which is why this returns candidates for a human or an AI
// to confirm rather than silently pinning everything nearby.
export function suggestAnchorsForPartition(garment, partition, { maxDistanceMetres = 0.03 } = {}) {
  const anchors = [];
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    if (partition.distances[vertex] > maxDistanceMetres) continue;
    anchors.push({
      vertexId: garment.vertexIds[vertex],
      vertex,
      region: partition.regions[vertex],
      distanceMetres: partition.distances[vertex],
    });
  }
  return anchors;
}

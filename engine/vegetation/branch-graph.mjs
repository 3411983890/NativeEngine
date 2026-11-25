// Explicit branch structure.
//
// The previous generator grew a tree by recursion over "depth", and three things were wrong
// with that:
//
//   1. A child branch was pushed *away* from its parent by `mul(childDir, length * 0.08)`,
//      which opens a real air gap in the skeleton (G01).
//   2. The child direction was built from a world-horizontal azimuth and world up, not from
//      the parent's own frame, so the authored "spread" angle was not the angle between the
//      child and its parent, and every level drifted back toward world vertical (G02).
//   3. `parent` was stored but never used; the tree was really a level table, so "attach this
//      frond to level 3" had no structural meaning (G05).
//
// This module produces a BranchGraph instead: a directed tree of branches with stable ids,
// an explicit attachment (arc fraction + azimuth on the parent), a transported frame along
// every centreline, an arc-length table and a radius profile. Mesh building, leaf placement,
// LOD and the editor all read this one structure, so "the branch is attached" is a property
// of the data rather than of a particular mesher.
//
// Everything here is deterministic: the RNG stream for a branch is seeded from its stable id,
// so editing one branch does not reshuffle its siblings.

import { Stream, deriveSeed } from './random.mjs';
import { branchOrderAt, envelopeOf, resolveProfile, shapeCrownRadius } from './species-profile.mjs';

export const BRANCH_GRAPH_SCHEMA = 'nativegpu.vegetation-branch-graph/1';

const EPS = 1e-9;

// ---------------------------------------------------------------- vector helpers
export const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
export const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
export const length3 = (a) => Math.hypot(a[0], a[1], a[2]);
export function normalize(a) {
  const l = length3(a);
  return l < EPS ? [0, 1, 0] : [a[0] / l, a[1] / l, a[2] / l];
}
export const lerp3 = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];

// Any unit vector perpendicular to `d`. The choice is stable for a given `d`, which matters:
// a frame that flips its seed axis between two adjacent branches twists the bark texture.
export function perpendicular(d) {
  const axis = Math.abs(d[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  return normalize(cross(d, axis));
}

// Rodrigues rotation of `v` around unit axis `k` by `angle`.
function rotateAround(v, k, angle) {
  const c = Math.cos(angle), s = Math.sin(angle);
  return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1 - c)));
}

// Rotation-minimizing transport of a normal from `from` to `to`.
//
// A naive "perpendicular(newTangent)" per step re-seeds the frame and makes the bark UV
// rotate around the trunk between segments. Transporting the previous normal by the minimal
// rotation that carries the old tangent onto the new one keeps the texture continuous and is
// what the seam duplication in the mesher assumes.
export function transportNormal(from, to, normal) {
  const axis = cross(from, to);
  const s = length3(axis);
  const c = Math.max(-1, Math.min(1, dot(from, to)));
  if (s < 1e-8) {
    if (c > 0) return normal;
    // 180 degree reversal: any perpendicular axis is valid, but it must be chosen
    // deterministically or the ring order flips.
    return mul(normal, -1);
  }
  return normalize(rotateAround(normal, mul(axis, 1 / s), Math.atan2(s, c)));
}

// ------------------------------------------------------------------- growth
function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-GRAPH';
  throw error;
}

// Rotates `from` toward `to`, but never by more than `maxAngle` radians. A branch has a bounded
// curvature per metre of length; without that bound the steering below can rotate the direction by
// most of the angle to its target in a single step, which is not a bend but a corner. Slerp keeps
// the path on the sphere between the two directions so the intermediate frames stay orthonormal.
function turnToward(from, to, maxAngle) {
  const cosine = Math.max(-1, Math.min(1, dot(from, to)));
  const angle = Math.acos(cosine);
  if (angle <= maxAngle || angle < 1e-6) return to;
  const t = maxAngle / angle;
  const sinAngle = Math.sin(angle);
  const w0 = Math.sin((1 - t) * angle) / sinAngle;
  const w1 = Math.sin(t * angle) / sinAngle;
  const mixed = add(mul(from, w0), mul(to, w1));
  const length = Math.hypot(mixed[0], mixed[1], mixed[2]);
  return length < 1e-9 ? to : mul(mixed, 1 / length);
}


// The growth shell. Its *shape* is part of the species identity, so it comes from the resolved
// profile (`species-profile.mjs`) instead of a branch on the species name here, and its scale is
// derived from height — never from the authored crown radius. The shell only steers growth (see
// growBranch); it is neither a projection target nor a declaration enforcer.
function makeEnvelope(height, profile) {
  return envelopeOf(profile, height);
}

function growBranch(context, branch) {
  const { levels, envelope, height, seed, branches, byId } = context;
  const definition = levels.byLevel.get(branch.level);
  const rng = new Stream(deriveSeed(seed, `branch/${branch.id}`), 'branch');
  const steps = Math.max(1, definition.steps);
  const stepLength = branch.length / steps;
  const taper = definition.segmentTaper;
  // Real arc length accumulated from the points as they are placed. `branch.length` is the
  // authored *target*; steering and the envelope clamp move the point, so a curved branch is
  // measurably longer than its target. Bark V, leaf density and every attachment sample are
  // metres on the actual curve, so they must read this and not `stepLength * index`.
  let arc = 0;
  // Steering momentum: how strongly this branch is currently giving up radial growth. Filtered
  // rather than read straight off the probe, so the shell produces a curve instead of a zigzag.
  let inwardBias = 0;
  // How many further steps the branch keeps travelling along the shell after it first touches it.
  // Without this the cap flips on and off every step 鈥?one step clipped, the next full outward 鈥?
  // and that alternation *is* the zigzag the spike counter reports.
  let shellHold = 0;

  let position = branch.start;
  let direction = branch.startDirection;
  let normal = branch.startNormal;
  const points = [{
    position, tangent: direction, normal,
    radius: branch.rootRadius,
    arcLength: 0,
    wind: branch.windRoot,
  }];

  for (let step = 0; step < steps; ++step) {
    const t = (step + 1) / steps;
    // Wobble in the branch's own frame, not in world axes: a branch that is already leaning
    // must wobble around its own axis or the trunk visibly kinks.
    const tangent = direction;
    const binormal = normalize(cross(tangent, normal));
    const lean = definition.lean;
    direction = normalize(add(direction, add(add(
      mul(normal, rng.span(-lean, lean)),
      mul(binormal, rng.span(-lean, lean))),
      mul(tangent, rng.span(-lean * 0.35, lean * 0.35)))));

    // Gravitropism and phototropism, both small and both applied in world space: a tip
    // droops, a young shoot reaches up. Without the second term a deep branch levels out
    // and the crown flattens into a disc.
    direction = normalize(add(direction, add(
      [0, definition.upBias, 0],
      [0, -definition.droop * t, 0])));

    // Soft spatial constraint. The old code projected the predicted point back onto the shell
    // whenever it left it, which (a) bent the curve at a hard kink and (b) arranged every tip on
    // a visible surface 鈥?the "algorithm sketch tree" look. Now the shell only *turns and slows*
    // the growth: the further outside, the harder the turn and the shorter the step, so a tip
    // approaches the bound asymptotically instead of being teleported onto it.
    const probe = add(position, mul(direction, stepLength));
    const allowed = envelope(position);
    const radial = Math.hypot(probe[0], probe[2]);
    // The crown shell owns the branches, not the trunk. It used to be applied to every level, so
    // changing a crown parameter also changed how the trunk grew — and with it where every limb
    // attached. That is why every parameter experiment in R1-13..R1-17 moved the crown in a
    // direction nobody predicted: the knob was moving the trunk as well. The trunk keeps the height
    // limit (it must not grow out of the tree) but is no longer steered sideways by the crown.
    // Only a *single-stem* tree's trunk is exempt: for a grass clump or a multi-stem shrub the
    // level-0 branches are the plant's limbs, and the shell is what gives the clump its width.
    const constrained = branch.level > 0 || (context.stems ?? 1) > 1;
    let excess = 0;
    if (constrained && radial > allowed && radial > EPS) excess = (radial - allowed) / Math.max(0.05, allowed);
    // The steering pressure is low-pass filtered. Steering straight off the instantaneous probe
    // made the branch flip inward the moment it crossed the shell and outward again on the next
    // step, which is a zigzag, not a shape: a short blade ended up with 17 sharp turns. A branch
    // that has started giving up radial growth keeps giving it up for a few steps, then relaxes.
    const target = excess > 0 ? Math.min(1, Math.pow(Math.min(1, excess), 0.7)) : 0;
    inwardBias = inwardBias * 0.55 + target * 0.45;
    // The turn is a rotation applied per step, so the same angle means a much sharper bend on a
    // short twig than on a long limb. Scaling by the physical step length (relative to a 0.3 m
    // reference) is what stops the outermost level from being where every kink lives: a 0.7 m twig
    // gets a quarter of the turn a 0.8 m trunk step gets, and pays for it with step suppression
    // instead, which bends nothing.
    const turnScale = Math.min(1, Math.max(0.2, stepLength / 0.3));
    const turn = 0.9 * inwardBias * turnScale;
    // Bounded curvature: at most ~0.45 rad per metre travelled, at least 0.06 rad per step so a very
    // short step can still converge. Hoisted out of the steering branch because the shell cap below
    // needs the same bound 鈥?unbounded rotation there is what kept the twig spikes alive.
    const maxTurn = Math.min(0.35, Math.max(0.06, stepLength * 0.45 * turnScale));
    if (turn > 1e-4) {
      const radialUnit = [probe[0] / Math.max(EPS, radial), 0, probe[2] / Math.max(EPS, radial)];
      const inward = [-radialUnit[0], 0, -radialUnit[2]];
      // Aim *along* the shell, not at the axis. Steering at the axis made the target direction
      // change sign whenever the branch drifted across it, and a rotating target is a rotating
      // turn 鈥?that is the oscillation the spike counter was reporting. The tangential direction
      // around the trunk varies smoothly from step to step, so the turn converges instead of
      // alternating. A small inward bias keeps the branch from leaving the shell, and the vertical
      // term lets a crown still gain height while it travels.
      let around = [-radialUnit[2], 0, radialUnit[0]];
      if (direction[0] * around[0] + direction[2] * around[2] < 0) around = [-around[0], 0, -around[2]];
      const aim = normalize(add(add(mul(around, 0.72), mul(inward, 0.28)), [0, 0.45, 0]));
      direction = turnToward(direction, normalize(add(mul(direction, 1 - turn), mul(aim, turn))), maxTurn);
    }
    // The top used to be a shelf: `if (next[1] > height) next[1] = height` flattened every tip
    // onto one plane ("榻愰《"). Upward growth is suppressed smoothly over the top fraction the
    // profile declares, so a conifer tapers over a longer stretch than a shrub.
    const topStart = 1 - Math.max(0.02, context.profile.heightSuppressionFraction);
    const aboveTop = Math.min(1, Math.max(0, (probe[1] - height * topStart) / Math.max(1e-6, height * (1 - topStart))));
    if (aboveTop > 0) {
      // Same lesson as the shell cap: adding a downward vector to the direction is an unbounded
      // rotation, and near the apex it fired on almost every step. Flatten the direction with the
      // bounded turn instead, so a leader eases into the crown top instead of being folded over.
      const flat = normalize([direction[0], direction[1] * (1 - 0.85 * aboveTop), direction[2]]);
      direction = turnToward(direction, flat, maxTurn);
    }

    // No space means less growth, not a relocated point. Weak steering (short steps, above) is
    // compensated here, so the branch still ends up inside the shell without being bent.
    const suppression = 1 - (0.6 * inwardBias + 0.3 * inwardBias * (1 - turnScale)) * Math.min(1, excess * 4 + 0.2);
    const delta = mul(direction, stepLength * suppression);
    let next = add(position, delta);
    // At the shell, growth continues *along* it. Only the outward radial component of the step is
    // removed; the tangential and vertical parts are kept, so the branch keeps gaining length and
    // bends once, at the boundary, instead of being re-corrected every step.
    //
    // Two earlier attempts are worth not repeating: projecting the point onto the shell changed the
    // segment direction at every step (that is what "kink" means), and shortening the whole step
    // also deleted the tangential growth 鈥?a pine ended up at 53% of its authored length while
    // every step was silently clipped.
    const radialNow = Math.hypot(position[0], position[2]);
    const radialNext = Math.hypot(next[0], next[2]);
    // Why this step moved where it did. Analytic traces are what turned the last two rounds from
    // guessing into finding: without them "the spikes are near the apex" is an impression, not a
    // measurement. `capped` is the shell, `apex` is the height suppression.
    let capped = false, apexed = false;
    if (shellHold > 0) shellHold -= 1;
    if (constrained && (shellHold > 0 || radialNext > envelope(next)) && radialNow > EPS) {
      const ux = position[0] / radialNow, uz = position[2] / radialNow;
      const outward = delta[0] * ux + delta[2] * uz;
      if (outward > 0) {
        // Turn the direction away from "outward" with the same curvature limit the steering uses,
        // instead of subtracting the outward vector from the position. Subtracting is equivalent to
        // an *unbounded* rotation: a twig pointing mostly away from the trunk lost nearly all of its
        // horizontal component in one step, which is a corner. Rotating keeps the step length and
        // caps the angle, so the same correction is spread over the steps that follow.
        const desired = normalize(sub(delta, mul([ux, 0, uz], outward)));
        const length = Math.hypot(delta[0], delta[1], delta[2]);
        direction = turnToward(direction, desired, maxTurn);
        next = add(position, mul(direction, length));
        shellHold = 4;
        capped = true;
      }
    }
    // The top is the same idea on the vertical axis: keep growing sideways, stop gaining height.
    if (next[1] > height * 1.005) { next = [next[0], Math.min(next[1], height * 1.005), next[2]]; apexed = true; }
    // There is deliberately no radial clamp here any more. A 1% projection used to guarantee that
    // the declared crown radius was a bound, and every experiment that tried to make the crown
    // reach its declaration did it by bending the branch — the shape and the declaration were one
    // constraint. The shell above still *steers* (that is the species shape); overshoot is now a
    // measurement published by summarize()/crownMismatch, and the author decides whether the
    // declaration or the branch statistics should change.
    // The tangent has to follow the geometry, not the intent: after the shell steering the
    // displacement can point somewhere else, and a ring built around the wrong tangent is a
    // twisted ring.
    const segmentDirection = normalize(sub(next, position));
    const transported = transportNormal(tangent, segmentDirection, normal);
    normal = normalize(sub(transported, mul(segmentDirection, dot(transported, segmentDirection))));
    const segment = sub(next, position);
    arc += Math.hypot(segment[0], segment[1], segment[2]);
    position = next;
    points.push({
      position,
      tangent: segmentDirection,
      normal,
      radius: Math.max(1e-4, branch.rootRadius * (1 - (1 - taper) * t)),
      arcLength: arc,
      wind: branch.windRoot + (branch.windTip - branch.windRoot) * t,
      // Trace flags, consumed by the morphology tool. They are not used by any geometry path.
      capped,
      apexed,
    });
  }

  branch.points = points;
  // The measured length of the curve, kept next to the authored target so a report can show
  // both (`actualLengthMetres / length` is the cheapest check that steering did not stretch
  // the branch). Sampling uses this one.
  branch.arcLengthMetres = arc;
  branch.tip = position;
  branch.tipDirection = points[points.length - 1].tangent;
  branch.tipRadius = points[points.length - 1].radius;
  branches.push(branch);
  byId.set(branch.id, branch);

  const childDefinition = levels.byLevel.get(branch.level + 1);
  if (!childDefinition) return;

  // Children are distributed along the parent's arc, not stacked at its end. `attachArc`
  // is the authored window; the last child sits at the tip and becomes the leader, which is
  // how a broadleaf keeps a dominant shoot instead of ending in a fork.
  const count = childDefinition.count;
  const [arcLow, arcHigh] = childDefinition.attachArc;
  const phase = rng.span(0, Math.PI * 2);
  for (let index = 0; index < count; ++index) {
    const fraction = count === 1
      ? arcHigh
      : arcLow + (arcHigh - arcLow) * (index / (count - 1));
    // A fraction of the branch means a fraction of its *measured* length: using the target here
    // would put the last child short of the tip whenever the branch curved away from it.
    const attachArc = arcLengthOf(branch) * fraction;
    const sample = sampleBranch(branch, attachArc);
    const azimuth = phase + index * childDefinition.divergence + rng.span(-0.25, 0.25);
    // Apical dominance and the base-to-tip gradient of the profile. For broadleaf/shrub/grass the
    // multipliers are exactly 1, so their trees are unchanged; for a conifer this is what makes
    // the low branches long and near-horizontal and the leader's branches short and steep.
    const order = branchOrderAt(context.profile, fraction);
    let spread = rng.span(childDefinition.spread[0], childDefinition.spread[1]) * order.spread;
    const isLeader = index === count - 1 && childDefinition.apicalDominance > 0;
    if (isLeader) spread *= 1 - childDefinition.apicalDominance;
    // Child direction in the parent frame (G02). `spread` is now the actual angle between
    // the child and the parent tangent, so an authored 40 degrees measures 40 degrees.
    const binormal = normalize(cross(sample.tangent, sample.normal));
    const alongParent = normalize(add(
      mul(sample.tangent, Math.cos(spread)),
      add(mul(sample.normal, Math.cos(azimuth) * Math.sin(spread)),
        mul(binormal, Math.sin(azimuth) * Math.sin(spread)))));
    // Directional apical dominance: a child attached high on its parent is biased toward the
    // vertical axis, which is how a conifer builds a spire out of its growth rule instead of out of
    // its constraint. Stated as a bias toward +Y on purpose — scaling `spread` cannot express this,
    // because at high levels the parent tangent already points outward (measured, R1-14).
    const upBias = context.profile.apicalUpBias ?? 0;
    const childDirection = upBias > 0
      ? normalize(add(alongParent, [0, upBias * fraction * fraction, 0]))
      : alongParent;
    const childLength = branch.length * rng.span(childDefinition.lengthRatio[0], childDefinition.lengthRatio[1]) * order.length;
    const childRadius = Math.max(1e-4, sample.radius * childDefinition.childRadiusScale);
    const childNormal = normalize(sub(
      transportNormal(sample.tangent, childDirection, sample.normal),
      mul(childDirection, dot(transportNormal(sample.tangent, childDirection, sample.normal), childDirection))));
    const id = `${childDefinition.id}/${branch.id}/${index}`;
    growBranch(context, {
      id,
      generatorId: childDefinition.id,
      level: branch.level + 1,
      parentId: branch.id,
      parentIndex: index,
      attachment: {
        parentArcFraction: fraction,
        parentArcMetres: attachArc,
        localAzimuth: azimuth,
        spreadRadians: spread,
        surfaceOffsetMetres: 0,
        junctionId: `${branch.id}@${id}`,
      },
      // The child root is the parent centreline point. No offset (G01): the child tube
      // starts *inside* the parent, so the union of the two surfaces has no hole, and the
      // mesher adds a collar that follows the parent surface.
      start: sample.position,
      startDirection: childDirection,
      startNormal: childNormal,
      startRadius: childRadius,
      rootRadius: childRadius,
      length: childLength,
      windRoot: sample.wind,
      windTip: childDefinition.wind,
      depth: branch.depth + 1,
      points: [],
      childIds: [],
    });
    byId.get(branch.id).childIds.push(id);
  }
}

// Samples a branch at an arc length. Returns the interpolated frame and radius, so callers
// (leaf placement, junction collar, wind) all agree on where a point "on the branch" is.
export function arcLengthOf(branch) {
  if (Number.isFinite(branch.arcLengthMetres)) return branch.arcLengthMetres;
  const points = branch.points;
  // A graph built before arc lengths were measured still has to sample sanely: fall back to the
  // last point's value, then to the authored target.
  if (points && points.length > 0 && Number.isFinite(points[points.length - 1].arcLength)) {
    return points[points.length - 1].arcLength;
  }
  return branch.length;
}

export function sampleBranch(branch, arcMetres) {
  const points = branch.points;
  if (!points || points.length === 0) fail(`branch ${branch.id} has no points`);
  // Metres means metres: clamping to the authored target would make the last stretch of a
  // curved branch unreachable, because the real curve is longer than the target.
  const clamped = Math.max(0, Math.min(arcLengthOf(branch), arcMetres));
  let low = 0, high = points.length - 1;
  while (low + 1 < high) {
    const middle = (low + high) >> 1;
    if (points[middle].arcLength <= clamped) low = middle; else high = middle;
  }
  const a = points[low], b = points[high];
  const span = Math.max(1e-9, b.arcLength - a.arcLength);
  const t = Math.max(0, Math.min(1, (clamped - a.arcLength) / span));
  const tangent = normalize(lerp3(a.tangent, b.tangent, t));
  const normal = normalize(sub(lerp3(a.normal, b.normal, t), mul(tangent, dot(lerp3(a.normal, b.normal, t), tangent))));
  return {
    position: lerp3(a.position, b.position, t),
    tangent,
    normal,
    binormal: normalize(cross(tangent, normal)),
    radius: a.radius + (b.radius - a.radius) * t,
    wind: a.wind + (b.wind - a.wind) * t,
    arcLength: clamped,
    segment: low,
  };
}

// Distance from a point to the branch centreline, used by the junction tests and by the
// leaf-pivot check ("the petiole base is on the branch, in metres").
export function distanceToCentreline(branch, point) {
  let best = Infinity;
  for (let index = 1; index < branch.points.length; ++index) {
    const a = branch.points[index - 1].position, b = branch.points[index].position;
    const ab = sub(b, a);
    const denom = dot(ab, ab);
    const t = denom < EPS ? 0 : Math.max(0, Math.min(1, dot(sub(point, a), ab) / denom));
    const closest = add(a, mul(ab, t));
    best = Math.min(best, length3(sub(point, closest)));
  }
  return best;
}

// ------------------------------------------------------------------ evaluation
export function evaluateBranchGraph({ spec, generators, seed }) {
  if (!generators) fail('evaluateBranchGraph needs a normalized generator graph');
  const height = spec.dimensions.heightMetres;
  const crown = spec.dimensions.crownRadiusMetres;
  // Resolved once, here: an unknown profile throws (NG-E-VEG-SPECIES) instead of quietly
  // generating a broadleaf for a species that asked for something else.
  const profile = resolveProfile(spec);
  // The shell's scale. `crown` above is only published and compared after growth.
  const shapeCrown = shapeCrownRadius(profile, height);
  const trunk = generators.byLevel.get(0);
  if (!trunk) fail('generator graph has no trunk');

  // How many stems this plant has. Declared here (before the growth context is built) because the
  // growth loop needs it: a single-stem tree's trunk is not steered by the crown shell, while a
  // grass clump's or a multi-stem shrub's level-0 branches *are* its limbs and must stay inside it.
  const stemCount = Math.max(1, Math.min(16, Math.round(trunk.count ?? 1)));

  const context = {
    levels: generators,
    envelope: makeEnvelope(height, profile),
    profile,
    stems: stemCount,
    height,
    seed,
    branches: [],
    byId: new Map(),
  };

  const rootRng = new Stream(deriveSeed(seed, 'branch/root'), 'root');
  // A shrub is several stems from the same base, not one trunk. `trunk.count` is the author's
  // knob for that; a tree keeps count = 1. Building them here means the shrub shares the same
  // attachment, sweep and leaf code as a tree instead of a separate recipe.
  //
  // A grass clump is the same mechanism with different numbers: blades spread evenly across the
  // crown footprint and fan outward instead of standing in a tight clump. Routing it through
  // the graph is what removed the last legacy mesher path.
  const grass = spec.kind === 'grass';
  const stems = Math.max(1, Math.min(16, Math.round(trunk.count ?? 1)));
  for (let stem = 0; stem < stems; ++stem) {
    const azimuth = (stem / stems) * Math.PI * 2 + rootRng.span(-0.25, 0.25);
    const single = stems === 1;
    let offset = [0, 0, 0];
    let direction = normalize([rootRng.span(-0.04, 0.04), 1, rootRng.span(-0.04, 0.04)]);
    if (grass) {
      // sqrt(unit) keeps blades area-uniform over the clump disc rather than bunched at the
      // centre, which is the difference between a tuft and a cylinder of grass.
      const radial = Math.sqrt(rootRng.unit()) * shapeCrown;
      const stemAzimuth = rootRng.span(0, Math.PI * 2);
      offset = [Math.cos(stemAzimuth) * radial, 0, Math.sin(stemAzimuth) * radial];
      direction = normalize([Math.cos(stemAzimuth) * 0.3, 1, Math.sin(stemAzimuth) * 0.3]);
    } else if (!single) {
      offset = [Math.cos(azimuth) * shapeCrown * 0.1, 0, Math.sin(azimuth) * shapeCrown * 0.1];
      direction = normalize([Math.cos(azimuth) * 0.38, 1, Math.sin(azimuth) * 0.38]);
    }
    const id = single ? trunk.id : `${trunk.id}/${stem}`;
    growBranch(context, {
      id,
      generatorId: trunk.id,
      level: 0,
      parentId: null,
      parentIndex: stem,
      attachment: null,
      start: offset,
      startDirection: direction,
      startNormal: perpendicular(direction),
      startRadius: spec.dimensions.trunkRadiusMetres,
      rootRadius: spec.dimensions.trunkRadiusMetres,
      // Blade length varies per stem; a clump whose blades are all the same height reads as a
      // brush. Trees keep the authored single value.
      length: height * trunk.startHeightRatio * (grass ? rootRng.span(0.62, 1.0) : 1),
      windRoot: 0,
      windTip: trunk.wind,
      depth: 0,
      points: [],
      childIds: [],
    });
  }

  const graph = {
    schema: BRANCH_GRAPH_SCHEMA,
    seed,
    branches: context.branches,
    byId: context.byId,
    rootId: stems === 1 ? trunk.id : `${trunk.id}/0`,
    heightMetres: height,
    // The declaration, kept for comparison only.
    crownRadiusMetres: crown,
    // The crown the species shape actually grew toward. Not the declaration: see
    // species-profile.mjs `crownRadiusPerHeight`.
    shapeCrownRadiusMetres: shapeCrown,
    stats: null,
  };
  graph.stats = summarize(graph);
  return graph;
}

// Compares the crown the graph actually grew against the authored bound, and says plainly whether
// they disagree. This is a diagnostic, not a correction: the generator no longer clamps a tree onto
// its declaration (see reports/vegetation-realism/design-shape-vs-bound.md), so the honest output
// is "the authoring numbers and the branch statistics disagree by N%". A caller that wants a hard
// failure can throw on `mismatch`; the cook reports it and keeps the geometry.
//
// The 15% default is the tolerance that matches `morphology.json`'s frozen target range for
// `crownFractionOfAuthored` (0.85鈥?.05): a report and an acceptance check must not disagree about
// how far is "too far", or a tree can pass one and be flagged by the other 鈥?which is exactly what
// happened when this defaulted to 8% and the anatomy check allowed 0.8.
export function crownMismatch(graph, tolerance = 0.15) {
  const declared = graph?.crownRadiusMetres;
  const measured = graph?.stats?.crownRadialMetres ?? graph?.crownRadialMetres;
  const shape = graph?.shapeCrownRadiusMetres ?? null;
  if (!(declared > 0) || !Number.isFinite(measured)) {
    return {
      declared: declared ?? null, measured: measured ?? null, shape, fraction: null,
      overshoot: null, shapeFraction: null, tolerance, mismatch: false, reason: 'unmeasured',
    };
  }
  const fraction = measured / declared;
  return {
    declared,
    measured,
    shape,
    fraction,
    overshoot: fraction - 1,
    shapeFraction: shape > 0 ? measured / shape : null,
    tolerance,
    mismatch: Math.abs(fraction - 1) > tolerance,
  };
}

function summarize(graph) {
  let totalLength = 0, totalArc = 0, maxDepth = 0, maxRadius = 0, minimumRadius = Infinity;
  let minimumY = Infinity, maximumY = -Infinity, maximumRadial = 0;
  let junctionGapMax = 0;
  for (const branch of graph.branches) {
    totalLength += branch.length;
    totalArc += arcLengthOf(branch);
    maxDepth = Math.max(maxDepth, branch.depth);
    maxRadius = Math.max(maxRadius, branch.rootRadius);
    minimumRadius = Math.min(minimumRadius, branch.tipRadius);
    for (const point of branch.points) {
      minimumY = Math.min(minimumY, point.position[1]);
      maximumY = Math.max(maximumY, point.position[1]);
      maximumRadial = Math.max(maximumRadial, Math.hypot(point.position[0], point.position[2]));
    }
    if (branch.parentId) {
      const parent = graph.byId.get(branch.parentId);
      // The child root must be *on* the parent centreline. This is the number the geometry
      // report publishes: a gap here is a visible crack in the wood.
      const parentPoint = sampleBranch(parent, branch.attachment.parentArcMetres).position;
      junctionGapMax = Math.max(junctionGapMax, length3(sub(branch.points[0].position, parentPoint)));
    }
  }
  return {
    branchCount: graph.branches.length,
    maxDepth,
    totalLengthMetres: totalLength,
    // Both numbers, because they answer different questions: the target is what the author asked
    // for, the arc is what the tree actually is. A report that only prints one of them cannot
    // tell "the tree is small" from "the clamp ate the growth".
    totalArcLengthMetres: totalArc,
    maxRadiusMetres: maxRadius,
    minTipRadiusMetres: minimumRadius === Infinity ? 0 : minimumRadius,
    boundsMinY: minimumY,
    boundsMaxY: maximumY,
    crownRadialMetres: maximumRadial,
    // Diagnostic, not a constraint: how far the grown crown landed from the authored declaration.
    // A tree is not forced onto its declaration — a value away from 1.0 means the authoring numbers
    // and the branch statistics disagree, and that is reported instead of being clamped away.
    crownFractionOfAuthored: graph.crownRadiusMetres > 0
      ? maximumRadial / graph.crownRadiusMetres
      : null,
    // The same gap stated as an overshoot fraction (0 = exactly on, 0.12 = 12% past). Kept next to
    // the ratio because an acceptance report reads "overshoots by 12%" more directly than "reaches
    // 112%", while code that already consumed the ratio keeps working.
    crownOvershootFraction: graph.crownRadiusMetres > 0
      ? maximumRadial / graph.crownRadiusMetres - 1
      : null,
    // How the grown crown compares with the species shape it grew toward. This is the calibration
    // number for `crownRadiusPerHeight`: the shape trait is right when this stays near 1 across
    // seeds, and the declaration is right when crownFractionOfAuthored does.
    shapeCrownRadiusMetres: graph.shapeCrownRadiusMetres ?? null,
    crownFractionOfShape: graph.shapeCrownRadiusMetres > 0
      ? maximumRadial / graph.shapeCrownRadiusMetres
      : null,
    junctionGapMaxMetres: junctionGapMax,
  };
}

// ---------------------------------------------------------------- validation
// Structural invariants. These are the checks that catch the class of bug this module
// exists to prevent, so they run on every build rather than only in tests.
export function validateBranchGraph(graph, options = {}) {
  const tolerance = options.attachmentToleranceMetres ?? 1e-5;
  const problems = [];
  const seen = new Set();
  for (const branch of graph.branches) {
    if (seen.has(branch.id)) problems.push(`duplicate branch id ${branch.id}`);
    seen.add(branch.id);
    if (branch.points.length < 2) problems.push(`branch ${branch.id} has fewer than two points`);
    for (const point of branch.points) {
      for (const value of [...point.position, ...point.tangent, point.radius, point.wind, point.arcLength]) {
        if (!Number.isFinite(value)) { problems.push(`branch ${branch.id} has a non-finite point field`); break; }
      }
      if (!(point.radius > 0)) problems.push(`branch ${branch.id} has a non-positive radius`);
    }
    if (branch.parentId === null) continue;
    const parent = graph.byId.get(branch.parentId);
    if (!parent) { problems.push(`branch ${branch.id} references missing parent ${branch.parentId}`); continue; }
    // Walk up to the root with a bound; a cycle would otherwise loop forever.
    let cursor = parent, steps = 0;
    while (cursor && cursor.parentId !== null) {
      cursor = graph.byId.get(cursor.parentId);
      if (++steps > graph.branches.length + 1) { problems.push(`branch ${branch.id} is part of a parent cycle`); break; }
    }
    const expected = sampleBranch(parent, branch.attachment.parentArcMetres).position;
    const gap = length3(sub(branch.points[0].position, expected));
    if (gap > tolerance) problems.push(`branch ${branch.id} is detached from ${parent.id} by ${gap.toFixed(6)} m`);
    const parentRadius = sampleBranch(parent, branch.attachment.parentArcMetres).radius;
    if (branch.rootRadius > parentRadius * 1.05) {
      problems.push(`branch ${branch.id} root radius ${branch.rootRadius.toFixed(4)} exceeds parent ${parentRadius.toFixed(4)}`);
    }
    if (branch.depth !== parent.depth + 1) problems.push(`branch ${branch.id} depth does not follow its parent`);
  }
  if (problems.length) {
    const error = new Error(`branch graph is invalid: ${problems.slice(0, 4).join('; ')}${problems.length > 4 ? ` (+${problems.length - 4} more)` : ''}`);
    error.code = 'NG-E-VEG-GRAPH';
    error.problems = problems;
    throw error;
  }
  return { branchCount: graph.branches.length, tolerance };
}

// Leaf/frond host selection. `parentLevel` means "branches at exactly this level", which is
// what an author expects from "attach foliage to level 3"; the old `>=` comparison silently
// attached foliage to every deeper level too.
export function selectHostBranches(graph, frond) {
  const exact = graph.branches.filter((branch) => branch.level === frond.parentLevel);
  if (exact.length) return exact;
  // A level that does not exist is an authoring error, not a reason to attach foliage
  // somewhere else; returning nothing makes the empty crown visible immediately.
  return [];
}

import { GarmentError } from './inspect.mjs';
import { TriangleGrid, signedDistanceRobust } from './grid.mjs';
import { skinPalette, poseSkeleton } from '../character/rig-mesh.mjs';
import { characterCollisionProxies } from '../hair/scalp.mjs';

// Character garment simulation: skin target, dynamic blend, displacement limit, anchors,
// body collision, and the measurements that decide whether it is acceptable.
//
// Scope is stated up front because overstating it is the failure mode the plan warns
// about: this solver supports a *single layer* of cloth colliding against bone-driven
// body proxies for the head, torso and shoulders involved in a hair/garment check, plus
// optional accurate body mesh collision for the final measurement. It does **not**
// support self-collision or cloth-cloth collision, and it says so in every report
// instead of producing a plausible-looking result for a long dress.

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

export const SUPPORTED = Object.freeze({
  singleLayer: true,
  selfCollision: false,
  layerCollision: false,
  continuousCollision: 'adaptive-substeps',
  // Endpoint projection is not CCD. Substepping reduces tunnelling; it does not prove
  // the absence of it, which is why `sweptCheck` still reports crossings.
  trueSweptCcd: false,
});

// Per-vertex physics contract. These are separate channels on purpose: the plan requires
// skin weight, dynamic blend, maximum displacement and collision thickness to be
// independently adjustable, and storing them in one number makes "the skirt is stiff"
// impossible to diagnose.
export function defaultPhysicsContract(garment, {
  dynamicBlend = 0.35, maxDisplacementMetres = 0.03, collisionThicknessMetres = 0.004,
  backsideLimitMetres = 0.002, anchorStiffness = 0.8, particleMassKg = 0.004,
} = {}) {
  const blend = new Float32Array(garment.vertices).fill(dynamicBlend);
  const maxDisplacement = new Float32Array(garment.vertices).fill(maxDisplacementMetres);
  const thickness = new Float32Array(garment.vertices).fill(collisionThicknessMetres);
  const backside = new Float32Array(garment.vertices).fill(backsideLimitMetres);
  const anchors = new Float32Array(garment.vertices);
  const mass = new Float32Array(garment.vertices).fill(particleMassKg);
  return { blend, maxDisplacement, thickness, backside, anchors, mass };
}

export function setRegionPhysics(contract, garment, regions, regionNames, values) {
  const allowed = new Set(['blend', 'maxDisplacement', 'thickness', 'backside', 'anchors', 'mass']);
  for (const key of Object.keys(values)) {
    fail(allowed.has(key), 'NG-E-GARMENT-PHYSICS', `unknown physics channel ${key}`);
  }
  const nameSet = new Set(regionNames);
  let touched = 0;
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    if (!nameSet.has(regions[vertex])) continue;
    ++touched;
    if (values.blend !== undefined) contract.blend[vertex] = values.blend;
    if (values.maxDisplacement !== undefined) contract.maxDisplacement[vertex] = values.maxDisplacement;
    if (values.thickness !== undefined) contract.thickness[vertex] = values.thickness;
    if (values.backside !== undefined) contract.backside[vertex] = values.backside;
    if (values.anchors !== undefined) contract.anchors[vertex] = values.anchors;
    if (values.mass !== undefined) contract.mass[vertex] = values.mass;
  }
  return { touched, regions: regionNames };
}

// Distance constraints over mesh edges, with rest lengths and a stiffness from the
// material. A garment without stretch constraints behaves like a rigid shell and the
// first measurement shows it: zero stretch, maximum penetration.
export function buildConstraints(garment, material) {
  const seen = new Set();
  const edges = [];
  for (let triangle = 0; triangle < garment.triangles; ++triangle) {
    const a = garment.indices[triangle * 3], b = garment.indices[triangle * 3 + 1], c = garment.indices[triangle * 3 + 2];
    for (const [from, to] of [[a, b], [b, c], [c, a]]) {
      const key = from < to ? `${from},${to}` : `${to},${from}`;
      if (seen.has(key)) continue;
      seen.add(key);
      const rest = Math.hypot(
        garment.positions[from * 3] - garment.positions[to * 3],
        garment.positions[from * 3 + 1] - garment.positions[to * 3 + 1],
        garment.positions[from * 3 + 2] - garment.positions[to * 3 + 2]);
      if (rest < 1e-9) continue;
      edges.push({ a: from, b: to, rest });
    }
  }
  return {
    edges,
    stretchCompliance: material.stretchCompliance ?? 0.0,
    shearCompliance: material.shearCompliance ?? 0.008,
    damping: material.damping ?? 0.12,
    drag: material.dragCoefficient ?? 0.15,
    // 15% strain is already visible as a stretched seam; beyond it the cloth is being
    // pulled apart by the pose, not stretching like fabric.
    strainLimit: material.strainLimit ?? 1.15,
  };
}

export function createSimulation(garment, character, weights, {
  contract = defaultPhysicsContract(garment), material = {}, anchors = null,
} = {}) {
  fail(garment.vertices === weights.vertices, 'NG-E-GARMENT-PHYSICS',
    'weights and garment must describe the same vertex count');
  const constraints = buildConstraints(garment, material);
  if (anchors) {
    for (const anchor of anchors) {
      fail(anchor.vertex >= 0 && anchor.vertex < garment.vertices, 'NG-E-GARMENT-PHYSICS',
        `anchor references vertex ${anchor.vertex}`);
      contract.anchors[anchor.vertex] = anchor.stiffness ?? 1;
    }
  }
  const state = {
    garment, character, weights, contract, constraints,
    position: Float64Array.from(garment.positions),
    previous: Float64Array.from(garment.positions),
    velocity: new Float64Array(garment.vertices * 3),
    target: Float64Array.from(garment.positions),
    anchorsApplied: anchors ? anchors.length : 0,
    timeSeconds: 0,
    paused: false,
    teleports: 0,
    resetEvents: 0,
    nonFiniteEvents: 0,
    lastSubsteps: 1,
    // Vertices whose displacement limit was active on the last tick. A diagnostic, not a
    // failure: the soft limit is expected to engage on a garment that hangs.
    limitExceeded: new Uint8Array(garment.vertices),
  };
  updateSkinTarget(state, {});
  state.position.set(state.target);
  state.previous.set(state.target);
  return state;
}

// Skin target for every garment vertex: position of its rest point transformed by the
// blended bone palette. This is the "skinned goal" the physics is blended towards, and
// it must be recomputed from the posed palette every tick — caching it is how a garment
// ends up one frame behind the character.
export function updateSkinTarget(state, pose) {
  const { character, weights, garment, contract } = state;
  const palette = skinPalette(character.bones, poseSkeleton(character.bones, pose));
  const { maximumInfluences } = weights;
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    const rest = [garment.positions[vertex * 3], garment.positions[vertex * 3 + 1], garment.positions[vertex * 3 + 2]];
    let x = 0, y = 0, z = 0, total = 0;
    for (let slot = 0; slot < maximumInfluences; ++slot) {
      const weight = weights.weights[vertex * maximumInfluences + slot];
      if (weight <= 0) continue;
      const matrix = palette[weights.joints[vertex * maximumInfluences + slot]];
      x += (matrix[0] * rest[0] + matrix[4] * rest[1] + matrix[8] * rest[2] + matrix[12]) * weight;
      y += (matrix[1] * rest[0] + matrix[5] * rest[1] + matrix[9] * rest[2] + matrix[13]) * weight;
      z += (matrix[2] * rest[0] + matrix[6] * rest[1] + matrix[10] * rest[2] + matrix[14]) * weight;
      total += weight;
    }
    fail(total > 1e-4, 'NG-E-GARMENT-PHYSICS', `vertex ${vertex} has no weight to build a skin target`);
    state.target[vertex * 3] = x / total;
    state.target[vertex * 3 + 1] = y / total;
    state.target[vertex * 3 + 2] = z / total;
  }
  void contract;
  return state.target;
}

function proxiesFor(state) {
  return characterCollisionProxies(state.character, {});
}

// One fixed tick. Fixed substeps, XPBD distance constraints, then collision projection and
// displacement clamping. Order matters: clamping before collision would push a clamped
// vertex back inside the body.
export function tick(state, {
  dt = 1 / 120, gravity = [0, -9.81, 0], wind = [0, 0, 0], bodyProxies = null, substeps = null,
} = {}) {
  if (state.paused) return collectMetrics(state, bodyProxies);
  const { garment, contract, constraints } = state;
  const proxies = bodyProxies ?? proxiesFor(state);

  // Adaptive substepping: when a vertex moves more than a quarter of the smallest
  // collision thickness in one tick, a single projection can pass through the body.
  let maximumSpeed = 0;
  for (let index = 0; index < state.velocity.length; ++index) {
    maximumSpeed = Math.max(maximumSpeed, Math.abs(state.velocity[index]));
  }
  const minimumThickness = Math.min(...contract.thickness);
  const wanted = Math.ceil((maximumSpeed * dt) / Math.max(1e-4, minimumThickness * 0.25));
  const steps = Math.min(8, Math.max(1, substeps ?? wanted));
  state.lastSubsteps = steps;
  const stepDt = dt / steps;
  const damping = Math.pow(1 - constraints.damping, stepDt * 60);

  // Position-based dynamics, in the canonical order: predict, project, derive velocity.
  // Doing projections in the middle of the integration loop (an easy shortcut) feeds the
  // correction back in as velocity and the cloth explodes at the first pose change.
  for (let step = 0; step < steps; ++step) {
    // 1. predict
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      if (contract.blend[vertex] <= 0) {
        for (let axis = 0; axis < 3; ++axis) {
          state.position[vertex * 3 + axis] = state.target[vertex * 3 + axis];
          state.previous[vertex * 3 + axis] = state.target[vertex * 3 + axis];
          state.velocity[vertex * 3 + axis] = 0;
        }
        continue;
      }
      for (let axis = 0; axis < 3; ++axis) {
        const acceleration = gravity[axis] + wind[axis] * constraints.drag;
        state.velocity[vertex * 3 + axis] = (state.velocity[vertex * 3 + axis] + acceleration * stepDt) * damping;
        state.previous[vertex * 3 + axis] = state.position[vertex * 3 + axis];
        state.position[vertex * 3 + axis] += state.velocity[vertex * 3 + axis] * stepDt;
      }
    }

    // 2. project. Two passes: collision can push a vertex away from its neighbours, so a
    //    single pass leaves the mesh stretched at the contact line. The second pass pulls
    //    the neighbours after the pushed vertex and re-applies the hard constraints.
    for (let pass = 0; pass < 2; ++pass) {
      for (let iteration = 0; iteration < 4; ++iteration) {
        for (const edge of constraints.edges) {
          const a = edge.a, b = edge.b;
          const wa = contract.blend[a] > 0 ? 1 : 0;
          const wb = contract.blend[b] > 0 ? 1 : 0;
          const total = wa + wb;
          if (total === 0) continue;
          const dx = state.position[b * 3] - state.position[a * 3];
          const dy = state.position[b * 3 + 1] - state.position[a * 3 + 1];
          const dz = state.position[b * 3 + 2] - state.position[a * 3 + 2];
          const length = Math.hypot(dx, dy, dz) || 1e-9;
          const correction = ((length - edge.rest) / length) / total;
          if (wa) {
            state.position[a * 3] += dx * correction;
            state.position[a * 3 + 1] += dy * correction;
            state.position[a * 3 + 2] += dz * correction;
          }
          if (wb) {
            state.position[b * 3] -= dx * correction;
            state.position[b * 3 + 1] -= dy * correction;
            state.position[b * 3 + 2] -= dz * correction;
          }
        }
      }

      for (let vertex = 0; vertex < garment.vertices; ++vertex) {
        if (contract.blend[vertex] <= 0) continue;
        const anchor = contract.anchors[vertex];
        if (anchor > 0) {
          for (let axis = 0; axis < 3; ++axis) {
            state.position[vertex * 3 + axis] += (state.target[vertex * 3 + axis] - state.position[vertex * 3 + axis]) * anchor;
          }
        }
        const offset = [
          state.position[vertex * 3] - state.target[vertex * 3],
          state.position[vertex * 3 + 1] - state.target[vertex * 3 + 1],
          state.position[vertex * 3 + 2] - state.target[vertex * 3 + 2],
        ];
        const distance = Math.hypot(offset[0], offset[1], offset[2]);
        const limit = contract.maxDisplacement[vertex];
        if (distance > limit && distance > 1e-9) {
          // Soft limit: pull half of the excess back per substep instead of snapping to
          // the limit. A hard clamp re-imposes itself after the strain constraint and turns
          // a region boundary into a torn seam, because two neighbouring vertices are then
          // each held rigidly by different limits.
          const allowed = limit + (distance - limit) * 0.5;
          for (let axis = 0; axis < 3; ++axis) {
            state.position[vertex * 3 + axis] = state.target[vertex * 3 + axis] + offset[axis] * (allowed / distance);
          }
          state.limitExceeded[vertex] = 1;
        }
        resolveCollision(state, vertex, proxies);
      }

      // Strain limiting. A pure XPBD solve with four iterations lets a long edge run far
      // past its rest length before the correction propagates, and the reported stretch
      // ratio then describes the iteration count rather than the cloth.
      const strainLimit = constraints.strainLimit;
      // Three sweeps: one sweep only halves the excess on a single edge, and a mesh needs
      // the correction to propagate from the contact line before the metric settles.
      for (let sweep = 0; sweep < 3; ++sweep)
      for (const edge of constraints.edges) {
        const a = edge.a, b = edge.b;
        const wa = contract.blend[a] > 0 ? 1 : 0;
        const wb = contract.blend[b] > 0 ? 1 : 0;
        const total = wa + wb;
        if (total === 0) continue;
        const dx = state.position[b * 3] - state.position[a * 3];
        const dy = state.position[b * 3 + 1] - state.position[a * 3 + 1];
        const dz = state.position[b * 3 + 2] - state.position[a * 3 + 2];
        const length = Math.hypot(dx, dy, dz) || 1e-9;
        const maximum = edge.rest * strainLimit;
        if (length <= maximum) continue;
        const correction = ((length - maximum) / length) / total;
        if (wa) {
          state.position[a * 3] += dx * correction;
          state.position[a * 3 + 1] += dy * correction;
          state.position[a * 3 + 2] += dz * correction;
        }
        if (wb) {
          state.position[b * 3] -= dx * correction;
          state.position[b * 3 + 1] -= dy * correction;
          state.position[b * 3 + 2] -= dz * correction;
        }
      }
    }

    // 3. derive velocity from the projected positions
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      if (contract.blend[vertex] <= 0) continue;
      for (let axis = 0; axis < 3; ++axis) {
        if (!Number.isFinite(state.position[vertex * 3 + axis])) {
          ++state.nonFiniteEvents;
          state.position[vertex * 3 + axis] = state.target[vertex * 3 + axis];
          state.previous[vertex * 3 + axis] = state.target[vertex * 3 + axis];
          state.velocity[vertex * 3 + axis] = 0;
          continue;
        }
        let velocity = (state.position[vertex * 3 + axis] - state.previous[vertex * 3 + axis]) / stepDt;
        const speedLimit = contract.maxSpeed ?? 12;
        if (Math.abs(velocity) > speedLimit) velocity = Math.sign(velocity) * speedLimit;
        state.velocity[vertex * 3 + axis] = velocity;
      }
    }
  }
  state.timeSeconds += dt;
  return collectMetrics(state, proxies);
}

function resolveCollision(state, vertex, proxies) {
  const { contract } = state;
  const thickness = contract.thickness[vertex];
  let touched = false;
  // Front side: keep the cloth outside the body by its own thickness. Back side: a
  // separate, usually smaller allowance for moving towards the body, so tightening a
  // sleeve does not make the whole shirt thinner.
  const limit = thickness;
  for (const sphere of proxies.spheres ?? []) {
    const dx = state.position[vertex * 3] - sphere.centre[0];
    const dy = state.position[vertex * 3 + 1] - sphere.centre[1];
    const dz = state.position[vertex * 3 + 2] - sphere.centre[2];
    const distance = Math.hypot(dx, dy, dz);
    const required = sphere.radiusMetres + limit;
    if (distance < required && distance > 1e-9) {
      const scale = required / distance;
      state.position[vertex * 3] = sphere.centre[0] + dx * scale;
      state.position[vertex * 3 + 1] = sphere.centre[1] + dy * scale;
      state.position[vertex * 3 + 2] = sphere.centre[2] + dz * scale;
      touched = true;
    }
  }
  for (const capsule of proxies.capsules ?? []) {
    const abx = capsule.b[0] - capsule.a[0], aby = capsule.b[1] - capsule.a[1], abz = capsule.b[2] - capsule.a[2];
    const lengthSquared = abx * abx + aby * aby + abz * abz;
    const apx = state.position[vertex * 3] - capsule.a[0];
    const apy = state.position[vertex * 3 + 1] - capsule.a[1];
    const apz = state.position[vertex * 3 + 2] - capsule.a[2];
    const t = lengthSquared > 0
      ? Math.max(0, Math.min(1, (apx * abx + apy * aby + apz * abz) / lengthSquared))
      : 0;
    const cx = capsule.a[0] + abx * t, cy = capsule.a[1] + aby * t, cz = capsule.a[2] + abz * t;
    const dx = state.position[vertex * 3] - cx;
    const dy = state.position[vertex * 3 + 1] - cy;
    const dz = state.position[vertex * 3 + 2] - cz;
    const distance = Math.hypot(dx, dy, dz);
    const required = capsule.radiusMetres + limit;
    if (distance < required && distance > 1e-9) {
      const scale = required / distance;
      state.position[vertex * 3] = cx + dx * scale;
      state.position[vertex * 3 + 1] = cy + dy * scale;
      state.position[vertex * 3 + 2] = cz + dz * scale;
      touched = true;
    }
  }
  return touched;
}

export function collectMetrics(state, proxies = null) {
  const { garment, constraints, contract } = state;
  let maximumStretch = 1;
  for (const edge of constraints.edges) {
    const length = Math.hypot(
      state.position[edge.b * 3] - state.position[edge.a * 3],
      state.position[edge.b * 3 + 1] - state.position[edge.a * 3 + 1],
      state.position[edge.b * 3 + 2] - state.position[edge.a * 3 + 2]);
    maximumStretch = Math.max(maximumStretch, length / edge.rest);
  }
  let maximumSpeed = 0;
  for (let index = 0; index < state.velocity.length; ++index) {
    maximumSpeed = Math.max(maximumSpeed, Math.abs(state.velocity[index]));
  }
  const penetration = proxies ? penetrationAgainstProxies(state, proxies) : null;
  return {
    timeSeconds: state.timeSeconds,
    maximumStretchRatio: maximumStretch,
    maximumSpeedMetresPerSecond: maximumSpeed,
    nonFiniteEvents: state.nonFiniteEvents,
    substeps: state.lastSubsteps,
    penetration,
    thicknessMetres: Math.min(...contract.thickness),
  };
}

export function penetrationAgainstProxies(state, proxies) {
  const depths = [];
  let worst = { vertex: -1, depthMetres: 0 };
  for (let vertex = 0; vertex < state.garment.vertices; ++vertex) {
    const point = [state.position[vertex * 3], state.position[vertex * 3 + 1], state.position[vertex * 3 + 2]];
    for (const sphere of proxies.spheres ?? []) {
      const depth = sphere.radiusMetres - Math.hypot(
        point[0] - sphere.centre[0], point[1] - sphere.centre[1], point[2] - sphere.centre[2]);
      if (depth > 0) { depths.push(depth); if (depth > worst.depthMetres) worst = { vertex, depthMetres: depth, proxy: sphere.bone }; }
    }
    for (const capsule of proxies.capsules ?? []) {
      const abx = capsule.b[0] - capsule.a[0], aby = capsule.b[1] - capsule.a[1], abz = capsule.b[2] - capsule.a[2];
      const lengthSquared = abx * abx + aby * aby + abz * abz;
      const apx = point[0] - capsule.a[0], apy = point[1] - capsule.a[1], apz = point[2] - capsule.a[2];
      const t = lengthSquared > 0
        ? Math.max(0, Math.min(1, (apx * abx + apy * aby + apz * abz) / lengthSquared)) : 0;
      const distance = Math.hypot(point[0] - (capsule.a[0] + abx * t), point[1] - (capsule.a[1] + aby * t), point[2] - (capsule.a[2] + abz * t));
      const depth = capsule.radiusMetres - distance;
      if (depth > 0) { depths.push(depth); if (depth > worst.depthMetres) worst = { vertex, depthMetres: depth, proxy: capsule.bone }; }
    }
  }
  depths.sort((a, b) => a - b);
  const percentile = (fraction) => (depths.length === 0 ? 0
    : depths[Math.min(depths.length - 1, Math.floor(depths.length * fraction))]);
  return {
    samples: depths.length,
    p95Metres: percentile(0.95),
    p99Metres: percentile(0.99),
    maximumMetres: depths.length ? depths[depths.length - 1] : 0,
    worst,
  };
}

// Accurate penetration against the body *mesh*, used at the end of a sequence. Proxy
// penetration proves the solver pushed particles out of a capsule; this proves they are
// outside the character.
export function measureMeshPenetration(state, body, { grid = null } = {}) {
  const bodyGrid = grid ?? new TriangleGrid(body.positions, body.indices);
  const depths = [];
  for (let vertex = 0; vertex < state.garment.vertices; ++vertex) {
    const point = [state.position[vertex * 3], state.position[vertex * 3 + 1], state.position[vertex * 3 + 2]];
    const nearest = bodyGrid.nearest(point, { maxDistanceMetres: 0.2 });
    const distance = signedDistanceRobust(point, nearest, bodyGrid, body.positions);
    if (distance !== null && distance < 0) depths.push(-distance);
  }
  depths.sort((a, b) => a - b);
  const percentile = (fraction) => (depths.length === 0 ? 0
    : depths[Math.min(depths.length - 1, Math.floor(depths.length * fraction))]);
  return {
    penetratingVertices: depths.length,
    p95Metres: percentile(0.95), p99Metres: percentile(0.99),
    maximumMetres: depths.length ? depths[depths.length - 1] : 0,
  };
}

// Motion sequence. A garment that passes at rest and fails during a walk has not passed,
// and the plan's action list (idle, walk, run, turn, arms up, crouch, jump, teleport) is
// what the sequence models.
export const GARMENT_TEST_POSES = Object.freeze({
  idle: {},
  walk_contact: { leftupleg: { axis: [1, 0, 0], radians: 0.5 }, rightupleg: { axis: [1, 0, 0], radians: -0.5 } },
  run_stride: { leftupleg: { axis: [1, 0, 0], radians: 0.9 }, rightupleg: { axis: [1, 0, 0], radians: -0.7 }, leftarm: { axis: [1, 0, 0], radians: -0.8 } },
  turn_fast: { hips: { axis: [0, 1, 0], radians: 1.2 }, spine: { axis: [0, 1, 0], radians: 0.4 } },
  arms_up: { leftarm: { axis: [0, 0, 1], radians: 1.4 }, rightarm: { axis: [0, 0, 1], radians: -1.4 } },
  crouch: { leftupleg: { axis: [1, 0, 0], radians: 1.1 }, leftleg: { axis: [1, 0, 0], radians: -1.6 }, rightupleg: { axis: [1, 0, 0], radians: 1.1 }, rightleg: { axis: [1, 0, 0], radians: -1.6 } },
  jump: { hips: { axis: [1, 0, 0], radians: 0.25 } },
});

export function simulatePose(state, poseName, pose, {
  ticks = 24, dt = 1 / 120, gravity = [0, -9.81, 0], wind = [0, 0, 0],
} = {}) {
  updateSkinTarget(state, pose);
  const proxies = proxiesFor(state);
  let worst = { p99Metres: 0, maximumStretchRatio: 1, nonFiniteEvents: 0 };
  let penetrationSum = 0;
  const samples = [];
  for (let index = 0; index < ticks; ++index) {
    const metrics = tick(state, { dt, gravity, wind, bodyProxies: proxies });
    if (metrics.penetration) {
      worst.p99Metres = Math.max(worst.p99Metres, metrics.penetration.p99Metres);
      penetrationSum += metrics.penetration.p99Metres;
    }
    worst.maximumStretchRatio = Math.max(worst.maximumStretchRatio, metrics.maximumStretchRatio);
    worst.nonFiniteEvents += metrics.nonFiniteEvents;
    samples.push(metrics);
  }
  return {
    pose: poseName,
    ticks,
    worstP99Metres: worst.p99Metres,
    meanP99Metres: penetrationSum / Math.max(1, ticks),
    maxStretchRatio: worst.maximumStretchRatio,
    nonFiniteEvents: worst.nonFiniteEvents,
    finalMetrics: samples[samples.length - 1],
  };
}

// Teleport and reset: the plan requires handling large displacements, which is exactly
// where a naive solver explodes. A teleport re-seeds positions from the new skin target
// and zeroes velocity instead of integrating a 5 m step.
export function teleport(state, pose) {
  updateSkinTarget(state, pose);
  state.position.set(state.target);
  state.previous.set(state.target);
  state.velocity.fill(0);
  ++state.teleports;
  return collectMetrics(state);
}

export function reset(state) {
  state.position.set(state.target);
  state.previous.set(state.target);
  state.velocity.fill(0);
  state.nonFiniteEvents = 0;
  ++state.resetEvents;
  return collectMetrics(state);
}

export function setPaused(state, paused) { state.paused = Boolean(paused); return { paused: state.paused }; }

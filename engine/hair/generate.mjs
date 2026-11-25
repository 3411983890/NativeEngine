import { Stream, mix32 } from '../vegetation/random.mjs';
import {
  add, sub, mul, dot, normalize, resampleCurve, arcLength, validateCurve,
} from '../groom/curves.mjs';
import { skinPalette, poseSkeleton } from '../character/rig-mesh.mjs';
import { HairError, regionSeed, normalizeHairstyle, regionCentreAzimuth } from './spec.mjs';
import {
  buildScalp, sampleRoots, guideCountForRegion, regionArea, resolveBoundPoint, scalpHash,
  characterCollisionProxies, resolveProxyPenetration,
} from './scalp.mjs';

// Region-aware guide generation on a real scalp.
//
// Three properties decide whether this generator is usable for the workflow in the
// plan, and none of them is "looks like hair from far away":
//
//  1. Determinism. Same source, same character, same bytes — including strand order.
//  2. Region isolation. Editing `bangs` regenerates `bangs` only. That is achieved by
//     giving every region its own seed stream and its own root sampling; there is no
//     shared pool of random numbers that an extra region could consume from.
//  3. Stable identity. A guide is named by where it grows on the scalp, not by its
//     index in an array, so pruning one guide does not rename the others and a patch
//     that targets `bangs#8f31c2` keeps meaning the same lock of hair.

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const DEG = Math.PI / 180;

// Angular distance that respects the ±180 seam.
export function angularDistance(a, b) {
  return Math.abs(((a - b + 540) % 360) - 180);
}

function stableSuffix(triangle, bary) {
  const q = bary.map((value) => Math.round(value * 4095));
  const hash = mix32(mix32(triangle + 0x9e3779b9) ^ (q[0] << 16) ^ (q[1] << 4) ^ q[2]);
  return hash.toString(16).padStart(8, '0').slice(0, 6);
}

// A parting is a line, not a parameter. Guides on one side start their flow away from
// it and the roots never cross it, which is what makes the scalp visible along the
// seam instead of the hair collapsing into it.
function partingOffset(region, root) {
  if (!region.parting || region.parting.enabled === false) return null;
  const distance = angularDistance(root.azimuthDegrees, region.parting.azimuthDegrees);
  if (distance > region.parting.widthDegrees) return null;
  // Inside the parting band: push sideways, away from the seam, scaled by how close
  // the root is. 1 means "at the seam, maximum deflection".
  const side = Math.sign(((root.azimuthDegrees - region.parting.azimuthDegrees + 540) % 360) - 180) || 1;
  return { side, strength: 1 - distance / region.parting.widthDegrees };
}

// Bundle membership for braids, ponytails and buns. Explicit, because a bundle is a
// constraint topology: strands in a braid share a spine and a twist phase, and those
// are not expressible as a noise parameter.
function bundleOf(region, root) {
  const bundle = region.bundle;
  if (!bundle || bundle.kind === 'none') return null;
  const anchor = bundle.anchorAzimuthDegrees ?? regionCentreAzimuth(region);
  // Bundle index by angular proximity so neighbouring guides share a bundle and the
  // group boundaries are stable under regeneration.
  const span = 360 / Math.max(1, bundle.count);
  const relative = ((root.azimuthDegrees - anchor + 540) % 360) - 180;
  const index = clamp(Math.floor((relative + 180) / span), 0, bundle.count - 1);
  return { kind: bundle.kind, index, anchor, span, turnPerMetre: bundle.turnPerMetre ?? 0 };
}

function growGuide(root, region, options, stream) {
  const { segments } = options;
  const override = options.override ?? null;
  // A per-guide length edit scales the curve, it never moves the root: the root frame
  // belongs to the scalp, and an edit that detaches it is the failure mode the plan
  // calls "发根漂移".
  const lengthMetres = options.lengthMetres * (override?.lengthScale ?? 1);
  const normal = root.normal;
  const comb = normalize(region.comb);
  const parting = partingOffset(region, root);
  const bundle = bundleOf(region, root);

  // Initial direction: mostly the surface normal with the comb direction pulling it
  // along the scalp. Pure normal gives a hedgehog; pure comb gives flat hair with no
  // root lift.
  let direction = normalize(add(add(normal, mul(comb, 0.62)),
    [stream.span(-0.16, 0.16), stream.span(-0.06, 0.06), stream.span(-0.16, 0.16)]));
  if (parting) {
    // Lateral push in the character frame: +X is the character's left.
    direction = normalize(add(direction, [parting.side * 0.55 * parting.strength, -0.12 * parting.strength, 0]));
  }

  const points = [[root.point[0], root.point[1], root.point[2]]];
  const radii = [options.radiusMetres];
  const down = [0, -1, 0];
  const gravityWeight = options.gravity;
  let position = points[0];
  const stepLength = lengthMetres / segments;

  // Curl is applied as a rotating offset around the local tangent. `turnsPerMetre`
  // is a real frequency: 30 turns/m on a 15 cm fringe is 4.5 waves, which is what a
  // strong curl looks like, instead of an arbitrary per-segment sine.
  const curl = region.curl ?? null;
  const curlScale = override?.curlScale ?? 1;
  const curlPhase = stream.span(0, Math.PI * 2);
  let curlAxis = curl
    ? normalize(add(normalize(curl.axisBias ?? [0, 1, 0]), [stream.span(-0.25, 0.25), stream.span(-0.1, 0.1), stream.span(-0.25, 0.25)]))
    : null;

  for (let segment = 1; segment <= segments; ++segment) {
    const t = segment / segments;
    // Gravity grows towards the tip so the root keeps its lift.
    const pull = gravityWeight * t * t;
    direction = normalize(add(add(direction, mul(down, pull * 0.32)), mul(comb, 0.06 * (1 - t))));
    if (bundle) {
      // Bundled hair converges on the bundle spine: the free end is pulled towards a
      // shared axis, which is what distinguishes a braid from straight hair.
      const towardAnchor = normalize([Math.sin(bundle.anchor * DEG), -0.55, Math.cos(bundle.anchor * DEG)]);
      const convergence = bundle.kind === 'ponytail' ? 0.28 : bundle.kind === 'braid' ? 0.34 : 0.4;
      direction = normalize(add(direction, mul(towardAnchor, convergence * t)));
    }
    let next = add(position, mul(direction, stepLength));
    if (curl && curl.radiusMetres > 0) {
      // Rotate the offset direction by a fixed angle per step: dephased per guide so
      // neighbours do not curl in lockstep.
      const angle = curlPhase + t * lengthMetres * curl.turnsPerMetre * Math.PI * 2;
      const side = normalize(cross(direction, curlAxis));
      const up = normalize(cross(side, direction));
      const noise = 1 + (curl.noise ?? 0) * (stream.unit() - 0.5);
      const amplitude = curl.radiusMetres * t * noise * curlScale;
      next = add(next, add(mul(side, Math.cos(angle) * amplitude), mul(up, Math.sin(angle) * amplitude)));
    }
    points.push(next);
    position = next;
    const taper = region.taper ?? { root: 1, tip: 0.35, power: 1.4 };
    radii.push(options.radiusMetres * (taper.root + (taper.tip - taper.root) * Math.pow(t, taper.power)));
  }

  if (bundle && bundle.turnPerMetre > 0) {
    // Explicit braid twist: rotate every point after the first third around the
    // bundle axis. Without this a "braid" is a tapered tube, which is exactly the
    // approximation the plan says must not be shipped as braid support.
    const start = Math.floor(segments * 0.34);
    const axis = normalize([Math.sin(bundle.anchor * DEG), 0, Math.cos(bundle.anchor * DEG)]);
    for (let index = start; index < points.length; ++index) {
      const t = (index - start) / Math.max(1, points.length - start);
      const relative = sub(points[index], points[start]);
      const angle = t * (index - start) * stepLength * bundle.turnPerMetre * Math.PI * 2 / Math.max(1, segments);
      const projected = mul(axis, dot(relative, axis));
      const perpendicular = sub(relative, projected);
      const side = normalize(cross(axis, perpendicular[0] || perpendicular[1] || perpendicular[2]
        ? perpendicular : [1, 0, 0]));
      const rotated = add(mul(perpendicular, Math.cos(angle)), mul(side, Math.sin(angle)));
      points[index] = add(add(points[start], projected), rotated);
    }
  }

  if (override?.pointOffsets) {
    // Local curve edits are deltas in metres on individual control points. Applied
    // last so an edit composes with the procedural shape instead of fighting it, and
    // index 0 is refused because the root is bound to the scalp regardless of author
    // intent.
    const deltas = new Map(override.pointOffsets
      .filter((entry) => entry.index > 0 && entry.index < points.length)
      .map((entry) => [entry.index, entry.delta]));
    for (const [index, delta] of deltas) {
      points[index] = add(points[index], delta);
    }
  }

  return { points, radii, bundle, parting: Boolean(parting) };
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

// k-nearest guides in angular space, per region. Region-local on purpose: a strand in
// `bangs` must never interpolate from `back`, even when the two are close in 3D.
function neighbourTable(params, k = 4) {
  const table = params.map(() => []);
  for (let index = 0; index < params.length; ++index) {
    const distances = [];
    for (let other = 0; other < params.length; ++other) {
      if (other === index) continue;
      const dAzimuth = angularDistance(params[index][0], params[other][0]) * Math.cos(params[index][1] * DEG);
      const dPolar = params[index][1] - params[other][1];
      distances.push({ other, distance: Math.hypot(dAzimuth, dPolar) + other * 1e-9 });
    }
    distances.sort((a, b) => a.distance - b.distance);
    table[index] = distances.slice(0, k).map((entry) => entry.other);
  }
  return table;
}

export function generateHairstyle(input, character, { pose = {}, scalp = null, drape = true } = {}) {
  const spec = normalizeHairstyle(input);
  const surface = scalp ?? buildScalp(character, { headBone: spec.character.headBone });
  const segments = spec.guides.segments;
  const streamsPerGuide = spec.strands.streamsPerGuide;
  // Body proxies are derived once per generation. They are what stops a guide from
  // being grown straight through the character's chest — a hairstyle that starts
  // interpenetrating can only be rescued by a solver fighting its own initial state.
  const proxies = drape ? characterCollisionProxies(character, { pose }) : { spheres: [], capsules: [] };
  const guides = [];
  const regionReports = [];

  for (const region of spec.regions) {
    const area = regionArea(surface, region);
    fail(area > 0, 'NG-E-HAIR-SCALP',
      `region ${region.id} covers no scalp triangles (${region.azimuthDegrees.join('..')}° azimuth, `
      + `${region.polarDegrees.join('..')}° polar)`);
    const count = guideCountForRegion(surface, region, { streamsPerGuide, maximum: spec.guides.maxGuides });
    const seed = regionSeed(spec, region.id, 'guides');
    const roots = sampleRoots(surface, region, count, { seed });
    const overrides = new Map((spec.guideOverrides ?? [])
      .filter((entry) => entry.guide.startsWith(`${region.id}#`))
      .map((entry) => [entry.guide, entry]));
    const ids = new Set();
    let lengthSum = 0;
    roots.forEach((root, index) => {
      const stream = new Stream(mix32(seed ^ Math.imul(index + 1, 2246822519)), `guide:${region.id}`);
      const stableId = `${region.id}#${stableSuffix(root.triangle, root.bary)}`;
      const override = overrides.get(stableId) ?? null;
      const grown = growGuide(root, region, {
        segments, lengthMetres: region.lengthMetres,
        radiusMetres: spec.guides.radiusMetres, gravity: spec.physics.gravityMetresPerSecond2 / 9.81,
        override,
      }, stream);
      // The root is bound to the scalp and is never moved by collision resolution.
      grown.points = resolveProxyPenetration(grown.points, proxies, { marginMetres: 0.004 });
      validateCurve(grown.points, grown.radii, `${region.id} guide ${index}`);
      fail(!ids.has(stableId), 'NG-E-HAIR-IDENTITY',
        `region ${region.id} produced two guides with the stable id ${stableId}; increase density or reduce polar span`);
      ids.add(stableId);
      const total = arcLength(grown.points).total;
      lengthSum += total;
      guides.push({
        stableId, region: region.id, regionIndex: regionReportIndex(spec, region.id),
        triangle: root.triangle, bary: root.bary, rootPoint: root.point, rootNormal: root.normal,
        azimuthDegrees: root.azimuthDegrees, polarDegrees: root.polarDegrees,
        points: grown.points, radii: grown.radii, lengthMetres: total,
        bundle: grown.bundle, parting: grown.parting,
        material: region.material, simulation: region.simulation,
        edited: Boolean(override),
      });
    });
    regionReports.push({
      id: region.id, areaCm2: area * 10000, guides: roots.length,
      meanLengthMetres: lengthSum / Math.max(1, roots.length),
      densityPerCm2: region.densityPerCm2, lockedFields: region.lockedFields,
      bundle: region.bundle?.kind ?? 'none',
      parting: region.parting ? { azimuthDegrees: region.parting.azimuthDegrees, widthDegrees: region.parting.widthDegrees } : null,
    });
  }

  const maxGuides = spec.budget.maxGuides ?? spec.guides.maxGuides;
  fail(guides.length <= maxGuides, 'NG-E-HAIR-BUDGET',
    `generated ${guides.length} guides, the guide budget is ${maxGuides}`);

  // Per-region neighbour tables, then per-guide strand interpolation.
  const byRegion = new Map();
  for (const guide of guides) {
    const list = byRegion.get(guide.region) ?? [];
    list.push(guide);
    byRegion.set(guide.region, list);
  }
  const neighbours = new Map();
  for (const [regionId, list] of byRegion) {
    const params = list.map((guide) => [guide.azimuthDegrees, guide.polarDegrees]);
    const table = neighbourTable(params, 4);
    list.forEach((guide, index) => neighbours.set(guide.stableId, table[index].map((other) => list[other].stableId)));
  }
  const guideById = new Map(guides.map((guide) => [guide.stableId, guide]));

  const strandRecords = [];
  for (const [regionId, list] of byRegion) {
    const region = spec.regions.find((entry) => entry.id === regionId);
    const perGuide = streamsPerGuide;
    const seed = regionSeed(spec, regionId, 'strands');
    for (let guideIndex = 0; guideIndex < list.length; ++guideIndex) {
      const guide = list[guideIndex];
      for (let streamIndex = 0; streamIndex < perGuide; ++streamIndex) {
        const stream = new Stream(mix32(seed ^ Math.imul(guideIndex * 64 + streamIndex + 1, 2246822519)), `strand:${regionId}`);
        const candidates = [guide.stableId, ...neighbours.get(guide.stableId)];
        const weights = [1];
        let total = 1;
        for (let index = 1; index < candidates.length; ++index) {
          const weight = 1 / (1 + index * 1.7);
          weights.push(weight);
          total += weight;
        }
        const normalised = weights.map((weight) => weight / total);
        const blend = candidates.map((id, index) => ({ guide: id, weight: normalised[index] }));
        strandRecords.push({
          stableId: `${guide.stableId}.${streamIndex}`,
          region: regionId,
          guide: guide.stableId,
          blend,
          baseIndex: strandRecords.length,
          // Every random value this strand needs is drawn once, here. Consuming a
          // stream inside the LOD builder would make a coarse level draw different
          // numbers than the fine level did, and the same strand would move 3 mm
          // sideways when the LOD switched.
          rootOffset: [stream.span(-spec.strands.rootJitterMetres, spec.strands.rootJitterMetres),
            stream.span(-spec.strands.rootJitterMetres, spec.strands.rootJitterMetres) * 0.4,
            stream.span(-spec.strands.rootJitterMetres, spec.strands.rootJitterMetres)],
          frizzPhase: stream.span(0, Math.PI * 2),
          region,
        });
      }
    }
  }

  fail(strandRecords.length <= spec.budget.maxStrands, 'NG-E-HAIR-BUDGET',
    `generated ${strandRecords.length} strands, budget.maxStrands is ${spec.budget.maxStrands}`);

  const levels = spec.lod.map((lod) => {
    const segmentCount = Math.max(3, Math.round(segments * lod.segmentScale));
    const keep = Math.round(strandRecords.length * lod.strandFraction);
    const step = strandRecords.length / Math.max(1, keep);
    const curves = [];
    for (let index = 0; index < keep; ++index) {
      // LOD selection is a deterministic prefix-free thinning of the same list: the
      // same strand survives at every level that keeps it, so a LOD change cannot
      // re-shuffle which lock of hair is where.
      const sourceIndex = Math.min(strandRecords.length - 1, Math.floor(index * step));
      const source = strandRecords[sourceIndex];
      const curve = buildStrand(source, guideById, segmentCount, spec);
      curve.sourceIndex = sourceIndex;
      curves.push(curve);
    }
    return {
      level: lod.level, segments: segmentCount, maxDistanceMetres: lod.maxDistanceMetres,
      strandFraction: lod.strandFraction, curves,
      points: curves.length * (segmentCount + 1),
    };
  });

  for (const level of levels) {
    fail(level.points <= spec.budget.maxPoints, 'NG-E-HAIR-BUDGET',
      `LOD ${level.level} needs ${level.points} points, budget.maxPoints is ${spec.budget.maxPoints}`);
  }

  const bounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
  for (const curve of levels[0].curves) {
    for (const point of curve.points) {
      for (let axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = Math.min(bounds.min[axis], point[axis]);
        bounds.max[axis] = Math.max(bounds.max[axis], point[axis]);
      }
    }
  }

  return {
    spec, scalp: surface, scalpHash: scalpHash(surface),
    guides, regions: regionReports, levels, bounds,
    stats: {
      guides: guides.length,
      strands: levels[0].curves.length,
      strandPoints: levels[0].points,
      guidePoints: guides.length * (segments + 1),
      segments,
      regions: regionReports.length,
      lod: levels.map((level) => ({
        level: level.level, strands: level.curves.length, segments: level.segments,
        points: level.points, maxDistanceMetres: level.maxDistanceMetres,
      })),
    },
    pose,
  };
}

function regionReportIndex(spec, id) {
  return spec.regions.findIndex((region) => region.id === id);
}

// One render strand from its guide blend. Frizz grows towards the tip and is applied
// perpendicular to the local tangent so it reads as stray hair, not as noise in 3D.
// Pure with respect to the strand record: no random numbers are drawn here, so the
// same strand builds identically at every LOD and in every re-generation.
function buildStrand(source, guideById, segments, spec) {
  const resampled = source.blend.map((entry) => {
    const guide = guideById.get(entry.guide);
    return { weight: entry.weight, ...resampleCurve(guide.points, guide.radii, segments + 1) };
  });
  const base = guideById.get(source.guide);
  const root = add(base.rootPoint, source.rootOffset);
  const frizzAmplitude = source.region.frizz?.amplitudeMetres ?? spec.strands.frizzMetres ?? 0;
  const frizzFrequency = source.region.frizz?.frequency ?? 9;
  const frizzPhase = source.frizzPhase;
  const points = [], radii = [];
  for (let index = 0; index <= segments; ++index) {
    const t = index / segments;
    let position = [0, 0, 0], radius = 0;
    for (const entry of resampled) {
      position = add(position, mul(entry.points[index], entry.weight));
      radius += entry.radii[index] * entry.weight;
    }
    if (frizzAmplitude > 0) {
      const a = resampled[0].points[Math.max(0, index - 1)];
      const b = resampled[0].points[Math.min(segments, index + 1)];
      const tangent = normalize(sub(b, a));
      const side = normalize(cross(tangent, [0, 1, 0.35]));
      const amplitude = frizzAmplitude * t * t;
      position = add(position, mul(side, Math.sin(frizzPhase + t * frizzFrequency * Math.PI) * amplitude));
    }
    if (index === 0) position = root;
    points.push(position);
    radii.push(Math.max(2e-5, radius * (1 - 0.45 * t) + spec.strands.widthMetres * (1 - t)));
  }
  validateCurve(points, radii, source.stableId);
  return {
    stableId: source.stableId, region: source.region, guide: source.guide,
    blend: source.blend, points, radii,
    lengthMetres: arcLength(points).total,
  };
}

// Applies a pose to the generated roots and reports how far each root moved. This is
// the "dynamic character binding" check for a hairstyle that was generated at rest:
// roots must follow the skull, tips must not.
export function rootTracking(character, generated, { pose = {} } = {}) {
  const scalp = generated.scalp;
  const palette = skinPalette(character.bones, poseSkeleton(character.bones, pose));
  const moved = generated.guides.map((guide) => {
    const after = resolveBoundPoint(character, palette, { triangle: guide.triangle, bary: guide.bary }, scalp, guide.rootPoint);
    return {
      stableId: guide.stableId,
      distanceMetres: Math.hypot(
        after[0] - guide.rootPoint[0], after[1] - guide.rootPoint[1], after[2] - guide.rootPoint[2]),
    };
  });
  const distances = moved.map((entry) => entry.distanceMetres);
  return { roots: moved.length, maximumMetres: Math.max(...distances), minimumMetres: Math.min(...distances) };
}

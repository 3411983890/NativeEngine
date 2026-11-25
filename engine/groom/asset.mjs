import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { buildPackage, digestPackage, packRecords, SectionKind } from '../assets/binary-package.mjs';
import { Stream, deriveSeed } from '../vegetation/random.mjs';
import {
  arcLength, arcParameters, boundsOf, GroomError, normalize, resampleCurve, tangentAt, validateCurve,
  add, sub, mul, dot, length,
} from './curves.mjs';
import {
  CURVE_HEADER_LAYOUT, BINDING_LAYOUT, CURVE_ATTRIBUTE_LAYOUT, GUIDE_WEIGHT_LAYOUT, CARD_LAYOUT,
} from './layouts.mjs';
import { compileExplicitGroom, GROOM_EXPLICIT_SCHEMA } from './explicit.mjs';

// Groom cook.
//
// One source of truth per hairstyle: guides are simulated, strands are interpolated
// from guides at render time, and cards are generated from the same strands. That is
// the property the plan asks for — strands, cards and the far mesh must describe the
// same hair, or the LOD transition changes the hairstyle.

export const GROOM_SCHEMA = 'nativegpu.groom/1';
export const GROOM_COOK_SCHEMA = 'nativegpu.groom-cook/1';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;

export { GroomError };
const fail = (ok, message, code = 'NG-E-GROOM-FORMAT') => { if (!ok) throw new GroomError(code, message); };

import { SCALP_SECTION, GUIDE_SECTION } from './layouts.mjs';

function object(value, label) {
  fail(value && typeof value === 'object' && !Array.isArray(value)
    && (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null), `${label} must be an object`);
}
function keys(value, required, optional, label) {
  object(value, label);
  for (const key of required) fail(Object.hasOwn(value, key), `${label}.${key} is required`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) fail(allowed.has(key), `${label}.${key} is unsupported`);
}
function integer(value, low, high, label) {
  fail(Number.isSafeInteger(value) && value >= low && value <= high, `${label} must be an integer in [${low},${high}]`);
}
function scalar(value, low, high, label) {
  fail(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high, `${label} must be finite and in [${low},${high}]`);
}

// ---------------------------------------------------------------- scalp geometry
// A spherical cap stands in for a real head until a character asset provides one. It
// has the properties the binding needs: an explicit triangle list, a UV parameter
// space and an outward normal at every vertex.
export function buildScalp({ radiusMetres, coverageRadians, rings, segments }) {
  const positions = [], normals = [], uvs = [], indices = [];
  for (let ring = 0; ring <= rings; ++ring) {
    const polar = (ring / rings) * coverageRadians;
    const y = Math.cos(polar) * radiusMetres, r = Math.sin(polar) * radiusMetres;
    for (let segment = 0; segment <= segments; ++segment) {
      const azimuth = (segment / segments) * Math.PI * 2;
      const x = Math.cos(azimuth) * r, z = Math.sin(azimuth) * r;
      positions.push(x, y, z);
      normals.push(x / radiusMetres, y / radiusMetres, z / radiusMetres);
      // v = 1 at the crown, so a root UV of (0.5, 1) is the top of the head.
      uvs.push(segment / segments, 1 - ring / rings);
    }
  }
  const stride = segments + 1;
  for (let ring = 0; ring < rings; ++ring) for (let segment = 0; segment < segments; ++segment) {
    const a = ring * stride + segment, b = a + 1, c = a + stride, d = c + 1;
    indices.push(a, c, b, b, c, d);
  }
  return { positions, normals, uvs, indices, radiusMetres, coverageRadians };
}

function closestPointOnTriangle(p, a, b, c) {
  const ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return { point: a, bary: [1, 0, 0] };
  const bp = sub(p, b), d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return { point: b, bary: [0, 1, 0] };
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) { const v = d1 / (d1 - d3); return { point: add(a, mul(ab, v)), bary: [1 - v, v, 0] }; }
  const cp = sub(p, c), d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return { point: c, bary: [0, 0, 1] };
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) { const w = d2 / (d2 - d6); return { point: add(a, mul(ac, w)), bary: [1 - w, 0, w] }; }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return { point: add(b, mul(sub(c, b), w)), bary: [0, 1 - w, w] };
  }
  const denominator = 1 / (va + vb + vc);
  const v = vb * denominator, w = vc * denominator;
  return { point: add(add(a, mul(ab, v)), mul(ac, w)), bary: [1 - v - w, v, w] };
}

export function bindToScalp(scalp, point) {
  let best = { distance: Infinity, triangle: -1, bary: [1, 0, 0], point: null };
  for (let triangle = 0; triangle < scalp.indices.length / 3; ++triangle) {
    const i0 = scalp.indices[triangle * 3], i1 = scalp.indices[triangle * 3 + 1], i2 = scalp.indices[triangle * 3 + 2];
    const a = [scalp.positions[i0 * 3], scalp.positions[i0 * 3 + 1], scalp.positions[i0 * 3 + 2]];
    const b = [scalp.positions[i1 * 3], scalp.positions[i1 * 3 + 1], scalp.positions[i1 * 3 + 2]];
    const c = [scalp.positions[i2 * 3], scalp.positions[i2 * 3 + 1], scalp.positions[i2 * 3 + 2]];
    const hit = closestPointOnTriangle(point, a, b, c);
    const distance = length(sub(point, hit.point));
    if (distance < best.distance) best = { distance, triangle, bary: hit.bary, point: hit.point };
  }
  return best;
}

// --------------------------------------------------------------------- guides
function growGuide(root, normal, rng, options) {
  const { segments, lengthMetres, comb, gravity, curl } = options;
  const points = [root], radii = [options.rootRadius];
  let direction = normalize(add(add(normal, mul(comb, 0.55)), [rng.span(-0.18, 0.18), 0, rng.span(-0.18, 0.18)]));
  let position = root;
  for (let segment = 1; segment <= segments; ++segment) {
    const t = segment / segments;
    // Gravity bends the tip down; curl adds a helical offset that is stable per guide.
    direction = normalize(add(direction, add(mul([0, -1, 0], gravity * t), [Math.cos(curl) * 0.05 * t, 0, Math.sin(curl) * 0.05 * t])));
    position = add(position, mul(direction, lengthMetres / segments));
    points.push(position);
    radii.push(options.rootRadius * (1 - 0.72 * t));
  }
  return { points, radii };
}

function sampleScalpRoots(scalp, count, rng) {
  // Stratified golden-angle sampling over the cap: even coverage without a hash set,
  // and the same root positions every time for the same count.
  const roots = [];
  const golden = Math.PI * (3 - Math.sqrt(5));
  for (let index = 0; index < count; ++index) {
    const t = (index + 0.5) / count;
    const polar = Math.acos(1 - t * (1 - Math.cos(scalp.coverageRadians)));
    const azimuth = index * golden + rng.span(-0.05, 0.05);
    const y = Math.cos(polar) * scalp.radiusMetres, r = Math.sin(polar) * scalp.radiusMetres;
    const point = [Math.cos(azimuth) * r, y, Math.sin(azimuth) * r];
    roots.push({ point, normal: normalize(point) });
  }
  return roots;
}

// -------------------------------------------------------------------- validation
function validateGroom(source) {
  keys(source, ['schema', 'id', 'style', 'seed', 'scalp', 'groups', 'guides', 'strands', 'lod', 'budget'], [], 'groom');
  fail(source.schema === GROOM_SCHEMA, `unsupported groom schema ${source.schema}`);
  fail(typeof source.id === 'string' && ID.test(source.id), 'groom.id is not a valid identifier');
  fail(['scalp', 'brows', 'lashes', 'beard', 'fur'].includes(source.style), 'groom.style must be scalp, brows, lashes, beard or fur');
  integer(source.seed, 0, 0xFFFFFFFF, 'groom.seed');
  keys(source.scalp, ['radiusMetres', 'coverageDegrees', 'rings', 'segments'], [], 'scalp');
  scalar(source.scalp.radiusMetres, 0.02, 1, 'scalp.radiusMetres');
  scalar(source.scalp.coverageDegrees, 10, 170, 'scalp.coverageDegrees');
  integer(source.scalp.rings, 2, 64, 'scalp.rings');
  integer(source.scalp.segments, 4, 128, 'scalp.segments');
  fail(Array.isArray(source.groups) && source.groups.length >= 1 && source.groups.length <= 8, 'groom.groups needs 1–8 entries');
  const groupIds = new Set();
  for (const group of source.groups) {
    keys(group, ['id', 'material', 'simulation', 'shadow'], [], 'group');
    fail(typeof group.id === 'string' && ID.test(group.id), 'group.id is not a valid identifier');
    fail(!groupIds.has(group.id), `duplicate group id ${group.id}`); groupIds.add(group.id);
    fail(['guide', 'rigid'].includes(group.simulation), 'group.simulation must be guide or rigid');
    fail(['deepOpacity', 'none'].includes(group.shadow), 'group.shadow must be deepOpacity or none');
    keys(group.material, ['albedo', 'roughness', 'radialRoughness', 'longitudinalRoughness', 'absorption', 'melanin', 'redness'], [], 'group.material');
    for (const key of ['albedo', 'absorption']) {
      fail(Array.isArray(group.material[key]) && group.material[key].length === 3, `group.material.${key} needs 3 values`);
      group.material[key].forEach((value) => scalar(value, 0, 1, `group.material.${key}`));
    }
    scalar(group.material.roughness, 0.02, 1, 'group.material.roughness');
    scalar(group.material.radialRoughness, 0.02, 1, 'group.material.radialRoughness');
    scalar(group.material.longitudinalRoughness, 0.02, 1, 'group.material.longitudinalRoughness');
    scalar(group.material.melanin, 0, 1, 'group.material.melanin');
    scalar(group.material.redness, 0, 1, 'group.material.redness');
  }
  keys(source.guides, ['count', 'segments', 'lengthMetres', 'rootRadiusMetres', 'gravity', 'comb'], [], 'guides');
  integer(source.guides.count, 4, 8192, 'guides.count');
  integer(source.guides.segments, 2, 64, 'guides.segments');
  scalar(source.guides.lengthMetres, 0.01, 2, 'guides.lengthMetres');
  scalar(source.guides.rootRadiusMetres, 0.00002, 0.01, 'guides.rootRadiusMetres');
  scalar(source.guides.gravity, 0, 3, 'guides.gravity');
  fail(Array.isArray(source.guides.comb) && source.guides.comb.length === 3, 'guides.comb needs 3 values');
  source.guides.comb.forEach((value) => scalar(value, -1, 1, 'guides.comb'));
  keys(source.strands, ['count', 'frizzMetres', 'widthMetres', 'rootJitterMetres'], [], 'strands');
  integer(source.strands.count, 16, 400000, 'strands.count');
  scalar(source.strands.frizzMetres, 0, 0.05, 'strands.frizzMetres');
  scalar(source.strands.widthMetres, 0.00002, 0.01, 'strands.widthMetres');
  scalar(source.strands.rootJitterMetres, 0, 0.01, 'strands.rootJitterMetres');
  fail(Array.isArray(source.lod) && source.lod.length >= 1 && source.lod.length <= 4, 'groom.lod needs 1–4 entries');
  let previousStrands = Infinity;
  source.lod.forEach((entry, index) => {
    keys(entry, ['level', 'strandFraction', 'segmentScale', 'maxDistanceMetres'], [], `lod[${index}]`);
    fail(entry.level === index, 'groom.lod levels must be ordered from 0');
    scalar(entry.strandFraction, 0.02, 1, `lod[${index}].strandFraction`);
    scalar(entry.segmentScale, 0.2, 1, `lod[${index}].segmentScale`);
    scalar(entry.maxDistanceMetres, 0.1, 1000, `lod[${index}].maxDistanceMetres`);
    fail(entry.strandFraction <= 1 && previousStrands >= entry.strandFraction, 'lod strand fractions must not increase');
    previousStrands = entry.strandFraction;
    if (index === 0) fail(entry.strandFraction === 1 && entry.segmentScale === 1, 'lod0 must be the full strand set');
  });
  keys(source.budget, ['maxStrands', 'maxPoints'], [], 'budget');
  integer(source.budget.maxStrands, 16, 400000, 'budget.maxStrands');
  integer(source.budget.maxPoints, 64, 40000000, 'budget.maxPoints');
  fail(source.strands.count <= source.budget.maxStrands, 'groom exceeds budget.maxStrands');
  fail(source.strands.count * (source.guides.segments + 1) <= source.budget.maxPoints, 'groom exceeds budget.maxPoints');
}

// ------------------------------------------------------------------------ cook
export function compileGroom(source) {
  // Two authoring forms share one package format: a procedural source that grows guides from
  // a cap, and an explicit source that carries the guides, strands, weights and scalp mesh a
  // hairstyle already produced. The package bytes are identical in shape, so the runtime does
  // not care which one wrote them.
  if (source?.schema === GROOM_EXPLICIT_SCHEMA) return compileExplicitGroom(source);
  validateGroom(source);
  const rng = new Stream(deriveSeed(source.seed, 'groom'), 'groom');
  const scalp = buildScalp({
    radiusMetres: source.scalp.radiusMetres,
    coverageRadians: (source.scalp.coverageDegrees * Math.PI) / 180,
    rings: source.scalp.rings, segments: source.scalp.segments,
  });
  const roots = sampleScalpRoots(scalp, source.guides.count, rng);
  const comb = normalize(source.guides.comb);
  const guides = roots.map((root, index) => {
    const guideRng = new Stream(deriveSeed(source.seed ^ (index * 2654435761), 'guide'), 'guide');
    const grown = growGuide(root.point, root.normal, guideRng, {
      segments: source.guides.segments, lengthMetres: source.guides.lengthMetres,
      comb, gravity: source.guides.gravity, curl: guideRng.span(0, Math.PI * 2),
      rootRadius: source.guides.rootRadiusMetres,
    });
    validateCurve(grown.points, grown.radii, `guide${index}`);
    const binding = bindToScalp(scalp, root.point);
    return { ...grown, root, binding, index, group: 0 };
  });

  // Guide selection for interpolation happens in root-UV space, not in 3D: two guides
  // that are close in space but on opposite sides of a parting must not be blended.
  const guideUv = guides.map((guide) => {
    const polar = Math.acos(Math.max(-1, Math.min(1, guide.root.normal[1])));
    let azimuth = Math.atan2(guide.root.normal[2], guide.root.normal[0]);
    if (azimuth < 0) azimuth += Math.PI * 2;
    return [azimuth / (Math.PI * 2), 1 - polar / scalp.coverageRadians];
  });

  const levels = [];
  for (const lod of source.lod) {
    const strandCount = Math.max(16, Math.round(source.strands.count * lod.strandFraction));
    const segments = Math.max(2, Math.round(source.guides.segments * lod.segmentScale));
    const curves = [], weights = [];
    for (let strand = 0; strand < strandCount; ++strand) {
      // The per-strand stream is seeded by the strand's global index, not by the LOD.
      // That is what makes a coarser LOD a subset of the finer one: the same strand
      // keeps the same root, the same frizz phase and the same guide blend, and only
      // its point count changes. A level-dependent stream would produce a different
      // hairstyle at every LOD and make the transition a visible re-shuffle.
      const strandRng = new Stream(deriveSeed(source.seed ^ Math.imul(strand + 1, 2246822519), 'strand'), 'strand');
      const base = Math.floor((strand / source.strands.count) * guides.length) % guides.length;
      const guide = guides[base];
      const jitter = source.strands.rootJitterMetres;
      const root = add(guide.root.point, [strandRng.span(-jitter, jitter), strandRng.span(-jitter, jitter) * 0.4, strandRng.span(-jitter, jitter)]);
      // Three nearest guides in UV; the weights are inverse-square and normalised.
      const distances = guideUv.map((uv, index) => ({ index, d: Math.hypot(uv[0] - guideUv[base][0], uv[1] - guideUv[base][1]) + index * 1e-6 }));
      distances.sort((a, b) => a.d - b.d);
      const picked = distances.slice(0, 3);
      const raw = picked.map((entry) => 1 / Math.max(1e-4, entry.d * entry.d));
      const sum = raw.reduce((total, value) => total + value, 0);
      const blend = picked.map((entry, index) => ({ guide: entry.index, weight: raw[index] / sum }));
      const resampledGuides = blend.map((entry) => resampleCurve(guides[entry.guide].points, guides[entry.guide].radii, segments + 1));
      const points = [], radii = [];
      const frizzPhase = strandRng.span(0, Math.PI * 2);
      for (let point = 0; point <= segments; ++point) {
        const t = point / segments;
        let position = [0, 0, 0], radius = 0;
        for (let index = 0; index < blend.length; ++index) {
          const source0 = resampledGuides[index];
          position = add(position, mul(source0.points[point], blend[index].weight));
          radius += source0.radii[point] * blend[index].weight;
        }
        // Frizz grows towards the tip and is perpendicular to the local tangent.
        const tangent = tangentAt(resampledGuides[0].points, point);
        const side = normalize([tangent[1], -tangent[0], tangent[2] * 0.5]);
        const amplitude = source.strands.frizzMetres * t * t;
        position = add(position, mul(side, Math.sin(frizzPhase + t * 9.0) * amplitude));
        if (point === 0) position = add(position, sub(root, position));
        points.push(position);
        radii.push(radius * (1 - 0.55 * t) + source.strands.widthMetres * (1 - t));
      }
      validateCurve(points, radii, `strand${strand}`);
      curves.push({ points, radii, group: 0, stableId: strand });
      weights.push({ blend, strand });
    }
    levels.push({ level: lod.level, segments, curves, weights, maxDistanceMetres: lod.maxDistanceMetres, strandFraction: lod.strandFraction });
  }

  // Cards are merged ribbons: a card references a contiguous run of strands and the
  // coverage it stands for. The renderer expands a card into `curveCount` ribbons and
  // widens them, so a card is a real geometric representation, not a texture the cook
  // never produced.
  const cardSize = 8;
  const cardsByLevel = levels.map((level) => {
    const cards = [];
    for (let start = 0; start < level.curves.length; start += cardSize) {
      const count = Math.min(cardSize, level.curves.length - start);
      const width = source.strands.widthMetres * cardSize * 0.75;
      cards.push({ firstCurve: start, curveCount: count, widthMetres: width, coverage: count / cardSize, layer: 0, material: 0, flags: 0, reserved: 0 });
    }
    return cards;
  });

  const bounds = boundsOf(levels[0].curves);
  const totalPoints = levels.reduce((sum, level) => sum + level.curves.length * (level.segments + 1), 0);
  const guidePoints = guides.length * (source.guides.segments + 1);

  const sections = [];
  sections.push({ kind: SectionKind.POSITION_F32X3, index: SCALP_SECTION, elements: Float32Array.from(scalp.positions) });
  sections.push({ kind: SectionKind.NORMAL_F32X3, index: SCALP_SECTION, elements: Float32Array.from(scalp.normals) });
  sections.push({ kind: SectionKind.UV_F32X2, index: SCALP_SECTION, elements: Float32Array.from(scalp.uvs) });
  sections.push({ kind: SectionKind.INDEX_U32, index: SCALP_SECTION, elements: Uint32Array.from(scalp.indices) });

  // Guides: one header per guide plus a flat point/radius pool.
  const guideHeaders = [], guidePointsFlat = [], guideRadiiFlat = [], guideBindings = [];
  let cursor = 0;
  for (const guide of guides) {
    guideHeaders.push({ firstPoint: cursor, pointCount: guide.points.length, group: 0, flags: 0 });
    for (let index = 0; index < guide.points.length; ++index) {
      guidePointsFlat.push(guide.points[index][0], guide.points[index][1], guide.points[index][2]);
      guideRadiiFlat.push(guide.radii[index]);
    }
    cursor += guide.points.length;
    guideBindings.push({
      triangle: guide.binding.triangle,
      bary0: guide.binding.bary[0], bary1: guide.binding.bary[1], bary2: guide.binding.bary[2],
      rootU: guideUv[guide.index][0], rootV: guideUv[guide.index][1],
      offsetMetres: guide.binding.distance, flags: 0,
    });
  }
  sections.push({ kind: SectionKind.CURVE_HEADER, index: GUIDE_SECTION, data: packRecords(guideHeaders, CURVE_HEADER_LAYOUT).buffer });
  sections.push({ kind: SectionKind.CURVE_POINT_F32X3, index: GUIDE_SECTION, elements: Float32Array.from(guidePointsFlat) });
  sections.push({ kind: SectionKind.CURVE_RADIUS_F32, index: GUIDE_SECTION, elements: Float32Array.from(guideRadiiFlat) });
  sections.push({ kind: SectionKind.BINDING, index: GUIDE_SECTION, data: packRecords(guideBindings, BINDING_LAYOUT).buffer });

  const lodMeta = [];
  for (const level of levels) {
    const headers = [], pointsFlat = [], radiiFlat = [], attributes = [], weights = [];
    let pointCursor = 0;
    level.curves.forEach((curve, index) => {
      headers.push({ firstPoint: pointCursor, pointCount: curve.points.length, group: curve.group, flags: 0 });
      for (let point = 0; point < curve.points.length; ++point) {
        pointsFlat.push(curve.points[point][0], curve.points[point][1], curve.points[point][2]);
        radiiFlat.push(curve.radii[point]);
      }
      const parameters = arcParameters(curve.points);
      attributes.push({ arcLength: arcLength(curve.points).total, rootToTip: 1, group: curve.group, stableId: curve.stableId });
      const blend = level.weights[index].blend;
      weights.push({
        guide0: blend[0].guide, guide1: blend[1].guide, guide2: blend[2].guide, strand: index,
        weight0: blend[0].weight, weight1: blend[1].weight, weight2: blend[2].weight, reserved: 0,
      });
      pointCursor += curve.points.length;
      void parameters;
    });
    sections.push({ kind: SectionKind.CURVE_HEADER, index: level.level, data: packRecords(headers, CURVE_HEADER_LAYOUT).buffer });
    sections.push({ kind: SectionKind.CURVE_POINT_F32X3, index: level.level, elements: Float32Array.from(pointsFlat) });
    sections.push({ kind: SectionKind.CURVE_RADIUS_F32, index: level.level, elements: Float32Array.from(radiiFlat) });
    sections.push({ kind: SectionKind.CURVE_ATTRIBUTE, index: level.level, data: packRecords(attributes, CURVE_ATTRIBUTE_LAYOUT).buffer });
    sections.push({ kind: SectionKind.GUIDE_WEIGHT, index: level.level, data: packRecords(weights, GUIDE_WEIGHT_LAYOUT).buffer });
    sections.push({ kind: SectionKind.CARD, index: level.level, data: packRecords(cardsByLevel[level.level], CARD_LAYOUT).buffer });
    lodMeta.push({
      level: level.level, strands: level.curves.length, segments: level.segments,
      points: level.curves.length * (level.segments + 1),
      cards: cardsByLevel[level.level].length,
      maxDistanceMetres: level.maxDistanceMetres,
      strandFraction: level.strandFraction,
    });
  }

  const meta = {
    schema: GROOM_COOK_SCHEMA,
    id: source.id, style: source.style, seed: source.seed,
    units: 'metres',
    sourceDigest: sha256Hex(canonicalJson(source)),
    representations: ['strands', 'cards', 'guides'],
    scalp: {
      kind: 'sphereCap', radiusMetres: scalp.radiusMetres, coverageRadians: scalp.coverageRadians,
      vertices: scalp.positions.length / 3, triangles: scalp.indices.length / 3,
      version: `sphereCap/${scalp.radiusMetres}/${scalp.coverageRadians}`,
    },
    groups: source.groups.map((group, index) => ({ ...group, index })),
    counts: {
      guides: guides.length, guidePoints,
      strands: levels[0].curves.length, strandPoints: totalPoints,
      cards: cardsByLevel[0].length, groups: source.groups.length,
    },
    lod: lodMeta,
    bounds,
    lengthMetres: arcLength(levels[0].curves[0].points).total,
    widthMetres: source.strands.widthMetres,
    cardModel: 'mergedRibbon',
    // Collision proxies are authored, not derived: a hair solver that invents a head
    // shape will collide against the wrong one.
    collision: { spheres: [], capsules: [] },
  };
  const bytes = buildPackage({ schema: GROOM_COOK_SCHEMA, meta, sections });
  return {
    id: source.id, meta, bytes, hash: digestPackage(bytes),
    guides: guides.length, strands: levels[0].curves.length,
    segments: source.guides.segments, levels: lodMeta,
    bytesTotal: bytes.byteLength,
  };
}

export {
  CURVE_HEADER_LAYOUT, BINDING_LAYOUT, CURVE_ATTRIBUTE_LAYOUT, GUIDE_WEIGHT_LAYOUT, CARD_LAYOUT,
} from './layouts.mjs';

export const SCALP_SECTION_INDEX = SCALP_SECTION;
export const GUIDE_SECTION_INDEX = GUIDE_SECTION;

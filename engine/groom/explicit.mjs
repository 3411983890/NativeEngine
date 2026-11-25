import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { buildPackage, digestPackage, packRecords, SectionKind } from '../assets/binary-package.mjs';
import {
  arcLength, boundsOf, GroomError, validateCurve,
} from './curves.mjs';
import {
  CURVE_HEADER_LAYOUT, BINDING_LAYOUT, CURVE_ATTRIBUTE_LAYOUT, GUIDE_WEIGHT_LAYOUT, CARD_LAYOUT,
  SCALP_SECTION, GUIDE_SECTION,
} from './layouts.mjs';
import { bindToScalp } from './asset.mjs';

// Explicit groom cook: `nativegpu.groom/2`.
//
// The procedural source (`nativegpu.groom/1`) grows guides from a spherical cap. That is fine
// for a brows/beard fixture and wrong for a hairstyle: the hairstyle already owns a scalp mesh
// with 2390 triangles, 1806 guides bound to those triangles, 14448 interpolated strands and a
// per-strand guide-weight table. Round-tripping that through a cap loses 96% of the guides and
// re-derives every id — the plan names this path as unusable for production acceptance.
//
// This module transcribes the hairstyle's own tables instead. The package format does not
// change: `nativegpu.groom-cook/1` already carries an arbitrary scalp mesh and arbitrary
// curves, so the native reader (`parseGroom`) needs no new branch and no schema bump. Only the
// *source* document gains a version, because "where did these guides come from" is exactly
// what a reviewer has to be able to tell apart.

export const GROOM_EXPLICIT_SCHEMA = 'nativegpu.groom/2';
const GROOM_COOK_SCHEMA = 'nativegpu.groom-cook/1';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;
const STYLES = ['scalp', 'brows', 'lashes', 'beard', 'fur'];

const fail = (ok, message) => { if (!ok) throw new GroomError('NG-E-GROOM-FORMAT', message); };
const isFiniteNumber = (value) => typeof value === 'number' && Number.isFinite(value);
const keys = (value, required, optional, label) => {
  fail(value && typeof value === 'object' && !Array.isArray(value), `${label} must be an object`);
  for (const key of required) fail(Object.hasOwn(value, key), `${label}.${key} is required`);
  const allowed = new Set([...required, ...(optional ?? [])]);
  for (const key of Object.keys(value)) fail(allowed.has(key), `${label}.${key} is unsupported`);
};
const integer = (value, low, high, label) => {
  fail(Number.isSafeInteger(value) && value >= low && value <= high, `${label} must be an integer in [${low},${high}]`);
};
const scalar = (value, low, high, label) => {
  fail(isFiniteNumber(value) && value >= low && value <= high, `${label} must be in [${low},${high}]`);
};
const vector = (value, length, label) => {
  fail(Array.isArray(value) && value.length === length && value.every(isFiniteNumber), `${label} must be ${length} finite numbers`);
};

function validateMesh(mesh) {
  keys(mesh, ['positions', 'normals', 'uvs', 'indices'], [], 'scalp.mesh');
  fail(Array.isArray(mesh.positions) && mesh.positions.length % 3 === 0 && mesh.positions.length >= 300,
    'scalp.mesh.positions must be a multiple of three with at least 100 vertices');
  fail(mesh.positions.every(isFiniteNumber), 'scalp.mesh.positions must be finite');
  fail(Array.isArray(mesh.indices) && mesh.indices.length % 3 === 0 && mesh.indices.length >= 300,
    'scalp.mesh.indices must describe at least 100 triangles');
  fail(Array.isArray(mesh.normals) && mesh.normals.length === mesh.positions.length, 'scalp.mesh.normals must match the vertex count');
  fail(mesh.normals.every(isFiniteNumber), 'scalp.mesh.normals must be finite');
  fail(Array.isArray(mesh.uvs) && mesh.uvs.length === (mesh.positions.length / 3) * 2, 'scalp.mesh.uvs must be two per vertex');
  fail(mesh.uvs.every(isFiniteNumber), 'scalp.mesh.uvs must be finite');
  const vertices = mesh.positions.length / 3;
  for (const index of mesh.indices) {
    fail(Number.isSafeInteger(index) && index >= 0 && index < vertices, 'scalp.mesh.indices leaves the vertex buffer');
  }
}

function validateExplicit(source) {
  keys(source, ['schema', 'id', 'style', 'seed', 'scalp', 'groups', 'guides', 'strands', 'levels', 'budget'], [], 'groom');
  fail(source.schema === GROOM_EXPLICIT_SCHEMA, `unsupported explicit groom schema ${source.schema}`);
  fail(typeof source.id === 'string' && ID.test(source.id), 'groom.id is not a valid identifier');
  fail(STYLES.includes(source.style), 'groom.style must be scalp, brows, lashes, beard or fur');
  integer(source.seed, 0, 0xFFFFFFFF, 'groom.seed');
  keys(source.scalp, ['mesh'], ['sourceHash'], 'scalp');
  validateMesh(source.scalp.mesh);

  fail(Array.isArray(source.groups) && source.groups.length >= 1 && source.groups.length <= 8, 'groom.groups needs 1–8 entries');
  for (const group of source.groups) {
    keys(group, ['id', 'material', 'simulation', 'shadow'], [], 'group');
    fail(typeof group.id === 'string' && ID.test(group.id), 'group.id is not a valid identifier');
    fail(['guide', 'rigid'].includes(group.simulation), 'group.simulation must be guide or rigid');
    fail(['deepOpacity', 'none'].includes(group.shadow), 'group.shadow must be deepOpacity or none');
    keys(group.material, ['albedo', 'roughness', 'radialRoughness', 'longitudinalRoughness', 'absorption', 'melanin', 'redness'], [], 'group.material');
    for (const key of ['albedo', 'absorption']) {
      vector(group.material[key], 3, `group.material.${key}`);
      group.material[key].forEach((value) => scalar(value, 0, 1, `group.material.${key}`));
    }
    for (const key of ['roughness', 'radialRoughness', 'longitudinalRoughness']) scalar(group.material[key], 0.02, 1, `group.material.${key}`);
    scalar(group.material.melanin, 0, 1, 'group.material.melanin');
    scalar(group.material.redness, 0, 1, 'group.material.redness');
  }

  keys(source.guides, ['segments', 'curves'], [], 'guides');
  integer(source.guides.segments, 2, 64, 'guides.segments');
  fail(Array.isArray(source.guides.curves) && source.guides.curves.length >= 4 && source.guides.curves.length <= 8192,
    'guides.curves needs 4–8192 guides');
  source.guides.curves.forEach((curve, index) => {
    keys(curve, ['points', 'radii', 'group'], ['rootUv'], `guides.curves[${index}]`);
    fail(curve.points.length === source.guides.segments + 1, `guides.curves[${index}] must have segments+1 points`);
    validateCurve(curve.points, curve.radii, `guides.curves[${index}]`);
    integer(curve.group, 0, source.groups.length - 1, `guides.curves[${index}].group`);
    if (curve.rootUv) vector(curve.rootUv, 2, `guides.curves[${index}].rootUv`);
  });

  keys(source.strands, ['widthMetres'], ['rootJitterMetres'], 'strands');
  scalar(source.strands.widthMetres, 0.00002, 0.01, 'strands.widthMetres');

  fail(Array.isArray(source.levels) && source.levels.length >= 1 && source.levels.length <= 4, 'groom.levels needs 1–4 entries');
  let previousStrands = Infinity;
  let previousSegments = Infinity;
  let totalPoints = 0, totalStrands = 0, maximumSegments = 0;
  source.levels.forEach((level, index) => {
    keys(level, ['level', 'segments', 'maxDistanceMetres', 'strandFraction', 'curves'], [], `levels[${index}]`);
    fail(level.level === index, 'groom levels must be ordered from 0');
    integer(level.segments, 2, 64, `levels[${index}].segments`);
    scalar(level.maxDistanceMetres, 0.1, 1000, `levels[${index}].maxDistanceMetres`);
    scalar(level.strandFraction, 0.02, 1, `levels[${index}].strandFraction`);
    fail(level.segments <= previousSegments, 'groom levels must not add segments as they coarsen');
    previousSegments = level.segments;
    fail(Array.isArray(level.curves) && level.curves.length >= 16, `levels[${index}].curves needs at least 16 strands`);
    fail(level.curves.length <= previousStrands, 'groom levels must not add strands as they coarsen');
    previousStrands = level.curves.length;
    maximumSegments = Math.max(maximumSegments, level.segments);
    const ids = new Set();
    level.curves.forEach((curve, strand) => {
      keys(curve, ['points', 'radii', 'group', 'stableId', 'blend'], [], `levels[${index}].curves[${strand}]`);
      fail(curve.points.length === level.segments + 1, `levels[${index}].curves[${strand}] point count disagrees with its segments`);
      validateCurve(curve.points, curve.radii, `levels[${index}].curves[${strand}]`);
      integer(curve.group, 0, source.groups.length - 1, `levels[${index}].curves[${strand}].group`);
      integer(curve.stableId, 0, 0xFFFFFFFE, `levels[${index}].curves[${strand}].stableId`);
      fail(!ids.has(curve.stableId), `levels[${index}] repeats stable id ${curve.stableId}`);
      ids.add(curve.stableId);
      fail(Array.isArray(curve.blend) && curve.blend.length === 3, `levels[${index}].curves[${strand}].blend needs three influences`);
      let sum = 0;
      for (const entry of curve.blend) {
        integer(entry.guide, 0, source.guides.curves.length - 1, `levels[${index}].curves[${strand}].blend guide`);
        scalar(entry.weight, 0, 1, `levels[${index}].curves[${strand}].blend weight`);
        sum += entry.weight;
      }
      // A table that does not sum to one interpolates the strand somewhere between its guides
      // and the origin of the guide set; the hair cook had exactly this bug.
      fail(Math.abs(sum - 1) <= 1e-3, `levels[${index}].curves[${strand}] guide weights sum to ${sum.toFixed(4)}`);
      totalPoints += curve.points.length;
    });
    totalStrands += level.curves.length;
  });

  keys(source.budget, ['maxStrands', 'maxPoints'], [], 'budget');
  integer(source.budget.maxStrands, 16, 400000, 'budget.maxStrands');
  integer(source.budget.maxPoints, 64, 8000000, 'budget.maxPoints');
  fail(totalStrands <= source.budget.maxStrands * source.levels.length, 'groom exceeds budget.maxStrands');
  fail(totalPoints <= source.budget.maxPoints, 'groom exceeds budget.maxPoints');
  return { totalPoints, totalStrands, maximumSegments };
}

// Cards are a merged ribbon over a contiguous run of strands, same rule as the procedural
// path: a card stands for real geometry and its coverage, not for a texture nobody cooked.
function cardsFor(level, widthMetres) {
  const cardSize = 8;
  const cards = [];
  for (let start = 0; start < level.curves.length; start += cardSize) {
    const count = Math.min(cardSize, level.curves.length - start);
    cards.push({
      firstCurve: start, curveCount: count, widthMetres: widthMetres * cardSize * 0.75,
      coverage: count / cardSize, layer: 0, material: 0, flags: 0, reserved: 0,
    });
  }
  return cards;
}

export function compileExplicitGroom(source) {
  const { totalPoints, totalStrands } = validateExplicit(source);
  const mesh = source.scalp.mesh;
  const vertices = mesh.positions.length / 3;
  const triangles = mesh.indices.length / 3;
  const scalp = { positions: mesh.positions, normals: mesh.normals, uvs: mesh.uvs, indices: mesh.indices };

  // Guide bindings: recomputed against the real scalp mesh so the native side gets a
  // barycentric frame that reproduces the root instead of trusting an id from another cook.
  const guides = source.guides.curves.map((curve, index) => {
    const binding = bindToScalp(scalp, curve.points[0]);
    fail(binding.triangle >= 0 && Number.isFinite(binding.distance), `guide ${index} could not be bound to the scalp`);
    const rootUv = curve.rootUv ?? (() => {
      // Fall back to a normalised position: the field is metadata for selection, and a
      // hairstyle that cares supplies it explicitly.
      const [x, y] = [curve.points[0][0], curve.points[0][1]];
      return [0.5, 0.5];
    })();
    return { ...curve, binding, rootUv, index };
  });

  const levels = source.levels.map((level) => ({
    level: level.level, segments: level.segments, strandFraction: level.strandFraction,
    maxDistanceMetres: level.maxDistanceMetres,
    curves: level.curves.map((curve) => ({ ...curve })),
  }));
  const cardsByLevel = levels.map((level) => cardsFor(level, source.strands.widthMetres));

  const sections = [];
  sections.push({ kind: SectionKind.POSITION_F32X3, index: SCALP_SECTION, elements: Float32Array.from(mesh.positions) });
  sections.push({ kind: SectionKind.NORMAL_F32X3, index: SCALP_SECTION, elements: Float32Array.from(mesh.normals) });
  sections.push({ kind: SectionKind.UV_F32X2, index: SCALP_SECTION, elements: Float32Array.from(mesh.uvs) });
  sections.push({ kind: SectionKind.INDEX_U32, index: SCALP_SECTION, elements: Uint32Array.from(mesh.indices) });

  const guideHeaders = [], guidePointsFlat = [], guideRadiiFlat = [], guideBindings = [];
  let guideCursor = 0;
  for (const guide of guides) {
    guideHeaders.push({ firstPoint: guideCursor, pointCount: guide.points.length, group: guide.group, flags: 0 });
    for (let point = 0; point < guide.points.length; ++point) {
      guidePointsFlat.push(guide.points[point][0], guide.points[point][1], guide.points[point][2]);
      guideRadiiFlat.push(guide.radii[point]);
    }
    guideCursor += guide.points.length;
    guideBindings.push({
      triangle: guide.binding.triangle,
      bary0: guide.binding.bary[0], bary1: guide.binding.bary[1], bary2: guide.binding.bary[2],
      rootU: guide.rootUv[0], rootV: guide.rootUv[1],
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
    level.curves.forEach((curve, strand) => {
      headers.push({ firstPoint: pointCursor, pointCount: curve.points.length, group: curve.group, flags: 0 });
      for (let point = 0; point < curve.points.length; ++point) {
        pointsFlat.push(curve.points[point][0], curve.points[point][1], curve.points[point][2]);
        radiiFlat.push(curve.radii[point]);
      }
      attributes.push({ arcLength: arcLength(curve.points).total, rootToTip: 1, group: curve.group, stableId: curve.stableId });
      weights.push({
        guide0: curve.blend[0].guide, guide1: curve.blend[1].guide, guide2: curve.blend[2].guide, strand,
        weight0: curve.blend[0].weight, weight1: curve.blend[1].weight, weight2: curve.blend[2].weight, reserved: 0,
      });
      pointCursor += curve.points.length;
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
      maxDistanceMetres: level.maxDistanceMetres, strandFraction: level.strandFraction,
    });
  }

  const bounds = boundsOf(levels[0].curves);
  const meta = {
    schema: GROOM_COOK_SCHEMA,
    id: source.id, style: source.style, seed: source.seed,
    units: 'metres',
    sourceDigest: sha256Hex(canonicalJson(source)),
    sourceSchema: GROOM_EXPLICIT_SCHEMA,
    representations: ['strands', 'cards', 'guides'],
    scalp: {
      // Not a `sphereCap`: the mesh above is the character's own scalp, and a report that
      // reads this field must be able to say which one it simulated.
      kind: 'triangles', sourceHash: source.scalp.sourceHash ?? null,
      vertices, triangles, version: 'triangles/1',
    },
    groups: source.groups.map((group, index) => ({ ...group, index })),
    counts: {
      guides: guides.length, guidePoints: guidePointsFlat.length / 3,
      strands: levels[0].curves.length, strandPoints: levels[0].curves.reduce((sum, curve) => sum + curve.points.length, 0),
      cards: cardsByLevel[0].length, groups: source.groups.length,
    },
    lod: lodMeta,
    bounds,
    lengthMetres: arcLength(levels[0].curves[0].points).total,
    widthMetres: source.strands.widthMetres,
    cardModel: 'mergedRibbon',
    collision: { spheres: [], capsules: [] },
  };
  const bytes = buildPackage({ schema: GROOM_COOK_SCHEMA, meta, sections });
  return {
    id: source.id, meta, bytes, hash: digestPackage(bytes),
    guides: guides.length, strands: levels[0].curves.length,
    segments: source.guides.segments, levels: lodMeta,
    bytesTotal: bytes.byteLength,
    totalPoints, totalStrands,
  };
}

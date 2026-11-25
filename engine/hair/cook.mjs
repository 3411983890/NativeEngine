import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { buildPackage, digestPackage, packRecords, readPackage, SectionKind } from '../assets/binary-package.mjs';
import { compileGroom } from '../groom/asset.mjs';
import { HairError, HAIR_COOK_SCHEMA, hairstyleRevision, regionCentreAzimuth } from './spec.mjs';

// Cook: editable hairstyle source → binary package the runtime reads.
//
// The cooked package is the only thing the renderer and the hair solver see. It is
// content-addressed and self-describing: a reader can tell which scalp it was bound
// to, which region each guide belongs to, which strands belong to which guide, and
// which LOD a section index refers to — without parsing the authoring document.
//
// Sections reuse the groom layout family, so the existing native reader can parse a
// hair package with its curve decoders. What is new is what those curves *mean*:
// roots are barycentric frames on the character's real head, and guide/strand tables
// carry region identity instead of a single implicit group.

export const HAIR_SCALP_SECTION = 900;
export const HAIR_GUIDE_SECTION = 1000;

// Field layouts are exported for tests and for the native reader's counterpart.
export const CURVE_HEADER_LAYOUT = [
  { name: 'firstPoint', type: 'u32', bytes: 4 },
  { name: 'pointCount', type: 'u32', bytes: 4 },
  { name: 'group', type: 'u32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 },
];
export const BINDING_LAYOUT = [
  { name: 'triangle', type: 'u32', bytes: 4 },
  { name: 'bary0', type: 'f32', bytes: 4 },
  { name: 'bary1', type: 'f32', bytes: 4 },
  { name: 'bary2', type: 'f32', bytes: 4 },
  { name: 'rootU', type: 'f32', bytes: 4 },
  { name: 'rootV', type: 'f32', bytes: 4 },
  { name: 'offsetMetres', type: 'f32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 },
];
export const CURVE_ATTRIBUTE_LAYOUT = [
  { name: 'arcLength', type: 'f32', bytes: 4 },
  { name: 'rootToTip', type: 'f32', bytes: 4 },
  { name: 'group', type: 'u32', bytes: 4 },
  { name: 'stableId', type: 'u32', bytes: 4 },
];
export const GUIDE_WEIGHT_LAYOUT = [
  { name: 'guide0', type: 'u32', bytes: 4 },
  { name: 'guide1', type: 'u32', bytes: 4 },
  { name: 'guide2', type: 'u32', bytes: 4 },
  { name: 'strand', type: 'u32', bytes: 4 },
  { name: 'weight0', type: 'f32', bytes: 4 },
  { name: 'weight1', type: 'f32', bytes: 4 },
  { name: 'weight2', type: 'f32', bytes: 4 },
  { name: 'reserved', type: 'u32', bytes: 4 },
];
export const CARD_LAYOUT = [
  { name: 'firstCurve', type: 'u32', bytes: 4 },
  { name: 'curveCount', type: 'u32', bytes: 4 },
  { name: 'widthMetres', type: 'f32', bytes: 4 },
  { name: 'coverage', type: 'f32', bytes: 4 },
  { name: 'layer', type: 'u32', bytes: 4 },
  { name: 'material', type: 'u32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 },
  { name: 'reserved', type: 'u32', bytes: 4 },
];

const fail = (ok, message, details) => { if (!ok) throw new HairError('NG-E-HAIR-COOK', message, details); };

// Spherical UV for the scalp, only so a debug shader has a stable parameterisation to
// draw region boundaries on. It is not a texture UV and is not presented as one.
function scalpUvs(scalp) {
  const uvs = new Float32Array(scalp.vertices * 2);
  for (let vertex = 0; vertex < scalp.vertices; ++vertex) {
    const point = [scalp.positions[vertex * 3], scalp.positions[vertex * 3 + 1], scalp.positions[vertex * 3 + 2]];
    const azimuth = Math.atan2(point[0] - scalp.centre[0], point[2] - scalp.centre[2]);
    const dy = point[1] - scalp.centre[1];
    const radius = Math.hypot(point[0] - scalp.centre[0], dy, point[2] - scalp.centre[2]) || 1e-9;
    uvs[vertex * 2] = (azimuth / (Math.PI * 2)) + 0.5;
    uvs[vertex * 2 + 1] = 1 - (Math.acos(Math.max(-1, Math.min(1, dy / radius))) / Math.PI);
  }
  return uvs;
}

function scalpNormals(scalp) {
  const normals = new Float32Array(scalp.vertices * 3);
  const accumulate = new Float64Array(scalp.vertices * 3);
  for (let triangle = 0; triangle < scalp.triangles; ++triangle) {
    const a = scalp.indices[triangle * 3], b = scalp.indices[triangle * 3 + 1], c = scalp.indices[triangle * 3 + 2];
    for (let axis = 0; axis < 3; ++axis) {
      const value = scalp.normals[triangle * 3 + axis] * scalp.areas[triangle];
      accumulate[a * 3 + axis] += value;
      accumulate[b * 3 + axis] += value;
      accumulate[c * 3 + axis] += value;
    }
  }
  for (let vertex = 0; vertex < scalp.vertices; ++vertex) {
    const x = accumulate[vertex * 3], y = accumulate[vertex * 3 + 1], z = accumulate[vertex * 3 + 2];
    const length = Math.hypot(x, y, z) || 1;
    normals[vertex * 3] = x / length;
    normals[vertex * 3 + 1] = y / length;
    normals[vertex * 3 + 2] = z / length;
  }
  return normals;
}

// Three influences, normalised. The generator blends five guides; every runtime table keeps
// three, and slicing without renormalising quietly stores 84.5% of the strand's weight. Both
// the hair package and the groom source go through this function so they cannot diverge.
function threeWayBlend(curve) {
  const blend = [0, 1, 2].map((slot) => curve.blend?.[slot] ?? { guide: curve.guide, weight: slot === 0 ? 1 : 0 });
  const total = blend.reduce((sum, entry) => sum + entry.weight, 0);
  fail(total > 1e-4, `strand ${curve.stableId} has no guide weight to interpolate with`);
  return blend.map((entry) => ({ guide: entry.guide, weight: entry.weight / total }));
}

export function cookHairstyle(generated, { binding = null, analysis = null } = {}) {
  const { spec, scalp, guides, levels, regions } = generated;
  const regionIndexOf = new Map(spec.regions.map((region, index) => [region.id, index]));
  const guideIndexById = new Map(guides.map((guide, index) => [guide.stableId, index]));
  const guideIds = guides.map((guide) => guide.stableId);
  const streamsPerGuide = spec.strands.streamsPerGuide;
  // Strand identity is derived, not stored: strands are generated region by region,
  // guide by guide, so index i belongs to guide floor(i / streamsPerGuide) at ordinal
  // i % streamsPerGuide. Storing 67k strings in the package header would triple the
  // metadata for information the tables already contain.
  fail(levels[0].curves.length === guides.length * streamsPerGuide, 'strand table does not match guides × streamsPerGuide');

  const sections = [];
  sections.push({ kind: SectionKind.POSITION_F32X3, index: HAIR_SCALP_SECTION, elements: Float32Array.from(scalp.positions) });
  sections.push({ kind: SectionKind.NORMAL_F32X3, index: HAIR_SCALP_SECTION, elements: scalpNormals(scalp) });
  sections.push({ kind: SectionKind.UV_F32X2, index: HAIR_SCALP_SECTION, elements: scalpUvs(scalp) });
  sections.push({ kind: SectionKind.INDEX_U32, index: HAIR_SCALP_SECTION, elements: Uint32Array.from(scalp.indices) });

  // Guides. `group` is the region index, `stableId` indexes meta.guideIds.
  const guideHeaders = [], guidePoints = [], guideRadii = [], guideAttributes = [], guideBindings = [];
  let cursor = 0;
  guides.forEach((guide, index) => {
    guideHeaders.push({ firstPoint: cursor, pointCount: guide.points.length, group: regionIndexOf.get(guide.region), flags: 0 });
    for (let point = 0; point < guide.points.length; ++point) {
      guidePoints.push(guide.points[point][0], guide.points[point][1], guide.points[point][2]);
      guideRadii.push(guide.radii[point]);
    }
    cursor += guide.points.length;
    guideAttributes.push({
      arcLength: guide.lengthMetres, rootToTip: 1,
      group: regionIndexOf.get(guide.region), stableId: index,
    });
    guideBindings.push({
      triangle: guide.triangle,
      bary0: guide.bary[0], bary1: guide.bary[1], bary2: guide.bary[2],
      rootU: guide.azimuthDegrees / 360 + 0.5, rootV: 1 - guide.polarDegrees / 96,
      offsetMetres: 0, flags: 0,
    });
  });
  sections.push({ kind: SectionKind.CURVE_HEADER, index: HAIR_GUIDE_SECTION, data: packRecords(guideHeaders, CURVE_HEADER_LAYOUT).buffer });
  sections.push({ kind: SectionKind.CURVE_POINT_F32X3, index: HAIR_GUIDE_SECTION, elements: Float32Array.from(guidePoints) });
  sections.push({ kind: SectionKind.CURVE_RADIUS_F32, index: HAIR_GUIDE_SECTION, elements: Float32Array.from(guideRadii) });
  sections.push({ kind: SectionKind.CURVE_ATTRIBUTE, index: HAIR_GUIDE_SECTION, data: packRecords(guideAttributes, CURVE_ATTRIBUTE_LAYOUT).buffer });
  sections.push({ kind: SectionKind.BINDING, index: HAIR_GUIDE_SECTION, data: packRecords(guideBindings, BINDING_LAYOUT).buffer });

  const lodMeta = [];
  for (const level of levels) {
    const headers = [], points = [], radii = [], attributes = [], weights = [];
    let pointCursor = 0;
    level.curves.forEach((curve, index) => {
      const regionIndex = regionIndexOf.get(curve.region);
      headers.push({ firstPoint: pointCursor, pointCount: curve.points.length, group: regionIndex, flags: 0 });
      for (let point = 0; point < curve.points.length; ++point) {
        points.push(curve.points[point][0], curve.points[point][1], curve.points[point][2]);
        radii.push(curve.radii[point]);
      }
      pointCursor += curve.points.length;
      // `stableId` is the level-0 strand index, so a coarse LOD keeps the identity a
      // patch was authored against even though its own position in the table differs.
      attributes.push({
        arcLength: curve.lengthMetres ?? 0, rootToTip: 1,
        group: regionIndex, stableId: curve.sourceIndex,
      });
      const blend = threeWayBlend(curve);
      const indices = blend.map((entry) => {
        const index2 = guideIndexById.get(entry.guide);
        fail(index2 !== undefined, `strand ${curve.stableId} blends unknown guide ${entry.guide}`);
        return index2;
      });
      weights.push({
        guide0: indices[0], guide1: indices[1], guide2: indices[2], strand: index,
        weight0: blend[0].weight, weight1: blend[1].weight, weight2: blend[2].weight, reserved: 0,
      });
    });

    // Cards: a merged ribbon over a contiguous run of strands. A card is a real
    // geometric fallback with coverage attributed to it, not a texture the cook never
    // produced, so a viewport that draws cards draws the same hair.
    const cardSize = 8, cards = [];
    for (let start = 0; start < level.curves.length; start += cardSize) {
      const count = Math.min(cardSize, level.curves.length - start);
      cards.push({
        firstCurve: start, curveCount: count,
        widthMetres: spec.strands.widthMetres * cardSize * 0.75,
        coverage: count / cardSize, layer: 0,
        material: regionIndexOf.get(level.curves[start].region) ?? 0, flags: 0, reserved: 0,
      });
    }

    sections.push({ kind: SectionKind.CURVE_HEADER, index: level.level, data: packRecords(headers, CURVE_HEADER_LAYOUT).buffer });
    sections.push({ kind: SectionKind.CURVE_POINT_F32X3, index: level.level, elements: Float32Array.from(points) });
    sections.push({ kind: SectionKind.CURVE_RADIUS_F32, index: level.level, elements: Float32Array.from(radii) });
    sections.push({ kind: SectionKind.CURVE_ATTRIBUTE, index: level.level, data: packRecords(attributes, CURVE_ATTRIBUTE_LAYOUT).buffer });
    sections.push({ kind: SectionKind.GUIDE_WEIGHT, index: level.level, data: packRecords(weights, GUIDE_WEIGHT_LAYOUT).buffer });
    sections.push({ kind: SectionKind.CARD, index: level.level, data: packRecords(cards, CARD_LAYOUT).buffer });

    lodMeta.push({
      level: level.level, strands: level.curves.length, segments: level.segments,
      points: level.points, cards: cards.length,
      maxDistanceMetres: level.maxDistanceMetres, strandFraction: level.strandFraction,
    });
  }

  const revision = hairstyleRevision(spec);
  const meta = {
    schema: HAIR_COOK_SCHEMA,
    id: spec.id,
    seed: spec.seed,
    units: 'metres',
    revision,
    sourceDigest: sha256Hex(canonicalJson(spec)),
    representations: ['strands', 'cards', 'guides'],
    scalp: {
      kind: 'characterMesh', hash: scalp.hash, headBone: scalp.headBone,
      vertices: scalp.vertices, triangles: scalp.triangles,
      centre: scalp.centre, radiusMetres: scalp.radius,
      characterHash: scalp.characterHash,
    },
    regions: regions.map((region, index) => ({
      index,
      id: region.id,
      areaCm2: Math.round(region.areaCm2 * 1000) / 1000,
      guides: region.guides,
      meanLengthMetres: Math.round(region.meanLengthMetres * 1e5) / 1e5,
      densityPerCm2: region.densityPerCm2,
      lockedFields: region.lockedFields,
      bundle: region.bundle,
      parting: region.parting,
      centreAzimuthDegrees: regionCentreAzimuth(spec.regions[index]),
    })),
    counts: {
      guides: guides.length, guidePoints: generated.stats.guidePoints,
      guideIds: guideIds.length,
      strands: levels[0].curves.length, strandPoints: generated.stats.strandPoints,
      segments: generated.stats.segments,
    },
    lod: lodMeta,
    bounds: generated.bounds,
    // Guide ids are stored because they are named by scalp location and cannot be
    // derived. Strand ids are derived from (guide, ordinal) — see strandStableId.
    guideIds,
    strandIdModel: { pattern: 'guide.ordinal', streamsPerGuide },
    material: spec.material,
    physics: {
      ...spec.physics,
      rootBinding: 'barycentric-on-character-scalp',
    },
    binding: binding ?? null,
    analysis: analysis ?? null,
    references: spec.references,
    cardModel: 'mergedRibbon',
    collision: { source: 'characterRig', headBone: spec.character.headBone },
  };

  const bytes = buildPackage({ schema: HAIR_COOK_SCHEMA, meta, sections });
  return {
    id: spec.id,
    revision,
    hash: digestPackage(bytes),
    bytes,
    meta,
    counts: meta.counts,
    lod: lodMeta,
    regions: meta.regions,
    bytesTotal: bytes.byteLength,
  };
}

// Compatibility export: a `nativegpu.groom/1` source that the existing groom cook
// accepts. It is deliberately lossy, and the losses are returned rather than implied:
// the old format has one length, one comb direction and a sphere-cap scalp, so region
// identity, partings, bundles and the real scalp cannot survive the trip.
export function toCompatibleGroomSource(spec) {
  // One groom group per *region*, not per material: the groom format's group index is what a
  // runtime reports as `region_id`, and collapsing five regions into one material group throws
  // away the only label a debug channel could show. The material is shared until a hairstyle
  // authors per-region materials, and saying so is cheaper than pretending.
  const material = spec.material.groups[0];
  const groups = spec.regions.map((region) => ({
    id: region.id,
    material: {
      albedo: material.albedo,
      roughness: material.roughness,
      radialRoughness: material.radialRoughness ?? material.roughness,
      longitudinalRoughness: material.longitudinalRoughness ?? material.roughness,
      absorption: material.absorption ?? [0.6, 0.42, 0.3],
      melanin: material.melanin ?? 0.6,
      redness: material.redness ?? 0.3,
    },
    simulation: 'guide',
    shadow: 'deepOpacity',
  }));
  const guides = spec.regions.reduce((sum, region) => sum + region.densityPerCm2, 0) / spec.regions.length;
  const lengths = spec.regions.map((region) => region.lengthMetres);
  const combs = spec.regions.reduce((sum, region) => [
    sum[0] + region.comb[0], sum[1] + region.comb[1], sum[2] + region.comb[2],
  ], [0, 0, 0]).map((value) => value / spec.regions.length);
  return {
    source: {
      schema: 'nativegpu.groom/1',
      id: spec.id,
      style: 'scalp',
      seed: spec.seed,
      // The cap radius comes from the scalp the hairstyle was actually bound to, so the
      // fallback is the right size even though it is not the right shape.
      scalp: {
        radiusMetres: Math.max(0.03, Math.min(0.5, (spec.character.scalpRadiusMetres ?? 0.095))),
        coverageDegrees: 118,
        rings: 12, segments: 24,
      },
      groups,
      guides: {
        count: Math.max(8, Math.min(2048, Math.round(guides * 2))),
        segments: spec.guides.segments,
        lengthMetres: Math.max(...lengths),
        rootRadiusMetres: spec.guides.radiusMetres,
        gravity: spec.physics.gravityMetresPerSecond2 / 9.81,
        comb: combs,
      },
      strands: {
        count: Math.max(16, Math.min(40000, Math.round(
          Math.max(8, Math.min(2048, Math.round(guides * 2))) * spec.strands.streamsPerGuide))),
        frizzMetres: spec.regions[0].frizz?.amplitudeMetres ?? spec.strands.frizzMetres ?? 0,
        widthMetres: spec.strands.widthMetres,
        rootJitterMetres: spec.strands.rootJitterMetres,
      },
      lod: spec.lod.map((entry) => ({
        level: entry.level, strandFraction: entry.strandFraction,
        segmentScale: entry.segmentScale, maxDistanceMetres: entry.maxDistanceMetres,
      })),
      budget: { maxStrands: Math.max(40000, spec.budget.maxStrands), maxPoints: spec.budget.maxPoints },
    },
    losses: [
      'scalp replaced by a spherical cap; root binding is no longer per-triangle',
      'region identity, partings, bundles and per-region material are collapsed to one average',
      'guide and strand stable ids are re-derived by the legacy cook and no longer match the hairstyle',
    ],
  };
}

// Explicit groom source: the hairstyle's own scalp mesh, guides, strands and guide weights,
// transcribed into `nativegpu.groom/2` instead of being flattened into a spherical cap.
//
// The compatible path below (`toCompatibleGroomSource`) exists for runtimes that only know the
// procedural source, and it documents three losses. This one keeps the real thing: 2390 scalp
// triangles, 1806 guides bound to them, 14448 strands and the same per-strand weight table the
// hair package carries. That is what makes the native guide solver simulate the authored
// hairstyle rather than a token cap.
export function groomSourceFromHairstyle(generated, { id = null, style = 'scalp' } = {}) {
  const { spec, scalp, guides, levels } = generated;
  const regionIndex = new Map(spec.regions.map((region, index) => [region.id, index]));
  const guideIndex = new Map(guides.map((guide, index) => [guide.stableId, index]));
  // One groom group per *region*, not per material: the groom format's group index is what a
  // runtime reports as `region_id`, and collapsing five regions into one material group throws
  // away the only label a debug channel could show. The material is shared until a hairstyle
  // authors per-region materials, and saying so is cheaper than pretending.
  const material = spec.material.groups[0];
  const groups = spec.regions.map((region) => ({
    id: region.id,
    material: {
      albedo: material.albedo,
      roughness: material.roughness,
      radialRoughness: material.radialRoughness ?? material.roughness,
      longitudinalRoughness: material.longitudinalRoughness ?? material.roughness,
      absorption: material.absorption ?? [0.6, 0.42, 0.3],
      melanin: material.melanin ?? 0.6,
      redness: material.redness ?? 0.3,
    },
    simulation: 'guide',
    shadow: 'deepOpacity',
  }));
  const normals = scalpNormals(scalp);
  const uvs = scalpUvs(scalp);
  const groomGuides = guides.map((guide) => ({
    points: guide.points.map((point) => [...point]),
    radii: [...guide.radii],
    group: regionIndex.get(guide.region) ?? 0,
    rootUv: [guide.azimuthDegrees / 360 + 0.5, 1 - guide.polarDegrees / 180],
  }));
  const groomLevels = levels.map((level) => {
    const segments = level.curves[0].points.length - 1;
    for (const curve of level.curves) {
      fail(curve.points.length === segments + 1, 'hairstyle level mixes curve lengths');
    }
    return {
      level: level.level,
      segments,
      maxDistanceMetres: level.maxDistanceMetres,
      strandFraction: level.strandFraction ?? 1,
      curves: level.curves.map((curve, index) => {
        const blend = threeWayBlend(curve).map((entry) => {
          const mapped = guideIndex.get(entry.guide);
          fail(mapped !== undefined, `strand ${curve.stableId} blends unknown guide ${entry.guide}`);
          return { guide: mapped, weight: entry.weight };
        });
        return {
          points: curve.points.map((point) => [...point]),
          radii: [...curve.radii],
          group: regionIndex.get(curve.region) ?? 0,
          stableId: curve.sourceIndex ?? index,
          blend,
        };
      }),
    };
  });
  return {
    schema: 'nativegpu.groom/2',
    id: id ?? `${spec.id}_groom`,
    style,
    seed: spec.seed,
    scalp: {
      mesh: {
        positions: Array.from(scalp.positions),
        normals: Array.from(normals),
        uvs: Array.from(uvs),
        indices: Array.from(scalp.indices),
      },
      sourceHash: scalp.hash ?? null,
    },
    groups,
    guides: { segments: spec.guides.segments, curves: groomGuides },
    strands: { widthMetres: spec.strands.widthMetres, rootJitterMetres: spec.strands.rootJitterMetres },
    levels: groomLevels,
    budget: { maxStrands: spec.budget.maxStrands, maxPoints: spec.budget.maxPoints },
  };
}

export function cookCompatibleGroom(spec) {
  const { source, losses } = toCompatibleGroomSource(spec);
  const cooked = compileGroom(source);
  return { ...cooked, losses, compatible: true };
}

// --------------------------------------------------------------- reading back
// Everything the tools report about a cooked hairstyle is read back from the bytes,
// never from the object that produced them. A validation that trusts the in-memory
// result cannot catch a writer that packs the wrong section.

// Strand identity: "<guide stable id>.<ordinal within that guide>". The owning guide
// is read from the GUIDE_WEIGHT table rather than recomputed from the index, so the
// identity survives a change of guide count in another region — which is exactly what
// happens when a patch adds density to `bangs` only.
export function strandStableId(meta, guideIndex, ordinal) {
  const guideId = meta.guideIds?.[guideIndex];
  return guideId ? `${guideId}.${ordinal}` : null;
}

export function strandIdentity(read, level, index) {
  const weights = read.levels[level].weights;
  const stride = 32;
  const guideIndex = weights.data.readUInt32LE(index * stride);
  let ordinal = 0;
  for (let cursor = index - 1; cursor >= 0; --cursor) {
    if (weights.data.readUInt32LE(cursor * stride) !== guideIndex) break;
    ++ordinal;
  }
  return { guideIndex, ordinal, stableId: strandStableId(read.meta, guideIndex, ordinal) };
}

function decodeCurves(pkg, index) {
  const headers = pkg.require(SectionKind.CURVE_HEADER, index);
  const points = pkg.require(SectionKind.CURVE_POINT_F32X3, index);
  const radii = pkg.require(SectionKind.CURVE_RADIUS_F32, index);
  const pointView = new Float32Array(points.data.buffer, points.data.byteOffset, points.bytes / 4);
  const radiusView = new Float32Array(radii.data.buffer, radii.data.byteOffset, radii.bytes / 4);
  const curves = [];
  for (let record = 0; record < headers.count; ++record) {
    const base = record * 16;
    const first = headers.data.readUInt32LE(base);
    const count = headers.data.readUInt32LE(base + 4);
    const group = headers.data.readUInt32LE(base + 8);
    const curvePoints = [];
    const curveRadii = [];
    for (let point = 0; point < count; ++point) {
      curvePoints.push([
        pointView[(first + point) * 3], pointView[(first + point) * 3 + 1], pointView[(first + point) * 3 + 2],
      ]);
      curveRadii.push(radiusView[first + point]);
    }
    curves.push({ firstPoint: first, group, points: curvePoints, radii: curveRadii });
  }
  return curves;
}

export function readHairPackage(bytes) {
  const pkg = readPackage(bytes);
  fail(pkg.schema === HAIR_COOK_SCHEMA, `package schema is ${pkg.schema}, expected ${HAIR_COOK_SCHEMA}`);
  const meta = pkg.meta;
  const scalpPositions = pkg.require(SectionKind.POSITION_F32X3, HAIR_SCALP_SECTION);
  const scalpIndices = pkg.require(SectionKind.INDEX_U32, HAIR_SCALP_SECTION);
  const guides = decodeCurves(pkg, HAIR_GUIDE_SECTION);
  const guideAttributes = pkg.require(SectionKind.CURVE_ATTRIBUTE, HAIR_GUIDE_SECTION);
  const bindings = pkg.require(SectionKind.BINDING, HAIR_GUIDE_SECTION);
  const levels = (meta.lod ?? []).map((entry) => ({
    ...entry,
    curves: decodeCurves(pkg, entry.level),
    attributes: pkg.require(SectionKind.CURVE_ATTRIBUTE, entry.level),
    weights: pkg.require(SectionKind.GUIDE_WEIGHT, entry.level),
    cards: pkg.require(SectionKind.CARD, entry.level),
  }));
  return {
    schema: pkg.schema, meta, hash: sha256Hex(bytes),
    scalp: {
      vertices: scalpPositions.count,
      triangles: scalpIndices.count / 3,
      positions: new Float32Array(scalpPositions.data.buffer, scalpPositions.data.byteOffset, scalpPositions.bytes / 4),
    },
    guides, guideAttributes, bindings, levels,
  };
}

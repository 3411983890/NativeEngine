import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';

// Editable hairstyle source contract: `nativegpu.hairstyle/1`.
//
// The old `nativegpu.groom/1` document described a whole hairstyle in one flat set of
// numbers: one length, one comb direction, one material, a sphere-cap scalp. That is
// enough to cook a wig, and not enough for the work in the plan — per-region length,
// partings, explicit bundles, locked regions, stable guide identity across edits, and
// a reference-image provenance trail.
//
// So the editable source is its own versioned schema, and a *cook* turns it into a
// package the runtime reads. The source is never loaded by the renderer; the cook
// output is. That split is what lets the authoring fields grow without breaking the
// strict loader.

export const HAIRSTYLE_SCHEMA = 'nativegpu.hairstyle/1';
export const HAIR_COOK_SCHEMA = 'nativegpu.hair-cook/1';

const ID = /^[a-z][a-z0-9_-]{0,63}$/;

export class HairError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'HairError';
    this.code = code;
    this.details = details;
  }
}

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };
const object = (value, label) => fail(value && typeof value === 'object' && !Array.isArray(value)
  && (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null),
  'NG-E-HAIR-FORMAT', `${label} must be an object`);

function keys(value, required, optional, label) {
  object(value, label);
  for (const key of required) {
    fail(Object.hasOwn(value, key), 'NG-E-HAIR-FORMAT', `${label}.${key} is required`);
  }
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    fail(allowed.has(key), 'NG-E-HAIR-FORMAT', `${label}.${key} is unsupported`);
  }
}

const integer = (value, low, high, label) => fail(Number.isSafeInteger(value) && value >= low && value <= high,
  'NG-E-HAIR-FORMAT', `${label} must be an integer in [${low},${high}]`);
const scalar = (value, low, high, label) => fail(typeof value === 'number' && Number.isFinite(value)
  && value >= low && value <= high, 'NG-E-HAIR-FORMAT', `${label} must be finite and in [${low},${high}]`);
const vector = (value, count, low, high, label) => {
  fail(Array.isArray(value) && value.length === count, 'NG-E-HAIR-FORMAT', `${label} requires ${count} values`);
  value.forEach((entry, index) => scalar(entry, low, high, `${label}[${index}]`));
};
const identifier = (value, label) => fail(typeof value === 'string' && ID.test(value),
  'NG-E-HAIR-FORMAT', `${label} is not a valid identifier`);

// Canonical region names. The plan names them explicitly, and sides follow the
// character's own left (+X in this engine), never the viewer's.
export const REGION_IDS = Object.freeze([
  'bangs', 'crown', 'left_side', 'right_side', 'back', 'nape',
]);

// Region presets carry the angular sector a person means by the name, so a caller
// that says "bangs" gets a front sector instead of having to know the convention.
// A sector whose start is greater than its end wraps through ±180 — that is how the
// back of a head is expressed without inventing a 0..360 convention that the rest of
// the file would have to remember.
export const REGION_PRESETS = Object.freeze({
  bangs: { azimuthDegrees: [-38, 38], polarDegrees: [0, 68] },
  crown: { azimuthDegrees: [-180, 180], polarDegrees: [0, 26] },
  left_side: { azimuthDegrees: [42, 132], polarDegrees: [10, 82] },
  right_side: { azimuthDegrees: [-132, -42], polarDegrees: [10, 82] },
  back: { azimuthDegrees: [128, -128], polarDegrees: [8, 88] },
  nape: { azimuthDegrees: [140, -140], polarDegrees: [70, 96] },
});

const BUNDLE_KINDS = new Set(['none', 'braid', 'ponytail', 'bun']);

function validateRegion(region, index, groupIds) {
  const label = `regions[${index}]`;
  keys(region, ['id', 'azimuthDegrees', 'polarDegrees', 'densityPerCm2', 'lengthMetres', 'comb'], [
    'material', 'parting', 'curl', 'clumping', 'frizz', 'taper', 'bundle', 'lockedFields',
    'simulation', 'name',
  ], label);
  identifier(region.id, `${label}.id`);
  vector(region.azimuthDegrees, 2, -180, 180, `${label}.azimuthDegrees`);
  vector(region.polarDegrees, 2, 0, 96, `${label}.polarDegrees`);
  fail(region.azimuthDegrees[0] !== region.azimuthDegrees[1],
    'NG-E-HAIR-FORMAT', `${label}.azimuthDegrees must span a non-zero sector`);
  fail(region.polarDegrees[0] < region.polarDegrees[1],
    'NG-E-HAIR-FORMAT', `${label}.polarDegrees must be [min,max]`);
  scalar(region.densityPerCm2, 1, 4000, `${label}.densityPerCm2`);
  scalar(region.lengthMetres, 0.005, 1.5, `${label}.lengthMetres`);
  // Comb is a direction in the character frame, not a unit vector the caller must
  // pre-normalise: a zero vector would silently turn every guide into the surface
  // normal, which is exactly the "sphere cap" failure the plan rejects.
  vector(region.comb, 3, -1, 1, `${label}.comb`);
  fail(Math.hypot(...region.comb) > 1e-3, 'NG-E-HAIR-FORMAT',
    `${label}.comb must be a non-zero direction`);
  if (region.material !== undefined) {
    fail(typeof region.material === 'string' && groupIds.has(region.material), 'NG-E-HAIR-FORMAT',
      `${label}.material references unknown group ${region.material}`);
  }
  if (region.simulation !== undefined) {
    fail(['guide', 'rigid'].includes(region.simulation), 'NG-E-HAIR-FORMAT',
      `${label}.simulation must be guide or rigid`);
  }
  if (region.parting !== undefined) {
    keys(region.parting, ['azimuthDegrees', 'widthDegrees'], ['enabled'], `${label}.parting`);
    scalar(region.parting.azimuthDegrees, -180, 180, `${label}.parting.azimuthDegrees`);
    scalar(region.parting.widthDegrees, 0.5, 40, `${label}.parting.widthDegrees`);
    if (region.parting.enabled !== undefined) {
      fail(typeof region.parting.enabled === 'boolean', 'NG-E-HAIR-FORMAT', `${label}.parting.enabled must be boolean`);
    }
  }
  if (region.curl !== undefined) {
    keys(region.curl, ['turnsPerMetre'], ['radiusMetres', 'noise', 'axisBias'], `${label}.curl`);
    scalar(region.curl.turnsPerMetre, 0, 80, `${label}.curl.turnsPerMetre`);
    if (region.curl.radiusMetres !== undefined) scalar(region.curl.radiusMetres, 0.002, 0.2, `${label}.curl.radiusMetres`);
    if (region.curl.noise !== undefined) scalar(region.curl.noise, 0, 1, `${label}.curl.noise`);
    if (region.curl.axisBias !== undefined) vector(region.curl.axisBias, 3, -1, 1, `${label}.curl.axisBias`);
  }
  if (region.clumping !== undefined) {
    keys(region.clumping, ['count', 'strength'], ['tipSpreadMetres'], `${label}.clumping`);
    integer(region.clumping.count, 1, 512, `${label}.clumping.count`);
    scalar(region.clumping.strength, 0, 1, `${label}.clumping.strength`);
    if (region.clumping.tipSpreadMetres !== undefined) {
      scalar(region.clumping.tipSpreadMetres, 0, 0.2, `${label}.clumping.tipSpreadMetres`);
    }
  }
  if (region.frizz !== undefined) {
    keys(region.frizz, ['amplitudeMetres', 'frequency'], [], `${label}.frizz`);
    scalar(region.frizz.amplitudeMetres, 0, 0.03, `${label}.frizz.amplitudeMetres`);
    scalar(region.frizz.frequency, 0, 120, `${label}.frizz.frequency`);
  }
  if (region.taper !== undefined) {
    keys(region.taper, ['root', 'tip'], ['power'], `${label}.taper`);
    scalar(region.taper.root, 0.05, 1, `${label}.taper.root`);
    scalar(region.taper.tip, 0.02, 1, `${label}.taper.tip`);
    if (region.taper.power !== undefined) scalar(region.taper.power, 0.2, 5, `${label}.taper.power`);
  }
  if (region.bundle !== undefined) {
    keys(region.bundle, ['kind', 'count'], ['turnPerMetre', 'anchorAzimuthDegrees'], `${label}.bundle`);
    fail(BUNDLE_KINDS.has(region.bundle.kind), 'NG-E-HAIR-FORMAT',
      `${label}.bundle.kind must be none, braid, ponytail or bun`);
    integer(region.bundle.count, 1, 64, `${label}.bundle.count`);
    if (region.bundle.turnPerMetre !== undefined) scalar(region.bundle.turnPerMetre, 0, 120, `${label}.bundle.turnPerMetre`);
    if (region.bundle.anchorAzimuthDegrees !== undefined) {
      scalar(region.bundle.anchorAzimuthDegrees, -180, 180, `${label}.bundle.anchorAzimuthDegrees`);
    }
  }
  if (region.lockedFields !== undefined) {
    fail(Array.isArray(region.lockedFields) && region.lockedFields.length <= 16, 'NG-E-HAIR-FORMAT',
      `${label}.lockedFields must be an array of at most 16 field names`);
    for (const field of region.lockedFields) {
      fail(typeof field === 'string' && field.length > 0, 'NG-E-HAIR-FORMAT', `${label}.lockedFields entry must be a string`);
    }
  }
}

function validateMaterialGroup(group, index) {
  const label = `material.groups[${index}]`;
  keys(group, ['id', 'albedo', 'roughness'], [
    'radialRoughness', 'longitudinalRoughness', 'melanin', 'redness', 'absorption',
    'rootToTipTint', 'transmission',
  ], label);
  identifier(group.id, `${label}.id`);
  vector(group.albedo, 3, 0, 1, `${label}.albedo`);
  scalar(group.roughness, 0.02, 1, `${label}.roughness`);
  if (group.radialRoughness !== undefined) scalar(group.radialRoughness, 0.02, 1, `${label}.radialRoughness`);
  if (group.longitudinalRoughness !== undefined) scalar(group.longitudinalRoughness, 0.02, 1, `${label}.longitudinalRoughness`);
  if (group.melanin !== undefined) scalar(group.melanin, 0, 1, `${label}.melanin`);
  if (group.redness !== undefined) scalar(group.redness, 0, 1, `${label}.redness`);
  if (group.absorption !== undefined) vector(group.absorption, 3, 0, 1, `${label}.absorption`);
  if (group.transmission !== undefined) scalar(group.transmission, 0, 1, `${label}.transmission`);
  if (group.rootToTipTint !== undefined) vector(group.rootToTipTint, 3, 0, 4, `${label}.rootToTipTint`);
}

export function validateHairstyle(spec) {
  keys(spec, ['schema', 'id', 'seed', 'character', 'regions', 'strands', 'physics', 'material', 'lod', 'budget'], [
    'references', 'analysis', 'guides', 'locks', 'notes', 'guideOverrides',
  ], 'hairstyle');
  fail(spec.schema === HAIRSTYLE_SCHEMA, 'NG-E-HAIR-FORMAT',
    `unsupported hairstyle schema ${spec.schema}; expected ${HAIRSTYLE_SCHEMA}`);
  identifier(spec.id, 'hairstyle.id');
  integer(spec.seed, 0, 0xFFFFFFFF, 'hairstyle.seed');

  keys(spec.character, ['headBone'], ['assetId', 'meshHash', 'scalpHash', 'characterHash', 'units', 'coordinateSystem'], 'character');
  fail(typeof spec.character.headBone === 'string' && spec.character.headBone.length > 0,
    'NG-E-HAIR-FORMAT', 'character.headBone is required: hair must bind a real skull joint');
  if (spec.character.units !== undefined) {
    fail(spec.character.units === 'metres', 'NG-E-HAIR-FORMAT', 'character.units must be metres');
  }

  fail(Array.isArray(spec.regions) && spec.regions.length >= 1 && spec.regions.length <= 16,
    'NG-E-HAIR-FORMAT', 'hairstyle.regions needs 1–16 entries');
  fail(Array.isArray(spec.material?.groups), 'NG-E-HAIR-FORMAT', 'material.groups must be an array');  const groupIds = new Set();
  spec.material.groups.forEach((group, index) => {
    validateMaterialGroup(group, index);
    fail(!groupIds.has(group.id), 'NG-E-HAIR-FORMAT', `duplicate material group ${group.id}`);
    groupIds.add(group.id);
  });
  const regionIds = new Set();
  spec.regions.forEach((region, index) => {
    validateRegion(region, index, groupIds);
    fail(!regionIds.has(region.id), 'NG-E-HAIR-FORMAT', `duplicate region id ${region.id}`);
    regionIds.add(region.id);
  });
  for (const lock of spec.locks ?? []) {
    keys(lock, ['region', 'fields'], [], 'lock');
    fail(regionIds.has(lock.region), 'NG-E-HAIR-FORMAT', `lock references unknown region ${lock.region}`);
    fail(Array.isArray(lock.fields) && lock.fields.length <= 16, 'NG-E-HAIR-FORMAT', 'lock.fields must be an array');
  }

  // Per-guide edits are stored in the source, not baked into a side file: an edit that
  // exists only in memory loses the ability to say which revision it belongs to, and
  // the plan requires every authored change to be revisable.
  fail(Array.isArray(spec.guideOverrides ?? []), 'NG-E-HAIR-FORMAT', 'guideOverrides must be an array');
  const overrideKeys = new Set();
  (spec.guideOverrides ?? []).forEach((override, index) => {
    const label = `guideOverrides[${index}]`;
    keys(override, ['guide'], ['region', 'lengthScale', 'curlScale', 'pointOffsets'], label);
    fail(typeof override.guide === 'string' && /^[a-z][a-z0-9_-]{0,15}#[0-9a-f]{6}$/.test(override.guide),
      'NG-E-HAIR-FORMAT', `${label}.guide must look like "<region>#<hex>"`);
    fail(!overrideKeys.has(override.guide), 'NG-E-HAIR-FORMAT', `duplicate guide override ${override.guide}`);
    overrideKeys.add(override.guide);
    if (override.lengthScale !== undefined) scalar(override.lengthScale, 0.2, 2, `${label}.lengthScale`);
    if (override.curlScale !== undefined) scalar(override.curlScale, 0, 3, `${label}.curlScale`);
    if (override.pointOffsets !== undefined) {
      fail(Array.isArray(override.pointOffsets) && override.pointOffsets.length <= 64,
        'NG-E-HAIR-FORMAT', `${label}.pointOffsets must be an array of at most 64 entries`);
      const seen = new Set();
      override.pointOffsets.forEach((entry, position) => {
        keys(entry, ['index', 'delta'], [], `${label}.pointOffsets[${position}]`);
        integer(entry.index, 0, 64, `${label}.pointOffsets[${position}].index`);
        vector(entry.delta, 3, -0.5, 0.5, `${label}.pointOffsets[${position}].delta`);
        fail(!seen.has(entry.index), 'NG-E-HAIR-FORMAT', `${label}.pointOffsets has a duplicate index`);
        seen.add(entry.index);
      });
    }
  });

  keys(spec.strands, ['widthMetres', 'rootJitterMetres'], ['streamsPerGuide', 'frizzMetres'], 'strands');
  scalar(spec.strands.widthMetres, 0.00002, 0.004, 'strands.widthMetres');
  scalar(spec.strands.rootJitterMetres, 0, 0.02, 'strands.rootJitterMetres');
  if (spec.strands.streamsPerGuide !== undefined) integer(spec.strands.streamsPerGuide, 1, 256, 'strands.streamsPerGuide');
  if (spec.strands.frizzMetres !== undefined) scalar(spec.strands.frizzMetres, 0, 0.05, 'strands.frizzMetres');

  if (spec.guides !== undefined) {
    keys(spec.guides, ['segments'], ['radiusMetres', 'maxGuides'], 'guides');
    integer(spec.guides.segments, 4, 48, 'guides.segments');
    if (spec.guides.radiusMetres !== undefined) scalar(spec.guides.radiusMetres, 0.00002, 0.01, 'guides.radiusMetres');
    if (spec.guides.maxGuides !== undefined) integer(spec.guides.maxGuides, 16, 8192, 'guides.maxGuides');
  }

  keys(spec.physics, ['gravityMetresPerSecond2'], [
    'bendCompliance', 'stretchCompliance', 'shapeStiffness', 'damping', 'substeps',
    'iterations', 'windResponse', 'maxSpeedMetresPerSecond', 'shoulderContact', 'sweptCollision',
  ], 'physics');
  scalar(spec.physics.gravityMetresPerSecond2, 0, 30, 'physics.gravityMetresPerSecond2');
  for (const key of ['bendCompliance', 'stretchCompliance', 'damping', 'windResponse']) {
    if (spec.physics[key] !== undefined) scalar(spec.physics[key], 0, 1, `physics.${key}`);
  }
  if (spec.physics.shapeStiffness !== undefined) scalar(spec.physics.shapeStiffness, 0, 1, 'physics.shapeStiffness');
  if (spec.physics.substeps !== undefined) integer(spec.physics.substeps, 1, 16, 'physics.substeps');
  if (spec.physics.iterations !== undefined) integer(spec.physics.iterations, 1, 32, 'physics.iterations');
  if (spec.physics.maxSpeedMetresPerSecond !== undefined) {
    scalar(spec.physics.maxSpeedMetresPerSecond, 0.1, 200, 'physics.maxSpeedMetresPerSecond');
  }
  if (spec.physics.shoulderContact !== undefined && spec.physics.shoulderContact !== null) {
    keys(spec.physics.shoulderContact, ['bones'], ['radiusMetres'], 'physics.shoulderContact');
    fail(Array.isArray(spec.physics.shoulderContact.bones) && spec.physics.shoulderContact.bones.length <= 8,
      'NG-E-HAIR-FORMAT', 'physics.shoulderContact.bones must list at most 8 bone names');
    for (const bone of spec.physics.shoulderContact.bones) {
      fail(typeof bone === 'string' && bone.length > 0, 'NG-E-HAIR-FORMAT', 'shoulderContact.bones entry must be a bone name');
    }
    if (spec.physics.shoulderContact.radiusMetres !== undefined) {
      scalar(spec.physics.shoulderContact.radiusMetres, 0.005, 0.5, 'physics.shoulderContact.radiusMetres');
    }
  }

  fail(Array.isArray(spec.lod) && spec.lod.length >= 1 && spec.lod.length <= 4,
    'NG-E-HAIR-FORMAT', 'hairstyle.lod needs 1–4 entries');
  let previousFraction = Infinity, previousDistance = 0;
  spec.lod.forEach((entry, index) => {
    keys(entry, ['level', 'strandFraction', 'segmentScale', 'maxDistanceMetres'], [], `lod[${index}]`);
    fail(entry.level === index, 'NG-E-HAIR-FORMAT', 'lod levels must be ordered from 0');
    scalar(entry.strandFraction, 0.01, 1, `lod[${index}].strandFraction`);
    scalar(entry.segmentScale, 0.2, 1, `lod[${index}].segmentScale`);
    scalar(entry.maxDistanceMetres, 0.1, 1000, `lod[${index}].maxDistanceMetres`);
    fail(entry.strandFraction <= previousFraction, 'NG-E-HAIR-FORMAT', 'lod strand fractions must not increase');
    fail(entry.maxDistanceMetres > previousDistance, 'NG-E-HAIR-FORMAT', 'lod distances must strictly increase');
    previousFraction = entry.strandFraction;
    previousDistance = entry.maxDistanceMetres;
    if (index === 0) {
      fail(entry.strandFraction === 1 && entry.segmentScale === 1, 'NG-E-HAIR-FORMAT',
        'lod0 must be the full strand set at full segment count');
    }
  });

  keys(spec.budget, ['maxStrands', 'maxPoints'], ['maxGuides'], 'budget');
  integer(spec.budget.maxStrands, 16, 400000, 'budget.maxStrands');
  integer(spec.budget.maxPoints, 64, 40000000, 'budget.maxPoints');
  if (spec.budget.maxGuides !== undefined) integer(spec.budget.maxGuides, 16, 8192, 'budget.maxGuides');

  return spec;
}

// Content revision. Two saves with the same numbers are the same revision; one changed
// digit is a different one. That is what makes `expectedRevision` a conflict check
// instead of a timestamp comparison, matching the vegetation and scene contracts.
export function hairstyleRevision(spec) {
  const payload = { ...spec };
  delete payload.__file;
  delete payload.__revision;
  return sha256Hex(canonicalJson(payload));
}

// Deterministic per-region, per-purpose seed derivation. Regions must not share a
// stream: editing `bangs` would then reshuffle `back`, which the plan forbids.
export function regionSeed(spec, regionId, purpose) {
  const text = `${spec.seed}:${spec.id}:${regionId}:${purpose}`;
  let hash = 2166136261;
  for (let index = 0; index < text.length; ++index) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash >>> 0;
}

export const DEFAULT_GUIDES = Object.freeze({ segments: 12, radiusMetres: 0.00007, maxGuides: 4096 });
export const DEFAULT_STRANDS = Object.freeze({ widthMetres: 0.00007, rootJitterMetres: 0.0012, streamsPerGuide: 40 });
export const DEFAULT_PHYSICS = Object.freeze({
  gravityMetresPerSecond2: 9.81, bendCompliance: 4e-4, stretchCompliance: 2e-6,
  shapeStiffness: 0.35, damping: 0.9, substeps: 2, iterations: 4,
  windResponse: 0.35, maxSpeedMetresPerSecond: 25, shoulderContact: null, sweptCollision: true,
});

// Fills every optional block with an explicit default. The cook never reads a
// `undefined` and guesses: a missing block means "the documented default", and the
// resolved value is written into the cooked metadata so a later reader can tell the
// difference between "author chose 12 segments" and "author omitted segments".
export function normalizeHairstyle(spec) {
  const normalized = structuredClone(spec);
  normalized.schema = HAIRSTYLE_SCHEMA;
  normalized.references = normalized.references ?? [];
  normalized.locks = normalized.locks ?? [];
  normalized.guideOverrides = normalized.guideOverrides ?? [];
  normalized.guides = { ...DEFAULT_GUIDES, ...(normalized.guides ?? {}) };
  normalized.strands = { ...DEFAULT_STRANDS, ...normalized.strands };
  normalized.physics = { ...DEFAULT_PHYSICS, ...normalized.physics };
  normalized.character.units = normalized.character.units ?? 'metres';
  normalized.character.coordinateSystem = normalized.character.coordinateSystem ?? 'left-handed-y-up';
  normalized.regions = normalized.regions.map((region) => {
    const resolved = {
      material: normalized.material.groups[0]?.id ?? null,
      simulation: 'guide',
      lockedFields: [],
      ...region,
    };
    // Optional blocks are defaulted only when the author supplied them. Writing
    // `curl: null` would be a third state the schema has to explain; omitting the key
    // means "flat" and the cook reads that as zero curl.
    if (region.curl) resolved.curl = { radiusMetres: 0.02, noise: 0.25, axisBias: [0, 1, 0], ...region.curl };
    if (region.clumping) resolved.clumping = { tipSpreadMetres: 0, ...region.clumping };
    if (region.frizz) resolved.frizz = { ...region.frizz };
    resolved.taper = region.taper ? { power: 1.4, ...region.taper } : { root: 1, tip: 0.35, power: 1.4 };
    return resolved;
  });
  return validateHairstyle(normalized);
}

export function regionById(spec, id) {
  return spec.regions.find((region) => region.id === id) ?? null;
}

// Region angular membership. Azimuth wraps at ±180 so a `back` sector can straddle
// the seam; comparing raw numbers would silently drop half of it.
export function insideRegion(region, azimuthDegrees, polarDegrees) {
  if (polarDegrees < region.polarDegrees[0] || polarDegrees > region.polarDegrees[1]) return false;
  const [start, end] = region.azimuthDegrees;
  const value = azimuthDegrees;
  const normalize = (angle) => ((angle + 540) % 360) - 180;
  if (start <= end) return value >= start && value <= end;
  return value >= start || value <= end;
}

// Angular distance to a region's azimuth centre, used for feathering locks and for
// deterministic selection when two sectors overlap.
export function azimuthDistance(region, azimuthDegrees) {
  return Math.abs(((azimuthDegrees - regionCentreAzimuth(region) + 540) % 360) - 180);
}

export function regionCentreAzimuth(region) {
  const [start, end] = region.azimuthDegrees;
  const span = start <= end ? end - start : end + 360 - start;
  return ((start + span / 2 + 540) % 360) - 180;
}

export function regionAzimuthSpan(region) {
  const [start, end] = region.azimuthDegrees;
  return start <= end ? end - start : end + 360 - start;
}

export function makeRegionId(base, existing) {
  let candidate = base, suffix = 2;
  while (existing.has(candidate)) { candidate = `${base}_${suffix++}`; }
  return candidate;
}

export { ID as HAIR_ID };

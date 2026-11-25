import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { HairError, hairstyleRevision, normalizeHairstyle, regionById } from './spec.mjs';

// Structured patches against a hairstyle source.
//
// Everything an AI asks for lands here, and this module is deliberately strict:
//
//  * a patch carries `expectedRevision`; a mismatch is a conflict, not a merge,
//  * a field that is locked is refused rather than silently ignored,
//  * a dry run returns the same diff the real application would produce, so "preview
//    then apply" cannot show one thing and do another,
//  * the diff names the regions that will be regenerated, which is how the caller
//    learns that touching `bangs` will not disturb `back`.

const REGION_FIELDS = new Set([
  'azimuthDegrees', 'polarDegrees', 'densityPerCm2', 'lengthMetres', 'comb', 'material',
  'parting', 'curl', 'clumping', 'frizz', 'taper', 'bundle', 'simulation',
]);

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

function deepEqual(a, b) {
  return canonicalJson(a) === canonicalJson(b);
}

export function hairRevision(spec) {
  return hairstyleRevision(normalizeHairstyle(spec));
}

// Which downstream fields a region edit invalidates. The list is explicit because it
// decides what "regenerate" means: changing a colour must not re-grow geometry.
const GEOMETRY_FIELDS = new Set([
  'azimuthDegrees', 'polarDegrees', 'densityPerCm2', 'lengthMetres', 'comb',
  'parting', 'curl', 'clumping', 'frizz', 'taper', 'bundle',
]);

export function validatePatch(patch) {
  fail(patch && typeof patch === 'object' && !Array.isArray(patch), 'NG-E-HAIR-PATCH', 'patch must be an object');
  const allowed = new Set(['expectedRevision', 'operations', 'requestId', 'note']);
  for (const key of Object.keys(patch)) {
    fail(allowed.has(key), 'NG-E-HAIR-PATCH', `patch.${key} is unsupported`);
  }
  fail(typeof patch.expectedRevision === 'string' && patch.expectedRevision.length >= 8,
    'NG-E-HAIR-PATCH', 'patch.expectedRevision is required: a patch without a base revision can overwrite another edit');
  if (patch.requestId !== undefined) {
    fail(typeof patch.requestId === 'string' && /^[A-Za-z0-9._:-]{1,64}$/.test(patch.requestId),
      'NG-E-HAIR-PATCH', 'patch.requestId must be 1–64 characters of [A-Za-z0-9._:-]');
  }
  fail(Array.isArray(patch.operations) && patch.operations.length >= 1 && patch.operations.length <= 64,
    'NG-E-HAIR-PATCH', 'patch.operations needs 1–64 entries');
  for (const [index, operation] of patch.operations.entries()) {
    const label = `operations[${index}]`;
    fail(operation && typeof operation === 'object' && !Array.isArray(operation), 'NG-E-HAIR-PATCH',
      `${label} must be an object`);
    fail(['region', 'guide', 'lock', 'unlock'].includes(operation.kind), 'NG-E-HAIR-PATCH',
      `${label}.kind must be region, guide, lock or unlock`);
    if (operation.kind === 'region') {
      fail(typeof operation.region === 'string', 'NG-E-HAIR-PATCH', `${label}.region is required`);
      fail(operation.changes && typeof operation.changes === 'object' && !Array.isArray(operation.changes),
        'NG-E-HAIR-PATCH', `${label}.changes is required`);
      for (const key of Object.keys(operation.changes)) {
        fail(REGION_FIELDS.has(key), 'NG-E-HAIR-PATCH', `${label}.changes.${key} is not an editable region field`);
      }
    } else if (operation.kind === 'guide') {
      fail(typeof operation.guide === 'string' && /^[a-z][a-z0-9_-]{0,15}#[0-9a-f]{6}$/.test(operation.guide),
        'NG-E-HAIR-PATCH', `${label}.guide must look like "<region>#<hex>"`);
      fail(operation.changes && typeof operation.changes === 'object', 'NG-E-HAIR-PATCH',
        `${label}.changes is required`);
      for (const key of Object.keys(operation.changes)) {
        fail(['lengthScale', 'curlScale', 'pointOffsets'].includes(key), 'NG-E-HAIR-PATCH',
          `${label}.changes.${key} is not an editable guide field`);
      }
    } else {
      fail(typeof operation.region === 'string', 'NG-E-HAIR-PATCH', `${label}.region is required`);
      fail(Array.isArray(operation.fields) && operation.fields.length >= 1 && operation.fields.length <= 16,
        'NG-E-HAIR-PATCH', `${label}.fields needs 1–16 field names`);
      for (const field of operation.fields) {
        fail(REGION_FIELDS.has(field), 'NG-E-HAIR-PATCH', `${label}.fields entry ${field} is not an editable field`);
      }
    }
  }
  return patch;
}

function lockedFieldsOf(spec, regionId) {
  const region = regionById(spec, regionId);
  if (!region) return new Set();
  const merged = new Set(region.lockedFields ?? []);
  for (const lock of spec.locks ?? []) {
    if (lock.region === regionId) for (const field of lock.fields) merged.add(field);
  }
  return merged;
}

// Pure: validates and produces the candidate source plus a diff. No I/O, which is what
// makes the dry run and the real application the same computation.
export function applyPatch(currentSpec, patch, { knownGuides = null } = {}) {
  validatePatch(patch);
  const spec = normalizeHairstyle(currentSpec);
  const revision = hairstyleRevision(spec);
  if (patch.expectedRevision !== revision) {
    throw new HairError('NG-E-HAIR-REVISION',
      ` hairstyle ${spec.id} is at revision ${revision.slice(0, 12)}; the patch expects `
      + `${String(patch.expectedRevision).slice(0, 12)}. Re-read the hairstyle and re-apply the patch.`,
      [{ currentRevision: revision, expectedRevision: patch.expectedRevision }]);
  }

  const candidate = structuredClone(spec);
  const changedRegions = new Set();
  const geometryRegions = new Set();
  const materialRegions = new Set();
  const guideEdits = [];
  const lockChanges = [];
  const conflicts = [];

  for (const operation of patch.operations) {
    if (operation.kind === 'lock' || operation.kind === 'unlock') {
      const region = candidate.regions.find((entry) => entry.id === operation.region);
      fail(region, 'NG-E-HAIR-PATCH', `operation references unknown region ${operation.region}`);
      const current = new Set(region.lockedFields ?? []);
      for (const field of operation.fields) {
        if (operation.kind === 'lock') current.add(field); else current.delete(field);
      }
      region.lockedFields = [...current].sort();
      lockChanges.push({ region: operation.region, fields: region.lockedFields, kind: operation.kind });
      continue;
    }

    const regionId = operation.kind === 'guide' ? operation.guide.split('#')[0] : operation.region;
    const target = candidate.regions.find((entry) => entry.id === regionId);
    fail(target, 'NG-E-HAIR-PATCH', `${operation.kind} operation references unknown region ${regionId}`);

    if (operation.kind === 'region') {
      const locked = lockedFieldsOf(candidate, regionId);
      const blocked = Object.keys(operation.changes).filter((field) => locked.has(field));
      if (blocked.length) {
        // Refusing is the point: a lock that silently drops the edit teaches the caller
        // that locks can be ignored, and the next patch overwrites confirmed work.
        throw new HairError('NG-E-HAIR-LOCKED',
          `region ${regionId} has ${blocked.join(', ')} locked; unlock it explicitly before patching`,
          [{ region: regionId, lockedFields: [...locked], blockedFields: blocked }]);
      }
      for (const [field, value] of Object.entries(operation.changes)) {
        if (deepEqual(target[field], value)) continue;
        target[field] = structuredClone(value);
        changedRegions.add(regionId);
        if (GEOMETRY_FIELDS.has(field)) geometryRegions.add(regionId); else materialRegions.add(regionId);
      }
      continue;
    }

    // Guide edit: only an existing guide can be edited. Editing a guide that the
    // generator does not produce means the caller is working from a stale generation.
    if (knownGuides && !knownGuides.has(operation.guide)) {
      throw new HairError('NG-E-HAIR-UNKNOWN-GUIDE',
        `guide ${operation.guide} does not exist in the current generation; re-read the hairstyle first`,
        [{ guide: operation.guide, region: regionId }]);
    }
    const overrides = candidate.guideOverrides ?? (candidate.guideOverrides = []);
    let override = overrides.find((entry) => entry.guide === operation.guide);
    if (!override) {
      override = { guide: operation.guide, region: regionId };
      overrides.push(override);
    }
    for (const [field, value] of Object.entries(operation.changes)) {
      if (deepEqual(override[field], value)) continue;
      override[field] = structuredClone(value);
      changedRegions.add(regionId);
      geometryRegions.add(regionId);
      guideEdits.push({ guide: operation.guide, field, value });
    }
    if (Object.keys(override).length === 2) conflicts.push({
      code: 'NG-E-HAIR-NOOP', guide: operation.guide, message: 'the guide edit changed nothing',
    });
  }

  const validated = normalizeHairstyle(candidate);
  const nextRevision = hairstyleRevision(validated);
  const diff = {
    revision: nextRevision,
    previousRevision: revision,
    changedRegions: [...changedRegions].sort(),
    regeneratedRegions: [...geometryRegions].sort(),
    materialOnlyRegions: [...materialRegions].sort(),
    guideEdits,
    lockChanges,
    conflicts,
    untouchedRegions: validated.regions.map((region) => region.id).filter((id) => !changedRegions.has(id)).sort(),
    changed: nextRevision !== revision,
  };
  return { spec: validated, diff };
}

export function patchDigest(diff) {
  return sha256Hex(canonicalJson({
    revision: diff.revision, regions: diff.changedRegions, guides: diff.guideEdits,
  })).slice(0, 16);
}

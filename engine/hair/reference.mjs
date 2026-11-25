import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { HairError, REGION_PRESETS } from './spec.mjs';

// Reference-image registration and AI analysis intake.
//
// The engine does not run a vision model. It accepts the analysis the calling AI
// produced and refuses to pretend the analysis is a measurement:
//
//  * every field carries a confidence and a provenance (visible / inferred),
//  * two views that disagree produce a diagnostic, never an average,
//  * anything the images cannot show is recorded as inferred and must be reviewed
//    with a back view before it is treated as a hairstyle decision.
//
// That last point is the difference between "the AI guessed the back of the head" and
// "the hairstyle has a back that nobody looked at".

export const REFERENCE_SCHEMA = 'nativegpu.hair-reference/1';
export const ANALYSIS_SCHEMA = 'nativegpu.hair-analysis/1';

const VIEWS = new Set([
  'front', 'back', 'left', 'right', 'three_quarter_left', 'three_quarter_right', 'top', 'unknown',
]);
const REFERENCE_ID = /^[a-z][a-z0-9_-]{0,47}$/;

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

// Fields a hairstyle can be inferred from, with the tolerance below which two views
// are considered to agree. A tight tolerance on `colorAlbedo` would produce conflicts
// from two photographs of the same hair under different light, so colour is graded
// loosely while length is graded in millimetres.
export const ANALYSIS_FIELDS = Object.freeze({
  lengthMetres: { tolerance: 0.015, kind: 'number', region: 'all' },
  partingAzimuthDegrees: { tolerance: 12, kind: 'number', region: 'crown' },
  partingWidthDegrees: { tolerance: 4, kind: 'number', region: 'crown' },
  curlTurnsPerMetre: { tolerance: 12, kind: 'number', region: 'all' },
  crownVolumeMetres: { tolerance: 0.012, kind: 'number', region: 'crown' },
  backVolumeMetres: { tolerance: 0.015, kind: 'number', region: 'back' },
  sideVolumeMetres: { tolerance: 0.015, kind: 'number', region: 'left_side' },
  strandWidthMetres: { tolerance: 0.00006, kind: 'number', region: 'all' },
  frizzMetres: { tolerance: 0.004, kind: 'number', region: 'all' },
  hairlineAzimuthDegrees: { tolerance: 10, kind: 'number', region: 'bangs' },
  bangsBottomMetres: { tolerance: 0.02, kind: 'number', region: 'bangs' },
  colorAlbedo: { tolerance: 0.14, kind: 'colour', region: 'all' },
  roughness: { tolerance: 0.22, kind: 'number', region: 'all' },
  style: { tolerance: null, kind: 'enum', region: 'all' },
});

const HAIR_STYLES = new Set(['straight', 'wavy', 'curly', 'coily', 'braided', 'ponytail', 'bun', 'short', 'bob']);

// PNG/JPEG dimensions are read from the header instead of decoding the image: the
// engine needs to prove the reference is a real image of a plausible size, not to
// render it.
function imageHeader(bytes) {
  if (bytes.length >= 24 && bytes.readUInt32BE(0) === 0x89504e47) {
    return { format: 'png', width: bytes.readUInt32BE(16), height: bytes.readUInt32BE(20) };
  }
  if (bytes.length >= 4 && bytes[0] === 0xFF && bytes[1] === 0xD8) {
    let offset = 2;
    while (offset + 9 < bytes.length) {
      if (bytes[offset] !== 0xFF) { offset += 1; continue; }
      const marker = bytes[offset + 1];
      const length = bytes.readUInt16BE(offset + 2);
      if (marker >= 0xC0 && marker <= 0xCF && marker !== 0xC4 && marker !== 0xC8 && marker !== 0xCC) {
        return { format: 'jpeg', width: bytes.readUInt16BE(offset + 7), height: bytes.readUInt16BE(offset + 5) };
      }
      offset += 2 + length;
    }
  }
  return null;
}

export async function registerReferenceFile(file, {
  id, view = 'unknown', crop = null, subject = '', hairlineVisible = null, confidence = 0.5,
  authorized = true, license = null,
} = {}) {
  fail(authorized === true, 'NG-E-HAIR-REFERENCE',
    'reference images must be explicitly authorized; the engine never uploads a project on its own');
  const bytes = await fs.readFile(file);
  const hash = createHash('sha256').update(bytes).digest('hex');
  const header = imageHeader(bytes);
  fail(header !== null, 'NG-E-HAIR-REFERENCE', `${path.basename(file)} is not a PNG or JPEG image`);
  const referenceId = id ?? `ref_${hash.slice(0, 8)}`;
  fail(REFERENCE_ID.test(referenceId), 'NG-E-HAIR-REFERENCE',
    'reference id must be lowercase letters, digits, dash or underscore (max 48)');
  fail(VIEWS.has(view), 'NG-E-HAIR-REFERENCE', `view must be one of ${[...VIEWS].join(', ')}`);
  if (crop !== null) {
    fail(crop && [crop.x, crop.y, crop.width, crop.height].every((value) => Number.isFinite(value))
      && crop.width > 0 && crop.height > 0 && crop.x >= 0 && crop.y >= 0
      && crop.x + crop.width <= header.width && crop.y + crop.height <= header.height,
      'NG-E-HAIR-REFERENCE', 'crop must be inside the image');
  }
  fail(typeof confidence === 'number' && confidence >= 0 && confidence <= 1,
    'NG-E-HAIR-REFERENCE', 'confidence must be in [0,1]');
  return {
    schema: REFERENCE_SCHEMA,
    id: referenceId,
    hash,
    file: path.basename(file),
    format: header.format,
    width: header.width,
    height: header.height,
    bytes: bytes.length,
    view,
    crop,
    subject,
    hairlineVisible: hairlineVisible === null ? view !== 'back' : Boolean(hairlineVisible),
    confidence,
    license,
    registeredAt: null,
  };
}

// A view that cannot show the back of the head cannot justify a decision about it.
export function viewCoversField(view, field) {
  const spec = ANALYSIS_FIELDS[field];
  if (!spec) return false;
  if (view === 'unknown') return false;
  if (spec.region === 'all') return view === 'front' || view === 'back' || view === 'left' || view === 'right'
    || view === 'three_quarter_left' || view === 'three_quarter_right';
  if (spec.region === 'back') return view === 'back';
  if (spec.region === 'crown') return view === 'front' || view === 'top' || view === 'back'
    || view === 'three_quarter_left' || view === 'three_quarter_right';
  if (spec.region === 'bangs') return view === 'front' || view === 'three_quarter_left' || view === 'three_quarter_right';
  if (spec.region === 'left_side') return view === 'left' || view === 'three_quarter_left' || view === 'front';
  return true;
}

function validateObservation(observation, label, field) {
  fail(observation && typeof observation === 'object' && !Array.isArray(observation), 'NG-E-HAIR-ANALYSIS',
    `${label} must be an object`);
  const allowed = new Set(['field', 'value', 'confidence', 'provenance', 'note']);
  for (const key of Object.keys(observation)) {
    fail(allowed.has(key), 'NG-E-HAIR-ANALYSIS', `${label}.${key} is unsupported`);
  }
  fail(observation.field === field, 'NG-E-HAIR-ANALYSIS', `${label}.field must be ${field}`);
  const spec = ANALYSIS_FIELDS[field];
  fail(spec, 'NG-E-HAIR-ANALYSIS',
    `${label}.field must be one of ${Object.keys(ANALYSIS_FIELDS).join(', ')}`);
  fail(typeof observation.confidence === 'number' && observation.confidence >= 0 && observation.confidence <= 1,
    'NG-E-HAIR-ANALYSIS', `${label}.confidence must be in [0,1]`);
  fail(['visible', 'inferred'].includes(observation.provenance), 'NG-E-HAIR-ANALYSIS',
    `${label}.provenance must be visible or inferred`);
  if (spec.kind === 'number') {
    fail(typeof observation.value === 'number' && Number.isFinite(observation.value), 'NG-E-HAIR-ANALYSIS',
      `${label}.value must be a finite number`);
  } else if (spec.kind === 'colour') {
    fail(Array.isArray(observation.value) && observation.value.length === 3
      && observation.value.every((value) => typeof value === 'number' && value >= 0 && value <= 1),
      'NG-E-HAIR-ANALYSIS', `${label}.value must be an RGB triple in [0,1]`);
  } else if (spec.kind === 'enum') {
    fail(HAIR_STYLES.has(observation.value), 'NG-E-HAIR-ANALYSIS',
      `${label}.value must be one of ${[...HAIR_STYLES].join(', ')}`);
  }
}

export function validateAnalysis(analysis) {
  fail(analysis && typeof analysis === 'object', 'NG-E-HAIR-ANALYSIS', 'analysis must be an object');
  fail(analysis.schema === ANALYSIS_SCHEMA, 'NG-E-HAIR-ANALYSIS',
    `analysis.schema must be ${ANALYSIS_SCHEMA}`);
  fail(typeof analysis.referenceId === 'string' && analysis.referenceId.length > 0,
    'NG-E-HAIR-ANALYSIS', 'analysis.referenceId is required');
  fail(Array.isArray(analysis.observations) && analysis.observations.length <= 64,
    'NG-E-HAIR-ANALYSIS', 'analysis.observations must be an array of at most 64 entries');
  const seen = new Set();
  analysis.observations.forEach((observation, index) => {
    const label = `analysis.observations[${index}]`;
    fail(typeof observation?.field === 'string', 'NG-E-HAIR-ANALYSIS', `${label}.field is required`);
    validateObservation(observation, label, observation.field);
    fail(!seen.has(observation.field), 'NG-E-HAIR-ANALYSIS',
      `${label}.field ${observation.field} appears twice in one analysis; provide one value per view`);
    seen.add(observation.field);
  });
  if (analysis.camera !== undefined && analysis.camera !== null) {
    const camera = analysis.camera;
    fail(camera && typeof camera === 'object', 'NG-E-HAIR-ANALYSIS', 'analysis.camera must be an object');
    for (const key of ['yawDegrees', 'pitchDegrees', 'fovYDegrees']) {
      if (camera[key] !== undefined) {
        fail(typeof camera[key] === 'number' && Number.isFinite(camera[key]), 'NG-E-HAIR-ANALYSIS',
          `analysis.camera.${key} must be a finite number`);
      }
    }
    if (camera.estimated !== undefined && camera.estimated !== true && camera.estimated !== false) {
      fail(false, 'NG-E-HAIR-ANALYSIS', 'analysis.camera.estimated must be boolean');
    }
  }
  return analysis;
}

function distance(a, b, field) {
  const spec = ANALYSIS_FIELDS[field];
  if (spec.kind === 'colour') {
    return Math.max(...a.map((value, index) => Math.abs(value - b[index])));
  }
  if (spec.kind === 'enum') return a === b ? 0 : Infinity;
  return Math.abs(a - b);
}

// Multi-view reconciliation.
//
// Visible observations win. When two visible observations of the same field disagree
// beyond tolerance the result is a *conflict*, not an average: averaging a fringe that
// two photographs place differently produces a fringe that matches neither. Inferred
// observations only fill fields nothing visible covered, and they are flagged so the
// caller can demand a back view.
export function reconcileAnalyses(references, analyses) {
  fail(Array.isArray(references) && references.length > 0, 'NG-E-HAIR-ANALYSIS',
    'at least one registered reference is required');
  const byId = new Map(references.map((reference) => [reference.id, reference]));
  const results = new Map();
  const conflicts = [];
  const inferredOnly = new Set();
  const uncovered = new Set(Object.keys(ANALYSIS_FIELDS));
  const diagnostics = [];

  for (const raw of analyses) {
    const analysis = validateAnalysis(raw);
    const reference = byId.get(analysis.referenceId);
    fail(reference, 'NG-E-HAIR-ANALYSIS',
      `analysis references ${analysis.referenceId}, which is not a registered reference`);
    for (const observation of analysis.observations) {
      uncovered.delete(observation.field);
      const covers = viewCoversField(reference.view, observation.field);
      if (observation.provenance === 'visible' && !covers) {
        // Claiming to see the back of the head from the front is the single most
        // common way an image analysis turns into an invented hairstyle.
        diagnostics.push({
          code: 'NG-E-HAIR-VIEW-MISMATCH', level: 'error', field: observation.field,
          reference: reference.id, view: reference.view,
          message: `view ${reference.view} cannot show ${observation.field}; the observation was ignored`,
        });
        continue;
      }
      if (observation.provenance === 'visible' && reference.hairlineVisible === false
          && observation.field === 'hairlineAzimuthDegrees') {
        diagnostics.push({
          code: 'NG-E-HAIR-HAIRLINE-HIDDEN', level: 'warning', field: observation.field,
          reference: reference.id, message: 'the hairline is marked as hidden in this view',
        });
        continue;
      }
      const list = results.get(observation.field) ?? [];
      list.push({
        value: observation.value,
        confidence: observation.confidence * reference.confidence,
        provenance: observation.provenance,
        reference: reference.id,
        view: reference.view,
        note: observation.note ?? null,
      });
      results.set(observation.field, list);
    }
  }

  const resolved = {};
  const provenance = {};
  for (const [field, observations] of results) {
    const visible = observations.filter((entry) => entry.provenance === 'visible');
    const inferred = observations.filter((entry) => entry.provenance === 'inferred');
    if (visible.length === 0) {
      // Only inferred observations exist for this field.
      const best = inferred.sort((a, b) => b.confidence - a.confidence)[0];
      resolved[field] = best.value;
      provenance[field] = { provenance: 'inferred', confidence: best.confidence, reference: best.reference, views: [best.view] };
      inferredOnly.add(field);
      continue;
    }
    const tolerance = ANALYSIS_FIELDS[field].tolerance;
    let worst = null;
    for (let i = 0; i < visible.length; ++i) {
      for (let j = i + 1; j < visible.length; ++j) {
        const delta = distance(visible[i].value, visible[j].value, field);
        if (delta > tolerance) {
          const conflict = {
            code: 'NG-E-HAIR-CONFLICT', level: 'error', field,
            views: [visible[i].view, visible[j].view],
            values: [visible[i].value, visible[j].value],
            delta: Math.round(delta * 1e6) / 1e6, tolerance,
            message: `${field} differs between ${visible[i].view} and ${visible[j].view}; `
              + 'the views were not averaged — resolve which view is correct',
          };
          conflicts.push(conflict);
          if (!worst || delta > worst.delta) worst = conflict;
        }
      }
    }
    if (worst) {
      // Still expose the highest-confidence visible value so the caller can create a
      // diagnostic hairstyle, but mark the field unresolved: it must not be treated as
      // a decision.
      const best = visible.sort((a, b) => b.confidence - a.confidence)[0];
      resolved[field] = best.value;
      provenance[field] = {
        provenance: 'conflict', confidence: best.confidence, reference: best.reference,
        views: visible.map((entry) => entry.view), conflict: worst,
      };
      continue;
    }
    const best = visible.sort((a, b) => b.confidence - a.confidence)[0];
    resolved[field] = best.value;
    provenance[field] = {
      provenance: 'visible', confidence: best.confidence, reference: best.reference,
      views: visible.map((entry) => entry.view),
    };
  }

  // A field with no observation at all is not "zero": it is unknown, and the caller
  // must either provide another view or accept a documented style prior.
  for (const field of uncovered) {
    diagnostics.push({
      code: 'NG-E-HAIR-UNOBSERVED', level: 'warning', field,
      message: `${field} was not observed in any reference view`,
    });
  }
  if (inferredOnly.size > 0) {
    diagnostics.push({
      code: 'NG-E-HAIR-BACK-VIEW-REQUIRED', level: 'warning',
      fields: [...inferredOnly],
      message: 'some fields are inferred, not observed; capture a back view and re-analyse before approval',
    });
  }

  return {
    fields: resolved,
    provenance,
    conflicts,
    inferredFields: [...inferredOnly],
    unobservedFields: [...uncovered],
    diagnostics,
    requiresBackView: inferredOnly.size > 0,
    references: references.map((reference) => ({ id: reference.id, hash: reference.hash, view: reference.view })),
    digest: sha256Hex(canonicalJson({ references: references.map((r) => r.hash), fields: resolved, conflicts })),
  };
}

// Observed values → concrete hairstyle parameters, per region.
//
// An inferred value is allowed to shape the result, but it is reported separately so a
// reviewer can see which parts of the hairstyle no photograph supports.
export function regionsFromReconciliation(reconciliation, { style = null } = {}) {
  const fields = reconciliation.fields;
  const inferred = new Set(reconciliation.inferredFields);
  const lengthMetres = fields.lengthMetres ?? 0.16;
  const curl = fields.curlTurnsPerMetre ?? 0;
  const chosenStyle = style ?? fields.style ?? (curl > 20 ? 'curly' : 'straight');
  const regions = [];
  const decide = (id, preset, regionLength, comb) => {
    const region = {
      id,
      ...REGION_PRESETS[id],
      densityPerCm2: chosenStyle === 'short' ? 170 : 150,
      lengthMetres: regionLength,
      comb,
    };
    if (id === 'crown' && fields.partingAzimuthDegrees !== undefined) {
      region.parting = {
        azimuthDegrees: fields.partingAzimuthDegrees,
        widthDegrees: fields.partingWidthDegrees ?? 5,
      };
    }
    if (curl > 0) {
      region.curl = { turnsPerMetre: curl, radiusMetres: 0.02, noise: 0.25, axisBias: [0, 1, 0] };
    }
    if (fields.frizzMetres !== undefined) {
      region.frizz = { amplitudeMetres: fields.frizzMetres, frequency: 9 + Math.round(fields.frizzMetres * 4000) };
    }
    if (fields.strandWidthMetres !== undefined) {
      region.taper = { root: 1, tip: 0.35, power: 1.4 };
    }
    if (chosenStyle === 'braided' || chosenStyle === 'ponytail' || chosenStyle === 'bun') {
      region.bundle = {
        kind: chosenStyle === 'braided' ? 'braid' : chosenStyle,
        count: chosenStyle === 'braided' ? 2 : 1,
        turnPerMetre: chosenStyle === 'braided' ? 12 : 0,
        anchorAzimuthDegrees: 180,
      };
    }
    regions.push(region);
  };
  // Bangs length comes from the fringe bottom when it was observed; otherwise the
  // overall length with a documented prior (fringe slightly shorter than the sides).
  decide('bangs', REGION_PRESETS.bangs, fields.bangsBottomMetres ?? lengthMetres * 0.8, [0, -0.25, 1]);
  decide('crown', REGION_PRESETS.crown, lengthMetres * (1 + (fields.crownVolumeMetres ?? 0) * 2), [0, -0.35, 0.5]);
  decide('left_side', REGION_PRESETS.left_side, lengthMetres * (1 + (fields.sideVolumeMetres ?? 0)), [0.45, -0.5, 0.25]);
  decide('right_side', REGION_PRESETS.right_side, lengthMetres * (1 + (fields.sideVolumeMetres ?? 0)), [-0.45, -0.5, 0.25]);
  decide('back', REGION_PRESETS.back, lengthMetres * (1 + (fields.backVolumeMetres ?? 0) * 1.5), [0, -0.5, -0.45]);
  return {
    regions,
    style: chosenStyle,
    inferredFields: [...inferred],
    colour: fields.colorAlbedo ?? null,
    roughness: fields.roughness ?? null,
  };
}

export const REFERENCE_VIEWS = Object.freeze([...VIEWS]);

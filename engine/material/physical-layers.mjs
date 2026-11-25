// Versioned reference for the second-stage physical layer stack.
// This module is a contract and CPU reference only: the production pixel shader
// still evaluates phase-one parameter blends. Enabling it in the runtime needs a
// separate capability flag and picture validation.
export const PHYSICAL_LAYERS_SCHEMA = 'nativegpu.physical-layers/1';
export const PHYSICAL_LAYERS_VERSION = 'physical-layers/1';

const fail = (path, message) => { const error = new Error(`${path}: ${message}`); error.code = 'NG-E-MAT-PHYSICAL'; error.path = path; throw error; };
const finite = (value, path) => { if (typeof value !== 'number' || !Number.isFinite(value)) fail(path, 'must be a finite number'); return value; };
const unit = (value, path) => { const v = finite(value, path); if (v < 0 || v > 1) fail(path, 'must be in [0, 1]'); return v; };
const vector = (value, path) => { if (!Array.isArray(value) || value.length !== 3) fail(path, 'must be a 3-vector'); return value.map((v, i) => finite(v, `${path}[${i}]`)); };
const direction = (value, path) => { const v = vector(value, path); const length = Math.hypot(...v); if (length < 1e-8) fail(path, 'must not be zero'); return v.map(x => x / length); };
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const surface = (value, path) => {
  if (!value || typeof value !== 'object') fail(path, 'must describe a surface');
  const albedo = vector(value.albedo, `${path}.albedo`); albedo.forEach((v, i) => unit(v, `${path}.albedo[${i}]`));
  const roughness = unit(value.roughness, `${path}.roughness`);
  const normal = value.normal === undefined ? [0, 0, 1] : direction(value.normal, `${path}.normal`);
  return { albedo, roughness, normal };
};

/** Schlick Fresnel for a dielectric interface. `cosTheta` is the absolute cosine. */
export function fresnelSchlick(f0, cosTheta) {
  unit(f0, 'f0'); unit(cosTheta, 'cosTheta');
  return f0 + (1 - f0) * Math.pow(1 - cosTheta, 5);
}

/** GGX normal-distribution lobe shapes used by both stages for comparison. */
export function specularLobe(cosHalf, roughness) {
  unit(cosHalf, 'cosHalf'); const rough = finite(roughness, 'roughness'); if (rough < 0.001 || rough > 1) fail('roughness', 'must be in [0.001, 1]');
  const a = Math.max(rough * rough, 0.0025), a2 = a * a, nh = Math.max(cosHalf, 0), denominator = nh * nh * (a2 - 1) + 1;
  return a2 / Math.max(Math.PI * denominator * denominator, 1e-8);
}

/**
 * Evaluate one physical coat over an opaque base.
 *
 * Semantics (frozen for nativegpu.physical-layers/1):
 * - `coverage` scales how much of the coat exists; 0 returns the base unchanged.
 * - `transmission` scales the base light that survives the coat; it cannot be
 *   raised by coverage and is independent of it.
 * - Reflection uses Fresnel against the coat normal; transmission uses its
 *   complement for a non-absorbing layer, so reflected + transmitted <= 1.
 */
export function evaluateCoatClosure({ base: baseValue, coat: coatValue, viewDirection, lightDirection, coverage = 1, transmission = 1 } = {}) {
  const base = surface(baseValue, 'base');
  const coat = surface(coatValue, 'coat');
  const view = direction(viewDirection, 'viewDirection'), light = direction(lightDirection, 'lightDirection');
  const cover = unit(coverage, 'coverage'), through = unit(transmission, 'transmission');
  const half = direction([view[0] + light[0], view[1] + light[1], view[2] + light[2]], 'halfVector');
  const F = fresnelSchlick(coat.f0 ?? 0.04, Math.abs(dot(coat.normal, view))) * cover;
  const transmitted = (1 - F) * through;
  return {
    reflectedShare: F,
    transmittedShare: transmitted,
    coatSpecular: specularLobe(Math.abs(dot(coat.normal, half)), coat.roughness) * F,
    baseDiffuse: base.albedo.map(v => v * transmitted),
    baseSpecular: specularLobe(Math.abs(dot(base.normal, half)), base.roughness) * transmitted,
  };
}

/**
 * Phase-one reference: blend parameters and evaluate one PBR lobe. Provided for
 * comparison; it cannot represent two separate reflection peaks.
 */
export function evaluateParameterMix({ bottom: bottomValue, top: topValue, coverage, viewDirection, lightDirection } = {}) {
  const bottom = surface(bottomValue, 'bottom'), top = surface(topValue, 'top');
  const cover = unit(coverage, 'coverage');
  const view = direction(viewDirection, 'viewDirection'), light = direction(lightDirection, 'lightDirection');
  const half = direction([view[0] + light[0], view[1] + light[1], view[2] + light[2]], 'halfVector');
  const normal = bottom.normal.map((v, i) => v + (top.normal[i] - v) * cover);
  const length = Math.hypot(...normal) || 1;
  const blendedNormal = normal.map(v => v / length);
  const rough = bottom.roughness + (top.roughness - bottom.roughness) * cover;
  return {
    roughness: rough,
    albedo: bottom.albedo.map((v, i) => v + (top.albedo[i] - v) * cover),
    specular: specularLobe(Math.abs(dot(blendedNormal, half)), rough),
  };
}

/** Canonical object used by the compiler identity for this stage. */
export function physicalLayerIdentity() {
  return { schema: PHYSICAL_LAYERS_SCHEMA, version: PHYSICAL_LAYERS_VERSION, closures: ['opaque-base', 'non-absorbing-coat'], coverageSeparateFromTransmission: true };
}

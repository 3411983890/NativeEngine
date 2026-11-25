// CPU reference for surface relief height semantics and parallax-occlusion
// ray intersection. The shader mirrors this math; changes must stay in lockstep
// with shaders/material_layer_sampling.hlsli.
import { surfaceHeightMetres } from './layered.mjs';

const finite = (value, label) => {
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error(`${label} must be finite`);
  return value;
};
const vec3 = (value, label) => {
  if (!Array.isArray(value) || value.length !== 3) throw new Error(`${label} must be three numbers`);
  return value.map((v) => finite(v, label));
};
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const length = (a) => Math.hypot(a[0], a[1], a[2]);
const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
function normalize(value, label) {
  const n = length(value);
  if (!(n > 1e-12)) throw new Error(`${label} must not be zero`);
  return scale(value, 1 / n);
}

/**
 * Surface basis for one UV set. `dpdu`/`dpdv` are world-space position
 * derivatives per one UV unit. Returns an orthonormal frame plus the world
 * metres covered by one UV unit on each axis.
 */
export function surfaceBasis({ normal, dpdu, dpdv }) {
  const n = normalize(vec3(normal, 'normal'), 'normal');
  const du = vec3(dpdu, 'dpdu'), dv = vec3(dpdv, 'dpdv');
  const lenU = length(du), lenV = length(dv);
  if (!(lenU > 1e-12) || !(lenV > 1e-12)) throw new Error('UV derivatives must not be degenerate');
  const projectedU = sub(du, scale(n, dot(n, du)));
  if (length(projectedU) < 1e-12 * lenU) throw new Error('UV U axis is parallel to the surface normal');
  const tangent = normalize(projectedU, 'tangent');
  let bitangent = normalize(cross(n, tangent), 'bitangent');
  if (dot(bitangent, dv) < 0) bitangent = scale(bitangent, -1);
  return { normal: n, tangent, bitangent, metresPerU: lenU, metresPerV: lenV };
}

/** Project a world-space tangential displacement back into one UV set. */
export function projectDisplacement(basis, displacement) {
  const d = vec3(displacement, 'displacement');
  return [dot(d, basis.tangent) / basis.metresPerU, dot(d, basis.bitangent) / basis.metresPerV];
}

/**
 * March a view ray from the surface into the height field.
 *
 * `sampleHeight(u,v)` returns normalized height in [0,1]. The march runs from
 * the top reference plane inward, so only negative displacement is searched.
 * Returns the UV offset and the physical depth along the surface normal.
 */
export function marchParallax({ relief, uv, view, basis, sampleHeight, distanceMetres = 0 }) {
  if (typeof sampleHeight !== 'function') throw new Error('sampleHeight must be a function');
  const [u0, v0] = Array.isArray(uv) && uv.length === 2 ? uv.map((v) => finite(v, 'uv')) : (() => { throw new Error('uv must be two numbers'); })();
  const V = normalize(vec3(view, 'view'), 'view');
  const n = basis.normal;
  const vn = dot(V, n);
  const amplitude = finite(relief?.amplitudeMetres ?? 0, 'amplitudeMetres');
  const reference = finite(relief?.referenceHeight ?? 1, 'referenceHeight');
  const maxDepth = amplitude * reference;
  const offsetZero = { uvOffset: [0, 0], depthMetres: 0, steps: 0, hit: false, fade: 0 };
  if (!(amplitude > 0) || !(maxDepth > 0) || !(vn > 1e-4)) return offsetZero;
  const grazing = relief?.grazingFade ?? 0;
  const grazingWeight = grazing > 0 ? Math.min(1, vn / Math.max(grazing, 1e-6)) : 1;
  if (!(grazingWeight > 0)) return offsetZero;
  const fadeStart = relief?.fadeStartMetres ?? 0, fadeEnd = relief?.fadeEndMetres ?? 0;
  const distanceFade = fadeEnd > fadeStart ? Math.max(0, Math.min(1, (fadeEnd - distanceMetres) / (fadeEnd - fadeStart))) : 1;
  const fade = Math.min(grazingWeight, distanceFade);
  if (!(fade > 0)) return offsetZero;

  const slopeU = dot(V, basis.tangent) / (vn * basis.metresPerU);
  const slopeV = dot(V, basis.bitangent) / (vn * basis.metresPerV);
  const minSteps = Math.max(1, Math.round(relief?.minSteps ?? 8));
  const maxSteps = Math.max(minSteps, Math.round(relief?.maxSteps ?? 24));
  const steps = Math.max(1, Math.round(maxSteps + (minSteps - maxSteps) * Math.min(1, Math.max(0, vn))));
  const refine = Math.max(0, Math.round(relief?.refineSteps ?? 2));
  const invert = relief?.invert ?? false;
  const depthAt = (t) => {
    const u = u0 + t * slopeU, v = v0 + t * slopeV;
    const h = sampleHeight(u, v);
    const d = -surfaceHeightMetres(h, { amplitudeMetres: amplitude, referenceHeight: reference, invert });
    return Math.min(maxDepth, Math.max(0, d));
  };
  // The ray and the top surface start at the same point; begin at the first
  // interior sample so an ordinary slope does not report a zero-depth hit.
  let previousT = 0, hitT = maxDepth, hit = false;
  for (let step = 1; step <= steps; step += 1) {
    const t = (maxDepth * step) / steps;
    if (t >= depthAt(t)) {
      let low = previousT, high = t;
      for (let i = 0; i < refine; i += 1) {
        const mid = (low + high) * 0.5;
        if (mid >= depthAt(mid)) high = mid; else low = mid;
      }
      hitT = high; hit = true;
      break;
    }
    previousT = t;
  }
  return { uvOffset: [hitT * slopeU * fade, hitT * slopeV * fade], depthMetres: hitT * fade, steps: hit ? steps : 0, hit, fade };
}

/** Analytic normalized height fields for the P0 reference suite. */
export function analyticHeightField(kind, options = {}) {
  const reference = { ref: 1, ...options };
  if (kind === 'plane') return () => reference.ref;
  if (kind === 'slope') {
    const { slopeU = 0, slopeV = 0, ref = 1 } = reference;
    return (u, v) => Math.min(1, Math.max(0, ref + slopeU * u + slopeV * v));
  }
  if (kind === 'groove') {
    const { center = 0.5, sigma = 0.15, depth = 0.5, ref = 1 } = reference;
    return (u) => Math.min(1, Math.max(0, ref - depth * Math.exp(-((u - center) ** 2) / (2 * sigma * sigma))));
  }
  throw new Error(`unknown analytic height field ${kind}`);
}

/** Dense root reference used to validate the bounded march. */
export function sampleParallaxReference({ relief, uv, view, basis, sampleHeight, resolution = 4096 }) {
  const V = normalize(vec3(view, 'view'), 'view'), n = basis.normal, vn = dot(V, n);
  const amplitude = relief.amplitudeMetres, reference = relief.referenceHeight ?? 1;
  const maxDepth = amplitude * reference;
  if (!(vn > 1e-4) || !(maxDepth > 0)) return { uvOffset: [0, 0], depthMetres: 0, hit: false };
  const slopeU = dot(V, basis.tangent) / (vn * basis.metresPerU);
  const slopeV = dot(V, basis.bitangent) / (vn * basis.metresPerV);
  const depthAt = (t) => {
    const h = sampleHeight(uv[0] + t * slopeU, uv[1] + t * slopeV);
    return Math.min(maxDepth, Math.max(0, -surfaceHeightMetres(h, { amplitudeMetres: amplitude, referenceHeight: reference, invert: relief.invert })));
  };
  const step = maxDepth / resolution;
  for (let i = 1; i <= resolution; i += 1) {
    const t = step * i;
    if (t >= depthAt(t)) {
      let low = t - step, high = t;
      for (let k = 0; k < 40; k += 1) { const mid = (low + high) * 0.5; if (mid >= depthAt(mid)) high = mid; else low = mid; }
      return { uvOffset: [high * slopeU, high * slopeV], depthMetres: high, hit: true };
    }
  }
  return { uvOffset: [maxDepth * slopeU, maxDepth * slopeV], depthMetres: maxDepth, hit: false };
}

export { add as addVec3, scale as scaleVec3 };

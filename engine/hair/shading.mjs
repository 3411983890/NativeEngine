import { HairError } from './spec.mjs';

// Hair appearance as executable math.
//
// A fibre shader that is only a .hlsl file cannot be reviewed, calibrated or tested,
// and "the hair looks flat" then becomes an opinion instead of a number. This module
// is the reference implementation the shader follows: the same coverage law, the same
// dual-lobe fibre response, the same transmission term, the same transparency budget.
// The shader is checked against these functions; these functions are checked by tests.

const fail = (ok, code, message) => { if (!ok) throw new HairError(code, message); };

const clamp01 = (value) => Math.max(0, Math.min(1, value));
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const normalize = (a) => {
  const length = Math.hypot(a[0], a[1], a[2]) || 1;
  return [a[0] / length, a[1] / length, a[2] / length];
};

// ------------------------------------------------------------- visibility
// A strand thinner than a pixel must contribute *partial* coverage, not be widened to
// a full pixel. Without this a 60 µm strand renders as a 1-pixel black line and the
// hairline becomes a hard band, which then gets "fixed" by making the strands wider.
export function strandCoveragePixels(widthMetres, depthMetres, viewportHeight, tanHalfFovY) {
  fail(widthMetres > 0 && widthMetres < 0.05, 'NG-E-HAIR-SHADING', 'strand width must be in (0, 0.05) m');
  fail(depthMetres > 0, 'NG-E-HAIR-SHADING', 'depth must be positive');
  fail(viewportHeight >= 16, 'NG-E-HAIR-SHADING', 'viewport height must be at least 16 pixels');
  fail(tanHalfFovY > 0 && tanHalfFovY < 4, 'NG-E-HAIR-SHADING', 'tanHalfFovY must be in (0, 4)');
  const metresPerPixel = (2 * depthMetres * tanHalfFovY) / viewportHeight;
  return widthMetres / metresPerPixel;
}

export function coverageAlpha(pixels, { minAlpha = 0.02 } = {}) {
  // Coverage below the alpha threshold is dropped: keeping 0.005 alpha everywhere
  // forces the transparent pass to shade every strand in the frame.
  const alpha = clamp01(pixels);
  return alpha < minAlpha ? 0 : alpha;
}

// Analytic blended alpha for K strands sharing a pixel, with per-strand alpha. Sorting
// is not required for the coverage estimate (it is), and the estimate is what decides
// how many transparency layers the tile needs.
export function accumulatedCoverage(alphas) {
  let remaining = 1;
  for (const alpha of alphas) remaining *= 1 - clamp01(alpha);
  return 1 - remaining;
}

// Bounded transparency cache. The plan asks for a chosen algorithm *and* a report of
// overflow; a cache that silently drops layers produces hair that thins out at grazing
// angles, which reads as a bug in the hairstyle rather than in the renderer.
export function transparencyBudget({
  strandsOnScreen, averageSegmentsPerStrand, averageCoverage = 0.25, layersPerTile = 8, tileSize = 8,
  resolution = { width: 1920, height: 1080 },
}) {
  fail(strandsOnScreen > 0, 'NG-E-HAIR-SHADING', 'strandsOnScreen must be positive');
  fail(averageSegmentsPerStrand >= 1, 'NG-E-HAIR-SHADING', 'averageSegmentsPerStrand must be at least 1');
  fail(layersPerTile >= 1 && layersPerTile <= 64, 'NG-E-HAIR-SHADING', 'layersPerTile must be 1–64');
  fail(tileSize >= 2 && tileSize <= 64, 'NG-E-HAIR-SHADING', 'tileSize must be 2–64');
  const tilesX = Math.ceil(resolution.width / tileSize);
  const tilesY = Math.ceil(resolution.height / tileSize);
  const tiles = tilesX * tilesY;
  const fragments = strandsOnScreen * averageSegmentsPerStrand;
  const averagePerTile = fragments / tiles;
  // Layer demand grows with how many fragments a tile must composite, weighted by how
  // much each one actually covers: thin coverage needs fewer layers for the same look.
  const demandPerTile = averagePerTile * clamp01(averageCoverage);
  const layersNeeded = Math.max(1, Math.ceil(demandPerTile));
  const overflow = Math.max(0, layersNeeded - layersPerTile);
  return {
    algorithm: 'bounded-tile-layers',
    tiles, tilesX, tilesY, tileSize,
    fragments,
    fragmentsPerTile: averagePerTile,
    layersNeeded,
    layersAvailable: layersPerTile,
    overflowLayers: overflow,
    overflowFraction: layersNeeded > 0 ? overflow / layersNeeded : 0,
    memoryBytes: tiles * layersPerTile * 8,
    fallback: overflow > 0 ? 'opaque-approximation' : null,
    fallbackReason: overflow > 0
      ? `${layersNeeded} layers demanded, ${layersPerTile} available; overflowing tiles fall back to an opaque `
        + 'approximation and the capture must report it rather than showing thinner hair'
      : null,
  };
}

// ------------------------------------------------------------------ colour
// Melanin → albedo. Two pigments: eumelanin (brown-black) and pheomelanin (red).
// Deriving colour from pigment instead of storing an RGB triple is what makes a
// "dark blonde" and a "black" hairstyle behave differently under the same light —
// their absorption spectra differ, not just their brightness.
export function albedoFromPigment(melanin, redness = 0.3) {
  fail(melanin >= 0 && melanin <= 1, 'NG-E-HAIR-SHADING', 'melanin must be in [0,1]');
  fail(redness >= 0 && redness <= 1, 'NG-E-HAIR-SHADING', 'redness must be in [0,1]');
  const eumelanin = [
    Math.exp(-2.4 * melanin), Math.exp(-3.9 * melanin), Math.exp(-5.2 * melanin),
  ];
  const pheomelanin = [
    Math.exp(-0.6 * melanin) * 0.92, Math.exp(-1.7 * melanin) * 0.52, Math.exp(-3.4 * melanin) * 0.24,
  ];
  return [0, 1, 2].map((channel) => clamp01(
    eumelanin[channel] * (1 - redness) + pheomelanin[channel] * redness));
}

// Per-hair-colour calibration. Light hair scatters more internally and shows a broad,
// low-contrast highlight; dark hair shows a narrow, relatively stronger highlight and
// almost no transmission. Using one set of constants for both is why dark hair
// usually renders as grey plastic.
export function calibrateForAlbedo(albedo) {
  const luma = 0.2126 * albedo[0] + 0.7152 * albedo[1] + 0.0722 * albedo[2];
  const lightness = clamp01(luma);
  return {
    lightness,
    category: lightness < 0.16 ? 'dark' : lightness < 0.42 ? 'medium' : 'light',
    diffuseScale: 0.55 + 0.45 * lightness,
    specularScale: 1.45 - 0.85 * lightness,
    transmissionScale: 0.12 + 0.88 * lightness * lightness,
    longitudinalRoughnessScale: 1.25 - 0.5 * lightness,
    radialRoughnessScale: 1.35 - 0.45 * lightness,
    // Root-to-tip darkening is stronger on light hair because the tip has lost more
    // cuticle; on black hair the same tint is invisible.
    rootToTipScale: 0.55 + 0.45 * lightness,
  };
}

// ----------------------------------------------------------------- fibre BSDF
// Dual-lobe Kajiya–Kay with a Marschner-style azimuthal shift, plus a forward
// transmission lobe. Terms are returned separately so a debug channel can show them
// one at a time — "the highlight is in the wrong place" needs to know which lobe.
export function fibreResponse({
  tangent, normal, viewDirection, lightDirection,
  longitudinalRoughness = 0.3, radialRoughness = 0.25, shiftRadians = 0.12,
}) {
  const T = normalize(tangent), N = normalize(normal);
  const V = normalize(viewDirection), L = normalize(lightDirection);
  const H = normalize([V[0] + L[0], V[1] + L[1], V[2] + L[2]]);
  const exponent = Math.max(1, 2 / Math.max(1e-3, longitudinalRoughness * longitudinalRoughness) - 2);
  const radialExponent = Math.max(1, 2 / Math.max(1e-3, radialRoughness * radialRoughness) - 2);

  const shiftAlongNormal = (sign) => normalize([
    T[0] + N[0] * shiftRadians * sign,
    T[1] + N[1] * shiftRadians * sign,
    T[2] + N[2] * shiftRadians * sign,
  ]);
  const lobe = (shiftedTangent) => {
    const sine = Math.sqrt(Math.max(0, 1 - Math.pow(dot(shiftedTangent, H), 2)));
    return Math.pow(sine, exponent) * Math.pow(Math.max(0, dot(shiftedTangent, H)), radialExponent * 0.15);
  };
  // R lobe (shifted one way) and TRT lobe (shifted the other, tinted by the fibre).
  const primary = lobe(shiftAlongNormal(1));
  const secondary = lobe(shiftAlongNormal(-1));

  // Kajiya–Kay diffuse: a fibre is a cylinder, so the diffuse term follows sin(T,L).
  const diffuse = Math.sqrt(Math.max(0, 1 - Math.pow(dot(T, L), 2)));

  // Forward transmission: strongest when the light is behind the fibre relative to the
  // viewer. A single-sided lamp in front must produce almost none — that asymmetry is
  // what makes backlit hair read as backlit.
  const backFacing = clamp01(-dot(V, L) * 0.5 + 0.5);
  const transmission = Math.pow(backFacing, 2.4) * (1 - Math.pow(Math.abs(dot(T, V)), 2) * 0.6);

  return { primary, secondary, diffuse, transmission, halfVector: H };
}

export function shadeSample({
  tangent, normal, viewDirection, lightDirection, lightColour = [1, 1, 1],
  material, rootToTip = 0.5,
}) {
  fail(material && typeof material === 'object', 'NG-E-HAIR-SHADING', 'material is required');
  const albedo = material.albedo ?? albedoFromPigment(material.melanin ?? 0.8, material.redness ?? 0.3);
  const calibration = calibrateForAlbedo(albedo);
  const response = fibreResponse({
    tangent, normal, viewDirection, lightDirection,
    longitudinalRoughness: (material.longitudinalRoughness ?? material.roughness ?? 0.3)
      * calibration.longitudinalRoughnessScale,
    radialRoughness: (material.radialRoughness ?? material.roughness ?? 0.25) * calibration.radialRoughnessScale,
    shiftRadians: material.shiftRadians ?? 0.12,
  });
  const tint = material.rootToTipTint ?? [1, 1, 1];
  const tipAmount = clamp01(rootToTip);
  const tintFactor = tint.map((value) => 1 - (1 - value) * tipAmount * calibration.rootToTipScale);
  const diffuse = [0, 1, 2].map((channel) => albedo[channel] * response.diffuse * calibration.diffuseScale * lightColour[channel]);
  const specular = [0, 1, 2].map((channel) => (response.primary + response.secondary * 0.35)
    * calibration.specularScale * lightColour[channel] * (0.55 + 0.45 * albedo[channel]));
  const transmission = [0, 1, 2].map((channel) => albedo[channel] * response.transmission
    * calibration.transmissionScale * lightColour[channel]);
  const radiance = [0, 1, 2].map((channel) => (diffuse[channel] + specular[channel] + transmission[channel]) * tintFactor[channel]);
  return { radiance, diffuse, specular, transmission, calibration, response };
}

// ---------------------------------------------------------------- shadows
// Deep-opacity shadow: alpha accumulates along the fibre so the shadow of a volume of
// hair is dark near the scalp and fades at the tips, instead of being one solid card.
export function deepOpacityAlpha(depthSamples, { opacityPerMetre = 900, maxAlpha = 0.94 } = {}) {
  fail(Array.isArray(depthSamples) && depthSamples.length > 0, 'NG-E-HAIR-SHADING',
    'depthSamples must be a non-empty array of sample counts per layer');
  let transmitted = 1;
  for (const count of depthSamples) {
    if (!(count > 0)) continue;
    // Exponential extinction: each fibre layer removes a fraction of what remains.
    transmitted *= Math.exp(-opacityPerMetre * 1e-4 * count);
  }
  return Math.min(maxAlpha, 1 - transmitted);
}

// Transmittance for a shadow ray through the hair volume, used by the shadow map pass.
export function shadowTransmittance(thicknessMetres, { sigmaPerMetre = 2400, wavelengthScale = [1, 1, 1] } = {}) {
  fail(thicknessMetres >= 0, 'NG-E-HAIR-SHADING', 'thickness must be non-negative');
  return wavelengthScale.map((scale) => clamp01(Math.exp(-sigmaPerMetre * thicknessMetres * scale)));
}

// ------------------------------------------------------------- stability
// Flicker is measured, not eyeballed. For a sequence of frames (camera move, head turn
// or LOD switch) the metric is the coverage-weighted mean change between consecutive
// frames, plus the largest single-frame jump. A jitter threshold is applied in screen
// coverage units, which is what a viewer perceives as shimmer.
export function stabilityMetric(frames, { threshold = 0.06 } = {}) {
  fail(Array.isArray(frames) && frames.length >= 2, 'NG-E-HAIR-SHADING', 'at least two frames are required');
  const jumps = [];
  let worst = { index: 0, delta: 0 };
  for (let index = 1; index < frames.length; ++index) {
    const previous = frames[index - 1], current = frames[index];
    fail(previous.coverage.length === current.coverage.length, 'NG-E-HAIR-SHADING',
      'frames must share the same coverage sample layout');
    let total = 0;
    for (let sample = 0; sample < current.coverage.length; ++sample) {
      total += Math.abs(current.coverage[sample] - previous.coverage[sample]);
    }
    const delta = total / current.coverage.length;
    jumps.push(delta);
    if (delta > worst.delta) worst = { index, delta };
  }
  const mean = jumps.reduce((sum, value) => sum + value, 0) / jumps.length;
  const sorted = [...jumps].sort((a, b) => a - b);
  return {
    frames: frames.length,
    meanDelta: mean,
    p95Delta: sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))],
    maxDelta: worst.delta,
    maxAtFrame: worst.index,
    threshold,
    flickering: worst.delta > threshold,
    note: 'Delta is mean absolute change in screen coverage between consecutive frames.',
  };
}

// LOD transition check: the same strands must occupy the same pixels after a level
// change. A level that moves roots shows up here as a large coverage delta at the
// switch, which is exactly the "发束体积变化" the plan asks to verify.
export function lodTransitionDelta(levelA, levelB) {
  fail(Array.isArray(levelA.roots) && Array.isArray(levelB.roots), 'NG-E-HAIR-SHADING',
    'both levels must expose root positions');
  const minimum = Math.min(levelA.roots.length, levelB.roots.length);
  let maximum = 0;
  for (let index = 0; index < minimum; ++index) {
    const a = levelA.roots[index], b = levelB.roots[index];
    maximum = Math.max(maximum, Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
  }
  return {
    compared: minimum,
    maxRootDriftMetres: maximum,
    strandCountA: levelA.roots.length,
    strandCountB: levelB.roots.length,
    acceptable: maximum < 1e-6,
    note: 'A LOD switch must keep the same roots in the same places; only the strand count and segment count change.',
  };
}

import { HairError } from './spec.mjs';
import {
  strandCoveragePixels, coverageAlpha, shadeSample, shadowTransmittance,
} from './shading.mjs';
import { viewMatrix } from './capture.mjs';

// CPU reference renderer for strands.
//
// Why this exists at all, given that hair is supposed to be drawn on the GPU: the plan
// requires a CPU authority for every GPU path (the cloth solver already follows that rule),
// and the image-level claims it makes — sub-pixel coverage, bounded transparency layers with
// a reported overflow, deep-opacity shadowing, backlight transmission — are only meaningful
// if something actually composites strands into pixels. This renderer is that authority: the
// GPU pass has to match it, and the numbers below are what "match" means.
//
// What it is not: an engine frame. It does not exercise the D3D12 pass, GPU timings, video
// memory or hardware resolve behaviour, and no report produced from it may be described as
// evidence for those. `stats.backend` says `cpu-reference` for exactly that reason.
//
// Deliberate simplifications, all of which the GPU pass must decide explicitly rather than
// inherit silently:
//   * screen-space linear interpolation instead of perspective-correct (strand quads are
//     a few pixels wide, so the error is below the coverage quantisation),
//   * a single directional light, no occlusion of the shadow grid by the body,
//   * one orthographic light-space optical-depth grid instead of deep-opacity layers.

export const REFERENCE_RENDER_SCHEMA = 'nativegpu.hair-reference-render/1';

const fail = (ok, message, code = 'NG-E-HAIR-RENDER') => {
  if (!ok) throw new HairError(code, message);
};
const clamp01 = (value) => (value < 0 ? 0 : value > 1 ? 1 : value);
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const normalize = (v) => {
  const length = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / length, v[1] / length, v[2] / length];
};

export const DEFAULT_RENDER_OPTIONS = Object.freeze({
  level: 0,
  maxLayersPerPixel: 8,
  // A strand thinner than this is drawn at this width and its alpha is scaled by the true
  // coverage, so a 60 µm strand contributes 30% of a pixel instead of a hard black line.
  minWidthPixels: 1,
  // Upper bound on the geometry a single strand may cover, so one strand crossing the
  // near field cannot fill a tile and starve the layers of everything behind it.
  maxWidthPixels: 6,
  background: [0.02, 0.021, 0.024],
  exposure: 1.7,
  shadow: true,
  shadowGridSize: 192,
  shadowSigmaPerMetre: 2400,
  shadowLayers: 4,
  // The grid accumulates one fibre diameter per texel a strand crosses, which is the
  // optical path of the strands this renderer actually draws. Scaling it to a "real" head of
  // hair would need the authored density, and pretending to know it here is how a shadow
  // ends up black rather than dense.
  shadowDensityScale: 1,
  ambient: [0.05, 0.052, 0.06],
  coverageEarlyOut: 0.995,
});

function orthonormalBasis(axis) {
  const helper = Math.abs(axis[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  const u = normalize(cross(helper, axis));
  return { u, v: normalize(cross(axis, u)) };
}

// Deep-opacity shadow along the light direction.
//
// A single depth map is not enough for hair: the shadow of a fringe is dark because a ray
// crosses many fibres, and it fades at the tips because it crosses fewer. This grid keeps a
// few depth-sorted fibre layers per light-space texel (the "deep opacity map" idea) so the
// lookup can ask "how much fibre lies between the light and this point", not just "is this
// texel occupied".
//
// It is not a full deep-opacity map: layers are capped, the body is not an occluder, and the
// thickness is that of the strands this renderer draws rather than of the authored density.
function buildShadowGrid({ curves, lightDirection, size, layers, densityScale }) {
  const basis = orthonormalBasis(lightDirection);
  let uMin = Infinity, uMax = -Infinity, vMin = Infinity, vMax = -Infinity;
  for (const curve of curves) {
    for (const point of curve.points) {
      const u = dot(point, basis.u), v = dot(point, basis.v);
      if (u < uMin) uMin = u;
      if (u > uMax) uMax = u;
      if (v < vMin) vMin = v;
      if (v > vMax) vMax = v;
    }
  }
  const margin = 0.005;
  uMin -= margin; uMax += margin; vMin -= margin; vMax += margin;
  const extent = Math.max(uMax - uMin, vMax - vMin);
  fail(extent > 1e-4, 'the strand volume has no extent along the light', 'NG-E-HAIR-RENDER-LIGHT');
  const texel = extent / size;
  const texels = size * size;
  const layerDepth = new Float32Array(texels * layers).fill(Infinity);
  const layerThickness = new Float32Array(texels * layers);
  const layerCount = new Uint8Array(texels);
  const totalThickness = new Float32Array(texels);
  const index = (u, v) => {
    const x = Math.floor((u - uMin) / texel), y = Math.floor((v - vMin) / texel);
    return x >= 0 && x < size && y >= 0 && y < size ? y * size + x : -1;
  };
  const insert = (cell, depth, thickness) => {
    const base = cell * layers;
    const count = layerCount[cell];
    let slot = count;
    while (slot > 0 && layerDepth[base + slot - 1] > depth) {
      if (slot < layers) {
        layerDepth[base + slot] = layerDepth[base + slot - 1];
        layerThickness[base + slot] = layerThickness[base + slot - 1];
      }
      --slot;
    }
    // A fragment further than the last stored layer cannot be seen along this ray, which is
    // exactly the overflow the report has to name.
    if (slot >= layers) return false;
    layerDepth[base + slot] = depth;
    layerThickness[base + slot] = thickness;
    if (count < layers) layerCount[cell] = count + 1;
    return true;
  };
  let crossings = 0, dropped = 0;
  for (const curve of curves) {
    const points = curve.points, radii = curve.radii;
    for (let i = 0; i + 1 < points.length; ++i) {
      const a = points[i], b = points[i + 1];
      const ua = dot(a, basis.u), va = dot(a, basis.v), da = dot(a, lightDirection);
      const ub = dot(b, basis.u), vb = dot(b, basis.v), db = dot(b, lightDirection);
      const steps = Math.max(1, Math.ceil(Math.hypot(ub - ua, vb - va) / (texel * 0.5)));
      const optical = 2 * ((radii[i] + radii[i + 1]) * 0.5) * densityScale;
      let previous = -2;
      for (let step = 0; step <= steps; ++step) {
        const t = step / steps;
        const cell = index(ua + (ub - ua) * t, va + (vb - va) * t);
        if (cell < 0 || cell === previous) continue;
        previous = cell;
        totalThickness[cell] += optical;
        ++crossings;
        if (!insert(cell, da + (db - da) * t, optical)) ++dropped;
      }
    }
  }
  let coveredTexels = 0, maximum = 0;
  for (let cell = 0; cell < texels; ++cell) {
    if (totalThickness[cell] > 0) ++coveredTexels;
    if (totalThickness[cell] > maximum) maximum = totalThickness[cell];
  }
  // Fibre between the light and this point. The point's own layer is excluded: a strand is
  // not shadowed by itself.
  const sample = (point) => {
    const cell = index(dot(point, basis.u), dot(point, basis.v));
    if (cell < 0) return 0;
    const depth = dot(point, lightDirection);
    const base = cell * layers;
    let thickness = 0;
    for (let slot = 0; slot < layerCount[cell]; ++slot) {
      const value = layerDepth[base + slot];
      // A layer closer to the camera along the light is *behind* the light direction sign:
      // larger depth means further along the light vector, i.e. deeper in the volume.
      if (value < depth - (texel * 0.75)) thickness += layerThickness[base + slot];
    }
    return thickness;
  };
  return {
    size, uMin, vMin, texel, layers, sample,
    coveredTexels, maximum, crossings, droppedLayers: dropped, extent,
  };
}

// A bounded per-pixel transparency cache. Insertion order is not depth order, so the
// composite sorts; when the cache is full the *farthest* fragment is dropped and counted,
// because dropping the nearest one would hollow out the silhouette.
class LayerCache {
  constructor(width, height, layers) {
    this.width = width; this.height = height; this.layers = layers;
    this.depth = new Float32Array(width * height * layers).fill(Infinity);
    this.colour = new Float32Array(width * height * layers * 3);
    this.alpha = new Float32Array(width * height * layers);
    this.count = new Uint8Array(width * height);
    this.dropped = new Uint32Array(width * height);
    // Overflow is not silent: a dropped fragment is accumulated into one premultiplied
    // "spill" layer per pixel and composited behind everything kept. Dropping it instead
    // would thin the hair at grazing angles, which reads as a bug in the hairstyle.
    this.spillAlpha = new Float32Array(width * height);
    this.spillColour = new Float32Array(width * height * 3);
    this.fragments = 0;
    this.droppedFragments = 0;
  }

  push(pixel, depth, r, g, b, a) {
    ++this.fragments;
    const base = pixel * this.layers;
    const count = this.count[pixel];
    if (count < this.layers) {
      const slot = base + count;
      this.depth[slot] = depth;
      this.colour[slot * 3] = r; this.colour[slot * 3 + 1] = g; this.colour[slot * 3 + 2] = b;
      this.alpha[slot] = a;
      this.count[pixel] = count + 1;
      return true;
    }
    let farthest = 0, farthestDepth = this.depth[base];
    for (let slot = 1; slot < count; ++slot) {
      if (this.depth[base + slot] > farthestDepth) { farthestDepth = this.depth[base + slot]; farthest = slot; }
    }
    if (depth < farthestDepth) {
      const slot = base + farthest;
      this.depth[slot] = depth;
      this.colour[slot * 3] = r; this.colour[slot * 3 + 1] = g; this.colour[slot * 3 + 2] = b;
      this.alpha[slot] = a;
      ++this.dropped[pixel];
      ++this.droppedFragments;
      return true;
    }
    this.spillColour[pixel * 3] += r * a;
    this.spillColour[pixel * 3 + 1] += g * a;
    this.spillColour[pixel * 3 + 2] += b * a;
    this.spillAlpha[pixel] = 1 - (1 - this.spillAlpha[pixel]) * (1 - a);
    ++this.dropped[pixel];
    ++this.droppedFragments;
    return false;
  }

  composite(background, earlyOut) {
    const rgb = new Float32Array(this.width * this.height * 3);
    const coverage = new Float32Array(this.width * this.height);
    const order = [];
    let covered = 0, layersUsed = 0, overflowPixels = 0, earlyOutPixels = 0, maximumLayers = 0, maximumDropped = 0;
    let spillPixels = 0;
    for (let pixel = 0; pixel < this.width * this.height; ++pixel) {
      const count = this.count[pixel];
      if (count === 0) {
        rgb[pixel * 3] = background[0]; rgb[pixel * 3 + 1] = background[1]; rgb[pixel * 3 + 2] = background[2];
        continue;
      }
      ++covered;
      layersUsed += count;
      if (count > maximumLayers) maximumLayers = count;
      if (this.dropped[pixel] > 0) ++overflowPixels;
      if (this.dropped[pixel] > maximumDropped) maximumDropped = this.dropped[pixel];
      order.length = 0;
      for (let slot = 0; slot < count; ++slot) order.push(pixel * this.layers + slot);
      order.sort((left, right) => this.depth[left] - this.depth[right]);
      let r = background[0], g = background[1], b = background[2];
      let alpha = 0;
      for (const slot of order) {
        const a = this.alpha[slot] * (1 - alpha);
        if (a <= 0) continue;
        r += this.colour[slot * 3] * a;
        g += this.colour[slot * 3 + 1] * a;
        b += this.colour[slot * 3 + 2] * a;
        alpha += a;
        if (alpha >= earlyOut) { ++earlyOutPixels; break; }
      }
      const spill = this.spillAlpha[pixel] * (1 - alpha);
      if (spill > 0) {
        // Normalise by the accumulated coverage: spillColour holds premultiplied values.
        const divisor = Math.max(this.spillAlpha[pixel], 1e-6);
        r += (this.spillColour[pixel * 3] / divisor) * spill;
        g += (this.spillColour[pixel * 3 + 1] / divisor) * spill;
        b += (this.spillColour[pixel * 3 + 2] / divisor) * spill;
        alpha += spill;
        ++spillPixels;
      }
      rgb[pixel * 3] = r; rgb[pixel * 3 + 1] = g; rgb[pixel * 3 + 2] = b;
      coverage[pixel] = alpha;
    }
    return {
      rgb, coverage,
      stats: {
        pixelsCovered: covered,
        coverageFraction: covered / (this.width * this.height),
        meanLayers: covered ? layersUsed / covered : 0,
        maxLayers: maximumLayers,
        overflowPixels, overflowFraction: covered ? overflowPixels / covered : 0,
        maxDroppedPerPixel: maximumDropped,
        spillPixels,
        fallback: overflowPixels > 0 ? 'opaque-spill-layer' : null,
        earlyOutPixels,
        fragments: this.fragments,
        droppedFragments: this.droppedFragments,
      },
    };
  }
}

function rasterizeTriangle(cache, v0, v1, v2, width, height) {
  const minX = Math.max(0, Math.floor(Math.min(v0.x, v1.x, v2.x)));
  const maxX = Math.min(width - 1, Math.ceil(Math.max(v0.x, v1.x, v2.x)));
  const minY = Math.max(0, Math.floor(Math.min(v0.y, v1.y, v2.y)));
  const maxY = Math.min(height - 1, Math.ceil(Math.max(v0.y, v1.y, v2.y)));
  if (minX > maxX || minY > maxY) return;
  const area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
  if (Math.abs(area) < 1e-10) return;
  const inverse = 1 / area;
  for (let y = minY; y <= maxY; ++y) {
    const py = y + 0.5;
    for (let x = minX; x <= maxX; ++x) {
      const px = x + 0.5;
      let w0 = ((v1.x - px) * (v2.y - py) - (v2.x - px) * (v1.y - py)) * inverse;
      let w1 = ((v2.x - px) * (v0.y - py) - (v0.x - px) * (v2.y - py)) * inverse;
      let w2 = 1 - w0 - w1;
      // Top-left style tolerance: without it a strand tiling exactly on texel centres
      // disappears instead of drawing once.
      if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) continue;
      w0 = Math.max(0, w0); w1 = Math.max(0, w1); w2 = Math.max(0, w2);
      const sum = w0 + w1 + w2 || 1;
      w0 /= sum; w1 /= sum; w2 /= sum;
      const depth = v0.depth * w0 + v1.depth * w1 + v2.depth * w2;
      const alpha = v0.alpha * w0 + v1.alpha * w1 + v2.alpha * w2;
      if (!(alpha > 0)) continue;
      cache.push(y * width + x, depth,
        v0.r * w0 + v1.r * w1 + v2.r * w2,
        v0.g * w0 + v1.g * w1 + v2.g * w2,
        v0.b * w0 + v1.b * w1 + v2.b * w2,
        alpha);
    }
  }
}

export function renderHairReference({
  generated, material, camera, width = 256, height = 256, light, options = {},
}) {
  fail(generated?.levels?.[0]?.curves?.length > 0, 'renderHairReference needs a generated hairstyle');
  fail(material && typeof material === 'object', 'a material group is required');
  fail(camera && Array.isArray(camera.position) && Array.isArray(camera.target), 'a capture camera is required');
  fail(light && Array.isArray(light.direction) && Array.isArray(light.colour),
    'a directional light with a direction and colour is required');
  // A zero direction normalises to NaN and shades every strand black without an error, which
  // is exactly the class of silent failure the plan asks reports to name.
  fail(Math.hypot(...light.direction) > 1e-6, 'the light direction must be non-zero', 'NG-E-HAIR-RENDER-LIGHT');
  const settings = { ...DEFAULT_RENDER_OPTIONS, ...options };
  const level = generated.levels[settings.level ?? 0];
  fail(level, `hairstyle has no LOD level ${settings.level}`);
  const intensity = light.intensity ?? 1;

  const started = Date.now();
  const tanHalfFovY = Math.tan((camera.fovYDegrees * Math.PI) / 360);
  const focal = (height * 0.5) / tanHalfFovY;
  const view = viewMatrix(camera);
  const toScreen = (point) => {
    const x = view[0] * point[0] + view[4] * point[1] + view[8] * point[2] + view[12];
    const y = view[1] * point[0] + view[5] * point[1] + view[9] * point[2] + view[13];
    const z = view[2] * point[0] + view[6] * point[1] + view[10] * point[2] + view[14];
    return { x: width * 0.5 + (x * focal) / z, y: height * 0.5 - (y * focal) / z, depth: z };
  };

  const lightDirection = normalize(light.direction);
  const shadowGrid = settings.shadow
    ? buildShadowGrid({
      curves: level.curves, lightDirection, size: settings.shadowGridSize,
      layers: settings.shadowLayers, densityScale: settings.shadowDensityScale,
    })
    : null;

  const cache = new LayerCache(width, height, settings.maxLayersPerPixel);
  let clampedWidths = 0, subPixelStrands = 0, droppedSegments = 0;
  const eye = camera.position;
  const background = settings.background;

  for (const curve of level.curves) {
    const points = curve.points, radii = curve.radii;
    const count = points.length;
    // Per-point shading: tangent from neighbours, camera-facing ribbon normal, root-to-tip
    // parameter. Shading per point rather than per fragment is what keeps this reference
    // fast enough to run over every view in a test.
    const shaded = [];
    for (let index = 0; index < count; ++index) {
      const previous = points[Math.max(0, index - 1)];
      const next = points[Math.min(count - 1, index + 1)];
      const tangent = normalize(sub(next, previous));
      const toEye = normalize(sub(eye, points[index]));
      const normal = normalize(cross(tangent, cross(toEye, tangent)));
      const rootToTip = count > 1 ? index / (count - 1) : 0;
      const thickness = shadowGrid ? shadowGrid.sample(points[index]) : 0;
      const transmittance = thickness > 0
        ? shadowTransmittance(thickness, { sigmaPerMetre: settings.shadowSigmaPerMetre })
        : [1, 1, 1];
      // Direct light is attenuated by the fibre the ray crossed; ambient is not, because
      // the ambient term stands in for multiple scattering inside the volume.
      const lightColour = [0, 1, 2].map((channel) =>
        light.colour[channel] * intensity * transmittance[channel]);
      const sample = shadeSample({
        tangent, normal, viewDirection: toEye, lightDirection, lightColour, material, rootToTip,
      });
      const albedo = material.albedo ?? [0.09, 0.06, 0.04];
      const radiance = [0, 1, 2].map((channel) =>
        sample.radiance[channel] + albedo[channel] * settings.ambient[channel]);
      const screen = toScreen(points[index]);
      shaded.push({ screen, radiance, rootToTip, radius: radii[index] });
    }

    for (let index = 0; index + 1 < count; ++index) {
      const a = shaded[index], b = shaded[index + 1];
      if (a.screen.depth <= camera.nearMetres || b.screen.depth <= camera.nearMetres) { ++droppedSegments; continue; }
      const widthA = strandCoveragePixels(2 * a.radius, a.screen.depth, height, tanHalfFovY);
      const widthB = strandCoveragePixels(2 * b.radius, b.screen.depth, height, tanHalfFovY);
      if (widthA < settings.minWidthPixels) ++subPixelStrands;
      const drawA = Math.min(settings.maxWidthPixels, Math.max(settings.minWidthPixels, widthA));
      const drawB = Math.min(settings.maxWidthPixels, Math.max(settings.minWidthPixels, widthB));
      if (widthA > settings.maxWidthPixels || widthB > settings.maxWidthPixels) ++clampedWidths;
      // Coverage-corrected alpha: widening a sub-pixel strand to a pixel must divide its
      // opacity by the same factor, otherwise the hairline renders as a solid band.
      const alphaA = coverageAlpha(Math.min(1, widthA / drawA));
      const alphaB = coverageAlpha(Math.min(1, widthB / drawB));
      if (alphaA <= 0 && alphaB <= 0) continue;
      const dx = b.screen.x - a.screen.x, dy = b.screen.y - a.screen.y;
      const length = Math.hypot(dx, dy) || 1e-6;
      const px = -dy / length, py = dx / length;
      const vertex = (point, offset, alpha) => ({
        x: point.screen.x + px * offset,
        y: point.screen.y + py * offset,
        depth: point.screen.depth,
        r: point.radiance[0], g: point.radiance[1], b: point.radiance[2],
        alpha,
      });
      const leftA = vertex(a, drawA * 0.5, alphaA);
      const rightA = vertex(a, -drawA * 0.5, alphaA);
      const leftB = vertex(b, drawB * 0.5, alphaB);
      const rightB = vertex(b, -drawB * 0.5, alphaB);
      rasterizeTriangle(cache, leftA, leftB, rightB, width, height);
      rasterizeTriangle(cache, leftA, rightB, rightA, width, height);
    }
  }

  const composited = cache.composite(background, settings.coverageEarlyOut);
  const elapsedMs = Date.now() - started;
  return {
    schema: REFERENCE_RENDER_SCHEMA,
    width, height,
    rgb: composited.rgb,
    coverage: composited.coverage,
    stats: {
      backend: 'cpu-reference',
      exposure: settings.exposure,
      level: settings.level,
      strands: level.curves.length,
      segments: level.curves.reduce((sum, curve) => sum + Math.max(0, curve.points.length - 1), 0),
      ...composited.stats,
      clampedWidths,
      subPixelStrands,
      droppedSegments,
      layeredTransparency: {
        algorithm: 'bounded-per-pixel-layers',
        layersAvailable: settings.maxLayersPerPixel,
        overflowPixels: composited.stats.overflowPixels,
      },
      shadow: shadowGrid
        ? {
          gridSize: settings.shadowGridSize,
          coveredTexels: shadowGrid.coveredTexels,
          maxOpticalDepthMetres: shadowGrid.maximum,
          crossings: shadowGrid.crossings,
          layersPerTexel: shadowGrid.layers,
          droppedLayers: shadowGrid.droppedLayers,
        }
        : null,
      elapsedMs,
    },
    camera: {
      view: camera.view ?? null, position: [...camera.position], target: [...camera.target],
      fovYDegrees: camera.fovYDegrees, distanceMetres: camera.distanceMetres ?? null,
    },
    light: {
      direction: [...lightDirection], colour: [...light.colour], intensity,
      ambient: [...settings.ambient],
    },
  };
}

// 8-bit sRGB encode with a fixed exposure. Deterministic for a given buffer, which is what
// the image-hash assertions in the render test rely on.
export function toRgba8(result) {
  const { width, height, rgb, coverage } = result;
  const exposure = result.stats?.exposure ?? DEFAULT_RENDER_OPTIONS.exposure;
  const rgba = new Uint8Array(width * height * 4);
  const encode = (linear) => {
    const exposed = 1 - Math.exp(-Math.max(0, linear) * exposure);
    const value = exposed <= 0.0031308 ? 12.92 * exposed : 1.055 * Math.pow(exposed, 1 / 2.4) - 0.055;
    return Math.round(clamp01(value) * 255);
  };
  for (let pixel = 0; pixel < width * height; ++pixel) {
    rgba[pixel * 4] = encode(rgb[pixel * 3]);
    rgba[pixel * 4 + 1] = encode(rgb[pixel * 3 + 1]);
    rgba[pixel * 4 + 2] = encode(rgb[pixel * 3 + 2]);
    rgba[pixel * 4 + 3] = 255;
    void coverage;
  }
  return rgba;
}

// Grayscale coverage channel, used to prove that the silhouette is the strands rather than
// the background, and to compare two renders without a colour-dependent threshold.
export function coverageRgba8(result) {
  const { width, height, coverage } = result;
  const rgba = new Uint8Array(width * height * 4);
  for (let pixel = 0; pixel < width * height; ++pixel) {
    const value = Math.round(clamp01(coverage[pixel]) * 255);
    rgba[pixel * 4] = value; rgba[pixel * 4 + 1] = value; rgba[pixel * 4 + 2] = value;
    rgba[pixel * 4 + 3] = 255;
  }
  return rgba;
}

export function meanLuminance(result, { coveredOnly = true } = {}) {
  const { width, height, rgb, coverage } = result;
  let total = 0, count = 0;
  for (let pixel = 0; pixel < width * height; ++pixel) {
    if (coveredOnly && coverage[pixel] <= 0) continue;
    total += 0.2126 * rgb[pixel * 3] + 0.7152 * rgb[pixel * 3 + 1] + 0.0722 * rgb[pixel * 3 + 2];
    ++count;
  }
  return count ? total / count : 0;
}

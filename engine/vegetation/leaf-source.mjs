// Canonical leaf texture sources.
//
// AI generators and DCC exports do not agree with an engine about which pixel is the root, which
// way the tip points, how much transparent margin is safe, or whether the image is mirrored. The
// previous pipeline assumed all of that: `atlas.mjs` defaulted the pivot to the bottom edge of the
// cell, so a leaf whose art had a transparent margin hung in the air, and a leaf whose art touched
// the bottom edge was sampled from its own mip bleed.
//
// This module is the missing normalization step, stated as a contract:
//
//   source RGBA + metadata → one upright, single-subject, safe-margined leaf
//   root at (0.5, 0.0625) of the canonical sheet, tip toward +V, aspect preserved
//
// and the same transform applied to every associated map. Normal maps are not rotated as colour
// pixels: their tangent-space XY components are rotated with the surface, then re-encoded.
//
// The implementation is deliberately pure (raw RGBA in, raw RGBA out). PNG decoding belongs to the
// existing asset tools; normalizing is not allowed to grow a second image decoder.

export const LEAF_SOURCE_SCHEMA = 'nativegpu.leaf-texture-source/1';
export const LEAF_SOURCE_SCHEMA_VERSION = 2;

// The canonical slot, from VEGETATION-AI-TEXTURE-AND-DAMAGE-SPEC §2.2. Coordinates are UV with the
// origin bottom-left, U right, V up. A 1024 px sheet therefore has its root at (512, 960) in
// top-left pixel coordinates: 1/16 of the sheet above the bottom edge.
export const CANONICAL_LEAF = Object.freeze({
  imageSize: 1024,
  rootPivotUV: Object.freeze([0.5, 0.0625]),
  safeMarginFraction: 0.0625,
  contentUMin: 0.0625,
  contentUMax: 0.9375,
  tipMaxV: 0.9375,
  coordinates: Object.freeze({ origin: 'bottomLeft', u: 'right', v: 'up' }),
  normalConvention: 'directX-',
});

const NORMAL_CONVENTIONS = new Set(['directX-', 'openGL+']);

function fail(message, code = 'NG-E-VEG-LEAF-SOURCE', diagnostics = null) {
  const error = new Error(message);
  error.code = code;
  if (diagnostics) error.diagnostics = diagnostics;
  throw error;
}

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

function finite(value, label) {
  if (typeof value !== 'number' || !Number.isFinite(value)) fail(`${label} must be a finite number`);
  return value;
}

function rgbaOf(map, label) {
  if (!map || !map.pixels) fail(`${label} needs decoded RGBA pixels`);
  const { width, height, pixels } = map;
  if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) {
    fail(`${label} has an invalid size`);
  }
  if (pixels.length !== width * height * 4) {
    fail(`${label} pixel buffer is ${pixels.length} bytes, expected ${width * height * 4}`);
  }
  return map;
}

// ------------------------------------------------------------------ metadata
// Accepts the fields the spec makes mandatory for a single-leaf source. Optional fields are the
// ones a generator cannot know: explicit landmarks and the orientation it happened to draw in.
export function validateLeafSourceMetadata(metadata) {
  if (!metadata || typeof metadata !== 'object') fail('leaf source metadata must be an object');
  const problems = [];
  const require = (condition, message) => { if (!condition) problems.push(message); };
  require(metadata.schema === LEAF_SOURCE_SCHEMA, `schema must be ${LEAF_SOURCE_SCHEMA}`);
  require(metadata.assetKind === 'leaf', 'assetKind must be leaf');
  require(['bladeOnly', 'bladeWithPetiole'].includes(metadata.leafProfile), 'leafProfile must be bladeOnly or bladeWithPetiole');
  require(Array.isArray(metadata.imageSize) && metadata.imageSize.length === 2
    && metadata.imageSize.every((value) => Number.isInteger(value) && value > 0), 'imageSize must be [width,height]');
  require(metadata.coordinates?.origin === 'bottomLeft' && metadata.coordinates?.u === 'right' && metadata.coordinates?.v === 'up',
    'coordinates must be {origin:bottomLeft,u:right,v:up}');
  require(Array.isArray(metadata.rootPivotUV) && metadata.rootPivotUV.length === 2
    && metadata.rootPivotUV.every((value) => Number.isFinite(value) && value >= 0 && value <= 1),
    'rootPivotUV must be a [u,v] pair inside the sheet');
  require(Array.isArray(metadata.tipDirectionUV) && metadata.tipDirectionUV.length === 2
    && Math.abs(metadata.tipDirectionUV[0]) <= 0.05 && metadata.tipDirectionUV[1] >= 0.95,
    'tipDirectionUV must point up (+V): [0,1]');
  require(metadata.safeMarginFraction === undefined
    || (Number.isFinite(metadata.safeMarginFraction) && metadata.safeMarginFraction >= 0 && metadata.safeMarginFraction <= 0.25),
    'safeMarginFraction must be 0..0.25');
  require(metadata.bladeLengthMetres === undefined
    || (Number.isFinite(metadata.bladeLengthMetres) && metadata.bladeLengthMetres > 0), 'bladeLengthMetres must be positive');
  require(metadata.bladeWidthMetres === undefined
    || (Number.isFinite(metadata.bladeWidthMetres) && metadata.bladeWidthMetres > 0), 'bladeWidthMetres must be positive');
  require(metadata.normalConvention === undefined || NORMAL_CONVENTIONS.has(metadata.normalConvention),
    `normalConvention must be one of ${[...NORMAL_CONVENTIONS].join(', ')}`);
  require(metadata.baseDamageState === 'healthy', 'baseDamageState must be healthy: damage is a separate procedural layer');
  require(metadata.maps && typeof metadata.maps === 'object' && metadata.maps.baseColor, 'maps.baseColor is required');
  // This is the clockwise rotation the normalizer *applies* to make the leaf upright: if the
  // source was drawn rotated 90° clockwise, declare 270.
  const rotation = metadata.orientation?.rotateClockwiseDegrees ?? 0;
  require([0, 90, 180, 270].includes(rotation), 'orientation.rotateClockwiseDegrees must be 0, 90, 180 or 270');
  return { ok: problems.length === 0, problems };
}

// --------------------------------------------------------- orientation transform
// A clockwise raster rotation of content divides the pixel grid. We express the transform forward
// (source → upright) and inverse (upright sample → source sample), so every output pixel is filled
// by construction instead of leaving holes.
function orientationTransform(width, height, degrees, mirrored) {
  if (![0, 90, 180, 270].includes(degrees)) fail(`unsupported rotation ${degrees}`);
  const quarter = degrees / 90;
  const swap = quarter % 2 === 1;
  const outWidth = swap ? height : width;
  const outHeight = swap ? width : height;
  // Mirror is applied first, in source coordinates.
  const mirror = mirrored ? -1 : 1;
  // Forward: centre-relative screen coordinates (x right, y down), mirror, then rotate clockwise.
  const forward = (x, y) => {
    let px = (x - width / 2) * mirror;
    let py = y - height / 2;
    for (let turn = 0; turn < quarter; ++turn) {
      const nx = -py, ny = px;
      px = nx; py = ny;
    }
    return [px + outWidth / 2, py + outHeight / 2];
  };
  // Inverse is the same rotation counter-clockwise, then unmirror.
  const inverse = (x, y) => {
    let px = x - outWidth / 2;
    let py = y - outHeight / 2;
    for (let turn = 0; turn < quarter; ++turn) {
      const nx = py, ny = -px;
      px = nx; py = ny;
    }
    return [px * mirror + width / 2, py + height / 2];
  };
  // Tangent-space XY matrix for a vector painted on the content. Rotation is clockwise in screen
  // coordinates; UV V points up, so the screen matrix is converted once here.
  let matrix;
  if (quarter === 0) matrix = mirrored ? [-1, 0, 0, 1] : [1, 0, 0, 1];
  else if (quarter === 1) matrix = mirrored ? [0, 1, 1, 0] : [0, 1, -1, 0];
  else if (quarter === 2) matrix = mirrored ? [1, 0, 0, -1] : [-1, 0, 0, -1];
  else matrix = mirrored ? [0, -1, -1, 0] : [0, -1, 1, 0];
  return { width: outWidth, height: outHeight, degrees, mirrored, forward, inverse, matrix };
}

function rotateAlpha(alpha, width, height, transform) {
  const out = new Uint8Array(transform.width * transform.height);
  for (let y = 0; y < transform.height; ++y) {
    for (let x = 0; x < transform.width; ++x) {
      const [sx, sy] = transform.inverse(x + 0.5, y + 0.5);
      const px = Math.floor(sx), py = Math.floor(sy);
      if (px < 0 || py < 0 || px >= width || py >= height) continue;
      out[y * transform.width + x] = alpha[py * width + px];
    }
  }
  return out;
}

// ------------------------------------------------------------------- alpha mask
// The mask is the opacity channel when the source ships one, otherwise the base colour alpha.
// Keeping one definition is what stops "albedo.A * opacity" from being applied twice downstream.
function alphaMask(maps) {
  const base = rgbaOf(maps.baseColor, 'maps.baseColor');
  if (maps.opacity) {
    const opacity = rgbaOf(maps.opacity, 'maps.opacity');
    if (opacity.width !== base.width || opacity.height !== base.height) {
      fail('maps.opacity must share the baseColor dimensions (pre-normalization)');
    }
    const alpha = new Uint8Array(opacity.width * opacity.height);
    for (let index = 0; index < alpha.length; ++index) alpha[index] = opacity.pixels[index * 4];
    return alpha;
  }
  const alpha = new Uint8Array(base.width * base.height);
  for (let index = 0; index < alpha.length; ++index) alpha[index] = base.pixels[index * 4 + 3];
  return alpha;
}

// Connected components, 4-neighbour. Returns the largest component as a mask plus the count of
// components that are large enough to be a second subject. Tiny specks are not subjects: a stray
// antialiased pixel is not a second leaf, but a second leaf is.
function largestComponent(alpha, width, height, cutoff) {
  const visited = new Uint8Array(width * height);
  const component = new Int32Array(width * height).fill(-1);
  const queue = new Int32Array(width * height);
  const areas = [];
  let id = 0;
  for (let start = 0; start < alpha.length; ++start) {
    if (visited[start] || alpha[start] < cutoff) continue;
    let head = 0, tail = 0;
    queue[tail++] = start;
    visited[start] = 1;
    let area = 0;
    while (head < tail) {
      const index = queue[head++];
      component[index] = id;
      ++area;
      const x = index % width, y = (index - x) / width;
      if (x > 0) { const next = index - 1; if (!visited[next] && alpha[next] >= cutoff) { visited[next] = 1; queue[tail++] = next; } }
      if (x + 1 < width) { const next = index + 1; if (!visited[next] && alpha[next] >= cutoff) { visited[next] = 1; queue[tail++] = next; } }
      if (y > 0) { const next = index - width; if (!visited[next] && alpha[next] >= cutoff) { visited[next] = 1; queue[tail++] = next; } }
      if (y + 1 < height) { const next = index + width; if (!visited[next] && alpha[next] >= cutoff) { visited[next] = 1; queue[tail++] = next; } }
    }
    areas.push(area);
    ++id;
  }
  if (!areas.length) fail('the source has no opaque pixels');
  let largest = 0;
  for (let index = 1; index < areas.length; ++index) if (areas[index] > areas[largest]) largest = index;
  const significant = areas.filter((area) => area >= Math.max(16, areas[largest] * 0.02)).length;
  const mask = new Uint8Array(alpha.length);
  for (let index = 0; index < component.length; ++index) if (component[index] === largest) mask[index] = 1;
  return { mask, area: areas[largest], components: areas.length, significant };
}

function contentBounds(mask, width, height) {
  let minX = width, minY = height, maxX = -1, maxY = -1;
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      if (!mask[y * width + x]) continue;
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
  }
  if (maxX < 0) fail('the source has no opaque pixels');
  return { minX, minY, maxX, maxY, width: maxX - minX + 1, height: maxY - minY + 1 };
}

// Explicit landmarks are declared in source UV (bottom-left origin), so they are converted to
// pixels once and then carried through the same forward transform as the image.
function landmarkToUpright(uv, width, height, transform) {
  const [u, v] = uv;
  if (!Array.isArray(uv) || uv.length !== 2 || !Number.isFinite(u) || !Number.isFinite(v)) {
    fail('landmarks.rootUV must be a [u,v] pair');
  }
  return transform.forward(u * width, (1 - v) * height);
}

// Inference fallback. With an upright image the root is the lowest content column band; confidence
// is high when that band is narrow (a petiole or leaf base) and low when the whole blade touches the
// bottom (the source is probably cut off). The caller must not present an inferred low-confidence
// source as production-ready.
function inferRoot(mask, bounds, width) {
  // A narrow band (6% of the leaf's length) is close to the actual base. A taller band averages
  // over lobes and reports a root in the middle of the blade, which then reads as low confidence.
  const bandHeight = Math.max(2, Math.round(bounds.height * 0.06));
  const bandStart = bounds.maxY - bandHeight + 1;
  let sumX = 0, count = 0, minX = width, maxX = -1;
  for (let y = bandStart; y <= bounds.maxY; ++y) {
    for (let x = bounds.minX; x <= bounds.maxX; ++x) {
      if (!mask[y * width + x]) continue;
      sumX += x; ++count;
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
    }
  }
  if (!count) fail('could not infer a root from the content');
  const rootX = sumX / count;
  const bandWidth = maxX - minX + 1;
  // Confidence is "how much narrower the base band is than the leaf's widest part". A broad base
  // can still be a leaf, but it is ambiguous: the caller is told so instead of being promised a
  // measured landmark.
  const confidence = clamp(1 - bandWidth / Math.max(1, bounds.width), 0, 1);
  // Pixel-centre coordinates: the root sits half a texel below the last opaque row, so no content
  // ends up beneath it and the canonical root can be placed on the safe-margin line exactly.
  return { x: rootX + 0.5, y: bounds.maxY + 0.5, confidence: Number(confidence.toFixed(3)), method: 'inferred-bottom-band' };
}

// The tip is the topmost content band. Its x is taken from that band's centroid, so a leaning leaf
// reports a tip that points where the content actually goes rather than straight above its root.
function inferTip(mask, bounds, width) {
  const bandHeight = Math.max(1, Math.round(bounds.height * 0.06));
  let sumX = 0, count = 0, minY = bounds.maxY;
  for (let y = bounds.minY; y < Math.min(bounds.maxY + 1, bounds.minY + bandHeight); ++y) {
    for (let x = bounds.minX; x <= bounds.maxX; ++x) {
      if (!mask[y * width + x]) continue;
      sumX += x; ++count;
      if (y < minY) minY = y;
    }
  }
  return { x: count ? sumX / count : (bounds.minX + bounds.maxX) / 2, y: minY };
}

// --------------------------------------------------------------- map resampling
function sampleBilinear(pixels, width, height, x, y) {
  const x0 = Math.floor(x), y0 = Math.floor(y);
  const fx = x - x0, fy = y - y0;
  const at = (px, py) => {
    const cx = clamp(px, 0, width - 1), cy = clamp(py, 0, height - 1);
    const offset = (cy * width + cx) * 4;
    return [pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]];
  };
  const a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
  const out = [0, 0, 0, 0];
  for (let channel = 0; channel < 4; ++channel) {
    out[channel] = Math.round(a[channel] * (1 - fx) * (1 - fy) + b[channel] * fx * (1 - fy)
      + c[channel] * (1 - fx) * fy + d[channel] * fx * fy);
  }
  return out;
}

// Normal maps carry a tangent-space direction, not a colour. The orientation transform's 2×2
// matrix is applied to the decoded XY vector; the Z component is kept and the vector renormalised.
// The green channel convention is converted exactly once: directX- stores +Y downward, while the
// canonical UV basis is +V upward.
function transformNormal(rgb, matrix, convention) {
  let nx = (rgb[0] / 127.5) - 1;
  let ny = (rgb[1] / 127.5) - 1;
  const nz = (rgb[2] / 127.5) - 1;
  if (convention === 'directX-') ny = -ny;
  const tx = matrix[0] * nx + matrix[1] * ny;
  const ty = matrix[2] * nx + matrix[3] * ny;
  nx = tx; ny = ty;
  if (convention === 'directX-') ny = -ny;
  const length = Math.hypot(nx, ny, nz);
  if (length < 1e-6) return [128, 128, 255, rgb[3]];
  nx /= length; ny /= length;
  const nzOut = nz / length;
  return [
    Math.round((nx * 0.5 + 0.5) * 255),
    Math.round((ny * 0.5 + 0.5) * 255),
    Math.round((nzOut * 0.5 + 0.5) * 255),
    rgb[3],
  ];
}

function canonicalizeMap(map, alphaMap, transform, layout, options = {}) {
  const { width, height, pixels } = map;
  const out = new Uint8Array(layout.size * layout.size * 4);
  for (let y = 0; y < layout.size; ++y) {
    for (let x = 0; x < layout.size; ++x) {
      // Canonical pixel → canonical sheet UV → upright pixel → source pixel.
      const cu = (x + 0.5) / layout.size;
      const cv = 1 - (y + 0.5) / layout.size;
      const uprightX = (cu * layout.size - layout.tx) / layout.scale;
      const uprightY = ((1 - cv) * layout.size - layout.ty) / layout.scale;
      const [sourceX, sourceY] = transform.inverse(uprightX, uprightY);
      if (sourceX < 0 || sourceY < 0 || sourceX >= width || sourceY >= height) continue;
      let rgba = sampleBilinear(pixels, width, height, sourceX - 0.5, sourceY - 0.5);
      // Coverage comes from the canonical alpha mask, not from each map's own alpha: one source of
      // truth for "is this texel part of the leaf".
      const outX = x, outY = y;
      const coverage = alphaMap[outY * layout.size + outX];
      if (coverage === 0) { out[(outY * layout.size + outX) * 4 + 3] = 0; continue; }
      rgba[3] = coverage;
      if (options.normal) rgba = transformNormal(rgba, transform.matrix, options.normalConvention);
      const offset = (outY * layout.size + outX) * 4;
      out[offset] = rgba[0]; out[offset + 1] = rgba[1]; out[offset + 2] = rgba[2]; out[offset + 3] = rgba[3];
    }
  }
  return { pixels: out, width: layout.size, height: layout.size };
}

// ----------------------------------------------------------------- normalize
export function normalizeLeafSource({ maps, metadata }) {
  const validation = validateLeafSourceMetadata(metadata);
  if (!validation.ok) fail(`leaf source metadata is invalid: ${validation.problems.join('; ')}`, 'NG-E-VEG-LEAF-SOURCE', validation.problems);
  const base = rgbaOf(maps.baseColor, 'maps.baseColor');
  if (metadata.imageSize[0] !== base.width || metadata.imageSize[1] !== base.height) {
    fail(`metadata.imageSize ${metadata.imageSize.join('x')} disagrees with the decoded image ${base.width}x${base.height}`);
  }
  for (const key of ['opacity', 'normal', 'orm', 'thickness']) {
    const map = maps[key];
    if (!map) continue;
    rgbaOf(map, `maps.${key}`);
    if (map.width !== base.width || map.height !== base.height) {
      fail(`maps.${key} is ${map.width}x${map.height}; every map must share the baseColor dimensions`);
    }
  }

  const sourceAlpha = alphaMask(maps);
  const cutoff = Math.round((metadata.alphaCutoff ?? 0.5) * 255);
  const components = largestComponent(sourceAlpha, base.width, base.height, cutoff);
  if (components.significant > 1) {
    fail(`the source contains ${components.significant} separate opaque subjects; expected one leaf`,
      'NG-E-VEG-LEAF-SOURCE', { reason: 'multiple-subjects', components: components.significant });
  }
  // A leaf that touches an edge has been cut off. Reject rather than silently crop it to the
  // margin, which is how a "one-click import" can destroy a leaf and still report success.
  const sourceBounds = contentBounds(components.mask, base.width, base.height);
  if (sourceBounds.minX === 0 || sourceBounds.minY === 0
    || sourceBounds.maxX === base.width - 1 || sourceBounds.maxY === base.height - 1) {
    fail('the leaf touches the image edge; it must have a transparent margin on all sides',
      'NG-E-VEG-LEAF-SOURCE', { reason: 'cut-off', bounds: sourceBounds });
  }

  const transform = orientationTransform(base.width, base.height,
    metadata.orientation?.rotateClockwiseDegrees ?? 0, metadata.orientation?.mirrored === true);
  const uprightAlpha = rotateAlpha(components.mask, base.width, base.height, transform);
  // The rotated buffer is a 0/1 mask, not coverage: threshold it as binary, not against the
  // source's alpha cutoff (which would always find "no opaque pixels").
  const uprightComponents = largestComponent(uprightAlpha, transform.width, transform.height, 1);
  if (uprightComponents.significant > 1) {
    fail(`after rotation the source contains ${uprightComponents.significant} subjects`,
      'NG-E-VEG-LEAF-SOURCE', { reason: 'multiple-subjects' });
  }
  const bounds = contentBounds(uprightComponents.mask, transform.width, transform.height);

  let root;
  let landmarkPixel = null;
  if (metadata.landmarks?.rootUV) {
    const [x, y] = landmarkToUpright(metadata.landmarks.rootUV, base.width, base.height, transform);
    landmarkPixel = [x, y];
    root = { x, y, confidence: 1, method: 'landmark' };
  } else {
    root = inferRoot(uprightComponents.mask, bounds, transform.width);
  }
  // A declared landmark sits on the leaf's boundary, and both UVs and the transform use pixel
  // centres: allow two pixels so a landmark exactly on the base is not rejected by half a texel.
  if (root.x < bounds.minX - 2 || root.x > bounds.maxX + 2 || root.y < bounds.minY - 2 || root.y > bounds.maxY + 2) {
    fail('the declared root landmark is outside the leaf', 'NG-E-VEG-LEAF-SOURCE', { reason: 'root-outside-content', root });
  }
  const tip = inferTip(uprightComponents.mask, bounds, transform.width);

  // The canonical slot. Scale is chosen so the content fits the safe box *relative to the root*,
  // which keeps the root exactly at the contract pivot without distorting aspect.
  const size = CANONICAL_LEAF.imageSize;
  const margin = metadata.safeMarginFraction ?? CANONICAL_LEAF.safeMarginFraction;
  const rootU = metadata.rootPivotUV[0], rootV = metadata.rootPivotUV[1];
  const tipMaxV = Math.min(CANONICAL_LEAF.tipMaxV, 1 - margin);
  const upPixels = Math.max(1, root.y - bounds.minY);
  const leftPixels = Math.max(1, root.x - bounds.minX);
  const rightPixels = Math.max(1, bounds.maxX - root.x);
  // Content below the root would sit outside the 1/16 bottom margin, because the root is *on* that
  // line. A landmark that is not the leaf base is therefore rejected rather than silently cropped.
  const downPixels = Math.max(0, bounds.maxY - root.y);
  // A two-texel inset absorbs the half-texel convention and the one-pixel shift a quarter-turn
  // rotation introduces. Without it the exact fit lands on the margin boundary and a rotated input
  // drifts just outside it.
  const inset = 2;
  const scale = Math.min(
    ((tipMaxV - rootV) * size - inset) / upPixels,
    ((rootU - margin) * size - inset) / leftPixels,
    ((1 - margin - rootU) * size - inset) / rightPixels,
    downPixels > 0 ? ((rootV - margin) * size - inset) / downPixels : Infinity,
  );
  if (!(scale > 1e-6)) fail('the leaf cannot be placed inside the canonical safe margin');
  const tx = rootU * size - root.x * scale;
  const ty = (1 - rootV) * size - root.y * scale;
  const layout = { size, scale, tx, ty, rootU, rootV, margin };

  const canonicalAlpha = new Uint8Array(size * size);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const uprightX = (x + 0.5 - tx) / scale;
      const uprightY = (y + 0.5 - ty) / scale;
      const px = Math.floor(uprightX), py = Math.floor(uprightY);
      if (px < 0 || py < 0 || px >= transform.width || py >= transform.height) continue;
      if (uprightComponents.mask[py * transform.width + px]) canonicalAlpha[y * size + x] = 255;
    }
  }
  // Rebuild the mask from the resampled alpha so mesh/atlas and textures agree on coverage.
  const canonicalMask = new Uint8Array(size * size);
  for (let index = 0; index < canonicalAlpha.length; ++index) canonicalMask[index] = canonicalAlpha[index] ? 1 : 0;
  const canonicalBounds = contentBounds(canonicalMask, size, size);

  const canonicalMaps = {};
  for (const key of ['baseColor', 'opacity', 'orm', 'thickness']) {
    if (!maps[key]) continue;
    canonicalMaps[key] = canonicalizeMap(maps[key], canonicalAlpha, transform, layout);
  }
  if (maps.normal) {
    canonicalMaps.normal = canonicalizeMap(maps.normal, canonicalAlpha, transform, layout, {
      normal: true, normalConvention: metadata.normalConvention ?? CANONICAL_LEAF.normalConvention,
    });
  }
  // Base colour alpha is the coverage, exactly once. When the source ships a separate opacity
  // sheet, its resampled value is the coverage so antialiased edges survive normalization; the
  // binary mask is only used when there is no opacity map (a cutout silhouette).
  if (canonicalMaps.baseColor) {
    const opacityPixels = canonicalMaps.opacity?.pixels ?? null;
    for (let index = 0; index < size * size; ++index) {
      canonicalMaps.baseColor.pixels[index * 4 + 3] = opacityPixels ? opacityPixels[index * 4] : canonicalAlpha[index];
    }
  }

  const rootCanonical = { x: tx + root.x * scale, y: ty + root.y * scale };
  const tipCanonical = { x: tx + tip.x * scale, y: ty + tip.y * scale };
  const toUV = (x, y) => [x / size, 1 - y / size];
  const contentBoundsUV = {
    u0: canonicalBounds.minX / size,
    v0: 1 - (canonicalBounds.maxY + 1) / size,
    u1: (canonicalBounds.maxX + 1) / size,
    v1: 1 - canonicalBounds.minY / size,
  };
  const rootPivotUV = toUV(rootCanonical.x, rootCanonical.y);
  const tipUV = toUV(tipCanonical.x, tipCanonical.y);
  const rootErrorPixels = Math.hypot(rootPivotUV[0] * size - rootU * size, (1 - rootPivotUV[1]) * size - (1 - rootV) * size);
  const pixelsPerMetre = metadata.bladeLengthMetres
    ? (Math.hypot(tip.x - root.x, tip.y - root.y) * scale) / metadata.bladeLengthMetres
    : null;
  if (rootErrorPixels > 1.5) {
    fail(`canonical root is ${rootErrorPixels.toFixed(2)} px from the contract pivot`, 'NG-E-VEG-LEAF-SOURCE', { reason: 'root-error' });
  }
  // The safe-margin contract is a hard check, not a suggestion. The tolerance is the resampling
  // footprint: a source texel spans 1/scale canonical pixels, so the last row centred on the root
  // line can cover a few pixels below it even though no content is authored there.
  const footprint = Math.ceil(scale) + 1;
  if (canonicalBounds.minX < Math.floor(margin * size) - footprint
    || canonicalBounds.maxX > Math.ceil((1 - margin) * size) + footprint
    || canonicalBounds.minY < Math.floor(margin * size) - footprint
    || canonicalBounds.maxY > Math.ceil((1 - margin) * size) + footprint) {
    fail('the normalized leaf leaves the safe margin', 'NG-E-VEG-LEAF-SOURCE', { reason: 'safe-margin', bounds: canonicalBounds, footprint });
  }

  const metadataOut = {
    schema: LEAF_SOURCE_SCHEMA,
    schemaVersion: LEAF_SOURCE_SCHEMA_VERSION,
    assetKind: 'leaf',
    leafProfile: metadata.leafProfile,
    speciesId: metadata.speciesId ?? null,
    side: metadata.side ?? 'front',
    imageSize: [size, size],
    sourceImageSize: [base.width, base.height],
    coordinates: { ...CANONICAL_LEAF.coordinates },
    rootPivotUV: [Number(rootPivotUV[0].toFixed(6)), Number(rootPivotUV[1].toFixed(6))],
    tipDirectionUV: [0, 1],
    tipUV: [Number(tipUV[0].toFixed(6)), Number(tipUV[1].toFixed(6))],
    contentBoundsUV: {
      u0: Number(contentBoundsUV.u0.toFixed(6)), v0: Number(contentBoundsUV.v0.toFixed(6)),
      u1: Number(contentBoundsUV.u1.toFixed(6)), v1: Number(contentBoundsUV.v1.toFixed(6)),
    },
    safeMarginFraction: margin,
    bladeLengthMetres: metadata.bladeLengthMetres ?? null,
    bladeWidthMetres: metadata.bladeWidthMetres ?? null,
    pixelsPerMetre: pixelsPerMetre === null ? null : Number(pixelsPerMetre.toFixed(3)),
    normalConvention: metadata.normalConvention ?? CANONICAL_LEAF.normalConvention,
    baseDamageState: 'healthy',
    maps: Object.fromEntries(Object.entries(metadata.maps).map(([key, value]) => [key, { ...value }])),
    provenance: { ...(metadata.provenance ?? {}), normalizedFrom: metadata.provenance?.source ?? null },
    transform: {
      rotateClockwiseDegrees: transform.degrees,
      mirrored: transform.mirrored,
      scale: Number(scale.toFixed(6)),
      translatePixels: [Number(tx.toFixed(3)), Number(ty.toFixed(3))],
      normalMatrix: transform.matrix,
      rootMethod: root.method,
      rootConfidence: root.confidence,
    },
    landmarks: {
      rootUV: metadata.landmarks?.rootUV ?? null,
      rootSourcePixel: [Number(root.x.toFixed(3)), Number(root.y.toFixed(3))],
      landmarkUprightPixel: landmarkPixel ? [Number(landmarkPixel[0].toFixed(3)), Number(landmarkPixel[1].toFixed(3))] : null,
      bladeBaseUV: metadata.landmarks?.bladeBaseUV ?? null,
    },
    validation: {
      status: 'ok',
      components: components.significant,
      rootErrorPixels: Number(rootErrorPixels.toFixed(4)),
      tipTolerancePixels: null,
      inference: root.method === 'inferred-bottom-band' ? (root.confidence >= 0.5 ? 'inferred' : 'inferred-low') : 'declared',
    },
  };
  return { maps: canonicalMaps, metadata: metadataOut };
}

// Brush geometry: stroke resampling, region masks and path sampling.
//
// The contract that matters here is distance, not event count. A pen that reports 400
// pointermove events over 3 metres and a pen that reports 40 must produce the same
// stamped circles, otherwise the world changes depending on the user's mouse polling rate.
//
// Everything is 2D in world XZ metres. Height is resolved later against the terrain so a
// brush stroke can be authored before the terrain exists.

export const STAMP_SPACING_FRACTION = 0.35;

export function distance2D(a, b) {
  const dx = b[0] - a[0];
  const dz = b[1] - a[1];
  return Math.hypot(dx, dz);
}

/**
 * Resamples a polyline into stamps spaced by `spacingMetres`.
 *
 * Returns `{ x, z, t }` where `t` is the normalized distance along the stroke, which the
 * caller uses to fade strength at the ends of a stroke.
 */
export function resampleStroke(points, spacingMetres, { includeEndpoints = true } = {}) {
  if (!Array.isArray(points) || points.length === 0) return [];
  if (!(spacingMetres > 0)) return points.map((point, index) => ({
    x: point[0], z: point[1], t: points.length === 1 ? 0 : index / (points.length - 1),
  }));
  const stamps = [];
  const total = strokeLength(points);
  if (total <= spacingMetres || points.length === 1) {
    const first = points[0];
    stamps.push({ x: first[0], z: first[1], t: 0 });
    const last = points[points.length - 1];
    if (distance2D(first, last) > 1e-6) stamps.push({ x: last[0], z: last[1], t: 1 });
    return stamps;
  }
  let travelled = 0;
  let next = 0;
  for (let index = 1; index < points.length; index += 1) {
    const from = points[index - 1];
    const to = points[index];
    const segment = distance2D(from, to);
    if (segment <= 1e-9) continue;
    while (next <= travelled + segment) {
      const ratio = (next - travelled) / segment;
      stamps.push({
        x: from[0] + (to[0] - from[0]) * ratio,
        z: from[1] + (to[1] - from[1]) * ratio,
        t: Math.min(1, next / total),
      });
      next += spacingMetres;
    }
    travelled += segment;
  }
  if (includeEndpoints) {
    const last = points[points.length - 1];
    const tail = stamps[stamps.length - 1];
    if (!tail || distance2D([tail.x, tail.z], last) > spacingMetres * 0.5) {
      stamps.push({ x: last[0], z: last[1], t: 1 });
    }
  }
  return stamps;
}

export function strokeLength(points) {
  let total = 0;
  for (let index = 1; index < points.length; index += 1) total += distance2D(points[index - 1], points[index]);
  return total;
}

/**
 * Bounds of a stroke expanded by a radius. Used for dirty tracking: a brush only dirties
 * the chunks its *influence* covers, not the ones its centre line passes through.
 */
export function strokeBounds(points, radiusMetres = 0) {
  if (!Array.isArray(points) || points.length === 0) {
    return { minX: 0, minZ: 0, maxX: 0, maxZ: 0 };
  }
  let minX = Infinity, minZ = Infinity, maxX = -Infinity, maxZ = -Infinity;
  for (const point of points) {
    if (point[0] < minX) minX = point[0];
    if (point[1] < minZ) minZ = point[1];
    if (point[0] > maxX) maxX = point[0];
    if (point[1] > maxZ) maxZ = point[1];
  }
  return {
    minX: minX - radiusMetres, minZ: minZ - radiusMetres,
    maxX: maxX + radiusMetres, maxZ: maxZ + radiusMetres,
  };
}

export function boundsUnion(a, b) {
  if (!a) return b;
  if (!b) return a;
  return {
    minX: Math.min(a.minX, b.minX), minZ: Math.min(a.minZ, b.minZ),
    maxX: Math.max(a.maxX, b.maxX), maxZ: Math.max(a.maxZ, b.maxZ),
  };
}

export function boundsIntersect(a, b) {
  return a.minX < b.maxX && b.minX < a.maxX && a.minZ < b.maxZ && b.minZ < a.maxZ;
}

export function boundsContains(outer, inner) {
  return outer.minX <= inner.minX && outer.minZ <= inner.minZ
    && outer.maxX >= inner.maxX && outer.maxZ >= inner.maxZ;
}

export function pointInBounds(bounds, x, z) {
  return x >= bounds.minX && x <= bounds.maxX && z >= bounds.minZ && z <= bounds.maxZ;
}

/**
 * Even-odd point-in-polygon. The polygon is an array of [x, z] pairs; it is closed
 * implicitly, so the caller must not repeat the first point.
 */
export function pointInPolygon(points, x, z) {
  let inside = false;
  for (let index = 0, previous = points.length - 1; index < points.length; previous = index, index += 1) {
    const [xi, zi] = points[index];
    const [xj, zj] = points[previous];
    const intersects = ((zi > z) !== (zj > z))
      && (x < ((xj - xi) * (z - zi)) / ((zj - zi) || 1e-12) + xi);
    if (intersects) inside = !inside;
  }
  return inside;
}

export function polygonBounds(points) {
  return strokeBounds(points, 0);
}

/**
 * Falloff weight for a distance inside a brush. 1 in the core, 0 at the rim.
 * `falloffMetres` = 0 is a hard edge, which is what the "stamp" mode wants.
 */
export function brushWeight(distanceMetres, radiusMetres, falloffMetres) {
  if (radiusMetres <= 0) return 0;
  if (distanceMetres >= radiusMetres) return 0;
  if (falloffMetres <= 0) return 1;
  const inner = Math.max(0, radiusMetres - falloffMetres);
  if (distanceMetres <= inner) return 1;
  const t = (distanceMetres - inner) / (radiusMetres - inner);
  // Smoothstep: a linear ramp leaves a visible ring where the density derivative jumps.
  return 1 - t * t * (3 - 2 * t);
}

/**
 * Catmull-Rom resampling of a path's control points. Roads and rivers are authored as a
 * handful of semantic nodes; the geometry the compiler flattens is the spline, not the
 * polygon, or every corner would be a hard angle.
 */
export function sampleCatmullRom(points, spacingMetres, { closed = false, tension = 0.5 } = {}) {
  if (!Array.isArray(points) || points.length < 2) return [];
  if (points.length === 2) return resampleStroke(points, spacingMetres);
  const output = [];
  const count = closed ? points.length : points.length - 1;
  const at = index => {
    if (closed) return points[(index + points.length) % points.length];
    return points[Math.max(0, Math.min(points.length - 1, index))];
  };
  for (let index = 0; index < count; index += 1) {
    const p0 = at(index - 1);
    const p1 = at(index);
    const p2 = at(index + 1);
    const p3 = at(index + 2);
    const segment = distance2D(p1, p2);
    const steps = Math.max(1, Math.ceil(segment / Math.max(spacingMetres, 0.05)));
    for (let step = 0; step < steps; step += 1) {
      const t = step / steps;
      output.push(catmullRomPoint(p0, p1, p2, p3, t, tension));
    }
  }
  if (!closed) output.push([...points[points.length - 1]]);
  return output;
}

function catmullRomPoint(p0, p1, p2, p3, t, tension) {
  const t2 = t * t;
  const t3 = t2 * t;
  const a = -tension * t3 + 2 * tension * t2 - tension * t;
  const b = (2 - tension) * t3 + (tension - 3) * t2 + 1;
  const c = (tension - 2) * t3 + (3 - 2 * tension) * t2 + tension * t;
  const d = tension * t3 - tension * t2;
  return [
    a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1],
  ];
}

/**
 * Distance from a point to a polyline. Used by exclusion zones and by the "is this
 * building footprint clear of the road" check.
 */
export function distanceToPolyline(points, x, z) {
  let best = Infinity;
  for (let index = 1; index < points.length; index += 1) {
    best = Math.min(best, distanceToSegment(points[index - 1], points[index], x, z));
  }
  if (points.length === 1) best = distance2D(points[0], [x, z]);
  return best;
}

export function distanceToSegment(a, b, x, z) {
  const dx = b[0] - a[0];
  const dz = b[1] - a[1];
  const lengthSquared = dx * dx + dz * dz;
  if (lengthSquared <= 1e-12) return Math.hypot(x - a[0], z - a[1]);
  let t = ((x - a[0]) * dx + (z - a[1]) * dz) / lengthSquared;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(x - (a[0] + dx * t), z - (a[1] + dz * t));
}

/**
 * Validates a path graph before it reaches the compiler. The compiler still re-checks
 * everything (it cannot trust a file), but the editor uses this to grey out the Generate
 * button instead of letting a user start a 40 s cook that must fail.
 */
export function pathDiagnostics(entry) {
  const diagnostics = [];
  const points = entry?.points ?? [];
  if (points.length < 2) {
    diagnostics.push({ code: 'PATH_TOO_SHORT', message: `path '${entry?.id}' needs at least two control points` });
    return diagnostics;
  }
  for (let index = 1; index < points.length; index += 1) {
    const segment = distance2D(points[index - 1], points[index]);
    if (segment < 0.25) {
      diagnostics.push({
        code: 'PATH_DUPLICATE_POINT',
        message: `path '${entry.id}' has a duplicate control point at index ${index}`,
        suggestion: 'Remove the repeated node; a zero-length segment has no direction.',
      });
    }
  }
  // Sharp corners: the angle between consecutive segments. A road that turns more than
  // 150° in one node cannot be flattened without a hole in the surface.
  for (let index = 1; index < points.length - 1; index += 1) {
    const previous = points[index - 1];
    const current = points[index];
    const next = points[index + 1];
    const ax = current[0] - previous[0];
    const az = current[1] - previous[1];
    const bx = next[0] - current[0];
    const bz = next[1] - current[1];
    const la = Math.hypot(ax, az);
    const lb = Math.hypot(bx, bz);
    if (la < 1e-6 || lb < 1e-6) continue;
    const cosine = Math.max(-1, Math.min(1, (ax * bx + az * bz) / (la * lb)));
    const degrees = Math.acos(cosine) * 180 / Math.PI;
    if (degrees > 150) {
      diagnostics.push({
        code: 'PATH_SHARP_TURN',
        message: `path '${entry.id}' turns ${degrees.toFixed(0)}° at node ${index}`,
        suggestion: 'Insert an intermediate node; the surface cannot be flattened through a hairpin.',
      });
    }
  }
  // Self-intersection: only non-adjacent segments count.
  for (let a = 0; a < points.length - 1; a += 1) {
    for (let b = a + 2; b < points.length - 1; b += 1) {
      if (segmentsIntersect(points[a], points[a + 1], points[b], points[b + 1])) {
        diagnostics.push({
          code: 'PATH_SELF_INTERSECTS',
          message: `path '${entry.id}' crosses itself between segments ${a} and ${b}`,
          suggestion: 'Split the path at the crossing and use a junction node.',
        });
      }
    }
  }
  return diagnostics;
}

function segmentsIntersect(a1, a2, b1, b2) {
  const d1 = cross2(a1, a2, b1);
  const d2 = cross2(a1, a2, b2);
  const d3 = cross2(b1, b2, a1);
  const d4 = cross2(b1, b2, a2);
  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0))
    && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

function cross2(a, b, c) {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

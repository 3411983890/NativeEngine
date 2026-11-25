// Curve maths for groom assets.
//
// Units are metres everywhere. Radius is a *radius*, never a diameter: the plan calls
// this out because mixing them silently halves or doubles every hair on a character.

export class GroomError extends Error {
  constructor(code, message) { super(message); this.name = 'GroomError'; this.code = code; }
}
const fail = (ok, code, message) => { if (!ok) throw new GroomError(code, message); };

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const length = (a) => Math.hypot(a[0], a[1], a[2]);
const normalize = (a) => { const l = length(a); return l < 1e-12 ? [0, 1, 0] : [a[0] / l, a[1] / l, a[2] / l]; };

export function arcLength(points) {
  let total = 0;
  const cumulative = [0];
  for (let i = 1; i < points.length; ++i) { total += length(sub(points[i], points[i - 1])); cumulative.push(total); }
  return { total, cumulative };
}

export function validateCurve(points, radii, label = 'curve') {
  fail(Array.isArray(points) && points.length >= 2 && points.length <= 256, 'NG-E-GROOM-ATTRIBUTE', `${label} needs 2–256 points`);
  fail(Array.isArray(radii) && radii.length === points.length, 'NG-E-GROOM-ATTRIBUTE', `${label} radius count must match its points`);
  for (let i = 0; i < points.length; ++i) {
    const point = points[i];
    fail(Array.isArray(point) && point.length === 3, 'NG-E-GROOM-ATTRIBUTE', `${label} point ${i} is not a 3-vector`);
    for (const value of point) fail(Number.isFinite(value) && Math.abs(value) <= 1e4, 'NG-E-GROOM-ATTRIBUTE', `${label} point ${i} is not finite`);
    fail(Number.isFinite(radii[i]) && radii[i] > 0 && radii[i] <= 0.05, 'NG-E-GROOM-ATTRIBUTE', `${label} radius ${i} must be in (0, 0.05] m`);
  }
  const { total } = arcLength(points);
  // A zero-length curve is not a short hair: it is a degenerate root that would divide
  // by zero in every tangent calculation downstream.
  fail(total > 1e-4, 'NG-E-GROOM-ATTRIBUTE', `${label} has zero arc length`);
  return total;
}

// Centripetal Catmull–Rom. A linear resample of a curved guide produces visible
// faceting at every knot, which the fiber shading then reads as a kink.
function catmullRom(p0, p1, p2, p3, t) {
  const t2 = t * t, t3 = t2 * t;
  return [0, 1, 2].map((axis) => 0.5 * ((2 * p1[axis])
    + (-p0[axis] + p2[axis]) * t
    + (2 * p0[axis] - 5 * p1[axis] + 4 * p2[axis] - p3[axis]) * t2
    + (-p0[axis] + 3 * p1[axis] - 3 * p2[axis] + p3[axis]) * t3));
}

export function resampleCurve(points, radii, count) {
  fail(count >= 2 && count <= 256, 'NG-E-GROOM-ATTRIBUTE', 'resample count must be in [2,256]');
  const { cumulative, total } = arcLength(points);
  const resultPoints = [], resultRadii = [];
  for (let sample = 0; sample < count; ++sample) {
    const target = (sample / (count - 1)) * total;
    let segment = 1;
    while (segment < cumulative.length - 1 && cumulative[segment] < target) ++segment;
    const previous = Math.max(0, segment - 1);
    const span = Math.max(1e-9, cumulative[segment] - cumulative[previous]);
    const t = Math.max(0, Math.min(1, (target - cumulative[previous]) / span));
    const p0 = points[Math.max(0, previous - 1)], p1 = points[previous];
    const p2 = points[segment], p3 = points[Math.min(points.length - 1, segment + 1)];
    resultPoints.push(catmullRom(p0, p1, p2, p3, t));
    // Radius interpolates linearly: it is a cross-section property, not a shape.
    resultRadii.push(radii[previous] * (1 - t) + radii[segment] * t);
  }
  return { points: resultPoints, radii: resultRadii };
}

// Root-to-tip parameterisation. Every consumer that needs "where along the hair" uses
// this instead of a point index, because LODs have different point counts.
export function arcParameters(points) {
  const { cumulative, total } = arcLength(points);
  return cumulative.map((value) => (total < 1e-9 ? 0 : value / total));
}

export function tangentAt(points, index) {
  const previous = points[Math.max(0, index - 1)];
  const next = points[Math.min(points.length - 1, index + 1)];
  return normalize(sub(next, previous));
}

export function boundsOf(curves) {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (const curve of curves) for (const point of curve.points) for (let axis = 0; axis < 3; ++axis) {
    min[axis] = Math.min(min[axis], point[axis]);
    max[axis] = Math.max(max[axis], point[axis]);
  }
  return { min, max, centre: [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2] };
}

export { add, sub, mul, dot, length, normalize };

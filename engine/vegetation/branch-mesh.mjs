// Continuous wood surface.
//
// The previous mesher emitted one independent tapered cylinder per segment (`tube(a, b)`).
// Each segment recomputed its own basis with `perpendicular(axis)`, so at every bend the ring
// rotated arbitrarily: the surface cracked, normals disagreed across the crack, and the bark
// UV jumped. The seam was never duplicated either, so the last quad interpolated U from ~1
// back to 0 across the entire texture.
//
// This module sweeps one tube along the whole branch curve instead:
//
//   * rings are shared between adjacent spans (one ring per centreline point);
//   * the frame is transported from point to point (rotation-minimising), so the surface
//     does not twist;
//   * the circumference seam is a duplicated vertex: identical position and normal, U = 0
//     and U = uScale, so the seam triangle is zero-width in UV rather than a full wrap;
//   * the normal includes the radius derivative, because a cone's surface normal is not the
//     radial direction;
//   * V is the branch's own arc length in metres divided by the tile size, offset by the
//     parent's phase at the attachment, so bark does not restart at every joint.
//
// Junctions get a collar: a band whose outer ring is projected onto the parent's cylinder.
// The child root is already *inside* the parent (branch-graph puts it on the parent
// centreline, with no gap), so the union is closed; the collar turns the hard intersection
// into a fillet that reads as wood thickening into a limb.

import { add, cross, dot, mul, normalize, sampleBranch, sub } from './branch-graph.mjs';

const TAU = Math.PI * 2;

function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-MESH';
  throw error;
}

// Sweeps one branch. `mesh` is any object with begin/end/vertex/quad, which keeps this module
// independent of the package builder and of the editor preview.
export function emitBranch(mesh, branch, options) {
  const radial = Math.max(3, Math.round(options.radial ?? 6));
  const tileMetres = options.tileMetres ?? 1.6;
  const phase = options.uvPhaseMetres ?? 0;
  const material = options.materialId;
  const colour = options.color ?? [110, 88, 64, 255];
  const points = branch.points;
  if (!points || points.length < 2) fail(`branch ${branch.id} needs at least two points to sweep`);
  const stride = radial + 1; // seam duplicate
  // U is scaled by the root circumference and held constant along the branch: a per-ring
  // scale would shear the texture along the taper and move the seam from ring to ring.
  const uScale = (TAU * branch.rootRadius) / tileMetres;

  mesh.begin(material);
  const rings = [];
  for (let index = 0; index < points.length; ++index) {
    const point = points[index];
    const binormal = normalize(cross(point.tangent, point.normal));
    const previous = points[Math.max(0, index - 1)];
    const next = points[Math.min(points.length - 1, index + 1)];
    const ds = Math.max(1e-6, next.arcLength - previous.arcLength);
    // dr/ds tilts the surface normal along the taper. Without it every normal is the radial
    // direction of a constant-radius cylinder and the shading reads as a pipe.
    const drds = (next.radius - previous.radius) / ds;
    const v = (point.arcLength + phase) / tileMetres;
    const ring = [];
    for (let j = 0; j <= radial; ++j) {
      const angle = (j / radial) * TAU;
      const direction = add(mul(point.normal, Math.cos(angle)), mul(binormal, Math.sin(angle)));
      const position = add(point.position, mul(direction, point.radius));
      const normal = normalize(sub(direction, mul(point.tangent, drds)));
      ring.push(mesh.vertex(position, normal, [(j / radial) * uScale, v], colour, point.wind));
    }
    rings.push(ring);
  }
  for (let index = 1; index < rings.length; ++index) {
    const previous = rings[index - 1], current = rings[index];
    for (let j = 0; j < radial; ++j) {
      mesh.quad(previous[j], previous[j + 1], current[j + 1], current[j]);
    }
  }

  if (options.capTip !== false) {
    // A tip fan, not a disc: an open end is a hole you can see through at close range.
    const tip = points[points.length - 1];
    const centre = mesh.vertex(
      add(tip.position, mul(tip.tangent, tip.radius * 0.5)),
      tip.tangent, [0.5 * uScale, (tip.arcLength + phase) / tileMetres], colour, tip.wind);
    const last = rings[rings.length - 1];
    for (let j = 0; j < radial; ++j) mesh.triangle(last[j], last[j + 1], centre);
  }
  if (options.capRoot) {
    const root = points[0];
    const centre = mesh.vertex(
      sub(root.position, mul(root.tangent, root.radius * 0.4)),
      mul(root.tangent, -1), [0.5 * uScale, phase / tileMetres], colour, root.wind);
    const first = rings[0];
    for (let j = 0; j < radial; ++j) mesh.triangle(first[j + 1], first[j], centre);
  }
  mesh.end();
  return { rings: rings.length, vertices: rings.length * stride, uScale };
}

// Radius of the parent tube at an arc position. Used by the collar and by the junction test.
function parentSurfaceRadius(parent, arc) {
  return sampleBranch(parent, arc).radius;
}

// Projects a point onto the parent's cylinder surface, returning both the projected point and
// the outward radial direction.
function projectOntoParent(parent, arc, point, lift) {
  const sample = sampleBranch(parent, arc);
  const relative = sub(point, sample.position);
  const along = dot(relative, sample.tangent);
  const radial = sub(relative, mul(sample.tangent, along));
  const radialLength = Math.hypot(radial[0], radial[1], radial[2]);
  const direction = radialLength < 1e-7 ? sample.normal : mul(radial, 1 / radialLength);
  return {
    position: add(add(sample.position, mul(sample.tangent, along)), mul(direction, sample.radius + lift)),
    normal: direction,
    radius: sample.radius,
    radialLength,
  };
}

// Collar between a parent and one child. Returns statistics so the geometry report can show
// how much of the junction is actually welded to the parent surface.
export function emitJunction(mesh, parent, child, options) {
  const radial = Math.max(4, Math.round(options.radial ?? 6));
  const tileMetres = options.tileMetres ?? 1.6;
  const material = options.materialId;
  const colour = options.color ?? [104, 82, 58, 255];
  const lift = options.liftMetres ?? 0.0015;

  const attachArc = child.attachment.parentArcMetres;
  const parentSample = sampleBranch(parent, attachArc);
  const childDirection = child.points[0].tangent;
  const radialComponent = Math.hypot(
    childDirection[0] - parentSample.tangent[0] * dot(childDirection, parentSample.tangent),
    childDirection[1] - parentSample.tangent[1] * dot(childDirection, parentSample.tangent),
    childDirection[2] - parentSample.tangent[2] * dot(childDirection, parentSample.tangent));
  // Where the child axis leaves the parent cylinder. A child that points almost straight
  // along the parent never leaves it; clamping keeps the collar finite instead of dividing
  // by ~0 and producing a kilometre-long band.
  const sinAngle = Math.max(0.2, radialComponent);
  const collarArc = Math.min(child.length * 0.5, parentSample.radius / sinAngle);

  const childSample = sampleBranch(child, collarArc);
  const binormal = normalize(cross(childSample.tangent, childSample.normal));
  const inner = [], outer = [];
  mesh.begin(material);
  for (let j = 0; j <= radial; ++j) {
    const angle = (j / radial) * TAU;
    const direction = add(mul(childSample.normal, Math.cos(angle)), mul(binormal, Math.sin(angle)));
    const onChild = add(childSample.position, mul(direction, childSample.radius));
    const projected = projectOntoParent(parent, attachArc, onChild, lift);
    const childNormal = normalize(sub(direction, mul(childSample.tangent, 0)));
    inner.push(mesh.vertex(onChild, childNormal,
      [(j / radial) * (TAU * childSample.radius) / tileMetres, collarArc / tileMetres], colour, childSample.wind));
    outer.push(mesh.vertex(projected.position, projected.normal,
      [(j / radial) * (TAU * childSample.radius) / tileMetres, 0], colour, parentSample.wind));
  }
  let emitted = 0, skipped = 0;
  for (let j = 0; j < radial; ++j) {
    // A collar quad is degenerate where the child surface already touches the parent (the
    // "downhill" side). Emitting it would add zero-area triangles that some rasterisers
    // reject as NaN normals; skip them and count them.
    const span = Math.hypot(...sub(
      add(childSample.position, mul(childSample.normal, childSample.radius)),
      projectOntoParent(parent, attachArc, add(childSample.position, mul(childSample.normal, childSample.radius)), lift).position));
    if (span < 1e-4) { ++skipped; continue; }
    mesh.quad(inner[j], inner[j + 1], outer[j + 1], outer[j]);
    ++emitted;
  }
  mesh.end();
  return { collarArc, quads: emitted, degenerateSkipped: skipped, lift };
}

// Measures how far a branch's root ring is from the parent's surface. Zero means the child
// starts inside the parent (no hole); a positive number is the air gap that used to be
// visible at every fork.
export function measureJunction(parent, child) {
  const sample = sampleBranch(parent, child.attachment.parentArcMetres);
  const root = child.points[0];
  const centreGap = Math.hypot(...sub(root.position, sample.position));
  const surfaceClearance = sample.radius - root.radius;
  return { centreGapMetres: centreGap, surfaceClearanceMetres: surfaceClearance, parentRadius: sample.radius, childRadius: root.radius };
}

import { closestPointOnTriangle } from '../character/rig-mesh.mjs';

// Uniform-grid triangle index.
//
// Both the garment fit check and the weight transfer need "nearest body triangle" for
// every garment vertex. At 37k body triangles and a few thousand garment vertices the
// brute-force version takes tens of seconds per authoring action, which makes the
// diagnostic feel broken. A uniform grid brings that to milliseconds and, unlike a BVH,
// its memory is bounded and its build is deterministic.
export class TriangleGrid {
  constructor(positions, indices, { cellSizeMetres = null } = {}) {
    this.positions = positions;
    this.indices = indices;
    this.triangles = indices.length / 3;
    this.cellSize = cellSizeMetres ?? TriangleGrid.suggestCellSize(positions, indices);
    this.cells = new Map();
    this.min = [Infinity, Infinity, Infinity];
    this.max = [-Infinity, -Infinity, -Infinity];
    for (let vertex = 0; vertex < positions.length / 3; ++vertex) {
      for (let axis = 0; axis < 3; ++axis) {
        const value = positions[vertex * 3 + axis];
        if (value < this.min[axis]) this.min[axis] = value;
        if (value > this.max[axis]) this.max[axis] = value;
      }
    }
    this.bounds = { min: this.min, max: this.max };
    for (let triangle = 0; triangle < this.triangles; ++triangle) {
      const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
      const low = [0, 1, 2].map((axis) => Math.min(
        positions[a * 3 + axis], positions[b * 3 + axis], positions[c * 3 + axis]));
      const high = [0, 1, 2].map((axis) => Math.max(
        positions[a * 3 + axis], positions[b * 3 + axis], positions[c * 3 + axis]));
      const x0 = this.cellIndex(low[0]), x1 = this.cellIndex(high[0]);
      const y0 = this.cellIndex(low[1]), y1 = this.cellIndex(high[1]);
      const z0 = this.cellIndex(low[2]), z1 = this.cellIndex(high[2]);
      for (let x = x0; x <= x1; ++x) for (let y = y0; y <= y1; ++y) for (let z = z0; z <= z1; ++z) {
        const key = `${x},${y},${z}`;
        const list = this.cells.get(key);
        if (list) list.push(triangle); else this.cells.set(key, [triangle]);
      }
    }
  }

  static suggestCellSize(positions, indices) {
    // Mean triangle edge length: cells smaller than a triangle would store the same
    // triangle in many cells for no query benefit.
    let total = 0, count = 0;
    for (let triangle = 0; triangle < indices.length / 3; triangle += Math.max(1, Math.floor(indices.length / 3 / 500))) {
      const a = indices[triangle * 3], b = indices[triangle * 3 + 1];
      total += Math.hypot(
        positions[b * 3] - positions[a * 3],
        positions[b * 3 + 1] - positions[a * 3 + 1],
        positions[b * 3 + 2] - positions[a * 3 + 2]);
      ++count;
    }
    const mean = count > 0 ? total / count : 0.02;
    return Math.max(0.005, Math.min(0.2, mean * 1.5));
  }

  cellIndex(value) {
    return Math.floor(value / this.cellSize);
  }

  triangleVertices(triangle) {
    const { positions, indices } = this;
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    return {
      a, b, c,
      pa: [positions[a * 3], positions[a * 3 + 1], positions[a * 3 + 2]],
      pb: [positions[b * 3], positions[b * 3 + 1], positions[b * 3 + 2]],
      pc: [positions[c * 3], positions[c * 3 + 1], positions[c * 3 + 2]],
    };
  }

  // Nearest triangle to a point within `maxDistanceMetres`. Rings of cells expand
  // until the best hit is provably closer than any unvisited cell could be.
  nearest(point, { maxDistanceMetres = Infinity, filter = null } = {}) {
    const base = [0, 1, 2].map((axis) => this.cellIndex(point[axis]));
    let best = { distance: Infinity, triangle: -1, bary: null, point: null, a: -1, b: -1, c: -1 };
    const visited = new Set();
    const maxRing = Math.ceil(maxDistanceMetres / this.cellSize) + 1;
    for (let ring = 0; ring <= maxRing; ++ring) {
      // Once the best hit is closer than the nearest possible point in this ring, stop.
      if (best.distance <= (ring - 1) * this.cellSize) break;
      let found = false;
      for (let dx = -ring; dx <= ring; ++dx) {
        for (let dy = -ring; dy <= ring; ++dy) {
          for (let dz = -ring; dz <= ring; ++dz) {
            // Only the shell of the cube is new at this ring.
            if (Math.max(Math.abs(dx), Math.abs(dy), Math.abs(dz)) !== ring) continue;
            const key = `${base[0] + dx},${base[1] + dy},${base[2] + dz}`;
            if (visited.has(key)) continue;
            visited.add(key);
            const list = this.cells.get(key);
            if (!list) continue;
            found = true;
            for (const triangle of list) {
              const { pa, pb, pc, a, b, c } = this.triangleVertices(triangle);
              const hit = closestPointOnTriangle(point, pa, pb, pc);
              const distance = Math.hypot(hit.point[0] - point[0], hit.point[1] - point[1], hit.point[2] - point[2]);
              if (distance < best.distance && (!filter || !filter({ triangle, a, b, c, distance }))) {
                best = { distance, triangle, bary: hit.bary, point: hit.point, a, b, c };
              }
            }
          }
        }
      }
      if (!found && ring > 0 && best.distance <= ring * this.cellSize) break;
    }
    return best;
  }
}

// Vertex-only proximity grid. Used where "is the body inside the garment here" only
// needs a distance to the nearest garment point, not to a triangle: three orders of
// magnitude cheaper than a triangle query for the same answer at this tolerance.
export class PointGrid {
  constructor(positions, { cellSizeMetres = 0.02 } = {}) {
    this.positions = positions;
    this.cellSize = cellSizeMetres;
    this.cells = new Map();
    for (let vertex = 0; vertex < positions.length / 3; ++vertex) {
      const key = this.keyFor(positions[vertex * 3], positions[vertex * 3 + 1], positions[vertex * 3 + 2]);
      const list = this.cells.get(key);
      if (list) list.push(vertex); else this.cells.set(key, [vertex]);
    }
  }

  keyFor(x, y, z) {
    return `${Math.floor(x / this.cellSize)},${Math.floor(y / this.cellSize)},${Math.floor(z / this.cellSize)}`;
  }

  within(point, maximum, { limit = Infinity } = {}) {
    const cells = Math.ceil(maximum / this.cellSize);
    const reached = [];
    for (let dx = -cells; dx <= cells; ++dx) {
      for (let dy = -cells; dy <= cells; ++dy) {
        for (let dz = -cells; dz <= cells; ++dz) {
          const key = this.keyFor(point[0] + dx * this.cellSize, point[1] + dy * this.cellSize, point[2] + dz * this.cellSize);
          const list = this.cells.get(key);
          if (!list) continue;
          for (const vertex of list) {
            const distance = Math.hypot(
              this.positions[vertex * 3] - point[0],
              this.positions[vertex * 3 + 1] - point[1],
              this.positions[vertex * 3 + 2] - point[2]);
            if (distance <= maximum) {
              reached.push(vertex);
              if (reached.length >= limit) return reached;
            }
          }
        }
      }
    }
    return reached;
  }
}

// Ray casting for an inside/outside test.
//
// Signed distance from the nearest triangle is only correct when that triangle's normal
// agrees with the surface's global orientation. Near a concavity — an armpit, the gap
// between the thighs, the inside of a sleeve — the nearest triangle can face the other
// way and a garment vertex that is plainly outside the body is reported as being 12 cm
// inside it. Parity counting along a ray does not depend on normals.
export function countRayHits(grid, origin, direction, { maxLength = 4 } = {}) {
  const length = Math.hypot(direction[0], direction[1], direction[2]) || 1;
  const unit = [direction[0] / length, direction[1] / length, direction[2] / length];
  const step = grid.cellSize * 0.5;
  const tested = new Set();
  let hits = 0;
  for (let distance = 0; distance <= maxLength; distance += step) {
    const x = origin[0] + unit[0] * distance;
    const y = origin[1] + unit[1] * distance;
    const z = origin[2] + unit[2] * distance;
    const key = `${grid.cellIndex(x)},${grid.cellIndex(y)},${grid.cellIndex(z)}`;
    const list = grid.cells.get(key);
    if (!list) continue;
    for (const triangle of list) {
      if (tested.has(triangle)) continue;
      tested.add(triangle);
      const { pa, pb, pc } = grid.triangleVertices(triangle);
      if (rayHitsTriangle(origin, unit, pa, pb, pc)) ++hits;
    }
  }
  return hits;
}

function rayHitsTriangle(origin, direction, a, b, c) {
  const edge1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const edge2 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
  const h = [
    direction[1] * edge2[2] - direction[2] * edge2[1],
    direction[2] * edge2[0] - direction[0] * edge2[2],
    direction[0] * edge2[1] - direction[1] * edge2[0],
  ];
  const determinant = edge1[0] * h[0] + edge1[1] * h[1] + edge1[2] * h[2];
  if (Math.abs(determinant) < 1e-12) return false;
  const inverse = 1 / determinant;
  const s = [origin[0] - a[0], origin[1] - a[1], origin[2] - a[2]];
  const u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) * inverse;
  if (u < 0 || u > 1) return false;
  const q = [
    s[1] * edge1[2] - s[2] * edge1[1],
    s[2] * edge1[0] - s[0] * edge1[2],
    s[0] * edge1[1] - s[1] * edge1[0],
  ];
  const v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * inverse;
  if (v < 0 || u + v > 1) return false;
  const t = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * inverse;
  return t > 1e-6;
}

// Inside test that does not trust a single triangle's normal: three rays along the
// coordinate axes, majority vote. One ray can graze an open boundary (a mesh with holes,
// a sleeve opening) and count an odd number by accident.
export function isInsideBody(grid, point, bodyBounds = null) {
  const directions = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  const bounds = bodyBounds ?? grid.bounds;
  let votes = 0;
  for (const direction of directions) {
    const length = Math.max(
      Math.abs(bounds.max[0] - point[0]), Math.abs(bounds.max[1] - point[1]),
      Math.abs(bounds.max[2] - point[2])) + 0.5;
    const count = countRayHits(grid, point, direction, { maxLength: length });
    if (count % 2 === 1) ++votes;
  }
  return votes >= 2;
}

// Triangle normal at a hit, from positions. Winding defines the outside, which is what
// makes "inside the body" a signed quantity rather than a guess.
export function hitNormal(positions, a, b, c) {
  const ab = [positions[b * 3] - positions[a * 3], positions[b * 3 + 1] - positions[a * 3 + 1], positions[b * 3 + 2] - positions[a * 3 + 2]];
  const ac = [positions[c * 3] - positions[a * 3], positions[c * 3 + 1] - positions[a * 3 + 1], positions[c * 3 + 2] - positions[a * 3 + 2]];
  const normal = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
  const length = Math.hypot(normal[0], normal[1], normal[2]) || 1;
  return [normal[0] / length, normal[1] / length, normal[2] / length];
}

// Signed distance.
//
// Magnitude comes from the nearest triangle. For the sign there are two signals, and each
// is wrong in a different case:
//
//   * ray parity is robust on a single closed surface, but a body is several parts. From
//     the middle of the torso, a +X ray can leave the torso and then enter and leave the
//     arm — two crossings, "outside". That is not a corner case: on the sample character
//     every axis ray from the torso centre reports an even count, so parity called the
//     centre of the body "outside" and every initial-penetration report came back clean.
//   * the nearest triangle's normal is local and correct when the point is roughly in
//     front of or behind that triangle, and wrong when the point is nearly in the plane of
//     the triangle (the armpit case), where the projection says nothing.
//
// So the normal decides when it is decisive (the point is within 60° of the triangle's
// direction), and parity only breaks the tie when it is not.
export function signedDistanceRobust(point, nearest, grid, positions) {
  if (!nearest || nearest.triangle < 0) return null;
  const normal = hitNormal(positions, nearest.a, nearest.b, nearest.c);
  const toPoint = [
    point[0] - nearest.point[0], point[1] - nearest.point[1], point[2] - nearest.point[2],
  ];
  const along = toPoint[0] * normal[0] + toPoint[1] * normal[1] + toPoint[2] * normal[2];
  const distance = nearest.distance;
  if (distance <= 1e-9) return 0;
  if (Math.abs(along) >= 0.5 * distance) return along >= 0 ? distance : -distance;
  return isInsideBody(grid, point) ? -distance : distance;
}

// Positive distance means the point is on the outside of the surface (along the normal),
// negative means it is inside the body. Kept for callers that have a trusted orientation
// (a closed, outward-wound mesh) and cannot afford a ray cast per vertex.
export function signedDistance(point, nearest, positions) {
  if (nearest.triangle < 0) return null;
  const normal = hitNormal(positions, nearest.a, nearest.b, nearest.c);
  const toPoint = [
    point[0] - nearest.point[0], point[1] - nearest.point[1], point[2] - nearest.point[2],
  ];
  const sign = toPoint[0] * normal[0] + toPoint[1] * normal[1] + toPoint[2] * normal[2] >= 0 ? 1 : -1;
  return sign * nearest.distance;
}

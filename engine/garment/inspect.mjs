import { TriangleGrid, PointGrid, signedDistance, signedDistanceRobust } from './grid.mjs';

// Garment input validation and fit diagnosis.
//
// A garment is a mesh somebody else made. The engine's job before touching weights is
// to say what is wrong with it, in measurements: degenerate triangles, non-manifold
// edges, inverted winding, initial penetration, layers closer than a millimetre, and a
// body that simply does not fit inside it. Shrinking the garment to make the numbers go
// away is exactly the silent quality loss the plan forbids.

export class GarmentError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'GarmentError';
    this.code = code;
    this.details = details;
  }
}

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

// Accepts the shapes exporters actually produce: flat typed arrays, arrays of triples,
// or arrays of {position} objects.
export function parseMesh(input, { label = 'garment', requireSkin = false } = {}) {
  fail(input && typeof input === 'object', 'NG-E-GARMENT-MESH', `${label} must be an object`);
  const rawPositions = input.positions;
  fail(rawPositions, 'NG-E-GARMENT-MESH', `${label}.positions is required`);
  let positions;
  if (ArrayBuffer.isView(rawPositions)) positions = Float64Array.from(rawPositions);
  else if (Array.isArray(rawPositions)) {
    if (rawPositions.length > 0 && Array.isArray(rawPositions[0])) {
      positions = new Float64Array(rawPositions.length * 3);
      rawPositions.forEach((point, index) => {
        fail(point.length === 3 && point.every(Number.isFinite), 'NG-E-GARMENT-MESH',
          `${label}.positions[${index}] must be a 3-vector`);
        positions[index * 3] = point[0]; positions[index * 3 + 1] = point[1]; positions[index * 3 + 2] = point[2];
      });
    } else {
      positions = Float64Array.from(rawPositions);
    }
  } else fail(false, 'NG-E-GARMENT-MESH', `${label}.positions must be an array or typed array`);
  fail(positions.length >= 9 && positions.length % 3 === 0, 'NG-E-GARMENT-MESH',
    `${label}.positions must contain at least 3 points`);
  const vertices = positions.length / 3;
  fail(vertices <= 200000, 'NG-E-GARMENT-MESH', `${label} exceeds 200k vertices`);
  for (let index = 0; index < positions.length; ++index) {
    fail(Number.isFinite(positions[index]) && Math.abs(positions[index]) <= 1e4, 'NG-E-GARMENT-MESH',
      `${label}.positions[${index}] is not a sane coordinate`);
  }

  const rawIndices = input.indices;
  fail(rawIndices, 'NG-E-GARMENT-MESH', `${label}.indices is required`);
  let indices;
  if (ArrayBuffer.isView(rawIndices)) indices = Uint32Array.from(rawIndices);
  else {
    fail(Array.isArray(rawIndices), 'NG-E-GARMENT-MESH', `${label}.indices must be an array`);
    indices = Array.isArray(rawIndices[0])
      ? Uint32Array.from(rawIndices.flat())
      : Uint32Array.from(rawIndices);
  }
  fail(indices.length >= 3 && indices.length % 3 === 0, 'NG-E-GARMENT-MESH',
    `${label}.indices must describe whole triangles`);
  for (const value of indices) {
    fail(value < vertices, 'NG-E-GARMENT-MESH', `${label}.indices references vertex ${value} of ${vertices}`);
  }

  let uvs = null;
  if (input.uvs) {
    uvs = Float32Array.from(input.uvs);
    fail(uvs.length === vertices * 2, 'NG-E-GARMENT-MESH', `${label}.uvs must have two values per vertex`);
  }

  const vertexIds = input.vertexIds
    ? Uint32Array.from(input.vertexIds)
    : Uint32Array.from({ length: vertices }, (_, index) => index);
  fail(vertexIds.length === vertices, 'NG-E-GARMENT-MESH', `${label}.vertexIds must have one id per vertex`);

  let skin = null;
  if (input.skin) {
    fail(Array.isArray(input.skin.joints) && Array.isArray(input.skin.weights), 'NG-E-GARMENT-MESH',
      `${label}.skin needs joints and weights`);
    skin = {
      joints: Uint32Array.from(input.skin.joints),
      weights: Float32Array.from(input.skin.weights),
    };
    fail(skin.joints.length === vertices * 4 && skin.weights.length === vertices * 4, 'NG-E-GARMENT-MESH',
      `${label}.skin must have four influences per vertex`);
  } else if (requireSkin) {
    fail(false, 'NG-E-GARMENT-MESH', `${label} must carry skin weights for this operation`);
  }

  return {
    label, positions, indices, uvs, vertexIds, skin,
    vertices, triangles: indices.length / 3,
    units: input.units ?? 'metres',
  };
}

export function bodyFromCharacter(character, { posePositions = null } = {}) {
  return {
    label: 'body',
    positions: posePositions ? Float64Array.from(posePositions) : Float64Array.from(character.positions),
    indices: Uint32Array.from(character.indices),
    vertices: character.vertices,
    triangles: character.indices.length / 3,
    character,
  };
}

// ------------------------------------------------------------------ topology
export function inspectTopology(mesh) {
  const { positions, indices, triangles } = mesh;
  const edges = new Map();
  const degenerate = [];
  const areas = new Float64Array(triangles);
  let totalArea = 0;
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    if (a === b || b === c || a === c) { degenerate.push({ triangle, reason: 'repeated-vertex' }); continue; }
    const ab = [positions[b * 3] - positions[a * 3], positions[b * 3 + 1] - positions[a * 3 + 1], positions[b * 3 + 2] - positions[a * 3 + 2]];
    const ac = [positions[c * 3] - positions[a * 3], positions[c * 3 + 1] - positions[a * 3 + 1], positions[c * 3 + 2] - positions[a * 3 + 2]];
    const cross = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
    const area = Math.hypot(cross[0], cross[1], cross[2]) * 0.5;
    areas[triangle] = area;
    totalArea += area;
    if (area < 1e-10) degenerate.push({ triangle, reason: 'zero-area' });
    for (let corner = 0; corner < 3; ++corner) {
      const from = indices[triangle * 3 + corner];
      const to = indices[triangle * 3 + (corner + 1) % 3];
      const key = from < to ? `${from},${to}` : `${to},${from}`;
      const record = edges.get(key) ?? { count: 0, orientations: [] };
      record.count += 1;
      record.orientations.push(from < to ? 1 : -1);
      edges.set(key, record);
    }
  }
  let boundary = 0, nonManifold = 0, inconsistent = 0;
  for (const record of edges.values()) {
    if (record.count === 1) { ++boundary; continue; }
    if (record.count > 2) { ++nonManifold; continue; }
    // Two triangles sharing an edge must traverse it in opposite directions. Same
    // direction means one of them is wound backwards, which makes the normal — and
    // therefore "inside the body" — wrong for that face.
    if (record.orientations[0] === record.orientations[1]) ++inconsistent;
  }

  // Connected components, to report a garment that is several disconnected shells and
  // to know whether a "piece" is actually attached.
  const adjacency = Array.from({ length: mesh.vertices }, () => []);
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    adjacency[a].push(b, c); adjacency[b].push(a, c); adjacency[c].push(a, b);
  }
  const component = new Int32Array(mesh.vertices).fill(-1);
  let components = 0;
  const componentSizes = [];
  for (let vertex = 0; vertex < mesh.vertices; ++vertex) {
    if (component[vertex] >= 0) continue;
    const stack = [vertex];
    component[vertex] = components;
    let size = 0;
    while (stack.length) {
      const current = stack.pop();
      ++size;
      for (const next of adjacency[current]) {
        if (component[next] >= 0) continue;
        component[next] = components;
        stack.push(next);
      }
    }
    componentSizes.push(size);
    ++components;
  }

  // Signed volume: a closed, outward-wound mesh has positive volume. A negative value
  // is the signature of an inside-out import or a mirror with a negative scale, and it
  // silently breaks every collision test downstream.
  let volume = 0;
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const a = indices[triangle * 3], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
    const ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2];
    const bx = positions[b * 3], by = positions[b * 3 + 1], bz = positions[b * 3 + 2];
    const cx = positions[c * 3], cy = positions[c * 3 + 1], cz = positions[c * 3 + 2];
    volume += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6;
  }

  return {
    triangles, edges: edges.size, boundaryEdges: boundary, nonManifoldEdges: nonManifold,
    inconsistentWindingEdges: inconsistent, degenerate,
    areas, totalArea, components, componentSizes,
    closed: boundary === 0 && nonManifold === 0,
    signedVolumeM3: volume,
    winding: volume >= 0 ? 'outward' : 'inward',
  };
}

// ------------------------------------------------------------------- fit
export function inspectFit(garment, body, {
  penetrationToleranceMetres = 0, thinLayerMetres = 0.0008, bodyGrid = null,
} = {}) {
  const grid = bodyGrid ?? new TriangleGrid(body.positions, body.indices);
  const penetrations = [];
  const clearances = [];
  const perVertex = new Array(garment.vertices);
  let outside = 0;
  for (let vertex = 0; vertex < garment.vertices; ++vertex) {
    const point = [garment.positions[vertex * 3], garment.positions[vertex * 3 + 1], garment.positions[vertex * 3 + 2]];
    const nearest = grid.nearest(point, { maxDistanceMetres: 0.12 });
    const distance = signedDistanceRobust(point, nearest, grid, body.positions);
    perVertex[vertex] = distance;
    if (distance === null) continue;
    if (distance < 0) { penetrations.push(-distance); outside += 0; } else { ++outside; clearances.push(distance); }
  }
  penetrations.sort((a, b) => a - b);
  clearances.sort((a, b) => a - b);
  const percentile = (list, fraction) => (list.length === 0 ? 0
    : list[Math.min(list.length - 1, Math.floor(list.length * fraction))]);
  const thin = clearances.filter((value) => value < thinLayerMetres).length;
  return {
    grid,
    vertices: garment.vertices,
    outsideVertices: outside,
    penetratingVertices: penetrations.length,
    penetration: {
      p50Metres: percentile(penetrations, 0.5),
      p95Metres: percentile(penetrations, 0.95),
      p99Metres: percentile(penetrations, 0.99),
      maximumMetres: penetrations.length ? penetrations[penetrations.length - 1] : 0,
      exceedsTolerance: penetrations.length > 0 && percentile(penetrations, 0.99) > penetrationToleranceMetres,
    },
    clearance: {
      minimumMetres: clearances.length ? clearances[0] : 0,
      p05Metres: percentile(clearances, 0.05),
      thinLayerVertices: thin,
    },
    perVertex,
  };
}

// Body-versus-garment proportions, measured only where the garment actually covers the
// body. Comparing whole-body radius per height slice reports a shirt as "too tight"
// because the arms are outside it — which is true of every shirt ever made. Coverage is
// decided by proximity, not by guessing which limb a vertex belongs to.
export function inspectProportions(garment, body, { buckets = 12, coverageMetres = 0.12 } = {}) {
  const bounds = (positions) => {
    const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
    for (let index = 0; index < positions.length; index += 3) {
      for (let axis = 0; axis < 3; ++axis) {
        min[axis] = Math.min(min[axis], positions[index + axis]);
        max[axis] = Math.max(max[axis], positions[index + axis]);
      }
    }
    return { min, max };
  };
  const garmentBounds = bounds(garment.positions), bodyBounds = bounds(body.positions);
  // Proximity, not topology: a vertex is "covered" when the garment has geometry within
  // a few centimetres of it. A point grid answers that in microseconds per vertex; a
  // triangle query would be the wrong tool for a yes/no question.
  const garmentPoints = new PointGrid(garment.positions, { cellSizeMetres: Math.max(0.01, coverageMetres / 3) });
  const cxCenter = (garmentBounds.min[0] + garmentBounds.max[0]) / 2;
  const czCenter = (garmentBounds.min[2] + garmentBounds.max[2]) / 2;
  const covered = [];
  for (let vertex = 0; vertex < body.vertices; ++vertex) {
    const point = [body.positions[vertex * 3], body.positions[vertex * 3 + 1], body.positions[vertex * 3 + 2]];
    if (point[0] < garmentBounds.min[0] - coverageMetres || point[0] > garmentBounds.max[0] + coverageMetres) continue;
    if (point[1] < garmentBounds.min[1] - coverageMetres || point[1] > garmentBounds.max[1] + coverageMetres) continue;
    if (point[2] < garmentBounds.min[2] - coverageMetres || point[2] > garmentBounds.max[2] + coverageMetres) continue;
    if (garmentPoints.within(point, coverageMetres, { limit: 1 }).length > 0) covered.push(vertex);
  }

  // Angular span of the garment around the body axis. A back cape covers 130°, and body
  // vertices outside that span (the arms, the chest) are not supposed to be inside it —
  // comparing their radius against the cape's reports every cape as a fit failure.
  let spanMin = 180, spanMax = -180;
  {
    const centreX = (garmentBounds.min[0] + garmentBounds.max[0]) / 2;
    const centreZ = (garmentBounds.min[2] + garmentBounds.max[2]) / 2;
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      const angle = Math.atan2(garment.positions[vertex * 3] - centreX, garment.positions[vertex * 3 + 2] - centreZ);
      const degrees = (angle * 180) / Math.PI;
      spanMin = Math.min(spanMin, degrees);
      spanMax = Math.max(spanMax, degrees);
    }
  }
  const insideSpan = (angleDegrees) => angleDegrees >= spanMin - 1 && angleDegrees <= spanMax + 1;

  const low = garmentBounds.min[1], high = garmentBounds.max[1];
  const height = high - low;
  const slices = [];
  for (let bucket = 0; bucket < buckets; ++bucket) {
    const sliceLow = low + (height * bucket) / buckets;
    const sliceHigh = low + (height * (bucket + 1)) / buckets;
    const bodyPoints = [], garmentPoints = [];
    for (const vertex of covered) {
      const y = body.positions[vertex * 3 + 1];
      if (y < sliceLow || y > sliceHigh) continue;
      const angle = (Math.atan2(body.positions[vertex * 3] - cxCenter, body.positions[vertex * 3 + 2] - czCenter) * 180) / Math.PI;
      if (!insideSpan(angle)) continue;
      bodyPoints.push(vertex);
    }
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      const y = garment.positions[vertex * 3 + 1];
      if (y < sliceLow || y > sliceHigh) continue;
      garmentPoints.push(vertex);
    }
    // Both radii are measured from the same axis: the centre of the body inside this
    // slice. Using the mesh bounds centre would make a sleeve look like it is 20 cm
    // away from the torso.
    let cx = 0, cz = 0;
    for (const vertex of bodyPoints) { cx += body.positions[vertex * 3]; cz += body.positions[vertex * 3 + 2]; }
    cx = bodyPoints.length ? cx / bodyPoints.length : (bodyBounds.min[0] + bodyBounds.max[0]) / 2;
    cz = bodyPoints.length ? cz / bodyPoints.length : (bodyBounds.min[2] + bodyBounds.max[2]) / 2;
    let bodyRadius = 0;
    for (const vertex of bodyPoints) {
      bodyRadius = Math.max(bodyRadius, Math.hypot(body.positions[vertex * 3] - cx, body.positions[vertex * 3 + 2] - cz));
    }
    let garmentRadius = 0;
    for (const vertex of garmentPoints) {
      garmentRadius = Math.max(garmentRadius, Math.hypot(garment.positions[vertex * 3] - cx, garment.positions[vertex * 3 + 2] - cz));
    }
    slices.push({
      lowY: sliceLow, highY: sliceHigh,
      coveredBodyVertices: bodyPoints.length,
      garmentVertices: garmentPoints.length,
      bodyRadiusMetres: bodyRadius,
      garmentRadiusMetres: garmentRadius,
      // Negative means the body sticks out of the garment at this height.
      slackMetres: garmentRadius - bodyRadius,
    });
  }
  const measured = slices.filter((slice) => slice.coveredBodyVertices > 0 && slice.garmentVertices > 0);
  return {
    garmentBounds, bodyBounds,
    heightMetres: height,
    coveredBodyVertices: covered.length,
    // Radii are reported for diagnosis, not used as a pass/fail: a comparison of maximum
    // radii says a back cape "fails" because the character's hands are wider than the
    // cape, which is true and irrelevant. The failure that matters is a body part
    // *inside* the garment, and that is measured by penetration per slice in
    // `tightenSlices`, below.
    slices, measuredSlices: measured.length,
    angularSpanDegrees: [spanMin, spanMax],
    tightSlices: 0,
    worstSlackMetres: 0,
    verdict: 'radial comparison only; penetration decides fit',
  };
}

// Marks the height slices where garment vertices are actually inside the body. This is the
// actionable form of "the body does not fit the garment".
export function tightenSlices(proportions, garment, perVertexSignedDistance, { toleranceMetres = 0.002 } = {}) {
  const slices = proportions.slices.map((slice) => {
    let penetrating = 0, worst = 0;
    for (let vertex = 0; vertex < garment.vertices; ++vertex) {
      const y = garment.positions[vertex * 3 + 1];
      if (y < slice.lowY || y > slice.highY) continue;
      const distance = perVertexSignedDistance[vertex];
      if (distance === null || distance === undefined) continue;
      if (distance < -toleranceMetres) {
        ++penetrating;
        worst = Math.max(worst, -distance);
      }
    }
    return { ...slice, penetratingVertices: penetrating, worstPenetrationMetres: worst };
  });
  const tight = slices.filter((slice) => slice.penetratingVertices > 0);
  return {
    ...proportions,
    slices,
    tightSlices: tight.length,
    worstSlackMetres: tight.length ? -Math.max(...tight.map((slice) => slice.worstPenetrationMetres)) : 0,
    verdict: tight.length === 0
      ? 'no garment vertex is inside the body in any measured height slice'
      : `garment vertices are inside the body in ${tight.length} of ${proportions.measuredSlices} slices; `
        + 'fix the fit before transferring weights',
  };
}

export function inspectGarment({
  garment: garmentInput, body, character = null, thresholds = {},
} = {}) {
  const garment = parseMesh(garmentInput, { label: 'garment', requireSkin: false });
  const bodyMesh = body ?? (character ? bodyFromCharacter(character) : null);
  fail(bodyMesh, 'NG-E-GARMENT-MESH', 'a body mesh or a character is required to inspect a garment');
  const topology = inspectTopology(garment);
  const bodyTopology = inspectTopology({ ...bodyMesh, areas: new Float64Array(bodyMesh.triangles) });
  const fit = inspectFit(garment, bodyMesh, {
    penetrationToleranceMetres: thresholds.penetrationToleranceMetres ?? 0,
    thinLayerMetres: thresholds.thinLayerMetres ?? 0.0008,
  });
  const proportions = tightenSlices(
    inspectProportions(garment, bodyMesh, { buckets: thresholds.buckets ?? 12 }),
    garment, fit.perVertex,
    { toleranceMetres: thresholds.penetrationToleranceMetres ?? 0.002 });

  const diagnostics = [];
  const add = (code, level, message, details = []) => diagnostics.push({ code, level, message, details });
  if (topology.degenerate.length) {
    add('NG-E-GARMENT-DEGENERATE', 'error',
      `${topology.degenerate.length} degenerate triangles cannot carry a rest shape`, topology.degenerate.slice(0, 8));
  }
  if (topology.nonManifoldEdges > 0) {
    add('NG-E-GARMENT-NONMANIFOLD', 'error',
      `${topology.nonManifoldEdges} edges are shared by more than two triangles; the surface has no well-defined inside`);
  }
  if (topology.inconsistentWindingEdges > 0) {
    add('NG-E-GARMENT-WINDING', 'error',
      `${topology.inconsistentWindingEdges} shared edges are traversed in the same direction by both triangles`);
  }
  if (topology.signedVolumeM3 < 0) {
    add('NG-E-GARMENT-MIRROR', 'error',
      'the mesh has negative signed volume: it is inside-out or was mirrored with a negative scale');
  }
  if (topology.components > 1) {
    add('NG-E-GARMENT-SHELLS', 'warning',
      `the garment is ${topology.components} disconnected shells (${topology.componentSizes.join(', ')} vertices)`);
  }
  if (fit.penetration.p99Metres > (thresholds.penetrationToleranceMetres ?? 0)) {
    add('NG-E-GARMENT-PENETRATION', 'error',
      `${fit.penetratingVertices} vertices start inside the body; P99 ${(fit.penetration.p99Metres * 1000).toFixed(2)} mm, `
      + `maximum ${(fit.penetration.maximumMetres * 1000).toFixed(2)} mm`,
      { p95: fit.penetration.p95Metres, max: fit.penetration.maximumMetres });
  }
  if (fit.clearance.thinLayerVertices > 0) {
    add('NG-E-GARMENT-THIN-LAYER', 'warning',
      `${fit.clearance.thinLayerVertices} vertices are closer than `
      + `${((thresholds.thinLayerMetres ?? 0.0008) * 1000).toFixed(2)} mm to the body, which will z-fight or pinch`);
  }
  if (proportions.tightSlices > 0) {
    add('NG-E-GARMENT-FIT', 'error',
      `${proportions.verdict} (worst ${(proportions.worstSlackMetres * 1000).toFixed(1)} mm)`,
      proportions.slices.filter((slice) => slice.slackMetres < 0));
  }

  const errors = diagnostics.filter((entry) => entry.level === 'error');
  return {
    schema: 'nativegpu.garment-inspection/1',
    garment: {
      vertices: garment.vertices, triangles: garment.triangles,
      bounds: proportions.garmentBounds, units: garment.units,
    },
    body: {
      vertices: bodyMesh.vertices, triangles: bodyMesh.triangles,
      bounds: proportions.bodyBounds, topology: {
        closed: bodyTopology.closed, winding: bodyTopology.winding,
      },
    },
    topology: {
      edges: topology.edges, boundaryEdges: topology.boundaryEdges,
      nonManifoldEdges: topology.nonManifoldEdges,
      inconsistentWindingEdges: topology.inconsistentWindingEdges,
      degenerateTriangles: topology.degenerate.length,
      components: topology.components, componentSizes: topology.componentSizes,
      closed: topology.closed, winding: topology.winding,
      signedVolumeM3: topology.signedVolumeM3, totalAreaM2: topology.totalArea,
    },
    fit: {
      outsideVertices: fit.outsideVertices,
      penetratingVertices: fit.penetratingVertices,
      penetration: fit.penetration,
      clearance: fit.clearance,
    },
    proportions: { heightMetres: proportions.heightMetres, slices: proportions.slices, verdict: proportions.verdict },
    diagnostics,
    valid: errors.length === 0,
    verdict: errors.length === 0
      ? 'geometry is ready for weight transfer'
      : 'geometry problems must be fixed before weights; body mismatch is not a weight problem',
    // The per-vertex signed distance is what a weights check reuses so the two agree.
    perVertexSignedDistance: fit.perVertex,
  };
}

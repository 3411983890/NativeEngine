// Leaf placement.
//
// The previous placement picked a point along a segment and then moved the card *centre* by a
// random 5–22 cm along its normal. Nothing connected that offset to the wood, so leaves floated
// next to branches and no amount of texture work could fix it (G04).
//
// Here every leaf is an attachment with a petiole:
//
//   host arc position → petiole base (on the bark) → petiole of a real length →
//   pivot (the blade's origin) → blade
//
// The pivot is the local origin of the blade, and it is the point that the atlas pivot UV
// corresponds to, so the leaf can never be "centred" away from its stalk. The blade itself is
// a small surface — 3 spans x 2 columns with a folded midrib — with normals derived from the
// actual geometry rather than one constant vector for all four corners.

import { add, arcLengthOf, cross, dot, lerp3, mul, normalize, sampleBranch, selectHostBranches, sub } from './branch-graph.mjs';
import { cellForIndex } from './atlas.mjs';
import { Stream, deriveSeed } from './random.mjs';

const TAU = Math.PI * 2;

// Leaves per metre of host branch. `cardsPerSegment` is kept as the authoring knob but is now
// interpreted as a leaf-density target along the branch, because "per segment" made the count
// depend on how many steps a branch happened to have.
const MAX_LEAVES_PER_BRANCH = 320;
// Shoots are the placement unit. A cap per host keeps a long limb from turning into a solid
// brush even if the density knob is raised; the number is high enough for an authored oak.
const MAX_SHOOTS_PER_BRANCH = 160;

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-LEAF';
  throw error;
}

function rotateAround(v, k, angle) {
  const c = Math.cos(angle), s = Math.sin(angle);
  return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1 - c)));
}

// Width profile of a leaf blade, normalised to [0,1]. Widest a little before the middle and
// zero at both ends: the base is where the petiole meets it, the tip is a point.
function bladeWidth(t) {
  if (t <= 0 || t >= 1) return 0;
  return Math.pow(Math.sin(Math.PI * Math.pow(t, 0.85)), 0.9);
}

// Fraction of the (width × length) rectangle a blade of this profile actually covers. Measured
// from the same function the geometry uses, so a leaf-area budget is not a second approximation.
export function bladeAreaFraction(samples = 64) {
  let total = 0;
  for (let index = 0; index < samples; ++index) total += bladeWidth((index + 0.5) / samples);
  return total / samples;
}
const BLADE_AREA_FRACTION = bladeAreaFraction();

// One shoot: the current year's short lateral axis that carries a run of leaves. Leaves used to be
// painted directly onto limbs at a constant arc spacing, which is what produced the "feather" /
// "brush" look: every leaf sat on the same line, evenly, with no bud, internode or cluster. A
// shoot gives the placement a hierarchy (host → shoot → internode → leaf) and an identity that
// survives LOD reduction (a coarser level drops whole shoots, not every third leaf).
function shootAxis(sample, radial, frond, exposure, rng) {
  // Shoots leave the host at an authored angle from its tangent, then reach for the light. The
  // upward term is what separates a young shoot from a drooping spray; `droop` still applies later
  // along the shoot's own length.
  const angle = frond.shootAngle + rng.span(-0.18, 0.18);
  const along = add(mul(sample.tangent, Math.cos(angle)), mul(radial, Math.sin(angle)));
  const direction = normalize(add(add(along, [0, 0.22, 0]), [0, -0.08 * (1 - exposure), 0]));
  // Shaded shoots are shorter: an inner bud does not extend as far as an outer one. Length is a
  // real metre value, so the leaves on a weak shoot are closer together and the cluster is smaller.
  const length = Math.max(0.01, frond.shootLengthMetres * rng.span(0.78, 1.22) * (0.55 + 0.45 * exposure));
  return { direction, length };
}

// Pivots are kept on a coarse world grid so leaves from different shoots cannot interpenetrate
// wholesale. Leaves belonging to the *same* shoot are exempt: they are already separated by the
// internode spacing along that shoot, and rejecting them would turn every cluster into one leaf.
function makeSpacingGrid(spacingMetres) {
  const grid = new Map();
  const key = (p) => `${Math.floor(p[0] / spacingMetres)},${Math.floor(p[1] / spacingMetres)},${Math.floor(p[2] / spacingMetres)}`;
  return {
    rejects(p, shootId) {
      const x = Math.floor(p[0] / spacingMetres), y = Math.floor(p[1] / spacingMetres), z = Math.floor(p[2] / spacingMetres);
      for (let dx = -1; dx <= 1; ++dx) for (let dy = -1; dy <= 1; ++dy) for (let dz = -1; dz <= 1; ++dz) {
        const bucket = grid.get(`${x + dx},${y + dy},${z + dz}`);
        if (!bucket) continue;
        for (const entry of bucket) {
          if (entry.shootId === shootId) continue;
          const dxp = entry.p[0] - p[0], dyp = entry.p[1] - p[1], dzp = entry.p[2] - p[2];
          if (dxp * dxp + dyp * dyp + dzp * dzp < spacingMetres * spacingMetres) return true;
        }
      }
      return false;
    },
    insert(p, shootId) {
      const bucketKey = key(p);
      const bucket = grid.get(bucketKey);
      if (bucket) bucket.push({ p, shootId }); else grid.set(bucketKey, [{ p, shootId }]);
    },
  };
}

export function placeLeaves({ graph, fronds, atlas, spec, quality, seed }) {
  const leaves = [];
  const perFrond = [];
  // Cross-shoot spacing is derived from the species' leaf size unless authored, and it is kept
  // *per frond*: a global grid made removing one frond change which leaves another frond rejected,
  // so a local edit reshuffled untouched foliage. Cell size equals the spacing, so a 3×3×3
  // neighbourhood covers every possible violation.
  const spacingFor = (frond) => Math.max(0.01, Math.min(0.35,
    frond.minLeafSpacingMetres || Math.min(frond.cardWidth, frond.cardHeight) * 0.35));
  const spacingMetres = Math.max(0.01, Math.min(...fronds.filter((frond) => frond.cardsPerSegment > 0).map(spacingFor), 0.35));

  for (const frond of fronds) {
    if (frond.cardsPerSegment <= 0) {
      perFrond.push({ id: frond.id, leaves: 0, hosts: 0, shoots: 0, emptyShoots: 0, leafAreaMetresSquared: 0, spacingRejected: 0 });
      continue;
    }
    const spacing = spacingFor(frond);
    const grid = makeSpacingGrid(spacing);
    const levels = new Set(frond.hostLevels ?? [frond.parentLevel]);
    // Exact level match. `>=` (the old behaviour) attached foliage to every deeper level as
    // well, so "frond on level 3" and "frond on level 4" produced the same tree.
    const hosts = graph.branches.filter((branch) => levels.has(branch.level));
    let placed = 0, shootsPlaced = 0, emptyShoots = 0, rejected = 0;
    let leafArea = 0;
    for (const host of hosts) {
      const rng = new Stream(deriveSeed(seed, `leaves/${frond.id}/${host.id}`), 'leaves');
      const hostArc = Math.max(0.05, arcLengthOf(host));
      // Density is stated as leaves per metre; the shoot count is derived from it and the leaves a
      // shoot carries. That keeps the authored number meaningful while changing the *unit* from
      // "evenly spaced leaf" to "cluster of leaves with a gap between clusters".
      const wantedLeaves = Math.min(MAX_LEAVES_PER_BRANCH, frond.cardsPerSegment * hostArc);
      const shootCount = Math.max(0, Math.min(MAX_SHOOTS_PER_BRANCH, Math.round(wantedLeaves / frond.leavesPerShoot)));
      const phase = rng.span(0, TAU);
      let hostLeaves = 0;
      for (let shootIndex = 0; shootIndex < shootCount && hostLeaves < MAX_LEAVES_PER_BRANCH; ++shootIndex) {
        const keepValue = rng.unit();
        const empty = rng.unit() < frond.emptyShootFraction;
        if (empty) { ++emptyShoots; continue; }
        // Shoots sit on an even arc grid with a bounded jitter: real internodes are spaced, not
        // Poisson-clustered, and a stable seed still gives each branch its own rhythm.
        const along = clamp((shootIndex + 0.5) / shootCount + rng.span(-0.22, 0.22) / Math.max(1, shootCount), 0.02, 0.995);
        const arc = hostArc * along;
        const sample = sampleBranch(host, arc);
        const azimuth = phase + shootIndex * frond.divergence + rng.span(-0.3, 0.3);
        const binormal = normalize(cross(sample.tangent, sample.normal));
        const radial = normalize(add(mul(sample.normal, Math.cos(azimuth)), mul(binormal, Math.sin(azimuth))));
        const base = add(sample.position, mul(radial, sample.radius * 0.92));
        // Light exposure from where this shoot sits in the crown, not from a per-leaf coin flip.
        // `innerDensity` is the floor: shaded inner shoots keep some foliage, they do not become bare.
        const shapeCrown = Math.max(0.01, graph.shapeCrownRadiusMetres ?? graph.crownRadiusMetres ?? 1);
        const radialFraction = clamp(Math.hypot(base[0], base[2]) / shapeCrown, 0, 1);
        const exposure = clamp(frond.innerDensity + (1 - frond.innerDensity) * Math.pow(radialFraction, 1.5), 0.1, 1);
        const axis = shootAxis(sample, radial, frond, exposure, rng);
        // Leaves per shoot scale with exposure. The fractional part is a deterministic coin so the
        // counts average out without ever drawing from a global stream.
        const wanted = frond.leavesPerShoot * exposure;
        let count = Math.floor(wanted);
        if (rng.unit() < wanted - count) ++count;
        count = Math.min(frond.leavesPerShoot, count);
        const shootId = `${frond.id}/${host.id}/s${shootIndex}`;
        let placedThisShoot = 0;
        for (let leafIndex = 0; leafIndex < count; ++leafIndex) {
          // Internode position along the shoot. Half-offset so the first leaf is not exactly at
          // the bud and the last is not exactly at the tip.
          const t = count === 1 ? 0.62 : (leafIndex + 0.5) / count;
          const attach = add(base, mul(axis.direction, axis.length * t));
          // Phyllotaxis around the shoot axis; each leaf gets its own radial direction.
          const leafAzimuth = phase + leafIndex * frond.divergence + rng.span(-0.2, 0.2);
          const leafBinormal = normalize(cross(axis.direction, sample.normal));
          const leafRadial = normalize(add(
            mul(sample.normal, Math.cos(leafAzimuth)),
            mul(leafBinormal, Math.sin(leafAzimuth))));
          const spread = frond.petioleAngle + rng.span(-0.18, 0.18);
          const petioleDirection = normalize(add(
            mul(leafRadial, Math.sin(spread)),
            mul(axis.direction, Math.cos(spread) * 0.85)));
          const petioleLength = frond.petioleLengthMetres * rng.span(0.8, 1.25);
          const pivot = add(attach, mul(petioleDirection, petioleLength));
          if (grid.rejects(pivot, shootId)) { ++rejected; continue; }

          const outward = normalize(sub(leafRadial, mul(axis.direction, dot(leafRadial, axis.direction))));
          const bladeDirection = normalize(add(mul(petioleDirection, 0.55), add(mul(outward, 0.45), [0, -0.18, 0])));
          const up = [0, 1, 0];
          let bladeNormal = normalize(sub(up, mul(bladeDirection, dot(up, bladeDirection))));
          if (!Number.isFinite(bladeNormal[0]) || Math.hypot(...bladeNormal) < 0.1) {
            bladeNormal = normalize(cross(bladeDirection, [1, 0, 0]));
          }
          const roll = rng.span(-0.5, 0.5);
          bladeNormal = normalize(rotateAround(bladeNormal, bladeDirection, roll));
          const bladeSide = normalize(cross(bladeNormal, bladeDirection));

          const scale = rng.span(frond.scale[0], frond.scale[1]);
          // Cell identity follows the leaf's stable id, not the global index: a local edit or a
          // removed shoot does not re-roll the atlas cell of every later leaf.
          const cell = cellForIndex(atlas, hashLeafId(`${shootId}/${leafIndex}`));
          const droop = rng.span(frond.droop[0], frond.droop[1]);
          grid.insert(pivot, shootId);
          const lengthMetres = frond.cardHeight * scale;
          const widthMetres = frond.cardWidth * scale;
          leafArea += widthMetres * lengthMetres * BLADE_AREA_FRACTION;
          leaves.push({
            id: `${shootId}/${leafIndex}`,
            frondId: frond.id,
            hostBranchId: host.id,
            hostLevel: host.level,
            hostArcFraction: along,
            hostArcMetres: arc,
            hostRadiusMetres: sample.radius,
            shoot: {
              id: shootId,
              index: shootIndex,
              base,
              direction: axis.direction,
              lengthMetres: axis.length,
              arcMetres: arc,
              exposure,
              leafIndex,
              leaves: count,
              // Stable survival rank. LOD selection happens *after* placement so a coarser level
              // only removes whole shoots and cannot reroll the spacing grid.
              keep: keepValue,
            },
            petiole: { base: attach, end: pivot, lengthMetres: petioleLength, direction: petioleDirection },
            pivot,
            blade: {
              direction: bladeDirection,
              normal: bladeNormal,
              side: bladeSide,
              lengthMetres,
              widthMetres,
              droop,
              curl: frond.bladeCurl,
              segments: frond.bladeSegments,
            },
            cell: cell ? cell.index : null,
            pivotUV: cell ? [cell.pivotU, cell.pivotV] : [0.5, 0],
            rectUV: cell ? [cell.u0, cell.v0, cell.u1, cell.v1] : [0, 0, 1, 1],
            wind: frond.wind * (0.65 + 0.35 * along),
            variationKey: rng.next(),
            leafAreaMetresSquared: widthMetres * lengthMetres * BLADE_AREA_FRACTION,
          });
          ++placed; ++hostLeaves; ++placedThisShoot;
        }
        if (placedThisShoot > 0) ++shootsPlaced;
      }
    }
    perFrond.push({
      id: frond.id,
      leaves: placed,
      hosts: hosts.length,
      shoots: shootsPlaced,
      emptyShoots,
      spacingRejected: rejected,
      leafAreaMetresSquared: Number(leafArea.toFixed(4)),
      minLeafSpacingMetres: spacing,
    });
  }
  // `quality.leaves` selects whole shoots after placement. Doing it here (instead of skipping
  // shoots while placing) is what makes a coarse LOD a strict subset: the spacing grid saw the full
  // set, so a kept leaf sits exactly where it does at LOD0 and no rejected leaf can reappear.
  const selected = quality && quality.leaves < 1
    ? leaves.filter((leaf) => leaf.shoot.keep < quality.leaves)
    : leaves;
  if (selected !== leaves) {
    const counts = new Map();
    for (const leaf of selected) {
      const entry = counts.get(leaf.frondId) ?? { leaves: 0, area: 0, shoots: new Set() };
      entry.leaves += 1;
      entry.area += leaf.leafAreaMetresSquared;
      entry.shoots.add(leaf.shoot.id);
      counts.set(leaf.frondId, entry);
    }
    for (const entry of perFrond) {
      const count = counts.get(entry.id);
      entry.leaves = count ? count.leaves : 0;
      entry.shoots = count ? count.shoots.size : 0;
      entry.leafAreaMetresSquared = count ? Number(count.area.toFixed(4)) : 0;
    }
  }
  return {
    leaves: selected,
    perFrond,
    // Coverage summary: leaf area on one side plus the crown box it occupies, so "the crown is
    // full" is a measurement instead of an impression. The crown is approximated as an ellipsoid
    // with the graph's measured radial reach and leaf-height extent.
    leafAreaMetresSquared: Number(perFrond.reduce((sum, entry) => sum + (entry.leafAreaMetresSquared ?? 0), 0).toFixed(4)),
    fullLeafCount: leaves.length,
    spacingMetres,
  };
}

// Stable 32-bit string hash for atlas cell choice. Only needs to be deterministic and well mixed;
// it is not a security primitive.
function hashLeafId(id) {
  let hash = 2166136261;
  for (let index = 0; index < id.length; ++index) {
    hash ^= id.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

// Emits one leaf blade into a mesh builder. The blade is a 3-span surface with a folded
// midrib; its normals come from finite differences of the actual surface, so a bent leaf
// shades as a bent leaf.
//
// Rows where the width profile reaches zero (the base and the tip) collapse to a single vertex
// and are connected with a fan. Emitting three coincident vertices there instead is how a leaf
// acquires four zero-area triangles: they rasterise to nothing, but they inflate the triangle
// count, break tangent generation and make every "degenerate triangles" audit red.
export function emitLeafBlade(mesh, leaf, options) {
  const material = options.materialId;
  const colour = options.color ?? [86, 132, 52, 255];
  const cell = options.cell ?? null;
  const spans = Math.max(1, leaf.blade.segments);
  const rows = spans + 1;
  const { direction, normal, side, lengthMetres, widthMetres, droop, curl } = leaf.blade;

  const point = (t, column) => {
    // column: -1 left, 0 midrib, +1 right.
    const width = bladeWidth(t) * widthMetres * 0.5;
    const along = add(leaf.pivot, mul(direction, lengthMetres * t));
    // Droop bends the blade toward the ground; curl folds it around the midrib. Both are
    // geometric, so the silhouette and the normals agree with them.
    const bend = mul(normal, -droop * lengthMetres * t * t);
    const fold = mul(normal, -curl * width * (1 - Math.abs(column)) * 0.9);
    return add(add(along, bend), add(mul(side, width * column), fold));
  };

  // The blade maps across the cell's *content* rect, not its full rect. A canonical leaf has a
  // transparent safe margin; mapping that margin onto the blade would shrink the visible leaf and
  // make the geometry cover transparent texels. A legacy cell has no content rect and keeps the
  // old full-cell mapping byte for byte.
  const uStart = cell ? (cell.contentU0 ?? cell.u0) : 0;
  const uEnd = cell ? (cell.contentU1 ?? cell.u1) : 1;
  const vBase = cell ? cell.pivotV : 0;
  const vTip = cell ? (cell.contentV1 ?? cell.v1) : 1;
  const uvAt = (column, t) => [uStart + (uEnd - uStart) * ((column + 1) / 2), vBase + (vTip - vBase) * t];
  const delta = 1e-3;
  const normalAt = (t, column) => {
    const along = sub(point(Math.min(1, t + delta), column), point(Math.max(0, t - delta), column));
    const across = sub(point(t, Math.min(1, column + 0.25)), point(t, Math.max(-1, column - 0.25)));
    let surface = normalize(cross(across, along));
    if (!Number.isFinite(surface[0]) || Math.hypot(...surface) < 0.1 || dot(surface, normal) < 0) {
      surface = dot(surface, normal) < 0 && Number.isFinite(surface[0]) ? mul(surface, -1) : normal;
    }
    return surface;
  };

  mesh.begin(material);
  const grid = [];
  for (let row = 0; row < rows; ++row) {
    const t = row / spans;
    const halfWidth = bladeWidth(t) * widthMetres * 0.5;
    const weight = leaf.wind * (0.55 + 0.45 * t);
    if (halfWidth < 1e-5) {
      // Collapsed row: one vertex, shared by the fan below/above it.
      grid.push([mesh.vertex(point(t, 0), normalAt(t, 0), uvAt(0, t), colour, weight)]);
      continue;
    }
    const line = [];
    for (let column = 0; column < 3; ++column) {
      const c = column - 1;
      line.push(mesh.vertex(point(t, c), normalAt(t, c), uvAt(c, t), colour, weight));
    }
    grid.push(line);
  }
  let triangles = 0;
  for (let row = 0; row + 1 < rows; ++row) {
    const a = grid[row], b = grid[row + 1];
    if (a.length === 1 && b.length === 3) {
      mesh.triangle(a[0], b[0], b[1]);
      mesh.triangle(a[0], b[1], b[2]);
      triangles += 2;
    } else if (a.length === 3 && b.length === 1) {
      mesh.triangle(a[0], a[1], b[0]);
      mesh.triangle(a[1], a[2], b[0]);
      triangles += 2;
    } else {
      mesh.quad(a[0], a[1], b[1], b[0]);
      mesh.quad(a[1], a[2], b[2], b[1]);
      triangles += 4;
    }
  }
  mesh.end();
  return { triangles, vertices: grid.reduce((sum, line) => sum + line.length, 0) };
}

// A grass blade. Not a leaf card and not a tube: a clump is a set of tapered ribbons, each one
// swept along its own strand of the branch graph. Before this existed, grass was the last
// remaining user of the legacy mesher — the one that opened an air gap at every fork and
// floated cards at a random offset — so a grass clump and a tree disagreed about wind weights,
// stable ids and LOD identity even though both went through "the same" cook.
//
// The blade is a 3-column surface (left edge, folded midrib, right edge). The fold is geometric
// so the normals differ across the blade instead of every vertex carrying the same vector,
// which is what made a flat ribbon read as a sticker rather than as a blade catching light.
export function emitGrassBlade(mesh, branch, options = {}) {
  const material = options.materialId;
  const colour = options.color ?? [86, 128, 52, 255];
  const baseWidth = options.widthMetres ?? 0.014;
  const fold = options.fold ?? 0.5;
  const step = Math.max(1, Math.round(options.step ?? 1));
  const source = branch.points;
  if (!source || source.length < 2) fail(`blade ${branch.id} needs at least two points to sweep`);
  // A coarser LOD drops every `step`-th centreline point as well as whole blades. Blade count
  // alone is a poor level of detail: two clumps that differ only in how many ribbons they carry
  // can measure the same silhouette error, because the worst-served point stays the same.
  const points = [];
  for (let index = 0; index < source.length; index += step) points.push(source[index]);
  if (points[points.length - 1] !== source[source.length - 1]) points.push(source[source.length - 1]);
  if (points.length < 2) points.push(source[source.length - 1]);
  const columns = 3;

  const grid = [];
  const reference = [];
  for (let row = 0; row < points.length; ++row) {
    const t = row / (points.length - 1);
    const point = points[row];
    let side = cross(point.tangent, [0, 1, 0]);
    let sideLength = Math.hypot(side[0], side[1], side[2]);
    if (sideLength < 1e-4) { side = point.normal; sideLength = 1; }
    const sideUnit = mul(side, 1 / sideLength);
    // Width tapers to a point at the tip. The exponent is what makes a blade look like a blade
    // rather than a triangle: it keeps most of the width for the first half.
    const width = baseWidth * Math.pow(Math.max(0, 1 - t), 0.7);
    grid.push([
      add(point.position, mul(sideUnit, width * 0.5)),
      add(point.position, mul(point.normal, -width * fold)),
      sub(point.position, mul(sideUnit, width * 0.5)),
    ]);
    reference.push(point.normal);
  }

  const normalAt = (row, column) => {
    const rowA = Math.max(0, row - 1), rowB = Math.min(grid.length - 1, row + 1);
    const columnA = Math.max(0, column - 1), columnB = Math.min(columns - 1, column + 1);
    const along = sub(grid[rowB][column], grid[rowA][column]);
    const across = sub(grid[row][columnB], grid[row][columnA]);
    let surface = normalize(cross(across, along));
    if (dot(surface, reference[row]) < 0) surface = mul(surface, -1);
    return surface;
  };

  mesh.begin(material);
  const vertices = [];
  for (let row = 0; row < grid.length; ++row) {
    const t = row / (grid.length - 1);
    // A collapsed row (a blade tip tapers to a point) is one vertex, not three coincident ones:
    // the three-vertex form adds two zero-area triangles per blade.
    if (Math.hypot(grid[row][0][0] - grid[row][2][0], grid[row][0][1] - grid[row][2][1],
      grid[row][0][2] - grid[row][2][2]) < 1e-5) {
      vertices.push([mesh.vertex(grid[row][1], normalAt(row, 1), [0.5, t], colour, points[row].wind)]);
      continue;
    }
    const line = [];
    for (let column = 0; column < columns; ++column) {
      line.push(mesh.vertex(grid[row][column], normalAt(row, column),
        [1 - column * 0.5, t], colour, points[row].wind));
    }
    vertices.push(line);
  }
  let triangles = 0;
  for (let row = 0; row + 1 < grid.length; ++row) {
    const a = vertices[row], b = vertices[row + 1];
    if (a.length === 1 && b.length === 3) {
      mesh.triangle(a[0], b[0], b[1]);
      mesh.triangle(a[0], b[1], b[2]);
      triangles += 2;
    } else if (a.length === 3 && b.length === 1) {
      mesh.triangle(a[0], a[1], b[0]);
      mesh.triangle(a[1], a[2], b[0]);
      triangles += 2;
    } else if (a.length === 3 && b.length === 3) {
      for (let column = 0; column + 1 < columns; ++column) {
        mesh.quad(a[column], a[column + 1], b[column + 1], b[column]);
      }
      triangles += (columns - 1) * 2;
    }
  }
  mesh.end();
  return { triangles, vertices: vertices.reduce((sum, line) => sum + line.length, 0) };
}

// Distance from the pivot to the host branch surface. Published in the geometry report: a
// leaf whose pivot is far from its host is a floating leaf, whatever it looks like.
export function pivotToHostDistance(leaf, graph) {
  const host = graph.byId.get(leaf.hostBranchId);
  if (!host) return Infinity;
  const sample = sampleBranch(host, leaf.hostArcMetres);
  return Math.hypot(...sub(leaf.pivot, sample.position)) - sample.radius;
}

export { bladeWidth };

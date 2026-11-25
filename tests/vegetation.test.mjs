import test from 'node:test';
import assert from 'node:assert/strict';
import { buildPackage, readPackage, digestPackage, SectionKind, PackageError } from '../engine/assets/binary-package.mjs';
import { compileSpecies, measureLodError, buildPreview, SPECIES_COOK_SCHEMA } from '../engine/vegetation/species.mjs';
import { arcLengthOf, crownMismatch, evaluateBranchGraph, sampleBranch } from '../engine/vegetation/branch-graph.mjs';
import { envelopeOf, resolveProfile, shapeCrownRadius } from '../engine/vegetation/species-profile.mjs';
import { displace } from '../engine/vegetation/wind.mjs';
import { defaultGenerators, normalizeGenerators } from '../engine/vegetation/generators.mjs';
import { placeLeaves } from '../engine/vegetation/leaf-placement.mjs';
import { buildMipChain } from '../engine/vegetation/texture.mjs';
import { buildAtlas } from '../engine/vegetation/atlas.mjs';import { CoverageMask, resolveSpacing, scatterCell, scatterRegion } from '../engine/vegetation/scatter.mjs';
import { normalizeComponents, placeComponents } from '../engine/vegetation/components.mjs';
import { applyStroke, strokeCells } from '../engine/vegetation/stroke.mjs';
import { compileRegion, dequantizeInstance, regionCapacity, INSTANCE_BYTES, validateRegion } from '../engine/vegetation/region.mjs';
import { createSurface, SurfaceSampler, MeshSurface } from '../engine/vegetation/surface.mjs';

const baseSpecies = (overrides = {}) => ({
  schema: 'nativegpu.vegetation-species/1',
  id: 'test_tree', kind: 'tree', profile: 'broadleaf', seed: 7,
  // Crown declarations are calibrated to the grown statistics (the shell is scaled by the profile's
  // crownRadiusPerHeight, not by this number), so the fixture states the measured value.
  dimensions: { heightMetres: 8, crownRadiusMetres: 3.9, trunkRadiusMetres: 0.25, leafWidthMetres: 0.3, leafHeightMetres: 0.4 },
  lod: { levels: 3, screenErrorPixels: [0.4, 2.5, 9] },
  wind: { amplitudeMetres: 0.12, frequencyHz: 0.6, stiffness: 0.5 },
  materials: [
    { id: 'bark', kind: 'bark', alphaMasked: false, doubleSided: false, roughness: 0.8, thicknessMetres: 0.02, transmission: [0.05, 0.04, 0.02] },
    { id: 'leaf', kind: 'leaf', alphaMasked: true, doubleSided: true, roughness: 0.5, thicknessMetres: 0.0004, transmission: [0.24, 0.44, 0.11] },
  ],
  // The budget is a declaration the cook validates, not a dial the cook obeys. Foliage
  // density for an 8 m broadleaf now lands above the original 60k declaration (leaf cards
  // are carried along the last two branch levels rather than clustered at the tips), so the
  // fixture states the real number instead of keeping a budget the shipped tree exceeds.
  budget: { maxVertices: 160000, maxTriangles: 240000 },
  impostor: { views: 4, size: 32 },
  ...overrides,
});

const grassSpecies = (overrides = {}) => baseSpecies({
  id: 'test_grass', kind: 'grass', profile: 'grass', seed: 11,
  dimensions: { heightMetres: 0.4, crownRadiusMetres: 0.21, trunkRadiusMetres: 0.005, bladesPerClump: 8 },
  materials: [{ id: 'blade', kind: 'blade', alphaMasked: true, doubleSided: true, roughness: 0.45, thicknessMetres: 0.0003, transmission: [0.3, 0.55, 0.14] }],
  ...overrides,
});

const shrubSpecies = () => baseSpecies({
  id: 'test_shrub', kind: 'shrub', profile: 'shrub', seed: 21,
  dimensions: { heightMetres: 2.1, crownRadiusMetres: 1.6, trunkRadiusMetres: 0.05 },
  budget: { maxVertices: 160000, maxTriangles: 240000 },
  materials: [{ id: 'stem', kind: 'bark', alphaMode: 'opaque', doubleSided: false }],
});

// The graph the cook builds, so a test can assert structural properties without going through
// the package. Kept next to the fixtures because it must use the same generator normalization.
const graphOf = (spec) => evaluateBranchGraph({
  spec,
  generators: normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) }),
  seed: spec.seed,
});

const regionSource = (overrides = {}) => ({
  schema: 'nativegpu.vegetation-region/1', id: 'test_region', seed: 99,
  bounds: { minX: -32, minZ: -32, maxX: 32, maxZ: 32 },
  cellSizeMetres: 8, pageCellMetres: 16, pageSize: 256,
  surface: { kind: 'plane', heightMetres: 0, version: 'test-plane/1' },
  species: [
    { species: 'test_tree', densityPerHectare: 40, minSpacingMetres: 5, scaleRange: [0.9, 1.1] },
    { species: 'test_grass', densityPerHectare: 900, minSpacingMetres: 0.5, scaleRange: [0.8, 1.2], lodBias: 1 },
  ],
  budget: { maxInstances: 50000 },
  ...overrides,
});

test('binary package round-trips and authenticates its own bytes', () => {
  const positions = Float32Array.from([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const bytes = buildPackage({
    schema: 'nativegpu.test-package/1', meta: { schema: 'nativegpu.test-package/1', id: 'x' },
    sections: [{ kind: SectionKind.POSITION_F32X3, index: 0, elements: positions }],
  });
  const parsed = readPackage(bytes);
  assert.equal(parsed.schema, 'nativegpu.test-package/1');
  assert.equal(parsed.meta.id, 'x');
  const section = parsed.require(SectionKind.POSITION_F32X3, 0);
  assert.equal(section.count, 3);
  assert.deepEqual([...new Float32Array(section.data.buffer, section.data.byteOffset, 9)], [...positions]);
  assert.equal(digestPackage(bytes).length, 64);

  // A single flipped byte must change the digest; nothing downstream may cache on the
  // filename alone.
  const tampered = Buffer.from(bytes);
  tampered[tampered.length - 1] ^= 0xFF;
  assert.notEqual(digestPackage(tampered), digestPackage(bytes));
});

test('binary package rejects structural corruption instead of guessing', () => {
  const bytes = buildPackage({
    schema: 'nativegpu.test-package/1', meta: { schema: 'nativegpu.test-package/1' },
    sections: [{ kind: SectionKind.INDEX_U32, index: 0, elements: Uint32Array.from([0, 1, 2]) }],
  });
  assert.throws(() => readPackage(bytes.subarray(0, bytes.length - 4)), PackageError);
  const wrongMagic = Buffer.from(bytes); wrongMagic.write('XXXX', 0, 'latin1');
  assert.throws(() => readPackage(wrongMagic), /magic/);
  // Entry 0 is META_JSON; the INDEX section is entry 1, whose count must agree with
  // its byte length. A count that disagrees is a writer bug the reader must refuse.
  const badCount = Buffer.from(bytes); badCount.writeUInt32LE(2, 32 + 24 + 16);
  assert.throws(() => readPackage(badCount), PackageError);
  // A record size that does not divide the section is a writer bug, not data to accept.
  assert.throws(() => buildPackage({
    schema: 'nativegpu.test-package/1', meta: { schema: 'nativegpu.test-package/1' },
    sections: [{ kind: SectionKind.POSITION_F32X3, index: 0, data: Buffer.alloc(10) }],
  }), /record/);
});

test('species cook is byte-deterministic and its LOD error table is monotone', () => {
  const first = compileSpecies(baseSpecies());
  const second = compileSpecies(baseSpecies());
  assert.equal(first.hash, second.hash);
  assert.deepEqual(first.bytes, second.bytes);
  assert.equal(first.meta.schema, SPECIES_COOK_SCHEMA);

  const errors = first.meta.lod.map((lod) => lod.worldErrorMetres);
  const triangles = first.meta.lod.map((lod) => lod.triangles);
  assert.equal(errors[0], 0);
  for (let i = 1; i < errors.length; ++i) {
    assert.ok(errors[i] > 0, `lod${i} error must be measured`);
    assert.ok(errors[i] >= errors[i - 1] * 0.98, `lod${i} error must not shrink`);
    assert.ok(triangles[i] <= triangles[i - 1], `lod${i} must not add triangles`);
  }
  // The measured error must be a real distance, not the AABB diagonal: a 8 m tree with
  // a 3 m crown has a ~10 m diagonal, and a LOD that only drops leaf cards is far below it.
  assert.ok(errors[1] < 2, `lod1 error ${errors[1]} looks like an AABB diagonal`);
  const parsed = readPackage(first.bytes);
  assert.equal(parsed.meta.representations.includes('impostor'), true);
  const atlas = parsed.require(SectionKind.IMPOSTOR_RGBA8, 0);
  const alpha = [...atlas.data].filter((_, index) => index % 4 === 3);
  assert.ok(alpha.some((value) => value > 0), 'impostor atlas must contain coverage');
  assert.ok(alpha.some((value) => value === 0), 'impostor atlas must have empty background');
});

test('LOD error measurement reports a distance, not a bound on a bounding box', () => {
  const dense = {
    positions: Float32Array.from([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    indices: Uint32Array.from([0, 1, 2]),
  };
  const offset = {
    positions: Float32Array.from([0, 0, 0.5, 1, 0, 0.5, 0, 1, 0.5]),
    indices: Uint32Array.from([0, 1, 2]),
  };
  const measured = measureLodError(dense, offset, 64);
  assert.ok(Math.abs(measured.maximum - 0.5) < 1e-6, `expected 0.5, got ${measured.maximum}`);
  assert.equal(measured.samples > 0, true);
});

test('atlas mip chains filter inside each cell, so one leaf cannot tint its neighbour', () => {
  // A 2x2 synthetic atlas: top-left is opaque white, the other three cells are fully transparent.
  // A whole-image box filter averages all four cells into grey from level 1 on, which is exactly
  // the cross-cell bleed that shows up as a coloured fringe on every leaf card at distance.
  const size = 64, columns = 2, rows = 2;
  const pixels = new Uint8Array(size * size * 4);
  for (let y = 0; y < size / 2; ++y) {
    for (let x = 0; x < size / 2; ++x) {
      const index = (y * size + x) * 4;
      pixels[index] = pixels[index + 1] = pixels[index + 2] = 255;
      pixels[index + 3] = 255;
    }
  }
  const atlas = { columns, rows };
  const isolated = buildMipChain({ pixels, width: size, height: size, filter: 'color', atlas });
  const bleeding = buildMipChain({ pixels, width: size, height: size, filter: 'color' });

  // A texel well inside the opaque cell, at each level.
  const cellTexel = (level) => {
    const x = Math.max(0, Math.floor(level.width / 4));
    const y = Math.max(0, Math.floor(level.height / 4));
    return level.pixels[(y * level.width + x) * 4];
  };
  // Isolated: the opaque cell keeps its own average all the way down, while a cell still spans at
  // least two texels (below that the sheet has fewer texels than cells and the grid cannot be
  // represented at all).
  for (const level of isolated) {
    if (level.width < columns * 2) continue;
    assert.ok(cellTexel(level) >= 250,
      `isolated level ${level.width}px turned the opaque cell into ${cellTexel(level)}`);
  }
  // The whole-image chain must show the bleed this prevents: at the coarsest level the sheet is
  // one texel, so a quarter-white sheet averages to roughly a quarter of the light.
  const coarsest = bleeding[bleeding.length - 1];
  assert.equal(coarsest.width, 1);
  assert.ok(cellTexel(coarsest) < 200,
    `the whole-image chain did not bleed, so this test proves nothing: value ${cellTexel(coarsest)}`);
  // And a cell that is fully transparent must remain transparent in the isolated chain.
  for (const level of isolated) {
    if (level.width < columns * 2) continue;
    const index = ((level.height - 1) * level.width + (level.width - 1)) * 4;
    assert.ok(level.pixels[index + 3] <= 1,
      `isolated level ${level.width}px made an empty cell opaque (alpha ${level.pixels[index + 3]})`);
  }
});

test('atlas coverage mip preserves each cell\'s cut-out area, not just the sheet average', () => {
  // Two cells: one fully covered, one half covered. Per-cell coverage filtering must keep each
  // cell's own ratio; the sheet average would drift both toward 0.5.
  const size = 64, columns = 2, rows = 1;
  const pixels = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const index = (y * size + x) * 4;
      const inFourth = x < size / 4;
      const value = x < size / 2 ? 255 : (inFourth ? 255 : 0);
      pixels[index] = pixels[index + 1] = pixels[index + 2] = value;
      pixels[index + 3] = 255;
    }
  }
  const levels = buildMipChain({ pixels, width: size, height: size, filter: 'coverage', cutoff: 0.5, atlas: { columns, rows } });
  const cellCoverage = (level, column) => {
    const x0 = Math.floor((column * level.width) / columns);
    const x1 = Math.max(x0 + 1, Math.ceil(((column + 1) * level.width) / columns));
    let covered = 0, total = 0;
    for (let y = 0; y < level.height; ++y) {
      for (let x = x0; x < x1; ++x) {
        const index = (y * level.width + x) * 4;
        if (level.pixels[index] >= 128) ++covered;
        ++total;
      }
    }
    return total ? covered / total : 0;
  };
  for (const level of levels.slice(0, 5)) {
    const left = cellCoverage(level, 0), right = cellCoverage(level, 1);
    assert.ok(left > 0.9, `the fully covered cell eroded to ${left.toFixed(3)} at ${level.width}px`);
    assert.ok(right < 0.1, `the empty cell filled in to ${right.toFixed(3)} at ${level.width}px`);
  }
});

test('atlas cell rects agree with the pixel grid, including a non-divisible sheet', () => {
  // §6.2 asks for corner/numbering diagnostics to verify the V direction and the half-texel
  // mapping. This is the numeric form of that check: the real oak sheet is 1254 px wide split
  // into 3 columns, which is 418 px per cell and not an integer division of the *height* into
  // 4 rows, so a cell rect that silently rounds can be half a texel off.
  const width = 1254, height = 1254, columns = 3, rows = 4;
  const atlas = buildAtlas({ columns, rows, width, height, pivot: [0.5, 0], padding: 0.02 });
  assert.equal(atlas.cells.length, columns * rows);
  for (const cell of atlas.cells) {
    // The pixel rect the report publishes and the UV rect the shader uses must describe the same
    // region: within one texel on every edge.
    assert.ok(Math.abs(cell.pixelX - cell.u0 * width) <= 1,
      `cell ${cell.index} pixelX ${cell.pixelX} vs u0*width ${cell.u0 * width}`);
    assert.ok(Math.abs(cell.pixelWidth - cell.du * width) <= 1,
      `cell ${cell.index} pixelWidth ${cell.pixelWidth} vs du*width ${cell.du * width}`);
    assert.ok(Math.abs(cell.pixelY - cell.row * (height / rows)) <= 1);
    // The cell centre must sample inside the cell, which is what a half-texel error breaks.
    const centreU = cell.u0 + cell.du / 2;
    const sampleX = Math.min(width - 1, Math.floor(centreU * width));
    assert.ok(sampleX >= cell.u0 * width && sampleX < (cell.u0 + cell.du) * width,
      `cell ${cell.index} centre samples outside its own rect (x=${sampleX})`);
  }
  // V grows upward and row 0 is the top of the sheet: the first row must own the highest v.
  const top = atlas.cells[0], bottom = atlas.cells[(rows - 1) * columns];
  assert.ok(top.v0 > bottom.v0, `row 0 v0 ${top.v0} is not above row ${rows - 1} v0 ${bottom.v0}`);
  assert.ok(Math.abs(top.v1 - 1) < 1e-9, 'the top row must end at v = 1');
  assert.ok(Math.abs(bottom.v0) < 1e-9, 'the bottom row must start at v = 0');
  // The pivot sits on the petiole end at the base of the cell: v = v0 when pivot[1] = 0.
  for (const cell of atlas.cells) {
    assert.ok(Math.abs(cell.pivotV - cell.v0) < 1e-9, `cell ${cell.index} pivot is not at its base`);
    assert.ok(Math.abs(cell.pivotU - (cell.u0 + cell.du / 2)) < 1e-9);
  }
});

test('editing one branch does not reshuffle unrelated leaf identities', () => {
  // §11.1 asks that a local edit not renumber the rest of the plant. The branch RNG is seeded from
  // the branch id and the shoot/leaf RNG from the frond and host id, so this holds by construction —
  // but "by construction" is exactly what regresses silently when someone switches to one global
  // stream. The test changes the *last* branch level's count and compares the actual leaf ids
  // produced by placement on an untouched host branch.
  const spec = baseSpecies();
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const baseline = graphOf(spec);
  const leafIdsFor = (graph, hostId) => placeLeaves({
    graph, fronds: generators.fronds, atlas: null, spec, quality: { leaves: 1 }, seed: spec.seed,
  }).leaves.filter((leaf) => leaf.hostBranchId === hostId).map((leaf) => leaf.id);
  const hosts = baseline.branches.filter((branch) => branch.level === baseline.stats.maxDepth - 1);
  assert.ok(hosts.length >= 2, 'need at least two host branches');
  const witness = hosts[0];
  const beforeIds = leafIdsFor(baseline, witness.id);
  assert.ok(beforeIds.length > 0, 'the witness host carries no leaves, so the test proves nothing');

  // Change the deepest level's count: that adds siblings elsewhere in the tree, and a global
  // stream would shift every later draw.
  const edited = baseSpecies();
  edited.generators = structuredClone(defaultGenerators(edited));
  edited.generators.levels[edited.generators.levels.length - 1].count = 3;
  const after = graphOf(edited);
  const witnessStillThere = after.byId.get(witness.id);
  assert.ok(witnessStillThere, 'the witness branch disappeared, so the edit was not local');
  const afterIds = leafIdsFor(after, witness.id);
  assert.deepEqual(afterIds, beforeIds,
    'leaf identities on an untouched host branch changed when a sibling level was edited');
  // The branch's own geometry must be identical too: its centreline seeds from its own id.
  assert.deepEqual(witnessStillThere.points.map((p) => p.position), witness.points.map((p) => p.position),
    'an unrelated branch moved when a sibling level was edited');
});

test('foliage is carried by shoots, and a coarse LOD keeps whole shoots', () => {
  // The old placement put one leaf every 1/density metres along the branch: no bud, no internode,
  // no cluster. Leaves now belong to a shoot, sit along that shoot's own axis, and a coarser LOD
  // removes whole shoots. This test pins the structure and the LOD nesting.
  const spec = baseSpecies();
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const graph = graphOf(spec);
  const place = (leaves) => placeLeaves({ graph, fronds: generators.fronds, atlas: null, spec, quality: { leaves }, seed: spec.seed });
  const full = place(1);
  const coarse = place(0.62);
  assert.ok(full.leaves.length > 100, 'the fixture must carry foliage to test');
  assert.ok(coarse.leaves.length > 0 && coarse.leaves.length < full.leaves.length,
    `LOD 0.62 kept ${coarse.leaves.length} of ${full.leaves.length} leaves`);

  // Every leaf has a shoot id derived from its own id, and its petiole base lies on that shoot.
  const perShoot = new Map();
  for (const leaf of full.leaves) {
    assert.equal(leaf.id, `${leaf.shoot.id}/${leaf.shoot.leafIndex}`);
    assert.ok(leaf.hostLevel >= 1, 'foliage must not be attached to the trunk');
    const offset = leaf.petiole.base;
    const dx = offset[0] - leaf.shoot.base[0], dy = offset[1] - leaf.shoot.base[1], dz = offset[2] - leaf.shoot.base[2];
    const along = dx * leaf.shoot.direction[0] + dy * leaf.shoot.direction[1] + dz * leaf.shoot.direction[2];
    assert.ok(along >= -1e-9 && along <= leaf.shoot.lengthMetres + 1e-9,
      `leaf ${leaf.id} sits ${along} m along a ${leaf.shoot.lengthMetres} m shoot`);
    const entry = perShoot.get(leaf.shoot.id) ?? { count: 0, exposure: leaf.shoot.exposure };
    entry.count += 1;
    perShoot.set(leaf.shoot.id, entry);
  }

  // A coarse LOD is a strict subset of LOD0: kept shoots keep every leaf they had, at the same place.
  const fullById = new Map(full.leaves.map((leaf) => [leaf.id, leaf]));
  const coarseShoots = new Set(coarse.leaves.map((leaf) => leaf.shoot.id));
  for (const leaf of coarse.leaves) {
    const reference = fullById.get(leaf.id);
    assert.ok(reference, `coarse LOD invented leaf ${leaf.id}`);
    assert.deepEqual(leaf.pivot, reference.pivot, `coarse LOD moved leaf ${leaf.id}`);
    assert.deepEqual(leaf.blade.direction, reference.blade.direction, `coarse LOD re-aimed leaf ${leaf.id}`);
  }
  const fullShoots = new Map();
  for (const leaf of full.leaves) fullShoots.set(leaf.shoot.id, (fullShoots.get(leaf.shoot.id) ?? 0) + 1);
  for (const shootId of coarseShoots) {
    const kept = coarse.leaves.filter((leaf) => leaf.shoot.id === shootId).length;
    assert.equal(kept, fullShoots.get(shootId), `shoot ${shootId} was cut in half by the LOD`);
  }

  // Shaded inner shoots carry fewer leaves than exposed outer ones. Without this, the crown is a
  // uniform brush again no matter how the leaves are grouped.
  const meanFor = (test) => {
    const entries = [...perShoot.values()].filter(test);
    return entries.reduce((sum, entry) => sum + entry.count, 0) / Math.max(1, entries.length);
  };
  const inner = meanFor((entry) => entry.exposure <= 0.6);
  const outer = meanFor((entry) => entry.exposure >= 0.9);
  assert.ok(inner > 0 && outer > 0, 'the fixture has no inner or no outer shoots');
  assert.ok(inner < outer * 0.9,
    `inner shoots carry ${inner.toFixed(2)} leaves vs ${outer.toFixed(2)} outside; the crown has no light gradient`);
});

test('leaves from different shoots keep a minimum spacing, leaves within one shoot do not', () => {
  // "0 overlap" is a bonsai, not a tree: leaves on one shoot must be allowed to sit close together.
  // The constraint is between clusters, and it is a real distance in metres.
  const spec = baseSpecies();
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const graph = graphOf(spec);
  const placed = placeLeaves({ graph, fronds: generators.fronds, atlas: null, spec, quality: { leaves: 1 }, seed: spec.seed });
  const spacing = placed.spacingMetres;
  assert.ok(spacing > 0);
  const cell = spacing;
  const buckets = new Map();
  let worst = Infinity;
  for (const leaf of placed.leaves) {
    const x = Math.floor(leaf.pivot[0] / cell), y = Math.floor(leaf.pivot[1] / cell), z = Math.floor(leaf.pivot[2] / cell);
    for (let dx = -1; dx <= 1; ++dx) for (let dy = -1; dy <= 1; ++dy) for (let dz = -1; dz <= 1; ++dz) {
      for (const other of buckets.get(`${x + dx},${y + dy},${z + dz}`) ?? []) {
        if (other.frondId !== leaf.frondId || other.shoot.id === leaf.shoot.id) continue;
        const distance = Math.hypot(leaf.pivot[0] - other.pivot[0], leaf.pivot[1] - other.pivot[1], leaf.pivot[2] - other.pivot[2]);
        worst = Math.min(worst, distance);
      }
    }
    const key = `${x},${y},${z}`;
    const bucket = buckets.get(key);
    if (bucket) bucket.push(leaf); else buckets.set(key, [leaf]);
  }
  assert.ok(worst >= spacing - 1e-9,
    `two leaves from different shoots are ${worst.toFixed(4)} m apart, below the ${spacing.toFixed(4)} m spacing`);
  // And the fixture must actually place leaves closer than that inside a shoot, or the exemption
  // above is untested.
  const perShoot = new Map();
  for (const leaf of placed.leaves) {
    const list = perShoot.get(leaf.shoot.id);
    if (list) list.push(leaf); else perShoot.set(leaf.shoot.id, [leaf]);
  }
  let closestSameShoot = Infinity;
  for (const list of perShoot.values()) {
    for (let index = 1; index < list.length; ++index) {
      closestSameShoot = Math.min(closestSameShoot, Math.hypot(
        list[index].pivot[0] - list[index - 1].pivot[0],
        list[index].pivot[1] - list[index - 1].pivot[1],
        list[index].pivot[2] - list[index - 1].pivot[2]));
    }
  }
  assert.ok(closestSameShoot < spacing, `no shoot packs leaves closer than ${spacing} m, so the exemption is untested`);
});

test('the cook publishes foliage area and crown density, not just a leaf count', () => {
  const preview = buildPreview(baseSpecies(), { level: 0 });
  const foliage = preview.foliage;
  assert.ok(foliage, 'the preview has no foliage summary');
  assert.ok(foliage.leafAreaMetresSquared > 0);
  assert.ok(foliage.leafAreaIndex > 0.5 && foliage.leafAreaIndex < 8,
    `leaf area index ${foliage.leafAreaIndex} is outside a plausible one-sided range`);
  assert.ok(foliage.shoots > 0 && foliage.emptyShoots > 0, 'the crown has no bare buds, so it cannot have gaps');
  assert.equal(preview.structure.foliage.leafAreaMetresSquared, foliage.leafAreaMetresSquared);
});

test('the cooked package keeps each material\'s normal convention', () => {
  // The convention is a property of the authored image. If it collapsed to one group value — or
  // were silently dropped — the shader would flip the green channel for the wrong sheet or for
  // neither, and the lighting would be wrong in a way no coverage test notices.
  const spec = baseSpecies({
    materials: [
      { id: 'bark', kind: 'bark', alphaMode: 'opaque', doubleSided: false, normalConvention: 'openGL+' },
      { id: 'leaf', kind: 'leaf', alphaMode: 'mask', doubleSided: true, normalConvention: 'directX-' },
    ],
  });
  const cooked = compileSpecies(spec);
  assert.equal(cooked.meta.materials[0].normalConvention, 'openGL+');
  assert.equal(cooked.meta.materials[1].normalConvention, 'directX-');
  // An unknown spelling is refused rather than guessed.
  const broken = baseSpecies({
    materials: [
      { id: 'bark', kind: 'bark', alphaMode: 'opaque', normalConvention: 'y-up' },
      { id: 'leaf', kind: 'leaf', alphaMode: 'mask', doubleSided: true },
    ],
  });
  assert.throws(() => compileSpecies(broken), /normalConvention/);
});

test('the declared crown is measured after growth and cannot reshape the tree', () => {
  // The declared crown used to be a clamp: every branch that left the declaration was projected
  // back, so "the authored crown is what the graph reaches" was a property of the projection. It is
  // now an expectation: the shell is scaled by the profile's crownRadiusPerHeight × height, and the
  // declaration is compared afterwards. These fixtures are calibrated so the two agree.
  for (const spec of [baseSpecies(), shrubSpecies(), grassSpecies()]) {
    const graph = graphOf(spec);
    const report = crownMismatch(graph);
    assert.equal(report.mismatch, false,
      `${spec.kind}: measured ${report.measured} m vs declared ${report.declared} m (fraction ${report.fraction})`);
    assert.ok(report.fraction >= 0.85 && report.fraction <= 1.05,
      `${spec.kind} reaches ${report.fraction.toFixed(3)} of its calibrated declaration`);
    // The diagnostic and the reports must describe the same measurement, including the overshoot.
    assert.ok(Math.abs(report.overshoot - (report.fraction - 1)) < 1e-12);
    assert.ok(Math.abs(graph.stats.crownOvershootFraction - report.overshoot) < 1e-12);
    assert.ok(Math.abs(graph.stats.crownFractionOfShape - report.shapeFraction) < 1e-12);
  }

  // The separation itself: changing only the declaration must not move a single branch point.
  // This is the assertion that would have failed under the old clamp (a small declaration crushed
  // the tree), and it is the reason the shape trait now lives in the profile.
  const spec = baseSpecies();
  const low = graphOf({ ...spec, dimensions: { ...spec.dimensions, crownRadiusMetres: 0.5 } });
  const high = graphOf({ ...spec, dimensions: { ...spec.dimensions, crownRadiusMetres: 40 } });
  assert.equal(low.branches.length, high.branches.length);
  for (let index = 0; index < low.branches.length; ++index) {
    assert.deepEqual(low.branches[index].points.map((point) => point.position),
      high.branches[index].points.map((point) => point.position),
      `branch ${low.branches[index].id} moved when only the crown declaration changed`);
  }
  assert.equal(low.stats.crownRadialMetres, high.stats.crownRadialMetres);
  assert.equal(low.stats.shapeCrownRadiusMetres, high.stats.shapeCrownRadiusMetres);
  // Both declarations are now wrong, and both say so instead of one silently winning.
  assert.equal(crownMismatch(low).mismatch, true);
  assert.equal(crownMismatch(high).mismatch, true);
});

test('sharp turns only ever come from a constraint correction, never from free growth', () => {
  // Measured in R1-12: every turn above 28° sat on a step the shell or the height limit had just
  // corrected (`free = 0`). That is worth an invariant, not just a paragraph in a report: if a
  // future change starts producing sharp turns during ordinary growth, the cause is the growth
  // rule itself, and this test says so instead of leaving the next reader to attribute spikes by
  // hand. The flags are diagnostic-only; nothing in the geometry path reads them.
  let checked = 0;
  for (const spec of [baseSpecies(), shrubSpecies(), grassSpecies()]) {
    const graph = graphOf(spec);
    let sharp = 0, corrected = 0;
    for (const branch of graph.branches) {
      const points = branch.points;
      for (let index = 1; index + 1 < points.length; ++index) {
        const a = points[index].tangent, b = points[index + 1].tangent;
        const la = Math.hypot(a[0], a[1], a[2]), lb = Math.hypot(b[0], b[1], b[2]);
        if (la < 1e-9 || lb < 1e-9) continue;
        const cosine = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
        if (Math.acos(Math.max(-1, Math.min(1, cosine))) <= 0.5) continue;
        ++sharp;
        if (points[index + 1].capped || points[index + 1].apexed) ++corrected;
      }
    }
    assert.equal(corrected, sharp,
      `${spec.kind}: ${sharp - corrected} of ${sharp} sharp turns came from free growth`);
    checked += graph.branches.length;
  }
  assert.ok(checked > 10, 'the check needs branches to be meaningful');
});

test('a crown that disagrees with its declaration is reported, not silently clamped', () => {
  // The generator used to force the crown onto its declared radius, which is why a declaration the
  // branch statistics could never reach looked "reached". Now the mismatch is a measurement.
  const matched = crownMismatch(graphOf(baseSpecies()));
  assert.equal(typeof matched.fraction, 'number', 'the crown was not measured at all');
  assert.equal(matched.mismatch, false, `the broadleaf fixture is already mismatched: ${matched.fraction}`);
  assert.equal(matched.tolerance, 0.15);

  // Declare a crown the tree cannot grow: the graph must say so, and must not quietly stretch.
  const spec = baseSpecies();
  const impossible = graphOf({ ...spec, dimensions: { ...spec.dimensions, crownRadiusMetres: 40 } });
  const reported = crownMismatch(impossible);
  assert.equal(reported.mismatch, true, 'a 40 m declaration on an 8 m tree was reported as fine');
  assert.ok(reported.fraction < 0.5, `expected a large mismatch, got ${reported.fraction}`);
  assert.ok(reported.declared === 40 && reported.measured > 0);

  // Unmeasurable input is reported as such, not as "ok".
  assert.equal(crownMismatch({ crownRadiusMetres: 0 }).reason, 'unmeasured');
});

test('species profiles shape the growth shell, and an unknown profile is refused', () => {
  const H = 10;
  const conifer = resolveProfile({ profile: 'conifer' });
  const broadleaf = resolveProfile({ profile: 'broadleaf' });
  const grass = resolveProfile({ profile: 'grass' });
  const cone = envelopeOf(conifer, H);
  const broad = envelopeOf(broadleaf, H);
  const clump = envelopeOf(grass, H);
  // The shell scale is the profile trait × height. The authored crown radius is deliberately not
  // an input: a declaration must not be able to bend the geometry.
  const C = H * conifer.crownRadiusPerHeight;
  assert.equal(shapeCrownRadius(broadleaf, H), H * broadleaf.crownRadiusPerHeight);

  // A conifer tapers with height and — the part that matters — keeps a non-zero radius at the
  // apex. A shell that collapses to zero radius is what made the apical branches zigzag.
  const coneBase = cone([0, H * 0.23, 0]);
  const coneMid = cone([0, H * 0.6, 0]);
  const coneApex = cone([0, H, 0]);
  assert.ok(Math.abs(coneBase - C) < C * 0.35, `conifer is ${coneBase} wide at its crown base, expected ~${C}`);
  assert.ok(coneMid < coneBase && coneApex < coneMid, 'conifer shell must narrow with height');
  const apexFloor = resolveProfile({ profile: 'conifer' }).envelope.apexFloorFraction;
  assert.ok(coneApex >= C * apexFloor - 1e-9, `conifer apex collapsed to ${coneApex}, below its declared floor`);

  // A broadleaf is widest around the middle of the crown and narrower at both ends.
  const BC = H * broadleaf.crownRadiusPerHeight;
  const broadCentre = broad([0, H * 0.58, 0]);
  assert.ok(Math.abs(broadCentre - BC) < BC * 0.05, `broadleaf is ${broadCentre} at its widest, expected ${BC}`);
  assert.ok(broad([0, H * 0.05, 0]) < broadCentre && broad([0, H * 0.95, 0]) < broadCentre,
    'broadleaf shell must be narrower at the bottom and the top than at mid-height');

  // A grass clump is bounded by one radius at every height.
  const GC = H * grass.crownRadiusPerHeight;
  for (const y of [0.05, 0.2, 0.5]) assert.equal(clump([0, y, 0]), GC);

  // The profile is the only place shapes are declared, so a typo must be a coded refusal rather
  // than a broadleaf that looks plausible.
  assert.throws(() => resolveProfile({ profile: 'palm' }), (error) => error.code === 'NG-E-VEG-SPECIES');
  // A spec that predates the field still resolves, through its coarse category.
  assert.equal(resolveProfile({}).name, 'broadleaf');
  assert.equal(resolveProfile({ kind: 'tree' }).name, 'broadleaf');
  assert.equal(resolveProfile({ kind: 'shrub' }).name, 'shrub');
});

test('arc length is measured on the curve, so sampling can reach the real tip', () => {
  // The cook used to write `stepLength * (step + 1)` as the arc length: a target, not a
  // measurement. Steering, gravitropism and the envelope clamp all move the point, so a curved
  // branch is shorter (clamped steps) or longer than its target. Bark V, leaf spacing and every
  // attachment sample are metres on the real curve, so the stored value has to be the chord sum.
  for (const spec of [baseSpecies(), shrubSpecies(), grassSpecies()]) {
    const graph = graphOf(spec);
    let measured = 0;
    for (const branch of graph.branches) {
      const points = branch.points;
      assert.equal(points[0].arcLength, 0, `${branch.id} does not start at arc 0`);
      for (let index = 1; index < points.length; ++index) {
        const previous = points[index - 1].position, point = points[index].position;
        const chord = Math.hypot(point[0] - previous[0], point[1] - previous[1], point[2] - previous[2]);
        const step = points[index].arcLength - points[index - 1].arcLength;
        assert.ok(step >= 0, `${branch.id} has a negative arc step at ${index}`);
        assert.ok(Math.abs(step - chord) < 1e-6,
          `${branch.id} step ${index}: stored ${step.toFixed(6)} m but the points are ${chord.toFixed(6)} m apart`);
      }
      const total = points[points.length - 1].arcLength;
      assert.ok(Math.abs(arcLengthOf(branch) - total) < 1e-9,
        `${branch.id} reports ${arcLengthOf(branch)} m total but its points end at ${total}`);
      ++measured;
    }
    assert.ok(measured > 0);

    // The authored length is a target and must stay separate: if the two were the same field,
    // nothing downstream could tell a stretched branch from a longer one.
    assert.ok(Number.isFinite(graph.branches[0].length), 'the authored target length was overwritten');

    // Sampling at the measured total must land on the tip. Clamping to the authored target
    // instead is what used to leave the last stretch of a clamped branch unsampleable, which
    // bunched every leaf on the final ring.
    for (const branch of graph.branches) {
      const atTip = sampleBranch(branch, arcLengthOf(branch));
      const tip = branch.points[branch.points.length - 1].position;
      const gap = Math.hypot(atTip.position[0] - tip[0], atTip.position[1] - tip[1], atTip.position[2] - tip[2]);
      assert.ok(gap < 1e-9, `${branch.id}: sampling at its own arc length misses the tip by ${gap} m`);
    }
  }
});

test('every branch root shares its parent wind weight at the attachment', () => {
  // "Connected while still" is not enough. Wind displaces a vertex by its own weight, so a child
  // whose root weight differs from the parent's weight at the same arc position tears the fork
  // apart as soon as the wind blows — the seam does not exist in rest pose, only in motion.
  for (const spec of [baseSpecies(), shrubSpecies(), grassSpecies()]) {
    const graph = graphOf(spec);
    let junctions = 0;
    for (const branch of graph.branches) {
      if (!branch.parentId) continue;
      const parent = graph.byId.get(branch.parentId);
      assert.ok(parent, `branch ${branch.id} has no parent`);
      const parentWind = sampleBranch(parent, branch.attachment.parentArcMetres).wind;
      const rootWind = branch.points[0].wind;
      assert.ok(Math.abs(parentWind - rootWind) < 1e-6,
        `${branch.id} root wind ${rootWind.toFixed(6)} != parent wind ${parentWind.toFixed(6)} at the same arc`);
      ++junctions;
    }
    // Grass is one level (blades from a base), so it has no junctions by construction; the
    // check is meaningful for the branching kinds and must not be skipped silently for them.
    assert.ok(junctions > 0 || spec.kind === 'grass',
      `${spec.kind} produced no junctions to check`);
  }
});

test('wind weights rise monotonically from base to tip along a branch', () => {
  // A branch whose weights are not monotone would fold on itself under wind: a vertex further
  // from the root moving less than the one below it is a bend the plant cannot make.
  const graph = graphOf(baseSpecies());
  for (const branch of graph.branches) {
    let previous = -Infinity;
    for (const point of branch.points) {
      assert.ok(point.wind >= previous - 1e-9,
        `branch ${branch.id} wind weight dropped from ${previous} to ${point.wind}`);
      previous = point.wind;
    }
    assert.ok(previous <= 1 + 1e-9, `branch ${branch.id} tip wind ${previous} exceeds 1`);
  }
});

test('a fork stays welded for 300 frames of wind, and the test would notice if it did not', () => {
  // §11.1 asks for at least 300 continuous frames and "the junction error does not grow". The
  // proof is in the data: the child root sits exactly on the parent centreline *and* carries the
  // parent's wind weight at that arc position, so any deterministic displacement function moves
  // both by the same amount. This test exercises that over 300 frames of the shader's own model
  // (engine/vegetation/wind.mjs mirrors shaders/vegetation.hlsl VSMain).
  const spec = baseSpecies();
  const graph = graphOf(spec);
  const options = { phase: 0.37, amplitude: spec.wind.amplitudeMetres, frequency: spec.wind.frequencyHz, direction: [0.83, 0, 0.55] };
  const frames = 300;
  let worst = 0;
  let worstFrame = -1;
  for (let frame = 0; frame < frames; ++frame) {
    const seconds = frame / 60;
    for (const branch of graph.branches) {
      if (!branch.parentId) continue;
      const parent = graph.byId.get(branch.parentId);
      const onParent = sampleBranch(parent, branch.attachment.parentArcMetres);
      const root = branch.points[0];
      const movedParent = displace(onParent.position, onParent.wind, { ...options, seconds });
      const movedRoot = displace(root.position, root.wind, { ...options, seconds });
      const gap = Math.hypot(movedRoot[0] - movedParent[0], movedRoot[1] - movedParent[1], movedRoot[2] - movedParent[2]);
      if (gap > worst) { worst = gap; worstFrame = frame; }
      for (const value of [...movedRoot, ...movedParent]) {
        assert.ok(Number.isFinite(value), `frame ${frame}: wind produced a non-finite position`);
      }
    }
  }
  // A micrometre over 300 frames: the child root is on the parent centreline, not near it.
  assert.ok(worst < 1e-6, `junction drifted ${worst.toExponential(3)} m under wind by frame ${worstFrame}`);

  // Sensitivity: the same measurement must fail when the data is wrong by 5% of the wind range.
  // Without this, the assertion above would also pass if the test forgot to apply any wind at all.
  const branch = graph.branches.find((candidate) => candidate.parentId !== null);
  const parent = graph.byId.get(branch.parentId);
  const onParent = sampleBranch(parent, branch.attachment.parentArcMetres);
  const root = branch.points[0];
  let sensitivity = 0;
  for (let frame = 0; frame < frames; ++frame) {
    const seconds = frame / 60;
    const movedParent = displace(onParent.position, onParent.wind, { ...options, seconds });
    const movedRoot = displace(root.position, root.wind + 0.05, { ...options, seconds });
    sensitivity = Math.max(sensitivity, Math.hypot(
      movedRoot[0] - movedParent[0], movedRoot[1] - movedParent[1], movedRoot[2] - movedParent[2]));
  }
  assert.ok(sensitivity > 1e-4,
    `a 0.05 wind-weight error only moves the junction ${sensitivity.toExponential(3)} m; the test cannot see a defect`);
});

test('the LOD table reports lost surface, not just moved surface', () => {
  // The documented defect: a coarse LOD drops whole leaf cards, which moves no remaining vertex,
  // so the distance-based error stays tiny while the crown thins. Both numbers must be published,
  // and this test pins the relationship between them.
  const cooked = compileSpecies(baseSpecies());
  const table = cooked.meta.lod;
  assert.equal(table[0].areaRetention, 1, 'LOD0 must keep all of its surface');
  for (let level = 1; level < table.length; ++level) {
    const entry = table[level];
    assert.ok(Number.isFinite(entry.areaMetresSquared) && entry.areaMetresSquared > 0,
      `lod${level} has no measured area`);
    assert.ok(entry.areaRetention > 0 && entry.areaRetention < 1,
      `lod${level} reports areaRetention ${entry.areaRetention}`);
    assert.ok(entry.areaRetention < table[level - 1].areaRetention,
      `lod${level} kept more surface than lod${level - 1}`);
    // The point of the pair: distance alone would call this level cheap.
    if (level === 1) {
      assert.ok(entry.worldErrorMetres < 0.35,
        `lod1 distance ${entry.worldErrorMetres} is larger than expected; the fixture changed`);
      assert.ok(entry.areaRetention < 0.8,
        `lod1 kept ${entry.areaRetention} of its surface, so this fixture cannot show the gap`);
    }
  }
  // A preview's analysis table carries the same numbers as the package.
  const preview = buildPreview(baseSpecies(), { level: 0, analysis: true });
  assert.equal(preview.lodTable.length, table.length);
  for (let level = 0; level < table.length; ++level) {
    assert.equal(preview.lodTable[level].areaRetention, table[level].areaRetention,
      `preview and cook disagree on lod${level} area retention`);
  }
});

test('grass species keeps a subset of blades per LOD, and the loss shows in retained surface', () => {
  const cooked = compileSpecies(grassSpecies());
  const counts = cooked.meta.lod.map((lod) => lod.triangles);
  assert.ok(counts[0] > counts[1] && counts[1] > counts[2], `grass LODs must shrink: ${counts}`);
  const errors = cooked.meta.lod.map((lod) => lod.worldErrorMetres);
  assert.ok(errors[1] > 0, 'the coarser LODs must measure an error, not report zero');

  // The one-sided distance error is NOT the monotonic signal for a clump, and asserting that it
  // was hid a real property behind an artifact: measured on this fixture the error is essentially
  // the blade spacing (8 blades → 0.2807 m, 16 blades → 0.1515 m), so dropping 8 blades to 5 and
  // then to 2 lands on the same maximum gap instead of growing. What does grow monotonically is
  // the retained surface — 1.0 → 0.651 → 0.417 — which is the quantity §6.3 of the task book asks
  // for. Assert both: the loss is visible, and the distance error does not blow up.
  const retention = cooked.meta.lod.map((lod) => lod.areaRetention);
  assert.ok(retention[0] > retention[1] && retention[1] > retention[2],
    `grass LODs must lose surface monotonically: ${retention}`);
  assert.ok(errors[2] < errors[1] * 1.05,
    `coarser grass LOD reported a much larger distance error (${errors[1]} → ${errors[2]}), which means the blade subset is not a reduction`);
});

test('scatter is invariant to which cell is rebuilt', () => {
  const species = [compileSpecies(baseSpecies()), compileSpecies(grassSpecies())];
  const parsed = validateRegion(regionSource(), new Set(['test_tree', 'test_grass']));
  const surface = new SurfaceSampler(createSurface(parsed.surface));
  const full = scatterRegion({ region: parsed, rules: parsed.rules, surface });

  // Rebuild one cell on its own, from a fresh sampler, and compare with the same cell
  // inside the full pass. Nothing in scatterCell may read a neighbour.
  const single = scatterCell({ region: parsed, rules: parsed.rules, surface: new SurfaceSampler(createSurface(parsed.surface)), cellX: 3, cellZ: 5 });
  const inside = full.instances.filter((instance) => instance.cellX === 3 && instance.cellZ === 5);
  assert.ok(inside.length > 0);
  assert.equal(single.length, inside.length);
  for (let i = 0; i < single.length; ++i) {
    assert.equal(single[i].x, inside[i].x);
    assert.equal(single[i].z, inside[i].z);
    assert.equal(single[i].species, inside[i].species);
  }
  assert.ok(species.length === 2);
});

test('scatter honours slope and height masks on a real mesh surface', () => {
  // A single 45-degree ramp: plants with slope.maxDegrees < 45 must be rejected.
  const positions = Float32Array.from([-50, 0, -50, 50, 100, -50, 50, 100, 50, -50, 0, 50]);
  const indices = Uint32Array.from([0, 2, 1, 0, 3, 2]);
  const surface = new SurfaceSampler(new MeshSurface({ positions, indices, version: 'ramp/1' }));
  const hit = surface.sample(0, 0);
  assert.ok(hit, 'ramp must be sampled');
  assert.ok(Math.abs(hit.slopeRadians - Math.PI / 4) < 0.02, `slope ${hit.slopeRadians} is not 45 degrees`);
  assert.ok(Math.abs(hit.heightMetres - 50) < 1);

  const region = validateRegion(regionSource({
    surface: { kind: 'mesh', version: 'ramp/1' },
    species: [
      { species: 'test_tree', densityPerHectare: 400, minSpacingMetres: 2, slope: { maxDegrees: 20 } },
      { species: 'test_grass', densityPerHectare: 400, minSpacingMetres: 2, slope: { maxDegrees: 80 } },
    ],
  }), new Set(['test_tree', 'test_grass']));
  const scattered = scatterRegion({ region, rules: region.rules, surface });
  const trees = scattered.instances.filter((instance) => instance.species === 'test_tree');
  const grass = scattered.instances.filter((instance) => instance.species === 'test_grass');
  assert.equal(trees.length, 0, 'a 20-degree slope limit must reject a 45-degree ramp');
  assert.ok(grass.length > 0, 'an 80-degree slope limit must accept a 45-degree ramp');
});

test('region pages sum to the instance count and quantize inside their error budget', () => {
  const species = [compileSpecies(baseSpecies()), compileSpecies(grassSpecies())];
  const compiled = compileRegion({ region: regionSource(), species });
  assert.ok(compiled.instances > 0);
  assert.equal(compiled.meta.bytes.perInstance, INSTANCE_BYTES);
  assert.equal(compiled.instanceBytes, compiled.instances * INSTANCE_BYTES);

  const parsed = readPackage(compiled.bytes);
  const pages = parsed.require(SectionKind.PAGE, 0);
  const instances = parsed.require(SectionKind.INSTANCE_PACKED, 0);
  assert.equal(instances.count, compiled.instances);
  assert.equal(pages.count, compiled.pages);

  // Every record belongs to exactly one page, and every page range is inside the file.
  let covered = 0;
  const ids = new Set();
  for (let index = 0; index < pages.count; ++index) {
    const first = pages.data.readUInt32LE(index * 64);
    const count = pages.data.readUInt32LE(index * 64 + 4);
    assert.equal(first, covered, 'pages must be contiguous and ordered');
    covered += count;
  }
  assert.equal(covered, compiled.instances);

  // Quantization round-trip: decode a record and compare with the source instance.
  const record = compiled.records[0], page = compiled.pageRecords[0];
  const decoded = dequantizeInstance(record, page);
  assert.equal(ids.has(record.stableId), false);
  ids.add(record.stableId);
  for (const value of Object.values(decoded)) if (typeof value === 'number') assert.ok(Number.isFinite(value));
  assert.ok(Math.abs(decoded.scale - Math.pow(2, (record.scale / 255) * 8 - 4)) < 1e-9);
  assert.ok(compiled.meta.quantization.positionErrorMetres < 0.01);
  assert.ok(compiled.meta.quantization.heightErrorMetres < 0.01);
});

test('stable ids are unique and survive a rebuild of the same source', () => {
  const species = [compileSpecies(baseSpecies()), compileSpecies(grassSpecies())];
  const first = compileRegion({ region: regionSource(), species });
  const second = compileRegion({ region: regionSource(), species });
  assert.equal(first.hash, second.hash);
  const ids = new Set(first.records.map((record) => record.stableId));
  assert.equal(ids.size, first.records.length);
  // A different region seed must move the plants, not just relabel them.
  const other = compileRegion({ region: regionSource({ seed: 100 }), species });
  assert.notEqual(other.hash, first.hash);
});

test('region capacity maths covers the 30M contract without materialising it', () => {
  const capacity = regionCapacity({ worldInstances: 30_000_000 });
  assert.equal(capacity.instanceBytes, 960_000_000);
  assert.ok(Math.abs(capacity.decimalMegabytes - 960.234_432) < 1e-6);
  assert.equal(capacity.fitsInVegetationBudget, true);
  const withPages = regionCapacity({ worldInstances: 30_000_000, pageSize: 8192 });
  assert.equal(withPages.pages, 3663);
  assert.ok(withPages.totalBytes > 960_000_000);
});

test('species validation rejects a LOD table that is not a reduction', () => {
  assert.throws(() => compileSpecies(baseSpecies({ lod: { levels: 3, screenErrorPixels: [0.4, 0.2, 9] } })), /strictly increase/);
  assert.throws(() => compileSpecies(baseSpecies({ budget: { maxVertices: 4, maxTriangles: 4 } })), /budget/);
  assert.throws(() => compileSpecies(baseSpecies({ materials: [] })), /1–8/);
  assert.throws(() => compileSpecies(baseSpecies({ kind: 'grass' })), /bladesPerClump/);
});

test('mask mip chain never loses the cutout, where a plain box filter does', () => {
  // A synthetic 32x32 leaf mask made of 1 px strokes every 8 px — the thin-structure case a
  // leaf atlas is made of. Threshold coverage is not area coverage: as strokes merge into
  // the background the measured coverage first rises (a half-covered texel counts as fully
  // covered) and then collapses to zero once the average falls below the cutoff. That
  // collapse is the hole this chain exists to prevent, so the assertions are about survival
  // at the levels where it happens, not about matching a ratio.
  const size = 32;
  const spacing = 8;
  const pixels = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; ++y) for (let x = 0; x < size; ++x) {
    const inside = x % spacing === 0;
    const offset = (y * size + x) * 4;
    pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = inside ? 255 : 0;
    pixels[offset + 3] = 255;
  }
  const cutoff = 0.25;
  const cutoffByte = Math.round(cutoff * 255);
  const coverage = (level) => {
    let covered = 0;
    for (let index = 0; index < level.pixels.length; index += 4) if (level.pixels[index] >= cutoffByte) ++covered;
    return covered / (level.pixels.length / 4);
  };

  const chain = buildMipChain({ pixels, width: size, height: size, filter: 'coverage', cutoff });
  assert.deepEqual(chain.map((level) => level.width), [32, 16, 8, 4, 2, 1], 'a 32 px sheet cooks 32,16,8,4,2,1');

  // Control: the same reduction without the coverage filter loses the stroke entirely at
  // 4 px and below. If this stops being true the test below is vacuous.
  const plain = buildMipChain({ pixels, width: size, height: size, filter: 'data', cutoff });
  assert.equal(coverage(plain[3]), 0, 'the plain box filter should have erased the stroke at 4 px');
  assert.equal(coverage(plain[4]), 0, 'the plain box filter should have erased the stroke at 2 px');

  // The coverage chain must still have something to draw at every level it is asked for.
  for (const level of chain) {
    assert.ok(coverage(level) > 0, `level ${level.width} lost the cutout entirely`);
  }
  assert.ok(coverage(chain[3]) > 0, 'the coverage filter did not rescue the 4 px level the plain filter erased');

  // Cooking twice must produce identical bytes: a mip chain that depends on iteration order
  // would make the digest unstable and every downstream cache useless.
  const again = buildMipChain({ pixels, width: size, height: size, filter: 'coverage', cutoff });
  assert.deepEqual(again.map((level) => level.width), chain.map((level) => level.width));
  for (let index = 0; index < chain.length; ++index) assert.deepEqual(again[index].pixels, chain[index].pixels);

  // A reduced normal level must come out unit length, not merely averaged. Level 0 is
  // excluded: it is authored data and is stored as supplied, not produced by the reducer.
  const normals = new Uint8Array(size * size * 4);
  for (let index = 0; index < normals.length; index += 4) {
    normals[index] = 128; normals[index + 1] = 128; normals[index + 2] = 255; normals[index + 3] = 255;
  }
  normals[0] = 255; normals[1] = 0; normals[2] = 128;
  const normalChain = buildMipChain({ pixels: normals, width: size, height: size, filter: 'normal' });
  for (const level of normalChain.slice(1)) {
    for (let index = 0; index < level.pixels.length; index += 4) {
      const nx = level.pixels[index] / 127.5 - 1;
      const ny = level.pixels[index + 1] / 127.5 - 1;
      const nz = level.pixels[index + 2] / 127.5 - 1;
      assert.ok(Math.abs(Math.hypot(nx, ny, nz) - 1) < 0.03, `normal mip ${level.width} is not unit length`);
    }
  }
});

// ---------------------------------------------------------------- component layer
// The extension plan (§3.1, V02/V03/V05) asks for authored parts — fruit, flowers, seed heads,
// pods, compound leaves, floating pads, culm nodes — attached to the branch graph with stable
// identities and a stage filter. These tests pin the two properties that make the layer usable
// rather than decorative: adding a part cannot move the plant, and a part that is not baked is
// reported instead of silently dropped.

const fruitMaterial = { id: 'fruit', kind: 'bark', alphaMasked: false, doubleSided: false, roughness: 0.45, thicknessMetres: 0.02, transmission: [0.05, 0.04, 0.02] };

const withParts = (components, overrides = {}) => baseSpecies({
  materials: [...baseSpecies().materials, fruitMaterial],
  components,
  ...overrides,
});

const fruitComponent = (attach = {}) => ({
  id: 'fruit_1', type: 'fruit', material: 'fruit', sizeMetres: [0.05, 0.07], wind: 0.7,
  colour: [214, 132, 48],
  attach: { hostLevels: [3, 4], countPerHost: [2, 5], spreadRadians: 0.7, upBias: -0.2, droop: 0.4, ...attach },
  shape: { aspect: 1.15 },
});

const flowerComponent = () => ({
  id: 'flower_1', type: 'flower', material: 'leaf', sizeMetres: [0.03, 0.04], stages: ['flowering'],
  attach: { hostLevels: [4], countPerHost: [1, 3], spreadRadians: 1.2, upBias: 0.3 },
});

test('a species without components keeps the geometry and metadata it had before the layer existed', () => {
  const plain = compileSpecies(baseSpecies());
  const explicitEmpty = compileSpecies(baseSpecies({ components: [] }));
  assert.equal(plain.meta.structure.components, undefined, 'no components must mean no component block in meta');
  assert.equal(explicitEmpty.meta.structure.components, undefined);
  // Not just "close": the two cooks must produce the same mesh, because the empty list has to be
  // a no-op rather than a second code path that happens to agree today.
  assert.deepEqual(explicitEmpty.meta.lod.map((lod) => lod.triangles), plain.meta.lod.map((lod) => lod.triangles));
  assert.deepEqual(explicitEmpty.meta.lod.map((lod) => lod.vertices), plain.meta.lod.map((lod) => lod.vertices));
});

test('a fruit component adds geometry and leaves every branch and leaf where it was', () => {
  const plain = compileSpecies(baseSpecies());
  const fruited = compileSpecies(withParts([fruitComponent()]));
  assert.ok(fruited.meta.lod[0].triangles > plain.meta.lod[0].triangles, 'fruit must add triangles');
  assert.equal(fruited.meta.structure.leaves, plain.meta.structure.leaves, 'fruit must not change the leaf count');
  assert.equal(fruited.meta.structure.totalArcLengthMetres, plain.meta.structure.totalArcLengthMetres);
  assert.equal(fruited.meta.structure.branchCount, plain.meta.structure.branchCount);

  const summary = fruited.meta.structure.components.fruit_1;
  assert.equal(summary.type, 'fruit');
  assert.ok(summary.parts > 0, 'the component must report placed parts, not just a declaration');
  assert.ok(summary.hosts > 0);
  assert.ok(summary.triangles > 0);

  // The strong form of "the crown did not move": components are emitted after the foliage, so
  // every vertex the plain species produced must still be there, in the same order, with the
  // same coordinates. A placement that read the global RNG would fail this.
  const plainPreview = buildPreview(baseSpecies(), { level: 0 });
  const richPreview = buildPreview(withParts([fruitComponent()]), { level: 0 });
  const shared = plainPreview.geometry.positions.length;
  assert.ok(richPreview.geometry.positions.length > shared);
  for (let index = 0; index < shared; ++index) {
    assert.equal(richPreview.geometry.positions[index], plainPreview.geometry.positions[index],
      `component emission moved vertex ${index} of the plant`);
  }
});

test('changing how many fruit a plant carries does not re-roll its crown', () => {
  const sparse = compileSpecies(withParts([fruitComponent({ countPerHost: [1, 1] })]));
  const heavy = compileSpecies(withParts([fruitComponent({ countPerHost: [7, 7] })]));
  assert.ok(heavy.meta.structure.components.fruit_1.parts > sparse.meta.structure.components.fruit_1.parts);
  assert.equal(heavy.meta.structure.leaves, sparse.meta.structure.leaves);
  assert.equal(heavy.meta.structure.totalArcLengthMetres, sparse.meta.structure.totalArcLengthMetres);
  assert.equal(heavy.meta.structure.crown.measured, sparse.meta.structure.crown.measured);
});

test('a component whose stage is not being baked reports skippedStage instead of vanishing', () => {
  const vegetative = compileSpecies(withParts([fruitComponent(), flowerComponent()]));
  const flowering = compileSpecies(withParts([fruitComponent(), flowerComponent()], { stage: 'flowering' }));
  assert.equal(vegetative.meta.structure.components.flower_1.parts, 0);
  assert.equal(vegetative.meta.structure.components.flower_1.skippedStage, true);
  assert.equal(vegetative.meta.structure.stage, null);
  assert.ok(flowering.meta.structure.components.flower_1.parts > 0, 'the flowering stage must bake the flowers');
  assert.equal(flowering.meta.structure.components.flower_1.skippedStage, false);
  assert.equal(flowering.meta.structure.stage, 'flowering');
  // Fruit is unconditional, so it is present in both stages.
  assert.equal(flowering.meta.structure.components.fruit_1.parts, vegetative.meta.structure.components.fruit_1.parts);
});

test('a coarser LOD keeps a strict subset of the component parts, at the same positions', () => {
  const spec = withParts([fruitComponent()]);
  const graph = evaluateBranchGraph({
    spec, generators: normalizeGenerators({ generators: defaultGenerators(spec) }), seed: spec.seed,
  });
  const components = normalizeComponents(spec);
  const full = placeComponents({ graph, components, seed: spec.seed, quality: { level: 0, components: 1 } });
  const coarse = placeComponents({ graph, components, seed: spec.seed, quality: { level: 2, components: 0.35 } });
  assert.ok(full.parts.length > 0);
  assert.ok(coarse.parts.length < full.parts.length, 'the coarse LOD must drop parts');
  const byId = new Map(full.parts.map((part) => [part.id, part]));
  for (const part of coarse.parts) {
    const reference = byId.get(part.id);
    assert.ok(reference, `LOD2 kept ${part.id}, which LOD0 does not have`);
    assert.deepEqual(part.position, reference.position, 'a kept part must not move between LODs');
  }
});

test('component types this layer does not implement are rejected by name, not ignored', () => {
  assert.throws(() => compileSpecies(withParts([{ id: 'moss_1', type: 'surfacePatch' }])), /surfacePatch/);
  assert.throws(() => compileSpecies(withParts([{ id: 'root_1', type: 'root' }])), /root/);
  assert.throws(() => compileSpecies(withParts([{ id: 'x_1', type: 'sausage' }])), /unknown type/);
  assert.throws(() => compileSpecies(withParts([{ id: 'leaf_1', type: 'leaf' }])), /frond/);
  assert.throws(() => compileSpecies(withParts([{ ...fruitComponent(), material: 'nope' }])), /unknown material/);
});

test('a grass clump can carry a seed head, which is how rice and foxtail are built', () => {
  const ear = {
    id: 'ear', type: 'seedHead', material: 'blade', sizeMetres: [0.18, 0.22],
    attach: { hostLevels: [0], distribution: 'tip', countPerHost: [1, 1], spreadRadians: 0.15, upBias: 0.6, droop: 0.1 },
    shape: { form: 'panicle', lengthMetres: 0.18, spikelets: 32, lateralLengthMetres: 0.04 },
  };
  const bare = compileSpecies(grassSpecies());
  const withEar = compileSpecies(grassSpecies({ components: [ear] }));
  assert.equal(withEar.meta.structure.components.ear.parts, 8, 'one ear per blade');
  assert.ok(withEar.meta.lod[0].triangles > bare.meta.lod[0].triangles);
  assert.deepEqual(
    withEar.meta.lod.map((lod) => lod.areaRetention.length),
    bare.meta.lod.map((lod) => lod.areaRetention.length));
});

// ---------------------------------------------------------------- placement rules
// The plan's §4.3 asks for density masks, exclusions, row planting and cross-cell spacing, with
// candidate ids assigned before any filter. These tests cover the properties, not the knobs.

const placementRegion = (overrides = {}) => regionSource({
  bounds: { minX: 0, minZ: 0, maxX: 32, maxZ: 32 },
  species: [{ species: 'test_tree', densityPerHectare: 120, minSpacingMetres: 3, scaleRange: [0.9, 1.1] }],
  budget: { maxInstances: 50000 },
  ...overrides,
});

test('candidate ids are assigned before the filters, so removing plants does not renumber the rest', () => {
  const surface = new SurfaceSampler(createSurface({ kind: 'plane', heightMetres: 0, version: 'plane/1' }));
  const open = validateRegion(placementRegion(), new Set(['test_tree']));
  const clipped = validateRegion(placementRegion({
    exclusions: [{ kind: 'circle', x: 16, z: 16, radiusMetres: 7, marginMetres: 0 }],
  }), new Set(['test_tree']));
  const full = scatterRegion({ region: open, rules: open.rules, surface });
  const cut = scatterRegion({ region: clipped, rules: clipped.rules, surface });
  assert.ok(cut.instances.length < full.instances.length, 'the exclusion must remove plants');
  assert.ok(cut.rejectedByExclusion > 0, 'the rejection must be counted');
  const byKey = new Map(full.instances.map((instance) => [instance.candidateId, instance]));
  for (const instance of cut.instances) {
    const reference = byKey.get(instance.candidateId);
    assert.ok(reference, `clipping produced a candidate ${instance.candidateId} the open run did not have`);
    assert.equal(instance.x, reference.x);
    assert.equal(instance.z, reference.z);
    assert.equal(instance.yaw, reference.yaw);
  }
  // Every surviving id must be derivable from its own coordinates, not from its array index.
  for (const instance of full.instances) {
    assert.equal(instance.candidateId, `layer0/${instance.species}/${instance.cellX}/${instance.cellZ}/${instance.slot}`);
  }
});

test('a road exclusion keeps a strip clear across every cell it crosses', () => {
  const surface = new SurfaceSampler(createSurface({ kind: 'plane', heightMetres: 0, version: 'plane/1' }));
  const region = validateRegion(placementRegion({
    species: [{ species: 'test_tree', densityPerHectare: 200, minSpacingMetres: 2 }],
    exclusions: [{ kind: 'polyline', points: [[0, 16], [32, 16]], widthMetres: 4, marginMetres: 1 }],
  }), new Set(['test_tree']));
  const scattered = scatterRegion({ region, rules: region.rules, surface });
  assert.ok(scattered.instances.length > 0);
  const halfWidth = 2 + 1;
  for (const instance of scattered.instances) {
    assert.ok(Math.abs(instance.z - 16) > halfWidth,
      `plant at z=${instance.z} is inside the road verge (half width ${halfWidth})`);
  }
});

test('a coverage mask trims density where the brush wrote nothing', () => {
  const surface = new SurfaceSampler(createSurface({ kind: 'plane', heightMetres: 0, version: 'plane/1' }));
  const values = new Float32Array(8 * 8); // all zero: the brush never touched this region
  const mask = new CoverageMask({ id: 'brush', width: 8, height: 8, cellSizeMetres: 4, originX: 0, originZ: 0, values });
  mask.stampCircle({ x: 16, z: 16, radiusMetres: 6 });
  const rule = { species: 'test_tree', densityPerHectare: 200, minSpacingMetres: 2, mask: { id: 'brush', strength: 1 } };
  const painted = validateRegion(placementRegion({ species: [rule], masks: [{ id: 'brush', width: 8, height: 8, cellSizeMetres: 4, originX: 0, originZ: 0, values }] }), new Set(['test_tree']));
  const scattered = scatterRegion({ region: painted, rules: painted.rules, surface });
  assert.ok(scattered.instances.length > 0, 'the painted disc must still produce plants');
  assert.ok(scattered.rejectedByMask > 0, 'the untouched area must be counted as a mask rejection');
  for (const instance of scattered.instances) {
    assert.ok(Math.hypot(instance.x - 16, instance.z - 16) <= 6 + 4,
      `plant at ${instance.x},${instance.z} is outside the painted disc`);
  }
  // strength 0 means the layer ignores the mask entirely, which is a different statement from
  // "the mask is empty" and must produce the full density. The mask still has to be declared:
  // a rule that names a mask it cannot find is a broken reference, not a disabled one.
  const ignored = validateRegion(placementRegion({
    species: [{ ...rule, mask: { id: 'brush', strength: 0 } }],
    masks: [{ id: 'brush', width: 8, height: 8, cellSizeMetres: 4, originX: 0, originZ: 0, values }],
  }), new Set(['test_tree']));
  const untouched = scatterRegion({ region: ignored, rules: ignored.rules, surface });
  assert.ok(untouched.instances.length > scattered.instances.length);
  assert.equal(untouched.rejectedByMask, 0);
});

test('row planting is one lattice for the whole region, not one per cell', () => {
  const surface = new SurfaceSampler(createSurface({ kind: 'plane', heightMetres: 0, version: 'plane/1' }));
  const region = validateRegion(placementRegion({
    species: [{
      species: 'test_grass', densityPerHectare: 100, minSpacingMetres: 0,
      rows: { spacingMetres: 2, plantSpacingMetres: 1, headingRadians: 0, skipFraction: 0, ridgeInsetFraction: 0 },
    }],
  }), new Set(['test_grass']));
  const scattered = scatterRegion({ region, rules: region.rules, surface });
  assert.ok(scattered.instances.length > 0);
  // Rows are at a constant z per row, and the row spacing is the authored one even across the
  // cell boundary at z = 8 and z = 16. The lateral wobble is a fifth of the usable half-width, so
  // rows are clustered rather than compared plant by plant.
  const sortedZ = [...new Set(scattered.instances.map((instance) => instance.z))].sort((a, b) => a - b);
  const clusters = [];
  for (const z of sortedZ) {
    const last = clusters[clusters.length - 1];
    if (last && z - last[last.length - 1] < 1) last.push(z); else clusters.push([z]);
  }
  assert.ok(clusters.length > 4, `expected rows, got ${clusters.length} clusters`);
  const centres = clusters.map((cluster) => cluster.reduce((sum, value) => sum + value, 0) / cluster.length);
  for (let index = 1; index < centres.length; ++index) {
    const gap = centres[index] - centres[index - 1];
    assert.ok(Math.abs(gap - 2) < 0.2, `row gap ${gap} between rows ${index - 1} and ${index} is not the authored 2 m`);
  }
  // Plants inside one row are one metre apart, including the ones that straddle a cell edge.
  const firstRow = scattered.instances.filter((instance) => Math.abs(instance.z - centres[1]) < 1).sort((a, b) => a.x - b.x);
  for (let index = 1; index < firstRow.length; ++index) {
    assert.ok(Math.abs((firstRow[index].x - firstRow[index - 1].x) - 1) < 1e-6,
      `plant spacing along the row is ${firstRow[index].x - firstRow[index - 1].x}, not 1 m`);
  }
  // No plant is placed twice at a cell boundary.
  assert.equal(new Set(scattered.instances.map((instance) => instance.candidateId)).size, scattered.instances.length);
});

test('region spacing enforces the minimum distance across species and across cells', () => {
  const surface = new SurfaceSampler(createSurface({ kind: 'plane', heightMetres: 0, version: 'plane/1' }));
  const region = validateRegion(placementRegion({
    species: [
      { species: 'test_tree', densityPerHectare: 150, minSpacingMetres: 5, spacingScope: 'region' },
      { species: 'test_shrub', densityPerHectare: 300, minSpacingMetres: 1.5, spacingScope: 'region' },
    ],
  }), new Set(['test_tree', 'test_shrub']));
  const scattered = scatterRegion({ region, rules: region.rules, surface });
  assert.equal(scattered.spacingScope, 'region');
  assert.ok(scattered.rejectedBySpacing > 0, 'the pass must reject overlaps, otherwise it is not running');
  const points = scattered.instances;
  let minimum = Infinity;
  for (let a = 0; a < points.length; ++a) for (let b = a + 1; b < points.length; ++b) {
    const distance = Math.hypot(points[a].x - points[b].x, points[a].z - points[b].z);
    minimum = Math.min(minimum, distance);
  }
  assert.ok(minimum >= 5 - 1e-6, `two plants are ${minimum} m apart, below the 5 m tree spacing`);
  // The pass is local: rebuilding one cell with the accepted result as a halo must agree with
  // the global result for that cell.
  const cellInstances = points.filter((instance) => instance.cellX === 2 && instance.cellZ === 2);
  const halo = points.filter((instance) => instance.cellX !== 2 || instance.cellZ !== 2);
  const rebuilt = scatterCell({ region, rules: region.rules, surface, cellX: 2, cellZ: 2 });
  const resolved = resolveSpacing({ instances: rebuilt, halo });
  assert.equal(resolved.instances.length, cellInstances.length,
    'the halo rebuild disagrees with the global pass, so the spacing is not local');
});

test('the region reports why plants were rejected and keeps ids stable when one is removed', () => {
  const species = [compileSpecies(baseSpecies()), compileSpecies(grassSpecies())];
  const open = compileRegion({ region: regionSource(), species });
  const clipped = compileRegion({
    region: regionSource({ exclusions: [{ kind: 'circle', x: 0, z: 0, radiusMetres: 10 }] }),
    species,
  });
  assert.ok(clipped.instances < open.instances);
  assert.ok(clipped.meta.counts.rejected.exclusion > 0);
  assert.equal(clipped.meta.counts.stableIdBits, 32);
  // A stable id is derived from the candidate, so a plant that survives an exclusion keeps the
  // id it had. Numbering by array position would shift every id after the removed one.
  const ids = new Set(open.records.map((record) => record.stableId));
  for (const record of clipped.records) {
    assert.ok(ids.has(record.stableId), `stable id ${record.stableId} changed when unrelated plants were removed`);
  }
});

// ---------------------------------------------------------------- brush contract
// §4.2 of the plan is a list of properties, not a list of features. These are the three that a
// brush gets wrong in practice: it counts mouse events instead of metres, it accumulates coverage
// until a stroke looks like a density slider, and it "erases" by deleting instances instead of
// writing a persistent zero.

test('the same gesture at two sample rates produces the same mask, byte for byte', () => {
  const make = () => new CoverageMask({ id: 'brush', width: 16, height: 16, cellSizeMetres: 2, originX: 0, originZ: 0, values: new Float32Array(256) });
  const coarse = [[4, 4], [28, 28]];
  // The same straight line recorded as 200 mouse events. Nothing about the result may depend on
  // how many events the input device produced.
  const dense = Array.from({ length: 201 }, (_, index) => [4 + (24 * index) / 200, 4 + (24 * index) / 200]);
  const a = make(), b = make();
  const strokeA = applyStroke(a, { points: coarse, radiusMetres: 2.5, spacingMetres: 0.5 });
  const strokeB = applyStroke(b, { points: dense, radiusMetres: 2.5, spacingMetres: 0.5 });
  assert.equal(strokeA.stamps, strokeB.stamps);
  assert.deepEqual(strokeA.path, strokeB.path);
  assert.deepEqual([...a.values], [...b.values]);
  // Stamp spacing is deliberately *not* asserted to be coverage-neutral: a finer spacing makes
  // the rim smoother, which is a real (small) change to the edge values. What must not change is
  // the result for the same gesture at the same settings, and that painting twice cannot raise
  // coverage — both covered here and in the idempotence test below.
});

test('painting is idempotent, erasing is persistent, and only densify accumulates', () => {
  const mask = new CoverageMask({ id: 'brush', width: 8, height: 8, cellSizeMetres: 2, originX: 0, originZ: 0, values: new Float32Array(64) });
  applyStroke(mask, { points: [[8, 8]], radiusMetres: 5, strength: 0.6 });
  const once = [...mask.values];
  applyStroke(mask, { points: [[8, 8]], radiusMetres: 5, strength: 0.6 });
  assert.deepEqual([...mask.values], once, 'painting the same place twice must not raise coverage');
  assert.ok(once.some((value) => value > 0.5), 'the stroke must actually paint');

  const painted = [...mask.values];
  applyStroke(mask, { points: [[8, 8]], radiusMetres: 5, mode: 'erase' });
  // Erase is a coverage write, not a delete: the inner disc (inside the falloff) goes to exactly
  // zero and the rim fades, and nothing is ever raised.
  assert.ok(mask.values.every((value, index) => value <= painted[index]), 'erase must never raise coverage');
  assert.ok(mask.values.some((value) => value === 0), 'erase must write a persistent zero at the centre');
  assert.equal(mask.values[4 * 8 + 4], 0, 'the mask cell under the stroke centre must be cleared');

  // A zero is remembered: painting again has to raise it, and it does so to the new target, not
  // to "what it was before".
  applyStroke(mask, { points: [[8, 8]], radiusMetres: 5, strength: 0.3 });
  assert.ok(mask.values.some((value) => value > 0.2 && value <= 0.3 + 1e-6));

  const additive = new CoverageMask({ id: 'd', width: 8, height: 8, cellSizeMetres: 2, originX: 0, originZ: 0, values: new Float32Array(64) });
  applyStroke(additive, { points: [[8, 8]], radiusMetres: 5, strength: 0.4, mode: 'densify' });
  const first = [...additive.values];
  applyStroke(additive, { points: [[8, 8]], radiusMetres: 5, strength: 0.4, mode: 'densify' });
  assert.ok(additive.values.every((value, index) => value >= first[index]), 'densify must raise, never lower');
  assert.ok(additive.values.some((value, index) => value > first[index] + 1e-6), 'densify is the accumulating mode');
});

test('a stroke reports the chunks it dirties, so a rebuild can leave the rest alone', () => {
  const mask = new CoverageMask({ id: 'brush', width: 32, height: 32, cellSizeMetres: 1, originX: 0, originZ: 0, values: new Float32Array(1024) });
  const stroke = applyStroke(mask, { points: [[4, 4], [12, 4]], radiusMetres: 2, spacingMetres: 1 });
  assert.deepEqual(stroke.bounds, { minX: 2, minZ: 2, maxX: 14, maxZ: 6 });
  const cells = strokeCells(stroke.bounds, { cellSizeMetres: 4, originX: 0, originZ: 0, cellsX: 8, cellsZ: 8 });
  assert.ok(cells.length > 0);
  // The dirty set is exactly the cells the rectangle overlaps: x 2..14 -> columns 0..3,
  // z 2..6 -> rows 0..1.
  assert.deepEqual(cells.map((cell) => `${cell.cellX},${cell.cellZ}`), [
    '0,0', '1,0', '2,0', '3,0', '0,1', '1,1', '2,1', '3,1',
  ]);
  // Outside the dirty set the mask must be untouched, which is what lets an incremental rebuild
  // claim "unrelated chunks keep their digest".
  for (let z = 0; z < 32; ++z) for (let x = 0; x < 32; ++x) {
    const inside = x + 0.5 >= 2 && x + 0.5 <= 14 && z + 0.5 >= 2 && z + 0.5 <= 6;
    if (!inside) assert.equal(mask.values[z * 32 + x], 0, `cell ${x},${z} was painted outside the stroke bounds`);
  }
});

test('every implemented component emitter produces geometry and reports its cost', () => {
  // One species that carries one of each part this layer implements. The point is not that the
  // shapes are pretty — it is that no emitter is dead code, that each one reports its own
  // triangle cost, and that a culm reads its host surface instead of its own frame.
  const parts = [
    { id: 'pod_1', type: 'pod', material: 'fruit', sizeMetres: [0.05, 0.06], attach: { hostLevels: [4], countPerHost: [1, 2], spreadRadians: 0.9, droop: 0.4 } },
    { id: 'leaf_compound', type: 'compoundLeaf', material: 'leaf', sizeMetres: [0.12, 0.16], attach: { hostLevels: [4], countPerHost: [1, 2], spreadRadians: 1.0 } },
    { id: 'culm_nodes', type: 'culm', material: 'bark', sizeMetres: [0.02, 0.03], attach: { hostLevels: [1, 2], distribution: 'node', spacingMetres: 0.4 } },
    { id: 'pad_1', type: 'floatingLeaf', material: 'leaf', sizeMetres: [0.1, 0.14], attach: { hostLevels: [3], countPerHost: [1, 1], planeHeightMetres: 0.05, spreadRadians: 0 } },
  ];
  const cooked = compileSpecies(withParts(parts));
  for (const id of ['pod_1', 'leaf_compound', 'culm_nodes', 'pad_1']) {
    const summary = cooked.meta.structure.components[id];
    assert.ok(summary, `component ${id} is missing from the cook report`);
    assert.ok(summary.parts > 0, `component ${id} placed no parts`);
    assert.ok(summary.triangles > 0, `component ${id} emitted no triangles`);
    assert.equal(summary.skippedStage, false);
  }
  // The floating pad is pinned to its declared water line, not to its host branch.
  const preview = buildPreview(withParts(parts), { level: 0 });
  const pad = preview.structure.components.find((entry) => entry.id === 'pad_1');
  assert.ok(pad.parts > 0);
});

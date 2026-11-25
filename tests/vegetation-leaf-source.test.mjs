import test from 'node:test';
import assert from 'node:assert/strict';
import { normalizeLeafSource, validateLeafSourceMetadata, LEAF_SOURCE_SCHEMA } from '../engine/vegetation/leaf-source.mjs';
import { buildAtlas, ATLAS_COORDINATES } from '../engine/vegetation/atlas.mjs';
import { placeLeaves, emitLeafBlade } from '../engine/vegetation/leaf-placement.mjs';
import { evaluateBranchGraph } from '../engine/vegetation/branch-graph.mjs';
import { defaultGenerators, normalizeGenerators } from '../engine/vegetation/generators.mjs';

const SIZE = 256;

// A synthetic single leaf: an obovate blade whose base sits at y = 0.8·size and whose tip is at
// 0.16·size, with real transparent margins and a separate opacity sheet. Every map is uniform so a
// rotation/mirror of the normal vector can be checked exactly.
function makeLeaf() {
  const baseColor = new Uint8Array(SIZE * SIZE * 4);
  const opacity = new Uint8Array(SIZE * SIZE * 4);
  const normal = new Uint8Array(SIZE * SIZE * 4);
  const root = { x: SIZE * 0.5, y: SIZE * 0.8 };
  const tipY = SIZE * 0.16;
  const halfWidth = SIZE * 0.22;
  const half = SIZE / 2;
  const nx = 0.6, ny = 0.2, nz = Math.sqrt(1 - nx * nx - ny * ny);
  for (let y = 0; y < SIZE; ++y) {
    for (let x = 0; x < SIZE; ++x) {
      const t = (root.y - y) / (root.y - tipY);
      if (t < 0 || t > 1) continue;
      const width = halfWidth * Math.pow(Math.sin(Math.PI * Math.pow(t, 0.85)), 0.9);
      if (Math.abs(x + 0.5 - root.x) > width) continue;
      const index = (y * SIZE + x) * 4;
      baseColor[index] = 74; baseColor[index + 1] = 122; baseColor[index + 2] = 46; baseColor[index + 3] = 255;
      opacity[index] = 255; opacity[index + 1] = 255; opacity[index + 2] = 255; opacity[index + 3] = 255;
      // directX-: green stores +Y downward.
      normal[index] = Math.round((nx * 0.5 + 0.5) * 255);
      normal[index + 1] = Math.round((ny * 0.5 + 0.5) * 255);
      normal[index + 2] = Math.round((nz * 0.5 + 0.5) * 255);
      normal[index + 3] = 255;
    }
  }
  return { baseColor, opacity, normal, root };
}

// Rotate RGBA clockwise by a multiple of 90°, nearest neighbour. Returns the new buffer and the
// dimension swap. This is the *input* transform; the normalizer is asked to undo it.
function rotateRgba(pixels, width, height, degrees) {
  const quarter = ((degrees / 90) % 4 + 4) % 4;
  const swap = quarter % 2 === 1;
  const outWidth = swap ? height : width;
  const outHeight = swap ? width : height;
  const out = new Uint8Array(outWidth * outHeight * 4);
  for (let y = 0; y < outHeight; ++y) {
    for (let x = 0; x < outWidth; ++x) {
      // Inverse map from the rotated image back to the source so every output texel is filled.
      let px, py;
      if (quarter === 0) { px = x; py = y; }
      else if (quarter === 1) { px = y; py = height - 1 - x; }
      else if (quarter === 2) { px = width - 1 - x; py = height - 1 - y; }
      else { px = width - 1 - y; py = x; }
      const source = (py * width + px) * 4;
      const target = (y * outWidth + x) * 4;
      out[target] = pixels[source]; out[target + 1] = pixels[source + 1];
      out[target + 2] = pixels[source + 2]; out[target + 3] = pixels[source + 3];
    }
  }
  return { pixels: out, width: outWidth, height: outHeight };
}

function rotatePoint(point, width, height, degrees) {
  const quarter = ((degrees / 90) % 4 + 4) % 4;
  if (quarter === 0) return { ...point };
  if (quarter === 1) return { x: height - 1 - point.y, y: point.x };
  if (quarter === 2) return { x: width - 1 - point.x, y: height - 1 - point.y };
  return { x: point.y, y: width - 1 - point.x };
}

function metadataFor({ width, height, root, degrees = 0, leafProfile = 'bladeOnly', speciesId = 'synthetic_leaf' }) {
  return {
    schema: LEAF_SOURCE_SCHEMA,
    assetKind: 'leaf',
    leafProfile,
    speciesId,
    side: 'front',
    imageSize: [width, height],
    coordinates: { origin: 'bottomLeft', u: 'right', v: 'up' },
    rootPivotUV: [0.5, 0.0625],
    tipDirectionUV: [0, 1],
    bladeLengthMetres: 0.14,
    bladeWidthMetres: 0.08,
    safeMarginFraction: 0.0625,
    normalConvention: 'directX-',
    baseDamageState: 'healthy',
    orientation: { rotateClockwiseDegrees: degrees, mirrored: false },
    landmarks: { rootUV: [(root.x + 0.5) / width, 1 - (root.y + 0.5) / height] },
    maps: {
      baseColor: { file: 'leaf.basecolor.png', colorSpace: 'srgb' },
      opacity: { file: 'leaf.opacity.png', channel: 'R', colorSpace: 'linear' },
      normal: { file: 'leaf.normal.png', colorSpace: 'linear' },
    },
    provenance: { provider: 'synthetic-fixture', quality: 'measured-fixture' },
  };
}

function normalizeRotated(degrees) {
  const leaf = makeLeaf();
  const input = degrees === 0
    ? { pixels: leaf.baseColor, width: SIZE, height: SIZE }
    : rotateRgba(leaf.baseColor, SIZE, SIZE, degrees);
  const opacity = degrees === 0
    ? { pixels: leaf.opacity, width: SIZE, height: SIZE }
    : rotateRgba(leaf.opacity, SIZE, SIZE, degrees);
  const normal = degrees === 0
    ? { pixels: leaf.normal, width: SIZE, height: SIZE }
    : rotateRgba(leaf.normal, SIZE, SIZE, degrees);
  const root = rotatePoint(leaf.root, SIZE, SIZE, degrees);
  const metadata = metadataFor({
    width: input.width, height: input.height, root,
    degrees: (360 - degrees) % 360,
  });
  return normalizeLeafSource({
    maps: {
      baseColor: { ...input, pixels: input.pixels },
      opacity: { ...opacity, pixels: opacity.pixels },
      normal: { ...normal, pixels: normal.pixels },
    },
    metadata,
  });
}

function alphaCoverage(map) {
  let covered = 0;
  for (let index = 3; index < map.pixels.length; index += 4) if (map.pixels[index] >= 128) ++covered;
  return covered / (map.width * map.height);
}

function meanNormal(map) {
  let r = 0, g = 0, b = 0, count = 0;
  for (let index = 0; index < map.pixels.length; index += 4) {
    if (map.pixels[index + 3] < 128) continue;
    r += map.pixels[index]; g += map.pixels[index + 1]; b += map.pixels[index + 2]; ++count;
  }
  return count ? { r: r / count, g: g / count, b: b / count } : null;
}

test('a leaf normalizes to the canonical root slot from every quarter turn', () => {
  const results = [];
  for (const degrees of [0, 90, 180, 270]) {
    const result = normalizeRotated(degrees);
    const meta = result.metadata;
    assert.equal(meta.schema, LEAF_SOURCE_SCHEMA);
    assert.equal(meta.schemaVersion, 2);
    assert.ok(meta.validation.rootErrorPixels <= 1.5, `root is ${meta.validation.rootErrorPixels} px from the contract pivot`);
    const rootPx = [meta.rootPivotUV[0] * meta.imageSize[0], (1 - meta.rootPivotUV[1]) * meta.imageSize[1]];
    assert.ok(Math.abs(rootPx[1] - 960) <= 1.5, `root sits at y=${rootPx[1]}, expected 960 of 1024`);
    assert.ok(Math.abs(meta.tipUV[0] - 0.5) < 0.06, `tip u=${meta.tipUV[0]} is not above the root`);
    assert.ok(meta.tipUV[1] > 0.9, `tip v=${meta.tipUV[1]} is not near the top of the canonical sheet`);
    // Safe margins are a hard contract, not a preference. The bottom edge is the root: a resampled
    // source texel centred on it can cover a few canonical pixels below the line, so the tolerance
    // is stated as the same footprint the normalizer uses.
    assert.ok(meta.contentBoundsUV.u0 >= 0.0625 - 0.006 && meta.contentBoundsUV.u1 <= 0.9375 + 0.006);
    assert.ok(meta.contentBoundsUV.v0 >= 0.0625 - 0.006 && meta.contentBoundsUV.v1 <= 0.9375 + 0.006);
    assert.ok(meta.pixelsPerMetre > 0);
    results.push(meta);
  }
  // The same leaf from all four input rotations must land on the same canonical rect and coverage.
  const reference = results[0];
  for (const meta of results.slice(1)) {
    for (const key of ['u0', 'v0', 'u1', 'v1']) {
      assert.ok(Math.abs(meta.contentBoundsUV[key] - reference.contentBoundsUV[key]) <= 0.01,
        `rotation moved content ${key} from ${reference.contentBoundsUV[key]} to ${meta.contentBoundsUV[key]}`);
    }
  }
  const coverages = results.map((_, index) => alphaCoverage(normalizeRotated(index * 90).maps.baseColor));
  for (const coverage of coverages) {
    assert.ok(Math.abs(coverage - coverages[0]) < 0.03,
      `coverage changed with rotation: ${coverages.map((value) => value.toFixed(3)).join(', ')}`);
  }

  // With no landmark the root is inferred from the bottom band. It must be labelled as inference:
  // a production import is allowed to accept it, but it may not claim a declared landmark.
  const leaf = makeLeaf();
  const inferredMetadata = metadataFor({ width: SIZE, height: SIZE, root: leaf.root });
  delete inferredMetadata.landmarks;
  const inferred = normalizeLeafSource({
    maps: { baseColor: { pixels: leaf.baseColor, width: SIZE, height: SIZE }, opacity: { pixels: leaf.opacity, width: SIZE, height: SIZE } },
    metadata: inferredMetadata,
  });
  assert.equal(inferred.metadata.transform.rootMethod, 'inferred-bottom-band');
  assert.ok(inferred.metadata.transform.rootConfidence > 0.5,
    `the fixture root could only be inferred with confidence ${inferred.metadata.transform.rootConfidence}`);
  assert.equal(inferred.metadata.validation.inference, 'inferred');
  assert.ok(inferred.metadata.validation.rootErrorPixels <= 1.5);
});

test('normal maps rotate and mirror as vectors, not as colour', () => {
  // The source stores +X in UV space, directX- green. Each case states what the vector must become
  // after the normalization transform; a colour-pixel rotation would keep r ≈ 0.8 in every case.
  const expectations = [
    // `degrees` is the rotation of the *input*; the normalizer applies 360-degrees to undo it.
    { degrees: 0, mirrored: false, want: { r: 0.8, g: 0.6 } },
    { degrees: 0, mirrored: true, want: { r: 0.2, g: 0.6 } },
    { degrees: 90, mirrored: false, want: { r: 0.6, g: 0.2 } },
    { degrees: 180, mirrored: false, want: { r: 0.2, g: 0.4 } },
    { degrees: 270, mirrored: false, want: { r: 0.4, g: 0.8 } },
  ];
  for (const entry of expectations) {
    const leaf = makeLeaf();
    let input = { pixels: leaf.baseColor, width: SIZE, height: SIZE };
    let opacity = { pixels: leaf.opacity, width: SIZE, height: SIZE };
    let normal = { pixels: leaf.normal, width: SIZE, height: SIZE };
    let root = { ...leaf.root };
    if (entry.mirrored) {
      for (const map of [input, opacity, normal]) {
        const pixels = new Uint8Array(map.pixels.length);
        for (let y = 0; y < SIZE; ++y) for (let x = 0; x < SIZE; ++x) {
          const source = (y * SIZE + (SIZE - 1 - x)) * 4;
          const target = (y * SIZE + x) * 4;
          for (let channel = 0; channel < 4; ++channel) pixels[target + channel] = map.pixels[source + channel];
        }
        map.pixels = pixels;
      }
      root.x = SIZE - 1 - root.x;
    }
    if (entry.degrees !== 0) {
      input = rotateRgba(input.pixels, SIZE, SIZE, entry.degrees);
      opacity = rotateRgba(opacity.pixels, SIZE, SIZE, entry.degrees);
      normal = rotateRgba(normal.pixels, SIZE, SIZE, entry.degrees);
      root = rotatePoint(root, SIZE, SIZE, entry.degrees);
    }
    const metadata = metadataFor({
      width: input.width, height: input.height, root,
      degrees: entry.mirrored ? 0 : (360 - entry.degrees) % 360,
    });
    metadata.orientation.mirrored = entry.mirrored;
    const result = normalizeLeafSource({ maps: { baseColor: input, opacity, normal }, metadata });
    const got = meanNormal(result.maps.normal);
    assert.ok(got, 'the normalized normal map has no coverage');
    // ±6/255 covers 8-bit encoding and one bilinear tap at the border.
    assert.ok(Math.abs(got.r - entry.want.r * 255) < 8,
      `rot=${entry.degrees} mirror=${entry.mirrored}: nx ${got.r.toFixed(1)} expected ${(entry.want.r * 255).toFixed(1)}`);
    assert.ok(Math.abs(got.g - entry.want.g * 255) < 8,
      `rot=${entry.degrees} mirror=${entry.mirrored}: ny(green) ${got.g.toFixed(1)} expected ${(entry.want.g * 255).toFixed(1)}`);
  }
});

test('the normalizer refuses cut-off, multi-subject and metadata-invalid sources', () => {
  const leaf = makeLeaf();
  const maps = {
    baseColor: { pixels: leaf.baseColor, width: SIZE, height: SIZE },
    opacity: { pixels: leaf.opacity, width: SIZE, height: SIZE },
  };
  // Cut off: a copy of the leaf sitting on the bottom row.
  const cut = new Uint8Array(SIZE * SIZE * 4);
  const shift = Math.round(SIZE * 0.8);
  cut.set(leaf.baseColor.subarray(0, leaf.baseColor.length - shift * SIZE * 4), shift * SIZE * 4);
  assert.throws(() => normalizeLeafSource({
    maps: { baseColor: { pixels: cut, width: SIZE, height: SIZE } },
    metadata: metadataFor({ width: SIZE, height: SIZE, root: { x: SIZE / 2, y: SIZE - 1 } }),
  }), /touches the image edge/);

  // Two separate leaves.
  const two = new Uint8Array(leaf.baseColor);
  for (let y = 80; y < 140; ++y) for (let x = 24; x < 64; ++x) {
    const index = (y * SIZE + x) * 4;
    two[index] = 10; two[index + 1] = 80; two[index + 2] = 10; two[index + 3] = 255;
  }
  assert.throws(() => normalizeLeafSource({
    maps: { baseColor: { pixels: two, width: SIZE, height: SIZE } },
    metadata: metadataFor({ width: SIZE, height: SIZE, root: leaf.root }),
  }), /separate opaque subjects/);

  // Empty.
  assert.throws(() => normalizeLeafSource({
    maps: { baseColor: { pixels: new Uint8Array(SIZE * SIZE * 4), width: SIZE, height: SIZE } },
    metadata: metadataFor({ width: SIZE, height: SIZE, root: leaf.root }),
  }), /no opaque pixels/);

  // Map size mismatch (a well-formed 128² normal sheet against a 256² base), and a damaged sheet.
  assert.throws(() => normalizeLeafSource({
    maps: { baseColor: { pixels: leaf.baseColor, width: SIZE, height: SIZE }, normal: { pixels: new Uint8Array(128 * 128 * 4), width: 128, height: 128 } },
    metadata: metadataFor({ width: SIZE, height: SIZE, root: leaf.root }),
  }), /share the baseColor dimensions/);
  const damaged = metadataFor({ width: SIZE, height: SIZE, root: leaf.root });
  damaged.baseDamageState = 'damaged';
  const validation = validateLeafSourceMetadata(damaged);
  assert.equal(validation.ok, false);
  assert.ok(validation.problems.some((problem) => problem.includes('baseDamageState')));
  assert.throws(() => normalizeLeafSource({ maps, metadata: damaged }), /baseDamageState/);
});

test('atlas pivots migrate by version instead of moving every existing leaf', () => {
  const legacy = buildAtlas({ columns: 2, rows: 2, width: 64, height: 64 });
  assert.equal(legacy.coordinatesVersion, ATLAS_COORDINATES.legacy);
  assert.equal(legacy.schemaVersion, 1);
  for (const cell of legacy.cells) assert.equal(cell.pivotV, cell.v0, 'legacy pivot must stay at the cell base');
  assert.equal(legacy.cells[0].contentU0, null, 'legacy cells have no content rect');

  const canonical = buildAtlas({ columns: 2, rows: 2, width: 64, height: 64, schemaVersion: 2 });
  assert.equal(canonical.coordinatesVersion, ATLAS_COORDINATES.canonical);
  for (const cell of canonical.cells) {
    assert.ok(Math.abs(cell.pivotV - (cell.v0 + cell.dv * 0.0625)) < 1e-12);
  }

  // A per-cell record may move its pivot and declare the leaf's real content rect.
  const perCell = buildAtlas({
    columns: 2, rows: 1, width: 64, height: 32, schemaVersion: 2,
    cells: [{ index: 1, pivot: [0.5, 0.1], contentBounds: { u0: 0.1, v0: 0.08, u1: 0.9, v1: 0.92 }, sourceId: 'oak-leaf-a' }],
  });
  const cell = perCell.cells[1];
  assert.ok(Math.abs(cell.pivotV - (cell.v0 + cell.dv * 0.1)) < 1e-12);
  assert.ok(Math.abs(cell.contentU0 - (cell.u0 + cell.du * 0.1)) < 1e-12);
  assert.ok(Math.abs(cell.contentV1 - (cell.v0 + cell.dv * 0.92)) < 1e-12);
  assert.equal(cell.sourceId, 'oak-leaf-a');
  assert.throws(() => buildAtlas({ columns: 1, rows: 1, width: 8, height: 8, schemaVersion: 2, cells: [{ index: 2 }] }), /outside the grid/);
  assert.throws(() => buildAtlas({ columns: 1, rows: 1, width: 8, height: 8, schemaVersion: 2, cells: [{ contentBounds: { u0: 0.5, v0: 0.1, u1: 0.4, v1: 0.9 } }] }), /contentBounds/);
});

test('a canonical cell maps blade UVs to its content rect, not its transparent margin', () => {
  const size = 64;
  const atlas = buildAtlas({
    columns: 1, rows: 1, width: size, height: size, schemaVersion: 2,
    cells: [{ index: 0, contentBounds: { u0: 0.1, v0: 0.07, u1: 0.9, v1: 0.93 } }],
    leafWidthMetres: 0.2, leafHeightMetres: 0.3,
  });
  const spec = {
    schema: 'nativegpu.vegetation-species/1', id: 'leaf_uv_fixture', kind: 'tree', profile: 'broadleaf', seed: 3,
    dimensions: { heightMetres: 6, crownRadiusMetres: 2.6, trunkRadiusMetres: 0.2, leafWidthMetres: 0.2, leafHeightMetres: 0.3 },
    lod: { levels: 3, screenErrorPixels: [0.4, 2.5, 9] },
    wind: { amplitudeMetres: 0.1, frequencyHz: 0.5, stiffness: 0.5 },
    materials: [
      { id: 'bark', kind: 'bark', alphaMasked: false, doubleSided: false, roughness: 0.8, thicknessMetres: 0.02, transmission: [0.05, 0.04, 0.02] },
      { id: 'leaf', kind: 'leaf', alphaMasked: true, doubleSided: true, roughness: 0.5, thicknessMetres: 0.0004, transmission: [0.24, 0.44, 0.11] },
    ],
    budget: { maxVertices: 160000, maxTriangles: 240000 },
    impostor: { views: 4, size: 32 },
  };
  const generators = normalizeGenerators({ generators: defaultGenerators(spec) });
  const graph = evaluateBranchGraph({ spec, generators, seed: spec.seed });
  const placed = placeLeaves({ graph, fronds: generators.fronds, atlas, spec, quality: { leaves: 1 }, seed: spec.seed });
  const leaf = placed.leaves.find((entry) => entry.cell !== null);
  assert.ok(leaf, 'no leaf was placed on the atlas');
  const cell = atlas.cells[leaf.cell];
  assert.ok(Math.abs(leaf.pivotUV[0] - cell.pivotU) < 1e-9 && Math.abs(leaf.pivotUV[1] - cell.pivotV) < 1e-9);

  const uvs = [];
  const mesh = {
    begin() {}, end() {},
    vertex(position, normal, uv) { uvs.push(uv); return uvs.length - 1; },
    triangle() {}, quad() {},
  };
  emitLeafBlade(mesh, leaf, { materialId: 'leaf', cell });
  const vs = uvs.map((uv) => uv[1]);
  const us = uvs.map((uv) => uv[0]);
  assert.ok(Math.abs(Math.min(...vs) - cell.pivotV) < 1e-9, `blade base v ${Math.min(...vs)} is not the pivot ${cell.pivotV}`);
  assert.ok(Math.abs(Math.max(...vs) - cell.contentV1) < 1e-9, `blade tip v ${Math.max(...vs)} is not the content top ${cell.contentV1}`);
  assert.ok(Math.min(...us) >= cell.contentU0 - 1e-9 && Math.max(...us) <= cell.contentU1 + 1e-9,
    `blade u spans ${Math.min(...us)}..${Math.max(...us)}, outside content ${cell.contentU0}..${cell.contentU1}`);
  // The content rect is narrower than the cell: this assertion fails if the margin is mapped again.
  assert.ok(cell.contentU1 - cell.contentU0 < cell.u1 - cell.u0 - 0.1);
});

import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {
  DATA_NAME, DEFAULT_BAND_VOXELS, ERROR_CODES, MANIFEST_NAME, SdfError,
  bakeSdf, loadSimpleMesh, loadVolume, readMeshFromPackage, sampleTrilinear, verifyVolume,
} from '../tools/offline/sdf-bake.mjs';
import { buildPackage } from '../tools/offline/geometry-package.mjs';

// Independent, procedurally generated analytic fixtures. The tool's own
// self-test uses different tessellation so a shared bug cannot hide here.
const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-sdf-test-'));
after(async () => { await fs.rm(temporary, { recursive: true, force: true }); });

const RESOLUTION = 32;
const BAND = DEFAULT_BAND_VOXELS;
const BAKE_OPTIONS = { resolution: RESOLUTION, bandVoxels: BAND };
// Stated contract from docs/SDF-BAKE.md: <= 1.5 x voxel diagonal in metres.
const TOLERANCE_VOXELS = 1.5 * Math.sqrt(3);
// Observed envelope of this implementation (measured max ~0.50 voxel for the
// box at 32^3); kept looser so the test catches regressions, not rounding.
const REGRESSION_MAX_ERROR_VOXELS = 1;

function sphereMesh(radius = 1, segments = 32, rings = 16) {
  const positions = [];
  const indices = [];
  const vertex = (x, y, z) => { positions.push(x, y, z); return positions.length / 3 - 1; };
  const north = vertex(0, radius, 0);
  const rows = [];
  for (let ring = 1; ring < rings; ring++) {
    const theta = (Math.PI * ring) / rings;
    const row = [];
    for (let segment = 0; segment < segments; segment++) {
      const phi = (2 * Math.PI * segment) / segments;
      row.push(vertex(
        radius * Math.sin(theta) * Math.sin(phi),
        radius * Math.cos(theta),
        radius * Math.sin(theta) * Math.cos(phi),
      ));
    }
    rows.push(row);
  }
  const south = vertex(0, -radius, 0);
  for (let segment = 0; segment < segments; segment++) {
    const next = (segment + 1) % segments;
    indices.push(north, rows[0][segment], rows[0][next]);
  }
  for (let ring = 0; ring < rows.length - 1; ring++) {
    for (let segment = 0; segment < segments; segment++) {
      const next = (segment + 1) % segments;
      const a = rows[ring][segment];
      const b = rows[ring][next];
      const c = rows[ring + 1][next];
      const d = rows[ring + 1][segment];
      indices.push(a, c, b, a, d, c);
    }
  }
  const last = rows.length - 1;
  for (let segment = 0; segment < segments; segment++) {
    indices.push(south, rows[last][(segment + 1) % segments], rows[last][segment]);
  }
  return { positions, indices };
}

function boxMesh(half = 1) {
  const signs = [
    [-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1],
    [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1],
  ];
  const positions = [];
  for (const [sx, sy, sz] of signs) positions.push(sx * half, sy * half, sz * half);
  const quads = [
    [1, 2, 6, 5],
    [0, 4, 7, 3],
    [3, 7, 6, 2],
    [0, 1, 5, 4],
    [4, 5, 6, 7],
    [0, 3, 2, 1],
  ];
  const indices = [];
  for (const [i0, i1, i2, i3] of quads) indices.push(i0, i1, i2, i0, i2, i3);
  return { positions, indices };
}

const sphereSdf = point => Math.hypot(point[0], point[1], point[2]) - 1;
const boxSdf = point => {
  const q = [Math.abs(point[0]) - 1, Math.abs(point[1]) - 1, Math.abs(point[2]) - 1];
  const outside = Math.hypot(Math.max(q[0], 0), Math.max(q[1], 0), Math.max(q[2], 0));
  return outside + Math.min(Math.max(q[0], q[1], q[2]), 0);
};

function measure(volume, analytic) {
  const { manifest, values } = volume;
  const [nx, ny, nz] = manifest.dimensions;
  const [ox, oy, oz] = manifest.origin;
  const h = manifest.voxelSize;
  const centerLimit = (BAND - 0.5) * h;
  let center = { count: 0, maxVoxels: 0, meanVoxels: 0 };
  let sum = 0;
  for (let iz = 0; iz < nz; iz++) {
    for (let iy = 0; iy < ny; iy++) {
      for (let ix = 0; ix < nx; ix++) {
        const reference = analytic([ox + ix * h, oy + iy * h, oz + iz * h]);
        if (Math.abs(reference) > centerLimit) continue;
        const error = Math.abs(values[(iz * ny + iy) * nx + ix] - reference) / h;
        center.maxVoxels = Math.max(center.maxVoxels, error);
        center.count++;
        sum += error;
      }
    }
  }
  center.meanVoxels = center.count === 0 ? 0 : sum / center.count;
  const midpointLimit = (BAND - 1) * h;
  let midpoint = { count: 0, maxVoxels: 0, meanVoxels: 0 };
  sum = 0;
  for (let iz = 0; iz < nz - 1; iz++) {
    for (let iy = 0; iy < ny - 1; iy++) {
      for (let ix = 0; ix < nx - 1; ix++) {
        const point = [ox + (ix + 0.5) * h, oy + (iy + 0.5) * h, oz + (iz + 0.5) * h];
        const reference = analytic(point);
        if (Math.abs(reference) > midpointLimit) continue;
        const error = Math.abs(sampleTrilinear(volume, point).value - reference) / h;
        midpoint.maxVoxels = Math.max(midpoint.maxVoxels, error);
        midpoint.count++;
        sum += error;
      }
    }
  }
  midpoint.meanVoxels = midpoint.count === 0 ? 0 : sum / midpoint.count;
  return { voxelSize: h, center, midpoint };
}

const sphereBake = await bakeSdf(sphereMesh(), path.join(temporary, 'sphere'), BAKE_OPTIONS);
const boxBake = await bakeSdf(boxMesh(), path.join(temporary, 'box'), BAKE_OPTIONS);
const sphereVolume = await loadVolume(sphereBake.manifestPath);
const boxVolume = await loadVolume(boxBake.manifestPath);

test('manifest declares the documented format, grid, band and chunk hashes', () => {
  const manifest = sphereVolume.manifest;
  assert.equal(manifest.formatVersion, 1);
  assert.equal(manifest.kind, 'nativegpu.sdf-volume');
  assert.equal(manifest.units, 'metres');
  assert.equal(manifest.signedConvention.inside, 'negative');
  assert.equal(manifest.signedConvention.outside, 'positive');
  assert.deepEqual(manifest.dimensions, [RESOLUTION, RESOLUTION, RESOLUTION]);
  assert.equal(manifest.voxelSize, 2 / (RESOLUTION - 2 * BAND));
  assert.deepEqual(manifest.origin, [0, 1, 2].map(() => -(RESOLUTION - 1) * manifest.voxelSize / 2));
  assert.deepEqual(manifest.band, { voxels: BAND, farValue: BAND * manifest.voxelSize, clamped: true });
  assert.equal(manifest.sign.watertight, true);
  assert.equal(manifest.sign.method, 'ray-parity');
  assert.equal(manifest.layout.format, 'float32le');
  assert.deepEqual(manifest.layout.chunkSize, [8, 8, 8]);
  assert.equal(manifest.layout.samplePosition, 'origin + voxelSize * (ix, iy, iz)');
  assert.equal(manifest.data.path, DATA_NAME);
  assert.equal(manifest.data.bytes, RESOLUTION ** 3 * 4);
  assert.match(manifest.data.sha256, /^[0-9a-f]{64}$/);
  assert.equal(manifest.source.kind, 'mesh');
  assert.equal(manifest.source.triangles, sphereMesh().indices.length / 3);
  assert.match(manifest.source.contentHash, /^[0-9a-f]{64}$/);
  assert.equal(manifest.chunks.length, (RESOLUTION / 8) ** 3);
  const surfaceVoxels = manifest.chunks.reduce((sum, chunk) => sum + chunk.surfaceVoxels, 0);
  assert.equal(surfaceVoxels, manifest.surface.surfaceVoxels);
  assert.ok(manifest.surface.surfaceVoxels <= manifest.surface.voxelsInBand);
  assert.equal(manifest.surface.outsideVoxels, RESOLUTION ** 3 - manifest.surface.insideVoxels);
  for (const [index, chunk] of manifest.chunks.entries()) {
    assert.equal(chunk.index, index);
    assert.equal(chunk.offset, index * 2048);
    assert.equal(chunk.bytes, 2048);
    assert.deepEqual(chunk.dimensions, [8, 8, 8]);
    assert.match(chunk.sha256, /^[0-9a-f]{64}$/);
  }
});

test('analytic sphere: sampled error stays within the stated tolerance', () => {
  const result = measure(sphereVolume, sphereSdf);
  console.log(`sphere: centre max ${result.center.maxVoxels.toFixed(4)} / mean ${result.center.meanVoxels.toFixed(4)} voxels over ${result.center.count}; midpoint max ${result.midpoint.maxVoxels.toFixed(4)} / mean ${result.midpoint.meanVoxels.toFixed(4)} over ${result.midpoint.count}`);
  assert.ok(result.center.count > 1000, 'expected enough in-band sphere samples');
  assert.ok(result.center.maxVoxels <= TOLERANCE_VOXELS, `centre error ${result.center.maxVoxels} exceeds 1.5 voxel diagonals`);
  assert.ok(result.midpoint.maxVoxels <= TOLERANCE_VOXELS, `trilinear error ${result.midpoint.maxVoxels} exceeds 1.5 voxel diagonals`);
  assert.ok(result.center.maxVoxels <= REGRESSION_MAX_ERROR_VOXELS);
  assert.ok(result.midpoint.maxVoxels <= REGRESSION_MAX_ERROR_VOXELS);
  assert.ok(sphereVolume.manifest.surface.voxelsInBand > 0);
});

test('analytic box: sampled error stays within the stated tolerance', () => {
  const result = measure(boxVolume, boxSdf);
  console.log(`box: centre max ${result.center.maxVoxels.toFixed(4)} / mean ${result.center.meanVoxels.toFixed(4)} voxels over ${result.center.count}; midpoint max ${result.midpoint.maxVoxels.toFixed(4)} / mean ${result.midpoint.meanVoxels.toFixed(4)} over ${result.midpoint.count}`);
  assert.ok(result.center.count > 1000, 'expected enough in-band box samples');
  assert.ok(result.center.maxVoxels <= TOLERANCE_VOXELS, `centre error ${result.center.maxVoxels} exceeds 1.5 voxel diagonals`);
  assert.ok(result.midpoint.maxVoxels <= TOLERANCE_VOXELS, `trilinear error ${result.midpoint.maxVoxels} exceeds 1.5 voxel diagonals`);
  assert.ok(result.center.maxVoxels <= REGRESSION_MAX_ERROR_VOXELS);
  assert.ok(result.midpoint.maxVoxels <= REGRESSION_MAX_ERROR_VOXELS);
});

test('sign convention: negative inside, positive outside, far field clamped', () => {
  for (const point of [[0, 0, 0], [0.3, 0, 0], [0, 0.6, 0], [0.5, 0.5, 0], [0, 0, 0.8]]) {
    const sample = sampleTrilinear(sphereVolume, point);
    assert.ok(sample.value < 0, `sphere point ${point} should be inside, got ${sample.value}`);
    assert.equal(sample.value < 0, sphereSdf(point) < 0);
  }
  for (const point of [[1.05, 0, 0], [1.1, 0, 0], [0, 1.1, 0], [2.5, 0, 0]]) {
    const sample = sampleTrilinear(sphereVolume, point);
    assert.ok(sample.value > 0, `sphere point ${point} should be outside, got ${sample.value}`);
  }
  for (const point of [[0, 0, 0], [0.5, 0.5, 0.5], [0.9, 0, 0], [0, 0.8, 0]]) {
    const sample = sampleTrilinear(boxVolume, point);
    assert.ok(sample.value < 0, `box point ${point} should be inside, got ${sample.value}`);
    assert.equal(sample.value < 0, boxSdf(point) < 0);
  }
  for (const point of [[1.05, 0, 0], [1.06, 0.5, 0], [0, 0, 1.05], [2.5, 0, 0]]) {
    const sample = sampleTrilinear(boxVolume, point);
    assert.ok(sample.value > 0, `box point ${point} should be outside, got ${sample.value}`);
  }
  const far = BAND * sphereVolume.manifest.voxelSize;
  const clamped = sampleTrilinear(sphereVolume, [0, 0, 0]);
  assert.ok(Math.abs(clamped.value + far) < 1e-6, `deep inside sphere should clamp to -farValue (${clamped.value} vs ${-far})`);
  const outsideGrid = sampleTrilinear(sphereVolume, [5, 0, 0]);
  assert.equal(outsideGrid.clamped, true);
  assert.ok(outsideGrid.value > 0);
  const exact = (() => {
    const manifest = sphereVolume.manifest;
    const [nx, ny] = manifest.dimensions;
    const [ox, oy, oz] = manifest.origin;
    const h = manifest.voxelSize;
    const ix = 9, iy = 16, iz = 20;
    const point = [ox + ix * h, oy + iy * h, oz + iz * h];
    return { value: sphereVolume.values[(iz * ny + iy) * nx + ix], sample: sampleTrilinear(sphereVolume, point).value };
  })();
  assert.ok(Math.abs(exact.sample - exact.value) < 1e-5, 'sampling at a voxel centre must return the stored value');
});

test('determinism: two bakes of the same mesh are byte-identical', async () => {
  const repeat = await bakeSdf(sphereMesh(), path.join(temporary, 'sphere-repeat'), BAKE_OPTIONS);
  assert.equal(repeat.volumeDigest, sphereBake.volumeDigest);
  assert.deepEqual(repeat.manifest, sphereVolume.manifest);
  assert.ok((await fs.readFile(repeat.manifestPath)).equals(await fs.readFile(sphereBake.manifestPath)));
  assert.ok((await fs.readFile(repeat.dataPath)).equals(await fs.readFile(sphereBake.dataPath)));
});

test('tamper detection reports NG-E-SDF-TAMPERED for data and manifest edits', async () => {
  const dataDirectory = path.join(temporary, 'tampered-data');
  await fs.cp(path.dirname(sphereBake.manifestPath), dataDirectory, { recursive: true });
  const dataPath = path.join(dataDirectory, DATA_NAME);
  const bytes = await fs.readFile(dataPath);
  bytes[0] ^= 0xff;
  await fs.writeFile(dataPath, bytes);
  await assert.rejects(verifyVolume(path.join(dataDirectory, MANIFEST_NAME)), error => {
    assert.ok(error instanceof SdfError);
    assert.equal(error.code, ERROR_CODES.TAMPERED);
    assert.equal(error.code, 'NG-E-SDF-TAMPERED');
    return true;
  });

  const manifestDirectory = path.join(temporary, 'tampered-manifest');
  await fs.cp(path.dirname(sphereBake.manifestPath), manifestDirectory, { recursive: true });
  const manifestPath = path.join(manifestDirectory, MANIFEST_NAME);
  const edited = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
  edited.origin[0] += 1;
  await fs.writeFile(manifestPath, `${JSON.stringify(edited, null, 2)}\n`, 'utf8');
  await assert.rejects(verifyVolume(manifestPath), error => {
    assert.ok(error instanceof SdfError);
    assert.equal(error.code, ERROR_CODES.TAMPERED);
    return true;
  });

  const missingDirectory = path.join(temporary, 'missing-data');
  await fs.cp(path.dirname(sphereBake.manifestPath), missingDirectory, { recursive: true });
  await fs.rm(path.join(missingDirectory, DATA_NAME));
  await assert.rejects(verifyVolume(path.join(missingDirectory, MANIFEST_NAME)), error => {
    assert.equal(error.code, ERROR_CODES.MISSING);
    return true;
  });
});

test('simple JSON mesh input bakes the same field as the in-memory mesh', async () => {
  const meshFile = path.join(temporary, 'box.mesh.json');
  await fs.writeFile(meshFile, `${JSON.stringify({ positions: boxMesh().positions, indices: boxMesh().indices })}\n`, 'utf8');
  const loaded = await loadSimpleMesh(meshFile);
  const baked = await bakeSdf(loaded, path.join(temporary, 'box-json'), BAKE_OPTIONS);
  const verified = await verifyVolume(baked.manifestPath);
  assert.equal(verified.ok, true);
  assert.equal(verified.source.kind, 'mesh-json');
  const volume = await loadVolume(baked.manifestPath);
  for (const point of [[0, 0, 0], [0.5, 0.5, 0.5], [1.05, 0, 0], [2.5, 0, 0]]) {
    assert.ok(Math.abs(sampleTrilinear(volume, point).value - sampleTrilinear(boxVolume, point).value) < 1e-6);
  }
});

test('geometry package reader reuses the INN-14 verified package', async () => {
  const packageDirectory = path.join(temporary, 'geometry-package');
  const built = await buildPackage(packageDirectory);
  const mesh = await readMeshFromPackage(built.manifestPath);
  assert.equal(mesh.source.kind, 'geometry-package');
  assert.equal(mesh.source.contentHash, built.manifest.source.contentHash);
  assert.equal(mesh.source.triangles, built.manifest.counts.sourceTriangles);
  const baked = await bakeSdf(mesh, path.join(temporary, 'package-sdf'), BAKE_OPTIONS);
  const verified = await verifyVolume(baked.manifestPath);
  assert.equal(verified.ok, true);
  assert.equal(verified.source.kind, 'geometry-package');
  assert.equal(verified.source.packageDigest, built.packageDigest);
  assert.equal(verified.source.triangles, built.manifest.counts.sourceTriangles);
});

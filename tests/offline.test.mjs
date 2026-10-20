import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {
  ERROR_CODES, GeometryError, MAX_CLUSTER_TRIANGLES, MAX_CLUSTER_VERTICES,
  buildPackage, computePackageDigest, verifyPackage,
} from '../tools/offline/geometry-package.mjs';
import { buildClusterDag, clusterStats, emitClusterDag } from '../tools/offline/cluster-dag.mjs';

const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-offline-test-'));
after(() => fs.rm(temporary, { recursive: true, force: true }));
const packageDirectory = path.join(temporary, 'package');
const built = await buildPackage(packageDirectory);
const manifest = built.manifest;

async function section(name) {
  const entry = manifest.sections.find(item => item.name === name);
  return fs.readFile(path.join(packageDirectory, entry.path));
}

function floats(buffer) {
  return new Float32Array(buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength));
}

function uints(buffer) {
  return new Uint32Array(buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength));
}

function vertexAt(vertices, index) {
  return [vertices[index * 6], vertices[index * 6 + 1], vertices[index * 6 + 2],
    vertices[index * 6 + 3], vertices[index * 6 + 4], vertices[index * 6 + 5]];
}

function vertexKey(vector) {
  return vector.join(',');
}

function triangleKey(vertices, indices, triangle) {
  return [0, 1, 2].map(offset => vertexKey(vertexAt(vertices, indices[triangle * 3 + offset]))).join('|');
}

test('deterministic rebuild: two builds produce byte-identical packages', async () => {
  const otherDirectory = path.join(temporary, 'package-repeat');
  const other = await buildPackage(otherDirectory);
  assert.equal(other.packageDigest, built.packageDigest);
  assert.equal(computePackageDigest(manifest), manifest.packageDigest);
  assert.ok((await fs.readFile(built.manifestPath)).equals(await fs.readFile(other.manifestPath)));
  for (const entry of manifest.sections) {
    const left = await fs.readFile(path.join(packageDirectory, entry.path));
    const right = await fs.readFile(path.join(otherDirectory, entry.path));
    assert.ok(left.equals(right), `section ${entry.name} should be byte-identical`);
  }
});

test('LOD0 clusters cover every source triangle exactly once', async () => {
  const sourceVertices = floats(await section('vertices'));
  const sourceIndices = uints(await section('indices'));
  const clusterVertices = floats(await section('cluster-vertices'));
  const clusterIndices = uints(await section('cluster-indices'));
  const lod0 = manifest.clusters.filter(cluster => cluster.lod === 0)
    .sort((left, right) => left.sourceTriangleStart - right.sourceTriangleStart);
  let cursor = 0;
  for (const cluster of lod0) {
    assert.equal(cluster.sourceTriangleStart, cursor, 'LOD0 triangle ranges must be contiguous');
    assert.equal(cluster.triangleCount, cluster.sourceTriangleCount);
    assert.ok(cluster.vertexCount <= MAX_CLUSTER_VERTICES);
    assert.ok(cluster.triangleCount <= MAX_CLUSTER_TRIANGLES);
    for (let triangle = 0; triangle < cluster.triangleCount; triangle++) {
      const local = [0, 1, 2].map(offset => clusterIndices[cluster.indexOffset + triangle * 3 + offset]);
      const localKey = local.map(index => vertexKey(vertexAt(clusterVertices, cluster.vertexOffset + index))).join('|');
      assert.equal(localKey, triangleKey(sourceVertices, sourceIndices, cursor + triangle));
    }
    cursor += cluster.triangleCount;
  }
  assert.equal(cursor, manifest.counts.sourceTriangles);
});

test('every cluster bounds is a valid AABB containing its vertices', async () => {
  const clusterVertices = floats(await section('cluster-vertices'));
  for (const cluster of manifest.clusters) {
    assert.ok(cluster.vertexCount <= MAX_CLUSTER_VERTICES);
    assert.ok(cluster.triangleCount <= MAX_CLUSTER_TRIANGLES);
    const min = [Infinity, Infinity, Infinity];
    const max = [-Infinity, -Infinity, -Infinity];
    for (let index = cluster.vertexOffset; index < cluster.vertexOffset + cluster.vertexCount; index++) {
      for (let axis = 0; axis < 3; axis++) {
        const value = clusterVertices[index * 6 + axis];
        min[axis] = Math.min(min[axis], value);
        max[axis] = Math.max(max[axis], value);
      }
    }
    for (let axis = 0; axis < 3; axis++) {
      assert.ok(cluster.bounds.min[axis] <= min[axis] + 1e-6);
      assert.ok(cluster.bounds.max[axis] >= max[axis] - 1e-6);
    }
  }
  for (const cluster of manifest.clusters) {
    for (const childId of cluster.children) {
      const child = manifest.clusters[childId];
      for (let axis = 0; axis < 3; axis++) {
        assert.ok(cluster.bounds.min[axis] <= child.bounds.min[axis]);
        assert.ok(cluster.bounds.max[axis] >= child.bounds.max[axis]);
      }
    }
  }
});

test('tamper detection reports the stable error code', async () => {
  const sectionDirectory = path.join(temporary, 'tampered-section');
  await fs.cp(packageDirectory, sectionDirectory, { recursive: true });
  const sectionManifest = path.join(sectionDirectory, 'geometry.package.json');
  const parsed = JSON.parse(await fs.readFile(sectionManifest, 'utf8'));
  const vertices = parsed.sections.find(item => item.name === 'vertices');
  const file = path.join(sectionDirectory, vertices.path);
  const bytes = await fs.readFile(file);
  bytes[16] ^= 0xff;
  await fs.writeFile(file, bytes);
  await assert.rejects(verifyPackage(sectionManifest), error => {
    assert.ok(error instanceof GeometryError);
    assert.equal(error.code, ERROR_CODES.TAMPERED);
    return true;
  });

  const manifestDirectory = path.join(temporary, 'tampered-manifest');
  await fs.cp(packageDirectory, manifestDirectory, { recursive: true });
  const manifestFile = path.join(manifestDirectory, 'geometry.package.json');
  const edited = JSON.parse(await fs.readFile(manifestFile, 'utf8'));
  edited.clusters[0].bounds.max[0] += 1;
  await fs.writeFile(manifestFile, `${JSON.stringify(edited, null, 2)}\n`, 'utf8');
  await assert.rejects(verifyPackage(manifestFile), error => {
    assert.ok(error instanceof GeometryError);
    assert.equal(error.code, ERROR_CODES.TAMPERED);
    return true;
  });
});

test('DAG is acyclic, levels are contiguous and strictly shrinking', () => {
  assert.equal(manifest.lodLevels[0].level, 0);
  for (let index = 0; index < manifest.lodLevels.length; index++) {
    const level = manifest.lodLevels[index];
    assert.equal(level.level, index);
    if (index > 0) {
      assert.ok(level.clusterCount < manifest.lodLevels[index - 1].clusterCount);
      assert.equal(level.clusterStart, manifest.lodLevels[index - 1].clusterStart
        + manifest.lodLevels[index - 1].clusterCount);
    } else {
      assert.equal(level.clusterStart, 0);
    }
  }
  const last = manifest.lodLevels.at(-1);
  assert.equal(last.clusterStart + last.clusterCount, manifest.clusters.length);
  const roots = manifest.clusters.filter(cluster => cluster.parent === -1);
  assert.equal(roots.length, 1);
  assert.equal(roots[0].lod, last.level);
  for (const [index, cluster] of manifest.clusters.entries()) {
    assert.equal(cluster.id, index);
    if (cluster.lod === 0) {
      assert.deepEqual(cluster.children, []);
    } else {
      assert.ok(cluster.children.length >= 1 && cluster.children.length <= 2);
      for (const childId of cluster.children) {
        assert.equal(manifest.clusters[childId].parent, cluster.id);
        assert.equal(manifest.clusters[childId].lod, cluster.lod - 1);
        assert.ok(childId < cluster.id);
      }
    }
    const seen = new Set();
    let current = cluster;
    while (current.parent !== -1) {
      assert.ok(!seen.has(current.id), `cycle at cluster ${current.id}`);
      seen.add(current.id);
      current = manifest.clusters[current.parent];
    }
  }
});

test('cluster-dag sidecar and stats report honest per-LOD ranges', async () => {
  const stats = clusterStats(manifest);
  assert.equal(stats.clusters, manifest.clusters.length);
  assert.equal(stats.totalVertices, manifest.clusters.reduce((sum, cluster) => sum + cluster.vertexCount, 0));
  assert.equal(stats.totalTriangles, manifest.clusters.reduce((sum, cluster) => sum + cluster.triangleCount, 0));
  assert.deepEqual(stats.lodLevels.map(level => level.clusterCount), manifest.lodLevels.map(level => level.clusterCount));
  assert.equal(stats.runtimeLodSelector, false);

  const sidecar = buildClusterDag(manifest);
  assert.equal(sidecar.kind, 'nativegpu.cluster-dag');
  assert.equal(sidecar.package.packageDigest, manifest.packageDigest);
  assert.equal(sidecar.runtimeLodSelector, false);
  assert.equal(sidecar.streaming, false);
  assert.equal(sidecar.roots.length, 1);
  assert.equal(sidecar.clusters.length, manifest.clusters.length);

  const output = path.join(temporary, 'cluster-dag.json');
  const emitted = await emitClusterDag(built.manifestPath, output);
  const parsed = JSON.parse(await fs.readFile(output, 'utf8'));
  assert.deepEqual(parsed, emitted.sidecar);
  assert.deepEqual(parsed.lodLevels.map(level => level.clusterCount), manifest.lodLevels.map(level => level.clusterCount));
});

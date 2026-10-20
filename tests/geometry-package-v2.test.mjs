import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {
  CAPABILITIES, CLUSTER_FLAG_BITS, ERROR_CODES, FORMAT_VERSION, GeometryError,
  boundsDiagonal, buildPackage, computeLodError, computePackageDigest, decodeMaterialHandle,
  encodeMaterialHandle, readPackage, resolveSourceMetadata, stableJSON, verifyPackage,
} from '../tools/offline/geometry-package.mjs';
import { buildClusterDag, clusterStats, emitClusterDag } from '../tools/offline/cluster-dag.mjs';

const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-geometry-v2-test-'));
after(() => fs.rm(temporary, { recursive: true, force: true }));

const packageDirectory = path.join(temporary, 'package');
const built = await buildPackage(packageDirectory);
const manifest = built.manifest;
const manifestFile = path.join(packageDirectory, 'geometry.package.json');

async function copyPackage(name) {
  const directory = path.join(temporary, name);
  await fs.cp(packageDirectory, directory, { recursive: true });
  return { directory, file: path.join(directory, 'geometry.package.json') };
}

async function writeManifest(file, value) {
  await fs.writeFile(file, `${stableJSON(value)}\n`, 'utf8');
}

function clusterAt(lod) {
  const cluster = manifest.clusters.find(item => item.lod === lod);
  assert.ok(cluster, `expected a cluster at LOD ${lod}`);
  return cluster;
}

// The old v1 writer wrote a manifest without capabilities and without the three
// v2 cluster fields. Recreate exactly that shape from the v2 build (sections are
// unchanged by the new declarative fields) and give it its own v1 digest.
function downgradeToV1(source) {
  const clone = JSON.parse(JSON.stringify(source));
  clone.formatVersion = 1;
  delete clone.capabilities;
  for (const cluster of clone.clusters) {
    delete cluster.lodError;
    delete cluster.flags;
    delete cluster.materialHandle;
  }
  clone.packageDigest = computePackageDigest(clone);
  return clone;
}

test('v2 manifest declares capabilities and every cluster carries the resolved fields', () => {
  assert.equal(FORMAT_VERSION, 2);
  assert.equal(manifest.formatVersion, 2);
  assert.deepEqual(manifest.capabilities, {
    clusterPipeline: true,
    lodError: true,
    materialHandles: true,
    flags: true,
  });
  const maxLod = manifest.lodLevels.length - 1;
  assert.ok(maxLod >= 1, 'the procedural source must produce multiple LOD levels');
  for (const cluster of manifest.clusters) {
    assert.ok(Object.hasOwn(cluster, 'lodError'), `cluster ${cluster.id} must carry lodError`);
    assert.ok(Object.hasOwn(cluster, 'flags'), `cluster ${cluster.id} must carry flags`);
    assert.ok(Object.hasOwn(cluster, 'materialHandle'), `cluster ${cluster.id} must carry materialHandle`);
    assert.ok(Number.isFinite(cluster.lodError) && cluster.lodError >= 0);
    assert.equal(cluster.flags, 0, 'procedural input declares no flags');
    assert.equal(cluster.materialHandle, 0, 'procedural input uses the default material');
    assert.equal(cluster.lodError, computeLodError(cluster.bounds, cluster.lod, maxLod));
    assert.equal(cluster.lodError, cluster.lod === 0 ? 0 : boundsDiagonal(cluster.bounds) / maxLod);
    if (cluster.parent !== -1) {
      assert.ok(manifest.clusters[cluster.parent].lodError >= cluster.lodError,
        `parent of cluster ${cluster.id} must not have a smaller lodError`);
    }
  }
});

test('deterministic rebuild is byte-identical and the digest covers the v2 fields', async () => {
  const otherDirectory = path.join(temporary, 'package-repeat');
  const other = await buildPackage(otherDirectory);
  assert.equal(other.packageDigest, manifest.packageDigest);
  assert.equal(computePackageDigest(manifest), manifest.packageDigest);
  assert.ok((await fs.readFile(manifestFile)).equals(await fs.readFile(other.manifestPath)));
  for (const section of manifest.sections) {
    const left = await fs.readFile(path.join(packageDirectory, section.path));
    const right = await fs.readFile(path.join(otherDirectory, section.path));
    assert.ok(left.equals(right), `section ${section.name} should be byte-identical`);
  }

  const mutations = [
    ['lodError', value => { value.clusters[clusterAt(1).id].lodError += 1; }],
    ['flags', value => { value.clusters[0].flags = 1; }],
    ['materialHandle', value => { value.clusters[0].materialHandle = 1; }],
  ];
  for (const [field, mutate] of mutations) {
    const changed = JSON.parse(JSON.stringify(manifest));
    mutate(changed);
    assert.notEqual(computePackageDigest(changed), manifest.packageDigest, `${field} must participate in the package digest`);
  }

  const tampered = await copyPackage('tampered-v2-field');
  const edited = JSON.parse(await fs.readFile(tampered.file, 'utf8'));
  edited.clusters[0].flags = 1;
  await writeManifest(tampered.file, edited);
  await assert.rejects(verifyPackage(tampered.file), error => {
    assert.ok(error instanceof GeometryError);
    assert.equal(error.code, ERROR_CODES.TAMPERED);
    return true;
  });
});

test('per-source metadata sets flags and the encoded materialHandle on every cluster', async () => {
  const allFlags = Object.fromEntries(Object.keys(CLUSTER_FLAG_BITS).map(name => [name, true]));
  const generation = 7;
  const sliceIndex = 42;
  const encoded = encodeMaterialHandle(generation, sliceIndex);
  assert.equal(encoded, (generation << 16) | sliceIndex);
  assert.deepEqual(decodeMaterialHandle(encoded), { generation, sliceIndex });
  assert.equal(encodeMaterialHandle(65535, 65535), 0xffffffff);
  assert.deepEqual(decodeMaterialHandle(0xffffffff), { generation: 65535, sliceIndex: 65535 });

  const metadataDirectory = path.join(temporary, 'package-metadata');
  const metadataBuilt = await buildPackage(metadataDirectory, {
    parameters: { gridQuads: 8 },
    sourceMetadata: { flags: allFlags, materialHandle: { generation, sliceIndex } },
  });
  for (const cluster of metadataBuilt.manifest.clusters) {
    assert.equal(cluster.flags, 0x3f);
    assert.equal(cluster.materialHandle, encoded);
  }
  const verified = await verifyPackage(metadataBuilt.manifestPath);
  assert.equal(verified.ok, true);
  assert.deepEqual(verified.manifest.clusters.map(cluster => cluster.materialHandle), metadataBuilt.manifest.clusters.map(() => encoded));
  assert.equal(verified.defaultsApplied, false);

  const flatDirectory = path.join(temporary, 'package-flat-metadata');
  const flatBuilt = await buildPackage(flatDirectory, { metadata: { skinned: true, materialHandle: 0 } });
  assert.ok(flatBuilt.manifest.clusters.every(cluster => cluster.flags === (1 << CLUSTER_FLAG_BITS.skinned)));
  assert.ok(flatBuilt.manifest.clusters.every(cluster => cluster.materialHandle === 0));

  assert.deepEqual(resolveSourceMetadata(undefined), { flags: 0, materialHandle: 0 });
  for (const [name, bit] of Object.entries(CLUSTER_FLAG_BITS)) {
    assert.deepEqual(resolveSourceMetadata({ flags: { [name]: true } }), { flags: 1 << bit, materialHandle: 0 });
  }
  assert.deepEqual(resolveSourceMetadata({ twoSided: 1, materialHandle: encoded }), { flags: 1 << 2, materialHandle: encoded });
  assert.deepEqual(resolveSourceMetadata({ flags: 0x21 }), { flags: 0x21, materialHandle: 0 });
  assert.throws(() => resolveSourceMetadata({ flags: 0x40 }), error => error instanceof GeometryError
    && error.code === ERROR_CODES.INPUT);

  for (const bad of [
    () => encodeMaterialHandle(65536, 0),
    () => encodeMaterialHandle(0, 65536),
    () => encodeMaterialHandle(-1, 0),
    () => encodeMaterialHandle(0, -1),
    () => encodeMaterialHandle(1.5, 0),
  ]) {
    assert.throws(bad, error => error instanceof GeometryError && error.code === ERROR_CODES.INPUT);
  }
  await assert.rejects(buildPackage(path.join(temporary, 'package-bad-handle'), {
    sourceMetadata: { materialHandle: { generation: 0, sliceIndex: 65536 } },
  }), error => error instanceof GeometryError && error.code === ERROR_CODES.INPUT);
  await assert.rejects(buildPackage(path.join(temporary, 'package-bad-flags'), {
    sourceMetadata: { flags: { skinned: 'yes' } },
  }), error => error instanceof GeometryError && error.code === ERROR_CODES.INPUT);
  await assert.rejects(buildPackage(path.join(temporary, 'package-bad-key'), {
    sourceMetadata: { unknownFlag: true },
  }), error => error instanceof GeometryError && error.code === ERROR_CODES.INPUT);
});

test('v1 packages verify against their own digest and read with documented defaults', async () => {
  const v1Manifest = downgradeToV1(manifest);
  const v1 = await copyPackage('package-v1');
  await writeManifest(v1.file, v1Manifest);

  assert.equal(v1Manifest.formatVersion, 1);
  assert.equal(computePackageDigest(v1Manifest), v1Manifest.packageDigest);
  assert.notEqual(v1Manifest.packageDigest, manifest.packageDigest, 'rebuilding the same geometry as v2 changes the digest');
  assert.deepEqual(v1Manifest.sections, manifest.sections, 'v2 did not change section bytes');

  const verified = await verifyPackage(v1.file);
  assert.equal(verified.ok, true);
  assert.equal(verified.formatVersion, 1);
  assert.equal(verified.packageDigest, v1Manifest.packageDigest);
  assert.equal(verified.defaultsApplied, true);
  assert.deepEqual(verified.defaultedFields, ['flags', 'lodError', 'materialHandle']);
  assert.equal(verified.defaultedClusters, v1Manifest.clusters.length);
  assert.ok(verified.rawManifest.clusters.every(cluster => !Object.hasOwn(cluster, 'lodError')));
  assert.ok(verified.manifest.clusters.every(cluster => cluster.lodError === 0 && cluster.flags === 0 && cluster.materialHandle === 0));

  const read = await readPackage(v1.file);
  assert.equal(read.formatVersion, 1);
  assert.equal(read.packageDigest, v1Manifest.packageDigest);
  assert.equal(read.defaultsApplied, true);
  assert.ok(read.manifest.clusters.every(cluster => cluster.lodError === 0 && cluster.flags === 0 && cluster.materialHandle === 0));

  const stats = clusterStats(read.manifest);
  assert.deepEqual(stats.lodLevels.map(level => level.lodError), read.manifest.lodLevels.map(() => ({ min: 0, max: 0 })));
  const sidecar = buildClusterDag(read.manifest);
  assert.ok(sidecar.clusters.every(cluster => cluster.lodError === 0 && cluster.flags === 0 && cluster.materialHandle === 0));
  assert.equal(sidecar.package.capabilities, null);
});

test('v2 with capabilities rejects missing cluster fields; transitional v2 reads as defaults', async () => {
  const strict = await copyPackage('package-strict-v2');
  for (const field of ['lodError', 'flags', 'materialHandle']) {
    const edited = JSON.parse(JSON.stringify(manifest));
    delete edited.clusters[0][field];
    edited.packageDigest = computePackageDigest(edited);
    await writeManifest(strict.file, edited);
    await assert.rejects(verifyPackage(strict.file), error => {
      assert.ok(error instanceof GeometryError);
      assert.equal(error.code, ERROR_CODES.MANIFEST);
      assert.match(error.message, new RegExp(field));
      return true;
    });
  }

  const transitionalManifest = downgradeToV1(manifest);
  transitionalManifest.formatVersion = 2;
  transitionalManifest.packageDigest = computePackageDigest(transitionalManifest);
  const transitional = await copyPackage('package-transitional-v2');
  await writeManifest(transitional.file, transitionalManifest);
  const verified = await verifyPackage(transitional.file);
  assert.equal(verified.ok, true);
  assert.equal(verified.formatVersion, 2);
  assert.equal(verified.defaultsApplied, true);
  assert.ok(verified.manifest.clusters.every(cluster => cluster.lodError === 0 && cluster.flags === 0 && cluster.materialHandle === 0));
});

test('cluster-dag sidecar emits the v2 fields and stats reports per-LOD lodError ranges', async () => {
  const sidecar = buildClusterDag(manifest);
  assert.deepEqual(sidecar.package.capabilities, CAPABILITIES);
  for (const [index, cluster] of manifest.clusters.entries()) {
    assert.equal(sidecar.clusters[index].lodError, cluster.lodError);
    assert.equal(sidecar.clusters[index].flags, cluster.flags);
    assert.equal(sidecar.clusters[index].materialHandle, cluster.materialHandle);
  }

  const stats = clusterStats(manifest);
  assert.equal(stats.formatVersion, 2);
  for (const [index, level] of manifest.lodLevels.entries()) {
    const errors = manifest.clusters.slice(level.clusterStart, level.clusterStart + level.clusterCount)
      .map(cluster => cluster.lodError);
    assert.deepEqual(stats.lodLevels[index].lodError, { min: Math.min(...errors), max: Math.max(...errors) });
    if (level.level === 0) assert.deepEqual(stats.lodLevels[index].lodError, { min: 0, max: 0 });
    else assert.ok(stats.lodLevels[index].lodError.max > 0);
  }

  const output = path.join(temporary, 'cluster-dag.json');
  const emitted = await emitClusterDag(built.manifestPath, output);
  const parsed = JSON.parse(await fs.readFile(output, 'utf8'));
  assert.deepEqual(parsed, emitted.sidecar);
  assert.ok(parsed.clusters.every(cluster => Number.isFinite(cluster.lodError)
    && Number.isInteger(cluster.flags) && Number.isInteger(cluster.materialHandle)));
});

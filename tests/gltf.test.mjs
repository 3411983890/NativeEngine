import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ERROR_CODES, GltfError, buildPackageFromMesh, createMeshDocument, load, meshFromDocument,
} from '../tools/offline/gltf.mjs';
import { computePackageDigest, verifyPackage } from '../tools/offline/geometry-package.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = path.join(here, '..', 'tools', 'offline', 'fixtures');
const quadPath = path.join(fixtures, 'quad.gltf');
const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-gltf-test-'));
after(() => fs.rm(temporary, { recursive: true, force: true }));

const QUAD_POSITIONS = [-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0];
const QUAD_NORMALS = [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1];
const QUAD_INDICES = [0, 1, 2, 0, 2, 3];

function boundsOf(positions) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let index = 0; index < positions.length; index += 3) {
    for (let axis = 0; axis < 3; axis++) {
      min[axis] = Math.min(min[axis], positions[index + axis]);
      max[axis] = Math.max(max[axis], positions[index + axis]);
    }
  }
  return { min, max };
}

test('loads the embedded-buffer quad fixture', async () => {
  const mesh = await load(quadPath);
  assert.ok(mesh.positions instanceof Float32Array);
  assert.ok(mesh.indices instanceof Uint32Array);
  assert.ok(mesh.normals instanceof Float32Array);
  assert.equal(mesh.positions.length / 3, 4);
  assert.equal(mesh.indices.length / 3, 2);
  assert.deepEqual(Array.from(mesh.positions), QUAD_POSITIONS);
  assert.deepEqual(Array.from(mesh.normals), QUAD_NORMALS);
  assert.deepEqual(Array.from(mesh.indices), QUAD_INDICES);
  assert.deepEqual(boundsOf(mesh.positions), { min: [-0.5, -0.5, 0], max: [0.5, 0.5, 0] });
  assert.equal(mesh.source.kind, 'gltf');
  assert.equal(mesh.source.meshes, 1);
  assert.equal(mesh.source.primitives, 1);
  assert.equal(mesh.source.hasNormals, true);
  assert.equal(mesh.source.buffers.length, 1);
  assert.equal(mesh.source.buffers[0].mode, 'embedded');
  assert.equal(mesh.source.buffers[0].bytes, 108);
});

test('loads the same quad from a sibling .bin buffer', async () => {
  const embedded = await load(quadPath);
  const sibling = await load(path.join(fixtures, 'quad-sibling.gltf'));
  assert.deepEqual(Array.from(sibling.positions), Array.from(embedded.positions));
  assert.deepEqual(Array.from(sibling.normals), Array.from(embedded.normals));
  assert.deepEqual(Array.from(sibling.indices), Array.from(embedded.indices));
  assert.equal(sibling.source.buffers[0].mode, 'sibling');
  assert.equal(sibling.source.buffers[0].file, 'quad.bin');
});

test('rejects an accessor that reads past its bufferView', async () => {
  await assert.rejects(load(path.join(fixtures, 'quad-broken-accessor.gltf')), error => {
    assert.ok(error instanceof GltfError);
    assert.equal(error.code, ERROR_CODES.ACCESSOR);
    assert.match(error.message, /needs 132 bytes but bufferView 0 has 96/);
    return true;
  });
});

test('rejects unsupported glTF extensions', async () => {
  await assert.rejects(load(path.join(fixtures, 'quad-unsupported-ext.gltf')), error => {
    assert.ok(error instanceof GltfError);
    assert.equal(error.code, ERROR_CODES.UNSUPPORTED);
    assert.match(error.message, /KHR_draco_mesh_compression/);
    return true;
  });
});

test('reports NG-E-GLTF-MISSING for a missing file', async () => {
  await assert.rejects(load(path.join(fixtures, 'does-not-exist.gltf')), error => {
    assert.ok(error instanceof GltfError);
    assert.equal(error.code, ERROR_CODES.MISSING);
    return true;
  });
});

test('the glTF path rebuilds a byte-identical, verifiable package', async () => {
  const mesh = await load(quadPath);
  const first = await buildPackageFromMesh(path.join(temporary, 'package-a'), mesh);
  const second = await buildPackageFromMesh(path.join(temporary, 'package-b'), mesh);
  assert.equal(first.packageDigest, second.packageDigest);
  assert.equal(computePackageDigest(first.manifest), first.manifest.packageDigest);
  assert.deepEqual(first.counts, {
    sourceVertices: 4, sourceTriangles: 2, clusters: 1, lodLevels: 1, clusterVertices: 4, clusterTriangles: 2,
  });
  assert.deepEqual(first.manifest.bounds, { min: [-0.5, -0.5, 0], max: [0.5, 0.5, 0] });
  assert.equal(first.manifest.source.kind, 'gltf');
  assert.equal(first.manifest.source.hasNormals, true);
  assert.ok((await fs.readFile(first.manifestPath)).equals(await fs.readFile(second.manifestPath)));
  for (const section of first.manifest.sections) {
    const left = await fs.readFile(path.join(path.dirname(first.manifestPath), section.path));
    const right = await fs.readFile(path.join(path.dirname(second.manifestPath), section.path));
    assert.ok(left.equals(right), `section ${section.name} should be byte-identical`);
  }
  const verified = await verifyPackage(first.manifestPath);
  assert.equal(verified.ok, true);
  assert.equal(verified.packageDigest, first.packageDigest);

  const document = createMeshDocument(mesh);
  assert.equal(document.packageDigest, first.packageDigest);
  const rebuilt = await buildPackageFromMesh(
    path.join(temporary, 'package-c'),
    meshFromDocument(JSON.parse(JSON.stringify(document))),
  );
  assert.equal(rebuilt.packageDigest, first.packageDigest);
  assert.deepEqual(rebuilt.counts, first.counts);
});

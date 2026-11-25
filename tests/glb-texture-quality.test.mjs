// Texture resolution, aspect and filtering for external model import.
//
// The reported GLB problems were two regressions in the cook step rather than in
// glTF parsing: every map was forced to a square `size` (512 by default) and the
// material bake point-sampled its sources, so a 2048 map arrived as a 512 square
// with most of its texels dropped.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { importAsset, verifyImport } from '../engine/assets/import-service.mjs';
import { cookDirectory, encodePng, decodePng, resizeRgba, targetDimensions, readNgtex } from '../tools/textures.mjs';

async function fixture(fn) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'ng-texture-size-'));
  try { await fn(directory); } finally { await fs.rm(directory, { force: true, recursive: true }); }
}

const solid = (width, height, rgba) => ({ width, height, data: Buffer.from(Array.from({ length: width * height }, () => rgba).flat()) });

function textureManifest(directory) {
  return fs.readFile(path.join(directory, 'textures', 'manifest.json'), 'utf8').then(JSON.parse);
}

test('aspect-preserving cook keeps the source ratio and never magnifies', () => fixture(async (directory) => {
  const source = path.join(directory, 'source'), out = path.join(directory, 'out');
  await fs.mkdir(source);
  await fs.writeFile(path.join(source, 'wide.albedo.png'), encodePng(solid(256, 64, [200, 120, 40, 255])));
  await fs.writeFile(path.join(source, 'big.albedo.png'), encodePng(solid(1024, 1024, [40, 90, 200, 255])));
  const { manifest } = cookDirectory({ source, out, size: 512, preserveAspect: true });
  const byName = Object.fromEntries(manifest.textures.map((entry) => [entry.name, entry]));
  assert.equal(byName.wide.width, 256, 'a texture smaller than the cap must not be enlarged');
  assert.equal(byName.wide.height, 64, '4:1 must survive the cook');
  assert.equal(byName.big.width, 512);
  assert.equal(byName.big.height, 512);
  assert.equal(byName.big.sourceWidth, 1024);
  // The container header must describe the non-square mip chain, not a square one.
  const container = readNgtex(await fs.readFile(path.join(out, 'wide.albedo.ngtex')));
  assert.deepEqual([container.width, container.height], [256, 64]);
  assert.deepEqual([container.mips[0].width, container.mips[0].height], [256, 64]);
}));

test('minification averages every covered texel instead of dropping them', () => {
  // A 2x2 checker reduced to one pixel must average, not pick one corner.
  const checker = { width: 2, height: 2, data: Buffer.from([255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255]) };
  const average = resizeRgba(checker, 1, 1, 'albedo');
  assert.ok(Math.abs(average.data[0] - 188) <= 1, `expected the sRGB mean 188, got ${average.data[0]}`);
  assert.equal(average.data[3], 255);
  // A 16x16 all-white source reduced to 1x1 must stay white; point sampling of a
  // single texel would too, so the interesting case is a ramp.
  const ramp = { width: 16, height: 1, data: Buffer.from(Array.from({ length: 16 }, (_, i) => [i * 17, i * 17, i * 17, 255]).flat()) };
  const mean = resizeRgba(ramp, 1, 1, 'orm').data[0];
  const expected = Math.round(Array.from({ length: 16 }, (_, i) => i * 17).reduce((sum, value) => sum + value, 0) / 16);
  assert.ok(Math.abs(mean - expected) <= 1, `expected ${expected}, got ${mean}`);
  // Normals stay unit length after filtering.
  const normals = { width: 2, height: 1, data: Buffer.from([255, 128, 255, 255, 0, 128, 255, 255]) };
  const filtered = resizeRgba(normals, 1, 1, 'normal').data;
  const length = Math.hypot(filtered[0] / 127.5 - 1, filtered[1] / 127.5 - 1, filtered[2] / 127.5 - 1);
  assert.ok(Math.abs(length - 1) < 0.02, `normal length ${length}`);
});

test('target dimensions preserve aspect, stay even and never exceed the source', () => {
  assert.deepEqual(targetDimensions(1024, 256, 512, true), { width: 512, height: 128 });
  assert.deepEqual(targetDimensions(300, 100, 4096, true), { width: 300, height: 100 });
  assert.deepEqual(targetDimensions(1000, 1000, 512, false), { width: 512, height: 512 });
  const odd = targetDimensions(1001, 501, 500, true);
  assert.equal(odd.width % 2, 0);
  assert.equal(odd.height % 2, 0);
  assert.ok(odd.width <= 1001 && odd.height <= 501);
});

async function modelFixture(directory, { width, height }) {
  await fs.mkdir(directory, { recursive: true });
  const png = encodePng(solid(width, height, [90, 140, 210, 255]));
  const positions = [-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1];
  const uv = [0, 0, 1, 0, 1, 1, 0, 1];
  const indices = [0, 1, 2, 0, 2, 3];
  const normals = [0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0];
  const binary = Buffer.alloc(positions.length * 4 + uv.length * 4 + indices.length * 2 + normals.length * 4);
  let offset = 0;
  for (const value of [...positions, ...uv]) { binary.writeFloatLE(value, offset); offset += 4; }
  for (const value of indices) { binary.writeUInt16LE(value, offset); offset += 2; }
  const normalOffset = offset;
  for (const value of normals) { binary.writeFloatLE(value, offset); offset += 4; }
  const doc = {
    asset: { version: '2.0', generator: 'nativegpu-texture-size-fixture' }, scene: 0, scenes: [{ nodes: [0] }],
    nodes: [{ name: 'Quad', mesh: 0 }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0, TEXCOORD_0: 1, NORMAL: 3 }, indices: 2, material: 0 }] }],
    materials: [{ pbrMetallicRoughness: { baseColorTexture: { index: 0 }, metallicFactor: 0, roughnessFactor: 1 } }],
    images: [{ uri: 'data:image/png;base64,' + png.toString('base64') }], textures: [{ source: 0 }],
    accessors: [
      { bufferView: 0, type: 'VEC3', componentType: 5126, count: 4 },
      { bufferView: 1, type: 'VEC2', componentType: 5126, count: 4 },
      { bufferView: 2, type: 'SCALAR', componentType: 5123, count: indices.length },
      { bufferView: 3, type: 'VEC3', componentType: 5126, count: 4 },
    ],
    bufferViews: [
      { buffer: 0, byteOffset: 0, byteLength: positions.length * 4 },
      { buffer: 0, byteOffset: positions.length * 4, byteLength: uv.length * 4 },
      { buffer: 0, byteOffset: (positions.length + uv.length) * 4, byteLength: indices.length * 2 },
      { buffer: 0, byteOffset: normalOffset, byteLength: normals.length * 4 },
    ],
    buffers: [{ byteLength: binary.length, uri: 'data:application/octet-stream;base64,' + binary.toString('base64') }],
  };
  const file = path.join(directory, 'quad.gltf');
  await fs.writeFile(file, JSON.stringify(doc));
  return file;
}

test('model import keeps the authored resolution by default', () => fixture(async (directory) => {
  const file = await modelFixture(directory, { width: 640, height: 640 });
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  await verifyImport(result.directory);
  const manifest = await textureManifest(result.directory);
  const albedo = manifest.textures.find((entry) => entry.name === 'm1' && entry.semantic === 'albedo');
  assert.equal(albedo.width, 640, 'auto must not downscale to the old 512 default');
  assert.equal(manifest.size, 4096);
  assert.equal(manifest.preserveAspect, true);
}));

test('model import honours an explicit cap and never upscales a small map', () => fixture(async (directory) => {
  const capped = await importAsset({ source: await modelFixture(path.join(directory, 'capped'), { width: 1024, height: 1024 }), output: path.join(directory, 'imports'), size: 256 });
  const cappedManifest = await textureManifest(capped.directory);
  assert.equal(cappedManifest.textures.find((entry) => entry.name === 'm1' && entry.semantic === 'albedo').width, 256);

  const small = await importAsset({ source: await modelFixture(path.join(directory, 'small'), { width: 64, height: 64 }), output: path.join(directory, 'imports') });
  const smallManifest = await textureManifest(small.directory);
  assert.equal(smallManifest.textures.find((entry) => entry.name === 'm1' && entry.semantic === 'albedo').width, 64, 'a 64px map must stay 64px');
}));

test('a non-square source map keeps its ratio through a model import', () => fixture(async (directory) => {
  const file = await modelFixture(directory, { width: 256, height: 64 });
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  const manifest = await textureManifest(result.directory);
  const albedo = manifest.textures.find((entry) => entry.name === 'm1' && entry.semantic === 'albedo');
  assert.deepEqual([albedo.width, albedo.height], [256, 64]);
  const container = readNgtex(await fs.readFile(path.join(result.directory, 'textures', albedo.file)));
  assert.deepEqual([container.width, container.height], [256, 64]);
}));

test('shared source maps are stored once and factor-only channels stay per material', () => fixture(async (directory) => {
  const file = await modelFixture(directory, { width: 32, height: 32 });
  const doc = JSON.parse(await fs.readFile(file, 'utf8'));
  const shared = structuredClone(doc.materials[0]);
  shared.pbrMetallicRoughness.baseColorFactor = [.5, .5, .5, 1];
  doc.materials.push(shared);
  doc.meshes[0].primitives.push({ ...doc.meshes[0].primitives[0], material: 1 });
  await fs.writeFile(file, JSON.stringify(doc));
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  const manifest = await textureManifest(result.directory);
  const albedo = manifest.textures.filter((entry) => entry.name.startsWith('m') && entry.semantic === 'albedo' && entry.sourceWidth === 32);
  // The tint is baked per material, so the two albedo maps legitimately differ.
  assert.equal(albedo.length, 2);
  const normal = manifest.textures.filter((entry) => entry.semantic === 'normal');
  // Both materials have no normal map, so the flat default is shared by content.
  assert.ok(normal.length >= 1);
  assert.ok(Object.values(result.manifest.materials).every((material) => material.albedo && material.normal && material.orm));
}));

test('off-centre tangents are orthogonalized instead of rejected', () => fixture(async (directory) => {
  const file = await modelFixture(directory, { width: 8, height: 8 });
  const doc = JSON.parse(await fs.readFile(file, 'utf8'));
  const binary = Buffer.from(doc.buffers[0].uri.split(',')[1], 'base64');
  // Tangents 30 degrees away from the +Y normal, unit length, right handed.
  const skewed = Buffer.alloc(4 * 16);
  for (let i = 0; i < 4; i += 1) {
    skewed.writeFloatLE(Math.cos(Math.PI / 6), i * 16);
    skewed.writeFloatLE(0, i * 16 + 4);
    skewed.writeFloatLE(-Math.sin(Math.PI / 6), i * 16 + 8);
    skewed.writeFloatLE(1, i * 16 + 12);
  }
  const merged = Buffer.concat([binary, skewed]);
  const base = doc.bufferViews.at(-1);
  doc.bufferViews.push({ buffer: 0, byteOffset: binary.length, byteLength: skewed.length });
  doc.accessors.push({ bufferView: doc.bufferViews.length - 1, type: 'VEC4', componentType: 5126, count: 4 });
  doc.meshes[0].primitives[0].attributes.TANGENT = doc.accessors.length - 1;
  doc.buffers[0] = { byteLength: merged.length, uri: 'data:application/octet-stream;base64,' + merged.toString('base64') };
  assert.ok(base);
  await fs.writeFile(file, JSON.stringify(doc));
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  await verifyImport(result.directory);
  const mesh = JSON.parse(await fs.readFile(path.join(result.directory, result.manifest.parts[0].mesh), 'utf8'));
  const row = mesh.vertices[0];
  const normal = row.slice(3, 6), tangent = row.slice(20, 23);
  const dot = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2];
  assert.ok(Math.abs(dot) < 1e-5, `tangent must be perpendicular to its normal, dot=${dot}`);
  assert.ok(Math.abs(Math.hypot(...tangent) - 1) < 1e-5, 'tangent must stay unit length');
  // The engine mirrors glTF Z, which flips the bitangent handedness sign.
  assert.equal(row[23], -1, 'handedness follows the glTF to engine mirror');
}));

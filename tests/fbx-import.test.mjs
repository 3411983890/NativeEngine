// External FBX import: document units, axes, texture resolution and diagnostics.
//
// FBX geometry is decoded by Assimp; these tests cover what that conversion does
// not carry over, namely the document-level unit/axis settings and the fact that
// an FBX usually references its textures by name only.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { importAsset, verifyImport, inspectAsset } from '../engine/assets/import-service.mjs';
import { readFbxMetadata, fbxCoordinateOptions, describeFbx } from '../engine/assets/fbx-metadata.mjs';
import { writeFbxFixture } from '../tools/asset-fixtures.mjs';
import { loadScene } from '../engine/scene/index.mjs';

async function fixture(fn) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'ng-fbx-'));
  try { await fn(directory); } finally { await fs.rm(directory, { force: true, recursive: true }); }
}

const stringProperty = (value) => {
  const bytes = Buffer.from(value, 'utf8'), out = Buffer.alloc(5 + bytes.length);
  out.write('S', 0); out.writeUInt32LE(bytes.length, 1); bytes.copy(out, 5); return out;
};
const doubleProperty = (value) => { const out = Buffer.alloc(9); out.write('D', 0); out.writeDoubleLE(value, 1); return out; };
const intProperty = (value) => { const out = Buffer.alloc(5); out.write('I', 0); out.writeInt32LE(value, 1); return out; };

/** Minimal binary FBX 7.4 container: absolute node offsets, uncompressed scalars. */
function buildBinaryFbx(nodes) {
  const build = (node, base) => {
    const name = Buffer.from(node.name, 'utf8');
    const properties = Buffer.concat(node.properties ?? []);
    const headerBytes = 13 + name.length;
    let childBase = base + headerBytes + properties.length;
    const children = [];
    for (const child of node.children ?? []) {
      const built = build(child, childBase);
      children.push(built.bytes);
      childBase += built.bytes.length;
    }
    const body = Buffer.concat([...children, Buffer.alloc(13)]);
    const header = Buffer.alloc(13);
    header.writeUInt32LE(base + headerBytes + properties.length + body.length, 0);
    header.writeUInt32LE((node.properties ?? []).length, 4);
    header.writeUInt32LE(properties.length, 8);
    header.writeUInt8(name.length, 12);
    return { bytes: Buffer.concat([header, name, properties, body]) };
  };
  const magic = Buffer.from('Kaydara FBX Binary  \u0000', 'latin1');
  assert.equal(magic.length, 21);
  const version = Buffer.alloc(4);
  version.writeUInt32LE(7400, 0);
  const head = Buffer.concat([magic, Buffer.from([0x1a, 0x00]), version]);
  const parts = [head];
  let offset = head.length;
  for (const node of nodes) {
    const built = build(node, offset);
    parts.push(built.bytes);
    offset += built.bytes.length;
  }
  parts.push(Buffer.alloc(13));
  return Buffer.concat(parts);
}

const setting = (name, type, value) => ({ name: 'P', properties: [stringProperty(name), stringProperty(type), stringProperty(''), stringProperty(''), type === 'int' ? intProperty(value) : doubleProperty(value)] });

test('FBX metadata reads unit and axes from both ASCII and binary containers', () => {
  const ascii = readFbxMetadata(Buffer.from(`; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXVersion: 7400
}
GlobalSettings:  {
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",2
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",1
		P: "FrontAxisSign", "int", "Integer", "",-1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",2.54
	}
}
`));
  assert.equal(ascii.format, 'ascii');
  assert.equal(ascii.version, 7400);
  assert.equal(ascii.metersPerUnit, 0.0254);
  assert.deepEqual(describeFbx(ascii).upAxis, '+Z');
  assert.equal(describeFbx(ascii).frontAxis, '-Y');
  // up +Z, front -Y, coord +X is a right-handed triple, so no mirrored handedness.
  assert.deepEqual(fbxCoordinateOptions(ascii), { metersPerUnit: 0.0254, upAxis: '+Z', forwardAxis: '-Y', handedness: 'right' });

  const binary = readFbxMetadata(buildBinaryFbx([
    { name: 'FBXHeaderExtension', properties: [], children: [{ name: 'FBXVersion', properties: [intProperty(7400)] }] },
    { name: 'GlobalSettings', properties: [], children: [{ name: 'Properties70', properties: [], children: [
      setting('UpAxis', 'int', 1), setting('FrontAxis', 'int', 2), setting('CoordAxis', 'int', 0), setting('UnitScaleFactor', 'double', 100),
    ] }] },
  ]));
  assert.equal(binary.format, 'binary');
  assert.equal(binary.version, 7400);
  assert.equal(binary.metersPerUnit, 1);
  // A conventional Y-up metre file must keep the importer default identity.
  assert.equal(fbxCoordinateOptions(binary), undefined);
  assert.equal(readFbxMetadata(Buffer.from('not an fbx file at all')), undefined);
});

test('ASCII FBX with a centimetre unit imports at metre scale and records the override', () => fixture(async (directory) => {
  const { file } = await writeFbxFixture(path.join(directory, 'source'), { unitScaleFactor: 1 });
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  await verifyImport(result.directory);
  assert.equal(result.manifest.kind, 'model');
  assert.equal(result.manifest.coordinates.metersPerUnit, 0.01);
  // The quad is 2x2 file units; at 1 cm per unit it must arrive 0.02 m wide.
  const part = result.manifest.parts[0];
  assert.ok(Math.abs(part.scale[0] - 0.02) < 1e-6, `expected 0.02 m, got ${part.scale[0]}`);
  assert.ok(result.manifest.warnings.some((warning) => warning.includes('1 cm per unit')), JSON.stringify(result.manifest.warnings));
}));

test('ASCII FBX resolves textures by name and keeps working when one is missing', () => fixture(async (directory) => {
  const source = path.join(directory, 'source');
  const { file } = await writeFbxFixture(source, { unitScaleFactor: 100 });
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  await verifyImport(result.directory);
  assert.ok(result.manifest.files.some((entry) => entry.file.endsWith('m1.albedo.ngtex')), JSON.stringify(result.manifest.files.map((f) => f.file)));

  // Without the sidecar the model still publishes; only the colour slot detaches.
  const missing = await writeFbxFixture(path.join(directory, 'bare'), { texture: false });
  const bare = await importAsset({ source: missing.file, output: path.join(directory, 'imports') });
  await verifyImport(bare.directory);
  assert.ok(bare.manifest.warnings.some((warning) => warning.includes('could not be found')), JSON.stringify(bare.manifest.warnings));
  assert.equal(bare.manifest.kind, 'model');
}));

test('Z-up FBX converts to engine Y-up without mirroring the source', () => fixture(async (directory) => {
  const source = path.join(directory, 'source');
  const { file } = await writeFbxFixture(source, { upAxis: 2, frontAxis: 1 });
  const result = await importAsset({ source: file, output: path.join(directory, 'imports') });
  await verifyImport(result.directory);
  assert.deepEqual(result.manifest.coordinates, { metersPerUnit: 1, upAxis: '+Z', forwardAxis: '+Y', handedness: 'left', yawDegrees: 0 });
  // The fixture quad lies in the source XZ plane, which is the engine XY plane
  // once Z becomes up: width stays 2 m, depth collapses and height follows source Z.
  const part = result.manifest.parts[0];
  assert.ok(Math.abs(part.scale[0] - 2) < 1e-6, JSON.stringify(part.scale));
  const preview = await loadScene(path.join(result.directory, 'preview.scene.json'));
  assert.ok(preview.entities.some((entity) => entity.mesh));
}));

test('FBX is reported as a model capability and inspected without conversion', () => fixture(async (directory) => {
  const { file } = await writeFbxFixture(path.join(directory, 'source'));
  const inspected = await inspectAsset(file);
  assert.equal(inspected.kind, 'model');
  assert.ok(inspected.capability.inputs.includes('.fbx'));
}));

#!/usr/bin/env node
// B7: reproducible glTF 2.0 fixtures for the minimal importer.
//
// Every byte is derived from the number arrays below at run time: the base64
// payload and the sibling .bin are written from the same generated buffer, so
// the fixtures are regenerable instead of hand-copied binary artifacts.
//
// Outputs (tools/offline/fixtures/):
//   quad.gltf               valid quad, embedded base64, interleaved vertices
//                           (POSITION byteOffset 0 / NORMAL 12, stride 24)
//   quad-sibling.gltf       same quad, buffer in sibling quad.bin
//   quad.bin                the 108-byte vertex+index buffer
//   quad-broken-accessor.gltf  POSITION accessor.count reads past its bufferView
//   quad-unsupported-ext.gltf  requires KHR_draco_mesh_compression
import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const outputDirectory = path.join(here, 'fixtures');
const GENERATOR = 'nativegpu-offline-gltf-fixtures 0.1.0';

const QUAD_POSITIONS = [-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0];
const QUAD_NORMALS = [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1];
const QUAD_INDICES = [0, 1, 2, 0, 2, 3];

const sha256 = data => createHash('sha256').update(data).digest('hex');

function float32Bytes(values) {
  const buffer = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => buffer.writeFloatLE(value, index * 4));
  return buffer;
}

function uint16Bytes(values) {
  const buffer = Buffer.alloc(values.length * 2);
  values.forEach((value, index) => buffer.writeUInt16LE(value, index * 2));
  return buffer;
}

// Interleaved px,py,pz,nx,ny,nz per vertex, then tightly packed uint16 indices.
const vertexBytes = Buffer.alloc((QUAD_POSITIONS.length / 3) * 24);
for (let vertex = 0; vertex < QUAD_POSITIONS.length / 3; vertex++) {
  float32Bytes(QUAD_POSITIONS.slice(vertex * 3, vertex * 3 + 3)).copy(vertexBytes, vertex * 24);
  float32Bytes(QUAD_NORMALS.slice(vertex * 3, vertex * 3 + 3)).copy(vertexBytes, vertex * 24 + 12);
}
const indexBytes = uint16Bytes(QUAD_INDICES);
const quadBuffer = Buffer.concat([vertexBytes, indexBytes]);

function quadDocument({ bufferUri }) {
  return {
    asset: { version: '2.0', generator: GENERATOR },
    scene: 0,
    scenes: [{ name: 'quad', nodes: [0] }],
    nodes: [{ name: 'quad', mesh: 0 }],
    meshes: [{ name: 'quad', primitives: [{ attributes: { POSITION: 0, NORMAL: 1 }, indices: 2, mode: 4 }] }],
    buffers: [{ byteLength: quadBuffer.length, uri: bufferUri }],
    bufferViews: [
      { buffer: 0, byteOffset: 0, byteLength: vertexBytes.length, byteStride: 24, target: 34962 },
      { buffer: 0, byteOffset: vertexBytes.length, byteLength: indexBytes.length, target: 34963 },
    ],
    accessors: [
      {
        bufferView: 0, byteOffset: 0, componentType: 5126, count: 4, type: 'VEC3',
        min: [-0.5, -0.5, 0], max: [0.5, 0.5, 0],
      },
      { bufferView: 0, byteOffset: 12, componentType: 5126, count: 4, type: 'VEC3' },
      { bufferView: 1, byteOffset: 0, componentType: 5123, count: QUAD_INDICES.length, type: 'SCALAR' },
    ],
  };
}

const clone = value => JSON.parse(JSON.stringify(value));

const embedded = quadDocument({
  bufferUri: `data:application/octet-stream;base64,${quadBuffer.toString('base64')}`,
});
const sibling = quadDocument({ bufferUri: 'quad.bin' });
const broken = clone(embedded);
broken.accessors[0].count = 6;
const unsupported = clone(embedded);
unsupported.extensionsUsed = ['KHR_draco_mesh_compression'];
unsupported.extensionsRequired = ['KHR_draco_mesh_compression'];

const outputs = [
  { file: 'quad.gltf', bytes: Buffer.from(`${JSON.stringify(embedded, null, 2)}\n`, 'utf8'), vertices: 4, triangles: 2 },
  { file: 'quad-sibling.gltf', bytes: Buffer.from(`${JSON.stringify(sibling, null, 2)}\n`, 'utf8'), vertices: 4, triangles: 2 },
  { file: 'quad.bin', bytes: quadBuffer },
  {
    file: 'quad-broken-accessor.gltf', bytes: Buffer.from(`${JSON.stringify(broken, null, 2)}\n`, 'utf8'),
    note: 'POSITION accessor.count=6 needs 132 bytes from a 96-byte bufferView',
  },
  {
    file: 'quad-unsupported-ext.gltf', bytes: Buffer.from(`${JSON.stringify(unsupported, null, 2)}\n`, 'utf8'),
    note: 'extensionsRequired includes KHR_draco_mesh_compression',
  },
];

await fs.mkdir(outputDirectory, { recursive: true });
const summary = [];
for (const output of outputs) {
  const target = path.join(outputDirectory, output.file);
  await fs.writeFile(target, output.bytes);
  summary.push({
    file: path.relative(process.cwd(), target),
    bytes: output.bytes.length,
    sha256: sha256(output.bytes),
    ...(output.vertices === undefined ? {} : { vertices: output.vertices, triangles: output.triangles }),
    ...(output.note === undefined ? {} : { note: output.note }),
  });
}

console.log(JSON.stringify({ ok: true, generator: GENERATOR, fixtures: summary }, null, 2));

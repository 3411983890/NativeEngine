import test from 'node:test';
import assert from 'node:assert/strict';
import { loadCharacterAsset } from '../../engine/character/rig-mesh.mjs';
import { buildScalp } from '../../engine/hair/scalp.mjs';
import { generateHairstyle } from '../../engine/hair/generate.mjs';
import { cookHairstyle, readHairPackage } from '../../engine/hair/cook.mjs';
import {
  packHairForGpu, validateHairPack, encodeHairPack, decodeHairPack,
  HAIR_POINT_STRIDE, HAIR_STRAND_STRIDE, HAIR_GPU_PACK_SCHEMA,
  HAIR_PACK_FILE_MAGIC, HAIR_PACK_HEADER_BYTES, HAIR_PACK_SECTION_ALIGNMENT,
} from '../../engine/hair/gpu-pack.mjs';
import { hairStyleSpec } from '../../tools/hair-style-fixture.mjs';

// The GPU upload contract for strands.
//
// Everything here is read back from the cooked bytes, not from the generator's in-memory
// output: a packer that trusts the object that produced the file cannot catch a writer that
// packs the wrong offset. The assertions are about what a renderer is allowed to assume —
// fixed strides, explicit attributes, a draw count that matches the buffers, and an error
// instead of a black sliver for a curve that cannot be drawn.

const character = await loadCharacterAsset('assets/builtin/robot/character.json');
const scalp = buildScalp(character);
const spec = hairStyleSpec();
const generated = generateHairstyle(spec, character, { scalp });
const cooked = cookHairstyle(generated);
const read = () => readHairPackage(cooked.bytes);

test('the pack exposes every attribute the draw path needs, at a fixed stride', () => {
  const pack = packHairForGpu(read(), { level: 0 });
  assert.equal(pack.schema, HAIR_GPU_PACK_SCHEMA);
  assert.equal(pack.hairHash, cooked.hash);
  assert.equal(pack.strides.point, HAIR_POINT_STRIDE);
  assert.equal(pack.strides.strand, HAIR_STRAND_STRIDE);
  assert.equal(pack.counts.strands, generated.levels[0].curves.length);
  assert.equal(pack.counts.points, pack.counts.strands * (pack.draw.segmentsPerStrand + 1));
  assert.deepEqual(validateHairPack(pack), { ok: true, problems: [] });
  // Tangents must be unit length and root-to-tip must run 0 → 1 per strand, otherwise the
  // fibre shading in `shadeSample` is fed a direction that is not a direction.
  for (let strand = 0; strand < pack.counts.strands; ++strand) {
    const first = pack.buffers.strands[strand * 8];
    const count = pack.buffers.strands[strand * 8 + 1];
    assert.equal(count, pack.draw.segmentsPerStrand + 1);
    assert.equal(pack.buffers.points[first * 8 + 7], 0, 'root must be at rootToTip 0');
    const tip = (first + count - 1) * 8;
    assert.ok(Math.abs(pack.buffers.points[tip + 7] - 1) < 1e-6, 'tip must be at rootToTip 1');
    for (let point = 0; point < count; ++point) {
      const base = (first + point) * 8;
      const length = Math.hypot(pack.buffers.points[base + 3], pack.buffers.points[base + 4], pack.buffers.points[base + 5]);
      assert.ok(Math.abs(length - 1) < 1e-4, `tangent ${point} of strand ${strand} is not unit length`);
      assert.ok(pack.buffers.points[base + 6] > 0, 'width must be positive');
    }
  }
});

test('width is a diameter and the guide tables are passed through unchanged', () => {
  const pack = packHairForGpu(read(), { level: 0 });
  const curve = generated.levels[0].curves[0];
  // A consumer that reads this as a radius draws hair at half its authored thickness and
  // nothing errors, so the contract says diameter in the layout and the test pins it.
  assert.ok(Math.abs(pack.buffers.points[6] - curve.radii[0] * 2) < 1e-6,
    `packed width ${pack.buffers.points[6]} is not twice the radius ${curve.radii[0]}`);
  const source = read();
  assert.equal(pack.buffers.guideWeights.length, source.levels[0].weights.data.length);
  assert.ok(Buffer.from(pack.buffers.guideWeights).equals(source.levels[0].weights.data),
    'guide weights must reach the GPU byte-identically to the package');
  assert.equal(pack.counts.guidePoints, source.guides.reduce((sum, guide) => sum + guide.points.length, 0));
});

test('guide weights are normalised in the cooked table', () => {
  // This is the defect the packer found: the generator blends five guides, the runtime table
  // keeps three, and simply slicing them stored 84.5% of the strand's weight. Interpolation
  // with that table pulls every strand short of its guides and no individual weight looks
  // wrong, so the check has to be on the sum.
  const source = read();
  let worst = 0;
  for (let index = 0; index < source.levels[0].weights.count; ++index) {
    const base = index * 32;
    const sum = source.levels[0].weights.data.readFloatLE(base + 16)
      + source.levels[0].weights.data.readFloatLE(base + 20)
      + source.levels[0].weights.data.readFloatLE(base + 24);
    worst = Math.max(worst, Math.abs(sum - 1));
  }
  assert.ok(worst < 1e-3, `cooked guide weights are off by ${worst.toFixed(4)}`);
});

test('the pack is byte-deterministic and its draw command matches its buffers', () => {
  const first = packHairForGpu(read(), { level: 0 });
  const second = packHairForGpu(read(), { level: 0 });
  assert.equal(first.hash, second.hash);
  assert.equal(first.uploadBytes, second.uploadBytes);
  const expected = first.counts.points * HAIR_POINT_STRIDE + first.counts.strands * HAIR_STRAND_STRIDE
    + first.buffers.guidePoints.byteLength + first.buffers.guideWeights.length + first.buffers.strandAttributes.length;
  assert.equal(first.uploadBytes, expected, 'upload accounting disagrees with the buffers');
  assert.equal(first.draw.vertices, first.counts.segments * 6);
  assert.equal(first.draw.segmentsPerStrand, read().levels[0].segments);
});

test('every LOD level packs, and a coarser level uploads less', () => {
  const fine = packHairForGpu(read(), { level: 0 });
  const coarse = packHairForGpu(read(), { level: 1 });
  assert.ok(coarse.counts.strands < fine.counts.strands, 'a coarse LOD must draw fewer strands');
  assert.ok(coarse.counts.points < fine.counts.points, 'a coarse LOD must upload fewer points');
  assert.ok(coarse.uploadBytes < fine.uploadBytes);
  assert.deepEqual(validateHairPack(coarse), { ok: true, problems: [] });
});

test('a curve that cannot be drawn is rejected instead of uploaded', () => {
  const broken = (mutate) => {
    const source = read();
    const level = source.levels[0];
    mutate(level);
    return source;
  };
  const rejects = (source, pattern) => assert.throws(() => packHairForGpu(source, { level: 0 }), pattern);

  rejects(broken((level) => { level.curves[0].points[0][1] = Number.NaN; }), /no length|finite 3-vector/);
  rejects(broken((level) => { level.curves[3].radii[1] = 0; }), /radius/);
  rejects(broken((level) => { level.curves[5].radii[2] = 0.4; }), /radius/);
  rejects(broken((level) => { level.curves[2].points[2] = [...level.curves[2].points[1]]; }), /zero-length segment/);
  rejects(broken((level) => { level.curves.pop(); }), /weight table/);
  rejects(broken((level) => { level.segments += 1; }), /declares/);
  assert.throws(() => packHairForGpu(read(), { level: 9 }), (error) => error.code === 'NG-E-HAIR-GPU-LEVEL');
  // The file has one flat guide-point table and no per-guide header: a guide with a
  // different point count cannot be reconstructed by the runtime.
  const uneven = read();
  uneven.guides[0].points.pop();
  uneven.guides[0].radii.pop();
  assert.throws(() => packHairForGpu(uneven, { level: 0 }), (error) => error.code === 'NG-E-HAIR-GPU-GUIDE');
});

test('budgets are enforced, not silently truncated', () => {
  const source = read();
  assert.throws(() => packHairForGpu(source, { level: 0, maxStrands: 10 }),
    (error) => error.code === 'NG-E-HAIR-GPU-BUDGET');
  assert.throws(() => packHairForGpu(source, { level: 0, maxPoints: 1000 }),
    (error) => error.code === 'NG-E-HAIR-GPU-BUDGET');
  // The renderer must not be handed a pack with a mismatched stride and left to discover it
  // as a half-drawn hairstyle.
  const pack = packHairForGpu(source, { level: 0 });
  const tampered = { ...pack, draw: { ...pack.draw, vertices: pack.draw.vertices - 6 } };
  assert.throws(() => validateHairPack(tampered), /six per segment/);
});

test('the on-disk pack round-trips and refuses every edit', () => {
  const source = packHairForGpu(read(), { level: 0 });
  const file = encodeHairPack(source);
  assert.equal(file.toString('ascii', 0, 4), HAIR_PACK_FILE_MAGIC);
  assert.equal(file.readUInt32LE(8), HAIR_PACK_HEADER_BYTES);
  assert.equal(file.length % HAIR_PACK_SECTION_ALIGNMENT, 0);
  const decoded = decodeHairPack(file);
  assert.equal(decoded.hash, source.hash);
  assert.equal(decoded.hairHash, source.hairHash);
  assert.deepEqual(decoded.counts, source.counts);
  assert.deepEqual(decoded.strides, source.strides);
  assert.deepEqual(decoded.bounds, source.bounds);
  assert.equal(decoded.draw.vertices, source.draw.vertices);
  assert.equal(decoded.uploadBytes, source.uploadBytes);
  for (const name of ['points', 'strands', 'guidePoints', 'guideWeights', 'strandAttributes']) {
    assert.ok(Buffer.from(decoded.buffers[name].buffer, decoded.buffers[name].byteOffset,
      decoded.buffers[name].byteLength).equals(Buffer.from(source.buffers[name].buffer,
      source.buffers[name].byteOffset, source.buffers[name].byteLength)), `${name} changed on the way to disk`);
  }

  const rejects = (bytes, pattern) => assert.throws(() => decodeHairPack(bytes), pattern);
  const patch = (offset, action) => { const copy = Buffer.from(file); action(copy); return copy; };
  rejects(patch(0, (copy) => copy.write('BAD!', 0, 'ascii')), /magic/);
  rejects(patch(4, (copy) => copy.writeUInt32LE(9, 4)), /version/);
  rejects(patch(40, (copy) => copy.writeUInt32LE(24, 40)), /strides/);
  rejects(patch(20, (copy) => copy.writeUInt32LE(copy.readUInt32LE(20) + 3, 20)), /point count/);
  rejects(patch(108, (copy) => copy.writeUInt32LE(copy.readUInt32LE(108) - 4, 108)), /require/);
  rejects(patch(88, (copy) => copy.writeUInt32LE(copy.readUInt32LE(88) + 4, 88)), /aligned/);
  rejects(patch(152, (copy) => copy.write('0'.repeat(64), 152, 'ascii')), /does not match/);
  const firstSection = file.readUInt32LE(88);
  assert.ok(firstSection >= HAIR_PACK_HEADER_BYTES);
  rejects(patch(firstSection, (copy) => { copy[firstSection] ^= 0xff; }), /does not match/);
  rejects(patch(216, (copy) => copy.write('not-a-hash', 216, 'ascii')), /cooked hairstyle/);
  const lastSectionEnd = file.readUInt32LE(88 + 4 * 4) + file.readUInt32LE(108 + 4 * 4);
  rejects(file.subarray(0, lastSectionEnd - 1), /leaves the file/);
  // The writer, not only the reader: a buffer edited after packing is not serialized under
  // the old hash.
  const edited = { ...source, buffers: { ...source.buffers, points: Float32Array.from(source.buffers.points) } };
  edited.buffers.points[0] += 0.001;
  assert.throws(() => encodeHairPack(edited), /does not hash/);
});

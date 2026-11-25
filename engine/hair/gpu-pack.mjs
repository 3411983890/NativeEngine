import { createHash } from 'node:crypto';
import { HairError } from './spec.mjs';

// GPU upload contract for strands.
//
// The cook produces a package; a renderer needs interleaved buffers with fixed strides, a
// draw command and a hard bound on what it will accept. Putting that translation here — and
// reading it back from the cooked bytes rather than from the in-memory generator output —
// means the renderer never has to re-derive a tangent or decide what to do with a degenerate
// curve at frame time.
//
// The layout is deliberately plain: one rib bone-ordered point buffer, one record per strand,
// and the guide tables passed through untouched so a GPU guide solve can consume the same
// document. Every field the plan asks to be explicit (tangent, radius, root-to-tip, material
// group, stable id) is a named attribute with a fixed offset, and a curve that cannot be
// rendered is rejected here instead of producing a black sliver on screen.

export const HAIR_GPU_PACK_SCHEMA = 'nativegpu.hair-gpu-pack/1';
// On-disk form. The renderer reads this file, not a JSON wrapper: the sections are the
// typed arrays themselves, so a stride change cannot arrive at the GPU as a half-filled
// buffer that happens to parse.
export const HAIR_PACK_FILE_MAGIC = 'HGP1';
export const HAIR_PACK_FILE_SCHEMA = 'nativegpu.hair-gpu-pack-file/1';
export const HAIR_PACK_VERSION = 1;
export const HAIR_PACK_HEADER_BYTES = 320;
export const HAIR_PACK_SECTION_ALIGNMENT = 256;

// 32 bytes: position(3) tangent(3) width(1) rootToTip(1).
export const HAIR_POINT_LAYOUT = Object.freeze([
  { name: 'position', type: 'f32x3', offset: 0 },
  { name: 'tangent', type: 'f32x3', offset: 12 },
  { name: 'widthMetres', type: 'f32', offset: 24 },
  { name: 'rootToTip', type: 'f32', offset: 28 },
]);
// 32 bytes: where the points start, how many, which material, which authored id, and the
// three guides that drive it.
export const HAIR_STRAND_LAYOUT = Object.freeze([
  { name: 'firstPoint', type: 'u32', offset: 0 },
  { name: 'pointCount', type: 'u32', offset: 4 },
  { name: 'materialGroup', type: 'u32', offset: 8 },
  { name: 'stableId', type: 'u32', offset: 12 },
  { name: 'guide0', type: 'u32', offset: 16 },
  { name: 'guide1', type: 'u32', offset: 20 },
  { name: 'guide2', type: 'u32', offset: 24 },
  { name: 'flags', type: 'u32', offset: 28 },
]);
export const HAIR_POINT_STRIDE = 32;
export const HAIR_STRAND_STRIDE = 32;
export const HAIR_GUIDE_POINT_STRIDE = 16;

const fail = (ok, code, message) => { if (!ok) throw new HairError(code, message); };
const finite = (value) => typeof value === 'number' && Number.isFinite(value);
const normalize = (vector) => {
  const length = Math.hypot(vector[0], vector[1], vector[2]);
  return length > 0 ? [vector[0] / length, vector[1] / length, vector[2] / length] : [0, 0, 0];
};

export function packHairForGpu(read, {
  level = 0, maxStrands = null, maxPoints = null, maximumRadiusMetres = 0.05,
} = {}) {
  fail(read && Array.isArray(read.levels), 'NG-E-HAIR-GPU-PACK', 'a cooked hairstyle read is required');
  const entry = read.levels[level];
  fail(entry, 'NG-E-HAIR-GPU-LEVEL', `the hairstyle has no LOD level ${level}`);
  const curves = entry.curves;
  fail(Array.isArray(curves) && curves.length > 0, 'NG-E-HAIR-GPU-PACK', 'the level has no curves to upload');

  const strandBudget = maxStrands ?? read.meta?.budget?.maxStrands ?? 200000;
  const pointBudget = maxPoints ?? read.meta?.budget?.maxPoints ?? 4000000;
  fail(curves.length <= strandBudget, 'NG-E-HAIR-GPU-BUDGET',
    `${curves.length} strands exceed the ${strandBudget} strand budget`);

  const levelSegments = entry.segments;
  fail(levelSegments >= 1, 'NG-E-HAIR-GPU-PACK', 'a level needs at least one segment');

  let totalPoints = 0;
  for (const curve of curves) {
    fail(curve.points.length >= 2, 'NG-E-HAIR-GPU-GEOMETRY', 'a strand with fewer than two points cannot be drawn');
    fail(curve.points.length === levelSegments + 1, 'NG-E-HAIR-GPU-GEOMETRY',
      `strand has ${curve.points.length} points, the level declares ${levelSegments + 1}`);
    totalPoints += curve.points.length;
  }
  fail(totalPoints <= pointBudget, 'NG-E-HAIR-GPU-BUDGET',
    `${totalPoints} points exceed the ${pointBudget} point budget`);

  const weightsStride = 32;
  fail(entry.weights && entry.weights.count === curves.length, 'NG-E-HAIR-GPU-PACK',
    'the guide weight table does not describe every strand');
  fail(entry.attributes && entry.attributes.count === curves.length, 'NG-E-HAIR-GPU-PACK',
    'the attribute table does not describe every strand');

  const points = new Float32Array(totalPoints * (HAIR_POINT_STRIDE / 4));
  const strands = new Uint32Array(curves.length * (HAIR_STRAND_STRIDE / 4));
  const boundsMin = [Infinity, Infinity, Infinity];
  const boundsMax = [-Infinity, -Infinity, -Infinity];
  const stableIds = new Set();
  let cursor = 0, degenerateSegments = 0, maximumRadius = 0, maximumStretch = 1;

  for (let index = 0; index < curves.length; ++index) {
    const curve = curves[index];
    const count = curve.points.length;
    const arc = [0];
    for (let point = 1; point < count; ++point) {
      const previous = curve.points[point - 1], current = curve.points[point];
      const segment = Math.hypot(current[0] - previous[0], current[1] - previous[1], current[2] - previous[2]);
      if (segment < 1e-9) ++degenerateSegments;
      arc.push(arc[point - 1] + segment);
    }
    const total = arc[count - 1];
    fail(total > 1e-6, 'NG-E-HAIR-GPU-GEOMETRY', `strand ${index} has no length`);
    fail(degenerateSegments === 0, 'NG-E-HAIR-GPU-GEOMETRY',
      `strand ${index} contains a zero-length segment, which has no tangent`);

    for (let point = 0; point < count; ++point) {
      const position = curve.points[point];
      fail(Array.isArray(position) && position.length === 3 && position.every(finite),
        'NG-E-HAIR-GPU-GEOMETRY', `strand ${index} point ${point} is not a finite 3-vector`);
      fail(Math.abs(position[0]) <= 1000 && Math.abs(position[1]) <= 1000 && Math.abs(position[2]) <= 1000,
        'NG-E-HAIR-GPU-GEOMETRY', `strand ${index} leaves the metre mapping bound`);
      const radius = curve.radii[point];
      fail(finite(radius) && radius > 0 && radius <= maximumRadiusMetres, 'NG-E-HAIR-GPU-GEOMETRY',
        `strand ${index} radius ${radius} is outside (0, ${maximumRadiusMetres}]`);
      maximumRadius = Math.max(maximumRadius, radius);
      const before = curve.points[Math.max(0, point - 1)];
      const after = curve.points[Math.min(count - 1, point + 1)];
      const tangent = normalize([after[0] - before[0], after[1] - before[1], after[2] - before[2]]);
      const base = (cursor + point) * (HAIR_POINT_STRIDE / 4);
      points[base] = position[0]; points[base + 1] = position[1]; points[base + 2] = position[2];
      points[base + 3] = tangent[0]; points[base + 4] = tangent[1]; points[base + 5] = tangent[2];
      // Diameter, not radius: the renderer expands a ribbon to this width and a consumer that
      // assumed radius draws hair at half thickness with no error anywhere.
      points[base + 6] = radius * 2;
      points[base + 7] = total > 0 ? arc[point] / total : 0;
      for (let axis = 0; axis < 3; ++axis) {
        boundsMin[axis] = Math.min(boundsMin[axis], position[axis]);
        boundsMax[axis] = Math.max(boundsMax[axis], position[axis]);
      }
    }
    // Segment lengths against the mean length: a cook that resampled badly shows up here
    // before it shows up as a shimmering strand.
    const mean = total / (count - 1);
    for (let point = 1; point < count; ++point) {
      const segment = arc[point] - arc[point - 1];
      maximumStretch = Math.max(maximumStretch, segment / mean);
    }

    const attributeBase = index * 16;
    const stableId = entry.attributes.data.readUInt32LE(attributeBase + 12);
    fail(!stableIds.has(stableId), 'NG-E-HAIR-GPU-PACK', `duplicate strand stable id ${stableId}`);
    stableIds.add(stableId);
    const strapBase = index * (HAIR_STRAND_STRIDE / 4);
    const weightBase = index * weightsStride;
    const guide0 = entry.weights.data.readUInt32LE(weightBase);
    const guide1 = entry.weights.data.readUInt32LE(weightBase + 4);
    const guide2 = entry.weights.data.readUInt32LE(weightBase + 8);
    const weightSum = entry.weights.data.readFloatLE(weightBase + 16)
      + entry.weights.data.readFloatLE(weightBase + 20)
      + entry.weights.data.readFloatLE(weightBase + 24);
    fail(Math.abs(weightSum - 1) <= 1e-3, 'NG-E-HAIR-GPU-PACK',
      `strand ${index} guide weights sum to ${weightSum}, so it would interpolate to the wrong length`);
    fail(guide0 < read.guides.length && guide1 < read.guides.length && guide2 < read.guides.length,
      'NG-E-HAIR-GPU-PACK', `strand ${index} references a guide that is not in the package`);
    strands[strapBase] = cursor;
    strands[strapBase + 1] = count;
    strands[strapBase + 2] = curve.group ?? 0;
    strands[strapBase + 3] = stableId;
    strands[strapBase + 4] = guide0;
    strands[strapBase + 5] = guide1;
    strands[strapBase + 6] = guide2;
    strands[strapBase + 7] = 0;
    cursor += count;
  }

  // Guide buffers: the same bytes the package holds, so a GPU solve and the CPU reference
  // read identical inputs. Re-packing them here would let the two drift.
  // The buffer is sized by the number of guide *points*, not of guides: sizing it by the guide
  // count and then indexing per point writes past the end of a typed array, which silently
  // discards the write instead of failing.
  //
  // Guides must all have the same point count. The file stores the guide points as one flat
  // table with no per-guide header, so a non-uniform table cannot be reconstructed by the
  // runtime; rejecting it here is what keeps the uploaded buffer self-describing. The guide
  // resolution is shared across LODs and is not required to match this level's segment count:
  // a coarse strand level is resampled from the same guides.
  const guidePointsPerGuide = read.guides[0]?.points?.length ?? 0;
  fail(guidePointsPerGuide >= 2, 'NG-E-HAIR-GPU-GUIDE', 'the guides have fewer than two points');
  for (const guide of read.guides)
    fail(guide.points.length === guidePointsPerGuide, 'NG-E-HAIR-GPU-GUIDE',
      `guide has ${guide.points.length} points, the first guide has ${guidePointsPerGuide}`);
  const totalGuidePoints = read.guides.reduce((sum, guide) => sum + guide.points.length, 0);
  const guidePoints = new Float32Array(totalGuidePoints * (HAIR_GUIDE_POINT_STRIDE / 4));
  let guideCursor = 0;
  const guideHeaders = [];
  for (const guide of read.guides) {
    guideHeaders.push({
      firstPoint: guideCursor, pointCount: guide.points.length, group: guide.group, flags: 0,
    });
    for (let point = 0; point < guide.points.length; ++point) {
      const base = (guideCursor + point) * (HAIR_GUIDE_POINT_STRIDE / 4);
      guidePoints[base] = guide.points[point][0];
      guidePoints[base + 1] = guide.points[point][1];
      guidePoints[base + 2] = guide.points[point][2];
      guidePoints[base + 3] = guide.radii[point];
    }
    guideCursor += guide.points.length;
  }
  fail(guideCursor === totalGuidePoints, 'NG-E-HAIR-GPU-PACK',
    'the guide point buffer was not filled to its declared size');

  const chunks = [
    Buffer.from(points.buffer, points.byteOffset, points.byteLength),
    Buffer.from(strands.buffer, strands.byteOffset, strands.byteLength),
    Buffer.from(guidePoints.buffer, guidePoints.byteOffset, guidePoints.byteLength),
    entry.weights.data,
    entry.attributes.data,
  ];
  const hash = createHash('sha256').update(Buffer.concat(chunks)).digest('hex');
  const uploadBytes = chunks.reduce((sum, chunk) => sum + chunk.length, 0);

  return {
    schema: HAIR_GPU_PACK_SCHEMA,
    hairHash: read.hash,
    level,
    counts: {
      strands: curves.length,
      points: totalPoints,
      segments: totalPoints - curves.length,
      guides: read.guides.length,
      guidePoints: guideCursor,
      materialGroups: read.meta?.material?.groups?.length ?? 1,
    },
    strides: {
      point: HAIR_POINT_STRIDE,
      strand: HAIR_STRAND_STRIDE,
      guidePoint: HAIR_GUIDE_POINT_STRIDE,
      guideWeight: weightsStride,
    },
    buffers: {
      points,
      strands,
      guidePoints,
      guideWeights: entry.weights.data,
      strandAttributes: entry.attributes.data,
    },
    bounds: { min: boundsMin, max: boundsMax },
    draw: {
      topology: 'trianglelist',
      // Two triangles per segment, expanded in the vertex shader from the strand record.
      vertices: (totalPoints - curves.length) * 6,
      strands: curves.length,
      segmentsPerStrand: levelSegments,
    },
    validation: {
      duplicateStableIds: 0,
      degenerateSegments,
      maximumRadiusMetres: maximumRadius,
      // 1.0 means every segment is exactly the mean length; the cook resamples, so a value
      // far from 1 means the resampling disagreed with the level's segment count.
      maximumSegmentStretch: maximumStretch,
      strandBudgetLimit: strandBudget,
      pointBudgetLimit: pointBudget,
    },
    uploadBytes,
    hash,
  };
}

// A pack is only usable if the buffers are the sizes the draw command claims. Checking it in
// the renderer instead of here is how a stride change lands as a partially drawn hairstyle.
export function validateHairPack(pack) {
  const problems = [];
  const expectPoints = pack.counts.points;
  if (pack.buffers.points.length !== expectPoints * (pack.strides.point / 4))
    problems.push('point buffer length disagrees with the point count');
  if (pack.buffers.strands.length !== pack.counts.strands * (pack.strides.strand / 4))
    problems.push('strand buffer length disagrees with the strand count');
  if (pack.draw.vertices !== pack.counts.segments * 6)
    problems.push('draw vertex count is not six per segment');
  if (pack.draw.segmentsPerStrand < 1 || pack.counts.segments !== pack.counts.strands * pack.draw.segmentsPerStrand)
    problems.push('segment count disagrees with strands × segments per strand');
  if (!(pack.uploadBytes > 0)) problems.push('pack has no bytes to upload');
  if (problems.length) throw new HairError('NG-E-HAIR-GPU-PACK', problems.join('; '));
  return { ok: true, problems };
}

// Section order is the file ABI. The native reader has the same list; moving a section
// here without moving it there is a corrupt upload rather than a compile error.
export const HAIR_PACK_SECTIONS = Object.freeze([
  'points', 'strands', 'guidePoints', 'guideWeights', 'strandAttributes',
]);
const HASH_OFFSET = 152;
const HAIR_HASH_OFFSET = 216;

const padTo = (value, alignment) => Math.ceil(value / alignment) * alignment;
const asBuffer = (value, name) => {
  fail(value && (Buffer.isBuffer(value) || ArrayBuffer.isView(value)),
    'NG-E-HAIR-GPU-PACK', `${name} is not a typed buffer`);
  return Buffer.isBuffer(value) ? value : Buffer.from(value.buffer, value.byteOffset, value.byteLength);
};

// Serializes a pack for the native renderer. The header duplicates every count, stride,
// section extent and the payload hash so the reader never has to trust a section size it
// derived itself; a mismatch between the two is rejected before anything is uploaded.
export function encodeHairPack(pack) {
  validateHairPack(pack);
  fail(pack.schema === HAIR_GPU_PACK_SCHEMA, 'NG-E-HAIR-GPU-PACK',
    `pack schema ${pack.schema} is not ${HAIR_GPU_PACK_SCHEMA}`);
  fail(typeof pack.hairHash === 'string' && /^[0-9a-f]{64}$/.test(pack.hairHash), 'NG-E-HAIR-GPU-PACK',
    'the pack must name the cooked hairstyle it came from');
  fail(Array.isArray(pack.bounds?.min) && Array.isArray(pack.bounds?.max)
    && [...pack.bounds.min, ...pack.bounds.max].every(Number.isFinite)
    && pack.bounds.min.every((value, axis) => value <= pack.bounds.max[axis]), 'NG-E-HAIR-GPU-PACK',
    'the pack bounds are not a finite min ≤ max box');
  const sections = HAIR_PACK_SECTIONS.map((name) => asBuffer(pack.buffers[name], name));
  const payload = Buffer.concat(sections);
  const hash = createHash('sha256').update(payload).digest('hex');
  fail(hash === pack.hash, 'NG-E-HAIR-GPU-PACK',
    'the serialized payload does not hash to the pack hash; a buffer was changed after packing');
  const offsets = [];
  let cursor = HAIR_PACK_HEADER_BYTES;
  for (const section of sections) {
    cursor = padTo(cursor, HAIR_PACK_SECTION_ALIGNMENT);
    offsets.push(cursor);
    cursor += section.length;
  }
  const file = Buffer.alloc(padTo(cursor, HAIR_PACK_SECTION_ALIGNMENT));
  const header = file;
  header.write(HAIR_PACK_FILE_MAGIC, 0, 'ascii');
  header.writeUInt32LE(HAIR_PACK_VERSION, 4);
  header.writeUInt32LE(HAIR_PACK_HEADER_BYTES, 8);
  header.writeUInt32LE(pack.level, 12);
  const counts = [
    pack.counts.strands, pack.counts.points, pack.counts.segments,
    pack.counts.guides, pack.counts.guidePoints, pack.counts.materialGroups,
  ];
  counts.forEach((value, index) => header.writeUInt32LE(value, 16 + index * 4));
  const strides = [
    pack.strides.point, pack.strides.strand, pack.strides.guidePoint, pack.strides.guideWeight,
  ];
  strides.forEach((value, index) => header.writeUInt32LE(value, 40 + index * 4));
  pack.bounds.min.forEach((value, index) => header.writeFloatLE(value, 56 + index * 4));
  pack.bounds.max.forEach((value, index) => header.writeFloatLE(value, 72 + index * 4));
  offsets.forEach((value, index) => header.writeUInt32LE(value, 88 + index * 4));
  sections.forEach((section, index) => header.writeUInt32LE(section.length, 108 + index * 4));
  header.writeUInt32LE(pack.draw.vertices, 128);
  header.writeUInt32LE(pack.draw.segmentsPerStrand, 132);
  header.writeUInt32LE(0, 136);
  header.writeUInt32LE(0, 140);
  header.writeBigUInt64LE(BigInt(pack.uploadBytes), 144);
  header.write(hash, HASH_OFFSET, 64, 'ascii');
  header.write(pack.hairHash ?? '', HAIR_HASH_OFFSET, 64, 'ascii');
  sections.forEach((section, index) => section.copy(file, offsets[index]));
  return file;
}

// Reads a pack back. Everything is copied out of the file: a view into the mmap'd bytes
// would keep the whole file alive for one typed array and would alias a buffer the caller
// may still be writing.
export function decodeHairPack(bytes) {
  const file = asBuffer(bytes, 'hair pack');
  fail(file.length >= HAIR_PACK_HEADER_BYTES, 'NG-E-HAIR-GPU-PACK-FILE', 'hair pack is shorter than its header');
  fail(file.toString('ascii', 0, 4) === HAIR_PACK_FILE_MAGIC, 'NG-E-HAIR-GPU-PACK-FILE',
    'hair pack magic is not HGP1');
  const version = file.readUInt32LE(4), headerBytes = file.readUInt32LE(8);
  fail(version === HAIR_PACK_VERSION, 'NG-E-HAIR-GPU-PACK-FILE', `unsupported hair pack version ${version}`);
  fail(headerBytes === HAIR_PACK_HEADER_BYTES, 'NG-E-HAIR-GPU-PACK-FILE',
    `header declares ${headerBytes} bytes, the reader knows ${HAIR_PACK_HEADER_BYTES}`);
  const level = file.readUInt32LE(12);
  const counts = {
    strands: file.readUInt32LE(16), points: file.readUInt32LE(20), segments: file.readUInt32LE(24),
    guides: file.readUInt32LE(28), guidePoints: file.readUInt32LE(32), materialGroups: file.readUInt32LE(36),
  };
  const strides = {
    point: file.readUInt32LE(40), strand: file.readUInt32LE(44),
    guidePoint: file.readUInt32LE(48), guideWeight: file.readUInt32LE(52),
  };
  fail(strides.point === HAIR_POINT_STRIDE && strides.strand === HAIR_STRAND_STRIDE
    && strides.guidePoint === HAIR_GUIDE_POINT_STRIDE && strides.guideWeight === 32,
    'NG-E-HAIR-GPU-PACK-FILE', 'hair pack declares strides this reader does not implement');
  for (const [name, value] of Object.entries(counts))
    fail(Number.isInteger(value) && value > 0, 'NG-E-HAIR-GPU-PACK-FILE', `hair pack ${name} count is ${value}`);
  const segmentsPerStrand = file.readUInt32LE(132), drawVertices = file.readUInt32LE(128);
  fail(segmentsPerStrand >= 1, 'NG-E-HAIR-GPU-PACK-FILE', 'a hair pack level needs at least one segment per strand');
  fail(counts.points === counts.strands * (segmentsPerStrand + 1), 'NG-E-HAIR-GPU-PACK-FILE',
    'point count is not strands × (segments + 1)');
  fail(counts.segments === counts.strands * segmentsPerStrand, 'NG-E-HAIR-GPU-PACK-FILE',
    'segment count is not strands × segments per strand');
  fail(drawVertices === counts.segments * 6, 'NG-E-HAIR-GPU-PACK-FILE',
    'draw vertex count is not six per segment');
  const expectedSizes = [
    counts.points * strides.point,
    counts.strands * strides.strand,
    counts.guidePoints * strides.guidePoint,
    counts.strands * strides.guideWeight,
    counts.strands * 16,
  ];
  const sections = HAIR_PACK_SECTIONS.map((name, index) => {
    const offset = file.readUInt32LE(88 + index * 4);
    const size = file.readUInt32LE(108 + index * 4);
    fail(size === expectedSizes[index], 'NG-E-HAIR-GPU-PACK-FILE',
      `${name} section is ${size} bytes, its counts and stride require ${expectedSizes[index]}`);
    fail(offset % HAIR_PACK_SECTION_ALIGNMENT === 0, 'NG-E-HAIR-GPU-PACK-FILE',
      `${name} section offset ${offset} is not ${HAIR_PACK_SECTION_ALIGNMENT}-byte aligned`);
    fail(offset >= HAIR_PACK_HEADER_BYTES && offset + size <= file.length, 'NG-E-HAIR-GPU-PACK-FILE',
      `${name} section leaves the file`);
    return file.subarray(offset, offset + size);
  });
  const hash = file.toString('ascii', HASH_OFFSET, HASH_OFFSET + 64);
  fail(/^[0-9a-f]{64}$/.test(hash), 'NG-E-HAIR-GPU-PACK-FILE', 'hair pack hash is not a sha256 hex string');
  fail(createHash('sha256').update(Buffer.concat(sections)).digest('hex') === hash,
    'NG-E-HAIR-GPU-PACK-FILE', 'hair pack payload does not match its declared hash');
  const hairHash = file.toString('ascii', HAIR_HASH_OFFSET, HAIR_HASH_OFFSET + 64);
  fail(/^[0-9a-f]{64}$/.test(hairHash), 'NG-E-HAIR-GPU-PACK-FILE',
    'hair pack does not name the cooked hairstyle it was packed from');
  const copy = (section) => { const out = Buffer.alloc(section.length); section.copy(out); return out; };
  const [pointsBytes, strandsBytes, guidePointBytes, weightBytes, attributeBytes] = sections.map(copy);
  const asFloat32 = (buffer) => new Float32Array(buffer.buffer, buffer.byteOffset, buffer.length / 4);
  const asUint32 = (buffer) => new Uint32Array(buffer.buffer, buffer.byteOffset, buffer.length / 4);
  const boundsMin = [0, 1, 2].map((axis) => file.readFloatLE(56 + axis * 4));
  const boundsMax = [0, 1, 2].map((axis) => file.readFloatLE(72 + axis * 4));
  fail([...boundsMin, ...boundsMax].every(Number.isFinite) && boundsMin.every((v, i) => v <= boundsMax[i]),
    'NG-E-HAIR-GPU-PACK-FILE', 'hair pack bounds are not a finite min ≤ max box');
  const pack = {
    schema: HAIR_PACK_FILE_SCHEMA,
    level,
    counts,
    strides,
    bounds: { min: boundsMin, max: boundsMax },
    draw: { topology: 'trianglelist', vertices: drawVertices, strands: counts.strands, segmentsPerStrand },
    buffers: {
      points: asFloat32(pointsBytes),
      strands: asUint32(strandsBytes),
      guidePoints: asFloat32(guidePointBytes),
      guideWeights: weightBytes,
      strandAttributes: attributeBytes,
    },
    uploadBytes: Number(file.readBigUInt64LE(144)),
    hash,
    hairHash,
    fileBytes: file.length,
  };
  return pack;
}

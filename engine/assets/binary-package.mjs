import { createHash } from 'node:crypto';
import { canonicalJson } from './canonical.mjs';

// Content-addressed binary container shared by vegetation species, vegetation
// regions and groom packages.
//
// Why a binary container instead of one large JSON file:
//  * A 30M-instance region is ~960 MB of packed records. JSON would multiply that
//    by roughly 8x and force the whole world index through a text parser.
//  * The native loader needs a bounded, random-access table so it can reject a
//    truncated or tampered file before touching any data.
//  * The digest must cover the exact bytes, not a re-serialisation of them.
//
// Layout (little endian, all offsets absolute from the start of the file):
//   0   char[8]  magic  "NGPKG\0\0\0"
//   8   u32      version
//   12  u32      headerBytes (32 + 24 * sectionCount)
//   16  u32      sectionCount
//   20  u32      flags (reserved, must be 0)
//   24  u32      totalBytes
//   28  u32      reserved (must be 0)
//   32  section table, 24 bytes per entry:
//        u32 kind, u32 index, u32 offset, u32 bytes, u32 count, u32 reserved
//   ... payload, every section 16-byte aligned, padding zeroed.
//
// Sections are written in (kind, index) order and padding is zero, so the same
// inputs always produce the same bytes.

export const PACKAGE_MAGIC = 'NGPKG\0\0\0';
export const PACKAGE_VERSION = 1;
const HEADER_BYTES = 32;
const SECTION_BYTES = 24;
const ALIGNMENT = 16;
const MAX_SECTIONS = 4096;
const MAX_PACKAGE_BYTES = 4 * 1024 * 1024 * 1024;

export class PackageError extends Error {
  constructor(code, message) { super(message); this.name = 'PackageError'; this.code = code; }
}

function fail(ok, code, message) { if (!ok) throw new PackageError(code, message); }

// Section kinds are frozen with the first format version. A consumer that does not
// know a kind must skip it rather than guess, so adding a kind is a compatible
// change and reinterpreting one is not.
export const SectionKind = Object.freeze({
  META_JSON: 1,
  POSITION_F32X3: 2,
  NORMAL_F32X3: 3,
  UV_F32X2: 4,
  COLOR_U8X4: 5,
  INDEX_U32: 6,
  PRIMITIVE: 7,
  LOD: 8,
  INSTANCE_PACKED: 9,
  PAGE: 10,
  SPATIAL: 11,
  CURVE_HEADER: 12,
  CURVE_POINT_F32X3: 13,
  CURVE_RADIUS_F32: 14,
  CURVE_ATTRIBUTE: 15,
  BINDING: 16,
  CARD: 17,
  IMPOSTOR_RGBA8: 18,
  IMPOSTOR_DEPTH_F32: 19,
  WIND_VERTEX: 20,
  COLLISION: 21,
  SHADOW_PROXY: 22,
  GUIDE_WEIGHT: 23,
});

const KIND_NAMES = new Map(Object.entries(SectionKind).map(([name, kind]) => [kind, name]));

// Element size in bytes for fixed-layout kinds. Variable-length kinds (META_JSON)
// are described by their own byte count.
const ELEMENT_BYTES = new Map([
  [SectionKind.POSITION_F32X3, 12],
  [SectionKind.NORMAL_F32X3, 12],
  [SectionKind.UV_F32X2, 8],
  [SectionKind.COLOR_U8X4, 4],
  [SectionKind.INDEX_U32, 4],
  [SectionKind.PRIMITIVE, 12],
  [SectionKind.LOD, 40],
  [SectionKind.INSTANCE_PACKED, 32],
  [SectionKind.PAGE, 64],
  [SectionKind.SPATIAL, 16],
  [SectionKind.CURVE_HEADER, 16],
  [SectionKind.CURVE_POINT_F32X3, 12],
  [SectionKind.CURVE_RADIUS_F32, 4],
  [SectionKind.CURVE_ATTRIBUTE, 16],
  [SectionKind.BINDING, 32],
  [SectionKind.CARD, 32],
  [SectionKind.IMPOSTOR_RGBA8, 4],
  [SectionKind.IMPOSTOR_DEPTH_F32, 4],
  // One f32 per vertex: the bounded procedural sway weight. A bone hierarchy would
  // need a separate kind, and is not declared by this format version.
  [SectionKind.WIND_VERTEX, 4],
  [SectionKind.COLLISION, 32],
  [SectionKind.SHADOW_PROXY, 24],
  // Three guide indices, three weights, the strand index and one reserved word.
  [SectionKind.GUIDE_WEIGHT, 32],
]);

export function sectionKindName(kind) { return KIND_NAMES.get(kind) ?? `unknown(${kind})`; }

function alignUp(value) { return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1); }

// canonicalJson refuses NaN/Infinity/undefined, but "non-JSON value" without a path is
// useless when a manifest has three hundred fields. Walk first and name the field.
function assertJsonSafe(value, path = 'meta') {
  if (value === undefined) throw new PackageError('NG-E-VEG-FORMAT', `${path} is undefined`);
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) throw new PackageError('NG-E-VEG-FORMAT', `${path} is ${value}`);
    return;
  }
  if (value === null || typeof value !== 'object') return;
  if (Array.isArray(value)) { value.forEach((entry, index) => assertJsonSafe(entry, `${path}[${index}]`)); return; }
  for (const key of Object.keys(value)) assertJsonSafe(value[key], `${path}.${key}`);
}

function asBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  // Any typed-array view, not just Uint8Array: sections are written from Float32Array,
  // Uint32Array and Uint8Array, and Buffer.from(view.buffer, ...) keeps the exact bytes
  // without a copy.
  if (ArrayBuffer.isView(value)) return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  if (value instanceof ArrayBuffer) return Buffer.from(value);
  fail(false, 'NG-E-VEG-FORMAT', 'section data must be a Buffer or typed array');
}

// `sections` entries: { kind, index, data } for raw bytes, or
// { kind, index, elements, stride } where `elements` is a typed array.
export function buildPackage({ schema, meta, sections }) {
  fail(typeof schema === 'string' && schema.length > 0 && schema.length <= 96, 'NG-E-VEG-FORMAT', 'package schema must be a short non-empty string');
  fail(meta && typeof meta === 'object' && !Array.isArray(meta), 'NG-E-VEG-FORMAT', 'package meta must be an object');
  fail(Array.isArray(sections), 'NG-E-VEG-FORMAT', 'package sections must be an array');
  assertJsonSafe(meta, 'meta');

  const metaBytes = Buffer.from(canonicalJson({ schema, ...meta }), 'utf8');
  const all = [{ kind: SectionKind.META_JSON, index: 0, data: metaBytes }, ...sections.map((section, position) => {
    fail(section && typeof section === 'object', 'NG-E-VEG-FORMAT', `section ${position} must be an object`);
    const kind = section.kind;
    fail(Number.isSafeInteger(kind) && kind > 0 && kind <= 0xFFFF, 'NG-E-VEG-FORMAT', `section ${position} kind out of range`);
    const index = section.index ?? 0;
    fail(Number.isSafeInteger(index) && index >= 0 && index <= 0xFFFF, 'NG-E-VEG-FORMAT', `section ${position} index out of range`);
    if (section.elements !== undefined) {
      fail(ArrayBuffer.isView(section.elements), 'NG-E-VEG-FORMAT', `section ${position} elements must be a typed array`);
      return { kind, index, data: asBuffer(section.elements) };
    }
    fail(section.data !== undefined, 'NG-E-VEG-FORMAT', `section ${position} needs data or elements`);
    return { kind, index, data: asBuffer(section.data) };
  })];

  fail(all.length <= MAX_SECTIONS, 'NG-E-VEG-FORMAT', 'too many sections');
  all.sort((a, b) => (a.kind - b.kind) || (a.index - b.index));
  for (let i = 1; i < all.length; ++i) {
    fail(!(all[i].kind === all[i - 1].kind && all[i].index === all[i - 1].index),
      'NG-E-VEG-FORMAT', `duplicate section ${sectionKindName(all[i].kind)}#${all[i].index}`);
  }
  // A section's element size is part of the format. Rejecting a mis-sized blob here
  // means the native reader never has to trust a `count` it cannot verify.
  for (const section of all) {
    const stride = ELEMENT_BYTES.get(section.kind);
    if (stride !== undefined) fail(section.data.byteLength % stride === 0, 'NG-E-VEG-FORMAT',
      `${sectionKindName(section.kind)}#${section.index} is not a whole number of ${stride}-byte records`);
  }

  const headerBytes = HEADER_BYTES + SECTION_BYTES * all.length;
  let cursor = alignUp(headerBytes);
  const placed = all.map((section) => {
    const offset = cursor;
    cursor = alignUp(cursor + section.data.byteLength);
    return { ...section, offset };
  });
  fail(cursor <= MAX_PACKAGE_BYTES, 'NG-E-VEG-FORMAT', 'package exceeds 4 GiB');

  const buffer = Buffer.alloc(cursor);
  buffer.write(PACKAGE_MAGIC, 0, 'latin1');
  buffer.writeUInt32LE(PACKAGE_VERSION, 8);
  buffer.writeUInt32LE(headerBytes, 12);
  buffer.writeUInt32LE(all.length, 16);
  buffer.writeUInt32LE(0, 20);
  buffer.writeUInt32LE(cursor, 24);
  buffer.writeUInt32LE(0, 28);
  placed.forEach((section, position) => {
    const base = HEADER_BYTES + SECTION_BYTES * position;
    const stride = ELEMENT_BYTES.get(section.kind) ?? 0;
    buffer.writeUInt32LE(section.kind, base);
    buffer.writeUInt32LE(section.index, base + 4);
    buffer.writeUInt32LE(section.offset, base + 8);
    buffer.writeUInt32LE(section.data.byteLength, base + 12);
    buffer.writeUInt32LE(stride ? section.data.byteLength / stride : 0, base + 16);
    buffer.writeUInt32LE(0, base + 20);
    section.data.copy(buffer, section.offset);
  });
  return buffer;
}

export function readPackage(buffer, { maxBytes = MAX_PACKAGE_BYTES } = {}) {
  const bytes = asBuffer(buffer);
  fail(bytes.byteLength >= HEADER_BYTES, 'NG-E-VEG-FORMAT', 'package shorter than its header');
  fail(bytes.byteLength <= maxBytes, 'NG-E-VEG-CAPACITY', `package is ${bytes.byteLength} bytes, above the ${maxBytes} byte limit`);
  fail(bytes.toString('latin1', 0, 8) === PACKAGE_MAGIC, 'NG-E-VEG-FORMAT', 'package magic mismatch');
  const version = bytes.readUInt32LE(8);
  fail(version === PACKAGE_VERSION, 'NG-E-VEG-FORMAT', `unsupported package version ${version}`);
  const headerBytes = bytes.readUInt32LE(12);
  const sectionCount = bytes.readUInt32LE(16);
  const flags = bytes.readUInt32LE(20);
  const totalBytes = bytes.readUInt32LE(24);
  fail(flags === 0 && bytes.readUInt32LE(28) === 0, 'NG-E-VEG-FORMAT', 'package reserved header fields must be zero');
  fail(sectionCount > 0 && sectionCount <= MAX_SECTIONS, 'NG-E-VEG-FORMAT', 'package section count out of range');
  fail(headerBytes === HEADER_BYTES + SECTION_BYTES * sectionCount, 'NG-E-VEG-FORMAT', 'package header size disagrees with its section count');
  fail(totalBytes === bytes.byteLength, 'NG-E-VEG-FORMAT', 'package declares a different total size than it occupies');

  const sections = [];
  let previous = null;
  for (let position = 0; position < sectionCount; ++position) {
    const base = HEADER_BYTES + SECTION_BYTES * position;
    const kind = bytes.readUInt32LE(base);
    const index = bytes.readUInt32LE(base + 4);
    const offset = bytes.readUInt32LE(base + 8);
    const size = bytes.readUInt32LE(base + 12);
    const count = bytes.readUInt32LE(base + 16);
    fail(bytes.readUInt32LE(base + 20) === 0, 'NG-E-VEG-FORMAT', 'section reserved field must be zero');
    fail(kind > 0 && kind <= 0xFFFF, 'NG-E-VEG-FORMAT', 'section kind out of range');
    fail(offset >= headerBytes && offset % ALIGNMENT === 0, 'NG-E-VEG-FORMAT', 'section offset must be aligned and after the header');
    fail(offset + size <= totalBytes, 'NG-E-VEG-FORMAT', 'section extends past the end of the package');
    const stride = ELEMENT_BYTES.get(kind);
    if (stride !== undefined) fail(size % stride === 0 && count === size / stride, 'NG-E-VEG-FORMAT',
      `${sectionKindName(kind)}#${index} record size disagrees with its element count`);
    if (previous) fail(!(kind === previous.kind && index === previous.index), 'NG-E-VEG-FORMAT', 'duplicate section');
    previous = { kind, index };
    sections.push({ kind, index, offset, bytes: size, count, data: bytes.subarray(offset, offset + size) });
  }

  const metaSection = sections.find((section) => section.kind === SectionKind.META_JSON);
  fail(metaSection !== undefined, 'NG-E-VEG-FORMAT', 'package has no META_JSON section');
  let meta;
  try { meta = JSON.parse(metaSection.data.toString('utf8')); }
  catch { throw new PackageError('NG-E-VEG-FORMAT', 'package meta is not valid JSON'); }
  fail(meta && typeof meta === 'object' && typeof meta.schema === 'string', 'NG-E-VEG-FORMAT', 'package meta has no schema');
  return {
    schema: meta.schema, meta, version, sections,
    find(kind, index = 0) { return sections.find((section) => section.kind === kind && section.index === index); },
    require(kind, index = 0) {
      const found = this.find(kind, index);
      fail(found !== undefined, 'NG-E-VEG-FORMAT', `package is missing ${sectionKindName(kind)}#${index}`);
      return found;
    },
  };
}

export function digestPackage(buffer) { return createHash('sha256').update(asBuffer(buffer)).digest('hex'); }

// Typed-array views over a section. The caller keeps the package alive, so a view is
// always a read-only alias rather than a copy; native code does the same.
export function sectionF32(pkg, kind, index = 0, stride = 1) {
  const section = pkg.require(kind, index);
  fail(section.bytes % (4 * stride) === 0, 'NG-E-VEG-FORMAT', `${sectionKindName(kind)}#${index} is not 4-byte aligned data`);
  return new Float32Array(section.data.buffer, section.data.byteOffset, section.bytes / 4);
}
export function sectionU32(pkg, kind, index = 0) {
  const section = pkg.require(kind, index);
  return new Uint32Array(section.data.buffer, section.data.byteOffset, section.bytes / 4);
}
export function sectionU16(pkg, kind, index = 0) {
  const section = pkg.require(kind, index);
  return new Uint16Array(section.data.buffer, section.data.byteOffset, section.bytes / 2);
}
export function sectionU8(pkg, kind, index = 0) {
  const section = pkg.require(kind, index);
  return new Uint8Array(section.data.buffer, section.data.byteOffset, section.bytes);
}

// Packs a record array into one buffer. Keeping the writer next to the reader means
// a layout change cannot land on one side only.
export function packRecords(records, layout) {
  const stride = layout.reduce((sum, field) => sum + field.bytes, 0);
  const buffer = Buffer.alloc(stride * records.length);
  let cursor = 0;
  for (const record of records) {
    for (const field of layout) {
      const value = record[field.name];
      switch (field.type) {
        case 'f32': buffer.writeFloatLE(value, cursor); break;
        case 'u32': buffer.writeUInt32LE(value >>> 0, cursor); break;
        case 'i32': buffer.writeInt32LE(value | 0, cursor); break;
        case 'u16': buffer.writeUInt16LE(value & 0xFFFF, cursor); break;
        case 'i16': buffer.writeInt16LE(value | 0, cursor); break;
        case 'u8': buffer.writeUInt8(value & 0xFF, cursor); break;
        default: throw new PackageError('NG-E-VEG-FORMAT', `unknown field type ${field.type}`);
      }
      cursor += field.bytes;
    }
  }
  return { buffer, stride };
}

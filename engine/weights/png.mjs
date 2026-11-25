import { deflateSync, inflateSync } from 'node:zlib';

// Minimal deterministic PNG encoder.
//
// The engine already depends on `sharp` for texture import, but a diagnostic export must
// be byte-reproducible and must not go through a platform-specific binary: two runs of the
// same weight snapshot have to produce the same file so a report can be diffed. This
// encoder writes 8-bit RGBA with no filtering and a fixed deflate level; the only variance
// a decoder can see is the compression stream, which is deterministic for a given zlib.
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; ++index) {
    let value = index;
    for (let bit = 0; bit < 8; ++bit) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    table[index] = value >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let crc = 0xffffffff;
  for (let index = 0; index < bytes.length; ++index) {
    crc = CRC_TABLE[(crc ^ bytes[index]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const typeBytes = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBytes, data])), 0);
  return Buffer.concat([length, typeBytes, data, crc]);
}

export function encodePng({ width, height, rgba }) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) {
    throw new Error('PNG dimensions must be positive integers');
  }
  if (rgba.length !== width * height * 4) {
    throw new Error(`RGBA buffer is ${rgba.length} bytes, expected ${width * height * 4}`);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;   // bit depth
  header[9] = 6;   // colour type: RGBA
  header[10] = 0;  // deflate
  header[11] = 0;  // adaptive filtering
  header[12] = 0;  // no interlace
  // One filter byte (0 = none) per scanline. Filtering would improve compression but
  // makes the encoder harder to verify against a reference decoder.
  const raw = Buffer.alloc(height * (width * 4 + 1));
  for (let y = 0; y < height; ++y) {
    raw[y * (width * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4)
      .copy(raw, y * (width * 4 + 1) + 1);
  }
  const idat = deflateSync(raw, { level: 9 });
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header),
    chunk('IDAT', idat),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// Decodes the subset this encoder writes, so tests and reports can verify a file without
// pulling in an image library.
export function decodePng(bytes) {
  const buffer = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
  if (buffer.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  let offset = 8, width = 0, height = 0;
  const idat = [];
  while (offset < buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      if (data[8] !== 8 || data[9] !== 6) throw new Error('only 8-bit RGBA is supported');
    } else if (type === 'IDAT') idat.push(Buffer.from(data));
    else if (type === 'IEND') break;
    offset += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const rgba = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; ++y) {
    const row = y * (width * 4 + 1);
    if (raw[row] !== 0) throw new Error('filtered scanlines are not supported');
    rgba.set(raw.subarray(row + 1, row + 1 + width * 4), y * width * 4);
  }
  return { width, height, rgba };
}

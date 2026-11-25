#!/usr/bin/env node
// Texture cook tool: PNG -> semantic validation -> mip chain -> .ngtex container.
//
// The runtime never decodes PNG. It reads the cooked container directly, so the bytes
// that were measured and hashed here are the bytes the GPU uploads. The container is
// deliberately simple: a fixed header, a mip offset table, then tightly packed RGBA8.
//
// Usage:
//   node tools/textures.mjs cook --source assets/source/textures --out assets/textures
//   node tools/textures.mjs inspect assets/textures/*.ngtex
import { createHash } from 'node:crypto';
import { deflateSync, inflateSync } from 'node:zlib';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync, renameSync } from 'node:fs';
import { basename, dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  decodeBc1,
  decodeBc3,
  decodeBc5,
  decodeBc7,
  encodeBc1,
  encodeBc3,
  encodeBc5,
  encodeBc7,
  normalAngularError,
  psnr,
} from './bc.mjs';

// Changing either encoder invalidates caches even if sources and options are unchanged.
const COOK_COMPILER = createHash('sha256').update(readFileSync(fileURLToPath(import.meta.url)))
  .update(readFileSync(fileURLToPath(new URL('./bc.mjs',import.meta.url)))).digest('hex');

export const SEMANTICS = ['albedo', 'normal', 'orm', 'mask', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal', 'height', 'roughness', 'metallic'];
export const SEMANTIC_CODE = Object.freeze({
  albedo: 0,
  normal: 1,
  orm: 2,
  mask: 3,
  emissive: 4,
  occlusion: 5,
  coat:6,
  coatRoughness:7,
  coatNormal:8,
  height:9,
  roughness:10,
  metallic:11,
});
export const NGTEX_MAGIC = 'NGTX';
export const NGTEX_VERSION = 1;
export const NGTEX_FORMAT_RGBA8 = 0;
export const NGTEX_FORMAT_BC1 = 1;
export const NGTEX_FORMAT_BC3 = 2;
export const NGTEX_FORMAT_BC5 = 3;
export const NGTEX_FORMAT_BC7 = 4;
// Linear single-channel 16-bit UNORM. Height data keeps its source precision;
// average mips are means, never conservative bounds.
export const NGTEX_FORMAT_R16 = 5;
export const NGTEX_FORMATS = Object.freeze(['rgba8', 'bc1', 'bc3', 'bc5', 'bc7', 'r16']);
export const NGTEX_FLAG_SRGB = 1;
export const NGTEX_HEADER_BYTES = 32;

export function mipBytes(width, height, format) {
  if (format === NGTEX_FORMAT_RGBA8) return width * height * 4;
  if (format === NGTEX_FORMAT_R16) return width * height * 2;
  const block = format === NGTEX_FORMAT_BC1 ? 8 : 16;
  return Math.ceil(width / 4) * Math.ceil(height / 4) * block;
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  let c = 0xffffffff;
  for (let i = 0; i < buffer.length; i += 1) c = CRC_TABLE[(c ^ buffer[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([length, body, crc]);
}

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

/** Decode an 8-bit, non-interlaced PNG into tightly packed RGBA8. */
export function decodePng(buffer, label = 'png') {
  if (buffer.length < 8 || buffer.readUInt32BE(0) !== 0x89504e47) throw new Error(`${label}: not a PNG`);
  let offset = 8;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = -1;
  let interlace = 0;
  const idat = [];
  while (offset < buffer.length) {
    if (offset + 8 > buffer.length) throw new Error(`${label}: truncated chunk header`);
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (data.length !== length) throw new Error(`${label}: truncated ${type} chunk`);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    offset += 12 + length;
  }
  if (!width || !height) throw new Error(`${label}: missing IHDR`);
  if (bitDepth !== 8) throw new Error(`${label}: only 8-bit PNG is supported (got ${bitDepth})`);
  if (interlace !== 0) throw new Error(`${label}: interlaced PNG is not supported`);
  const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType];
  if (!channels) throw new Error(`${label}: unsupported color type ${colorType}`);
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(width * height * 4);
  let previous = Buffer.alloc(stride);
  let cursor = 0;
  for (let y = 0; y < height; y += 1) {
    const filter = raw[cursor];
    cursor += 1;
    const row = Buffer.from(raw.subarray(cursor, cursor + stride));
    cursor += stride;
    for (let x = 0; x < stride; x += 1) {
      const left = x >= channels ? row[x - channels] : 0;
      const up = previous[x];
      const upLeft = x >= channels ? previous[x - channels] : 0;
      switch (filter) {
        case 0: break;
        case 1: row[x] = (row[x] + left) & 0xff; break;
        case 2: row[x] = (row[x] + up) & 0xff; break;
        case 3: row[x] = (row[x] + ((left + up) >> 1)) & 0xff; break;
        case 4: row[x] = (row[x] + paeth(left, up, upLeft)) & 0xff; break;
        default: throw new Error(`${label}: unknown filter ${filter}`);
      }
    }
    for (let x = 0; x < width; x += 1) {
      const src = x * channels;
      const dst = (y * width + x) * 4;
      if (channels === 1) {
        out[dst] = out[dst + 1] = out[dst + 2] = row[src];
        out[dst + 3] = 255;
      } else if (channels === 2) {
        out[dst] = out[dst + 1] = out[dst + 2] = row[src];
        out[dst + 3] = row[src + 1];
      } else if (channels === 3) {
        out[dst] = row[src];
        out[dst + 1] = row[src + 1];
        out[dst + 2] = row[src + 2];
        out[dst + 3] = 255;
      } else {
        out[dst] = row[src];
        out[dst + 1] = row[src + 1];
        out[dst + 2] = row[src + 2];
        out[dst + 3] = row[src + 3];
      }
    }
    previous = row;
  }
  return { width, height, data: out };
}

/** Read IHDR fields without decoding the pixel stream. */
export function pngInfo(buffer, label = 'png') {
  if (buffer.length < 33 || buffer.readUInt32BE(0) !== 0x89504e47) throw new Error(`${label}: not a PNG`);
  const length = buffer.readUInt32BE(8);
  if (buffer.toString('ascii', 12, 16) !== 'IHDR' || length !== 13) throw new Error(`${label}: missing IHDR`);
  return {
    width: buffer.readUInt32BE(16),
    height: buffer.readUInt32BE(20),
    bitDepth: buffer[24],
    colorType: buffer[25],
    interlace: buffer[28],
  };
}

/** Decode a 16-bit grayscale PNG without quantizing it. */
export function decodePng16Gray(buffer, label = 'png') {
  const header = pngInfo(buffer, label);
  if (header.bitDepth !== 16) throw new Error(`${label}: expected 16-bit grayscale (got ${header.bitDepth})`);
  if (header.interlace !== 0) throw new Error(`${label}: interlaced PNG is not supported`);
  const channels = { 0: 1, 4: 2 }[header.colorType];
  if (!channels) throw new Error(`${label}: 16-bit height must be grayscale (color type 0 or 4)`);
  let offset = 8;
  const idat = [];
  while (offset < buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (data.length !== length) throw new Error(`${label}: truncated ${type} chunk`);
    if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    offset += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const bytesPerPixel = channels * 2;
  const stride = header.width * bytesPerPixel;
  const out = new Uint16Array(header.width * header.height);
  let previous = Buffer.alloc(stride);
  let cursor = 0;
  for (let y = 0; y < header.height; y += 1) {
    const filter = raw[cursor];
    cursor += 1;
    const row = Buffer.from(raw.subarray(cursor, cursor + stride));
    cursor += stride;
    for (let x = 0; x < stride; x += 1) {
      const left = x >= bytesPerPixel ? row[x - bytesPerPixel] : 0;
      const up = previous[x];
      const upLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel] : 0;
      switch (filter) {
        case 0: break;
        case 1: row[x] = (row[x] + left) & 0xff; break;
        case 2: row[x] = (row[x] + up) & 0xff; break;
        case 3: row[x] = (row[x] + ((left + up) >> 1)) & 0xff; break;
        case 4: row[x] = (row[x] + paeth(left, up, upLeft)) & 0xff; break;
        default: throw new Error(`${label}: unknown filter ${filter}`);
      }
    }
    for (let x = 0; x < header.width; x += 1) out[y * header.width + x] = row.readUInt16BE(x * bytesPerPixel);
    previous = row;
  }
  return { width: header.width, height: header.height, data: out };
}

/** Area-resample a 16-bit single-channel image. */
export function resizeR16(image, targetWidth, targetHeight) {
  if (image.width === targetWidth && image.height === targetHeight) return image;
  const xs = axisTaps(image.width, targetWidth);
  const ys = axisTaps(image.height, targetHeight);
  const out = new Uint16Array(targetWidth * targetHeight);
  for (let y = 0; y < targetHeight; y += 1) {
    const tapY = ys[y];
    for (let x = 0; x < targetWidth; x += 1) {
      const tapX = xs[x];
      let sum = 0;
      for (let j = 0; j < tapY.index.length; j += 1) {
        const row = tapY.index[j] * image.width;
        const wy = tapY.weight[j];
        for (let i = 0; i < tapX.index.length; i += 1) sum += image.data[row + tapX.index[i]] * tapX.weight[i] * wy;
      }
      out[y * targetWidth + x] = Math.max(0, Math.min(65535, Math.round(sum)));
    }
  }
  return { width: targetWidth, height: targetHeight, data: out };
}

/** Average 2x2 mips keep the mean height. They are not conservative bounds. */
export function makeMipsR16(image) {
  const mips = [image];
  let current = image;
  while (current.width > 1 || current.height > 1) {
    const width = Math.max(1, current.width >> 1), height = Math.max(1, current.height >> 1);
    const out = new Uint16Array(width * height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        let sum = 0;
        for (let dy = 0; dy < 2; dy += 1) for (let dx = 0; dx < 2; dx += 1) {
          const sx = Math.min(current.width - 1, x * 2 + dx), sy = Math.min(current.height - 1, y * 2 + dy);
          sum += current.data[sy * current.width + sx];
        }
        out[y * width + x] = Math.round(sum / 4);
      }
    }
    current = { width, height, data: out };
    mips.push(current);
  }
  return mips;
}

export function r16Stats(image) {
  let min = 65535, max = 0, sum = 0;
  for (const value of image.data) { if (value < min) min = value; if (value > max) max = value; sum += value; }
  const toByte = (value) => Math.round(value / 257);
  const mean = sum / Math.max(1, image.data.length);
  return { min: [toByte(min), toByte(min), toByte(min), 255], max: [toByte(max), toByte(max), toByte(max), 255], mean: [Number((mean / 65535).toFixed(4)), Number((mean / 65535).toFixed(4)), Number((mean / 65535).toFixed(4)), 1], normalMeanLength: null };
}

export function r16Buffer(image) {
  const out = Buffer.alloc(image.width * image.height * 2);
  for (let i = 0; i < image.data.length; i += 1) out.writeUInt16LE(image.data[i], i * 2);
  return out;
}

/** True when the source PNG can stay lossless in the R16 runtime format. */
export function isR16HeightSource(buffer) {
  const info = pngInfo(buffer);
  return info.bitDepth === 16 && (info.colorType === 0 || info.colorType === 4) && info.interlace === 0;
}


/** Encode a 16-bit grayscale PNG (filter 0). `data` may be Uint16Array or LE Buffer. */
export function encodePng16Gray({ width, height, data }) {
  const samples = data instanceof Uint16Array ? data : (() => {
    const out = new Uint16Array(width * height);
    for (let i = 0; i < out.length; i += 1) out[i] = data.readUInt16LE(i * 2);
    return out;
  })();
  if (samples.length !== width * height) throw new Error('16-bit PNG payload size mismatch');
  const stride = width * 2;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (stride + 1);
    raw[row] = 0;
    for (let x = 0; x < width; x += 1) raw.writeUInt16BE(samples[y * width + x], row + 1 + x * 2);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 16;
  ihdr[9] = 0;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/** Encode RGBA8 as a PNG (filter 0). Deterministic for identical input. */
export function encodePng({ width, height, data }) {
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y += 1) {
    raw[y * (stride + 1)] = 0;
    data.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/**
 * Separable resampling kernel taps.
 *
 * Minification uses exact area coverage so a 2048² source keeps every texel's
 * energy when it lands in a 512² target; point or bilinear sampling there drops
 * 15 of every 16 texels and turns detail into aliasing. Magnification stays
 * bilinear, which is what a nearest-neighbour 2× upscale would visibly band.
 */
function axisTaps(source, target) {
  const scale = source / target;
  const taps = [];
  for (let i = 0; i < target; i += 1) {
    if (scale <= 1) {
      const center = Math.min(source - 1, Math.max(0, (i + 0.5) * scale - 0.5));
      const a = Math.floor(center);
      const b = Math.min(source - 1, a + 1);
      const w = center - a;
      taps.push({ index: Int32Array.of(a, b), weight: Float64Array.of(1 - w, w) });
      continue;
    }
    const start = i * scale;
    const end = start + scale;
    const index = [];
    const weight = [];
    for (let s = Math.floor(start); s < Math.ceil(end); s += 1) {
      const overlap = Math.min(end, s + 1) - Math.max(start, s);
      if (overlap <= 0) continue;
      index.push(Math.min(source - 1, Math.max(0, s)));
      weight.push(overlap);
    }
    const total = weight.reduce((sum, value) => sum + value, 0);
    taps.push({ index: Int32Array.from(index), weight: Float64Array.from(weight, (value) => value / total) });
  }
  return taps;
}

/** Resample RGBA8 with area (minify) or bilinear (magnify) filtering. */
const colorSemantic = (semantic) => semantic === 'albedo' || semantic === 'emissive';
const toLinear = (byte) => { const x = byte / 255; return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; };
const fromLinear = (x) => Math.round(255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055));

export function resizeRgba(image, targetWidth, targetHeight, semantic = 'data') {
  if (image.width === targetWidth && image.height === targetHeight) return image;
  const color = colorSemantic(semantic);
  const normal = semantic === 'normal' || semantic === 'coatNormal';
  const xs = axisTaps(image.width, targetWidth);
  const ys = axisTaps(image.height, targetHeight);
  const out = Buffer.alloc(targetWidth * targetHeight * 4);
  const accumulation = new Float64Array(4);
  for (let y = 0; y < targetHeight; y += 1) {
    const tapY = ys[y];
    for (let x = 0; x < targetWidth; x += 1) {
      const tapX = xs[x];
      accumulation.fill(0);
      for (let j = 0; j < tapY.index.length; j += 1) {
        const row = tapY.index[j] * image.width;
        const wy = tapY.weight[j];
        for (let i = 0; i < tapX.index.length; i += 1) {
          const at = (row + tapX.index[i]) * 4;
          const w = tapX.weight[i] * wy;
          accumulation[0] += (color ? toLinear(image.data[at]) : image.data[at]) * w;
          accumulation[1] += (color ? toLinear(image.data[at + 1]) : image.data[at + 1]) * w;
          accumulation[2] += (color ? toLinear(image.data[at + 2]) : image.data[at + 2]) * w;
          accumulation[3] += image.data[at + 3] * w;
        }
      }
      const dst = (y * targetWidth + x) * 4;
      if (normal) {
        let nx = accumulation[0] / 127.5 - 1;
        let ny = accumulation[1] / 127.5 - 1;
        let nz = accumulation[2] / 127.5 - 1;
        const length = Math.hypot(nx, ny, nz) || 1;
        nx /= length; ny /= length; nz /= length;
        out[dst] = Math.round((nx * 0.5 + 0.5) * 255);
        out[dst + 1] = Math.round((ny * 0.5 + 0.5) * 255);
        out[dst + 2] = Math.round((nz * 0.5 + 0.5) * 255);
      } else if (color) {
        out[dst] = fromLinear(accumulation[0]);
        out[dst + 1] = fromLinear(accumulation[1]);
        out[dst + 2] = fromLinear(accumulation[2]);
      } else {
        out[dst] = Math.round(accumulation[0]);
        out[dst + 1] = Math.round(accumulation[1]);
        out[dst + 2] = Math.round(accumulation[2]);
      }
      out[dst + 3] = Math.round(accumulation[3]);
    }
  }
  return { width: targetWidth, height: targetHeight, data: out };
}

/**
 * Target dimensions for a source image.
 *
 * `preserveAspect` keeps the source ratio and never magnifies, so a 4:1 decal
 * stays 4:1 and a 64² detail map is not inflated to the array cap.
 */
export function targetDimensions(width, height, limit, preserveAspect = false) {
  if (!preserveAspect) return { width: limit, height: limit };
  const longest = Math.max(width, height);
  if (longest <= limit) return { width, height };
  const scale = limit / longest;
  const align = (value) => {
    const scaled = Math.max(2, Math.min(65535, Math.round(value * scale)));
    return scaled % 2 === 0 ? scaled : Math.max(2, scaled - 1);
  };
  return { width: Math.min(width, align(width)), height: Math.min(height, align(height)) };
}

function downsample(image, semantic) {
  const width = Math.max(1, image.width >> 1);
  const height = Math.max(1, image.height >> 1);
  const out = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      let r = 0;
      let g = 0;
      let b = 0;
      let a = 0;
      let count = 0;
      for (let dy = 0; dy < 2; dy += 1) {
        for (let dx = 0; dx < 2; dx += 1) {
          const sx = Math.min(image.width - 1, x * 2 + dx);
          const sy = Math.min(image.height - 1, y * 2 + dy);
          const src = (sy * image.width + sx) * 4;
          const decode = colorSemantic(semantic) ? toLinear : (x) => x;
          r += decode(image.data[src]);
          g += decode(image.data[src + 1]);
          b += decode(image.data[src + 2]);
          a += image.data[src + 3];
          count += 1;
        }
      }
      const dst = (y * width + x) * 4;
      if ((semantic === 'normal' || semantic === 'coatNormal')) {
        let nx = (r / count) / 127.5 - 1;
        let ny = (g / count) / 127.5 - 1;
        let nz = (b / count) / 127.5 - 1;
        const length = Math.hypot(nx, ny, nz) || 1;
        nx /= length;
        ny /= length;
        nz /= length;
        out[dst] = Math.round((nx * 0.5 + 0.5) * 255);
        out[dst + 1] = Math.round((ny * 0.5 + 0.5) * 255);
        out[dst + 2] = Math.round((nz * 0.5 + 0.5) * 255);
        out[dst + 3] = Math.round(a / count);
      } else {
        const encode = colorSemantic(semantic) ? fromLinear : Math.round;
        out[dst] = encode(r / count);
        out[dst + 1] = encode(g / count);
        out[dst + 2] = encode(b / count);
        out[dst + 3] = Math.round(a / count);
      }
    }
  }
  return { width, height, data: out };
}

/** Build a full mip chain. Normal maps are re-normalized after averaging. */
export function makeMips(image, semantic) {
  const mips = [image];
  let current = image;
  while (current.width > 1 || current.height > 1) {
    current = downsample(current, semantic);
    mips.push(current);
  }
  return mips;
}

/** Compute channel statistics used by the cook report and the quality gate. */
export function analyze(image, semantic) {
  let min = [255, 255, 255, 255];
  let max = [0, 0, 0, 0];
  let sum = [0, 0, 0, 0];
  let normalLengthSum = 0;
  const pixels = image.width * image.height;
  for (let i = 0; i < pixels; i += 1) {
    const offset = i * 4;
    for (let c = 0; c < 4; c += 1) {
      const v = image.data[offset + c];
      if (v < min[c]) min[c] = v;
      if (v > max[c]) max[c] = v;
      sum[c] += v;
    }
    if ((semantic === 'normal' || semantic === 'coatNormal')) {
      const nx = image.data[offset] / 127.5 - 1;
      const ny = image.data[offset + 1] / 127.5 - 1;
      const nz = image.data[offset + 2] / 127.5 - 1;
      normalLengthSum += Math.hypot(nx, ny, nz);
    }
  }
  const mean = sum.map((v) => Number((v / pixels / 255).toFixed(4)));
  return {
    min,
    max,
    mean,
    normalMeanLength: (semantic === 'normal' || semantic === 'coatNormal') ? Number((normalLengthSum / pixels).toFixed(4)) : null,
  };
}

/**
 * Write the .ngtex container.
 * Layout: 32-byte header, mip table (u32 offset, u32 size per mip), payload.
 * Offsets are absolute file offsets so the reader never has to trust a derived layout.
 */
export function writeNgtex({ mips, semantic, format = NGTEX_FORMAT_RGBA8, srgb = false }) {
  if (!SEMANTICS.includes(semantic)) throw new Error(`unknown semantic ${semantic}`);
  if (['height', 'roughness', 'metallic'].includes(semantic) && srgb) throw new Error(`${semantic} requires linear storage`);
  if (!NGTEX_FORMATS[format]) throw new Error(`unknown format ${format}`);
  if (!mips.length || mips[0].width > 65535 || mips[0].height > 65535) throw new Error('invalid mip chain');
  const mipCount = mips.length;
  const tableBytes = mipCount * 8;
  const payloadBytes = mips.reduce((total, mip) => total + mipBytes(mip.width, mip.height, format), 0);
  const totalBytes = NGTEX_HEADER_BYTES + tableBytes + payloadBytes;
  const out = Buffer.alloc(totalBytes);
  out.write(NGTEX_MAGIC, 0, 'ascii');
  out.writeUInt16LE(NGTEX_VERSION, 4);
  out.writeUInt16LE(SEMANTIC_CODE[semantic], 6);
  out.writeUInt16LE(mips[0].width, 8);
  out.writeUInt16LE(mips[0].height, 10);
  out.writeUInt8(mipCount, 12);
  out.writeUInt8(format, 13);
  out.writeUInt16LE(srgb ? NGTEX_FLAG_SRGB : 0, 14);
  out.writeUInt32LE(payloadBytes, 16);
  out.writeUInt32LE(NGTEX_HEADER_BYTES + tableBytes, 20);
  out.writeUInt32LE(0, 24);
  out.writeUInt32LE(0, 28);
  let offset = NGTEX_HEADER_BYTES + tableBytes;
  for (let index = 0; index < mipCount; index += 1) {
    const mip = mips[index];
    const bytes = mipBytes(mip.width, mip.height, format);
    if (mip.data.length !== bytes) {
      throw new Error(`mip ${index} payload is ${mip.data.length} bytes, expected ${bytes}`);
    }
    out.writeUInt32LE(offset, NGTEX_HEADER_BYTES + index * 8);
    out.writeUInt32LE(bytes, NGTEX_HEADER_BYTES + index * 8 + 4);
    mip.data.copy(out, offset);
    offset += bytes;
  }
  return out;
}

/** Read and validate a .ngtex container. Throws on any structural mismatch. */
export function readNgtex(buffer, label = 'ngtex') {
  if (buffer.length < NGTEX_HEADER_BYTES) throw new Error(`${label}: truncated header`);
  if (buffer.toString('ascii', 0, 4) !== NGTEX_MAGIC) throw new Error(`${label}: bad magic`);
  const version = buffer.readUInt16LE(4);
  if (version !== NGTEX_VERSION) throw new Error(`${label}: unsupported version ${version}`);
  const semantic = SEMANTICS[buffer.readUInt16LE(6)] ?? null;
  if (!semantic) throw new Error(`${label}: unknown semantic code`);
  const width = buffer.readUInt16LE(8);
  const height = buffer.readUInt16LE(10);
  const mipCount = buffer.readUInt8(12);
  const format = buffer.readUInt8(13);
  if (!NGTEX_FORMATS[format]) throw new Error(`${label}: unsupported format ${format}`);
  const flags = buffer.readUInt16LE(14);
  const srgb = (flags & NGTEX_FLAG_SRGB) !== 0;
  if (['height', 'roughness', 'metallic'].includes(semantic) && srgb) throw new Error(`${label}: ${semantic} requires linear storage`);
  const payloadBytes = buffer.readUInt32LE(16);
  const dataStart = buffer.readUInt32LE(20);
  if (dataStart !== NGTEX_HEADER_BYTES + mipCount * 8) throw new Error(`${label}: bad data offset`);
  const mips = [];
  let expected = 0;
  for (let index = 0; index < mipCount; index += 1) {
    const offset = buffer.readUInt32LE(NGTEX_HEADER_BYTES + index * 8);
    const bytes = buffer.readUInt32LE(NGTEX_HEADER_BYTES + index * 8 + 4);
    if (offset !== dataStart + expected) throw new Error(`${label}: non-contiguous mip ${index}`);
    const mipWidth = Math.max(1, width >> index);
    const mipHeight = Math.max(1, height >> index);
    if (bytes !== mipBytes(mipWidth, mipHeight, format)) {
      throw new Error(`${label}: mip ${index} size mismatch`);
    }
    if (offset + bytes > buffer.length) throw new Error(`${label}: mip ${index} out of range`);
    mips.push({ width: mipWidth, height: mipHeight, data: buffer.subarray(offset, offset + bytes) });
    expected += bytes;
  }
  if (expected !== payloadBytes) throw new Error(`${label}: payload size mismatch`);
  return {
    width,
    height,
    mipCount,
    semantic,
    format,
    formatName: NGTEX_FORMATS[format],
    srgb,
    mips,
    bytes: buffer.length,
  };
}

export function textureName(file) {
  const base = basename(file, extname(file));
  const dot = base.lastIndexOf('.');
  if (dot <= 0) return { name: base, semantic: 'albedo' };
  const semantic = base.slice(dot + 1);
  if (!SEMANTICS.includes(semantic)) return { name: base, semantic: 'albedo' };
  return { name: base.slice(0, dot), semantic };
}

export function compressMips(mips, format) {
  if (format === NGTEX_FORMAT_RGBA8) return mips;
  return mips.map((mip) => {
    let data;
    if (format === NGTEX_FORMAT_BC1) data = encodeBc1(mip);
    else if (format === NGTEX_FORMAT_BC3) data = encodeBc3(mip);
    else if (format === NGTEX_FORMAT_BC5) data = encodeBc5(mip);
    else if (format === NGTEX_FORMAT_BC7) data = encodeBc7(mip);
    else throw new Error(`cannot compress to format ${format}`);
    return { width: mip.width, height: mip.height, data };
  });
}

export function decodeFirstMip(container, semantic=container.semantic) {
  const first = container.mips[0];
  if (container.format === NGTEX_FORMAT_BC1) return decodeBc1(first.data, first.width, first.height);
  if (container.format === NGTEX_FORMAT_BC3) return decodeBc3(first.data, first.width, first.height);
  if (container.format === NGTEX_FORMAT_BC5) return decodeBc5(first.data, first.width, first.height);
  if (container.format === NGTEX_FORMAT_BC7) return decodeBc7(first.data, first.width, first.height);
  return Buffer.from(first.data);
}

/** Semantic whose runtime reads one channel only; the rest of the bytes are unused. */
const singleChannelOf = (semantic) => semantic === 'occlusion' || semantic === 'coat' ? 0 : semantic === 'coatRoughness' ? 1 : null;
function channelPsnr(reference, tested, channel) {
  let sum = 0;
  for (let i = channel; i < reference.length; i += 4) {
    const delta = reference[i] - tested[i];
    sum += delta * delta;
  }
  const mse = sum / Math.max(1, Math.ceil(reference.length / 4));
  return mse === 0 ? Infinity : 10 * Math.log10((255 * 255) / mse);
}

function qualityFor(semantic, reference, decoded) {
  if ((semantic === 'normal' || semantic === 'coatNormal')) {
    const angle = normalAngularError(reference.data, decoded);
    return {
      metric: 'normal-angle',
      psnr: Number(psnr(reference.data, decoded, 3).toFixed(2)),
      ...angle,
    };
  }
  const channel = singleChannelOf(semantic);
  if (channel !== null) {
    // Only the sampled channel is ever read, so scoring the unused bytes would
    // reject a good single-channel block encode for no visible reason.
    return { metric: 'psnr-channel', psnr: Number(channelPsnr(reference.data, decoded, channel).toFixed(2)) };
  }
  const channels = semantic === 'orm' || semantic === 'mask' || colorSemantic(semantic)&&reference.data.some((v,i)=>i%4===3&&v!==255) ? 4 : 3;
  return {
    metric: 'psnr',
    psnr: Number(psnr(reference.data, decoded, channels).toFixed(2)),
  };
}

function chooseFormat(semantic, reference, requested) {
  const explicit = requested && requested !== 'auto' ? requested : null;
  if (explicit) {
    if (!NGTEX_FORMATS.includes(explicit)) throw new Error(`unknown format ${explicit}`);
    const code = NGTEX_FORMATS.indexOf(explicit);
    if (colorSemantic(semantic) && explicit === 'bc5') throw new Error('BC5 cannot encode RGB colour textures');
    return { format: code, srgb: colorSemantic(semantic) };
  }
  let preferred;
  if ((semantic === 'normal' || semantic === 'coatNormal')) preferred = { format: NGTEX_FORMAT_BC5, srgb: false };
  else if (semantic === 'orm') preferred = { format: NGTEX_FORMAT_BC3, srgb: false };
  // Occlusion and the two coating factors are read from one channel only; BC5
  // keeps that channel at 1 byte per pixel instead of 4.
  else if (singleChannelOf(semantic) !== null) preferred = { format: NGTEX_FORMAT_BC5, srgb: false };
  else if (semantic === 'albedo' || semantic === 'emissive') preferred = { format: reference.data.some((v,i)=>i%4===3&&v!==255)?NGTEX_FORMAT_BC3:NGTEX_FORMAT_BC1, srgb: true };
  else preferred = { format: NGTEX_FORMAT_RGBA8, srgb: false };

  const compressed = writeNgtex({
    mips: compressMips([reference], preferred.format),
    semantic,
    format: preferred.format,
    srgb: preferred.srgb,
  });
  const decoded = decodeFirstMip(readNgtex(compressed), semantic);
  const quality = qualityFor(semantic, reference, decoded);
  let accept = true;
  if (semantic === 'albedo' || semantic === 'emissive') accept = quality.psnr >= 28;
  // Normal maps are scored by angle, not bytes: a handful of outlier blocks at a
  // 2048² seam must not force a 4x larger uncompressed upload.
  else if ((semantic === 'normal' || semantic === 'coatNormal')) accept = quality.mean <= 1.5 && quality.p95 <= 3.0 && quality.p99 <= 8.0 && quality.max <= 45.0;
  else if (semantic === 'orm') accept = quality.psnr >= 26;
  else if (singleChannelOf(semantic) !== null) accept = quality.psnr >= 34;
  else accept = false;
  if (accept) {
    // Auto mode promotes colour textures to BC7 only when BC1 leaves visible error and
    // BC7 buys at least 1.5 dB. BC7 is 8 bpp, so the budget solver still decides whether
    // the extra bytes survive.
    if ((semantic === 'albedo' || semantic === 'emissive') && reference.width >= 128 &&
        quality.psnr < 40) {
      const better = writeNgtex({
        mips: compressMips([reference], NGTEX_FORMAT_BC7),
        semantic,
        format: NGTEX_FORMAT_BC7,
        srgb: true,
      });
      const betterQuality = qualityFor(semantic, reference, decodeFirstMip(readNgtex(better), semantic));
      if (betterQuality.psnr >= quality.psnr + 1.5 && betterQuality.psnr >= 30) {
        return { format: NGTEX_FORMAT_BC7, srgb: true, quality: betterQuality, autoBc7: true };
      }
    }
    return { ...preferred, quality };
  }
  // Falling back to RGBA8 keeps the semantic correct (sRGB for colour) while losing size.
  return {
    format: NGTEX_FORMAT_RGBA8,
    srgb: semantic === 'albedo' || semantic === 'emissive',
    quality,
    fallback: true,
  };
}

function candidateFor(source, semantic, width, height, requestedFormat) {
  const resized = resizeRgba(source, width, height, semantic);
  const mips = makeMips(resized, semantic);
  const choice = chooseFormat(semantic, mips[0], requestedFormat);
  const container = writeNgtex({
    mips: compressMips(mips, choice.format),
    semantic,
    format: choice.format,
    srgb: choice.srgb,
  });
  const decoded = decodeFirstMip(readNgtex(container), semantic);
  const upscaled = resizeRgba(
    { width, height, data: decoded },
    source.width,
    source.height,
    semantic,
  );
  const quality = qualityFor(semantic, source, upscaled.data);
  const rawScore = (semantic === 'normal' || semantic === 'coatNormal') ? -quality.p95 : quality.psnr;
  const score = Number.isFinite(rawScore) ? rawScore : 100;
  if (!Number.isFinite(quality.psnr)) quality.psnr = 100;
  return {
    size: Math.max(width, height),
    width,
    height,
    mips: mips.length,
    format: choice.format,
    formatName: NGTEX_FORMATS[choice.format],
    srgb: choice.srgb,
    container,
    bytes: container.length,
    quality,
    score,
    fallback: choice.fallback ?? false,
  };
}

function candidateForR16(source, width, height) {
  const resized = resizeR16(source, width, height);
  const mips = makeMipsR16(resized).map((mip) => ({ width: mip.width, height: mip.height, data: r16Buffer(mip) }));
  const container = writeNgtex({ mips, semantic: 'height', format: NGTEX_FORMAT_R16, srgb: false });
  return {
    size: Math.max(width, height), width, height, mips: mips.length,
    format: NGTEX_FORMAT_R16, formatName: NGTEX_FORMATS[NGTEX_FORMAT_R16], srgb: false,
    container, bytes: container.length, quality: { metric: 'r16-exact', psnr: 100 }, score: 100, fallback: false,
  };
}

/** Cook every *.png in a directory into .ngtex plus a manifest and a report. */
function writeChanged(file, bytes) {
  const content=Buffer.isBuffer(bytes)?bytes:Buffer.from(bytes);
  if(existsSync(file) && readFileSync(file).equals(content)) return;
  const temporary=`${file}.${process.pid}.tmp`;
  writeFileSync(temporary,content);
  renameSync(temporary,file);
}

export function cookDirectory({ source, out, size = 512, format = 'auto', budgetBytes = null, incremental = true, preserveAspect = false }) {
  if (!existsSync(source)) throw new Error(`source directory does not exist: ${source}`);
  mkdirSync(out, { recursive: true });
  const files = readdirSync(source).filter((name) => name.endsWith('.png')).sort();
  if (!files.length) throw new Error('no PNG files in ' + source);
  const sourceBytes = new Map(files.map((file) => [file, readFileSync(join(source, file))]));
  const fingerprint = createHash('sha256').update(JSON.stringify({
    version: 3, compiler: COOK_COMPILER, size, format, budgetBytes, preserveAspect,
    files: files.map((file) => [file, createHash('sha256').update(sourceBytes.get(file)).digest('hex')]),
  })).digest('hex');
  const cachePath = join(out, '.cook-cache.json');
  if (incremental && existsSync(cachePath)) {
    try {
      const cache = JSON.parse(readFileSync(cachePath, 'utf8'));
      const valid = cache.fingerprint === fingerprint && cache.manifest && Array.isArray(cache.textures)
        && cache.textures.every((entry) => {
          const file = join(out, entry.file);
          return existsSync(file)
            && createHash('sha256').update(readFileSync(file)).digest('hex') === entry.sha256;
        });
      if (valid) {
        writeChanged(join(out,'manifest.json'),`${JSON.stringify(cache.manifest,null,2)}\n`);
        return { manifest: cache.manifest, textures: cache.textures, cacheHit: true, reusedTextures: cache.textures.length, cookedTextures: 0 };
      }
    } catch { /* Corrupt cache: fall through to a full cook. */ }
  }
  const smallest = Math.min(16, size);
  const textures = [];
  const candidatesByFile = new Map();
  const decisions = [];
  const itemCacheDirectory=join(out,'.cook-items');
  if(incremental) mkdirSync(itemCacheDirectory,{recursive:true});
  let reusedTextures=0, cookedTextures=0;
  let selectedBytes = 0;
  let overBudget = false;
  for (const file of files) {
    const { name, semantic } = textureName(file);
    const sourcePath = join(source, file);
    const itemKey=createHash('sha256').update(JSON.stringify({version:3,compiler:COOK_COMPILER,size,format,semantic,preserveAspect})).update(sourceBytes.get(file)).digest('hex');
    const itemFile=join(itemCacheDirectory,`${itemKey}.json`);
    let candidates, stats, decodedWidth, decodedHeight, ladder;
    if(incremental && existsSync(itemFile)) {
      try {
        const saved=JSON.parse(readFileSync(itemFile,'utf8'));
        if(saved.key!==itemKey || !Array.isArray(saved.candidates) || !saved.candidates.length) throw new Error('Invalid cache');
        candidates=saved.candidates.map(c=>({...c,container:Buffer.from(c.container,'base64')}));
        for(const c of candidates) {
          if(createHash('sha256').update(c.container).digest('hex')!==c.hash) throw new Error('Corrupt cache');
          readNgtex(c.container);
        }
        stats=saved.stats;decodedWidth=saved.width;decodedHeight=saved.height;ladder=saved.ladder;
        reusedTextures++;
      } catch {candidates=null;}
    }
    if(!candidates) {
      const bytes=sourceBytes.get(file);
      const r16=semantic==='height'&&isR16HeightSource(bytes);
      if(format==='r16'&&!r16)throw new Error(`${file}: r16 requires a 16-bit grayscale source PNG`);
      if(semantic==='height'&&r16&&format!=='auto'&&format!=='r16')throw new Error(`${file}: a 16-bit height source must use the r16 format (use --format auto)`);
      const decodedWidthHeight=r16?decodePng16Gray(bytes,file):decodePng(bytes,file);
      decodedWidth=decodedWidthHeight.width;decodedHeight=decodedWidthHeight.height;
      const dims=targetDimensions(decodedWidth,decodedHeight,size,preserveAspect);
      ladder=[];
      for(let rung=0;rung<3;rung+=1){
        const width=Math.max(1,dims.width>>rung),height=Math.max(1,dims.height>>rung);
        if(ladder.some(entry=>entry.width===width&&entry.height===height))continue;
        if(Math.max(width,height)<smallest&&ladder.length)continue;
        ladder.push({width,height});
      }
      if(r16){
        const normalized=resizeR16(decodedWidthHeight,dims.width,dims.height);
        candidates=ladder.map(rung=>candidateForR16(normalized,rung.width,rung.height));
        stats=r16Stats(normalized);
      }else{
        const normalized=resizeRgba(decodedWidthHeight,dims.width,dims.height,semantic);
        candidates=ladder.map(rung=>candidateFor(normalized,semantic,rung.width,rung.height,format));
        stats=analyze(normalized,semantic);
      }
      cookedTextures++;
      if(incremental) writeChanged(itemFile,JSON.stringify({key:itemKey,width:decodedWidth,height:decodedHeight,ladder,stats,candidates:candidates.map(c=>({...c,hash:createHash('sha256').update(c.container).digest('hex'),container:c.container.toString('base64')}))}));
    }
    candidatesByFile.set(file,candidates);
    // Start from the best quality candidate; the budget pass only moves down the ladder.
    const selected = 0;
    const chosen = candidates[selected];
    selectedBytes += chosen.bytes;
    const outputName = `${name}.${semantic}.ngtex`;
    writeChanged(join(out, outputName), chosen.container);
    textures.push({
      name,
      semantic,
      file: outputName,
      source: file,
      width: chosen.width,
      height: chosen.height,
      sourceWidth: decodedWidth,
      sourceHeight: decodedHeight,
      mips: chosen.mips,
      bytes: chosen.bytes,
      format: chosen.formatName,
      formatCode: chosen.format,
      srgb: chosen.srgb,
      quality: chosen.quality,
      fallback: chosen.fallback,
      candidateIndex: selected,
      candidateCount: candidates.length,
      candidates: candidates.map((candidate) => ({
        size: candidate.size,
        width: candidate.width,
        height: candidate.height,
        format: candidate.formatName,
        bytes: candidate.bytes,
        quality: candidate.quality,
      })),
      sha256: createHash('sha256').update(chosen.container).digest('hex'),
      stats,
    });
  }
  if (budgetBytes !== null && selectedBytes > budgetBytes) {
    // Greedy downgrade: move the texture whose next rung loses the least quality per byte
    // saved. This is deterministic and explainable, not a claim of global optimality.
    for (;;) {
      selectedBytes = textures.reduce((total, entry) => total + entry.bytes, 0);
      if (selectedBytes <= budgetBytes) break;
      let bestIndex = -1;
      let bestRatio = Infinity;
      let bestSaving = 0;
      for (let index = 0; index < textures.length; index += 1) {
        const entry = textures[index];
        if (entry.candidateIndex + 1 >= entry.candidateCount) continue;
        const current = entry.candidates[entry.candidateIndex];
        const next = entry.candidates[entry.candidateIndex + 1];
        const saving = current.bytes - next.bytes;
        if (saving <= 0) continue;
        const loss = current.quality.psnr !== undefined
          ? current.quality.psnr - next.quality.psnr
          : next.quality.p95 - current.quality.p95;
        const ratio = loss / saving;
        if (ratio < bestRatio) {
          bestRatio = ratio;
          bestIndex = index;
          bestSaving = saving;
        }
      }
      if (bestIndex < 0) {
        overBudget = true;
        break;
      }
      const entry = textures[bestIndex];
      entry.candidateIndex += 1;
      const candidate = entry.candidates[entry.candidateIndex];
      entry.width = candidate.width;
      entry.height = candidate.height;
      entry.mips = candidate.mips;
      entry.bytes = candidate.bytes;
      entry.format = candidate.format;
      entry.quality = candidate.quality;
      // Rebuild the selected container at the new rung.
      const rebuilt = candidatesByFile.get(entry.source)[entry.candidateIndex];
      writeChanged(join(out, entry.file), rebuilt.container);
      entry.formatCode = rebuilt.format;
      entry.format = rebuilt.formatName;
      entry.srgb = rebuilt.srgb;
      entry.sha256 = createHash('sha256').update(rebuilt.container).digest('hex');
      entry.quality = rebuilt.quality;
      entry.bytes = rebuilt.bytes;
      selectedBytes -= bestSaving;
      decisions.push({
        texture: `${entry.name}.${entry.semantic}`,
        from: { size: entry.candidates[entry.candidateIndex - 1].size, bytes: entry.candidates[entry.candidateIndex - 1].bytes },
        to: { size: candidate.size, bytes: candidate.bytes },
        reason: 'budget',
      });
    }
  }
  const manifest = {
    schema: 'nativegpu.textures/1',
    size,
    preserveAspect,
    budgetBytes,
    totalBytes: textures.reduce((total, entry) => total + entry.bytes, 0),
    overBudget,
    decisions,
    textures: textures.map(({ stats, candidates, ...entry }) => entry),
  };
  writeChanged(join(out, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  if (incremental) {
    writeFileSync(cachePath, `${JSON.stringify({ schema: 'nativegpu.texture-cache/2', fingerprint, manifest, textures }, null, 2)}\n`);
  }
  return { manifest, textures, cacheHit: false, reusedTextures, cookedTextures };
}

function main(argv) {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
  const command = argv[0] ?? 'cook';
  const options = {};
  for (let index = 1; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg.startsWith('--')) {
      const key = arg.slice(2);
      const value = argv[index + 1] && !argv[index + 1].startsWith('--') ? argv[++index] : 'true';
      options[key] = value;
    }
  }
  if (command === 'cook') {
    const source = resolve(root, options.source ?? 'assets/source/textures');
    const out = resolve(root, options.out ?? 'assets/textures');
    const size = Number(options.size ?? 512);
    const format = options.format ?? 'auto';
    const budgetMiB = options['budget-mib'] ? Number(options['budget-mib']) : null;
    const budgetBytes = budgetMiB === null ? null : Math.round(budgetMiB * 1048576);
    const preserveAspect = options['preserve-aspect'] !== undefined;
    const { manifest, textures, cacheHit } = cookDirectory({ source, out, size, format, budgetBytes, preserveAspect });
    const reportDir = resolve(root, options.report ?? 'reports/textures');
    mkdirSync(reportDir, { recursive: true });
    writeFileSync(join(reportDir, 'cook.json'), `${JSON.stringify({
      schema: 'nativegpu.texture-cook/1',
      source,
      out,
      size,
      format,
      preserveAspect,
      budgetBytes,
      cacheHit,
      totalBytes: manifest.totalBytes,
      overBudget: manifest.overBudget,
      decisions: manifest.decisions,
      textures,
    }, null, 2)}\n`);
    const suffix = manifest.overBudget ? ' (over budget)' : '';
    console.log(`cooked ${textures.length} textures, ${(manifest.totalBytes / 1048576).toFixed(2)} MiB${suffix} -> ${out}${cacheHit ? ' (cache hit)' : ''}`);
    return 0;
  }
  if (command === 'inspect') {
    const files = argv.slice(1).filter((arg) => !arg.startsWith('--'));
    if (!files.length) throw new Error('inspect needs at least one .ngtex path');
    for (const file of files) {
      const texture = readNgtex(readFileSync(resolve(file)), file);
      console.log(`${basename(file)}: ${texture.width}x${texture.height} ${texture.semantic} ` +
        `${texture.mipCount} mips ${texture.bytes} bytes`);
    }
    return 0;
  }
  throw new Error(`unknown command ${command}`);
}

const isMain = process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}

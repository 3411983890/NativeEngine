import * as fs from 'node:fs/promises';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

// Original procedural needle atlas.
//
// The conifer had no foliage texture at all: its leaf cards fell back to vertex colour and had no
// alpha cutout, so at close range they were opaque rectangles. There is no photographed needle set
// in this project and inventing one from the oak leaf sheet would be a false label, so this tool
// draws the atlas from geometry instead: for each cell, a twig and two rows of needles at a
// species-typical angle, anti-aliased, with straight alpha.
//
// Output: `assets/vegetation/textures/generated/needle_atlas.png` (baseColour + alpha) and
// `needle_atlas_opacity.png` (coverage). Both share one 4x4 cell grid, which is the convention
// `atlas.mjs` and the shader both assume.

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.join(root, 'assets/vegetation/textures/generated');
const columns = 4, rows = 4, cell = 128;
const width = columns * cell, height = rows * cell;

// --------------------------------------------------------------- PNG encoding
const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; ++index) {
    let value = index;
    for (let bit = 0; bit < 8; ++bit) value = (value & 1) ? (0xEDB88320 ^ (value >>> 1)) : (value >>> 1);
    table[index] = value >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([length, body, crc]);
}

function encodePng(rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; ++y) {
    raw[y * (w * 4 + 1)] = 0; // filter: none
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(w, 0);
  header.writeUInt32BE(h, 4);
  header[8] = 8;   // bit depth
  header[9] = 6;   // colour type RGBA
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// --------------------------------------------------------------- drawing
function makeCanvas(w, h) {
  return { w, h, data: new Uint8Array(w * h * 4) };
}

// Soft round dot: the anti-aliasing is a 1-texel falloff, which is enough because the cook's mip
// chain and the shader's alpha test both act on coverage, not on the atlas edge.
function dot(canvas, x, y, radius, colour, alpha) {
  const x0 = Math.max(0, Math.floor(x - radius)), x1 = Math.min(canvas.w - 1, Math.ceil(x + radius));
  const y0 = Math.max(0, Math.floor(y - radius)), y1 = Math.min(canvas.h - 1, Math.ceil(y + radius));
  for (let py = y0; py <= y1; ++py) {
    for (let px = x0; px <= x1; ++px) {
      const distance = Math.hypot(px + 0.5 - x, py + 0.5 - y);
      const coverage = Math.max(0, Math.min(1, radius - distance + 0.5));
      if (coverage <= 0) continue;
      const index = (py * canvas.w + px) * 4;
      const a = coverage * alpha;
      const existing = canvas.data[index + 3] / 255;
      const out = a + existing * (1 - a);
      if (out <= 0) continue;
      for (let channel = 0; channel < 3; ++channel) {
        canvas.data[index + channel] = Math.round(
          (colour[channel] * a + canvas.data[index + channel] * existing * (1 - a)) / out);
      }
      canvas.data[index + 3] = Math.round(out * 255);
    }
  }
}

function stroke(canvas, x0, y0, x1, y1, radius, colour, alpha) {
  const length = Math.hypot(x1 - x0, y1 - y0);
  const steps = Math.max(1, Math.ceil(length * 2));
  for (let step = 0; step <= steps; ++step) {
    const t = step / steps;
    dot(canvas, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, radius, colour, alpha);
  }
}

// A deterministic stream so the atlas is reproducible byte for byte.
function stream(seed) {
  let state = seed >>> 0;
  return {
    unit() {
      state ^= state << 13; state >>>= 0;
      state ^= state >> 17;
      state ^= state << 5; state >>>= 0;
      return state / 4294967296;
    },
    span(low, high) { return low + (high - low) * this.unit(); },
  };
}

function drawNeedleCluster(canvas, originX, originY, rng) {
  // Twig: a slightly curved stem through the cell.
  const twigAngle = rng.span(-0.45, 0.45);
  const twigLength = cell * rng.span(0.62, 0.8);
  const tipX = originX + Math.sin(twigAngle) * twigLength;
  const tipY = originY - Math.cos(twigAngle) * twigLength;
  const barkColour = [96, 78, 56];
  stroke(canvas, originX, originY, tipX, tipY, 2.2, barkColour, 1);
  // Needles: two ranks along the twig, angled back toward the base at a shallow angle, which is
  // what a pine fascicle looks like from the side.
  const pairs = 16;
  for (let pair = 0; pair < pairs; ++pair) {
    const t = 0.12 + (pair / (pairs - 1)) * 0.86;
    const alongX = originX + (tipX - originX) * t;
    const alongY = originY + (tipY - originY) * t;
    for (const side of [-1, 1]) {
      const angle = twigAngle + side * rng.span(0.85, 1.15);
      const length = cell * rng.span(0.22, 0.34) * (1 - 0.35 * t);
      const endX = alongX + Math.sin(angle) * length;
      const endY = alongY - Math.cos(angle) * length;
      // Needle colour: dark at the base, lighter at the tip; the blue channel is kept low so the
      // sheet reads as foliage rather than as grass.
      const shade = rng.span(0.7, 1.05);
      const colour = [Math.round(54 * shade), Math.round(96 * shade), Math.round(44 * shade)];
      stroke(canvas, alongX, alongY, endX, endY, rng.span(0.9, 1.4), colour, 1);
      // Fascicle tip highlight: a slightly brighter dot reads as a needle tip at close range.
      dot(canvas, endX, endY, 1.1, [Math.round(96 * shade), Math.round(140 * shade), Math.round(62 * shade)], 0.9);
    }
  }
}

async function main() {
  await fs.mkdir(output, { recursive: true });
  const canvas = makeCanvas(width, height);
  const opacity = makeCanvas(width, height);
  let cellIndex = 0;
  for (let row = 0; row < rows; ++row) {
    for (let column = 0; column < columns; ++column) {
      const rng = stream(0x51ED + cellIndex * 7919);
      const originX = column * cell + cell * rng.span(0.34, 0.66);
      const originY = row * cell + cell * rng.span(0.82, 0.95);
      drawNeedleCluster(canvas, originX, originY, rng);
      ++cellIndex;
    }
  }
  // The opacity sheet is the colour sheet's alpha, not a second drawing: two drawings would drift
  // apart on the first parameter change and the cutout would stop matching the visible needles.
  for (let index = 0; index < width * height * 4; ++index) {
    opacity.data[index + 0] = canvas.data[index + 3];
    opacity.data[index + 1] = canvas.data[index + 3];
    opacity.data[index + 2] = canvas.data[index + 3];
    opacity.data[index + 3] = 255;
  }
  const colourPng = encodePng(canvas.data, width, height);
  const opacityPng = encodePng(opacity.data, width, height);
  await fs.writeFile(path.join(output, 'needle_atlas.png'), colourPng);
  await fs.writeFile(path.join(output, 'needle_atlas_opacity.png'), opacityPng);
  let covered = 0;
  for (let index = 3; index < canvas.data.length; index += 4) if (canvas.data[index] >= Math.round(0.35 * 255)) ++covered;
  console.log(JSON.stringify({
    schema: 'nativegpu.vegetation-needle-atlas/1',
    directory: path.relative(root, output).split(path.sep).join('/'),
    width, height, columns, rows,
    colourBytes: colourPng.length, opacityBytes: opacityPng.length,
    coverageAtCutoff035: Number((covered / (width * height)).toFixed(4)),
  }));
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });

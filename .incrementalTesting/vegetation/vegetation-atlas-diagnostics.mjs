import * as fs from 'node:fs/promises';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

// Numbered atlas diagnostic.
//
// §6.2 asks for a corner/numbering diagnostic image to verify the V direction, the cell rects and
// the half-texel mapping. A numeric test can prove the rect arithmetic; only a picture can show
// that row 0 lands at the top of the leaf card and that the digits are not mirrored.
//
// Each cell carries:
//   * its index as digits, drawn upright in the cell's own frame;
//   * four corner markers: top-left red, top-right green, bottom-left blue, bottom-right yellow;
//   * a 1-px border, so a cell edge that samples a neighbour is visible as a colour fringe.
//
// Output goes to assets/vegetation/textures/generated/ and the species fixture is written to
// build/vegetation/source so nothing here is mistaken for authored content.

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const output = path.join(root, 'assets/vegetation/textures/generated');
// The diagnostic species is cooked into its own directory. It must not join the main content
// library: a region's species table is the whole library, so an extra species there changes what
// every region records and breaks the "region references four species" contract.
const sourceDirectory = path.join(root, 'build/vegetation/diagnostics');
const columns = 3, rows = 4, size = 512;

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
function encodePng(rgba, width, height) {
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; ++y) {
    raw[y * (width * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4).copy(raw, y * (width * 4 + 1) + 1);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 6;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// --------------------------------------------------------------- drawing
function makeCanvas(width, height) {
  return { width, height, data: new Uint8Array(width * height * 4) };
}
function pixel(canvas, x, y, colour) {
  if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) return;
  const index = (y * canvas.width + x) * 4;
  canvas.data[index] = colour[0];
  canvas.data[index + 1] = colour[1];
  canvas.data[index + 2] = colour[2];
  canvas.data[index + 3] = colour.length > 3 ? colour[3] : 255;
}
function rect(canvas, x0, y0, x1, y1, colour) {
  for (let y = Math.round(y0); y < Math.round(y1); ++y) {
    for (let x = Math.round(x0); x < Math.round(x1); ++x) pixel(canvas, x, y, colour);
  }
}

// 3x5 digit font. Written as rows so a digit reads the same way the shader's V axis does: the
// first row is the top of the glyph, which must end up at the top of the leaf card.
const FONT = {
  0: ['111', '101', '101', '101', '111'],
  1: ['010', '110', '010', '010', '111'],
  2: ['111', '001', '111', '100', '111'],
  3: ['111', '001', '111', '001', '111'],
  4: ['101', '101', '111', '001', '001'],
  5: ['111', '100', '111', '001', '111'],
  6: ['111', '100', '111', '101', '111'],
  7: ['111', '001', '010', '010', '010'],
  8: ['111', '101', '111', '101', '111'],
  9: ['111', '101', '111', '001', '111'],
};

function drawDigits(canvas, text, originX, originY, scale, colour) {
  let cursor = originX;
  for (const character of text) {
    const glyph = FONT[character];
    if (!glyph) { cursor += 4 * scale; continue; }
    for (let row = 0; row < 5; ++row) {
      for (let column = 0; column < 3; ++column) {
        if (glyph[row][column] !== '1') continue;
        rect(canvas, cursor + column * scale, originY + row * scale,
          cursor + (column + 1) * scale, originY + (row + 1) * scale, colour);
      }
    }
    cursor += 4 * scale;
  }
}

async function main() {
  await fs.mkdir(output, { recursive: true });
  await fs.mkdir(sourceDirectory, { recursive: true });
  const colour = makeCanvas(size, size);
  const opacity = makeCanvas(size, size);

  for (let row = 0; row < rows; ++row) {
    for (let column = 0; column < columns; ++column) {
      const index = row * columns + column;
      // Cell rects use the same fractional expression the atlas module does, so the diagnostic
      // shows the real edges rather than their rounded stand-ins.
      const x0 = (column * size) / columns, x1 = ((column + 1) * size) / columns;
      const y0 = (row * size) / rows, y1 = ((row + 1) * size) / rows;
      // Background: a per-cell hue so a colour bleeding in from a neighbour is obvious.
      const hue = index / (columns * rows);
      const background = [
        Math.round(90 + 120 * Math.abs(Math.sin(hue * Math.PI * 2))),
        Math.round(90 + 120 * Math.abs(Math.sin(hue * Math.PI * 2 + 2.1))),
        Math.round(90 + 120 * Math.abs(Math.sin(hue * Math.PI * 2 + 4.2))),
        255,
      ];
      rect(colour, x0, y0, x1, y1, background);

      const marker = Math.max(6, Math.round((x1 - x0) * 0.14));
      // Corner markers. In UV space the top of the cell is the *higher* v, which the shader maps
      // to the top of the card; if the sheet were flipped these four colours would swap ends.
      rect(colour, x0, y0, x0 + marker, y0 + marker, [255, 40, 40, 255]);      // top-left red
      rect(colour, x1 - marker, y0, x1, y0 + marker, [40, 220, 60, 255]);      // top-right green
      rect(colour, x0, y1 - marker, x0 + marker, y1, [60, 90, 255, 255]);      // bottom-left blue
      rect(colour, x1 - marker, y1 - marker, x1, y1, [250, 220, 40, 255]);     // bottom-right yellow

      // Index digits, centred.
      const scale = Math.max(2, Math.round((x1 - x0) / 34));
      const text = String(index);
      const width = text.length * 4 * scale - scale;
      drawDigits(colour, text, (x0 + x1) / 2 - width / 2, (y0 + y1) / 2 - (5 * scale) / 2, scale, [16, 16, 16, 255]);

      // 1-px border, drawn last so it sits on top of the background.
      rect(colour, x0, y0, x1, y0 + 1, [8, 8, 8, 255]);
      rect(colour, x0, y1 - 1, x1, y1, [8, 8, 8, 255]);
      rect(colour, x0, y0, x0 + 1, y1, [8, 8, 8, 255]);
      rect(colour, x1 - 1, y0, x1, y1, [8, 8, 8, 255]);

      rect(opacity, x0, y0, x1, y1, [255, 255, 255, 255]);
    }
  }

  const colourPng = encodePng(colour.data, size, size);
  const opacityPng = encodePng(opacity.data, size, size);
  await fs.writeFile(path.join(output, 'atlas_diagnostics.png'), colourPng);
  await fs.writeFile(path.join(output, 'atlas_diagnostics_opacity.png'), opacityPng);

  // A species that exists only to be rendered for this check. It lives in build/ so it is never
  // mistaken for authored content, and it uses the same generator defaults as a broadleaf.
  const species = {
    schema: 'nativegpu.vegetation-species/1',
    id: 'atlas_diag',
    kind: 'tree',
    profile: 'broadleaf',
    seed: 4242,
    dimensions: { heightMetres: 6, crownRadiusMetres: 2.4, trunkRadiusMetres: 0.16, leafWidthMetres: 0.5, leafHeightMetres: 0.5 },
    lod: { levels: 3, screenErrorPixels: [0.4, 2.5, 9] },
    wind: { amplitudeMetres: 0.06, frequencyHz: 0.5, stiffness: 0.6 },
    materials: [
      { id: 'bark', kind: 'bark', alphaMode: 'opaque', doubleSided: false },
      { id: 'leaf', kind: 'leaf', alphaMode: 'mask', alphaCutoff: 0.35, doubleSided: true },
    ],
    budget: { maxVertices: 400000, maxTriangles: 400000 },
    impostor: { views: 4, size: 64 },
    textures: { leaf: { baseColor: 'diag_basecolor', opacity: 'diag_opacity' } },
  };
  await fs.writeFile(path.join(sourceDirectory, 'atlas_diag.species.json'), `${JSON.stringify(species, null, 2)}\n`);

  console.log(JSON.stringify({
    schema: 'nativegpu.vegetation-atlas-diagnostics/1',
    directory: path.relative(root, output).split(path.sep).join('/'),
    species: path.relative(root, sourceDirectory).split(path.sep).join('/') + '/atlas_diag.species.json',
    size, columns, rows, colourBytes: colourPng.length, opacityBytes: opacityPng.length,
  }));
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });

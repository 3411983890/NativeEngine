import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import sharp from 'sharp';
import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';

// Vegetation texture cook.
//
// The native renderer has no PNG decoder and should not grow one: decoding a PNG at load
// time would put a deflate implementation and an error surface into the frame path. So
// textures are cooked here into a raw, header-prefixed package (`NGT3`) that the loader
// can validate and upload directly.
//
// The masked leaf atlas needs one extra step. The supplied leaf sheets are leaves drawn on
// a flat background; sampling that background at a card edge produces a bright fringe. When
// a role declares a `mask`, colour is dilated into the masked-out region so bilinear
// filtering never reads the background.
//
// NGT3 adds a mip chain. A single-level sheet is not usable at distance: the sampler
// minifies the mask with nearest-ish filtering, leaves dissolve into holes, and the same
// texel changes its answer every frame as the camera moves. The mask chain is not a plain
// box filter either — averaging a binary cutout erodes it level by level, which is the same
// hole by another route. Each mask level is therefore remapped so that the fraction of
// texels that survive the alpha cutoff stays equal to the base level's; see
// `remapCoverage`.

export const TEXTURE_MAGIC = 'NGT3';
export const TEXTURE_VERSION = 2;
const HEADER_BYTES = 32;
const MAX_LEVELS = 12;
// How a level is reduced. `color` averages in linear light, `normal` renormalises,
// `coverage` preserves the alpha-test coverage, `data` is a plain box filter.
const FILTERS = new Set(['color', 'normal', 'coverage', 'data']);

export class TextureError extends Error {
  constructor(code, message) { super(message); this.name = 'TextureError'; this.code = code; }
}
const fail = (ok, message, code = 'NG-E-VEG-FORMAT') => { if (!ok) throw new TextureError(code, message); };

function packTexture({ srgb, levels }) {
  fail(levels.length >= 1 && levels.length <= MAX_LEVELS, 'texture needs 1–12 mip levels');
  const width = levels[0].width, height = levels[0].height;
  fail(width > 0 && height > 0 && width <= 4096 && height <= 4096, 'texture dimensions out of range');
  let payloadBytes = 0;
  for (const [level, entry] of levels.entries()) {
    fail(entry.width > 0 && entry.height > 0, `mip ${level} has no extent`);
    fail(entry.pixels.byteLength === entry.width * entry.height * 4, `mip ${level} payload disagrees with its dimensions`);
    payloadBytes += entry.pixels.byteLength;
  }
  const buffer = Buffer.alloc(HEADER_BYTES + payloadBytes);
  buffer.write(TEXTURE_MAGIC, 0, 'latin1');
  buffer.writeUInt32LE(TEXTURE_VERSION, 4);
  buffer.writeUInt32LE(width, 8);
  buffer.writeUInt32LE(height, 12);
  buffer.writeUInt32LE(0, 16); // format 0 = RGBA8
  buffer.writeUInt32LE(srgb ? 1 : 0, 20);
  buffer.writeUInt32LE(levels.length, 24);
  buffer.writeUInt32LE(payloadBytes, 28);
  // Levels are concatenated in order, each `width>>level` by `height>>level`. No offset
  // table: the reader can derive every offset from the count, which is one less thing that
  // can disagree with the header.
  let offset = HEADER_BYTES;
  for (const entry of levels) {
    Buffer.from(entry.pixels.buffer, entry.pixels.byteOffset, entry.pixels.byteLength).copy(buffer, offset);
    offset += entry.pixels.byteLength;
  }
  return buffer;
}

function srgbToLinear(value) {
  const v = value / 255;
  return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
}
function linearToSrgb(value) {
  const v = Math.min(1, Math.max(0, value));
  const s = v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
  return Math.round(Math.min(1, Math.max(0, s)) * 255);
}

// Fraction of texels whose red channel survives the alpha cutoff. Red is the channel the
// vegetation shader reads for the leaf mask; measuring a different one would report a
// coverage the runtime never sees.
function coverageRatio(pixels, cutoffByte) {
  let covered = 0;
  for (let index = 0; index < pixels.length; index += 4) if (pixels[index] >= cutoffByte) ++covered;
  return covered / (pixels.length / 4);
}

// Expands the mask until it covers as much of the level as the base level did. Averaging
// alone erodes a binary cutout: at 1/16 the average of four leaves is often below the
// cutoff everywhere, so the patch disappears and the crown is full of holes at range.
//
// The correction is an offset, not a scale. Scaling about the cutoff is a no-op for
// coverage — `cutoff + (a - cutoff) * s >= cutoff` holds exactly when `a >= cutoff`, for
// every s > 0 — so only a shift moves texels across the threshold. The offset is solved by
// bisection rather than fixed, because how much expansion a level needs depends on how much
// of it is edge (a thin fern cutout needs far more than a solid card).
function remapCoverage(pixels, cutoffByte, target) {
  const cutoff = cutoffByte / 255;
  const measure = (offset) => {
    let covered = 0;
    for (let index = 0; index < pixels.length; index += 4) if (pixels[index] / 255 + offset >= cutoff) ++covered;
    return covered / (pixels.length / 4);
  };
  // Only expand. A level that did not erode is left exactly as the box filter produced it,
  // so this never turns a correctly thin cutout into a solid quad.
  if (target <= 0 || measure(0) >= target) return;
  const ceiling = cutoff; // below this, the averaged background can never reach the cutoff
  let low = 0, high = ceiling, offset = ceiling;
  if (measure(high) <= target) {
    offset = high;
  } else {
    for (let step = 0; step < 24; ++step) {
      const middle = (low + high) / 2;
      if (measure(middle) < target) low = middle; else high = middle;
    }
    offset = high;
  }
  const shift = Math.round(offset * 255);
  for (let index = 0; index < pixels.length; index += 4) {
    pixels[index] = Math.min(255, pixels[index] + shift);
    pixels[index + 1] = Math.min(255, pixels[index + 1] + shift);
    pixels[index + 2] = Math.min(255, pixels[index + 2] + shift);
  }
}

// Per-cell coverage correction for an atlas. An atlas has one coverage target per cell, not one per
// sheet: a sparse cell and a dense cell in the same image need different offsets, and a single
// sheet-wide offset can only satisfy their average. Each cell's rect is corrected with that cell's
// own base coverage, so no cell is filled in or eroded on behalf of its neighbours.
function remapCoverageCells(out, width, height, cutoffByte, targets) {
  for (let index = 0; index < targets.length; ++index) {
    if (targets[index] <= 0) continue;
    const rect = cellRect(index, width, height, targets.atlas);
    const x0 = Math.max(0, rect.x0), x1 = Math.min(width - 1, rect.x1);
    const y0 = Math.max(0, rect.y0), y1 = Math.min(height - 1, rect.y1);
    const cellWidth = x1 - x0 + 1, cellHeight = y1 - y0 + 1;
    if (cellWidth <= 0 || cellHeight <= 0) continue;
    const cell = new Uint8Array(cellWidth * cellHeight * 4);
    for (let y = 0; y < cellHeight; ++y) {
      for (let x = 0; x < cellWidth; ++x) {
        const from = ((y0 + y) * width + (x0 + x)) * 4;
        const to = (y * cellWidth + x) * 4;
        cell[to] = out[from]; cell[to + 1] = out[from + 1]; cell[to + 2] = out[from + 2]; cell[to + 3] = out[from + 3];
      }
    }
    remapCoverage(cell, cutoffByte, targets[index]);
    for (let y = 0; y < cellHeight; ++y) {
      for (let x = 0; x < cellWidth; ++x) {
        const to = ((y0 + y) * width + (x0 + x)) * 4;
        const from = (y * cellWidth + x) * 4;
        out[to] = cell[from]; out[to + 1] = cell[from + 1]; out[to + 2] = cell[from + 2]; out[to + 3] = cell[from + 3];
      }
    }
  }
}

function cellCoverageTargets(pixels, width, height, atlas, cutoffByte) {
  const targets = [];
  targets.atlas = atlas;
  const cells = atlas.columns * atlas.rows;
  for (let index = 0; index < cells; ++index) {
    const rect = cellRect(index, width, height, atlas);
    let covered = 0, total = 0;
    for (let y = Math.max(0, rect.y0); y <= Math.min(height - 1, rect.y1); ++y) {
      for (let x = Math.max(0, rect.x0); x <= Math.min(width - 1, rect.x1); ++x) {
        if (pixels[(y * width + x) * 4] / 255 >= cutoffByte / 255) ++covered;
        ++total;
      }
    }
    targets.push(total ? covered / total : 0);
  }
  return targets;
}

// Cell rect of a *source* level, in texels. `atlas` is the normalised grid the role declared.
function cellRect(index, width, height, atlas) {
  const column = index % atlas.columns, row = Math.floor(index / atlas.columns);
  return {
    x0: Math.floor((column * width) / atlas.columns),
    x1: Math.ceil(((column + 1) * width) / atlas.columns) - 1,
    y0: Math.floor((row * height) / atlas.rows),
    y1: Math.ceil(((row + 1) * height) / atlas.rows) - 1,
  };
}

// Which cell a destination texel belongs to, from its normalised centre.
function cellAt(x, y, width, height, atlas) {
  const column = Math.min(atlas.columns - 1, Math.floor(((x + 0.5) / width) * atlas.columns));
  const row = Math.min(atlas.rows - 1, Math.floor(((y + 0.5) / height) * atlas.rows));
  return row * atlas.columns + column;
}

function reduceLevel(level, filter, context, atlas = null) {
  const width = Math.max(1, level.width >> 1), height = Math.max(1, level.height >> 1);
  const out = new Uint8Array(width * height * 4);
  const at = (x, y, channel) => level.pixels[(y * level.width + x) * 4 + channel];
  // An atlas is a grid of separate leaves in one image. Filtering across the grid averages one
  // leaf into its neighbour, which shows up as a coloured fringe on every card at distance — the
  // mip level, not the geometry, is where the bleed happens. When the role declares a grid, each
  // destination texel clamps its 2x2 footprint to its own cell, so a cell can only ever average
  // itself.
  const clampCache = atlas ? new Map() : null;
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      let x0 = Math.min(x * 2, level.width - 1), x1 = Math.min(x * 2 + 1, level.width - 1);
      let y0 = Math.min(y * 2, level.height - 1), y1 = Math.min(y * 2 + 1, level.height - 1);
      if (atlas) {
        const index = cellAt(x, y, width, height, atlas);
        const rect = clampCache.get(index) ?? cellRect(index, level.width, level.height, atlas);
        clampCache.set(index, rect);
        // A cell narrower than one texel still has to sample something: collapse to its centre.
        x0 = Math.max(rect.x0, Math.min(rect.x1, x0));
        x1 = Math.max(rect.x0, Math.min(rect.x1, x1));
        y0 = Math.max(rect.y0, Math.min(rect.y1, y0));
        y1 = Math.max(rect.y0, Math.min(rect.y1, y1));
      }
      const index = (y * width + x) * 4;
      if (filter === 'color') {
        // Averaging 8-bit sRGB values directly darkens the result: sRGB is not linear in
        // light, so a half-covered white edge would come out too dark at every level.
        let r = 0, g = 0, b = 0, a = 0;
        for (const [sx, sy] of [[x0, y0], [x1, y0], [x0, y1], [x1, y1]]) {
          r += srgbToLinear(at(sx, sy, 0)); g += srgbToLinear(at(sx, sy, 1)); b += srgbToLinear(at(sx, sy, 2));
          a += at(sx, sy, 3);
        }
        out[index] = linearToSrgb(r / 4);
        out[index + 1] = linearToSrgb(g / 4);
        out[index + 2] = linearToSrgb(b / 4);
        out[index + 3] = Math.round(a / 4);
        continue;
      }
      for (let channel = 0; channel < 4; ++channel) {
        const sum = at(x0, y0, channel) + at(x1, y0, channel) + at(x0, y1, channel) + at(x1, y1, channel);
        out[index + channel] = Math.round(sum / 4);
      }
    }
  }
  if (filter === 'normal') {
    // An averaged normal is not a unit vector. Without this every level flattens the leaf
    // relief and the surface drifts toward the geometric normal.
    for (let index = 0; index < out.length; index += 4) {
      const nx = out[index] / 127.5 - 1, ny = out[index + 1] / 127.5 - 1, nz = out[index + 2] / 127.5 - 1;
      const length = Math.hypot(nx, ny, nz) || 1;
      out[index] = Math.round((nx / length + 1) * 127.5);
      out[index + 1] = Math.round((ny / length + 1) * 127.5);
      out[index + 2] = Math.round((nz / length + 1) * 127.5);
    }
  }
  if (filter === 'coverage') {
    if (context.cellTargets) remapCoverageCells(out, width, height, context.cutoffByte, context.cellTargets);
    else remapCoverage(out, context.cutoffByte, context.targetCoverage);
  }
  return { width, height, pixels: out };
}

// Exported for the mip tests: the coverage invariant is a property of this function, not of
// the PNG pipeline, so it can be checked on a synthetic mask without invoking sharp. `atlas`
// switches to per-cell filtering, which is what stops one leaf averaging into its neighbour.
export function buildMipChain({ pixels, width, height, filter, cutoff = 0, atlas = null }) {
  fail(FILTERS.has(filter), `unknown texture filter ${filter}`);
  fail(pixels.byteLength === width * height * 4, 'mip source disagrees with its dimensions');
  return buildLevels(pixels, width, height, filter, Math.round(cutoff * 255), atlas);
}

function buildLevels(pixels, width, height, filter, cutoffByte, atlas = null) {
  const levels = [{ width, height, pixels }];
  const context = { cutoffByte, targetCoverage: filter === 'coverage' ? coverageRatio(pixels, cutoffByte) : 0 };
  // An atlas preserves coverage per cell; a plain texture preserves it for the whole image.
  if (filter === 'coverage' && atlas) context.cellTargets = cellCoverageTargets(pixels, width, height, atlas, cutoffByte);
  while (levels.length < MAX_LEVELS) {
    const previous = levels[levels.length - 1];
    if (previous.width === 1 && previous.height === 1) break;
    levels.push(reduceLevel(previous, filter, context, atlas));
  }
  return levels;
}

// Nearest-colour dilation into the transparent region.
//
// The frontier has to grow every pass. The previous version tested the *original* mask on
// every pass, so pass two only re-read the same one-pixel ring and the real radius stayed at
// one pixel — which is why a masked atlas edge still sampled background at 512 px. `filled`
// is the growing frontier instead, and `radius` reports what actually happened.
function dilateIntoMask(rgba, mask, width, height, passes = 2) {
  let filled = new Uint8Array(width * height);
  for (let index = 0; index < filled.length; ++index) filled[index] = mask[index * 4] > 32 ? 1 : 0;
  let radius = 0;
  for (let pass = 0; pass < passes; ++pass) {
    const copy = Uint8Array.from(rgba);
    const next = Uint8Array.from(filled);
    let changed = 0;
    for (let y = 0; y < height; ++y) for (let x = 0; x < width; ++x) {
      const index = y * width + x;
      if (filled[index]) continue;
      let r = 0, g = 0, b = 0, count = 0;
      for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) {
        const nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= width || ny >= height || !filled[ny * width + nx]) continue;
        const offset = (ny * width + nx) * 4;
        r += copy[offset]; g += copy[offset + 1]; b += copy[offset + 2]; ++count;
      }
      if (count === 0) continue;
      const offset = index * 4;
      rgba[offset] = Math.round(r / count);
      rgba[offset + 1] = Math.round(g / count);
      rgba[offset + 2] = Math.round(b / count);
      next[index] = 1;
      ++changed;
    }
    filled = next;
    if (changed > 0) radius = pass + 1;
  }
  return radius;
}

// Extracts one channel into a greyscale RGBA image. Used when a role's data lives in a shared
// packed map: the authoring set ships ORM, so a "roughness" role is ORM.G rather than a
// separately painted image, and the semantic stays checkable.
function extractChannel(pixels, channel) {
  const index = { R: 0, G: 1, B: 2, A: 3 }[channel];
  if (index === undefined) return null;
  const out = new Uint8Array(pixels.length);
  for (let offset = 0; offset < pixels.length; offset += 4) {
    const value = pixels[offset + index];
    out[offset] = value; out[offset + 1] = value; out[offset + 2] = value; out[offset + 3] = 255;
  }
  return out;
}

async function readRgba(file, size) {
  const { data, info } = await sharp(file)
    .resize(size, size, { fit: 'fill', kernel: 'lanczos3' })
    .ensureAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });
  fail(info.channels === 4, `texture ${file} did not decode to RGBA`);
  return { pixels: Uint8Array.from(data), width: info.width, height: info.height };
}

// Semantic contract. A role says what its data *is*; the cook refuses a combination that
// cannot be right (an sRGB-decoded normal map, a coverage-filtered colour map) instead of
// producing bytes that silently mean something else.
const SEMANTICS = new Map([
  ['baseColor', { filter: 'color', srgb: true }],
  ['normal', { filter: 'normal', srgb: false }],
  ['orm', { filter: 'data', srgb: false }],
  ['occlusion', { filter: 'data', srgb: false }],
  ['roughness', { filter: 'data', srgb: false }],
  ['metallic', { filter: 'data', srgb: false }],
  ['thickness', { filter: 'data', srgb: false }],
  ['height', { filter: 'data', srgb: false }],
  ['opacity', { filter: 'coverage', srgb: false }],
]);

export async function cookTextureRole(role, definition, sourceDirectory, outputDirectory, rolesDirectory = null) {
  // A role may point at its own directory, resolved relative to the roles file. The oak set came
  // from one image-generation run; the needle atlas is generated by this project's own tool, and
  // mixing the two in one directory would blur which files are author art and which are
  // procedural fixtures.
  const directory = definition.directory && rolesDirectory
    ? path.resolve(rolesDirectory, definition.directory)
    : sourceDirectory;
  const sourcePath = path.join(directory, definition.source);
  const size = definition.size ?? 512;
  fail([128, 256, 512, 1024].includes(size), `role ${role} size must be 128, 256, 512 or 1024`);
  const srgb = definition.srgb === true;
  // The filter is declared per role rather than inferred from the name: a role called
  // "normal" that is not a normal map would silently get renormalised if the cook guessed.
  const filter = definition.filter ?? (srgb ? 'color' : 'data');
  fail(FILTERS.has(filter), `role ${role} has an unknown filter ${filter}`);
  fail(filter !== 'coverage' || Number.isFinite(definition.coverageCutoff), `role ${role} declares coverage filtering without a coverageCutoff`);
  const cutoff = Number.isFinite(definition.coverageCutoff) ? definition.coverageCutoff : 0;
  fail(cutoff >= 0 && cutoff <= 1, `role ${role} coverageCutoff must be in 0..1`);

  const semantic = definition.semantic ?? null;
  if (semantic !== null) {
    const expected = SEMANTICS.get(semantic);
    fail(expected !== undefined, `role ${role} declares unknown semantic ${semantic}`);
    fail(filter === expected.filter, `role ${role} is semantic ${semantic} but filters as ${filter} (expected ${expected.filter})`);
    fail(srgb === expected.srgb, `role ${role} is semantic ${semantic} but colour space ${srgb ? 'srgb' : 'linear'} (expected ${expected.srgb ? 'srgb' : 'linear'})`);
  }
  const sourceChannel = definition.sourceChannel ?? null;
  if (sourceChannel !== null) fail(['R', 'G', 'B', 'A'].includes(sourceChannel), `role ${role} sourceChannel must be R, G, B or A`);

  const image = await readRgba(sourcePath, size);
  let pixels = image.pixels;
  if (sourceChannel !== null) pixels = extractChannel(pixels, sourceChannel);

  let dilationRadius = 0;
  if (definition.mask) {
    const maskImage = await readRgba(path.join(directory, definition.mask), size);
    dilationRadius = dilateIntoMask(pixels, maskImage.pixels, image.width, image.height);
  }

  // An atlas role filters per cell: the grid is a set of separate leaves, and a whole-image
  // box filter averages one leaf into its neighbour at every level below the base.
  const atlasGrid = definition.atlas && Number.isFinite(definition.atlas.columns)
    ? { columns: Math.max(1, Math.round(definition.atlas.columns)), rows: Math.max(1, Math.round(definition.atlas.rows ?? 1)) }
    : null;
  const levels = buildLevels(pixels, image.width, image.height, filter, Math.round(cutoff * 255), atlasGrid);
  const bytes = packTexture({ srgb, levels });
  const digest = createHash('sha256').update(bytes).digest('hex');
  const file = `${digest}.ngt3`;
  await fs.mkdir(outputDirectory, { recursive: true });
  try { await fs.writeFile(path.join(outputDirectory, file), bytes, { flag: 'wx' }); }
  catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = await fs.readFile(path.join(outputDirectory, file));
    if (!existing.equals(bytes)) throw new TextureError('NG-E-VEG-HASH', `texture object conflict: ${file}`);
  }
  return {
    role, file, digest, width: image.width, height: image.height, srgb,
    mipCount: levels.length, filter, bytes: bytes.byteLength,
    semantic, colorSpace: srgb ? 'srgb' : 'linear',
    channels: definition.channels ?? null,
    normalConvention: definition.normalConvention ?? null,
    sourceChannel,
    source: definition.source,
    coverageCutoff: filter === 'coverage' ? cutoff : null,
    atlas: definition.atlas ?? null, tileMetres: definition.tileMetres ?? null,
    masked: Boolean(definition.mask), dilationRadiusPixels: dilationRadius,
  };
}

export async function cookTextureLibrary({ sourceDirectory, outputDirectory, rolesFile }) {
  const roles = JSON.parse((await fs.readFile(rolesFile, 'utf8')).replace(/^\uFEFF/, ''));
  fail(roles.schema === 'nativegpu.vegetation-texture-roles/1', `unsupported roles schema ${roles.schema}`);
  const entries = Object.entries(roles.roles ?? {});
  fail(entries.length >= 1 && entries.length <= 32, 'texture library needs 1–32 roles');
  const cooked = [];
  const rolesDirectory = path.dirname(path.resolve(rolesFile));
  for (const [role, definition] of entries.sort(([a], [b]) => a.localeCompare(b))) {
    fail(definition && typeof definition === 'object', `role ${role} must be an object`);
    cooked.push(await cookTextureRole(role, definition, sourceDirectory, outputDirectory, rolesDirectory));
  }
  const manifest = {
    schema: 'nativegpu.vegetation-texture-library/1',
    roles: cooked.sort((a, b) => a.role.localeCompare(b.role)),
    sourceDigest: sha256Hex(canonicalJson(roles)),
  };
  const payloadJson = canonicalJson(manifest);
  const document = { ...manifest, payloadJson, packageHash: sha256Hex(payloadJson) };
  await fs.writeFile(path.join(outputDirectory, 'library.json'), `${canonicalJson(document)}\n`);
  return { ...document, outputDirectory };
}

// Native loader contract: header validation happens before any pixel is read.
export function readTexturePackage(bytes) {
  fail(bytes.length >= HEADER_BYTES, 'texture package shorter than its header');
  fail(bytes.toString('latin1', 0, 4) === TEXTURE_MAGIC, 'texture magic mismatch');
  fail(bytes.readUInt32LE(4) === TEXTURE_VERSION, 'unsupported texture version');
  const width = bytes.readUInt32LE(8), height = bytes.readUInt32LE(12);
  const format = bytes.readUInt32LE(16), flags = bytes.readUInt32LE(20);
  const mipCount = bytes.readUInt32LE(24);
  const payloadBytes = bytes.readUInt32LE(28);
  fail(format === 0, 'unsupported texture format');
  fail(mipCount >= 1 && mipCount <= MAX_LEVELS, 'texture declares an implausible mip count');
  const levels = [];
  let offset = HEADER_BYTES, expected = 0;
  for (let level = 0; level < mipCount; ++level) {
    const levelWidth = Math.max(1, width >> level), levelHeight = Math.max(1, height >> level);
    const levelBytes = levelWidth * levelHeight * 4;
    expected += levelBytes;
    levels.push({ width: levelWidth, height: levelHeight, pixels: bytes.subarray(offset, offset + levelBytes) });
    offset += levelBytes;
  }
  fail(payloadBytes === expected && bytes.length === HEADER_BYTES + expected, 'texture payload size disagrees with its header');
  return { width, height, srgb: (flags & 1) !== 0, mipCount, levels };
}

import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { decodePng, encodePng, resizeRgba } from './textures.mjs';
import { normalizeLeafSource, LEAF_SOURCE_SCHEMA } from '../engine/vegetation/leaf-source.mjs';

// Normalizes a real leaf image into the canonical slot. This is the import-side integration for
// T1: it reuses the repository's PNG decoder and the engine's normalizer, and writes the canonical
// maps plus the transform/root report that `reports/vegetation-realism/texture-damage/` cites.
//
// Usage:
//   node tools/vegetation-leaf-source.mjs \
//     --baseColor assets/source/.../oak_leaves_basecolor_alpha.png \
//     --opacity  assets/source/.../oak_leaves_opacity.png \
//     --normal   assets/source/.../oak_leaves_normal_dx.png \
//     --cell 0,0 --columns 3 --rows 4 --border 32 \
//     --blade-length 0.17 --blade-width 0.17 \
//     --out reports/vegetation-realism/texture-damage/oak-leaf-canonical

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const option = (name, fallback) => {
  const index = args.indexOf(name);
  return index >= 0 && args[index + 1] !== undefined ? args[index + 1] : fallback;
};
const required = (name) => {
  const value = option(name, null);
  if (!value) throw new Error(`${name} is required`);
  return value;
};

// A crop with a transparent border. The border is what gives an atlas cell the safe margin the
// canonical slot requires. It never crops the art: the cell itself is copied whole.
function cropWithBorder(image, rect, border, kind) {
  const width = rect.width + border * 2;
  const height = rect.height + border * 2;
  const out = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const sx = rect.x + x - border, sy = rect.y + y - border;
      const offset = (y * width + x) * 4;
      // The border must be transparent, not a window onto the neighbouring atlas cell. Sampling the
      // source image outside the cell rect is how a crop acquires a phantom second leaf.
      if (sx < rect.x || sy < rect.y || sx >= rect.x + rect.width || sy >= rect.y + rect.height) continue;
      if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height) continue;
      const source = (sy * image.width + sx) * 4;
      if (kind === 'normal') {
        // Neutral tangent-space normal in an empty texel; alpha 0 keeps it out of coverage.
        out[offset] = 128; out[offset + 1] = 128; out[offset + 2] = 255; out[offset + 3] = 0;
        continue;
      }
      out[offset] = image.data[source];
      out[offset + 1] = image.data[source + 1];
      out[offset + 2] = image.data[source + 2];
      out[offset + 3] = image.data[source + 3];
    }
  }
  return { width, height, data: out };
}

async function main() {
  const baseFile = required('--baseColor');
  const opacityFile = option('--opacity', null);
  const normalFile = option('--normal', null);
  const outDir = path.resolve(root, option('--out', 'reports/vegetation-realism/texture-damage/canonical-leaf'));
  const border = Number(option('--border', '32'));
  const columns = Number(option('--columns', '1'));
  const rows = Number(option('--rows', '1'));
  const cell = option('--cell', null);

  const read = (file) => fs.readFile(path.resolve(root, file)).then((bytes) => decodePng(bytes, file));
  const base = await read(baseFile);
  const opacity = opacityFile ? resizeRgba(await read(opacityFile), base.width, base.height) : null;
  const normal = normalFile ? resizeRgba(await read(normalFile), base.width, base.height) : null;

  let rect = { x: 0, y: 0, width: base.width, height: base.height };
  if (cell) {
    const [column, row] = cell.split(',').map(Number);
    if (!Number.isInteger(column) || !Number.isInteger(row) || column < 0 || row < 0 || column >= columns || row >= rows) {
      throw new Error(`--cell ${cell} is outside the ${columns}x${rows} grid`);
    }
    const x0 = Math.round((base.width * column) / columns);
    const x1 = Math.round((base.width * (column + 1)) / columns);
    const y0 = Math.round((base.height * row) / rows);
    const y1 = Math.round((base.height * (row + 1)) / rows);
    rect = { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
  }
  const source = (image, kind) => cropWithBorder(image, rect, border, kind);
  const maps = {
    baseColor: source(base, 'color'),
    ...(opacity ? { opacity: source(opacity, 'color') } : {}),
    ...(normal ? { normal: source(normal, 'normal') } : {}),
  };

  const metadata = {
    schema: LEAF_SOURCE_SCHEMA,
    assetKind: 'leaf',
    leafProfile: 'bladeOnly',
    speciesId: option('--species', 'oak_reference_profile'),
    side: 'front',
    imageSize: [maps.baseColor.width, maps.baseColor.height],
    coordinates: { origin: 'bottomLeft', u: 'right', v: 'up' },
    rootPivotUV: [0.5, 0.0625],
    tipDirectionUV: [0, 1],
    bladeLengthMetres: Number(option('--blade-length', '0.17')),
    bladeWidthMetres: Number(option('--blade-width', '0.17')),
    safeMarginFraction: 0.0625,
    // Must equal the runtime material's cutoff. The oak art has a soft petiole whose alpha sits
    // between 0.35 and 0.5; thresholding at 0.5 splits it into "two subjects", which is a
    // normalization failure that has nothing to do with the leaf being wrong.
    alphaCutoff: Number(option('--alpha-cutoff', '0.35')),
    normalConvention: 'directX-',
    baseDamageState: 'healthy',
    maps: {
      baseColor: { file: 'canonical.basecolor.png', colorSpace: 'srgb' },
      ...(opacity ? { opacity: { file: 'canonical.opacity.png', channel: 'R', colorSpace: 'linear' } } : {}),
      ...(normal ? { normal: { file: 'canonical.normal.png', colorSpace: 'linear' } } : {}),
    },
    provenance: {
      provider: 'oak-imagegen-v1 (AI-inferred prototype, not a measured scan)',
      quality: 'inferred',
      source: { baseColor: baseFile, opacity: opacityFile, normal: normalFile },
      crop: { cell, grid: [columns, rows], borderPixels: border, rect },
    },
  };

  const result = normalizeLeafSource({
    maps: Object.fromEntries(Object.entries(maps).map(([key, image]) => [key, { pixels: image.data, width: image.width, height: image.height }])),
    metadata,
  });

  await fs.mkdir(outDir, { recursive: true });
  for (const [key, image] of Object.entries(result.maps)) {
    const name = `canonical.${key === 'baseColor' ? 'basecolor' : key}.png`;
    await fs.writeFile(path.join(outDir, name), encodePng({ width: image.width, height: image.height, data: Buffer.from(image.pixels) }));
  }
  const report = {
    schema: 'nativegpu.vegetation-leaf-normalization-report/1',
    generatedAt: new Date().toISOString(),
    inputs: metadata.provenance.source,
    crop: metadata.provenance.crop,
    output: path.relative(root, outDir).split(path.sep).join('/'),
    metadata: result.metadata,
  };
  await fs.writeFile(path.join(outDir, 'metadata.json'), `${JSON.stringify(result.metadata, null, 2)}\n`);
  await fs.writeFile(path.join(outDir, 'report.json'), `${JSON.stringify(report, null, 2)}\n`);
  console.log(JSON.stringify({
    out: path.relative(root, outDir).split(path.sep).join('/'),
    rootPivotUV: result.metadata.rootPivotUV,
    contentBoundsUV: result.metadata.contentBoundsUV,
    rootErrorPixels: result.metadata.validation.rootErrorPixels,
    inference: result.metadata.validation.inference,
    rootConfidence: result.metadata.transform.rootConfidence,
    pixelsPerMetre: result.metadata.pixelsPerMetre,
    maps: Object.keys(result.maps),
  }, null, 2));
}

await main();

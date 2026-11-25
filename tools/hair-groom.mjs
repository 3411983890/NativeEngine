import fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { compileGroom } from '../engine/groom/asset.mjs';
import { buildHairFixture } from './hair-style-fixture.mjs';
import { toCompatibleGroomSource, groomSourceFromHairstyle } from '../engine/hair/cook.mjs';

// Cooks the hair fixture into the groom package a native runtime consumes.
//
// Two authoring forms, one package schema (`nativegpu.groom-cook/1`):
//   default        `nativegpu.groom/2` — the hairstyle's own scalp mesh, guides, strands and
//                  guide weights, transcribed. 1806 guides on 2390 scalp triangles.
//   --compatible   `nativegpu.groom/1` — the procedural cap, with its three documented losses
//                  (65 guides, re-derived ids, averaged region identity).
//
// The source document is compiled in memory rather than written as JSON first: an explicit
// hairstyle source is ~12 MB of curve data, and `tools/groom.mjs` intentionally caps authored
// sources at 2 MiB. Serialising it would also invite someone to hand-edit 14k strands, which
// is not an authoring workflow this repo supports.

const ROOT = path.resolve(import.meta.dirname, '..');

async function writeObject(directory, hash, bytes) {
  const file = path.join(directory, `${hash}.nggroom`);
  try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
  catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = await fs.readFile(file);
    if (!existing.equals(Buffer.from(bytes))) throw new Error(`cooked object conflict: ${file}`);
  }
  return file;
}

async function main() {
  const args = process.argv.slice(2);
  const compatible = args.includes('--compatible');
  const outputDirectory = path.resolve(
    args.find((value) => !value.startsWith('--')) ?? path.join(ROOT, 'build/hair-groom'));
  const cookedDirectory = path.join(outputDirectory, compatible ? 'cooked-compatible' : 'cooked');
  await fs.mkdir(cookedDirectory, { recursive: true });

  const { spec, generated } = await buildHairFixture();
  const converted = compatible ? toCompatibleGroomSource(spec) : null;
  const source = compatible ? converted.source : groomSourceFromHairstyle(generated);
  const losses = compatible
    ? converted.losses
    : ['material is shared across regions until a hairstyle authors per-region materials'];
  const cooked = compileGroom(source);
  const file = await writeObject(cookedDirectory, cooked.hash, cooked.bytes);

  const entry = {
    id: cooked.id, hash: cooked.hash, file: path.basename(file),
    guides: cooked.guides, strands: cooked.strands, segments: cooked.segments,
    levels: cooked.levels, bytes: cooked.bytesTotal,
  };
  const manifest = { schema: 'nativegpu.groom-library/1', grooms: [entry] };
  const payloadJson = canonicalJson(manifest);
  const document = { ...manifest, payloadJson, packageHash: sha256Hex(payloadJson) };
  const temporary = path.join(cookedDirectory, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(document)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(cookedDirectory, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }

  console.log(JSON.stringify({
    schema: 'nativegpu.hair-groom-fixture/1',
    mode: compatible ? 'compatible-cap' : 'explicit-hairstyle',
    hairstyle: {
      id: spec.id, guides: generated.guides.length, strands: generated.levels[0].curves.length,
      scalpTriangles: generated.scalp.triangles,
    },
    groom: entry,
    losses,
    cookedDirectory,
    libraryFile: path.join(cookedDirectory, 'library.json'),
  }));
}

await main();

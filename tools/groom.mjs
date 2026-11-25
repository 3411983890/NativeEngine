import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { readPackage } from '../engine/assets/binary-package.mjs';
import { compileGroom, GROOM_COOK_SCHEMA, GroomError } from '../engine/groom/asset.mjs';

const inside = (parent, child) => {
  const relative = path.relative(parent, child);
  return relative !== '' && !path.isAbsolute(relative) && relative !== '..' && !relative.startsWith(`..${path.sep}`);
};
async function resolvedOutput(directory) {
  let ancestor = path.resolve(directory);
  const tail = [];
  for (;;) {
    try { return path.join(await fs.realpath(ancestor), ...tail.reverse()); }
    catch (error) {
      if (error.code !== 'ENOENT') throw error;
      tail.push(path.basename(ancestor));
      const parent = path.dirname(ancestor);
      if (parent === ancestor) throw error;
      ancestor = parent;
    }
  }
}
async function readJson(file, maximum = 2 * 1024 * 1024) {
  const stats = await fs.stat(file);
  if (!stats.isFile() || stats.size > maximum) throw new GroomError('NG-E-GROOM-FORMAT', `${file} must be a file at most ${maximum} bytes`);
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

async function writeObject(directory, hash, bytes, suffix) {
  const file = path.join(directory, `${hash}${suffix}`);
  try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
  catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = await fs.readFile(file);
    if (!existing.equals(Buffer.from(bytes))) throw new GroomError('NG-E-GROOM-FORMAT', `cooked object conflict: ${file}`);
  }
  return file;
}

export async function cookGroomFile({ input, output }) {
  const source = await readJson(input);
  const cooked = compileGroom(source);
  await fs.mkdir(output, { recursive: true });
  const file = await writeObject(output, cooked.hash, cooked.bytes, '.nggroom');
  return {
    id: cooked.id, hash: cooked.hash, file: path.basename(file),
    guides: cooked.guides, strands: cooked.strands, segments: cooked.segments,
    levels: cooked.levels, bytes: cooked.bytesTotal,
  };
}

export async function cookGroomLibrary({ sourceDirectory, outputDirectory }) {
  const input = await fs.realpath(sourceDirectory), output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output)) throw new GroomError('NG-E-GROOM-FORMAT', 'cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.groom.json')).sort();
  if (names.length === 0) throw new GroomError('NG-E-GROOM-FORMAT', 'no *.groom.json sources found');
  if (names.length > 256) throw new GroomError('NG-E-GROOM-CAPACITY', 'groom library exceeds 256 assets');
  const grooms = [];
  for (const name of names) grooms.push(await cookGroomFile({ input: path.join(input, name), output }));
  grooms.sort((a, b) => a.id.localeCompare(b.id));
  const manifest = { schema: 'nativegpu.groom-library/1', grooms };
  const payloadJson = canonicalJson(manifest);
  const document = { ...manifest, payloadJson, packageHash: sha256Hex(payloadJson) };
  const temporary = path.join(output, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(document)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(output, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }
  return { ...document, outputDirectory: output };
}

// Reports what a package actually contains. Used by the evidence scripts to prove a
// package was re-read from bytes rather than echoed from the cook.
export async function inspectGroom({ input }) {
  const bytes = await fs.readFile(input);
  const parsed = readPackage(bytes);
  if (parsed.schema !== GROOM_COOK_SCHEMA) throw new GroomError('NG-E-GROOM-FORMAT', `unexpected schema ${parsed.schema}`);
  return {
    id: parsed.meta.id, hash: sha256Hex(bytes), schema: parsed.schema,
    counts: parsed.meta.counts, lod: parsed.meta.lod,
    representations: parsed.meta.representations,
    sections: parsed.sections.map((section) => ({ kind: section.kind, index: section.index, bytes: section.bytes, count: section.count })),
  };
}

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!['--in', '--out'].includes(rest[i]) || rest[i + 1] === undefined) throw new Error('Usage: groom.mjs <cook|library|inspect> --in <path> --out <path>');
    if (Object.hasOwn(options, rest[i])) throw new Error(`duplicate option ${rest[i]}`);
    options[rest[i]] = rest[i + 1];
  }
  if (command === 'cook' && options['--in'] && options['--out']) {
    console.log(JSON.stringify(await cookGroomFile({ input: path.resolve(options['--in']), output: path.resolve(options['--out']) })));
  } else if (command === 'library' && options['--in'] && options['--out']) {
    const { payloadJson, ...metadata } = await cookGroomLibrary({ sourceDirectory: options['--in'], outputDirectory: options['--out'] });
    console.log(JSON.stringify(metadata));
  } else if (command === 'inspect' && options['--in']) {
    console.log(JSON.stringify(await inspectGroom({ input: path.resolve(options['--in']) })));
  } else throw new Error('Use cook --in file --out dir, library --in dir --out dir, or inspect --in file');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-GROOM'}: ${error.message}`); process.exitCode = 1; });
}

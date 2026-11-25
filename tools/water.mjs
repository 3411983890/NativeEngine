import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { COOKED_WATER_SCHEMA, WaterError, compileWater, compileWaterLibrary } from '../engine/water/asset.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
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
async function readAsset(file) {
  const stats = await fs.stat(file);
  if (!stats.isFile() || stats.size > 2 * 1024 * 1024) throw new WaterError('NG-E-WATER-SIZE', 'Water asset must be a file at most 2 MiB');
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

export async function cookWaterLibrary({ sourceDirectory, outputDirectory }) {
  const input = await fs.realpath(sourceDirectory), output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output))
    throw new WaterError('NG-E-WATER-PATH', 'Cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.water.json')).sort();
  if (names.length > 256) throw new WaterError('NG-E-WATER-SIZE', 'Water library exceeds 256 assets');
  const sources = [];
  for (const name of names) {
    const file = await fs.realpath(path.join(input, name));
    if (!inside(input, file)) throw new WaterError('NG-E-WATER-PATH', 'Water source resolves outside source directory');
    sources.push(await readAsset(file));
  }
  const library = compileWaterLibrary(sources);
  const receipt = {
    schema: 'nativegpu.water-library/1', libraryHash: library.hash,
    bodies: library.bodies.map((body) => ({ id: body.source.id, hash: body.hash, file: `${body.hash}.ngwater.json` })),
    runtimeIntegrated: false,
  };
  const packageHash = sha256Hex(canonicalJson(receipt));
  await fs.mkdir(output, { recursive: true });
  for (const body of library.bodies) {
    const payloadJson = canonicalJson(body.payload);
    const file = path.join(output, `${body.hash}.ngwater.json`);
    const bytes = `${canonicalJson({ schema: COOKED_WATER_SCHEMA, payloadJson, hash: body.hash })}\n`;
    try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (await fs.readFile(file, 'utf8') !== bytes) throw new WaterError('NG-E-WATER-HASH', `Cooked object conflict: ${file}`);
    }
  }
  const manifest = { ...receipt, payloadJson: canonicalJson(receipt), packageHash };
  const temporary = path.join(output, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(manifest)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(output, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }
  return {
    ...manifest, outputDirectory: output,
    cells: library.bodies.reduce((sum, body) => sum + body.cells, 0),
    poolBytes: library.bodies.reduce((sum, body) => sum + body.poolBytes, 0),
  };
}

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!['--in', '--out'].includes(rest[i]) || rest[i + 1] === undefined)
      throw new Error('Usage: water.mjs validate --in file | cook --in directory --out directory');
    if (Object.hasOwn(options, rest[i])) throw new Error(`Duplicate option ${rest[i]}`);
    options[rest[i]] = rest[i + 1];
  }
  if (command === 'validate' && options['--in']) {
    const compiled = compileWater(await readAsset(path.resolve(options['--in'])));
    console.log(JSON.stringify({
      id: compiled.source.id, hash: compiled.hash, backend: compiled.source.backend,
      columns: compiled.columns, rows: compiled.rows, cells: compiled.cells,
      poolBytes: compiled.poolBytes, fft: compiled.fft, runtimeIntegrated: false,
    }));
  } else if (command === 'cook' && options['--in'] && options['--out']) {
    const { payloadJson, ...metadata } = await cookWaterLibrary({ sourceDirectory: options['--in'], outputDirectory: options['--out'] });
    console.log(JSON.stringify(metadata));
  } else throw new Error('Use validate --in file or cook --in directory --out directory');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-WATER'}: ${error.message}`); process.exitCode = 1; });
}

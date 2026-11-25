import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { readPackage } from '../engine/assets/binary-package.mjs';
import { compileSpecies, SPECIES_COOK_SCHEMA, VegetationError } from '../engine/vegetation/species.mjs';
import { compileRegion, regionCapacity, REGION_SCHEMA } from '../engine/vegetation/region.mjs';
import { createSurface } from '../engine/vegetation/surface.mjs';

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
async function readJson(file, maximum = 4 * 1024 * 1024) {
  const stats = await fs.stat(file);
  if (!stats.isFile() || stats.size > maximum) throw new VegetationError('NG-E-VEG-FORMAT', `${file} must be a file at most ${maximum} bytes`);
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

// Writes an immutable content-addressed object. Re-cooking the same source must
// produce the same bytes; a different byte sequence under the same name is a
// hash collision or a nondeterministic cook, and both are fatal.
async function writeObject(directory, hash, bytes, suffix) {
  const file = path.join(directory, `${hash}${suffix}`);
  try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
  catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const existing = await fs.readFile(file);
    if (!existing.equals(Buffer.from(bytes))) throw new VegetationError('NG-E-VEG-HASH', `cooked object conflict: ${file}`);
  }
  return file;
}

async function writeManifest(directory, manifest) {
  const payloadJson = canonicalJson(manifest);
  const document = { ...manifest, payloadJson, packageHash: sha256Hex(payloadJson) };
  const temporary = path.join(directory, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(document)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(directory, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }
  return document;
}

export async function cookSpeciesFile({ input, output, textures }) {
  const source = await readJson(input);
  // The texture library is optional: a species without a `textures` block stays vertex
  // coloured, which keeps the earlier fixtures valid.
  const cooked = compileSpecies(source, { textures });
  await fs.mkdir(output, { recursive: true });
  const file = await writeObject(output, cooked.hash, cooked.bytes, '.ngveg');
  return {
    id: cooked.id, hash: cooked.hash, file: path.basename(file),
    vertices: cooked.vertices, triangles: cooked.triangles,
    lodTriangles: cooked.lodTriangles, lodErrorMetres: cooked.lodErrorMetres,
    bytes: cooked.bytesTotal,
  };
}

export async function cookSpeciesLibrary({ sourceDirectory, outputDirectory, textureDirectory }) {
  const input = await fs.realpath(sourceDirectory), output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output)) throw new VegetationError('NG-E-VEG-PATH', 'cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.species.json')).sort();
  if (names.length === 0) throw new VegetationError('NG-E-VEG-FORMAT', 'no *.species.json sources found');
  if (names.length > 256) throw new VegetationError('NG-E-VEG-CAPACITY', 'species library exceeds 256 assets');
  const textures = textureDirectory ? await readJson(path.join(textureDirectory, 'library.json')) : null;
  const species = [];
  for (const name of names) species.push(await cookSpeciesFile({ input: path.join(input, name), output, textures }));
  species.sort((a, b) => a.id.localeCompare(b.id));
  const manifest = { schema: 'nativegpu.vegetation-library/1', species, regions: [] };
  return { ...(await writeManifest(output, manifest)), outputDirectory: output };
}

// A region source either embeds a plane surface or points at an imported mesh. The
// mesh case needs a real imported asset, so the cook refuses to invent one.
function surfaceFor(region) {
  const description = region.surface;
  if (description.kind === 'plane') return createSurface(description);
  if (description.kind === 'mesh') {
    if (!description.positions || !description.indices) {
      throw new VegetationError('NG-E-VEG-SURFACE', 'mesh surface needs positions and indices from an imported asset');
    }
    return createSurface(description);
  }
  throw new VegetationError('NG-E-VEG-SURFACE', `unsupported surface kind ${description.kind}`);
}

export async function cookRegionFile({ input, output, speciesDirectory }) {
  const region = await readJson(input);
  if (region.schema !== REGION_SCHEMA) throw new VegetationError('NG-E-VEG-SCATTER', `region schema must be ${REGION_SCHEMA}`);
  const library = await readJson(path.join(speciesDirectory, 'library.json'));
  if (!Array.isArray(library.species) || library.species.length === 0) throw new VegetationError('NG-E-VEG-SPECIES', 'species library is empty');
  // The region references cooked species by id+digest. Re-reading and re-parsing the
  // package here keeps the region cook honest: it cannot reference a species whose
  // bytes changed, and it reads LOD/error data from the package itself instead of
  // trusting a manifest summary that could drift.
  const species = [];
  for (const entry of library.species) {
    const bytes = await fs.readFile(path.join(speciesDirectory, entry.file));
    const digest = sha256Hex(bytes);
    if (digest !== entry.hash) throw new VegetationError('NG-E-VEG-HASH', `species ${entry.id} bytes do not match its manifest digest`);
    const parsed = readPackage(bytes);
    if (parsed.schema !== SPECIES_COOK_SCHEMA) throw new VegetationError('NG-E-VEG-FORMAT', `species ${entry.id} has schema ${parsed.schema}`);
    if (parsed.meta.id !== entry.id) throw new VegetationError('NG-E-VEG-FORMAT', `species ${entry.id} package identifies itself as ${parsed.meta.id}`);
    species.push({ id: entry.id, hash: entry.hash, meta: parsed.meta });
  }
  const compiled = compileRegion({ region, species, surface: surfaceFor(region) });
  await fs.mkdir(output, { recursive: true });
  const file = await writeObject(output, compiled.hash, compiled.bytes, '.ngvrg');
  return {
    id: compiled.id, hash: compiled.hash, file: path.basename(file),
    instances: compiled.instances, pages: compiled.pages,
    instanceBytes: compiled.instanceBytes, totalBytes: compiled.bytes.byteLength,
    density: compiled.meta.density, counts: compiled.meta.counts,
    quantization: compiled.meta.quantization,
  };
}

export async function cookRegionLibrary({ sourceDirectory, outputDirectory, speciesDirectory }) {
  const input = await fs.realpath(sourceDirectory), output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output)) throw new VegetationError('NG-E-VEG-PATH', 'cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.region.json')).sort();
  if (names.length === 0) throw new VegetationError('NG-E-VEG-FORMAT', 'no *.region.json sources found');
  const regions = [];
  for (const name of names) regions.push(await cookRegionFile({ input: path.join(input, name), output, speciesDirectory }));
  regions.sort((a, b) => a.id.localeCompare(b.id));
  const library = await readJson(path.join(speciesDirectory, 'library.json'));
  const manifest = { schema: 'nativegpu.vegetation-library/1', species: library.species, regions };
  return { ...(await writeManifest(output, manifest)), outputDirectory: output };
}

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!['--in', '--out', '--species', '--instances', '--textures'].includes(rest[i]) || rest[i + 1] === undefined) {
      throw new Error('Usage: vegetation.mjs <species|library|region|regions|capacity> --in <path> --out <path> [--species <dir>] [--instances <n>]');
    }
    if (Object.hasOwn(options, rest[i])) throw new Error(`duplicate option ${rest[i]}`);
    options[rest[i]] = rest[i + 1];
  }
  if (command === 'species' && options['--in'] && options['--out']) {
    console.log(JSON.stringify(await cookSpeciesFile({ input: path.resolve(options['--in']), output: path.resolve(options['--out']) })));
  } else if (command === 'library' && options['--in'] && options['--out']) {
    const { payloadJson, ...metadata } = await cookSpeciesLibrary({
      sourceDirectory: options['--in'], outputDirectory: options['--out'],
      textureDirectory: options['--textures'] ? path.resolve(options['--textures']) : null,
    });
    console.log(JSON.stringify(metadata));
  } else if (command === 'region' && options['--in'] && options['--out'] && options['--species']) {
    console.log(JSON.stringify(await cookRegionFile({
      input: path.resolve(options['--in']), output: path.resolve(options['--out']), speciesDirectory: path.resolve(options['--species']),
    })));
  } else if (command === 'regions' && options['--in'] && options['--out'] && options['--species']) {
    const { payloadJson, ...metadata } = await cookRegionLibrary({
      sourceDirectory: options['--in'], outputDirectory: options['--out'], speciesDirectory: path.resolve(options['--species']),
    });
    console.log(JSON.stringify(metadata));
  } else if (command === 'capacity') {
    const instances = Number(options['--instances'] ?? 30_000_000);
    console.log(JSON.stringify({ schema: 'nativegpu.vegetation-capacity/1', ...regionCapacity({ worldInstances: instances }) }));
  } else throw new Error('Use species|library|region|regions|capacity with the documented options');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-VEG'}: ${error.message}`); process.exitCode = 1; });
}

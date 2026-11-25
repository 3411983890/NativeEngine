import { createHash, randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, compileEffect, compileEffectLibrary, EffectError } from '../engine/effects/asset.mjs';

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
  if (!stats.isFile() || stats.size > 2 * 1024 * 1024) throw new EffectError('NG-E-EFFECT-SIZE', 'Effect asset must be a file at most 2 MiB');
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

export async function cookEffectLibrary({ sourceDirectory, outputDirectory, assetRoot = path.join(root, 'assets') }) {
  const input = await fs.realpath(sourceDirectory), assets = await fs.realpath(assetRoot);
  const output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output))
    throw new EffectError('NG-E-EFFECT-PATH', 'Cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.effect.json')).sort();
  if (names.length > 4096) throw new EffectError('NG-E-EFFECT-SIZE', 'Effect library exceeds 4096 assets');
  const sourceAssets = [];
  for (const name of names) {
    const file = await fs.realpath(path.join(input, name));
    if (!inside(input, file)) throw new EffectError('NG-E-EFFECT-PATH', 'Effect source resolves outside source directory');
    sourceAssets.push(await readAsset(file));
  }
  const library = compileEffectLibrary(sourceAssets);
  // The receipt locks actual external bytes. Identical references with changed
  // textures/meshes must produce a new package identity.
  const dependencies = {};
  for (const name of [...new Set(library.effects.flatMap((effect) => effect.dependencies))].sort()) {
    const file = await fs.realpath(path.resolve(assets, name));
    if (!inside(assets, file)) throw new EffectError('NG-E-EFFECT-PATH', `Dependency escapes asset root: ${name}`);
    if (!(await fs.stat(file)).isFile()) throw new EffectError('NG-E-EFFECT-PATH', `Dependency is not a file: ${name}`);
    dependencies[name] = createHash('sha256').update(await fs.readFile(file)).digest('hex');
  }
  const receipt = { schema: 'nativegpu.effect-library/2', libraryHash: library.hash, dependencies,
    effects: library.effects.map((effect) => ({ id: effect.source.id, hash: effect.hash, file: `${effect.hash}.ngeffect.json` })),
    runtimeIntegrated: false };
  const packageHash = createHash('sha256').update(canonicalJson(receipt)).digest('hex');
  const out = output;
  await fs.mkdir(out, { recursive: true });
  for (const effect of library.effects) {
    const file = path.join(out, `${effect.hash}.ngeffect.json`), bytes = `${canonicalJson(effect)}\n`;
    try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (await fs.readFile(file, 'utf8') !== bytes) throw new EffectError('NG-E-EFFECT-HASH', `Cooked object conflict: ${file}`);
    }
  }
  const manifest = { ...receipt, payloadJson: canonicalJson(receipt), packageHash }, temporary = path.join(out, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(manifest)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(out, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }
  return { ...manifest, outputDirectory: out, poolBytes: library.effects.reduce((n, effect) => n + effect.poolBytes, 0) };
}

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!['--in', '--out', '--asset-root'].includes(rest[i]) || rest[i+1] === undefined)
      throw new Error('Usage: effects.mjs validate --in file | cook --in directory --out directory [--asset-root assets]');
    if (Object.hasOwn(options, rest[i])) throw new Error(`Duplicate option ${rest[i]}`);
    options[rest[i]] = rest[i+1];
  }
  if (command === 'validate' && options['--in']) {
    const compiled = compileEffect(await readAsset(path.resolve(options['--in'])));
    console.log(JSON.stringify({ id: compiled.source.id, hash: compiled.hash, poolBytes: compiled.poolBytes, runtimeIntegrated: false }));
  } else if (command === 'cook' && options['--in'] && options['--out']) {
    const { payloadJson, ...metadata } = await cookEffectLibrary({ sourceDirectory: options['--in'], outputDirectory: options['--out'],
      ...(options['--asset-root'] ? { assetRoot: options['--asset-root'] } : {}) });
    console.log(JSON.stringify(metadata));
  } else throw new Error('Use validate --in file or cook --in directory --out directory');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-EFFECT'}: ${error.message}`); process.exitCode = 1; });
}

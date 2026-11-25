import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { ClothError, compileCloth, compileClothLibrary, migrateCloth } from '../engine/cloth/asset.mjs';

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
  if (!stats.isFile() || stats.size > 2 * 1024 * 1024) throw new ClothError('NG-E-CLOTH-SIZE', 'Cloth asset must be a file at most 2 MiB');
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

export async function cookClothLibrary({ sourceDirectory, outputDirectory }) {
  const input = await fs.realpath(sourceDirectory), output = await resolvedOutput(outputDirectory);
  if (output === input || inside(input, output))
    throw new ClothError('NG-E-CLOTH-PATH', 'Cook output must be separate from source assets');
  const names = (await fs.readdir(input)).filter((name) => name.endsWith('.cloth.json')).sort();
  if (names.length > 256) throw new ClothError('NG-E-CLOTH-SIZE', 'Cloth library exceeds 256 assets');
  const sources = [];
  for (const name of names) {
    const file = await fs.realpath(path.join(input, name));
    if (!inside(input, file)) throw new ClothError('NG-E-CLOTH-PATH', 'Cloth source resolves outside source directory');
    sources.push(await readAsset(file));
  }
  const library = compileClothLibrary(sources);
  const receipt = {
    schema: 'nativegpu.cloth-library/1', libraryHash: library.hash,
    cloths: library.cloths.map((cloth) => ({ id: cloth.source.id, hash: cloth.hash, file: `${cloth.hash}.ngcloth.json` })),
    runtimeIntegrated: false,
  };
  const packageHash = sha256Hex(canonicalJson(receipt));
  await fs.mkdir(output, { recursive: true });
  for (const cloth of library.cloths) {
    // The container authenticates the exact payload bytes; the payload itself is what
    // the native loader parses, so a whitespace edit invalidates the object.
    const payloadJson = canonicalJson(cloth.payload);
    const file = path.join(output, `${cloth.hash}.ngcloth.json`);
    const bytes = `${canonicalJson({ schema: cloth.payload.schema, payloadJson, hash: cloth.hash })}\n`;
    try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (await fs.readFile(file, 'utf8') !== bytes) throw new ClothError('NG-E-CLOTH-HASH', `Cooked object conflict: ${file}`);
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
    vertices: library.cloths.reduce((sum, cloth) => sum + cloth.vertexCount, 0),
    constraints: library.cloths.reduce((sum, cloth) => sum + cloth.constraintCount, 0),
    poolBytes: library.cloths.reduce((sum, cloth) => sum + cloth.poolBytes, 0),
  };
}

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!['--in', '--out','--bend-angle-compliance'].includes(rest[i]) || rest[i + 1] === undefined)
      throw new Error('Usage: cloth.mjs validate --in file | cook --in directory --out directory');
    if (Object.hasOwn(options, rest[i])) throw new Error(`Duplicate option ${rest[i]}`);
    options[rest[i]] = rest[i + 1];
  }
  if(command==='migrate'&&options['--in']&&options['--out']&&options['--bend-angle-compliance']!==undefined){
    const migrated=migrateCloth(await readAsset(path.resolve(options['--in'])),Number(options['--bend-angle-compliance']));
    const compiled=compileCloth(migrated);await fs.writeFile(path.resolve(options['--out']),`${JSON.stringify(migrated,null,2)}\n`,{flag:'wx'});
    console.log(JSON.stringify({schema:migrated.schema,id:migrated.id,hash:compiled.hash,bendAngleCompliance:migrated.material.bendAngleCompliance,unit:'rad/(N*m)'}));return;
  }
  if(options['--bend-angle-compliance']!==undefined)throw new Error('Angular compliance is only accepted by migrate --in old --out new --bend-angle-compliance value');
  if (command === 'validate' && options['--in']) {
    const compiled = compileCloth(await readAsset(path.resolve(options['--in'])));
    console.log(JSON.stringify({
      id: compiled.source.id, hash: compiled.hash, vertices: compiled.vertexCount,
      triangles: compiled.triangleCount, constraints: compiled.constraintCount,
      pins: compiled.pinCount, poolBytes: compiled.poolBytes, runtimeIntegrated: false,
    }));
  } else if (command === 'cook' && options['--in'] && options['--out']) {
    const { payloadJson, ...metadata } = await cookClothLibrary({ sourceDirectory: options['--in'], outputDirectory: options['--out'] });
    console.log(JSON.stringify(metadata));
  } else throw new Error('Use validate --in file or cook --in directory --out directory');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-CLOTH'}: ${error.message}`); process.exitCode = 1; });
}

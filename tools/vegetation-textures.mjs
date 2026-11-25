import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { cookTextureLibrary, readTexturePackage } from '../engine/vegetation/texture.mjs';

// Cooks the authored PNG set into raw NGT3 packages. Usage:
//   node tools/vegetation-textures.mjs cook --in assets/vegetation/textures/source \
//        --out build/vegetation/textures --roles assets/vegetation/textures/roles.json
//   node tools/vegetation-textures.mjs inspect --in build/vegetation/textures

async function main(args) {
  const [command, ...rest] = args;
  const options = {};
  for (let index = 0; index < rest.length; index += 2) {
    if (!['--in', '--out', '--roles'].includes(rest[index]) || rest[index + 1] === undefined) {
      throw new Error('Usage: vegetation-textures.mjs <cook|inspect> --in <dir> [--out <dir>] [--roles <file>]');
    }
    if (Object.hasOwn(options, rest[index])) throw new Error(`duplicate option ${rest[index]}`);
    options[rest[index]] = rest[index + 1];
  }
  if (command === 'cook' && options['--in'] && options['--out'] && options['--roles']) {
    const result = await cookTextureLibrary({
      sourceDirectory: path.resolve(options['--in']),
      outputDirectory: path.resolve(options['--out']),
      rolesFile: path.resolve(options['--roles']),
    });
    const { payloadJson, ...metadata } = result;
    console.log(JSON.stringify(metadata));
    return;
  }
  // Coverage per mip level, measured the way the shader measures it (red channel against
  // the cutoff it discards at). This is the check that the mask chain does not erode: a
  // plain box filter drops the coverage level by level until the card is a hole.
  if (command === 'mips' && options['--in']) {
    const directory = path.resolve(options['--in']);
    const library = JSON.parse(await fs.readFile(path.join(directory, 'library.json'), 'utf8'));
    const report = [];
    for (const entry of library.roles) {
      if (entry.filter !== 'coverage') continue;
      const texture = readTexturePackage(await fs.readFile(path.join(directory, entry.file)));
      const cutoff = Math.round(entry.coverageCutoff * 255);
      const levels = texture.levels.map((level) => {
        let covered = 0;
        for (let index = 0; index < level.pixels.length; index += 4) if (level.pixels[index] >= cutoff) ++covered;
        return { width: level.width, coverage: Number((covered / (level.pixels.length / 4)).toFixed(4)) };
      });
      const nominal = levels[0].coverage;
      const ratio = (level) => (nominal > 0 ? level.coverage / nominal : 1);
      // Both bounds are reported. Under-coverage is the hole this whole chain exists to
      // prevent; over-coverage is the price paid at the two coarsest levels, where a 3x4
      // atlas of leaves no longer resolves and the solve deliberately rounds up so a
      // sub-pixel leaf stays in the silhouette instead of disappearing from it.
      const worst = Math.min(...levels.map(ratio));
      const peak = Math.max(...levels.map(ratio));
      report.push({
        role: entry.role, cutoff: entry.coverageCutoff, mipCount: texture.mipCount, levels,
        worstRatio: Number(worst.toFixed(4)), peakRatio: Number(peak.toFixed(4)),
      });
    }
    console.log(JSON.stringify({ schema: 'nativegpu.vegetation-texture-mips/1', roles: report }));
    return;
  }
  if (command === 'inspect' && options['--in']) {    const directory = path.resolve(options['--in']);
    const library = JSON.parse(await fs.readFile(path.join(directory, 'library.json'), 'utf8'));
    const roles = [];
    for (const entry of library.roles) {
      const bytes = await fs.readFile(path.join(directory, entry.file));
      const texture = readTexturePackage(bytes);
      roles.push({
        role: entry.role, digest: entry.digest, width: texture.width, height: texture.height,
        srgb: texture.srgb, mipCount: texture.mipCount, filter: entry.filter,
        coverageCutoff: entry.coverageCutoff, atlas: entry.atlas, bytes: bytes.byteLength,
      });
    }
    console.log(JSON.stringify({ schema: library.schema, packageHash: library.packageHash, roles }));
    return;
  }
  throw new Error('Use cook --in --out --roles, or inspect --in');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(`${error.code ?? 'NG-E-VEG'}: ${error.message}`); process.exitCode = 1; });
}

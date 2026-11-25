import * as fs from 'node:fs/promises';
import path from 'node:path';
import { cookEffectLibrary } from './effects.mjs';

// Disposable renderer contract inputs, deliberately separate from the authored
// effect library. They exercise supported blends rather than claiming new effects.
const root = process.cwd();
const directory = path.join(root, 'build', 'effects-validation');
const sourceDirectory = path.join(directory, 'source');
await fs.mkdir(sourceDirectory, { recursive: true });
const template = JSON.parse(await fs.readFile(path.join(root, 'assets/effects/sprite_demo.effect.json'), 'utf8'));
for (const blend of ['additive', 'premultiplied', 'cutout']) {
  const asset = structuredClone(template);
  asset.id = `validation_${blend}`;
  asset.budget.maxSpawnPerTick = 64;
  asset.emitters[0].renderers[0].blend = blend;
  if (blend !== 'additive') {
    asset.emitters[0].modules[0].params.size = [0.28, 0.28];
    asset.emitters[0].modules[0].params.color = [0.1, 0.8, 2, 1];
    asset.emitters[0].modules[2].params.keys[0].value = [0.1, 0.8, 2, 1];
  }
  await fs.writeFile(path.join(sourceDirectory, `${asset.id}.effect.json`), `${JSON.stringify(asset, null, 2)}\n`);
}
const result = await cookEffectLibrary({ sourceDirectory, outputDirectory: path.join(directory, 'cooked') });
console.log(JSON.stringify({ manifest: path.join(result.outputDirectory, 'library.json'), packageHash: result.packageHash }));

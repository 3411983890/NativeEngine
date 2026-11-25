import * as fs from 'node:fs/promises';
import path from 'node:path';
import { cookEffectLibrary } from './effects.mjs';
const directory = path.resolve('build/effects-fields/source');
await fs.mkdir(directory, {recursive:true});
await fs.copyFile('assets/effects/wind_drift.effect.json', path.join(directory, 'wind_drift.effect.json'));
const result = await cookEffectLibrary({sourceDirectory:directory, outputDirectory:path.resolve('build/effects-fields/cooked')});
console.log(JSON.stringify({library:result.outputDirectory, hash:result.packageHash}));

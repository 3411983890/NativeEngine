import fs from 'node:fs';
import { compileEffect } from '../engine/effects/asset.mjs';
const directory = 'build/smoke-sharing'; fs.mkdirSync(directory, { recursive: true });
const a = JSON.parse(fs.readFileSync('assets/effects/smoke_solver_reference.effect.json', 'utf8'));
a.id = 'smoke_fixture'; a.maxAlive = 4; a.loop = true; a.durationTicks = 120;
a.emitters[0].position = [.25, .25, .25]; a.emitters[0].localSpace = true;
a.emitters[0].ratePerSecond = 0; a.emitters[0].bursts = [{ tick: 0, count: 2 }];
a.emitters[0].lifetimeTicks = [1000, 1000]; a.emitters[0].modules[0].params.velocity = [0, 0, 0];
a.smoke = { dimensions: [4, 4, 4], cellMetres: .5, originOffset: [0, 0, 0],
  channels: ['density', 'temperatureKelvin', 'fuel'], tailTicks: 3, unitsPerSource: [1024, 1024, 1024],
  sources: [{ emitter: 'source', densityMassRate: .125, heatVolumeRate: .25, fuelMassRate: .0625 }],
  parameters: { maxSpeed: 1, densityDecay: 0, cooling: 0, fuelDecay: 0, burnRate: 0, buoyancy: 0, smokeWeight: 0,
    vorticity: 0, pressureIterations: 4, macCormack: false } };
const compiled = compileEffect(a);
fs.writeFileSync(directory + '/fixture.effect.json', JSON.stringify(a, null, 2) + '\n');
fs.writeFileSync(directory + '/fixture.cooked.json', JSON.stringify(compiled) + '\n');
console.log(JSON.stringify({ hash: compiled.hash, smoke: compiled.smoke }));

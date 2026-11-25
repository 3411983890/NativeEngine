import fs from 'node:fs/promises';
import path from 'node:path';
import { cookEffectLibrary } from './effects.mjs';
import { validateScene } from '../engine/scene/index.mjs';
const directory = 'build/smoke-host/fixtures'; await fs.mkdir(directory, { recursive: true });
const town = JSON.parse(await fs.readFile('.screen/town.scene.json', 'utf8'));
let serial = 1;
const entity = (name, kind, position, scale, color = [1, 1, 1]) => ({
  id: `ca700000-0000-4000-8000-${String(serial++).padStart(12, '0')}`,
  name, kind, position, scale, color, collider: true, material: 'default',
});
for (const quality of [0, 1]) for (const mode of ['plain', 'blocked', 'emission', 'scattering', 'fog']) {
  serial = 1;
  const scene = {
    schemaVersion: 2, projectId: 'ca700000-0000-4000-8000-000000000001', name: `Smoke optical ${quality} ${mode}`, revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    materials: { default: { ...town.materials.default, unlit: true } },
    lighting: { quality, ambient: [1, 1, 1], keyDirection: [.3, .8, -.5], keyColor: [1, 1, 1], keyIntensity: 0,
      skyColor: [0, 0, 0], fogColor: [0, 0, 0], fogDensity: 0, fogHeightFalloff: 0, fogBaseHeight: 0,
      cloudCoverage: 0, cloudHeight: 40, cloudThickness: 20, cloudSpeed: 0,
      toneMap: false, exposure: 1, localShadows: false, ibl: false, ddgi: false, cloudShadows: false, temporal: true },
    entities: [entity('Ground', 'ground', [0, -1, 0], [100, 1, 100]),
      entity('Player', 'player', [30, 1, 0], [.8, 1.8, .8]),
      entity('Receiver', 'wall', [0, 1, 4], [6, 4, .2], ['emission', 'scattering'].includes(mode) ? [0, 0, 0] : [1, 1, 1])],
  };
  if (mode === 'scattering') scene.lighting.keyIntensity = 1;
  if (mode === 'fog') scene.lighting.fogDensity = .01;
  if (mode === 'blocked') scene.entities.push(entity('Foreground blocker', 'wall', [0, 1, -2.5], [2.5, 3, .2]));
  const result = validateScene(scene); if (!result.valid) throw Error(result.errors.join('\n'));
  await fs.writeFile(`${directory}/${mode}-q${quality}.scene.json`, JSON.stringify(scene, null, 2) + '\n');
}
const template = JSON.parse(await fs.readFile('assets/effects/smoke_solver_reference.effect.json', 'utf8'));
const uniform = structuredClone(template); uniform.id = 'medium_absorb'; uniform.loop = true; uniform.durationTicks = 12000; uniform.maxAlive = 8;
uniform.bounds = { min: [-1, 0, -1], max: [1, 2, 1] }; uniform.budget.maxSpawnPerTick = 8;
uniform.emitters = Array.from({ length: 8 }, (_, i) => ({ ...structuredClone(template.emitters[0]), id: `source_${i}`,
  position: [(i & 1) ? .5 : -.5, (i & 2) ? 1.5 : .5, (i & 4) ? .5 : -.5], localSpace: false,
  ratePerSecond: 0, bursts: [{ tick: 0, count: 1 }], lifetimeTicks: [12000, 12000],
  modules: [{ id: 'initial', type: 'initialize', params: { velocity: [0, 0, 0], color: [0, 0, 0, 0], size: [0, 0] } }],
}));
uniform.smoke = { dimensions: [2, 2, 2], cellMetres: 1, originOffset: [-1, 0, -1], channels: ['density'], tailTicks: 3,
  sources: uniform.emitters.map(e => ({ emitter: e.id, densityMassRate: .125, heatVolumeRate: 0, fuelMassRate: 0 })),
  parameters: { maxSpeed: 1, pressureIterations: 4, macCormack: false, densityDecay: 0, cooling: 0, fuelDecay: 0, burnRate: 0, buoyancy: 0, smokeWeight: 0, vorticity: 0 },
  optics: { extinctionPerDensity: 1, scatteringAlbedo: [0, 0, 0], emissionPerDensity: 0, emissionStartKelvin: 700 } };
const clear = structuredClone(uniform); clear.id = 'medium_clear'; delete clear.smoke;
const hot = structuredClone(uniform); hot.id = 'medium_hot'; hot.smoke.channels.push('temperatureKelvin');
const scatter = structuredClone(uniform); scatter.id = 'medium_scatter'; scatter.smoke.optics.scatteringAlbedo = [1, .5, .25];
for (const source of hot.smoke.sources) source.heatVolumeRate = 250;
hot.smoke.optics.extinctionPerDensity = 0; hot.smoke.optics.emissionPerDensity = 1;
// Two z layers of different density; their column integral equals the coarse
// cell mean. This exercises real 8-to-1 projection rather than a format swap.
const coarse = structuredClone(uniform); coarse.id = 'medium_coarse';
coarse.bounds = { min: [0, 0, 0], max: [1, 1, 1] };
coarse.smoke.cellMetres = .5; coarse.smoke.originOffset = [0, 0, 0];
coarse.smoke.unitsPerSource = [1048576, 1024, 1048576];
coarse.emitters.forEach((e, i) => { e.position = [(i & 1) ? .75 : .25, (i & 2) ? .75 : .25, (i & 4) ? .75 : .25]; });
coarse.smoke.sources.forEach((s, i) => { s.densityMassRate = (i & 4) ? .0234375 : .0078125; });
const source = `${directory}/effects-source`; await fs.mkdir(source, { recursive: true });
for (const asset of [uniform, clear, hot, scatter, coarse]) await fs.writeFile(path.join(source, asset.id + '.effect.json'), JSON.stringify(asset, null, 2) + '\n');
await cookEffectLibrary({ sourceDirectory: source, outputDirectory: `${directory}/effects-cooked` });
console.log(JSON.stringify({ directory, effects: ['medium_clear', 'medium_absorb', 'medium_hot'] }));

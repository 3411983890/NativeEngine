import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { compileEffect, previewEffectPatch } from '../engine/effects/asset.mjs';
const fixture = () => JSON.parse(fs.readFileSync(new URL('../assets/effects/smoke_solver_reference.effect.json', import.meta.url), 'utf8'));
test('smoke cook preserves source units, hashes and float solver defaults', () => {
  const asset = fixture(), before = structuredClone(asset), a = compileEffect(asset);
  assert.deepEqual(asset, before); assert.equal(a.hash, compileEffect(asset).hash);
  assert.deepEqual(a.smoke.sources, [{ emitter: 0, quantum: [5000, 1000, 2000] }]);
  assert.deepEqual(a.smoke.unitsPerSource, [1000000, 1000, 1000000]);
  assert.equal(a.smoke.parameters.ambientKelvin, Math.fround(293.15));
  assert.equal(a.smoke.channels, 7); assert.deepEqual(JSON.parse(a.payloadJson).smoke, a.smoke);
  const noSmoke = fixture(); delete noSmoke.smoke; assert.equal(Object.hasOwn(compileEffect(noSmoke), 'smoke'), false);
});
test('smoke authoring rejects incompatible channels, source references and quantization', () => {
  for (const mutate of [
    a => { a.smoke.sources[0].emitter = 'missing'; },
    a => { a.smoke.sources.push(structuredClone(a.smoke.sources[0])); },
    a => { a.smoke.channels = ['density']; },
    a => { a.smoke.sources[0].densityMassRate = -1; },
    a => { a.smoke.sources[0].densityMassRate = 1e-20; },
    a => { a.smoke.sources[0].densityMassRate = 1000000; },
    a => { a.smoke.unitsPerSource = [0, 1000, 1000000]; },
    a => { a.smoke.unitsPerSource = null; },
    a => { a.smoke.unitsPerSource = [1, 1, 1000000001]; },
    a => { for (const k of ['densityMassRate', 'heatVolumeRate', 'fuelMassRate']) a.smoke.sources[0][k] = 0; },
    a => { a.smoke.channels = ['density', 'density']; },
    a => { a.smoke.sources[0].heat = 1; },
    a => { a.smoke.unitsPerSource = [1, 1, 1]; a.smoke.sources[0].densityMassRate = 10000; a.smoke.cellMetres = .01; },
  ]) { const a = fixture(); mutate(a); assert.throws(() => compileEffect(a)); }
});
test('smoke solver contract rejects invalid schedule, float underflow and temperature bounds', () => {
  for (const mutate of [
    a => { a.smoke.dimensions = [1, 4, 4]; },
    a => { a.smoke.cellMetres = NaN; },
    a => { a.smoke.parameters.courant = 0; },
    a => { a.smoke.parameters.maxSpeed = 1e-100; },
    a => { a.smoke.parameters.pressureRelaxation = 2; },
    a => { a.smoke.parameters.ambientKelvin = 800; },
    a => { a.smoke.parameters.maxTemperature = 290; },
    a => { a.smoke.parameters.maxSpeed = 100; a.smoke.parameters.maxSubsteps = 1; },
    a => { a.smoke.parameters.macCormack = 1; },
    a => { a.smoke.parameters.pressureIterations = 257; },
    a => { a.smoke.parameters.unrecognized = 2; },
    a => { a.smoke.parameters = null; },
    a => { a.smoke.tailTicks = 432001; },
  ]) { const a = fixture(); mutate(a); assert.throws(() => compileEffect(a)); }
});
test('smoke patches require reset when solver inputs or sources change', () => {
  const a = fixture();
  assert.equal(previewEffectPatch(a, 0, { '/smoke/parameters/pressureIterations': 16 }).migration, 'reset-required');
  assert.equal(previewEffectPatch(a, 0, { '/smoke/sources/0/densityMassRate': .01 }).migration, 'reset-required');
  assert.equal(previewEffectPatch(a, 0, { '/seed': 99 }).migration, 'preserve-layout');
});
test('smoke optics have explicit bounded coefficients and participate in reset identity', () => {
  const a = fixture(); a.smoke.optics = { extinctionPerDensity: 2, scatteringAlbedo: [.1, .2, .3], emissionPerDensity: 0, emissionStartKelvin: 900 };
  const c = compileEffect(a); assert.equal(c.smoke.optics.extinctionPerDensity, 2);
  assert.deepEqual(c.smoke.optics.scatteringAlbedo, [.1, .2, .3].map(Math.fround));
  assert.equal(previewEffectPatch(a, 0, { '/smoke/optics/extinctionPerDensity': 1 }).migration, 'reset-required');
  for (const optics of [{ extinctionPerDensity: -1 }, { emissionPerDensity: Infinity }, { scatteringAlbedo: [0, 1, 2] },
    { emissionStartKelvin: 0 }, { unsupported: 1 }, null]) {
    const b = fixture(); b.smoke.optics = optics; assert.throws(() => compileEffect(b));
  }
});

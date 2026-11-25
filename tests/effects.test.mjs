import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { compileEffect, compileEffectLibrary, previewEffectPatch, sampleCurve } from '../engine/effects/asset.mjs';
import { cookEffectLibrary } from '../tools/effects.mjs';

const root = path.resolve(import.meta.dirname, '..');
const fixture = async () => JSON.parse(await fs.readFile(path.join(root, 'assets/effects/spark_reference.effect.json'), 'utf8'));
test('effect compiler emits aligned SoA layouts and stable source identity', async () => {
  const asset = await fixture(), before = structuredClone(asset);
  const compiled = compileEffect(asset);
  assert.deepEqual(asset, before);
  assert.equal(compiled.hash, compileEffect(JSON.parse(JSON.stringify(asset))).hash);
  assert.ok(compiled.emitters[0].layout.some((a) => a.name === 'color' && a.type === 'float4'));
  for (const a of compiled.emitters[0].layout) { assert.equal(a.offset % 16, 0); assert.equal(a.bytes % 16, 0); }
  const reordered = Object.fromEntries(Object.entries(asset).reverse());
  assert.equal(compileEffect(reordered).hash, compiled.hash);
  asset.emitters[0].modules[0].params.velocity[1] = 99;
  assert.equal(compiled.emitters[0].modules[0].params.velocity[1], 4, 'source mutation changed a compiled snapshot');
});
test('effect asset rejects unknown keys, invalid numbers, excessive rates and escaping dependencies', async () => {
  const invalid = [
    (a) => { a.unknown = true; }, (a) => { a.seed = NaN; }, (a) => { a.emitters[0].ratePerSecond = 0.0001; },
    (a) => { a.emitters[0].ratePerSecond = 0.0000000001; },
    (a) => { a.bounds.max[0] = a.bounds.min[0]; }, (a) => { a.emitters[0].renderers[0].texture = '../outside.png'; },
    (a) => { a.emitters[0].renderers[0].texture = 'C:/outside.png'; },
    (a) => { a.emitters[0].modules[0].params.color[3] = 2; },
    (a) => { a.emitters[0].lifetimeTicks = [100, 10]; },
  ];
  for (const mutate of invalid) { const asset = await fixture(); mutate(asset); assert.throws(() => compileEffect(asset)); }
});
test('module dependencies are ordered and reject cycles or missing ids', async () => {
  const asset = await fixture();
  asset.emitters[0].modules.reverse();
  const result = compileEffect(asset);
  assert.equal(result.emitters[0].modules[0].type, 'initialize');
  asset.emitters[0].modules.at(-1).after = ['fade'];
  assert.throws(() => compileEffect(asset), /cycle/);
  const missing = await fixture(); missing.emitters[0].modules[1].after = ['missing'];
  assert.throws(() => compileEffect(missing), /unknown module/);
});
test('lifetime curves interpolate and reject incomplete, repeated or reversed keys', async () => {
  const asset = await fixture(), curve = asset.emitters[0].modules[2].params.keys;
  const middle = sampleCurve(curve,0.5);
  assert.equal(middle[0], 2.25); assert.equal(middle[3],0.5);
  assert.deepEqual(sampleCurve(curve,0),curve[0].value);
  assert.deepEqual(sampleCurve(curve,1),curve[1].value);
  assert.throws(() => sampleCurve(curve,NaN));
  for (const time of [0,0.5,-1]) {
    const changed = await fixture(); changed.emitters[0].modules[2].params.keys[1].t = time;
    assert.throws(() => compileEffect(changed));
  }
});
test('field modules cannot access undeclared channels', async () => {
  const source = JSON.parse(await fs.readFile(path.join(root,'assets/effects/smoke_reference.effect.json'),'utf8'));
  assert.ok(compileEffect(source).hash);
  source.field.channels = ['density'];
  assert.throws(() => compileEffect(source), /unleased windVelocity/);
  delete source.field;
  assert.throws(() => compileEffect(source), /requires a field/);
});
test('read-only velocity field declares its velocity access, response and leased channel', async () => {
  const asset = await fixture();
  asset.field = {level:'worldVisible',channels:['windVelocity'],dimensions:[16,16,16],voxelMetres:1};
  asset.emitters[0].modules.push({id:'wind',type:'velocityField',after:['initial'],params:{channel:'windVelocity',response:3}});
  // Reference fixtures name their initializer differently; bind by actual id.
  asset.emitters[0].modules.at(-1).after = [asset.emitters[0].modules[0].id];
  const compiled = compileEffect(asset), module = compiled.emitters[0].modules.find((m) => m.type === 'velocityField');
  assert.deepEqual(module.access.reads,['position','velocity']);
  assert.deepEqual(module.access.writes,['velocity']);
  const without = structuredClone(asset); without.emitters[0].modules.pop();
  assert.equal(compiled.layoutHash,compileEffect(without).layoutHash,'read-only field added an undeclared particle attribute');
  for (const change of [
    (a) => { delete a.field; },
    (a) => { a.emitters[0].modules.at(-1).params.channel = 'waterVelocity'; },
    (a) => { a.emitters[0].modules.at(-1).params.channel = 'density'; },
    (a) => { a.emitters[0].modules.at(-1).params.response = -1; },
    (a) => { a.emitters[0].modules.at(-1).params.response = NaN; },
  ]) { const invalid = structuredClone(asset); change(invalid); assert.throws(() => compileEffect(invalid)); }
});
test('effect event graph rejects missing references and recursive storms', async () => {
  const a = await fixture(), b = await fixture(); b.id = 'child';
  a.budget.maxDepth = 2; a.budget.maxEventsPerTick = 32;
  a.events = [{ on: 'death', spawn: 'child', limit: 16, delayTicks: 0 }];
  assert.equal(compileEffectLibrary([a,b]).effects.length,2);
  assert.throws(() => compileEffectLibrary([a]), /missing subeffect/);
  b.budget = structuredClone(a.budget); b.events = [{ on: 'spawn', spawn: a.id, limit: 1, delayTicks: 0 }];
  assert.throws(() => compileEffectLibrary([a,b]), /cycle/);
  a.events[0].spawn = a.id;
  assert.throws(() => compileEffect(a), /self-spawning/);
});
test('revision-aware patches are pure and flag layout reset without claiming runtime migration', async () => {
  const asset = await fixture();
  const preview = previewEffectPatch(asset,0,{'/emitters/0/modules/0/params/velocity/1':8});
  assert.equal(preview.migration,'preserve-layout'); assert.equal(preview.runtimeApplied,false);
  assert.equal(asset.emitters[0].modules[0].params.velocity[1],4);
  assert.equal(preview.asset.revision,1);
  assert.throws(() => previewEffectPatch(asset,1,{'/seed':3}),/revision conflict/);
  assert.throws(() => previewEffectPatch(asset,0,{'/emitters/constructor/prototype/x':3}),/unsafe/);
  const fields = JSON.parse(await fs.readFile(path.join(root,'assets/effects/smoke_reference.effect.json'),'utf8'));
  const changed = previewEffectPatch(fields,0,{'/emitters/0/renderers':[
    { type:'mesh',mesh:'mesh.ngmesh',material:'material.json' }]});
  assert.equal(changed.migration,'reset-required');
});
test('library cooking is deterministic, locks dependency bytes and publishes only after validation', async () => {
  const directory = await fs.mkdtemp(path.join(tmpdir(),'ng-effects-'));
  try {
    const source = path.join(directory,'source'), out = path.join(directory,'cooked'), assets = path.join(directory,'assets');
    await fs.mkdir(source); await fs.mkdir(path.join(assets,'textures'),{recursive:true});
    await fs.writeFile(path.join(source,'spark.effect.json'),JSON.stringify(await fixture()));
    const dependency = path.join(assets,'textures/lamp.albedo.ngtex');
    await fs.writeFile(dependency,'texture-v1');
    const options = {sourceDirectory:source,outputDirectory:out,assetRoot:assets};
    const first = await cookEffectLibrary(options), second = await cookEffectLibrary(options);
    assert.equal(first.packageHash,second.packageHash);
    assert.equal(first.runtimeIntegrated,false);
    await fs.writeFile(dependency,'texture-v2');
    const third = await cookEffectLibrary(options);
    assert.notEqual(third.packageHash,first.packageHash);
    assert.equal(third.libraryHash,first.libraryHash);
    const published = await fs.readFile(path.join(out,'library.json'),'utf8');
    const bad = await fixture(); bad.emitters[0].renderers[0].texture = '../bad';
    await fs.writeFile(path.join(source,'invalid.effect.json'),JSON.stringify(bad));
    await assert.rejects(cookEffectLibrary(options));
    assert.equal(await fs.readFile(path.join(out,'library.json'),'utf8'),published);
  } finally { await fs.rm(directory,{recursive:true,force:true}); }
});

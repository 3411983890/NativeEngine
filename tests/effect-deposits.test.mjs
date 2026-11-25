import test from 'node:test';
import assert from 'node:assert/strict';
import { compileEffect } from '../engine/effects/asset.mjs';
import { depositFixture } from '../tools/effect-deposit-fixtures.mjs';

test('fieldDeposit keeps integer quantum, declared access and stable authored layout', () => {
  const source=depositFixture(), before=structuredClone(source), result=compileEffect(source);
  assert.deepEqual(source,before);
  const module=result.emitters[0].modules.find(m=>m.type==='fieldDeposit');
  assert.equal(module.params.quantum,100);
  assert.deepEqual(module.access.reads,['position']);assert.deepEqual(module.access.fieldWrites,['density']);
  assert.equal(module.access.phase,'postSimulation');
  assert.equal(result.hash,compileEffect(structuredClone(source)).hash);
  const plain=structuredClone(source);for(const e of plain.emitters)e.modules=e.modules.filter(m=>m.type!=='fieldDeposit');
  assert.equal(result.layoutHash,compileEffect(plain).layoutHash);
});
test('fieldDeposit rejects undefined scalar meaning and undeclared field access', () => {
  for(const change of [
    a=>{delete a.field;},a=>{a.field.channels=['moisture'];},
    a=>{a.emitters[0].modules[1].params.channel='temperatureKelvin';},
    a=>{a.emitters[0].modules[1].params.channel='windVelocity';},
    a=>{a.emitters[0].modules[1].params.unit='fake-unit';},
  ]){const a=depositFixture();change(a);assert.throws(()=>compileEffect(a));}
});
test('fieldDeposit quantum must be a positive exact UInt32 integer', () => {
  for(const quantum of [0,-1,.5,NaN,Infinity,0x100000000]){const a=depositFixture();a.emitters[0].modules[1].params.quantum=quantum;assert.throws(()=>compileEffect(a));}
  const a=depositFixture();a.maxAlive=1;a.emitters.length=1;a.emitters[0].modules[1].params.quantum=0xFFFFFFFF;
  assert.equal(compileEffect(a).source.emitters[0].modules[1].params.quantum,0xFFFFFFFF);
});
test('fieldDeposit proves capacity times all same-channel quanta before runtime', () => {
  const a=depositFixture();a.maxAlive=8;a.emitters[0].modules[1].params.quantum=Math.floor(0xFFFFFFFF/8);
  assert.throws(()=>compileEffect(a),{code:'NG-E-EFFECT-BUDGET'});
  a.emitters.length=1;assert.doesNotThrow(()=>compileEffect(a));
  a.emitters[0].modules.push({id:'again',type:'fieldDeposit',params:{channel:'density',quantum:1}});
  assert.throws(()=>compileEffect(a),{code:'NG-E-EFFECT-BUDGET'});
});
test('separate scalar channels have independent UInt32 accumulation bounds', () => {
  const a=depositFixture();a.maxAlive=1;a.field.channels=['density','moisture'];
  a.emitters[0].modules[1].params.quantum=0xFFFFFFFF;
  a.emitters[1].modules[1].params={channel:'moisture',quantum:0xFFFFFFFF};
  assert.doesNotThrow(()=>compileEffect(a));
});
test('a simulation module cannot depend on the later deposition stage', () => {
  const a=depositFixture();a.emitters[0].modules.push({id:'late',type:'acceleration',after:['deposit'],params:{value:[0,1,0]}});
  assert.throws(()=>compileEffect(a),/postSimulation/);
  a.emitters[0].modules[2].after=['initial'];a.emitters[0].modules[1].after=['late'];
  assert.doesNotThrow(()=>compileEffect(a));
});

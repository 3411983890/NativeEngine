import test from 'node:test';
import assert from 'node:assert/strict';
import {
  compileMir,
  canonicalJson,
  materialToMir,
  mirFeatureBits,
  mirHash,
  mirToMaterial,
  validateMir,
} from '../engine/material/mir.mjs';

const SOURCE = {
  albedo: 'cobble',
  normal: 'cobble',
  orm: 'cobble',
  uvScale: 2,
  roughness: 0.9,
  metalness: 0.1,
  wetness: 0.35,
  wear: 0.1,
};

test('materialToMir/mirToMaterial round-trip', () => {
  const mir = materialToMir('cobble', SOURCE);
  assert.equal(validateMir(mir), true);
  assert.deepEqual(mirToMaterial(mir), { ...SOURCE, uvScale: 2 });
  assert.equal(mirHash(mir).length, 64);
});

test('MIR hash is canonical across key order', () => {
  const mir = materialToMir('cobble', SOURCE);
  const reordered = {
    ...mir,
    params: { wear: mir.params.wear, wetness: mir.params.wetness, ...mir.params },
  };
  assert.equal(canonicalJson(mir), canonicalJson(reordered));
  assert.equal(mirHash(mir), mirHash(reordered));
});

test('MIR rejects untyped/unknown fields', () => {
  const mir = materialToMir('cobble', SOURCE);
  assert.throws(() => validateMir({ ...mir, schema: 'other' }), /schema/);
  assert.throws(() => validateMir({ ...mir, typo: true }), /unsupported/);
  assert.throws(() => validateMir({ ...mir, params: {} }), /finite/);
  assert.throws(() => validateMir({ ...mir, base: { ...mir.base, typo: {} } }), /unsupported/);
  assert.throws(() => materialToMir('cobble', { ...SOURCE, mystery: 1 }), /unsupported/);
  assert.throws(() => materialToMir('cobble', { ...SOURCE, wetness: 2 }), /\[0, 1\]/);
  assert.throws(() => materialToMir('cobble', { ...SOURCE, albedo: 'bad name!' }), /texture/);
});

test('compileMir reports stable features and material', () => {
  const compiled = compileMir(materialToMir('cobble', SOURCE));
  assert.deepEqual(compiled.features, ['normal-map', 'wet', 'wear']);
  assert.equal(compiled.material.wetness, 0.35);
  assert.equal(compiled.hash, mirHash(materialToMir('cobble', SOURCE)));
});

test('clearcoat layer is typed and round-trips', () => {
  const mir = materialToMir('paint', { ...SOURCE, clearcoat: 0.8, clearcoatRoughness: 0.2 });
  assert.deepEqual(mirFeatureBits(mir), ['normal-map', 'wet', 'wear', 'clearcoat']);
  assert.equal(mirToMaterial(mir).clearcoat, 0.8);
  assert.throws(() => materialToMir('paint', { ...SOURCE, clearcoat: 2 }), /\[0, 1\]/);
});
test('alpha mask mode and cutoff survive canonical MIR compilation',()=>{
  const source={...SOURCE,alphaMode:'MASK',alphaCutoff:.4},mir=materialToMir('leaf',source),compiled=compileMir(mir);
  assert.deepEqual(compiled.material,source);assert.ok(compiled.features.includes('alpha-mask'));
  assert.throws(()=>validateMir({...mir,alpha:{mode:'INVALID',cutoff:.4}}),/OPAQUE, MASK or BLEND/);
  assert.throws(()=>validateMir({...mir,alpha:{mode:'MASK',cutoff:2}}),/\[0, 1\]/);
});

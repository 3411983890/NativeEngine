import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import {
  LAYERED_SCHEMA, LAYERED_SCHEMA_V2, defaultSurfaceRelief, migrateLayeredMaterialToV2,
  normalizeLayeredMaterial, surfaceHeightMetres, compileLayeredMaterial, LayeredMaterialError,
} from '../../engine/material/layered.mjs';
import { encodeSurfaceRelief, SURFACE_RELIEF_GPU_BYTES } from '../../engine/material/layered-gpu.mjs';
import { surfaceBasis, marchParallax, sampleParallaxReference, analyticHeightField, projectDisplacement } from '../../engine/material/surface-relief.mjs';
import {
  cookDirectory, encodePng, encodePng16Gray, readNgtex, decodePng16Gray, mipBytes,
  NGTEX_FORMAT_R16, NGTEX_FORMAT_RGBA8,
} from '../../tools/textures.mjs';

const v1 = () => ({
  schema: LAYERED_SCHEMA, name: 'stone_mud',
  layers: [
    { id: 'stone', textures: { albedo: { name: 'stone' }, height: { name: 'stone' } } },
    { id: 'mud', textures: { albedo: { name: 'mud' }, height: { name: 'mud' } }, blend: { mode: 'height', source: 'textureMask', texture: { name: 'mud_spread' }, channel: 'r' } },
  ],
});
const v2 = (relief = {}) => ({
  schema: LAYERED_SCHEMA_V2, name: 'stone_mud', layers: v1().layers,
  surfaceRelief: { ...defaultSurfaceRelief('stone'), mode: 'pom', amplitudeMetres: .03, ...relief },
});

test('v1 keeps its shape; v2 migration is explicit and defaults to off', () => {
  const v1Material = normalizeLayeredMaterial(v1());
  assert.equal(v1Material.schema, LAYERED_SCHEMA);
  assert.equal(v1Material.surfaceRelief, undefined);
  const migrated = migrateLayeredMaterialToV2(v1());
  assert.equal(migrated.schema, LAYERED_SCHEMA_V2);
  assert.equal(migrated.surfaceRelief.mode, 'off');
  assert.equal(migrated.surfaceRelief.amplitudeMetres, 0);
  assert.equal(migrated.surfaceRelief.heightSource.layer, 'stone');
  // Migrating a v2 material is a no-op with respect to the authored relief.
  assert.deepEqual(migrateLayeredMaterialToV2(v2()), normalizeLayeredMaterial(v2()));
});

test('v2 surface relief rejects unknown fields, bad ranges and dangling references', () => {
  assert.throws(() => normalizeLayeredMaterial({ ...v2(), surfaceRelief: undefined }), /surfaceRelief/);
  assert.throws(() => normalizeLayeredMaterial(v2({ mode: 'screenSpace' })), /mode/);
  assert.throws(() => normalizeLayeredMaterial(v2({ amplitudeMetres: 2 })), /amplitudeMetres/);
  assert.throws(() => normalizeLayeredMaterial(v2({ minSteps: 32, maxSteps: 16 })), /maxSteps/);
  assert.throws(() => normalizeLayeredMaterial(v2({ fadeStartMetres: 10, fadeEndMetres: 4 })), /fadeEndMetres/);
  assert.throws(() => normalizeLayeredMaterial(v2({ heightSource: { kind: 'layer', layer: 'missing' } })), /existing layer/);
  assert.throws(() => normalizeLayeredMaterial(v2({ heightSource: { kind: 'texture', texture: { name: 'g', uv: { set: 99 } } } })), /integer in \[0, 15\]/);
  const extra = v2(); extra.surfaceRelief.selfShadowStrength = 1;
  assert.throws(() => normalizeLayeredMaterial(extra), /unsupported field/);
  assert.throws(() => normalizeLayeredMaterial({ ...v1(), surfaceRelief: defaultSurfaceRelief('stone') }), /unsupported field/);
});

test('relief GPU record matches the 96-byte HLSL layout', () => {
  const relief = v2({ invert: true, selfShadow: true, minSteps: 8, maxSteps: 24, refineSteps: 3, fadeStartMetres: 6, fadeEndMetres: 14, grazingFade: .12, shadowSteps: 5 });
  const { buffer } = encodeSurfaceRelief(relief);
  assert.equal(buffer.length, SURFACE_RELIEF_GPU_BYTES);
  assert.equal(buffer.readUInt32LE(12), 0xffffffff);
  assert.equal(buffer.readUInt32LE(28), 0, 'layer height source has no sampler mapping');
  assert.equal(buffer.readUInt32LE(32), 0);
  assert.equal(buffer.readUInt32LE(36), 0);
  assert.equal(buffer.readUInt32LE(40), 1 | 4 | 8, 'mode 1, invert and self shadow bits');
  assert.equal(buffer.readUInt32LE(44), 0, 'stone is layer zero');
  assert.ok(Math.abs(buffer.readFloatLE(48) - .03) < 1e-7);
  assert.equal(buffer.readFloatLE(52), 1);
  assert.equal(buffer.readFloatLE(56), 8);
  assert.equal(buffer.readFloatLE(60), 24);
  assert.equal(buffer.readFloatLE(64), 3);
  assert.equal(buffer.readFloatLE(68), 6);
  assert.equal(buffer.readFloatLE(72), 14);
  assert.ok(Math.abs(buffer.readFloatLE(76) - .12) < 1e-7);
  assert.equal(buffer.readFloatLE(80), 5);
  const texture = v2({ heightSource: { kind: 'texture', texture: { name: 'ground_height', uv: { set: 1, mapping: { scale: [2, 3], offset: [.5, .5] } } } } });
  const encoded = encodeSurfaceRelief(texture, { textures: { 'height/ground_height': { index: 42 } } });
  assert.equal(encoded.buffer.readUInt32LE(12), 42);
  assert.equal(encoded.buffer.readUInt32LE(32), 1);
  assert.equal(encoded.buffer.readUInt32LE(40) & 16, 16, 'baked texture source bit');
});

test('compile includes the baked height texture dependency', () => {
  const hashes = { 'albedo/stone': '1'.repeat(64), 'height/stone': '2'.repeat(64), 'albedo/mud': '3'.repeat(64), 'height/mud': '4'.repeat(64), 'mask/mud_spread': '5'.repeat(64), 'height/ground_height': '6'.repeat(64) };
  const material = v2({ heightSource: { kind: 'texture', texture: { name: 'ground_height', uv: { set: 0 } } } });
  const compiled = compileLayeredMaterial(material, { hashes, shaderFingerprint: 'a'.repeat(64) });
  assert.ok(compiled.dependencies.some((entry) => entry.semantic === 'height' && entry.name === 'ground_height'));
  assert.throws(() => compileLayeredMaterial(material, { hashes: { ...hashes, 'height/ground_height': undefined }, shaderFingerprint: 'a'.repeat(64) }), /content hash/);
});

test('height metres use the configured reference, amplitude and direction', () => {
  const relief = { amplitudeMetres: .04, referenceHeight: 1, invert: false };
  assert.equal(surfaceHeightMetres(1, relief), 0);
  assert.ok(Math.abs(surfaceHeightMetres(.5, relief) + .02) < 1e-12);
  assert.ok(Math.abs(surfaceHeightMetres(.75, { ...relief, invert: true }) + .03) < 1e-12);
  assert.ok(Math.abs(surfaceHeightMetres(.25, { ...relief, invert: true }) + .01) < 1e-12);
  assert.ok(Math.abs(surfaceHeightMetres(.5, { amplitudeMetres: .04, referenceHeight: .5 }) ) < 1e-12);
});

test('bounded POM march matches a dense analytic root', () => {
  const basis = surfaceBasis({ normal: [0, 0, 1], dpdu: [1, 0, 0], dpdv: [0, 1, 0] });
  assert.deepEqual(projectDisplacement(basis, [.5, .25, 0]), [.5, .25]);
  const field = analyticHeightField('groove', { center: .012, sigma: .004, depth: .7 });
  const relief = { amplitudeMetres: .05, referenceHeight: 1, invert: false, minSteps: 16, maxSteps: 64, refineSteps: 5, fadeStartMetres: 0, fadeEndMetres: 0, grazingFade: 0 };
  const angle = 20 * Math.PI / 180;
  const view = [Math.sin(angle), 0, Math.cos(angle)];
  const marched = marchParallax({ relief, uv: [0, .5], view, basis, sampleHeight: field });
  const reference = sampleParallaxReference({ relief, uv: [0, .5], view, basis, sampleHeight: field, resolution: 1 << 16 });
  assert.equal(marched.hit, true);
  assert.equal(reference.hit, true);
  assert.ok(Math.abs(marched.depthMetres - reference.depthMetres) < .002, `${marched.depthMetres} vs ${reference.depthMetres}`);
  assert.ok(Math.abs(marched.uvOffset[0] - reference.uvOffset[0]) < .002);
});

test('plane and slope references degrade to no displacement or a bounded offset', () => {
  const basis = surfaceBasis({ normal: [0, 0, 1], dpdu: [1, 0, 0], dpdv: [0, 1, 0] });
  const relief = { amplitudeMetres: .03, referenceHeight: 1, invert: false, minSteps: 8, maxSteps: 24, refineSteps: 3 };
  const plane = marchParallax({ relief, uv: [.2, .3], view: [.3, 0, .95], basis, sampleHeight: analyticHeightField('plane') });
  assert.equal(plane.hit, true);
  assert.ok(Math.abs(plane.depthMetres) < 1e-3);
  assert.ok(Math.abs(plane.uvOffset[0]) < 1e-3);
  const slope = marchParallax({ relief, uv: [0, 0], view: [.4, 0, .92], basis, sampleHeight: analyticHeightField('slope', { slopeU: .5 }) });
  assert.ok(slope.depthMetres >= 0 && slope.depthMetres <= relief.amplitudeMetres + 1e-9);
});

test('zero amplitude restores the legacy sampling path', () => {
  const basis = surfaceBasis({ normal: [0, 0, 1], dpdu: [1, 0, 0], dpdv: [0, 1, 0] });
  const result = marchParallax({ relief: { amplitudeMetres: 0, referenceHeight: 1 }, uv: [0, 0], view: [.5, 0, .86], basis, sampleHeight: analyticHeightField('groove') });
  assert.deepEqual(result, { uvOffset: [0, 0], depthMetres: 0, steps: 0, hit: false, fade: 0 });
});

test('R16 cook preserves 16-bit height precision and averages mips', () => {
  assert.equal(mipBytes(2048, 2048, NGTEX_FORMAT_R16), 2048 * 2048 * 2);
  assert.equal(mipBytes(1024, 1024, NGTEX_FORMAT_RGBA8), 1024 * 1024 * 4);
  const width = 16, height = 16, samples = new Uint16Array(width * height);
  for (let i = 0; i < samples.length; i += 1) samples[i] = 1000 + i * 977 % 60000;
  const png = encodePng16Gray({ width, height, data: samples });
  const decoded = decodePng16Gray(png);
  assert.deepEqual([...decoded.data], [...samples]);
  const dir = mkdtempSync(path.join(tmpdir(), 'ng-r16-'));
  try {
    const source = path.join(dir, 'source'), out = path.join(dir, 'textures');
    mkdirSync(source, { recursive: true });
    writeFileSync(path.join(source, 'ground.height.png'), png);
    const result = cookDirectory({ source, out, size: width, format: 'auto', incremental: false });
    const entry = result.manifest.textures[0];
    assert.equal(entry.format, 'r16');
    assert.equal(entry.formatCode, 5);
    const container = readNgtex(readFileSync(path.join(out, entry.file)));
    assert.equal(container.format, NGTEX_FORMAT_R16);
    assert.equal(container.srgb, false);
    assert.equal(container.mipCount, 1 + Math.floor(Math.log2(Math.max(width, height))));
    assert.equal(container.mips[0].data.length, width * height * 2);
    assert.equal(container.mips[0].data.readUInt16LE(0), samples[0]);
    const second = container.mips[1];
    const expected = Math.round((samples[0] + samples[1] + samples[width] + samples[width + 1]) / 4);
    assert.equal(second.data.readUInt16LE(0), expected);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('8-bit height sources still cook to the RGBA8 fallback', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'ng-rgba8-height-'));
  try {
    const source = path.join(dir, 'source'), out = path.join(dir, 'textures');
    mkdirSync(source, { recursive: true });
    const data = Buffer.alloc(4 * 4 * 4, 128);
    for (let i = 3; i < data.length; i += 4) data[i] = 255;
    writeFileSync(path.join(source, 'flat.height.png'), encodePng({ width: 4, height: 4, data }));
    const result = cookDirectory({ source, out, size: 4, format: 'auto', incremental: false });
    assert.equal(result.manifest.textures[0].formatCode, NGTEX_FORMAT_RGBA8);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

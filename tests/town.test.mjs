import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { validateScene } from '../engine/scene/index.mjs';
import { readNgtex } from '../tools/textures.mjs';

const scene = JSON.parse(readFileSync(join('assets', 'town.scene.json'), 'utf8'));

test('town scene validates against the v2 schema', () => {
  const result = validateScene(scene);
  assert.equal(result.valid, true, result.errors.join('\n'));
  assert.equal(scene.schemaVersion, 2);
  assert.equal(scene.entities.filter((entity) => entity.kind === 'player').length, 1);
  assert.ok(scene.entities.filter((entity) => entity.kind === 'ground').length >= 1);
  assert.ok(scene.entities.filter((entity) => entity.kind === 'light').length <= 4);
});

test('every entity material resolves to a named material', () => {
  const names = new Set(Object.keys(scene.materials));
  for (const entity of scene.entities) {
    if (entity.material === undefined) continue;
    assert.equal(typeof entity.material, 'string', `${entity.name} uses an inline material`);
    assert.ok(names.has(entity.material), `${entity.name} references ${entity.material}`);
  }
});

test('every material texture is cooked with a full mip chain', () => {
  const sizes = new Set();
  for (const [name, material] of Object.entries(scene.materials)) {
    for (const semantic of ['albedo', 'normal', 'orm']) {
      const file = join('assets', 'textures', `${material[semantic]}.${semantic}.ngtex`);
      const texture = readNgtex(readFileSync(file), file);
      assert.equal(texture.semantic, semantic);
      assert.equal(texture.width, texture.height, `${name} ${semantic} aspect`);
      assert.ok(texture.width >= 64 && texture.width <= 256, `${name} ${semantic} size`);
      assert.equal(texture.mipCount, Math.log2(texture.width) + 1, `${name} ${semantic} mips`);
      sizes.add(`${texture.width}x${texture.height}x${texture.formatName}:${texture.srgb}`);
    }
  }
  // The budget cook may produce a mix of 256/128/64 rungs; all must be complete chains.
  assert.ok(sizes.size >= 1);
});

test('the town has a default material built from flat plates', () => {
  assert.ok(scene.materials.default);
  assert.equal(scene.materials.default.albedo, 'flat');
  assert.equal(scene.materials.default.normal, 'flat');
  assert.equal(scene.materials.default.orm, 'flat');
});

test('the town references every generated material set at least once', () => {
  const used = new Set(scene.entities.map((entity) => entity.material).filter(Boolean));
  // Flat is only used as a fallback material source, not directly by entities.
  const expected = Object.keys(scene.materials).filter((name) => name !== 'default');
  for (const name of expected) assert.ok(used.has(name), `${name} is unused`);
});

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { undoSceneFile, loadScene } from '../engine/scene/index.mjs';
import {
  applyMaterialPatch,
  applyMaterialInstance,
  listMaterials,
  materialInstance,
  patchMaterial,
  previewMaterialPatch,
} from '../tools/material.mjs';

function scene() {
  return {
    schemaVersion: 2,
    projectId: 'b1000000-0000-4000-8000-000000000001',
    name: 'material fixture',
    revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    materials: {
      default: { albedo: 'flat', normal: 'flat', orm: 'flat' },
      brick: { albedo: 'brick', normal: 'brick', orm: 'brick', wetness: 0.1, wear: 0 },
    },
    entities: [
      { id: 'b1000000-0000-4000-8000-000000000010', name: 'Ground', kind: 'ground', position: [0, 0, 0], scale: [10, 0.2, 10], color: [1, 1, 1], collider: true, material: 'default' },
      { id: 'b1000000-0000-4000-8000-000000000011', name: 'Wall', kind: 'wall', position: [0, 1, 0], scale: [1, 2, 1], color: [1, 1, 1], collider: true, material: 'brick' },
      { id: 'b1000000-0000-4000-8000-000000000012', name: 'Player', kind: 'player', position: [0, 0, 3], scale: [0.6, 1.8, 0.6], color: [1, 1, 1], collider: true, material: 'default' },
    ],
  };
}

test('material commits reject stale revisions and share scene undo history', async () => {
  const directory=mkdtempSync(join(tmpdir(),'nativegpu-material-history-'));
  try {
    const file=join(directory,'scene.json'), original=scene();
    writeFileSync(file,JSON.stringify(original));
    await applyMaterialPatch({scenePath:file,material:'brick',changes:{wetness:0.8},expectedRevision:0,root:directory});
    await assert.rejects(applyMaterialPatch({scenePath:file,material:'brick',changes:{wear:0.4},expectedRevision:0,root:directory}), {code:'REVISION_CONFLICT'});
    await assert.rejects(applyMaterialPatch({scenePath:file,material:'brick',changes:{wear:0.4},root:directory}), {code:'REVISION_CONFLICT'});
    assert.equal((await loadScene(file)).materials.brick.wetness,0.8);
    await undoSceneFile(file,{projectId:original.projectId,expectedRevision:1});
    assert.equal((await loadScene(file)).materials.brick.wetness,0.1);
    await applyMaterialInstance({scenePath:file,source:'brick',name:'brick_copy',expectedRevision:2,root:directory});
    await undoSceneFile(file,{projectId:original.projectId,expectedRevision:3});
    assert.equal((await loadScene(file)).materials.brick_copy,undefined);
    assert.equal((await loadScene(file)).revision,4);
  } finally {rmSync(directory,{recursive:true,force:true});}
});

test('material saves respect the scene writer lock', async () => {
  const directory=mkdtempSync(join(tmpdir(),'nativegpu-material-lock-'));
  try {
    const file=join(directory,'scene.json');writeFileSync(file,JSON.stringify(scene()));
    writeFileSync(`${file}.lock`,'{}');
    await assert.rejects(applyMaterialPatch({scenePath:file,material:'brick',changes:{wetness:0.8},expectedRevision:0,root:directory}), {code:'PROJECT_BUSY'});
    assert.equal(JSON.parse(readFileSync(file,'utf8')).revision,0);
  } finally {rmSync(directory,{recursive:true,force:true});}
});

test('patchMaterial validates fields and counts affected entities', () => {
  const result = patchMaterial(scene(), 'brick', { wetness: 0.7, wear: 0.2 });
  assert.deepEqual(result.changed, ['wetness', 'wear']);
  assert.equal(result.after.wetness, 0.7);
  assert.equal(result.affectedEntities, 1);
  assert.equal(result.revision, 1);
  assert.throws(() => patchMaterial(scene(), 'brick', { unknown: 1 }), /unsupported/);
  assert.throws(() => patchMaterial(scene(), 'nope', { wetness: 0.2 }), /unknown material/);
  assert.throws(() => patchMaterial(scene(), 'brick', { wetness: 2 }), /\[0, 1\]/);
});

test('preview does not write and apply is atomic', async () => {
  const directory = mkdtempSync(join(tmpdir(), 'nativegpu-material-'));
  try {
    const file = join(directory, 'scene.json');
    writeFileSync(file, `${JSON.stringify(scene(), null, 2)}\n`);
    const preview = previewMaterialPatch({
      scenePath: file,
      material: 'brick',
      changes: { roughness: 0.5 },
      root: directory,
    });
    assert.equal(preview.applied, false);
    assert.equal(JSON.parse(readFileSync(file, 'utf8')).materials.brick.roughness, undefined);
    const applied = await applyMaterialPatch({
      expectedRevision: 0,
      scenePath: file,
      material: 'brick',
      changes: { roughness: 0.5 },
      root: directory,
    });
    assert.equal(applied.applied, true);
    const stored = JSON.parse(readFileSync(file, 'utf8'));
    assert.equal(stored.materials.brick.roughness, 0.5);
    assert.equal(stored.revision, 1);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('materialInstance clones a material and optionally rebinds one entity', () => {
  const source = scene();
  const result = materialInstance(source, 'brick', 'brick_wet', 'b1000000-0000-4000-8000-000000000011');
  assert.equal(result.material, 'brick_wet');
  assert.equal(result.scene.materials.brick_wet.wetness, 0.1);
  assert.equal(result.scene.entities.find((entity) => entity.id.endsWith('11')).material, 'brick_wet');
  assert.equal(result.revision, 1);
  assert.throws(() => materialInstance(source, 'brick', 'brick'), /differ/);
  assert.throws(() => materialInstance(result.scene, 'brick', 'brick_wet'), /already exists/);
});

test('clearcoat is a typed scalar and survives material patches', () => {
  const result = patchMaterial(scene(), 'brick', { clearcoat: 0.6, clearcoatRoughness: 0.2 });
  assert.equal(result.after.clearcoat, 0.6);
  assert.equal(result.after.clearcoatRoughness, 0.2);
  assert.throws(() => patchMaterial(scene(), 'brick', { clearcoat: 2 }), /\[0, 1\]/);
});

test('listMaterials reports entity usage and surface controls', () => {
  const list = listMaterials(scene(), process.cwd());
  const brick = list.find((entry) => entry.name === 'brick');
  assert.equal(brick.entityCount, 1);
  assert.equal(brick.wetness, 0.1);
  assert.equal(brick.wear, 0);
});

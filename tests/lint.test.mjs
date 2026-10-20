import test from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import { ENTITY_KINDS as ENGINE_ENTITY_KINDS } from '../engine/scene/index.mjs';
import {
  lintScene, ENTITY_KINDS, LINT_RULES,
} from '../engine/scene/lint.mjs';

const PROJECT = 'b2e00000-0000-4000-8000-000000000001';
const PLAYER = 'b2e00000-0000-4000-8000-000000000010';
const PLAYER_TWO = 'b2e00000-0000-4000-8000-000000000011';
const GROUND = 'b2e00000-0000-4000-8000-000000000012';
const CRATE_A = 'b2e00000-0000-4000-8000-000000000013';
const KEY = 'b2e00000-0000-4000-8000-000000000014';
const DOOR = 'b2e00000-0000-4000-8000-000000000015';
const GOAL = 'b2e00000-0000-4000-8000-000000000016';
const PROP = 'b2e00000-0000-4000-8000-000000000017';
const BAD = 'b2e00000-0000-4000-8000-000000000018';
const OUT_FAR = 'b2e00000-0000-4000-8000-000000000019';
const OUT_SUNK = 'b2e00000-0000-4000-8000-00000000001a';
const EDGE = 'b2e00000-0000-4000-8000-00000000001b';

function entity(id, name, kind, position, scale = [1, 1, 1], collider = false) {
  return { id, name, kind, position, scale, color: [0.5, 0.5, 0.5], collider };
}

function makeScene(entities, overrides = {}) {
  return {
    schemaVersion: 1,
    projectId: PROJECT,
    name: 'Lint fixture',
    revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities,
    ...overrides,
  };
}

function ofCode(result, code) {
  return result.findings.filter(finding => finding.code === code);
}

function codes(result) {
  return result.findings.map(finding => finding.code);
}

function ground() {
  return entity(GROUND, 'Ground', 'ground', [0, -0.2, 0], [20, 0.4, 20], true);
}

function player(position = [0, 0, 6]) {
  return entity(PLAYER, 'Player spawn', 'player', position, [0.6, 1.8, 0.6], true);
}

function goal(position = [0, 0, -2]) {
  return entity(GOAL, 'Finish marker', 'goal', position, [1.5, 0.1, 1.5]);
}

/** Four thin walls forming a closed square around `center`. */
function enclosedWalls(prefix, center, half) {
  const [x, , z] = center;
  const height = 3;
  const thickness = 0.4;
  const span = half * 2;
  return [
    entity(randomUUID(), `${prefix} north`, 'wall', [x, height / 2, z - half], [span, height, thickness], true),
    entity(randomUUID(), `${prefix} south`, 'wall', [x, height / 2, z + half], [span, height, thickness], true),
    entity(randomUUID(), `${prefix} west`, 'wall', [x - half, height / 2, z], [thickness, height, span], true),
    entity(randomUUID(), `${prefix} east`, 'wall', [x + half, height / 2, z], [thickness, height, span], true),
  ];
}

test('structural rules report a missing player, a missing goal and multiple players', () => {
  const noPlayer = lintScene(makeScene([ground(), goal()]));
  assert.equal(noPlayer.ok, false);
  assert.equal(ofCode(noPlayer, 'NG-LINT-NO-PLAYER').length, 1);
  assert.equal(ofCode(noPlayer, 'NG-LINT-NO-PLAYER')[0].entityId, null);

  const noGoal = lintScene(makeScene([ground(), player()]));
  assert.equal(noGoal.ok, false);
  assert.equal(ofCode(noGoal, 'NG-LINT-NO-GOAL').length, 1);
  assert.equal(ofCode(noGoal, 'NG-LINT-NO-GOAL')[0].entityId, null);

  const twoPlayers = lintScene(makeScene([
    ground(), player(), goal(),
    entity(PLAYER_TWO, 'Second spawn', 'player', [2, 0, 6], [0.6, 1.8, 0.6], true),
  ]));
  assert.equal(twoPlayers.ok, false);
  const multi = ofCode(twoPlayers, 'NG-LINT-MULTI-PLAYER');
  assert.equal(multi.length, 1);
  assert.equal(multi[0].entityId, PLAYER_TWO);
  assert.equal(multi[0].severity, 'error');
  assert.equal(multi[0].detail.count, 2);
});

test('duplicate ids are compared case-insensitively and unknown kinds are rejected', () => {
  const result = lintScene(makeScene([
    ground(), player(), goal(),
    entity(CRATE_A, 'Crate', 'prop', [2, 0.6, 2], [1.2, 1.2, 1.2], true),
    entity(CRATE_A.toUpperCase(), 'Crate copy', 'prop', [-2, 0.6, 2], [1.2, 1.2, 1.2], true),
    entity(BAD, 'Mystery', 'npc', [0, 1, -5], [1, 1, 1]),
  ]));

  assert.equal(result.ok, false);
  const duplicates = ofCode(result, 'NG-LINT-DUP-ID');
  assert.equal(duplicates.length, 1);
  assert.equal(duplicates[0].entityId, CRATE_A.toUpperCase());
  assert.equal(duplicates[0].severity, 'error');

  const badKind = ofCode(result, 'NG-LINT-BAD-KIND');
  assert.equal(badKind.length, 1);
  assert.equal(badKind[0].entityId, BAD);
  assert.equal(badKind[0].detail.kind, 'npc');
});

test('out-of-world flags positions beyond the play area or below the floor, but not the boundary', () => {
  const result = lintScene(makeScene([
    ground(), player(), goal(),
    entity(OUT_FAR, 'Far prop', 'prop', [12, 0.5, 0], [1, 1, 1]),
    entity(OUT_SUNK, 'Sunk prop', 'prop', [0, -0.5, 0], [1, 1, 1]),
    entity(EDGE, 'Boundary prop', 'prop', [10, 0.5, 0], [1, 1, 1]),
  ]));

  assert.equal(result.ok, false);
  const out = ofCode(result, 'NG-LINT-OUT-OF-WORLD');
  assert.equal(out.length, 2);
  assert.deepEqual(out.map(finding => finding.entityId).sort(), [OUT_FAR, OUT_SUNK].sort());
  const far = out.find(finding => finding.entityId === OUT_FAR);
  assert.deepEqual(far.detail.axes, ['x']);
  const sunk = out.find(finding => finding.entityId === OUT_SUNK);
  assert.deepEqual(sunk.detail.axes, ['y']);
  assert.equal(ofCode(result, 'NG-LINT-OUT-OF-WORLD').some(finding => finding.entityId === EDGE), false);
});

test('overlap reports intersecting solid colliders and ignores touching boxes', () => {
  const result = lintScene(makeScene([
    ground(), player(), goal(),
    entity(CRATE_A, 'Crate A', 'prop', [0, 0.6, 0], [1.2, 1.2, 1.2], true),
    entity(PROP, 'Crate B', 'prop', [0.3, 0.6, 0], [1.2, 1.2, 1.2], true),
    entity(BAD, 'Crate C', 'prop', [1.5, 0.6, 0], [1.2, 1.2, 1.2], true),
  ]));

  assert.equal(result.ok, false);
  const overlaps = ofCode(result, 'NG-LINT-OVERLAP');
  assert.equal(overlaps.length, 1);
  assert.deepEqual(overlaps[0].detail.entities, [CRATE_A, PROP]);
  assert.ok(overlaps[0].detail.penetration.every(value => value > 0));
  assert.equal(overlaps[0].severity, 'error');
});

test('key unreachable: a walled-off key reports the reachable area percentage', () => {
  const walls = enclosedWalls('key box', [0, 0, 0], 2);
  const result = lintScene(makeScene([
    ground(), player(), goal([0, 0, -6]),
    entity(KEY, 'Courtyard key', 'key', [0, 0.5, 0], [0.4, 0.4, 0.4]),
    ...walls,
  ]));

  assert.equal(result.ok, false);
  assert.deepEqual(codes(result), ['NG-LINT-KEY-UNREACHABLE']);
  const finding = ofCode(result, 'NG-LINT-KEY-UNREACHABLE')[0];
  assert.equal(finding.entityId, KEY);
  assert.ok(finding.detail.reachablePercent < 100);
  assert.equal(finding.detail.reachablePercent, finding.detail.reachableAreaPercent);
  assert.ok(finding.detail.reachableCells < finding.detail.freeCells);
  assert.match(finding.message, /unreachable/i);
});

test('goal unreachable: a walled-off goal reports a reachable percentage below 100', () => {
  const walls = enclosedWalls('goal box', [0, 0, 0], 2);
  const result = lintScene(makeScene([
    ground(), player(), goal([0, 0, 0]),
    entity(KEY, 'Key outside', 'key', [4, 0.5, 4], [0.4, 0.4, 0.4]),
    ...walls,
  ]));

  assert.equal(result.ok, false);
  assert.deepEqual(codes(result), ['NG-LINT-GOAL-UNREACHABLE']);
  const finding = ofCode(result, 'NG-LINT-GOAL-UNREACHABLE')[0];
  assert.equal(finding.entityId, GOAL);
  assert.ok(finding.detail.reachablePercent < 100);
  assert.ok(finding.detail.reachablePercent >= 0);
  assert.equal(finding.detail.reason, 'separated');
  assert.equal(finding.detail.grid.cellSize, 0.5);
  assert.equal(finding.detail.grid.cellsPerAxis, 40);
});

test('door rules distinguish a missing key from a key placed behind the door', () => {
  const base = [
    ground(), player(),
    entity(DOOR, 'Exit door', 'door', [0, 1.5, 4], [2, 3, 0.4], true),
    goal(),
  ];

  const noKey = lintScene(makeScene(base));
  assert.equal(noKey.ok, false);
  const missing = ofCode(noKey, 'NG-LINT-DOOR-WITHOUT-KEY');
  assert.equal(missing.length, 1);
  assert.equal(missing[0].entityId, DOOR);
  assert.equal(missing[0].severity, 'error');

  const keyBehind = lintScene(makeScene([
    ...base,
    entity(KEY, 'Late key', 'key', [0, 0.5, 0], [0.4, 0.4, 0.4]),
  ]));
  assert.equal(keyBehind.ok, true);
  assert.deepEqual(codes(keyBehind), ['NG-LINT-KEY-AFTER-DOOR']);
  const warning = ofCode(keyBehind, 'NG-LINT-KEY-AFTER-DOOR')[0];
  assert.equal(warning.severity, 'warning');
  assert.equal(warning.entityId, DOOR);
  assert.equal(warning.detail.keyId, KEY);

  const keyFirst = lintScene(makeScene([
    ...base,
    entity(KEY, 'Early key', 'key', [0, 0.5, 5], [0.4, 0.4, 0.4]),
  ]));
  assert.deepEqual(keyFirst, { ok: true, findings: [] });
});

test('entity budget is a warning, configurable and 200 by default', () => {
  const entities = [ground(), player(), goal()];
  for (let index = 0; index < 198; index += 1) {
    const x = -8 + (index % 17);
    const z = -8 + Math.floor(index / 17);
    entities.push(entity(randomUUID(), `Prop ${index}`, 'prop', [x, 0.5, z], [0.4, 1, 0.4]));
  }
  assert.equal(entities.length, 201);

  const result = lintScene(makeScene(entities));
  assert.equal(result.ok, true);
  assert.deepEqual(codes(result), ['NG-LINT-ENTITY-BUDGET']);
  const budget = ofCode(result, 'NG-LINT-ENTITY-BUDGET')[0];
  assert.equal(budget.severity, 'warning');
  assert.equal(budget.detail.count, 201);
  assert.equal(budget.detail.maxEntities, 200);

  const custom = lintScene(makeScene([ground(), player(), goal()]), { maxEntities: 2 });
  assert.deepEqual(codes(custom), ['NG-LINT-ENTITY-BUDGET']);
  assert.equal(ofCode(custom, 'NG-LINT-ENTITY-BUDGET')[0].detail.maxEntities, 2);
});

test('a clean scene returns ok true with no findings and is not mutated', () => {
  const document = makeScene([
    ground(), player(),
    entity(KEY, 'Early key', 'key', [0, 0.5, 3], [0.4, 0.4, 0.4]),
    entity(DOOR, 'Exit door', 'door', [0, 1.5, 1], [2, 3, 0.4], true),
    goal(),
  ]);
  const before = structuredClone(document);
  const result = lintScene(document);
  assert.deepEqual(result, { ok: true, findings: [] });
  assert.deepEqual(document, before);
});

test('the courtyard asset passes with zero error findings', async () => {
  const document = JSON.parse(await fs.readFile(
    new URL('../assets/courtyard.scene.json', import.meta.url), 'utf8',
  ));
  const result = lintScene(document);
  const errors = result.findings.filter(finding => finding.severity === 'error');
  assert.deepEqual(errors, []);
  assert.equal(result.ok, true);
});

test('lint rule catalog stays aligned with the engine and every finding honours the contract', () => {
  assert.deepEqual(ENTITY_KINDS, ENGINE_ENTITY_KINDS);
  const ruleCodes = LINT_RULES.map(rule => rule.code);
  assert.equal(new Set(ruleCodes).size, ruleCodes.length);
  for (const rule of LINT_RULES) {
    assert.ok(['error', 'warning'].includes(rule.severity));
  }

  const result = lintScene(makeScene([
    entity(BAD, 'Mystery', 'npc', [12, -1, 0], [1, 1, 1]),
  ]));
  assert.equal(result.ok, false);
  assert.ok(result.findings.length >= 3);
  for (const finding of result.findings) {
    assert.ok(ruleCodes.includes(finding.code), `${finding.code} must be registered`);
    assert.ok(finding.severity === 'error' || finding.severity === 'warning');
    assert.ok(finding.entityId === null || typeof finding.entityId === 'string');
    assert.equal(typeof finding.message, 'string');
    assert.ok(finding.message.length > 0);
    assert.equal(typeof finding.detail, 'object');
    assert.ok(finding.detail !== null && !Array.isArray(finding.detail));
  }
});

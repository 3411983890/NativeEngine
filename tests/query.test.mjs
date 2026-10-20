import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import {
  queryScene, QueryError, QUERY_FIELDS, QUERY_OPS, QUERY_SELECTS,
} from '../engine/scene/query.mjs';

const PROJECT = 'c3e00000-0000-4000-8000-000000000001';

const ALPHA = 'c3e00000-0000-4000-8000-000000000010';
const BETA = 'c3e00000-0000-4000-8000-000000000011';
const GAMMA = 'c3e00000-0000-4000-8000-000000000012';
const DELTA = 'c3e00000-0000-4000-8000-000000000013';
const EPSILON = 'c3e00000-0000-4000-8000-000000000014';
const ZETA = 'c3e00000-0000-4000-8000-000000000015';

const PATTERN = Object.freeze({
  checker: { albedo: [0.7, 0.45, 0.2], roughness: 0.7, metalness: 0, pattern: 'checker' },
  brick: { albedo: [0.3, 0.35, 0.4], roughness: 0.8, metalness: 0, pattern: 'brick' },
  metal: { albedo: [0.6, 0.65, 0.7], roughness: 0.2, metalness: 0.9, pattern: 'metal' },
});
const LIGHT = Object.freeze({ color: [1, 0.8, 0.4], intensity: 6, range: 8 });

function entity(id, name, kind, position, scale = [1, 1, 1], extra = {}) {
  return {
    id, name, kind, position, scale, color: [0.5, 0.5, 0.5], collider: false, ...extra,
  };
}

function makeScene(entities) {
  return {
    schemaVersion: 1,
    projectId: PROJECT,
    name: 'Query fixture',
    revision: 0,
    settings: { walkSpeed: 3, runSpeed: 6, jumpSpeed: 7, gravity: 20 },
    entities,
  };
}

/** Four entities with distinct kinds and one of each material pattern. */
function baseScene() {
  return makeScene([
    entity(ALPHA, 'Alpha Crate', 'prop', [-2, 1, 3], [1, 2, 3], {
      collider: true, material: PATTERN.checker,
    }),
    entity(BETA, 'Beta Wall', 'wall', [0, 2, 0], [4, 1, 0.5], {
      collider: true, material: PATTERN.brick,
    }),
    entity(GAMMA, 'Gamma Lamp', 'light', [5, 3, -1], [0.2, 0.2, 0.2], {
      material: PATTERN.metal, light: LIGHT,
    }),
    entity(DELTA, 'Delta Barrel', 'prop', [5, 0.5, 4], [1, 1, 1], {
      material: PATTERN.metal,
    }),
  ]);
}

function ids(result) {
  return result.rows.map(row => row.id);
}

function materials(result) {
  return result.rows.map(row => row.material);
}

function queryError(fn, code) {
  assert.throws(fn, error => {
    assert.ok(error instanceof QueryError, `expected QueryError, got ${error}`);
    assert.equal(error.code, code);
    return true;
  });
}

function deepFreeze(value) {
  if (value !== null && typeof value === 'object' && !Object.isFrozen(value)) {
    Object.freeze(value);
    for (const key of Object.keys(value)) deepFreeze(value[key]);
  }
  return value;
}

async function courtyard() {
  return JSON.parse(await fs.readFile(
    new URL('../assets/courtyard.scene.json', import.meta.url), 'utf8',
  ));
}

test('result contract: defaults, normalized appliedQuery and entity clones', () => {
  const document = baseScene();
  const result = queryScene(document);
  assert.deepEqual(Object.keys(result).sort(), [
    'aggregate', 'appliedQuery', 'count', 'ok', 'rows',
  ]);
  assert.equal(result.ok, true);
  assert.equal(result.count, 4);
  assert.equal(result.aggregate, null);
  assert.deepEqual(result.rows, document.entities);
  assert.notStrictEqual(result.rows[0], document.entities[0]);
  assert.notStrictEqual(result.rows[0].material, document.entities[0].material);
  assert.notStrictEqual(result.rows[0].position, document.entities[0].position);
  assert.deepEqual(result.appliedQuery, {
    select: 'entity', where: [], orderBy: null, limit: null, aggregate: null,
  });
  assert.deepEqual(QUERY_SELECTS, ['entity', 'material', 'light']);
  assert.deepEqual(QUERY_FIELDS, [
    'kind', 'id', 'name', 'x', 'y', 'z',
    'scaleX', 'scaleY', 'scaleZ', 'collider', 'material', 'lightIntensity',
  ]);
  assert.deepEqual(QUERY_OPS, [
    'eq', 'ne', 'lt', 'lte', 'gt', 'gte', 'in', 'contains', 'within',
  ]);
});

test('eq, ne and the four ordering comparisons filter the documented fields', () => {
  const document = baseScene();
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'kind', op: 'eq', value: 'wall' }],
  })), [BETA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'kind', op: 'ne', value: 'wall' }],
  })), [ALPHA, GAMMA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'y', op: 'lt', value: 2 }],
  })), [ALPHA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'y', op: 'lte', value: 2 }],
  })), [ALPHA, BETA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'y', op: 'gt', value: 2 }],
  })), [GAMMA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'y', op: 'gte', value: 2 }],
  })), [BETA, GAMMA]);
  // Booleans, strings and derived fields are first-class.
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'collider', op: 'eq', value: true }],
  })), [ALPHA, BETA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'material', op: 'eq', value: 'metal' }],
  })), [GAMMA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'lightIntensity', op: 'eq', value: null }],
  })), [ALPHA, BETA, DELTA]);
  // Predicates are AND-ed.
  assert.deepEqual(ids(queryScene(document, {
    where: [
      { field: 'kind', op: 'ne', value: 'light' },
      { field: 'x', op: 'gte', value: 0 },
    ],
  })), [BETA, DELTA]);
});

test('in matches any listed scalar and accepts an empty list', () => {
  const document = baseScene();
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'kind', op: 'in', value: ['wall', 'light'] }],
  })), [BETA, GAMMA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'y', op: 'in', value: [0.5, 3] }],
  })), [GAMMA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'kind', op: 'in', value: [] }],
  })), []);
});

test('contains is a case-insensitive substring test on string fields', () => {
  const document = baseScene();
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'name', op: 'contains', value: 'crate' }],
  })), [ALPHA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'name', op: 'contains', value: 'WALL' }],
  })), [BETA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'material', op: 'contains', value: 'met' }],
  })), [GAMMA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'name', op: 'contains', value: 'zzz' }],
  })), []);
  // Non-string values never match instead of throwing.
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'x', op: 'contains', value: '5' }],
  })), []);
});

test('within uses XZ distance only, includes the boundary and ignores y', () => {
  const center = { x: 0, y: 0, z: 0, radius: 5 };
  const document = makeScene([
    entity(ALPHA, 'Origin', 'prop', [0, 0, 0]),
    entity(BETA, 'Boundary', 'prop', [3, 0, 4]),
    entity(GAMMA, 'High above', 'prop', [0, 99, 0]),
    entity(DELTA, 'Just out', 'prop', [3, 0, 5]),
    entity(EPSILON, 'Far away', 'prop', [10, 0, 10]),
  ]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'x', op: 'within', value: center }],
  })), [ALPHA, BETA, GAMMA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'x', op: 'within', value: { ...center, radius: 4.99 } }],
  })), [ALPHA, GAMMA]);
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'x', op: 'within', value: { ...center, radius: 0 } }],
  })), [ALPHA, GAMMA]);
  // The predicate field is validated but `within` always measures the entity's
  // x/z position against the supplied centre.
  assert.deepEqual(ids(queryScene(document, {
    where: [{ field: 'name', op: 'within', value: center }],
  })), [ALPHA, BETA, GAMMA]);
});

test('orderBy is stable, direction-aware and defaults to input order', () => {
  const document = makeScene([
    entity(ALPHA, 'Two', 'prop', [2, 0, 0]),
    entity(BETA, 'Minus one', 'prop', [-1, 0, 0]),
    entity(GAMMA, 'Five', 'prop', [5, 0, 0]),
    entity(DELTA, 'Zero', 'prop', [0, 0, 0]),
    entity(EPSILON, 'Zero again', 'prop', [0, 0, 0]),
  ]);
  assert.deepEqual(ids(queryScene(document, {
    orderBy: { field: 'x' },
  })), [BETA, DELTA, EPSILON, ALPHA, GAMMA]);
  assert.deepEqual(ids(queryScene(document, {
    orderBy: { field: 'x', direction: 'desc' },
  })), [GAMMA, ALPHA, DELTA, EPSILON, BETA]);
  assert.deepEqual(ids(queryScene(document, {
    orderBy: { field: 'x', direction: 'asc' }, limit: 2,
  })), [BETA, DELTA]);
  assert.deepEqual(ids(queryScene(document, {
    orderBy: { field: 'x', direction: 'desc' }, limit: 2,
  })), [GAMMA, ALPHA]);
  assert.deepEqual(ids(queryScene(document, { limit: 0 })), []);
  assert.deepEqual(ids(queryScene(document, { limit: 99 })), [ALPHA, BETA, GAMMA, DELTA, EPSILON]);
});

test('aggregate supports count, sum, min and max over the matched rows', () => {
  const document = makeScene([
    entity(ALPHA, 'A', 'prop', [10, 1, 0]),
    entity(BETA, 'B', 'prop', [20, 2, 0]),
    entity(GAMMA, 'C', 'light', [30, 3, 0], [0.2, 0.2, 0.2], { light: LIGHT }),
  ]);
  assert.deepEqual(queryScene(document, { aggregate: 'count' }).aggregate, { value: 3 });
  assert.deepEqual(queryScene(document, { aggregate: 'sum:x' }).aggregate, { value: 60 });
  assert.deepEqual(queryScene(document, { aggregate: 'min:y' }).aggregate, { value: 1 });
  assert.deepEqual(queryScene(document, { aggregate: 'max:y' }).aggregate, { value: 3 });
  assert.deepEqual(queryScene(document, { aggregate: 'sum:lightIntensity' }).aggregate, { value: 6 });
  // `limit` truncates rows, not the aggregate.
  assert.deepEqual(queryScene(document, { limit: 1, aggregate: 'sum:x' }), {
    ok: true,
    rows: [document.entities[0]],
    count: 1,
    aggregate: { value: 60 },
    appliedQuery: {
      select: 'entity', where: [], orderBy: null, limit: 1, aggregate: 'sum:x',
    },
  });
  // Empty matches: count is 0, sum is 0, min/max are null.
  const empty = queryScene(document, {
    where: [{ field: 'kind', op: 'eq', value: 'door' }], aggregate: 'count',
  });
  assert.deepEqual(empty.aggregate, { value: 0 });
  assert.deepEqual(queryScene(document, {
    where: [{ field: 'kind', op: 'eq', value: 'door' }], aggregate: 'sum:x',
  }).aggregate, { value: 0 });
  assert.deepEqual(queryScene(document, {
    where: [{ field: 'kind', op: 'eq', value: 'door' }], aggregate: 'min:y',
  }).aggregate, { value: null });
});

test('select material groups by pattern with counts and uses only count aggregate', () => {
  const document = makeScene([
    entity(ALPHA, 'A', 'prop', [0, 0, 0], undefined, { material: PATTERN.brick }),
    entity(BETA, 'B', 'wall', [1, 0, 0], undefined, { material: PATTERN.metal }),
    entity(GAMMA, 'C', 'prop', [2, 0, 0], undefined, { material: PATTERN.metal }),
    entity(DELTA, 'D', 'prop', [3, 0, 0]),
  ]);
  const grouped = queryScene(document, { select: 'material' });
  assert.deepEqual(grouped.rows, [
    { material: 'brick', count: 1, entityIds: [ALPHA] },
    { material: 'metal', count: 2, entityIds: [BETA, GAMMA] },
  ]);
  assert.equal(grouped.count, 2);
  assert.deepEqual(Object.keys(grouped.rows[0]).sort(), ['count', 'entityIds', 'material']);
  assert.deepEqual(queryScene(document, { select: 'material' }).aggregate, null);
  assert.deepEqual(queryScene(document, {
    select: 'material', aggregate: 'count',
  }).aggregate, { value: 2 });
  assert.deepEqual(materials(queryScene(document, {
    select: 'material',
    orderBy: { field: 'count', direction: 'desc' },
  })), ['metal', 'brick']);
  assert.deepEqual(materials(queryScene(document, {
    select: 'material',
    where: [{ field: 'kind', op: 'eq', value: 'prop' }],
  })), ['brick', 'metal']);
  // Grouped order defaults to first appearance and `limit` only truncates rows.
  assert.deepEqual(materials(queryScene(document, {
    select: 'material', limit: 1, aggregate: 'count',
  })), ['brick']);
  queryError(() => queryScene(document, {
    select: 'material', orderBy: { field: 'x' },
  }), 'NG-E-QUERY-FIELD');
  queryError(() => queryScene(document, {
    select: 'material', aggregate: 'sum:x',
  }), 'NG-E-QUERY-SHAPE');
});

test('select light returns light entities only and keeps their typed fields', () => {
  const document = makeScene([
    entity(ALPHA, 'Crate', 'prop', [0, 0, 0]),
    entity(BETA, 'Lamp', 'light', [1, 3, 1], [0.2, 0.2, 0.2], { light: LIGHT }),
    entity(GAMMA, 'Dim lamp', 'light', [2, 3, 2], [0.2, 0.2, 0.2], {
      light: { color: [1, 1, 1], intensity: 2.5, range: 5 },
    }),
  ]);
  const lights = queryScene(document, {
    select: 'light', orderBy: { field: 'lightIntensity', direction: 'desc' },
  });
  assert.deepEqual(ids(lights), [BETA, GAMMA]);
  assert.equal(lights.rows[0].light.intensity, 6);
  assert.equal(lights.rows[0].material, undefined);
  assert.equal(queryScene(document, { select: 'light', aggregate: 'sum:lightIntensity' })
    .aggregate.value, 8.5);
  assert.deepEqual(ids(queryScene(document, {
    select: 'light', where: [{ field: 'lightIntensity', op: 'gte', value: 5 }],
  })), [BETA]);
  // `select: "light"` implicitly restricts the row source to light entities.
  assert.deepEqual(ids(queryScene(document, {
    select: 'light', where: [{ field: 'kind', op: 'eq', value: 'prop' }],
  })), []);
});

test('unknown fields are rejected with NG-E-QUERY-FIELD everywhere', () => {
  const document = baseScene();
  queryError(() => queryScene(document, {
    where: [{ field: 'colour', op: 'eq', value: 'red' }],
  }), 'NG-E-QUERY-FIELD');
  queryError(() => queryScene(document, {
    orderBy: { field: 'colour' },
  }), 'NG-E-QUERY-FIELD');
  assert.deepEqual(materials(queryScene(document, {
    select: 'material', orderBy: { field: 'count', direction: 'asc' },
  })), ['checker', 'brick', 'metal']);
  queryError(() => queryScene(document, {
    aggregate: 'sum:colour',
  }), 'NG-E-QUERY-FIELD');
});

test('unknown ops are rejected with NG-E-QUERY-OP', () => {
  const document = baseScene();
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'between', value: [0, 1] }],
  }), 'NG-E-QUERY-OP');
  queryError(() => queryScene(document, {
    where: [{ field: 'name', op: 'like', value: 'crate%' }],
  }), 'NG-E-QUERY-OP');
});

test('malformed query shapes are rejected with NG-E-QUERY-SHAPE', () => {
  const document = baseScene();
  for (const bad of [null, [], 'query', 7]) {
    queryError(() => queryScene(document, bad), 'NG-E-QUERY-SHAPE');
  }
  queryError(() => queryScene(document, { extra: true }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { select: 'mesh' }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { where: {} }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { where: [null] }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'eq', value: 1, extra: 2 }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'eq' }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'lt', value: '1' }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'in', value: 'wall' }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'in', value: [{}] }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'contains', value: 3 }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'within', value: { x: 0, y: 0, z: 0 } }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'within', value: { x: 0, y: 0, z: 0, radius: -1 } }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    where: [{ field: 'x', op: 'within', value: { x: 0, y: 0, z: 0, radius: 1, extra: 2 } }],
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { orderBy: 'x' }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    orderBy: { field: 'x', direction: 'sideways' },
  }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, {
    orderBy: { field: 'x', unexpected: 1 },
  }), 'NG-E-QUERY-SHAPE');
  for (const limit of [-1, 1.5, '3', null]) {
    queryError(() => queryScene(document, { limit }), 'NG-E-QUERY-SHAPE');
  }
  queryError(() => queryScene(document, { aggregate: 'average:x' }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { aggregate: 'sum:name' }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(document, { aggregate: 5 }), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene(null), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene({}), 'NG-E-QUERY-SHAPE');
  queryError(() => queryScene({ entities: 'none' }), 'NG-E-QUERY-SHAPE');
});

test('querying never mutates the document, the query or the returned rows', () => {
  const document = baseScene();
  const documentSnapshot = structuredClone(document);
  const query = {
    select: 'entity',
    where: [
      { field: 'kind', op: 'in', value: ['prop', 'light'] },
      { field: 'x', op: 'within', value: { x: 0, y: 0, z: 0, radius: 10 } },
    ],
    orderBy: { field: 'x', direction: 'desc' },
    limit: 3,
    aggregate: 'count',
  };
  const querySnapshot = structuredClone(query);
  const result = queryScene(document, query);
  assert.deepEqual(document, documentSnapshot);
  assert.deepEqual(query, querySnapshot);

  result.rows[0].name = 'Mutated';
  result.rows[0].position[0] = 999;
  result.rows[0].material.pattern = 'mutated';
  result.appliedQuery.where[0].value.push('wall');
  result.appliedQuery.orderBy.direction = 'asc';
  assert.deepEqual(document, documentSnapshot);
  assert.deepEqual(query, querySnapshot);

  // Frozen inputs prove the implementation only reads.
  assert.doesNotThrow(() => queryScene(
    deepFreeze(structuredClone(document)),
    deepFreeze(structuredClone(query)),
  ));
});

test('repeated queries are deterministic, including material grouping', () => {
  const document = baseScene();
  const query = {
    select: 'material', orderBy: { field: 'count', direction: 'desc' }, aggregate: 'count',
  };
  assert.deepEqual(queryScene(document, query), queryScene(document, query));
  assert.deepEqual(queryScene(document), queryScene(document));
});

test('courtyard: wall count and ids come from a real asset query', async () => {
  const document = await courtyard();
  const walls = queryScene(document, {
    where: [{ field: 'kind', op: 'eq', value: 'wall' }],
  });
  assert.equal(walls.count, 5);
  assert.deepEqual(ids(walls), [
    'a1e00000-0000-4000-8000-000000000011',
    'a1e00000-0000-4000-8000-000000000012',
    'a1e00000-0000-4000-8000-000000000013',
    'a1e00000-0000-4000-8000-000000000014',
    'a1e00000-0000-4000-8000-000000000015',
  ]);
  assert.deepEqual(walls.rows.map(row => row.name), [
    'West wall', 'East wall', 'South wall', 'Exit wall left', 'Exit wall right',
  ]);
});

test('courtyard: props near the player spawn are found by XZ distance', async () => {
  const document = await courtyard();
  const player = document.entities.find(item => item.kind === 'player');
  assert.deepEqual(player.position, [0, 0, 6]);
  const props = queryScene(document, {
    where: [
      { field: 'kind', op: 'eq', value: 'prop' },
      {
        field: 'x',
        op: 'within',
        value: { x: player.position[0], y: 0, z: player.position[2], radius: 6.5 },
      },
    ],
    orderBy: { field: 'z', direction: 'asc' },
  });
  assert.deepEqual(ids(props), [
    'a1e00000-0000-4000-8000-000000000030', // Crate, 5.66 m
    'a1e00000-0000-4000-8000-000000000034', // Low platform, 6.00 m
  ]);
  const distances = props.rows.map(row => Math.hypot(
    row.position[0] - player.position[0],
    row.position[2] - player.position[2],
  ));
  assert.deepEqual(distances.map(value => Number(value.toFixed(2))), [5.66, 6]);
  const column = document.entities.find(item => item.name === 'Column');
  const columnDistance = Math.hypot(
    column.position[0] - player.position[0],
    column.position[2] - player.position[2],
  );
  assert.ok(columnDistance > 6.5);
});

test('courtyard: material usage answers "used by more than one entity"', async () => {
  const document = await courtyard();
  const materials = queryScene(document, { select: 'material' });
  assert.deepEqual(materials.rows, [
    {
      material: 'brick',
      count: 8,
      entityIds: [
        'a1e00000-0000-4000-8000-000000000010',
        'a1e00000-0000-4000-8000-000000000011',
        'a1e00000-0000-4000-8000-000000000012',
        'a1e00000-0000-4000-8000-000000000013',
        'a1e00000-0000-4000-8000-000000000014',
        'a1e00000-0000-4000-8000-000000000015',
        'a1e00000-0000-4000-8000-000000000031',
        'a1e00000-0000-4000-8000-000000000033',
      ],
    },
    {
      material: 'metal',
      count: 6,
      entityIds: [
        'a1e00000-0000-4000-8000-000000000020',
        'a1e00000-0000-4000-8000-000000000040',
        'a1e00000-0000-4000-8000-000000000050',
        'a1e00000-0000-4000-8000-000000000060',
        'a1e00000-0000-4000-8000-000000000070',
        'a1e00000-0000-4000-8000-000000000071',
      ],
    },
    {
      material: 'checker',
      count: 3,
      entityIds: [
        'a1e00000-0000-4000-8000-000000000030',
        'a1e00000-0000-4000-8000-000000000032',
        'a1e00000-0000-4000-8000-000000000034',
      ],
    },
  ]);
  assert.ok(materials.rows.every(row => row.count > 1));
  assert.equal(materials.rows.reduce((total, row) => total + row.count, 0), 17);
  assert.deepEqual(queryScene(document, {
    select: 'material', where: [{ field: 'kind', op: 'eq', value: 'wall' }],
  }).rows, [{
    material: 'brick',
    count: 5,
    entityIds: [
      'a1e00000-0000-4000-8000-000000000011',
      'a1e00000-0000-4000-8000-000000000012',
      'a1e00000-0000-4000-8000-000000000013',
      'a1e00000-0000-4000-8000-000000000014',
      'a1e00000-0000-4000-8000-000000000015',
    ],
  }]);
});

test('courtyard: lights can be ordered and aggregated by intensity', async () => {
  const document = await courtyard();
  const lights = queryScene(document, {
    select: 'light',
    orderBy: { field: 'lightIntensity', direction: 'desc' },
  });
  assert.deepEqual(lights.rows.map(row => row.name), ['Courtyard lamp', 'Gate lamp']);
  assert.deepEqual(lights.rows.map(row => row.light.intensity), [5.5, 4.2]);
  const total = queryScene(document, { select: 'light', aggregate: 'sum:lightIntensity' });
  assert.ok(Math.abs(total.aggregate.value - 9.7) < 1e-9);
  assert.deepEqual(queryScene(document, {
    select: 'light', aggregate: 'count',
  }).aggregate, { value: 2 });
});

/**
 * World query language for the NativeGPU scene model (A07 / INN-08).
 *
 * `queryScene` answers structural questions such as "which walls are within
 * 4 m of the spawn?" or "which materials are used more than once?" without
 * touching disk, the renderer or the MCP surface. The module is intentionally
 * dependency-free (it imports nothing and performs no I/O), pure, and never
 * mutates the document or the query: every returned entity is deep-cloned.
 *
 * Schema conformance stays the job of `validateScene` in
 * `engine/scene/index.mjs`. Like `lint.mjs`, this module tolerates malformed
 * entities so a query never throws for data reasons: an unreadable field is
 * treated as `null` and simply fails value predicates.
 */

export const QUERY_SELECTS = Object.freeze(['entity', 'material', 'light']);

/** Flat entity projection used by `where`, `orderBy` and `aggregate`. */
export const QUERY_FIELDS = Object.freeze([
  'kind',
  'id',
  'name',
  'x',
  'y',
  'z',
  'scaleX',
  'scaleY',
  'scaleZ',
  'collider',
  'material',
  'lightIntensity',
]);

export const QUERY_OPS = Object.freeze([
  'eq', 'ne', 'lt', 'lte', 'gt', 'gte', 'in', 'contains', 'within',
]);

/** Stable error codes raised as `QueryError.code`. */
export const QUERY_ERRORS = Object.freeze({
  FIELD: 'NG-E-QUERY-FIELD',
  OP: 'NG-E-QUERY-OP',
  SHAPE: 'NG-E-QUERY-SHAPE',
});

const ERROR = QUERY_ERRORS;
const QUERY_KEYS = ['select', 'where', 'orderBy', 'limit', 'aggregate'];
const PREDICATE_KEYS = ['field', 'op', 'value'];
const WITHIN_KEYS = ['x', 'y', 'z', 'radius'];
const MATERIAL_ORDER_FIELDS = ['material', 'count'];
const ORDER_OPS = new Set(['lt', 'lte', 'gt', 'gte']);
const NUMERIC_FIELDS = new Set(['x', 'y', 'z', 'scaleX', 'scaleY', 'scaleZ', 'lightIntensity']);
const AGGREGATE = /^(sum|min|max):([A-Za-z][A-Za-z0-9]*)$/;

export class QueryError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'QueryError';
    this.code = code;
    this.details = details;
  }
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function fail(code, message, details = []) {
  throw new QueryError(code, message, details);
}

function scalar(value) {
  return value === null
    || typeof value === 'string'
    || typeof value === 'boolean'
    || (typeof value === 'number' && Number.isFinite(value));
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

/**
 * Reads one field of the flat entity projection. Missing or malformed source
 * values become `null`; the query language never guesses.
 */
function readField(entity, field) {
  const position = Array.isArray(entity.position) ? entity.position : [];
  const scale = Array.isArray(entity.scale) ? entity.scale : [];
  switch (field) {
    case 'kind':
      return typeof entity.kind === 'string' ? entity.kind : null;
    case 'id':
      return typeof entity.id === 'string' ? entity.id : null;
    case 'name':
      return typeof entity.name === 'string' ? entity.name : null;
    case 'x':
      return finite(position[0]);
    case 'y':
      return finite(position[1]);
    case 'z':
      return finite(position[2]);
    case 'scaleX':
      return finite(scale[0]);
    case 'scaleY':
      return finite(scale[1]);
    case 'scaleZ':
      return finite(scale[2]);
    case 'collider':
      return typeof entity.collider === 'boolean' ? entity.collider : null;
    case 'material':
      if (typeof entity.material === 'string') return entity.material;
      return isObject(entity.material) && typeof entity.material.pattern === 'string'
        ? entity.material.pattern : null;
    case 'lightIntensity':
      return isObject(entity.light) ? finite(entity.light.intensity) : null;
    default:
      return null;
  }
}

function normalizePredicateValue(value, op, index) {
  if (op === 'within') {
    const location = `where[${index}].value`;
    if (!isObject(value)) fail(ERROR.SHAPE, `${location} must be an object {x, y, z, radius}`);
    for (const key of Object.keys(value)) {
      if (!WITHIN_KEYS.includes(key)) fail(ERROR.SHAPE, `${location}.${key} is not supported`);
    }
    for (const key of WITHIN_KEYS) {
      if (!Number.isFinite(value[key])) fail(ERROR.SHAPE, `${location}.${key} must be a finite number`);
    }
    if (value.radius < 0) fail(ERROR.SHAPE, `${location}.radius must be zero or greater`);
    return { x: value.x, y: value.y, z: value.z, radius: value.radius };
  }
  if (op === 'in') {
    if (!Array.isArray(value)) fail(ERROR.SHAPE, `where[${index}].value must be an array for op "in"`);
    if (!value.every(scalar)) {
      fail(ERROR.SHAPE, `where[${index}].value must contain only strings, numbers, booleans or null`);
    }
    return [...value];
  }
  if (op === 'contains') {
    if (typeof value !== 'string') fail(ERROR.SHAPE, `where[${index}].value must be a string for op "contains"`);
    return value;
  }
  if (ORDER_OPS.has(op)) {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      fail(ERROR.SHAPE, `where[${index}].value must be a finite number for op "${op}"`);
    }
    return value;
  }
  // eq / ne
  if (!scalar(value)) {
    fail(ERROR.SHAPE, `where[${index}].value must be a string, number, boolean or null for op "${op}"`);
  }
  return value;
}

function normalizePredicate(predicate, index) {
  if (!isObject(predicate)) fail(ERROR.SHAPE, `where[${index}] must be an object`);
  for (const key of Object.keys(predicate)) {
    if (!PREDICATE_KEYS.includes(key)) fail(ERROR.SHAPE, `where[${index}].${key} is not supported`);
  }
  const { field, op } = predicate;
  if (typeof field !== 'string') fail(ERROR.SHAPE, `where[${index}].field must be a string`);
  if (!QUERY_FIELDS.includes(field)) {
    fail(ERROR.FIELD, `Unknown query field: ${field}`, [{ index, field, allowed: [...QUERY_FIELDS] }]);
  }
  if (typeof op !== 'string') fail(ERROR.SHAPE, `where[${index}].op must be a string`);
  if (!QUERY_OPS.includes(op)) {
    fail(ERROR.OP, `Unknown query op: ${op}`, [{ index, op, allowed: [...QUERY_OPS] }]);
  }
  if (!Object.hasOwn(predicate, 'value')) fail(ERROR.SHAPE, `where[${index}].value is required`);
  return {
    field,
    op,
    value: normalizePredicateValue(predicate.value, op, index),
  };
}

function normalizeWhere(where) {
  if (where === undefined) return [];
  if (!Array.isArray(where)) fail(ERROR.SHAPE, 'query.where must be an array');
  return where.map((predicate, index) => normalizePredicate(predicate, index));
}

function normalizeOrderBy(orderBy, select) {
  if (orderBy === undefined) return null;
  if (!isObject(orderBy)) fail(ERROR.SHAPE, 'query.orderBy must be an object {field, direction}');
  for (const key of Object.keys(orderBy)) {
    if (key !== 'field' && key !== 'direction') {
      fail(ERROR.SHAPE, `query.orderBy.${key} is not supported`);
    }
  }
  const { field } = orderBy;
  if (typeof field !== 'string') fail(ERROR.SHAPE, 'query.orderBy.field must be a string');
  const allowed = select === 'material' ? MATERIAL_ORDER_FIELDS : QUERY_FIELDS;
  if (!allowed.includes(field)) {
    fail(ERROR.FIELD, `Unknown orderBy field for select "${select}": ${field}`, [
      { field, select, allowed: [...allowed] },
    ]);
  }
  const direction = orderBy.direction === undefined ? 'asc' : orderBy.direction;
  if (direction !== 'asc' && direction !== 'desc') {
    fail(ERROR.SHAPE, 'query.orderBy.direction must be "asc" or "desc"');
  }
  return { field, direction };
}

function normalizeLimit(limit) {
  if (limit === undefined) return null;
  if (!Number.isSafeInteger(limit) || limit < 0) {
    fail(ERROR.SHAPE, 'query.limit must be a non-negative safe integer');
  }
  return limit;
}

function normalizeAggregate(aggregate, select) {
  if (aggregate === undefined) return null;
  if (typeof aggregate !== 'string') fail(ERROR.SHAPE, 'query.aggregate must be a string');
  if (aggregate === 'count') return 'count';
  const match = AGGREGATE.exec(aggregate);
  if (!match) {
    fail(ERROR.SHAPE, 'query.aggregate must be "count", "sum:<field>", "min:<field>" or "max:<field>"');
  }
  const [, fn, field] = match;
  if (!QUERY_FIELDS.includes(field)) {
    fail(ERROR.FIELD, `Unknown aggregate field: ${field}`, [
      { aggregate, field, allowed: [...QUERY_FIELDS] },
    ]);
  }
  if (select === 'material') {
    fail(ERROR.SHAPE, `query.aggregate "${fn}" is not supported for select "material"; only count is available`);
  }
  if (!NUMERIC_FIELDS.has(field)) {
    fail(ERROR.SHAPE, `query.aggregate "${fn}" requires a numeric field, got ${field}`);
  }
  return `${fn}:${field}`;
}

function normalizeQuery(query) {
  if (!isObject(query)) fail(ERROR.SHAPE, 'query must be an object');
  for (const key of Object.keys(query)) {
    if (!QUERY_KEYS.includes(key)) fail(ERROR.SHAPE, `query.${key} is not supported`);
  }
  const select = query.select === undefined ? 'entity' : query.select;
  if (!QUERY_SELECTS.includes(select)) {
    fail(ERROR.SHAPE, `query.select must be one of: ${QUERY_SELECTS.join(', ')}`);
  }
  return {
    select,
    where: normalizeWhere(query.where),
    orderBy: normalizeOrderBy(query.orderBy, select),
    limit: normalizeLimit(query.limit),
    aggregate: normalizeAggregate(query.aggregate, select),
  };
}

function matches(entity, predicate) {
  const value = readField(entity, predicate.field);
  switch (predicate.op) {
    case 'eq':
      return value === predicate.value;
    case 'ne':
      return value !== predicate.value;
    case 'lt':
      return typeof value === 'number' && value < predicate.value;
    case 'lte':
      return typeof value === 'number' && value <= predicate.value;
    case 'gt':
      return typeof value === 'number' && value > predicate.value;
    case 'gte':
      return typeof value === 'number' && value >= predicate.value;
    case 'in':
      return predicate.value.includes(value);
    case 'contains':
      return typeof value === 'string'
        && value.toLowerCase().includes(predicate.value.toLowerCase());
    case 'within': {
      const x = readField(entity, 'x');
      const z = readField(entity, 'z');
      if (x === null || z === null) return false;
      return Math.hypot(x - predicate.value.x, z - predicate.value.z) <= predicate.value.radius;
    }
    default:
      return false;
  }
}

function groupMaterials(entities) {
  const groups = new Map();
  for (const entity of entities) {
    const material = readField(entity, 'material');
    if (material === null) continue;
    let group = groups.get(material);
    if (!group) {
      group = { material, count: 0, entityIds: [] };
      groups.set(material, group);
    }
    group.count += 1;
    if (typeof entity.id === 'string') group.entityIds.push(entity.id);
  }
  return [...groups.values()];
}

function projectRows(entities, select) {
  if (select === 'material') return groupMaterials(entities);
  if (select === 'light') {
    return entities
      .filter(entity => entity.kind === 'light')
      .map(entity => structuredClone(entity));
  }
  return entities.map(entity => structuredClone(entity));
}

function rowValue(row, field, select) {
  if (select === 'material') return Object.hasOwn(row, field) ? row[field] : null;
  return readField(row, field);
}

function typeRank(value) {
  if (typeof value === 'number') return 0;
  if (typeof value === 'string') return 1;
  if (typeof value === 'boolean') return 2;
  return 3;
}

function compareAscending(left, right) {
  const leftRank = typeRank(left);
  const rightRank = typeRank(right);
  if (leftRank !== rightRank) return leftRank - rightRank;
  if (typeof left === 'number') return left - right;
  if (typeof left === 'string') return left < right ? -1 : left > right ? 1 : 0;
  if (typeof left === 'boolean') return left === right ? 0 : left ? 1 : -1;
  return 0;
}

function orderRows(rows, orderBy, select) {
  const direction = orderBy.direction === 'desc' ? -1 : 1;
  return rows
    .map((row, index) => ({ row, index }))
    .sort((left, right) => {
      const a = rowValue(left.row, orderBy.field, select);
      const b = rowValue(right.row, orderBy.field, select);
      let compared;
      // Null/missing values always sort last, in both directions.
      if (a === null && b === null) compared = 0;
      else if (a === null) compared = 1;
      else if (b === null) compared = -1;
      else compared = compareAscending(a, b) * direction;
      // Ties keep input (or first-appearance) order in both directions.
      return compared !== 0 ? compared : left.index - right.index;
    })
    .map(entry => entry.row);
}

function aggregateRows(rows, aggregate, select) {
  if (aggregate === 'count') return { value: rows.length };
  const [fn, field] = aggregate.split(':');
  const values = [];
  for (const row of rows) {
    const value = rowValue(row, field, select);
    if (typeof value === 'number' && Number.isFinite(value)) values.push(value);
  }
  if (fn === 'sum') return { value: values.reduce((total, value) => total + value, 0) };
  if (values.length === 0) return { value: null };
  let value = values[0];
  for (let index = 1; index < values.length; index += 1) {
    const candidate = values[index];
    if (fn === 'min' ? candidate < value : candidate > value) value = candidate;
  }
  return { value };
}

/**
 * Runs a declarative query against a parsed scene document.
 *
 * @param {object} document Parsed scene JSON (`{ entities: [...] }`).
 * @param {object} [query] Query object; see `.developmentAgent/core/SCENE-QUERY.md`.
 * @returns {{ ok: true, rows: object[], count: number,
 *   aggregate: { value: number|null }|null, appliedQuery: object }}
 * @throws {QueryError} `NG-E-QUERY-FIELD`, `NG-E-QUERY-OP` or `NG-E-QUERY-SHAPE`.
 */
export function queryScene(document, query = {}) {
  const appliedQuery = normalizeQuery(query);
  if (!isObject(document) || !Array.isArray(document.entities)) {
    fail(ERROR.SHAPE, 'document.entities must be an array');
  }
  const matched = document.entities
    .filter(isObject)
    .filter(entity => appliedQuery.where.every(predicate => matches(entity, predicate)));
  const rows = projectRows(matched, appliedQuery.select);
  // Order before aggregating so `min`/`max` ties and material first-appearance
  // ordering are deterministic; `limit` only truncates the returned rows and
  // therefore does not change `aggregate`.
  const ordered = appliedQuery.orderBy === null
    ? rows : orderRows(rows, appliedQuery.orderBy, appliedQuery.select);
  const aggregate = appliedQuery.aggregate === null
    ? null : aggregateRows(ordered, appliedQuery.aggregate, appliedQuery.select);
  const limited = appliedQuery.limit === null ? ordered : ordered.slice(0, appliedQuery.limit);
  return {
    ok: true,
    rows: limited,
    count: limited.length,
    aggregate,
    appliedQuery: structuredClone(appliedQuery),
  };
}

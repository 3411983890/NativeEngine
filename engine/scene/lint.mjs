/**
 * Design-time lint for the NativeGPU scene model (S3E-07 / INN-07).
 *
 * This module is intentionally dependency-free (it imports nothing and performs
 * no I/O) and every rule is a pure function of the document, so the whole rule
 * engine can be unit-tested without touching disk. Schema conformance stays the
 * job of `validateScene` in `engine/scene/index.mjs`; lint deliberately accepts
 * malformed documents so it can report problems the validator cannot express.
 */

export const ENTITY_KINDS = Object.freeze([
  'ground', 'wall', 'prop', 'player', 'key', 'door', 'goal', 'light',
]);

export const LINT_RULES = Object.freeze([
  { code: 'NG-LINT-NO-PLAYER', severity: 'error', summary: 'Scene has no player spawn' },
  { code: 'NG-LINT-NO-GOAL', severity: 'error', summary: 'Scene has no goal' },
  { code: 'NG-LINT-MULTI-PLAYER', severity: 'error', summary: 'Scene has more than one player spawn' },
  { code: 'NG-LINT-DUP-ID', severity: 'error', summary: 'Two entities share the same id' },
  { code: 'NG-LINT-BAD-KIND', severity: 'error', summary: 'Entity kind is not one of the eight supported kinds' },
  { code: 'NG-LINT-OUT-OF-WORLD', severity: 'error', summary: 'Entity is outside the play area or below the ground' },
  { code: 'NG-LINT-OVERLAP', severity: 'error', summary: 'Two solid colliders intersect beyond the tolerance' },
  { code: 'NG-LINT-KEY-UNREACHABLE', severity: 'error', summary: 'Key cannot be reached by walking from the spawn' },
  { code: 'NG-LINT-GOAL-UNREACHABLE', severity: 'error', summary: 'Goal cannot be reached by walking from the spawn' },
  { code: 'NG-LINT-DOOR-WITHOUT-KEY', severity: 'error', summary: 'Door exists but the scene contains no key' },
  { code: 'NG-LINT-KEY-AFTER-DOOR', severity: 'warning', summary: 'Nearest key is farther from spawn than the door' },
  { code: 'NG-LINT-ENTITY-BUDGET', severity: 'warning', summary: 'Entity count exceeds the configured budget' },
]);

const SEVERITY = new Map(LINT_RULES.map(rule => [rule.code, rule.severity]));

/** Defaults are part of the public contract; tests and docs pin them. */
export const DEFAULT_OPTIONS = Object.freeze({
  maxEntities: 200,
  overlapTolerance: 0.2,
  worldExtent: 10,
  cellSize: 0.5,
  playerRadius: null,
});

const EPSILON = 1e-6;
const MAX_GRID_CELLS_PER_AXIS = 512;

// Kinds whose collider is static level geometry. The player is a spawn marker,
// and the ground is the floor everything rests on, so neither participates in
// the overlap rule (their contact with props is expected).
const OVERLAP_EXCLUDED_KINDS = new Set(['player', 'ground']);

// Kinds that block walking. Doors are passable on purpose: a door is opened by
// its key and is handled by the door rules instead of making every gate look
// like an unreachable region.
const BLOCKING_KINDS = new Set(['wall', 'prop']);

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function vector3(value) {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite)
    ? value : null;
}

function positiveNumber(value, fallback, minimum, maximum) {
  return Number.isFinite(value) && value >= minimum && value <= maximum ? value : fallback;
}

function round2(value) {
  return Math.round(value * 100) / 100;
}

function issue(code, entityId, message, detail = {}) {
  const severity = SEVERITY.get(code);
  if (!severity) throw new Error(`Unknown lint rule: ${code}`);
  return {
    code,
    severity,
    entityId: typeof entityId === 'string' ? entityId : null,
    message,
    detail,
  };
}

function label(entity, index) {
  if (typeof entity.name === 'string' && entity.name.length > 0) return `"${entity.name}"`;
  if (typeof entity.id === 'string' && entity.id.length > 0) return entity.id;
  return `entities[${index}]`;
}

function normalizeOptions(options) {
  const source = isObject(options) ? options : {};
  return {
    maxEntities: Number.isSafeInteger(source.maxEntities) && source.maxEntities >= 1
      ? source.maxEntities : DEFAULT_OPTIONS.maxEntities,
    overlapTolerance: positiveNumber(source.overlapTolerance, DEFAULT_OPTIONS.overlapTolerance, 0, 1000),
    worldExtent: positiveNumber(source.worldExtent, DEFAULT_OPTIONS.worldExtent, 1, 1000),
    cellSize: positiveNumber(source.cellSize, DEFAULT_OPTIONS.cellSize, 0.05, 5),
    playerRadius: positiveNumber(source.playerRadius, null, 0, 10),
  };
}

function boxOf(entity) {
  const position = vector3(entity.position);
  const scale = vector3(entity.scale);
  if (!position || !scale) return null;
  const half = [Math.abs(scale[0]) / 2, Math.abs(scale[1]) / 2, Math.abs(scale[2]) / 2];
  return {
    min: [position[0] - half[0], position[1] - half[1], position[2] - half[2]],
    max: [position[0] + half[0], position[1] + half[1], position[2] + half[2]],
  };
}

function floorLevel(entities) {
  let floor = 0;
  for (const entity of entities) {
    if (entity.kind !== 'ground') continue;
    const position = vector3(entity.position);
    const scale = vector3(entity.scale);
    if (!position || !scale) continue;
    floor = Math.max(floor, position[1] + Math.abs(scale[1]) / 2);
  }
  return floor;
}

function distanceXZ(a, b) {
  return Math.hypot(a[0] - b[0], a[2] - b[2]);
}

function playerRadiusOf(player, configured) {
  if (configured !== null) return configured;
  const scale = player ? vector3(player.scale) : null;
  return scale ? Math.max(Math.abs(scale[0]), Math.abs(scale[2])) / 2 : 0.3;
}

function overlapFindings(entities, tolerance) {
  const findings = [];
  const boxes = [];
  for (const [index, entity] of entities.entries()) {
    if (entity.collider !== true || OVERLAP_EXCLUDED_KINDS.has(entity.kind)) continue;
    const box = boxOf(entity);
    if (!box) continue;
    boxes.push({ index, entity, box });
  }
  // Sweep-and-prune on X keeps the rule practical for the validator's 10k
  // entity ceiling while still examining every potentially intersecting pair.
  boxes.sort((left, right) => left.box.min[0] - right.box.min[0] || left.index - right.index);
  const pairs = [];
  for (let a = 0; a < boxes.length; a += 1) {
    const first = boxes[a];
    for (let b = a + 1; b < boxes.length; b += 1) {
      const second = boxes[b];
      if (second.box.min[0] >= first.box.max[0] - tolerance) break;
      const penetrationX = Math.min(first.box.max[0], second.box.max[0])
        - Math.max(first.box.min[0], second.box.min[0]);
      if (penetrationX <= tolerance) continue;
      const penetrationY = Math.min(first.box.max[1], second.box.max[1])
        - Math.max(first.box.min[1], second.box.min[1]);
      if (penetrationY <= tolerance) continue;
      const penetrationZ = Math.min(first.box.max[2], second.box.max[2])
        - Math.max(first.box.min[2], second.box.min[2]);
      if (penetrationZ <= tolerance) continue;
      pairs.push({ first, second, penetration: [penetrationX, penetrationY, penetrationZ] });
    }
  }
  pairs.sort((left, right) => left.first.index - right.first.index
    || left.second.index - right.second.index);
  for (const pair of pairs) {
    const { first, second, penetration } = pair;
    findings.push(issue(
      'NG-LINT-OVERLAP',
      second.entity.id,
      `${label(first.entity, first.index)} overlaps ${label(second.entity, second.index)} `
        + `by ${penetration.map(value => round2(value)).join(' x ')} world units`,
      {
        entities: [first.entity.id ?? null, second.entity.id ?? null],
        names: [first.entity.name ?? null, second.entity.name ?? null],
        penetration: penetration.map(round2),
      },
    ));
  }
  return findings;
}

function buildGrid(entities, { worldExtent, cellSize }, playerRadius) {
  const cellsPerAxis = Math.max(2, Math.min(
    MAX_GRID_CELLS_PER_AXIS,
    Math.ceil((2 * worldExtent) / cellSize),
  ));
  const cell = (2 * worldExtent) / cellsPerAxis;
  const blocked = new Uint8Array(cellsPerAxis * cellsPerAxis);
  for (const entity of entities) {
    if (entity.collider !== true || !BLOCKING_KINDS.has(entity.kind)) continue;
    const box = boxOf(entity);
    if (!box) continue;
    const minX = box.min[0] - playerRadius;
    const maxX = box.max[0] + playerRadius;
    const minZ = box.min[2] - playerRadius;
    const maxZ = box.max[2] + playerRadius;
    // Integer range of cell centres inside the inflated AABB.
    const startI = Math.max(0, Math.ceil((minX + worldExtent) / cell - 0.5 - 1e-9));
    const endI = Math.min(cellsPerAxis - 1, Math.floor((maxX + worldExtent) / cell - 0.5 + 1e-9));
    const startJ = Math.max(0, Math.ceil((minZ + worldExtent) / cell - 0.5 - 1e-9));
    const endJ = Math.min(cellsPerAxis - 1, Math.floor((maxZ + worldExtent) / cell - 0.5 + 1e-9));
    for (let j = startJ; j <= endJ; j += 1) {
      for (let i = startI; i <= endI; i += 1) blocked[j * cellsPerAxis + i] = 1;
    }
  }
  return { worldExtent, cell, cellsPerAxis, blocked };
}

function cellIndex(grid, position) {
  const n = grid.cellsPerAxis;
  const i = Math.min(n - 1, Math.max(0, Math.floor((position[0] + grid.worldExtent) / grid.cell)));
  const j = Math.min(n - 1, Math.max(0, Math.floor((position[2] + grid.worldExtent) / grid.cell)));
  return j * n + i;
}

function nearestFreeCell(grid, start, maxRing = 4) {
  if (start < 0) return -1;
  if (grid.blocked[start] === 0) return start;
  const n = grid.cellsPerAxis;
  const startI = start % n;
  const startJ = (start - startI) / n;
  for (let ring = 1; ring <= maxRing; ring += 1) {
    for (let dj = -ring; dj <= ring; dj += 1) {
      for (let di = -ring; di <= ring; di += 1) {
        if (Math.max(Math.abs(di), Math.abs(dj)) !== ring) continue;
        const i = startI + di;
        const j = startJ + dj;
        if (i < 0 || j < 0 || i >= n || j >= n) continue;
        const index = j * n + i;
        if (grid.blocked[index] === 0) return index;
      }
    }
  }
  return -1;
}

function flood(grid, start) {
  const n = grid.cellsPerAxis;
  const visited = new Uint8Array(n * n);
  if (start < 0) return { visited, count: 0 };
  const queue = new Int32Array(n * n);
  let head = 0;
  let tail = 0;
  queue[tail] = start;
  tail += 1;
  visited[start] = 1;
  while (head < tail) {
    const index = queue[head];
    head += 1;
    const i = index % n;
    const j = (index - i) / n;
    if (i > 0) {
      const next = index - 1;
      if (visited[next] === 0 && grid.blocked[next] === 0) { visited[next] = 1; queue[tail] = next; tail += 1; }
    }
    if (i + 1 < n) {
      const next = index + 1;
      if (visited[next] === 0 && grid.blocked[next] === 0) { visited[next] = 1; queue[tail] = next; tail += 1; }
    }
    if (j > 0) {
      const next = index - n;
      if (visited[next] === 0 && grid.blocked[next] === 0) { visited[next] = 1; queue[tail] = next; tail += 1; }
    }
    if (j + 1 < n) {
      const next = index + n;
      if (visited[next] === 0 && grid.blocked[next] === 0) { visited[next] = 1; queue[tail] = next; tail += 1; }
    }
  }
  return { visited, count: tail };
}

/**
 * Runs a coarse 2D BFS over the XZ plane from the player spawn. Obstacles are
 * solid wall/prop colliders inflated by the player radius; the ground is the
 * walkable plane and doors are treated as passable.
 */
function reachability(entities, config, player) {
  const playerPosition = player ? vector3(player.position) : null;
  if (!player || !playerPosition) return null;
  const playerRadius = playerRadiusOf(player, config.playerRadius);
  const grid = buildGrid(entities, config, playerRadius);
  const freeCells = grid.blocked.reduce((total, blocked) => total + (blocked === 0 ? 1 : 0), 0);
  const start = nearestFreeCell(grid, cellIndex(grid, playerPosition));
  const { visited, count } = flood(grid, start);
  const reachablePercent = freeCells === 0 ? 0 : round2((count / freeCells) * 100);
  return {
    grid,
    start,
    visited,
    freeCells,
    reachableCells: count,
    reachablePercent,
    playerRadius,
  };
}

function reachabilityDetail(report, reason) {
  return {
    reason,
    reachablePercent: report.reachablePercent,
    reachableAreaPercent: report.reachablePercent,
    reachableCells: report.reachableCells,
    freeCells: report.freeCells,
    grid: {
      worldExtent: report.grid.worldExtent,
      cellSize: round2(report.grid.cell),
      cellsPerAxis: report.grid.cellsPerAxis,
      playerRadius: report.playerRadius,
    },
  };
}

function unreachableFindings(entities, report, kind, code) {
  if (!report) return [];
  const findings = [];
  for (const [index, entity] of entities.entries()) {
    if (entity.kind !== kind) continue;
    const position = vector3(entity.position);
    if (!position) continue;
    const { grid } = report;
    const outside = Math.abs(position[0]) > grid.worldExtent + EPSILON
      || Math.abs(position[2]) > grid.worldExtent + EPSILON;
    let reason = null;
    if (report.start < 0) {
      reason = 'player-spawn-blocked';
    } else if (outside) {
      reason = 'outside-world';
    } else {
      const targetIndex = cellIndex(grid, position);
      if (grid.blocked[targetIndex] === 1) reason = 'blocked-cell';
      else if (report.visited[targetIndex] === 0) reason = 'separated';
    }
    if (!reason) continue;
    const noun = kind === 'key' ? 'Key' : 'Goal';
    findings.push(issue(
      code,
      entity.id,
      `${noun} ${label(entity, index)} is unreachable from the player spawn `
        + `(reachable area ${report.reachablePercent.toFixed(2)}% of the walkable grid)`,
      reachabilityDetail(report, reason),
    ));
  }
  return findings;
}

function doorFindings(entities, player) {
  const findings = [];
  const doors = [];
  const keys = [];
  for (const [index, entity] of entities.entries()) {
    if (entity.kind === 'door') doors.push({ entity, index });
    else if (entity.kind === 'key') keys.push({ entity, index, position: vector3(entity.position) });
  }
  if (doors.length === 0) return findings;
  if (keys.length === 0) {
    for (const door of doors) {
      findings.push(issue(
        'NG-LINT-DOOR-WITHOUT-KEY',
        door.entity.id,
        `Door ${label(door.entity, door.index)} can never open because the scene has no key`,
        { doorId: door.entity.id ?? null, keyCount: 0 },
      ));
    }
    return findings;
  }
  const spawn = player ? vector3(player.position) : null;
  if (!spawn) return findings;
  const placedKeys = keys.filter(entry => entry.position !== null);
  if (placedKeys.length === 0) return findings;
  let nearest = placedKeys[0];
  let nearestDistance = distanceXZ(spawn, nearest.position);
  for (const key of placedKeys.slice(1)) {
    const distance = distanceXZ(spawn, key.position);
    if (distance < nearestDistance) {
      nearest = key;
      nearestDistance = distance;
    }
  }
  for (const door of doors) {
    const doorPosition = vector3(door.entity.position);
    if (!doorPosition) continue;
    const doorDistance = distanceXZ(spawn, doorPosition);
    if (nearestDistance > doorDistance + EPSILON) {
      findings.push(issue(
        'NG-LINT-KEY-AFTER-DOOR',
        door.entity.id,
        `Door ${label(door.entity, door.index)} is ${round2(doorDistance)} away from spawn `
          + `but the nearest key ${label(nearest.entity, nearest.index)} `
          + `is farther (${round2(nearestDistance)}); the key is likely behind the door`,
        {
          doorId: door.entity.id ?? null,
          doorDistance: round2(doorDistance),
          keyId: nearest.entity.id ?? null,
          keyDistance: round2(nearestDistance),
        },
      ));
    }
  }
  return findings;
}

/**
 * Lints a scene document.
 *
 * @param {unknown} document Parsed scene JSON. Malformed documents never throw.
 * @param {object} [options]
 * @param {number} [options.maxEntities=200] Entity budget threshold.
 * @param {number} [options.overlapTolerance=0.2] Minimum penetration on every axis.
 * @param {number} [options.worldExtent=10] Half-size of the square play area.
 * @param {number} [options.cellSize=0.5] Reachability grid resolution in world units.
 * @param {number|null} [options.playerRadius=null] Overrides the radius derived from player scale.
 * @returns {{ ok: boolean, findings: Array<{
 *   code: string, severity: 'error'|'warning', entityId: string|null,
 *   message: string, detail: object,
 * }> }}
 */
export function lintScene(document, options = {}) {
  const source = isObject(document) ? document : {};
  const rawEntities = Array.isArray(source.entities) ? source.entities : [];
  const entities = rawEntities.filter(isObject);
  const config = normalizeOptions(options);
  const findings = [];

  const players = entities.filter(entity => entity.kind === 'player');
  const goals = entities.filter(entity => entity.kind === 'goal');

  if (players.length === 0) {
    findings.push(issue(
      'NG-LINT-NO-PLAYER',
      null,
      'Scene has no player spawn; exactly one is required',
      { entityCount: rawEntities.length },
    ));
  }
  if (goals.length === 0) {
    findings.push(issue(
      'NG-LINT-NO-GOAL',
      null,
      'Scene has no goal; the level cannot be completed',
      { entityCount: rawEntities.length },
    ));
  }
  if (players.length > 1) {
    const playerIds = players.map(entity => entity.id ?? null);
    for (const [offset, player] of players.entries()) {
      if (offset === 0) continue;
      findings.push(issue(
        'NG-LINT-MULTI-PLAYER',
        player.id,
        `Scene has ${players.length} player spawns; exactly one is allowed`,
        { count: players.length, playerIds },
      ));
    }
  }

  const seenIds = new Map();
  for (const [index, entity] of entities.entries()) {
    if (typeof entity.id !== 'string') continue;
    const key = entity.id.toLowerCase();
    if (seenIds.has(key)) {
      findings.push(issue(
        'NG-LINT-DUP-ID',
        entity.id,
        `Duplicate entity id "${entity.id}" (first used by entities[${seenIds.get(key)}])`,
        { index, firstIndex: seenIds.get(key), id: entity.id },
      ));
    } else {
      seenIds.set(key, index);
    }
  }

  for (const [index, entity] of entities.entries()) {
    if (!ENTITY_KINDS.includes(entity.kind)) {
      findings.push(issue(
        'NG-LINT-BAD-KIND',
        entity.id,
        `Entity ${label(entity, index)} has unsupported kind ${JSON.stringify(entity.kind ?? null)}`,
        { index, kind: typeof entity.kind === 'string' ? entity.kind : null },
      ));
    }
  }

  const floor = floorLevel(entities);
  for (const [index, entity] of entities.entries()) {
    if (entity.kind === 'ground') continue;
    const position = vector3(entity.position);
    if (!position) continue;
    const axes = [];
    if (Math.abs(position[0]) > config.worldExtent + EPSILON) axes.push('x');
    if (Math.abs(position[2]) > config.worldExtent + EPSILON) axes.push('z');
    if (position[1] < floor - EPSILON) axes.push('y');
    if (axes.length === 0) continue;
    findings.push(issue(
      'NG-LINT-OUT-OF-WORLD',
      entity.id,
      `Entity ${label(entity, index)} is outside the play area `
        + `(position [${position.map(value => round2(value)).join(', ')}], `
        + `world ±${config.worldExtent}, floor ${round2(floor)})`,
      {
        index,
        position: position.map(round2),
        axes,
        worldExtent: config.worldExtent,
        floorY: round2(floor),
      },
    ));
  }

  findings.push(...overlapFindings(entities, config.overlapTolerance));

  const report = reachability(entities, config, players[0]);
  findings.push(...unreachableFindings(entities, report, 'key', 'NG-LINT-KEY-UNREACHABLE'));
  findings.push(...unreachableFindings(entities, report, 'goal', 'NG-LINT-GOAL-UNREACHABLE'));
  findings.push(...doorFindings(entities, players[0]));

  if (rawEntities.length > config.maxEntities) {
    findings.push(issue(
      'NG-LINT-ENTITY-BUDGET',
      null,
      `Scene has ${rawEntities.length} entities, above the budget of ${config.maxEntities}`,
      { count: rawEntities.length, maxEntities: config.maxEntities },
    ));
  }

  return {
    ok: findings.every(finding => finding.severity !== 'error'),
    findings,
  };
}

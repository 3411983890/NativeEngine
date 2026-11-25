// Offline Voronoi bake tests: determinism, volume conservation, convexity and
// adjacency symmetry. These run without a GPU or the native host.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { bakeVoronoi, validatePackage, SCHEMA } from '../tools/offline/voronoi.mjs';

const WALL = { half: [1.5, 1.0, 0.15], cells: 24, seed: 7 };

test('bake is byte-identical for the same seed', () => {
  const a = JSON.stringify(bakeVoronoi(WALL));
  const b = JSON.stringify(bakeVoronoi(WALL));
  assert.equal(a, b);
  const c = JSON.stringify(bakeVoronoi({ ...WALL, seed: 8 }));
  assert.notEqual(a, c);
});

test('cells conserve the box volume', () => {
  const pkg = bakeVoronoi(WALL);
  assert.equal(pkg.schema, SCHEMA);
  assert.equal(pkg.cells.length, 24);
  assert.ok(pkg.bonds.length > 24);
  assert.deepEqual(validatePackage(pkg), []);
  const boxVolume = 8 * WALL.half[0] * WALL.half[1] * WALL.half[2];
  assert.ok(Math.abs(pkg.totals.volume - boxVolume) < boxVolume * 0.005);
  for (const cell of pkg.cells) assert.ok(cell.volume > 0);
});

test('every cell is convex and inside the box', () => {
  const pkg = bakeVoronoi(WALL);
  for (const cell of pkg.cells) {
    for (const face of cell.faces) {
      for (const index of face.indices) {
        const v = cell.vertices[index];
        const d = face.normal[0] * v[0] + face.normal[1] * v[1] + face.normal[2] * v[2];
        assert.ok(d <= face.offset + 1e-4, `cell ${cell.index} vertex in front of a face`);
      }
    }
    for (const v of cell.vertices) {
      for (let axis = 0; axis < 3; ++axis) {
        assert.ok(Math.abs(v[axis]) <= WALL.half[axis] + 1e-4);
      }
    }
  }
});

test('bonds are symmetric, ordered and have area', () => {
  const pkg = bakeVoronoi(WALL);
  const seen = new Set();
  for (const bond of pkg.bonds) {
    assert.ok(bond.a < bond.b);
    assert.ok(bond.area > 0);
    const key = `${bond.a}:${bond.b}`;
    assert.ok(!seen.has(key));
    seen.add(key);
    const cellA = pkg.cells[bond.a];
    const cellB = pkg.cells[bond.b];
    assert.ok(cellA && cellB);
    assert.ok(cellA.faces.length >= 4);
    assert.ok(cellB.faces.length >= 4);
  }
  // A 24-cell packing in a thin wall is well connected.
  assert.ok(pkg.bonds.length >= pkg.cells.length);
});

test('degenerate requests are rejected instead of emitting broken cells', () => {
  const pkg = bakeVoronoi({ half: [1, 1, 1], cells: 1, seed: 3 });
  assert.ok(pkg.cells.length >= 2);
  assert.deepEqual(validatePackage(pkg), []);
});

test('oriented proxy bounds every vertex of its cell', () => {
  const pkg = bakeVoronoi(WALL);
  for (const cell of pkg.cells) {
    const obb = cell.obb;
    assert.ok(obb && obb.half.every((h) => h > 0));
    const obbVolume = 8 * obb.half[0] * obb.half[1] * obb.half[2];
    assert.ok(obbVolume >= cell.volume * 0.98, 'proxy smaller than the cell');
    for (const v of cell.vertices) {
      for (let axis = 0; axis < 3; ++axis) {
        const d = (v[0] - obb.center[0]) * obb.axes[axis][0]
          + (v[1] - obb.center[1]) * obb.axes[axis][1]
          + (v[2] - obb.center[2]) * obb.axes[axis][2];
        assert.ok(Math.abs(d) <= obb.half[axis] + 1e-3,
          `cell ${cell.index} vertex escapes its OBB`);
      }
    }
  }
});

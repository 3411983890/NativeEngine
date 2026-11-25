import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { bakeClothGrid, compileCloth, compileClothLibrary, migrateCloth } from '../engine/cloth/asset.mjs';
import { colorConstraints } from '../engine/cloth/topology.mjs';
import { cookClothLibrary } from '../tools/cloth.mjs';

const root = path.resolve(import.meta.dirname, '..');
const fixture = async (name = 'flag_banner') =>
  JSON.parse(await fs.readFile(path.join(root, `assets/cloth/${name}.cloth.json`), 'utf8'));

test('cloth bake derives rest geometry, mass and three constraint families', async () => {
  const asset = await fixture(), before = structuredClone(asset);
  const compiled = compileCloth(asset);
  assert.deepEqual(asset, before, 'compile mutated the authored source');
  const { baked } = compiled.payload.source;
  assert.equal(baked.positions.length, 20 * 14);
  assert.equal(baked.triangles.length, 2 * 19 * 13);
  assert.equal(baked.stretch.length, 19 * 14 + 20 * 13);
  assert.equal(baked.shear.length, 2 * 19 * 13);
  assert.equal(baked.bend.length, 18 * 14 + 20 * 12);
  assert.equal(baked.pins.length, 8);
  const mass = baked.masses.reduce((sum, value) => sum + value, 0);
  assert.ok(Math.abs(mass - asset.material.arealDensityKgM2 * baked.totalAreaMetres2) < 1e-6);
  for (const triangle of baked.triangles) assert.equal(new Set(triangle).size, 3);
  assert.equal(baked.positions[0][1], 0, 'row 0 must sit on the bottom edge');
  assert.equal(baked.positions.at(-1)[1], 0.8, 'the top row must be one cloth height up');
});

test('cloth identity is stable under key order and changes with geometry or material', async () => {
  const asset = await fixture();
  const compiled = compileCloth(asset);
  const reordered = Object.fromEntries(Object.entries(asset).reverse());
  assert.equal(compileCloth(reordered).hash, compiled.hash);
  const wider = await fixture(); wider.mesh.widthMetres = 1.25;
  const stiffer = await fixture(); stiffer.material.bendAngleCompliance = 100;
  assert.notEqual(compileCloth(wider).hash, compiled.hash);
  assert.notEqual(compileCloth(stiffer).hash, compiled.hash);
  assert.equal(compiled.hash.length, 64);
});

test('cloth asset rejects malformed grids, pins, colliders, LOD and budgets', async () => {
  const invalid = [
    (a) => { a.unknown = true; },
    (a) => { a.mesh.columns = 1; },
    (a) => { a.mesh.rows = 129; },
    (a) => { a.mesh.widthMetres = 0; },
    (a) => { a.pins = [{ column: 20, row: 0 }]; },
    (a) => { a.pins = [{ column: 0, row: 0 }, { column: 0, row: 0 }]; },
    (a) => { a.collision.plane = { normal: [0, 2, 0], offsetMetres: 0 }; },
    (a) => { a.collision.spheres = [{ centre: [0, 0, 0], radiusMetres: 0 }]; },
    (a) => { a.lod = [{ maxDistanceMetres: 5, columns: 10, rows: 7 }]; },
    (a) => { a.lod = [{ maxDistanceMetres: 8, columns: 20, rows: 14 }, { maxDistanceMetres: 7, columns: 10, rows: 7 }]; },
    (a) => { a.budget.maxVertices = 100; },
    (a) => { a.material.stretchCompliance = -1; },
    (a) => { a.kind = 'rigid'; },
  ];
  for (const mutate of invalid) {
    const asset = await fixture(); mutate(asset);
    assert.throws(() => compileCloth(asset), undefined, mutate.toString());
  }
});

test('bake rejects a zero-area sheet and reports per-vertex mass from triangles', async () => {
  const mesh = { type: 'grid', columns: 3, rows: 3, widthMetres: 1, heightMetres: 1 };
  const material = { arealDensityKgM2: 2, thicknessMetres: 0.01, stretchCompliance: 0, shearCompliance: 0, bendCompliance: 0, damping: 0, dragCoefficient: 0 };
  const baked = bakeClothGrid(mesh, material, [{ column: 1, row: 1 }]);
  assert.equal(baked.positions.length, 9);
  assert.equal(baked.pins[0].vertex, 4);
  const centre = baked.masses[4], corner = baked.masses[0];
  assert.ok(centre > corner, 'an interior vertex must carry more mass than a corner');
  assert.ok(Math.abs(baked.masses.reduce((s, v) => s + v, 0) - 2) < 1e-6);
  assert.throws(() => bakeClothGrid({ ...mesh, widthMetres: 0 }, material, []));
});

test('cloth library cooking is deterministic, content addressed and publish-on-success', async () => {
  const directory = await fs.mkdtemp(path.join(tmpdir(), 'ng-cloth-'));
  try {
    const source = path.join(directory, 'source'), out = path.join(directory, 'cooked');
    await fs.mkdir(source);
    for (const name of ['flag_banner', 'cape_cloak'])
      await fs.writeFile(path.join(source, `${name}.cloth.json`), JSON.stringify(await fixture(name)));
    const first = await cookClothLibrary({ sourceDirectory: source, outputDirectory: out });
    const second = await cookClothLibrary({ sourceDirectory: source, outputDirectory: out });
    assert.equal(first.packageHash, second.packageHash);
    assert.equal(first.runtimeIntegrated, false);
    assert.equal(first.cloths.length, 2);
    for (const entry of first.cloths) {
      const container = JSON.parse(await fs.readFile(path.join(out, entry.file), 'utf8'));
      assert.equal(container.schema, 'nativegpu.cloth-cook/2');
      assert.equal(container.hash, entry.hash);
      const payload = JSON.parse(container.payloadJson);
      assert.equal(payload.schema, 'nativegpu.cloth-cook/2');
      assert.equal(payload.source.id, entry.id);
    }
    const published = await fs.readFile(path.join(out, 'library.json'), 'utf8');
    const bad = await fixture(); bad.mesh.columns = 1;
    await fs.writeFile(path.join(source, 'invalid.cloth.json'), JSON.stringify(bad));
    await assert.rejects(cookClothLibrary({ sourceDirectory: source, outputDirectory: out }));
    assert.equal(await fs.readFile(path.join(out, 'library.json'), 'utf8'), published, 'failed cook replaced the published library');
  } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test('triangle cloth bakes rest strain, complete disjoint color schedules and oriented hinges', async()=>{
  const compiled=compileCloth(await fixture()),b=compiled.source.baked,t=b.topology;
  assert.equal(t.model,'triangle-xpbd/1');assert.equal(t.membranes.length,b.triangles.length);assert.equal(t.hinges.length,19*13+18*13+19*12);
  const checkColors=(colors,vertices)=>{const seen=new Set();for(const batch of colors){const occupied=new Set();for(const id of batch){assert.ok(!seen.has(id));seen.add(id);for(const v of vertices[id]){assert.ok(!occupied.has(v));occupied.add(v);}}}assert.equal(seen.size,vertices.length);};
  checkColors(t.membraneColors,b.triangles);checkColors(t.hingeColors,t.hinges.map(h=>h.vertices));
  assert.deepEqual(t.membraneColors,colorConstraints(b.triangles,b.positions.length));
  for(let id=0;id<b.triangles.length;++id){const ids=b.triangles[id],p=ids.map(v=>b.positions[v]),m=t.membranes[id];const e0=p[1].map((v,i)=>v-p[0][i]),e1=p[2].map((v,i)=>v-p[0][i]);const u=e0.map((v,i)=>v*m.inverse[0]+e1[i]*m.inverse[1]),v=e0.map((v,i)=>v*m.inverse[2]+e1[i]*m.inverse[3]);assert.ok(Math.abs(Math.hypot(...u)-1)<1e-12);assert.ok(Math.abs(Math.hypot(...v)-1)<1e-12);for(const vertex of ids)assert.ok(t.adjacency[vertex].includes(id));}
  for(const h of t.hinges){assert.equal(new Set(h.vertices).size,4);assert.ok(Math.abs(h.angle)<1e-12);}
});

test('legacy distance cloth remains explicit and angular migration requires a new parameter',async()=>{
  const current=await fixture(),old=structuredClone(current);old.schema='nativegpu.cloth/1';delete old.material.bendAngleCompliance;old.material.bendCompliance=1e-5;
  const legacy=compileCloth(old);assert.equal(legacy.payload.schema,'nativegpu.cloth-cook/1');assert.equal(legacy.source.baked.topology,undefined);
  assert.throws(()=>migrateCloth(old));const migrated=migrateCloth(old,1000);assert.equal(migrated.schema,'nativegpu.cloth/2');assert.equal(migrated.material.bendAngleCompliance,1000);assert.equal(migrated.material.bendCompliance,undefined);assert.deepEqual(old.material.bendCompliance,1e-5);assert.ok(compileCloth(migrated).source.baked.topology);
  assert.throws(()=>colorConstraints([[0,0,1]],3));assert.throws(()=>colorConstraints([[0,1,3]],3));
});

test('cloth library rejects duplicate ids and an empty source directory', async () => {
  const asset = await fixture();
  assert.throws(() => compileClothLibrary([asset, structuredClone(asset)]), /duplicate cloth id/);
  assert.throws(() => compileClothLibrary([]));
});

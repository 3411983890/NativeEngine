import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { buildHairFixture } from '../../tools/hair-style-fixture.mjs';
import { groomSourceFromHairstyle, toCompatibleGroomSource } from '../../engine/hair/cook.mjs';
import { compileGroom } from '../../engine/groom/asset.mjs';

// The authored hairstyle inside the native groom runtime.
//
// Before this entry the only groom a native runtime could load came from `toCompatibleGroomSource`:
// a spherical cap with 65 procedural guides and re-derived ids, i.e. 4% of the hairstyle's guides.
// `nativegpu.groom/2` transcribes the hairstyle's own scalp mesh, guides, strands and
// guide-weight table instead, and this test proves the native reader and the CPU XPBD guide
// solver accept that document — by compiling `tests/native_hair_groom.cpp` and running it.
//
// The compile/run is orchestrated from node on purpose: the repo's PowerShell entries cannot run
// native children in every environment, and a test that silently skips is worse than no test.

const ROOT = process.cwd();
const COMPILER = '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';
const BUILD = 'build/hair-groom';
const SOURCES = [
  'tests/native_hair_groom.cpp',
  'engine/native/groom/asset.cpp',
  'engine/native/groom/solver.cpp',
  'engine/native/assets/package.cpp',
  'engine/native/assets/sha256.cpp',
];

const { spec, generated } = await buildHairFixture();
const explicit = groomSourceFromHairstyle(generated);
const cooked = compileGroom(explicit);

mkdirSync(BUILD, { recursive: true });
const libraryDirectory = join(BUILD, 'cooked');
mkdirSync(libraryDirectory, { recursive: true });
writeFileSync(join(libraryDirectory, `${cooked.hash}.nggroom`), cooked.bytes);
const entry = {
  id: cooked.id, hash: cooked.hash, file: `${cooked.hash}.nggroom`,
  guides: cooked.guides, strands: cooked.strands, segments: cooked.segments,
  levels: cooked.levels, bytes: cooked.bytesTotal,
};
writeFileSync(join(libraryDirectory, 'library.json'),
  `${JSON.stringify({ schema: 'nativegpu.groom-library/1', grooms: [entry] })}\n`);

test('the explicit groom carries the hairstyle, not a cap', () => {
  // The two paths, side by side: this is the difference the plan complained about.
  const compatible = compileGroom(toCompatibleGroomSource(spec).source);
  assert.ok(cooked.guides >= 1000, `explicit groom has only ${cooked.guides} guides`);
  assert.ok(compatible.guides <= 100, `the cap path should stay a token groom, got ${compatible.guides} guides`);
  assert.equal(cooked.guides, generated.guides.length, 'every hairstyle guide must reach the package');
  assert.equal(cooked.strands, generated.levels[0].curves.length, 'strand count must match the hairstyle LOD 0');
  assert.equal(cooked.meta.scalp.triangles, generated.scalp.triangles, 'the scalp must be the character mesh');
  assert.equal(cooked.meta.scalp.kind, 'triangles');
  assert.equal(cooked.meta.groups.length, spec.regions.length, 'regions must survive as groom groups');
  assert.equal(cooked.meta.counts.strands, generated.levels[0].curves.length);
  // Byte-deterministic, like every other cook in this repo.
  assert.equal(compileGroom(groomSourceFromHairstyle(generated)).hash, cooked.hash);
});

test('the native reader and guide solver accept the explicit groom', (t) => {
  if (!existsSync(COMPILER)) {
    // Not a skip: without the pinned toolchain this entry cannot prove anything, and the repo
    // ships that toolchain. A missing compiler is a broken checkout, not an environment quirk.
    assert.fail(`compiler missing at ${COMPILER}; the pinned LLVM toolchain is required`);
  }
  const exe = join(BUILD, 'native_hair_groom_tests.exe');
  const compile = spawnSync(COMPILER, [
    '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-static', '-DNOMINMAX',
    ...SOURCES, '-o', exe, '-lbcrypt',
  ], { cwd: ROOT, encoding: 'utf8' });
  assert.equal(compile.status, 0, `native build failed:\n${compile.stdout}\n${compile.stderr}`);

  const run = spawnSync(exe, ['--library', join(libraryDirectory, 'library.json')],
    { cwd: ROOT, encoding: 'utf8' });
  assert.equal(run.status, 0, `native hair-groom evidence failed:\n${run.stderr}`);
  const evidence = JSON.parse(run.stdout);
  writeFileSync(join(BUILD, 'evidence.json'), `${JSON.stringify(evidence, null, 2)}\n`);

  assert.equal(evidence.ok, true);
  assert.equal(evidence.groom.guides, generated.guides.length);
  assert.equal(evidence.groom.strands, generated.levels[0].curves.length);
  assert.equal(evidence.groom.scalpTriangles, generated.scalp.triangles);
  assert.ok(evidence.groom.groups >= spec.regions.length, 'region groups must survive the cook');

  // The numbers the harness measured, asserted here as well so a solver regression fails this
  // entry rather than only changing a JSON file nobody reads.
  assert.ok(evidence.solve.defaultSettings.stretchRatio < 2.0,
    'the default stiffness must stay bounded on a hairstyle groom');
  assert.ok(evidence.solve.tunedSettings.stretchRatio < 1.15,
    'tuned stiffness must hold a 15 cm strand under gravity');
  assert.ok(evidence.solve.tunedWithHeadCollision.penetrationMetres <= 2e-3,
    'a guide point stayed inside the head sphere');
  assert.ok(evidence.solve.tunedWithHeadCollision.stretchRatio < 1.3,
    'head collision stretched the strands');
  assert.equal(evidence.solve.rootDrift.collided, 0, 'a pinned root moved under collision');
  assert.equal(Object.values(evidence.capabilities).filter((value) => value === false).length, 2,
    'the evidence must still name the two missing capabilities (GPU solve, strand draw)');
  t.diagnostic(`native: ${evidence.checks} checks, ${evidence.groups} groups, `
    + `stretch ${evidence.solve.tunedSettings.stretchRatio.toFixed(3)} tuned / `
    + `${evidence.solve.defaultSettings.stretchRatio.toFixed(3)} default`);
});

test('the package is readable without re-deriving anything', () => {
  // `points`/`segments` are what a runtime allocates from; a mismatch here is a package that
  // parses and then reads out of bounds.
  for (const level of cooked.levels) {
    assert.equal(level.points, level.strands * (level.segments + 1));
    assert.ok(level.cards > 0, 'every level needs cards or the far representation is missing');
  }
  assert.ok(cooked.levels[0].strands > cooked.levels[1].strands, 'LOD 1 must be coarser than LOD 0');
  const bytes = readFileSync(join(libraryDirectory, `${cooked.hash}.nggroom`));
  assert.equal(bytes.length, cooked.bytesTotal);
});

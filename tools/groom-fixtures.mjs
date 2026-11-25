import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// Original groom fixtures: a short bob, a long straight style and a brow/beard pair.
// They exist so the strand/card/guide pipeline can be exercised without shipping a
// licensed character scan.

const hairMaterial = (overrides = {}) => ({
  albedo: [0.09, 0.06, 0.045], roughness: 0.32, radialRoughness: 0.28, longitudinalRoughness: 0.34,
  absorption: [0.62, 0.44, 0.33], melanin: 0.82, redness: 0.35, ...overrides,
});

const grooms = [
  {
    schema: 'nativegpu.groom/1', id: 'bob_short', style: 'scalp', seed: 1001,
    scalp: { radiusMetres: 0.095, coverageDegrees: 118, rings: 10, segments: 24 },
    groups: [{ id: 'hair', material: hairMaterial(), simulation: 'guide', shadow: 'deepOpacity' }],
    guides: { count: 320, segments: 10, lengthMetres: 0.22, rootRadiusMetres: 0.00006, gravity: 0.85, comb: [0, -0.25, -0.6] },
    strands: { count: 4000, frizzMetres: 0.0025, widthMetres: 0.00007, rootJitterMetres: 0.0016 },
    lod: [
      { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 4 },
      { level: 1, strandFraction: 0.45, segmentScale: 0.7, maxDistanceMetres: 12 },
      { level: 2, strandFraction: 0.16, segmentScale: 0.5, maxDistanceMetres: 30 },
    ],
    budget: { maxStrands: 40000, maxPoints: 4000000 },
  },
  {
    schema: 'nativegpu.groom/1', id: 'long_straight', style: 'scalp', seed: 2002,
    scalp: { radiusMetres: 0.098, coverageDegrees: 126, rings: 12, segments: 28 },
    groups: [
      { id: 'hair', material: hairMaterial({ albedo: [0.28, 0.17, 0.07], melanin: 0.42, redness: 0.18, absorption: [0.4, 0.28, 0.16] }), simulation: 'guide', shadow: 'deepOpacity' },
    ],
    guides: { count: 480, segments: 14, lengthMetres: 0.52, rootRadiusMetres: 0.00007, gravity: 1.15, comb: [0, -0.5, -0.45] },
    strands: { count: 9000, frizzMetres: 0.004, widthMetres: 0.00006, rootJitterMetres: 0.0018 },
    lod: [
      { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 5 },
      { level: 1, strandFraction: 0.4, segmentScale: 0.65, maxDistanceMetres: 16 },
      { level: 2, strandFraction: 0.12, segmentScale: 0.45, maxDistanceMetres: 40 },
    ],
    budget: { maxStrands: 60000, maxPoints: 6000000 },
  },
  {
    schema: 'nativegpu.groom/1', id: 'brow_patch', style: 'brows', seed: 3003,
    scalp: { radiusMetres: 0.03, coverageDegrees: 52, rings: 6, segments: 16 },
    groups: [{ id: 'brow', material: hairMaterial({ albedo: [0.05, 0.035, 0.025], melanin: 0.92, absorption: [0.7, 0.5, 0.38] }), simulation: 'rigid', shadow: 'none' }],
    guides: { count: 64, segments: 5, lengthMetres: 0.022, rootRadiusMetres: 0.00004, gravity: 0.25, comb: [0.7, 0.2, 0.1] },
    strands: { count: 420, frizzMetres: 0.0006, widthMetres: 0.00005, rootJitterMetres: 0.0009 },
    lod: [
      { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 2 },
      { level: 1, strandFraction: 0.5, segmentScale: 0.7, maxDistanceMetres: 8 },
    ],
    budget: { maxStrands: 4000, maxPoints: 200000 },
  },
];

export async function writeGroomFixtures(directory) {
  const target = path.resolve(directory);
  await fs.mkdir(target, { recursive: true });
  for (const entry of grooms) {
    await fs.writeFile(path.join(target, `${entry.id}.groom.json`), `${JSON.stringify(entry, null, 2)}\n`);
  }
  return { directory: target, grooms: grooms.map((entry) => entry.id) };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const target = process.argv[2] ?? 'build/groom/source';
  writeGroomFixtures(target).then((result) => console.log(JSON.stringify(result))).catch((error) => { console.error(error); process.exitCode = 1; });
}

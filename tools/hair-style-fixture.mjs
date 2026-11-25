import { loadCharacterAsset } from '../engine/character/rig-mesh.mjs';
import { REGION_PRESETS, normalizeHairstyle } from '../engine/hair/spec.mjs';
import { buildScalp } from '../engine/hair/scalp.mjs';
import { generateHairstyle } from '../engine/hair/generate.mjs';
import { toCompatibleGroomSource } from '../engine/hair/cook.mjs';

// One hairstyle used by every hair fixture in this repository: the render reference, the
// native guide-solver harness and the GPU data-pack test.
//
// Density is deliberately moderate. These fixtures exist to prove contracts — coverage,
// depth ordering, shadow transmittance, guide determinism — and a 60k-strand style makes
// every one of those checks slower without making any of them stronger. The numbers a real
// hairstyle produces are a budget question, not a contract question.

export const HAIR_STYLE_ID = 'fixture_bob';

export function hairStyleSpec(overrides = {}) {
  return normalizeHairstyle({
    schema: 'nativegpu.hairstyle/1',
    id: HAIR_STYLE_ID,
    seed: 4242,
    character: { headBone: 'head', assetId: 'assets/builtin/robot/character.json' },
    regions: [
      {
        id: 'bangs', ...REGION_PRESETS.bangs, densityPerCm2: 34, lengthMetres: 0.115, comb: [0, -0.25, 1],
        parting: { azimuthDegrees: 0, widthDegrees: 6 },
      },
      { id: 'crown', ...REGION_PRESETS.crown, densityPerCm2: 34, lengthMetres: 0.15, comb: [0, -0.35, 0.5] },
      { id: 'left_side', ...REGION_PRESETS.left_side, densityPerCm2: 30, lengthMetres: 0.16, comb: [0.4, -0.4, 0.2] },
      { id: 'right_side', ...REGION_PRESETS.right_side, densityPerCm2: 30, lengthMetres: 0.16, comb: [-0.4, -0.4, 0.2] },
      { id: 'back', ...REGION_PRESETS.back, densityPerCm2: 34, lengthMetres: 0.17, comb: [0, -0.5, -0.4] },
    ],
    strands: { widthMetres: 0.00007, rootJitterMetres: 0.0012, streamsPerGuide: 8 },
    guides: { segments: 8, radiusMetres: 0.00007 },
    material: { groups: [{ id: 'hair', albedo: [0.09, 0.06, 0.04], roughness: 0.3 }] },
    physics: { gravityMetresPerSecond2: 9.81 },
    lod: [
      { level: 0, strandFraction: 1, segmentScale: 1, maxDistanceMetres: 4 },
      { level: 1, strandFraction: 0.5, segmentScale: 0.7, maxDistanceMetres: 12 },
    ],
    budget: { maxStrands: 60000, maxPoints: 3000000 },
    ...overrides,
  });
}

export async function buildHairFixture({ characterPath = 'assets/builtin/robot/character.json', overrides = {} } = {}) {
  const character = await loadCharacterAsset(characterPath);
  const scalp = buildScalp(character);
  const spec = hairStyleSpec(overrides);
  const generated = generateHairstyle(spec, character, { scalp });
  return { character, scalp, spec, generated };
}

// The groom source a native runtime can consume today. `toCompatibleGroomSource` reports its
// own losses — spherical-cap scalp, averaged material, re-derived ids — and those losses are
// carried through into every report that uses this document rather than being dropped.
export function hairGroomSource(spec) {
  return toCompatibleGroomSource(spec);
}

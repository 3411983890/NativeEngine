import fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { loadCharacterAsset } from '../engine/character/rig-mesh.mjs';
import { buildScalp } from '../engine/hair/scalp.mjs';
import { generateHairstyle } from '../engine/hair/generate.mjs';
import { cookHairstyle, readHairPackage } from '../engine/hair/cook.mjs';
import { validateHairstyle, HAIR_TEST_POSES } from '../engine/hair/validate.mjs';
import { applyPatch, hairRevision } from '../engine/hair/patch.mjs';
import { buildCapturePlan, captureMetadata, cameraForView } from '../engine/hair/capture.mjs';
import { packHairForGpu, validateHairPack } from '../engine/hair/gpu-pack.mjs';
import { renderHairReference, toRgba8, coverageRgba8, meanLuminance } from '../engine/hair/render-reference.mjs';
import { encodePng } from '../engine/weights/png.mjs';
import { toCompatibleGroomSource, groomSourceFromHairstyle } from '../engine/hair/cook.mjs';
import { compileGroom } from '../engine/groom/asset.mjs';
import { hairStyleSpec } from './hair-style-fixture.mjs';

// Reproducible sample record for the hair pipeline.
//
// One command produces the numbers the AI guide quotes: cook hashes and byte counts, the
// validation report, the GPU pack, the capture plan, a localised patch and its revision, and
// the CPU reference renders. It exists so a reader can re-run the *exact* sequence rather
// than trust a transcript, and so a regression shows up as a changed record instead of as
// advice that quietly stopped being true.
//
// The renders are CPU reference images, not engine frames. `renderStats.backend` says so on
// every entry, and no part of this record may be quoted as evidence for the D3D12 pass.

const ROOT = path.resolve(import.meta.dirname, '..');
const outputDirectory = path.resolve(process.argv[2] ?? path.join(ROOT, 'build/hair-sample'));
const framesDirectory = path.join(outputDirectory, 'frames');
await fs.mkdir(framesDirectory, { recursive: true });

const character = await loadCharacterAsset(path.join(ROOT, 'assets/builtin/robot/character.json'));
const scalp = buildScalp(character);
const spec = hairStyleSpec();
const generated = generateHairstyle(spec, character, { scalp });

// 1. Cook and read back. Everything below is taken from the bytes, not from `generated`.
const cooked = cookHairstyle(generated);
const read = readHairPackage(cooked.bytes);
const materialGroup = spec.material.groups.find((group) => group.id === 'hair');

// 2. Validation: binding, guide penetration, LOD identity, budget, transparency demand.
const validation = validateHairstyle({
  generated, spec, scalp, character,
  poses: HAIR_TEST_POSES,
  resolution: { width: 1920, height: 1080 },
});

// 3. GPU pack for the draw path.
const pack = packHairForGpu(read, { level: 0 });
const packCheck = validateHairPack(pack);
const coarsePack = packHairForGpu(read, { level: 1 });

// 4. Capture plan: fixed cameras, lighting and channels, each with its own metadata.
const plan = buildCapturePlan({
  hairstyleId: spec.id, revision: hairRevision(spec), assetHash: cooked.hash, seed: spec.seed,
  views: ['front', 'back', 'three_quarter_left', 'hairline_closeup'],
  lighting: ['neutral', 'backlight'], headCentre: scalp.centre, headRadiusMetres: scalp.radius,
  resolution: { width: 320, height: 320 },
});

// 5. Reference renders for three views plus a coverage channel.
const keyLight = { direction: [0.35, 0.55, 0.75], colour: [1, 0.97, 0.92], intensity: 3.6 };
const rimLight = { direction: [-0.25, 0.1, -0.96], colour: [1, 0.95, 0.88], intensity: 4.2 };
const frames = [];
for (const view of ['front', 'three_quarter_left', 'back']) {
  for (const [lightName, light] of [['neutral', keyLight], ['backlight', rimLight]]) {
    const camera = cameraForView(view, {
      headCentre: scalp.centre, headRadiusMetres: scalp.radius, targetOffsetMetres: 0.42,
    });
    const result = renderHairReference({
      generated, material: materialGroup, camera, width: 320, height: 320, light,
      options: { maxLayersPerPixel: 16 },
    });
    const name = `${view}-${lightName}`;
    await fs.writeFile(path.join(framesDirectory, `${name}.png`),
      encodePng({ width: 320, height: 320, rgba: toRgba8(result) }));
    if (lightName === 'neutral') {
      await fs.writeFile(path.join(framesDirectory, `${name}-coverage.png`),
        encodePng({ width: 320, height: 320, rgba: coverageRgba8(result) }));
    }
    const metadata = captureMetadata(plan.entries[0], {
      view, lighting: lightName, backend: 'cpu-reference', elapsedMs: result.stats.elapsedMs,
    });
    frames.push({
      file: `frames/${name}.png`,
      view, lighting: lightName, backend: 'cpu-reference',
      coverageFraction: Number(result.stats.coverageFraction.toFixed(4)),
      meanLayers: Number(result.stats.meanLayers.toFixed(2)),
      overflowFraction: Number(result.stats.overflowFraction.toFixed(4)),
      spillPixels: result.stats.spillPixels,
      meanLuminance: Number(meanLuminance(result).toFixed(4)),
      shadowMaxOpticalDepthMetres: result.stats.shadow
        ? Number(result.stats.shadow.maxOpticalDepthMetres.toFixed(5)) : null,
      elapsedMs: result.stats.elapsedMs,
      imageHash: createHash('sha256').update(toRgba8(result)).digest('hex').slice(0, 16),
      metadata,
    });
  }
}

// 6. A localised patch: shorten the bangs only, and prove the other regions did not move.
const revision = hairRevision(spec);
const patch = applyPatch(spec, {
  expectedRevision: revision,
  note: 'shorten the bangs so the right eye clears',
  operations: [{ kind: 'region', region: 'bangs', fields: ['lengthMetres'], changes: { lengthMetres: 0.095 } }],
});
const patched = generateHairstyle(patch.spec, character, { scalp });
const bangsBefore = spec.regions.find((region) => region.id === 'bangs');
const bangsAfter = patch.spec.regions.find((region) => region.id === 'bangs');
// Tips per guide for one region, keyed by stable id: a patch that regenerates the bangs must
// leave the other regions' guides exactly where they were, and "exactly" is the point.
const regionTips = (regionId, generatedSet) => new Map(
  generatedSet.guides.filter((guide) => guide.region === regionId).map((guide) => [guide.stableId, guide.points.at(-1)]));

const movedTips = (regionId, before, after) => {
  const previous = regionTips(regionId, before);
  const next = regionTips(regionId, after);
  let moved = 0;
  for (const [id, tip] of previous) {
    const other = next.get(id);
    if (!other) { moved += 1; continue; }
    const distance = Math.hypot(tip[0] - other[0], tip[1] - other[1], tip[2] - other[2]);
    if (distance > 1e-4) moved += 1;
  }
  return moved;
};

// 7. Both groom paths: the explicit one is what a native runtime should load, and the
// compatible cap is kept so the difference between them stays measurable.
const compatible = toCompatibleGroomSource(patch.spec);
const compatibleCooked = compileGroom(compatible.source);
const explicitGroom = compileGroom(groomSourceFromHairstyle(patched));

const record = {
  schema: 'nativegpu.hair-sample-record/1',
  generatedAt: new Date().toISOString(),
  hairstyle: {
    id: spec.id, seed: spec.seed, revision,
    guides: generated.guides.length, strands: generated.levels[0].curves.length,
    scalpTriangles: scalp.triangles, strandsPerGuide: spec.strands.streamsPerGuide,
    regions: spec.regions.map((region) => region.id),
  },
  cook: {
    schema: cooked.meta?.schema ?? null, hash: cooked.hash,
    bytes: cooked.bytesTotal, counts: cooked.counts, lod: cooked.lod, regions: cooked.regions,
  },
  validation: {
    schema: validation.schema,
    passes: validation.passes,
    failures: validation.failures,
    verdict: validation.verdict,
    poses: validation.poseSummary ?? validation.poses.map((pose) => ({
      pose: pose.pose,
      rootsOffSurface: pose.rootsOffSurface,
      maxSurfaceErrorMillimetres: Number((pose.maxSurfaceErrorMetres * 1000).toFixed(5)),
      penetrationP99Millimetres: Number((pose.penetration.p99Metres * 1000).toFixed(3)),
      enforced: pose.enforced,
    })),
    counts: validation.counts,
    scalp: validation.scalp,
    lod: validation.lod,
    timings: validation.timings,
    unmeasured: validation.unmeasured,
  },
  gpuPack: {
    schema: pack.schema, level: pack.level, hash: pack.hash,
    counts: pack.counts, strides: pack.strides, draw: pack.draw,
    uploadBytes: pack.uploadBytes, bounds: pack.bounds, validation: pack.validation,
    check: packCheck,
    coarse: { level: coarsePack.level, counts: coarsePack.counts, uploadBytes: coarsePack.uploadBytes },
  },
  capturePlan: {
    entries: plan.entries.length,
    views: [...new Set(plan.entries.map((entry) => entry.view))],
    channels: [...new Set(plan.entries.flatMap((entry) => entry.channels))],
    firstEntry: plan.entries[0],
  },
  frames,
  patch: {
    expectedRevision: revision, newRevision: hairRevision(patch.spec),
    diff: patch.diff,
    regionsRegenerated: patch.diff.regeneratedRegions,
    bangsLengthBefore: bangsBefore?.lengthMetres ?? null,
    bangsLengthAfter: bangsAfter?.lengthMetres ?? null,
    bangsGuidesMoved: movedTips('bangs', generated, patched),
    backGuidesMoved: movedTips('back', generated, patched),
  },
  groom: {
    explicit: {
      sourceSchema: 'nativegpu.groom/2', hash: explicitGroom.hash, bytes: explicitGroom.bytesTotal,
      guides: explicitGroom.guides, strands: explicitGroom.strands,
      scalpKind: explicitGroom.meta.scalp.kind, scalpTriangles: explicitGroom.meta.scalp.triangles,
      groups: explicitGroom.meta.groups.length, levels: explicitGroom.levels,
    },
    compatible: {
      sourceSchema: 'nativegpu.groom/1', hash: compatibleCooked.hash, bytes: compatibleCooked.bytesTotal,
      guides: compatibleCooked.guides, strands: compatibleCooked.strands, losses: compatible.losses,
    },
  },
  notMeasured: [
    'GPU generation, guide simulation and draw timings',
    'video memory and transparency-cache overflow on real hardware',
    'frame-level flicker under camera motion on real hardware',
    'backlight transmission in the engine renderer',
  ],
};

const recordFile = path.join(outputDirectory, 'record.json');
await fs.writeFile(recordFile, `${JSON.stringify(record, null, 2)}\n`);
console.log(JSON.stringify({
  schema: record.schema, recordFile,
  cook: `${record.cook.hash.slice(0, 12)}… (${record.cook.bytes} B)`,
  validationPasses: record.validation.passes,
  packBytes: record.gpuPack.uploadBytes,
  frames: record.frames.length,
  patch: `${record.patch.expectedRevision.slice(0, 12)}… → ${record.patch.newRevision.slice(0, 12)}…`,
}));

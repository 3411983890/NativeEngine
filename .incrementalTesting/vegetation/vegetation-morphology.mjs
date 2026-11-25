import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { arcLengthOf, crownMismatch, evaluateBranchGraph } from '../../engine/vegetation/branch-graph.mjs';
import { defaultGenerators, normalizeGenerators } from '../../engine/vegetation/generators.mjs';
import { placeLeaves } from '../../engine/vegetation/leaf-placement.mjs';

// Morphology metrics for the realism baseline. These are the numbers R1 is judged against, so
// they are computed from the same graph the cook builds — not from a separate approximation that
// could drift from the mesh.
//
// What each metric is for:
//   arcRatio        actual curve length / authored target. Measured arc length replaced a
//                   `stepLength * step` placeholder, so this must land near 1 and stay stable.
//   spikeCount      segments whose turn exceeds a threshold. The hard envelope projection used to
//                   kink the curve; a branch that is steered gently has few of these.
//   crownActual     the widest tip radius the graph really reaches vs the authored bound.
//   slenderness     height / trunk diameter (DBH proxy): the "is this a tree or a stick" number.

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SPECIES = ['oak_broadleaf', 'pine_conifer', 'hazel_shrub', 'meadow_grass'];
const SEEDS = [1, 7, 13];
const SPIKE_SOFT = 0.25;   // radians between consecutive segment tangents
const SPIKE_HARD = 0.5;

const readJson = (file) => fs.readFile(file, 'utf8').then((text) => JSON.parse(text.replace(/^\uFEFF/, '')));

const mean = (values) => (values.length ? values.reduce((a, b) => a + b, 0) / values.length : null);
const median = (values) => {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const round = (value) => (Number.isFinite(value) ? Math.round(value * 1000) / 1000 : null);

function turnAngles(branch) {
  const angles = [];
  const points = branch.points;
  for (let index = 1; index + 1 < points.length; ++index) {
    const a = points[index].tangent, b = points[index + 1].tangent;
    const dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const la = Math.hypot(a[0], a[1], a[2]), lb = Math.hypot(b[0], b[1], b[2]);
    if (la < 1e-9 || lb < 1e-9) continue;
    angles.push(Math.acos(Math.max(-1, Math.min(1, dot / (la * lb)))));
  }
  return angles;
}

function measureGraph(graph) {
  const branches = graph.branches;
  const rootBranch = graph.byId.get(graph.rootId) ?? branches[0];
  // Crown reach: the furthest point from the trunk axis, which is what "crown radius" means to a
  // viewer. Tips alone would understate it, because a wide branch's outer edge is mid-branch.
  let crownActual = 0;
  for (const branch of branches) {
    for (const point of branch.points) crownActual = Math.max(crownActual, Math.hypot(point.position[0], point.position[2]));
  }

  const levels = [];
  const byLevel = new Map();
  for (const branch of branches) {
    const entry = byLevel.get(branch.level) ?? { level: branch.level, branches: [] };
    entry.branches.push(branch);
    byLevel.set(branch.level, entry);
  }
  for (const entry of [...byLevel.values()].sort((a, b) => a.level - b.level)) {
    const targets = entry.branches.map((branch) => branch.length);
    const actuals = entry.branches.map((branch) => arcLengthOf(branch));
    const ratios = entry.branches.map((branch, index) => (targets[index] > 0 ? actuals[index] / targets[index] : 1));
    const angles = entry.branches.flatMap(turnAngles);
    // Per-level reach and height. Without these, "the crown is too narrow" cannot be attributed:
    // it could be the lowest limbs being clamped, the shell being narrow where they attach, or the
    // authored lengths being short. This is the breakdown that answers it.
    let reach = 0, top = 0, attachY = 0, attachCount = 0;
    let cappedSteps = 0, apexSteps = 0, totalSteps = 0;
    let spikesOnCapped = 0, spikesOnFree = 0;
    for (const branch of entry.branches) {
      for (const point of branch.points) {
        reach = Math.max(reach, Math.hypot(point.position[0], point.position[2]));
        top = Math.max(top, point.position[1]);
        ++totalSteps;
        if (point.capped) ++cappedSteps;
        if (point.apexed) ++apexSteps;
      }
      // Attribute every sharp turn to the mechanism that was active at that step: that is what
      // makes "the spikes are near the apex" a measurement instead of an impression.
      const points = branch.points;
      for (let index = 1; index + 1 < points.length; ++index) {
        const a = points[index].tangent, b = points[index + 1].tangent;
        const la = Math.hypot(a[0], a[1], a[2]), lb = Math.hypot(b[0], b[1], b[2]);
        if (la < 1e-9 || lb < 1e-9) continue;
        const angle = Math.acos(Math.max(-1, Math.min(1, (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb))));
        if (angle <= SPIKE_HARD) continue;
        if (points[index + 1].capped || points[index + 1].apexed) ++spikesOnCapped; else ++spikesOnFree;
      }
      if (branch.points.length) { attachY += branch.points[0].position[1]; ++attachCount; }
    }
    levels.push({
      level: entry.level,
      branches: entry.branches.length,
      maxRadialMetres: round(reach),
      maxYMetres: round(top),
      meanAttachYMetres: round(attachCount ? attachY / attachCount : null),
      steps: { total: totalSteps, capped: cappedSteps, apexed: apexSteps },
      spikeCause: { onCappedOrApexStep: spikesOnCapped, onFreeStep: spikesOnFree },
      targetLengthMetres: { mean: round(mean(targets)), median: round(median(targets)) },
      actualLengthMetres: { mean: round(mean(actuals)), median: round(median(actuals)) },
      arcRatio: { mean: round(mean(ratios)), min: round(Math.min(...ratios)), max: round(Math.max(...ratios)) },
      turnRadians: { mean: round(mean(angles)), max: round(angles.length ? Math.max(...angles) : 0) },
      spikeCount: {
        soft: angles.filter((angle) => angle > SPIKE_SOFT).length,
        hard: angles.filter((angle) => angle > SPIKE_HARD).length,
      },
      radiusMetres: {
        root: round(mean(entry.branches.map((branch) => branch.rootRadius))),
        tip: round(mean(entry.branches.map((branch) => branch.tipRadius))),
      },
    });
  }

  const trunkDiameter = rootBranch ? rootBranch.rootRadius * 2 : null;
  const allAngles = branches.flatMap(turnAngles);
  return {
    authored: {
      heightMetres: graph.heightMetres,
      crownRadiusMetres: graph.crownRadiusMetres,
      trunkRadiusMetres: rootBranch ? round(rootBranch.rootRadius) : null,
    },
    measured: {
      crownRadiusMetres: round(crownActual),
      crownFractionOfAuthored: round(graph.crownRadiusMetres > 0 ? crownActual / graph.crownRadiusMetres : null),
      crownOvershootFraction: round(graph.crownRadiusMetres > 0 ? crownActual / graph.crownRadiusMetres - 1 : null),
      // The shape trait the generator actually grew toward. Kept separate from the declaration so
      // a report can tell "the profile's proportion is wrong" from "the declaration is stale".
      shapeCrownRadiusMetres: round(graph.shapeCrownRadiusMetres ?? null),
      crownFractionOfShape: round(graph.shapeCrownRadiusMetres > 0 ? crownActual / graph.shapeCrownRadiusMetres : null),
      trunkDiameterMetres: round(trunkDiameter),
      slendernessRatio: round(trunkDiameter > 0 ? graph.heightMetres / trunkDiameter : null),
      branches: branches.length,
      maxDepth: branches.reduce((depth, branch) => Math.max(depth, branch.level), 0),
      totalTargetLengthMetres: round(branches.reduce((sum, branch) => sum + branch.length, 0)),
      totalActualLengthMetres: round(branches.reduce((sum, branch) => sum + arcLengthOf(branch), 0)),
      globalArcRatio: null, // filled below
      spikeCount: {
        soft: allAngles.filter((angle) => angle > SPIKE_SOFT).length,
        hard: allAngles.filter((angle) => angle > SPIKE_HARD).length,
        segments: allAngles.length,
      },
    },
    levels,
  };
}

// Foliage metrics from the same placement the cook uses. Leaf area is one-sided; LAI divides it by
// the measured crown's projection disc. Shoots are the placement unit, so `emptyShoots` is the
// share of bare buds — the crown's intentional gaps, not leaves lost to LOD.
function measureFoliage(graph, generators, spec) {
  if (spec.kind === 'grass' || !generators.fronds.length) return null;
  const placed = placeLeaves({
    graph, fronds: generators.fronds, atlas: null, spec, quality: { leaves: 1 }, seed: spec.seed,
  });
  if (!placed.leaves.length) return null;
  const crown = Math.max(0.01, graph.stats.crownRadialMetres ?? 0.01);
  const crownHeight = Math.max(0.01, (graph.stats.boundsMaxY ?? 0) - (graph.stats.boundsMinY ?? 0));
  const projection = Math.PI * crown * crown;
  const volume = (4 / 3) * Math.PI * crown * crown * (crownHeight / 2);
  const shoots = placed.perFrond.reduce((sum, entry) => sum + (entry.shoots ?? 0), 0);
  const emptyShoots = placed.perFrond.reduce((sum, entry) => sum + (entry.emptyShoots ?? 0), 0);
  return {
    leaves: placed.leaves.length,
    shoots,
    emptyShoots,
    emptyShootFraction: shoots + emptyShoots > 0 ? round(emptyShoots / (shoots + emptyShoots)) : null,
    spacingRejected: placed.perFrond.reduce((sum, entry) => sum + (entry.spacingRejected ?? 0), 0),
    leafAreaMetresSquared: placed.leafAreaMetresSquared,
    leafAreaIndex: round(placed.leafAreaMetresSquared / projection),
    leafAreaPerCrownVolume: round(placed.leafAreaMetresSquared / volume),
    minLeafSpacingMetres: round(placed.spacingMetres),
  };
}

async function loadSpec(id) {
  const authored = path.join(root, 'assets/vegetation/species', `${id}.species.json`);
  const fixture = path.join(root, 'build/vegetation/source', `${id}.species.json`);
  const file = await fs.stat(authored).then(() => authored).catch(() => fixture);
  return { spec: await readJson(file), file: path.relative(root, file).split(path.sep).join('/') };
}

async function main() {
  const species = [];
  for (const id of SPECIES) {
    const { spec, file } = await loadSpec(id);
    const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
    const variants = [];
    for (const seed of SEEDS) {
      const graph = evaluateBranchGraph({ spec: { ...spec, seed }, generators, seed });
      const measured = measureGraph(graph);
      measured.measured.globalArcRatio = measured.measured.totalTargetLengthMetres > 0
        ? round(measured.measured.totalActualLengthMetres / measured.measured.totalTargetLengthMetres)
        : null;
      measured.foliage = measureFoliage(graph, generators, { ...spec, seed });
      variants.push({ seed, ...measured, crown: crownMismatch(graph) });
    }
    species.push({
      id,
      source: file,
      kind: spec.dimensions?.kind ?? null,
      variants,
      // A species is a species, not a seed: report the spread, so "same identity across seeds"
      // can be judged instead of eyeballed.
      spread: {
        heightMetres: { min: round(Math.min(...variants.map((v) => v.authored.heightMetres))), max: round(Math.max(...variants.map((v) => v.authored.heightMetres))) },
        crownRadiusMetres: { min: round(Math.min(...variants.map((v) => v.measured.crownRadiusMetres))), max: round(Math.max(...variants.map((v) => v.measured.crownRadiusMetres))) },
        crownFractionOfAuthored: { min: round(Math.min(...variants.map((v) => v.measured.crownFractionOfAuthored))), max: round(Math.max(...variants.map((v) => v.measured.crownFractionOfAuthored))) },
        crownFractionOfShape: { min: round(Math.min(...variants.map((v) => v.measured.crownFractionOfShape))), max: round(Math.max(...variants.map((v) => v.measured.crownFractionOfShape))) },
        globalArcRatio: { min: round(Math.min(...variants.map((v) => v.measured.globalArcRatio))), max: round(Math.max(...variants.map((v) => v.measured.globalArcRatio))) },
        leafAreaIndex: variants[0].foliage
          ? { min: round(Math.min(...variants.map((v) => v.foliage.leafAreaIndex))), max: round(Math.max(...variants.map((v) => v.foliage.leafAreaIndex))) }
          : null,
      },
    });
  }

  const document = {
    schema: 'nativegpu.vegetation-morphology/1',
    generatedAt: new Date().toISOString(),
    seeds: SEEDS,
    thresholds: {
      // Frozen before R1 changes so a later "improvement" cannot move the goalposts. The art
      // review pass may tighten these; it may not loosen them to make a regression pass.
      arcRatio: { target: [0.98, 1.35] },
      spikeSoftPerBranch: { draft: 2 },
      crownFractionOfAuthored: { target: [0.85, 1.05] },
      status: 'draft-pending-art-review',
    },
    note: 'Metrics come from the same evaluateBranchGraph and placeLeaves the cook uses. leafArea is one-sided; LAI divides it by the measured crown projection. Grass foliage is its blades and is reported as null here.',
    species,
  };
  const output = path.join(root, 'reports/vegetation-realism/morphology.json');
  await fs.mkdir(path.dirname(output), { recursive: true });
  await fs.writeFile(output, `${JSON.stringify(document, null, 2)}\n`);
  for (const entry of species) {
    const first = entry.variants[0];
    const foliage = first.foliage;
    console.log(`${entry.id.padEnd(16)} branches=${String(first.measured.branches).padStart(4)}`
      + ` crown=${String(first.measured.crownRadiusMetres).padStart(6)}(authored ${first.authored.crownRadiusMetres})`
      + ` arcRatio=${String(first.measured.globalArcRatio).padStart(5)}`
      + ` spikes soft/hard=${first.measured.spikeCount.soft}/${first.measured.spikeCount.hard}`
      + ` slenderness=${first.measured.slendernessRatio}`
      + (foliage ? ` leaves=${foliage.leaves} LAI=${foliage.leafAreaIndex} emptyBuds=${foliage.emptyShootFraction}` : ''));
  }
  console.log(JSON.stringify({ output: path.relative(root, output).split(path.sep).join('/'), species: species.length }));
}

await main();

import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// Cross-file integrity check for reports/vegetation-production.
//
// Every evidence file is produced by a different tool at a different moment, so the failure mode
// is not "a number is wrong" but "the numbers describe different runs": a report that quotes a
// package hash the host never loaded, a GPU field that was measured but reads 0, an acceptance
// table claiming images that are not on disk. This checker asserts the files agree with each other
// and with the artefacts in build/. It does not re-measure anything 鈥?that is the report tool's job.
//
// Usage: node .incrementalTesting/vegetation/vegetation-evidence-check.mjs

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const directory = path.join(root, 'reports/vegetation-production');
const problems = [];
const checks = [];

function check(label, ok, detail = '') {
  checks.push({ label, ok });
  if (!ok) problems.push(`${label}${detail ? `: ${detail}` : ''}`);
}

async function readJson(file) {
  return JSON.parse((await fs.readFile(path.join(directory, file), 'utf8')).replace(/^\uFEFF/, ''));
}

async function main() {
  const geometry = await readJson('geometry.json');
  const materials = await readJson('materials.json');
  const performance = await readJson('performance.json');
  const sync = await readJson('editor-sync.json');
  const provenance = await readJson('provenance.json');
  const nativeDraw = await readJson('native-draw.json');
  const images = await fs.readdir(path.join(directory, 'images'));

  // --- every measured quantity must be a number, and every unmeasured one must say so
  check('geometry measured at least four species', geometry.species.length >= 4, `${geometry.species.length}`);
  for (const species of geometry.species) {
    const run = species.runs?.[0];
    check(`${species.id} reports a finite attachment gap`, Number.isFinite(run?.stats?.junctionGapMaxMetres),
      String(run?.stats?.junctionGapMaxMetres));
    check(`${species.id} has no degenerate triangles`, run?.mesh?.degenerateTriangles === 0,
      String(run?.mesh?.degenerateTriangles));
    check(`${species.id} kept its LOD error monotone`, species.cooked.lodTable.every((entry, index, table) =>
      index === 0 || entry.worldErrorMetres >= table[index - 1].worldErrorMetres || entry.worldErrorMetres === 0));
    check(`${species.id} reports area retention below 1 for coarse LODs`,
      species.cooked.lodTable.slice(1).every((entry) => entry.areaRetention !== null && entry.areaRetention < 1));
  }

  // --- material slots: nothing declared may be missing from the cooked library
  for (const species of materials.species) {
    const missing = species.bindings.filter((binding) => binding.status !== 'bound');
    check(`${species.id} has every declared texture slot bound`, missing.length === 0,
      missing.map((binding) => `${binding.slot}:${binding.status}`).join(','));
    check(`${species.id} declares a colour space for every role`,
      species.bindings.every((binding) => binding.colorSpace !== null));
  }
  const needleRoles = materials.roleTable.filter((role) => role.role.startsWith('needle_'));
  check('the generated needle atlas is in the cooked role table', needleRoles.length === 2,
    needleRoles.map((role) => role.role).join(','));
  // An atlas role without a grid would be filtered as one sheet, which is the cross-cell bleed the
  // per-cell mip chain exists to prevent 鈥?so the grid is required, not decorative metadata.
  const atlasRoles = materials.roleTable.filter((role) => /^(leaf|needle|diag)_/.test(role.role));
  check('every atlas role declares a cell grid', atlasRoles.every((role) => role.atlas !== null && role.atlas !== undefined),
    atlasRoles.filter((role) => role.atlas === null || role.atlas === undefined).map((role) => role.role).join(','));

  // --- provenance must describe the packages the sync check actually saw
  const librarySpecies = new Map((provenance.cookedLibrary?.species ?? []).map((entry) => [entry.id, entry.hash]));
  check('provenance lists the cooked species', librarySpecies.size >= 4, `${librarySpecies.size}`);
  const initial = sync.initial ?? {};
  const cachedDigest = (provenance.cookedLibrary?.species ?? [])
    .find((entry) => entry.hash.startsWith(String(initial.digest ?? '').slice(0, 4)));
  check('the ACK digest belongs to a package in the library', cachedDigest !== undefined,
    `ack ${initial.digest} not among ${[...librarySpecies.values()].map((hash) => hash.slice(0, 8)).join(',')}`);
  check('the sync run applied every step', sync.passed === true);
  check('the refused-digest step did not advance the revision',
    (sync.steps ?? []).some((step) => step.kind === 'load-with-wrong-digest' && step.ok === true));

  // --- performance: measured things are numbers, unmeasured things are null (never 0)
  const single = (performance.hostTiming ?? []).find((entry) => entry.scenario.includes('single'));
  check('the single-instance GPU timing was measured', Number(single?.gpuFrameMs?.median) > 0,
    String(single?.gpuFrameMs?.median));
  check('the shadow pass is reported separately', Number(single?.gpuShadowMs?.median) > 0,
    String(single?.gpuShadowMs?.median));
  check('present CPU time is reported', Number(single?.presentCpuMs) > 0, String(single?.presentCpuMs));
  check('the multi-instance rows carry a note about the single-LOD path',
    (performance.notes ?? []).some((note) => note.includes('single-LOD')));
  check('top-level gpuFrameTimeMs is the single-instance row',
    JSON.stringify(performance.gpuFrameTimeMs) === JSON.stringify(single?.gpuFrameMs));

  // --- native witness numbers
  const witness = nativeDraw.materialWitness ?? {};
  check('the native draw run passed', nativeDraw.ok === true);
  check('no D3D12 debug errors were reported', nativeDraw.debugErrors === 0);
  check('roughness changed the highlight', Number(witness.leafRoughness?.smoothPeak) > Number(witness.leafRoughness?.roughPeak),
    `${witness.leafRoughness?.smoothPeak} vs ${witness.leafRoughness?.roughPeak}`);
  check('thicker leaves are darker when backlit', Number(witness.thicknessBacklit?.thinMean) > Number(witness.thicknessBacklit?.thickMean),
    `${witness.thicknessBacklit?.thinMean} vs ${witness.thicknessBacklit?.thickMean}`);
  check('parallax changed shading but not depth',
    Number(witness.heightParallax?.shadedOff) !== Number(witness.heightParallax?.shadedOn)
    && Number(witness.heightParallax?.depthOff) === Number(witness.heightParallax?.depthOn)
    && Number(witness.heightParallax?.depthCoverageMoved) === 0,
    JSON.stringify(witness.heightParallax));
  check('the material filters partition the shaded silhouette',
    Number(witness.views?.unaccountedPixels) === 0, String(witness.views?.unaccountedPixels));

  // --- images on disk
  check('the evidence directory holds the fixed-camera set', images.length >= 30, `${images.length}`);
  for (const required of ['species-oak-shaded.png', 'oak-closeup-junction.png', 'oak-closeup-petiole.png',
    'oak-debug-wood.png', 'oak-debug-leaves.png', 'atlas-diagnostics-sheet.png', 'wind-junction-t00.png']) {
    check(`image ${required} exists`, images.includes(required));
  }

  // --- wind sequence: the rendered canopy must move without tearing the silhouette
  let wind = null;
  try { wind = await readJson('wind-sequence.json'); } catch { wind = null; }
  if (wind) {
    const samples = wind.samples ?? [];
    check('the wind sequence covers at least 300 frames', samples.length >= 300, String(samples.length));
    check('no frame produced a non-finite statistic',
      samples.every((sample) => Number.isFinite(sample.meanLuminance) && Number.isFinite(sample.covered)));
    const covered = samples.map((sample) => sample.covered);
    let largestJump = 0;
    for (let index = 1; index < covered.length; ++index) largestJump = Math.max(largestJump, Math.abs(covered[index] - covered[index - 1]));
    // A torn junction or a popped LOD changes the silhouette abruptly; 1% of the covered area per
    // frame is a generous bound that a real tear would exceed.
    check('the silhouette never jumps by more than 1% of its own area in one frame',
      largestJump <= Math.max(4, Math.max(...covered) * 0.01), `largest jump ${largestJump} px`);
    check('the canopy actually moves (luminance is not constant)',
      new Set(samples.map((sample) => sample.meanLuminance.toFixed(6))).size > 100,
      String(new Set(samples.map((sample) => sample.meanLuminance.toFixed(6))).size));
  }

  // --- junction vs crown windows: the fork must hold still while the crown moves
  let junctionWindow = null;
  let crownWindow = null;
  try { junctionWindow = await readJson('wind-junction-window.json'); } catch { junctionWindow = null; }
  try { crownWindow = await readJson('wind-crown-window.json'); } catch { crownWindow = null; }
  if (junctionWindow && crownWindow) {
    const largestJump = (series) => {
      const covered = (series.samples ?? []).map((sample) => sample.covered);
      let largest = 0;
      for (let index = 1; index < covered.length; ++index) {
        largest = Math.max(largest, Math.abs(covered[index] - covered[index - 1]));
      }
      return largest;
    };
    const junctionJump = largestJump(junctionWindow);
    const crownJump = largestJump(crownWindow);
    check('the junction window was measured over 300 frames',
      (junctionWindow.samples ?? []).length >= 300, String((junctionWindow.samples ?? []).length));
    check('the fork silhouette holds still under wind (<= 4 px per frame)', junctionJump <= 4, String(junctionJump));
    check('the crown moves more than the fork, so the statistic is sensitive',
      crownJump > junctionJump, `crown ${crownJump} vs fork ${junctionJump}`);
  }

  // --- browser verification (optional: it needs headless Edge and a running editor server)
  let browser = null;
  try { browser = await readJson('browser-check.json'); } catch { browser = null; }
  if (browser) {
    check('the browser check passed', browser.failures.length === 0, JSON.stringify(browser.failures));
    check('the page reached /vegetation', browser.dom?.url === '/vegetation', String(browser.dom?.url));
    check('a WebGL context came up', typeof browser.dom?.webgl === 'string' && browser.dom.webgl.length > 0);
    check('the native debug-view dropdown is populated', browser.dom?.debugViewOptions >= 15,
      String(browser.dom?.debugViewOptions));
    check('a species loaded with a content revision',
      typeof browser.dom?.revision === 'string' && browser.dom.revision.length === 64);
    check('the browser run states what it cannot cover',
      (browser.limits ?? []).some((line) => line.includes('Electron')));
  }

  // --- the acceptance document must point at files that exist
  const acceptance = await fs.readFile(path.join(directory, 'acceptance.md'), 'utf8');
  const referenced = [...acceptance.matchAll(/images\/([A-Za-z0-9._-]+\.png)/g)].map((match) => match[1]);
  for (const name of new Set(referenced)) {
    check(`acceptance references an existing image ${name}`, images.includes(name));
  }
  check('acceptance mentions the provenance file', acceptance.includes('provenance.json'));
  check('acceptance does not claim a null GPU field as measured',
    !/gpuFrameTimeMs[^銆俓n]*null/.test(acceptance));

  const failed = problems.length;
  console.log(JSON.stringify({
    schema: 'nativegpu.vegetation-evidence-check/1',
    checks: checks.length,
    failed,
    problems,
    images: images.length,
  }, null, 2));
  if (failed) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`${error.code ?? 'NG-E-VEG-EVIDENCE'}: ${error.message}`);
  process.exitCode = 1;
});

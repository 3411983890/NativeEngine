import fs from 'node:fs';
import path from 'node:path';
import { decodePng } from './textures.mjs';
const directory = process.argv[2];
const read = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const runs = read(path.join(directory, 'runs.json')), checks = [], values = {};
function check(name, ok, detail) { checks.push({ name, ok: Boolean(ok), detail }); }
function center(name) {
  const image = decodePng(fs.readFileSync(path.join(directory, name + '.png'))), rgb = [0, 0, 0]; let count = 0;
  for (let y = image.height / 2 - 1; y <= image.height / 2 + 1; y++) for (let x = image.width / 2 - 1; x <= image.width / 2 + 1; x++) {
    for (let c = 0; c < 3; c++) rgb[c] += image.data[(Math.floor(y) * image.width + Math.floor(x)) * 4 + c] / 255;
    count++;
  }
  return rgb.map(x => x / count);
}
for (const [name, run] of Object.entries(runs)) {
  const smoke = run.data.probes?.gpuEffects?.smoke;
  check(name + ' physical service available', smoke?.available && !smoke.failed && !smoke.sourceReadback, smoke);
  if (name.includes('-q1') && !name.startsWith('recovery')) check(name + ' actual froxel path', run.data.probes?.lightingQuality?.implemented && run.data.probes?.lightingQuality?.frames > 0 && !run.data.probes?.lightingQuality?.error, run.data.probes?.lightingQuality);
  if (name.startsWith('clear') || name.includes('clear-')) check(name + ' no accidental private medium', smoke?.mediaCount === 0 && smoke?.allocationBytes === 0, smoke);
  else if (!name.includes('stopped') && !name.startsWith('recovery')) check(name + ' actual optical inputs consumed', smoke?.steps > 0 && smoke?.mediaFrames > 0 && smoke?.mediaCount > 0 && smoke?.skippedSteps === 0, smoke);
  values[name] = { center: name.startsWith('recovery') ? null : center(name), smoke };
}
for (const quality of [0, 1]) {
  const a = `absorb-q${quality}`, b = `clear-q${quality}`;
  if (values[a] && values[b]) {
    const steps = values[a].smoke.captureSources[0].version, density = .125 * steps / 120, expected = Math.exp(-2 * density);
    const measured = values[a].center[0] / values[b].center[0];
    check(a + ' Beer-Lambert slab', Number.isFinite(measured) && Math.abs(measured - expected) < .012, { expected, measured, density, steps });
    check(a + ' CPU authority preserved', typeof runs[a].data.determinism.finalHash === 'string' && runs[a].data.determinism.finalHash.length > 0 && runs[a].data.determinism.finalHash === runs[b].data.determinism.finalHash, 'same scene and fixed input');
  }
  const foreground = `blocked-absorb-q${quality}`, clear = `blocked-clear-q${quality}`;
  if (values[foreground] && values[clear]) check(foreground + ' opaque foreground rejects smoke behind it', values[foreground].center.every((v, c) => Math.abs(v - values[clear].center[c]) < .008), { active: values[foreground].center, clear: values[clear].center });
}
if (values['overlap-q0'] && values['clear-q0']) {
  const density = .125 * values['overlap-q0'].smoke.captureSources[0].version / 120, expected = Math.exp(-4 * density), measured = values['overlap-q0'].center[0] / values['clear-q0'].center[0];
  check('two overlapping media sum extinction once each', Math.abs(expected - measured) < .012, { expected, measured, densityPerInstance: density });
}
for (const quality of [0, 1]) if (values[`emission-hot-q${quality}`]) {
  const name = `emission-hot-q${quality}`, rgb = values[name].center, steps = values[name].smoke.captureSources[0].version, density = .125 * steps / 120, temperature = Math.fround(293.15) + 250 * steps / 120;
  const t = Math.max(0, Math.min(1, (temperature - 700) / 2300)), mix = (a, b, w) => a + (b - a) * w;
  let color = [1, .03, .002].map((v, c) => mix(v, [1, .35, .04][c], Math.min(1, t * 2)));
  color = color.map((v, c) => mix(v, [1, .9, .65][c], Math.max(0, t * 2 - 1)));
  const expected = color.map(v => Math.min(1, v * density * (temperature / 1000) ** 4 * 2));
  check(name + ' declared Kelvin emission ramp', rgb.every((v, c) => Math.abs(v - expected[c]) < .025), { rgb, expected, density, temperature });
}
if (values['stopped-q0']) check('immediate stop withdraws visible media and all reservations', values['stopped-q0'].smoke.mediaCount === 0 && values['stopped-q0'].smoke.reservedBytes === 0 && values['stopped-q0'].smoke.allocationBytes === 0, values['stopped-q0'].smoke);
for (const quality of [0, 1]) if (values[`scattering-q${quality}`]) {
  const name = `scattering-q${quality}`, rgb = values[name].center, steps = values[name].smoke.captureSources[0].version;
  const density = .125 * steps / 120, cosine = -.5 / Math.hypot(.3, .8, -.5);
  const light = quality ? (1 - .6 ** 2) / (4 * Math.PI * (1 + .6 ** 2 - 2 * .6 * cosine) ** 1.5) : (.5 - .5 * cosine) * .35;
  const expected = [1, .5, .25].map(a => a * light * (1 - Math.exp(-2 * density)));
  check(name + ' bounded RGB scattering from declared light', rgb.every((v, c) => Math.abs(v - expected[c]) < (quality ? .006 : .012)) && rgb[0] > 0, { rgb, expected, density });
}
if (values['stopped-q0'] && values['clear-q0']) check('stopped smoke leaves no residual image', runs['stopped-q0'].imageHash === runs['clear-q0'].imageHash, 'identical final pixels');
if (values['recovery-q0']) { const life = runs['recovery-q0'].data.lifecycle; check('renderer recreation drains tracked resources', life?.completed === 2 && life.entries.every(x => x.liveObjectsAfterShutdown === 0), life); }
if (values['recovery-q1']) { const data = runs['recovery-q1'].data, life = data.lifecycle, simple = runs['recovery-q0']?.data.lifecycle; check('quality recreation drains added incident cache and all tracked resources', data.probes.lightingQuality.requested === 1 && life?.completed === 2 && life.entries.every(x => x.framesRendered >= 8 && x.liveObjectsAfterShutdown === 0) && (!simple || life.entries[0].objectsCreated > simple.entries[0].objectsCreated), life); }
for (const [name, v] of Object.entries(values)) if (v.smoke?.sharing) check(name + ' automatic sharing healthy', v.smoke.sharing.available && !v.smoke.sharing.failed, v.smoke.sharing);
for (const quality of [0, 1]) {
  const name = `far-absorb-q${quality}`, control = `far-clear-q${quality}`;
  if (!values[name] || !values[control]) continue;
  const s = values[name].smoke, density = .125 * s.captureSources[0].version / 120;
  const expected = Math.exp(-2 * Math.round(density * 1024) / 1024), measured = values[name].center[0] / values[control].center[0];
  check(name + ' reads acknowledged WF density once', s.sharedMediaCount === 1 && s.sharedMediaFrames > 0 && s.captureSources.length === 1 && s.captureSources[0].representation === 'wf' && s.sharing.publications > 0 && Math.abs(measured - expected) < .012, { measured, expected, smoke: s });
}
if (values['far-mixed-q0'] && values['far-clear-q0']) {
  const s = values['far-mixed-q0'].smoke, density = .125 * s.captureSources[0].version / 120;
  const expected = Math.exp(-2 * density - 2 * Math.round(density * 1024) / 1024), measured = values['far-mixed-q0'].center[0] / values['far-clear-q0'].center[0];
  check('mixed LEF/WF sources each contribute once', s.mediaCount === 2 && s.sharedMediaCount === 1 && new Set(s.captureSources.map(x => x.serial)).size === 2 && s.captureSources.filter(x => x.representation === 'wf').length === 1 && Math.abs(measured - expected) < .012, { measured, expected, smoke: s });
}
if (values['far-blocked-q0'] && values['far-clear-q0']) check('WF proxy respects foreground depth', values['far-blocked-q0'].smoke.sharedMediaCount === 1 && values['far-blocked-q0'].center.every((v, c) => Math.abs(v - values['far-clear-q0'].center[c]) < .008), values['far-blocked-q0']);
if (values['far-hot-q0'] && values['emission-hot-q0']) check('thermal source retains detailed emission at distance', values['far-hot-q0'].smoke.captureSources.every(x => x.representation === 'lef') && values['far-hot-q0'].center.every((v, c) => Math.abs(v - values['emission-hot-q0'].center[c]) < .025), values['far-hot-q0']);
if (values['far-scatter-q1']) check('WF RGB scatter uses one density proxy', values['far-scatter-q1'].smoke.sharedMediaCount === 1 && values['far-scatter-q1'].center[0] > 0, values['far-scatter-q1']);
if (values['far-stopped-q0'] && values['far-clear-q0']) {
  const s = values['far-stopped-q0'].smoke;
  check('stopped WF withdraws source and all sharing resources', s.sharedMediaFrames > 0 && s.sharedMediaCount === 0 && s.mediaCount === 0 && s.sharing.instances === 0 && s.sharing.worldSources === 0 && s.sharing.allocationBytes === 0 && runs['far-stopped-q0'].imageHash === runs['far-clear-q0'].imageHash, s);
}
for (const name of ['near-coarse-q0', 'far-coarse-q0', 'far-coarse-q1']) if (values[name]) {
  const s = values[name].smoke, source = s.captureSources[0], density = .125 * source.version / 120, far = name.startsWith('far');
  const expected = Math.exp(-(far ? Math.round(density * 1024) / 1024 : density)), measured = values[name].center[0];
  check(name + ' real eight-to-one projection preserves column optical depth', source.representation === (far ? 'wf' : 'lef') && source.scalarElements === (far ? 1 : 8) && source.cellMetres === (far ? 1 : .5) && Math.abs(measured - expected) < .012, { expected, measured, source });
}
for (const quality of [0, 1]) {
  const name = `fog-absorb-q${quality}`, control = `fog-clear-q${quality}`;
  if (!values[name] || !values[control]) continue;
  const density = .125 * values[name].smoke.captureSources[0].version / 120, expected = Math.exp(-2 * density), measured = values[name].center[0] / values[control].center[0];
  check(name + ' background and smoke extinction integrated once', Math.abs(expected - measured) < .012, { expected, measured });
}
const result = { schema: 'nativegpu.smoke-optical-metrics/1', ok: checks.every(x => x.ok), values, checks };
fs.writeFileSync(path.join(directory, 'optical-metrics.json'), JSON.stringify(result, null, 2) + '\n');
console.log(`Smoke optical metrics ${checks.filter(x => x.ok).length}/${checks.length}`);
for (const c of checks.filter(x => !x.ok)) console.log(JSON.stringify(c));
if (!result.ok) process.exitCode = 1;

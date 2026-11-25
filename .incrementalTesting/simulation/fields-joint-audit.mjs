import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

// Evidence-only audit. Does not start a build, GPU workload or regression.
const filenames = process.argv.slice(2);
if (filenames.length !== 4) throw Error('Expected world, joint, deposit-service and smoke-sharing reports');
const parse = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const matches = (file, expected) => fs.existsSync(file) && hash(file) === expected.toLowerCase();
const checks = [], receipts = [], currentChanges = {};
const check = (name, ok) => checks.push({ name, ok: !!ok });
let frozenComparisons = 0, currentComparisons = 0;
for (const file of filenames) {
  const report = parse(file), directory = path.dirname(file), build = report.build;
  check(file + ' incremental result', report.ok && report.fullRegressionRun === false);
  check(file + ' actual hardware debug result', report.results?.ok && report.results.debugLayer && report.results.debugErrors === 0);
  const executable = fs.readdirSync(directory).find(name => name.endsWith('_tests.exe'));
  check(file + ' frozen executable', !!executable && matches(path.join(directory, executable), build.binaryHash));
  const changes = [];
  for (const [source, expected] of Object.entries(build.sources)) {
    check(file + ' frozen source ' + source, matches(path.join(directory, 'source-snapshot', source), expected));
    ++frozenComparisons;
    if (!matches(source, expected)) changes.push(source);
    ++currentComparisons;
  }
  check(file + ' current dependency inputs', changes.length === 0);
  currentChanges[file] = changes;
  if (report.buildSnapshotFrozenBeforeRun) check(file + ' unchanged during frozen execution', report.sourceChangesAfterFreeze.length === 0);
  receipts.push({ file, binaryHash: build.binaryHash, frozenBeforeRun: !!report.buildSnapshotFrozenBeforeRun, groups: report.results.groups, checks: report.results.checks, comparisons: report.results.comparisons ?? 0 });
}
const [world, joint, deposit, smoke] = filenames.map(parse);
check('world joint rollback and foreign-writer cases exercised', world.results.groups >= 17 && ['joint-controller-transaction', 'joint-publication-ownership'].every(name => world.results.cases.some(entry => entry.case === name)));
check('actual particles and solved smoke in one typed world', joint.results.oneVisualWorld && joint.results.distinctTypedChannels && joint.results.comparisons >= 14 && joint.results.runtimeReadback === false);
check('original production deposit service regression', deposit.results.groups >= 27 && deposit.results.checks >= 432 && deposit.results.fieldReadback === false);
check('original automatic smoke sharing regression', smoke.results.groups >= 6 && smoke.results.checks >= 240 && smoke.results.comparisons >= 16);
// Compare source shader bytes to the accepted pre-ownership-guard increment.
const baselineFile = 'reports/0.11.0/31c00bc53c0b/20261026T071029Z/fields-joint-gpu-evidence.json';
const baseline = parse(baselineFile);
for (const [source, expected] of Object.entries(baseline.build.sources)) if (source.startsWith('shaders/')) {
  check('retained numerical shader input ' + source, matches(source, expected));
  check('retained baseline frozen shader ' + source, matches(path.join(path.dirname(baselineFile), 'source-snapshot', source), expected));
}
const version = fs.readFileSync('VERSION', 'utf8').trim();
const commit = execFileSync('git', ['rev-parse', '--short=12', 'HEAD'], { encoding: 'utf8' }).trim();
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
const directory = path.join('reports', version, commit, stamp + '-fields-joint-final');
fs.mkdirSync(directory, { recursive: true });
fs.copyFileSync('.incrementalTesting/simulation/fields-joint-audit.mjs', path.join(directory, 'audit.mjs'));
const result = {
  schema: 'nativegpu.fields-joint-finalization/1', ok: checks.every(entry => entry.ok),
  checks, frozenComparisons, currentComparisons, receipts, currentChanges,
  auditToolHash: hash('.incrementalTesting/simulation/fields-joint-audit.mjs'), shaderBaseline: baselineFile,
  productionServicesUnified: false, currentWholeHostValidated: false, fullRegressionRun: false,
  scope: 'One visual WF joint-publication backend with real GPU particle/smoke sources, ownership/rollback boundaries and existing service regressions. Production service phase split, renderer migration, CPU authority and remaining A-G goals are pending.',
};
const output = path.join(directory, 'fields-joint-finalization.json');
fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ ok: result.ok, checks: checks.length, frozenComparisons, currentComparisons, currentChanges, output }));
if (!result.ok) { console.log(JSON.stringify(checks.filter(entry => !entry.ok))); process.exitCode = 1; }

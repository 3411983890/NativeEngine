import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

// Read-only provenance inspection; never builds or runs tests.
const [nativeFile, hostFile, failureFile] = process.argv.slice(2);
if (!nativeFile || !hostFile || !failureFile || process.argv.length !== 5) throw Error('Expected native service, host delta and preserved failure reports');
const parse = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const matches = (file, expected) => fs.existsSync(file) && hash(file) === expected.toLowerCase();
const checks = [], receipts = [], currentChanges = {};
const check = (name, ok) => checks.push({ name, ok: !!ok });
let frozenComparisons = 0;
for (const file of [nativeFile, hostFile]) {
  const report = parse(file), directory = path.dirname(file), host = !!report.hostBuild, build = report.hostBuild ?? report.build;
  check(file + ' scoped acceptance', report.ok && report.fullRegressionRun === false);
  check(file + ' frozen executable', matches(path.join(directory, host ? 'NativeGPU.exe' : 'native_effect_deposit_service_tests.exe'), build.binaryHash));
  const changes = [];
  for (const [source, expected] of Object.entries(build.sources)) {
    const relative = path.isAbsolute(source) ? path.relative(process.cwd(), source) : source;
    check(file + ' frozen source ' + relative, matches(path.join(directory, host ? 'host-source-snapshot' : 'source-snapshot', relative), expected));
    ++frozenComparisons;
    if (!matches(source, expected)) changes.push(relative);
  }
  currentChanges[file] = changes;
  if (!host) check(file + ' native sources still current', changes.length === 0);
  if (host) {
    check(file + ' host binary/source frozen before execution', report.buildSnapshotFrozenBeforeRuns);
    for (const [source, expected] of Object.entries(report.inputs)) {
      check(file + ' frozen fixture ' + source, matches(path.join(directory, 'asset-source-snapshot', source), expected));
      ++frozenComparisons;
    }
  }
  receipts.push({ file, binaryHash: build.binaryHash, frozenBeforeExecution: !!report.buildSnapshotFrozenBeforeRuns, currentInputChanges: changes });
}
const native = parse(nativeFile), host = parse(hostFile), failure = parse(failureFile);
check('production service has shared publication scene', native.results.groups >= 28 && native.results.checks >= 475 && native.results.cases.some(entry => entry.case === 'phased-shared-world-service'));
check('production service actual hardware', native.results.ok && native.results.debugLayer && native.results.debugErrors === 0);
check('host delta has exactly two transaction scenes and eight accepted checks', host.transactionDeltaOnly && Object.keys(host.runs).length === 2 && host.checks.length === 8 && host.checks.every(entry => entry.ok));
for (const [name, run] of Object.entries(host.runs)) {
  const validation = run.data.probes.renderTargets;
  check(name + ' actual hardware error count', run.exit === 0 && run.data.success && validation.validationLayer && validation.validationErrors === 0);
  check(name + ' retained screenshot', matches(path.join(path.dirname(hostFile), name + '.png'), run.imageHash));
}
for (const source of ['engine/native/effects/field_deposits.cpp', 'engine/native/effects/field_deposits.hpp', 'engine/native/fields/gpu_sharing.cpp', 'engine/native/fields/gpu_world.cpp']) {
  const expected = host.hostBuild.sources[path.resolve(source)];
  check('host uses current phase implementation ' + source, !!expected && matches(source, expected));
}
check('fixture failure preserved without rewriting as success', failure.ok === false && failure.sourceMatchesFailedBuild && failure.binaryMatches && matches(path.join(path.dirname(failureFile), 'native_effect_deposit_service_tests.exe'), failure.build.binaryHash));
const version = fs.readFileSync('VERSION', 'utf8').trim();
const commit = execFileSync('git', ['rev-parse', '--short=12', 'HEAD'], { encoding: 'utf8' }).trim();
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
const directory = path.join('reports', version, commit, stamp + '-deposit-phases-final');fs.mkdirSync(directory, { recursive: true });
fs.copyFileSync('.incrementalTesting/simulation/field-service-phase-audit.mjs', path.join(directory, 'audit.mjs'));
const result = { schema: 'nativegpu.deposit-phases-finalization/1', ok: checks.every(entry => entry.ok), checks, frozenComparisons, receipts, currentChanges, preservedFailure: failureFile, auditToolHash: hash('.incrementalTesting/simulation/field-service-phase-audit.mjs'), currentHostInputsMatch: currentChanges[hostFile].length === 0, sharedSmokeProductionMigrated: false, jointProductionHostIntegrated: false, fullRegressionRun: false, scope: 'Phased production particle deposition in external typed WF, shared rollback and global demand, original service cases plus two independent-host lifecycle scenes. Smoke production phase split, joint host scheduling, CPU authority and complete A-G remain pending.' };
const output = path.join(directory, 'deposit-phases-finalization.json');fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ ok: result.ok, checks: checks.length, frozenComparisons, currentChanges, output }));
if (!result.ok) { console.log(JSON.stringify(checks.filter(entry => !entry.ok))); process.exitCode = 1; }

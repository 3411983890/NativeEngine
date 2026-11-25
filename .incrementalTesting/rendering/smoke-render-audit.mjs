import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

// Read-only provenance audit: does not build or run any regression.
const root = process.cwd();
const base = 'reports/0.11.0/31c00bc53c0b/';
const receipts = {
  host: base + '20261026T002737Z/smoke-host-evidence.json',
  deposit: base + '20261026T002855Z/effects-deposit-host-evidence.json',
  service: base + '20261026T001107Z/smoke-service-evidence.json',
};
const read = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const checks = [], currentChanges = {};
let frozenChecks = 0;
const check = (name, ok, detail) => checks.push({ name, ok: !!ok, ...(detail === undefined ? {} : { detail }) });
for (const [kind, filename] of Object.entries(receipts)) {
  const report = read(filename), directory = path.dirname(filename), build = report.hostBuild ?? report.build;
  check(kind + ' receipt passed', report.ok && !report.fullRegressionRun);
  const binary = kind === 'service' ? 'native_smoke_service_tests.exe' : 'NativeGPU.exe';
  check(kind + ' frozen executable', hash(path.join(directory, binary)) === build.binaryHash.toLowerCase());
  const changed = [];
  for (const [source, expected] of Object.entries(build.sources)) {
    const relative = path.isAbsolute(source) ? path.relative(root, source) : source;
    const frozen = path.join(directory, kind === 'service' ? 'source-snapshot' : 'host-source-snapshot', relative);
    check(kind + ' frozen source ' + relative, fs.existsSync(frozen) && hash(frozen) === expected.toLowerCase());
    ++frozenChecks;
    if (!fs.existsSync(source) || hash(source) !== expected.toLowerCase()) changed.push(source);
  }
  if (report.inputs) for (const [source, expected] of Object.entries(report.inputs)) {
    const frozen = path.join(directory, 'asset-source-snapshot', source);
    check(kind + ' frozen fixture ' + source, fs.existsSync(frozen) && hash(frozen) === expected.toLowerCase());
    ++frozenChecks;
  }
  currentChanges[kind] = changed;
}
const host = read(receipts.host), deposit = read(receipts.deposit), service = read(receipts.service);
check('same frozen host binary for field delta', host.hostBuild.binaryHash === deposit.hostBuild.binaryHash);
check('16 actual host cases', Object.keys(host.cases).length === 16 && host.checks.every(c => c.ok));
check('50 optical checks', host.optical.checks.length === 50 && host.optical.checks.every(c => c.ok));
check('8 field transaction delta checks', deposit.checks.length === 8 && deposit.checks.every(c => c.ok));
check('service scalar/asset checks', service.results.groups === 7 && service.results.checks === 694 && service.results.comparisons === 576 && service.assetTests.tests === 14);
const capture = host.sourceCapture, workspaceChanges = [];
for (const [source, expected] of Object.entries(capture.sources)) {
  check('capture ' + source, hash(path.join(capture.snapshotRoot, source)) === expected);
  ++frozenChecks;
  if (!fs.existsSync(source) || hash(source) !== expected) workspaceChanges.push(source);
}
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
const version = fs.readFileSync('VERSION', 'utf8').trim();
const commit = execFileSync('git', ['rev-parse', '--short=12', 'HEAD'], { encoding: 'utf8' }).trim();
const directory = path.join('reports', version, commit, stamp + '-smoke-render-final');
fs.mkdirSync(directory, { recursive: true });
const result = {
  schema: 'nativegpu.smoke-render-finalization/1', ok: checks.every(c => c.ok), receipts,
  frozenChecks, checks, currentChanges, workspaceChangesSinceCapture: workspaceChanges,
  currentWholeHostValidated: workspaceChanges.length === 0,
  fullRegressionRun: false,
  scope: 'Audit of retained smoke optics, GPU service and field transaction evidence. Frozen build only; concurrent workspace differences are not claimed validated. No new tests run, no full D or A-G acceptance.',
};
const output = path.join(directory, 'smoke-render-finalization.json');
fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ ok: result.ok, frozenChecks, checks: checks.length, workspaceChanges, output }));
if (!result.ok) { console.log(JSON.stringify(checks.filter(c => !c.ok))); process.exitCode = 1; }

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

// Provenance only: never builds or executes a regression.
const filenames = process.argv.slice(2);
if (filenames.length !== 3) throw Error('Expected smoke-fields, fields-local-gpu and fields-world-gpu report paths');
const parse = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const checks = [], changes = [], receipts = [];
const check = (name, ok) => checks.push({ name, ok: !!ok });
let frozenComparisons = 0, currentComparisons = 0;
for (const filename of filenames) {
  const r = parse(filename), directory = path.dirname(filename), manifest = r.build;
  check(filename + ' passed scoped evidence', r.ok && r.results.ok && r.results.debugLayer && r.results.debugErrors === 0 && !r.fullRegressionRun);
  const identity = parse(path.join(directory, 'inputs.json'));
  check(filename + ' manifest identity', JSON.stringify(identity) === JSON.stringify(manifest));
  const executable = fs.readdirSync(directory).find(p => p.endsWith('_tests.exe'));
  check(filename + ' frozen binary', !!executable && hash(path.join(directory, executable)) === manifest.binaryHash.toLowerCase());
  for (const [source, expected] of Object.entries(manifest.sources)) {
    const frozen = path.join(directory, 'source-snapshot', source);
    check(filename + ' frozen ' + source, fs.existsSync(frozen) && hash(frozen) === expected.toLowerCase());++frozenComparisons;
    ++currentComparisons;
    if (!fs.existsSync(source) || hash(source) !== expected.toLowerCase()) changes.push({ report: filename, source });
  }
  receipts.push({ filename, binaryHash: manifest.binaryHash, groups: r.results.groups, checks: r.results.checks, comparisons: r.results.comparisons ?? r.results.cellComparisons, frozenBeforeRun: !!r.buildSnapshotFrozenBeforeRun });
}
check('scoped inputs match current workspace', changes.length === 0);
const capturedRoot = 'build/smoke-snapshots/20261026T002217Z/';
const previous = parse(capturedRoot + 'shaders/manifest.json').blobs, current = parse('shaders/manifest.json').blobs;
const added = current.filter(a => !previous.some(b => a.symbol === b.symbol));
check('one additional scalar import shader', added.length === 1 && added[0].symbol === 'kLocalFieldImportScalar');
check('all previous shader definitions preserved', previous.every(b => JSON.stringify(b) === JSON.stringify(current.find(a => a.symbol === b.symbol))));
const normalize = p => fs.readFileSync(p, 'utf8').replaceAll('\r\n', '\n');
const oldShader = normalize(capturedRoot + 'shaders/field_local.hlsl'), shader = normalize('shaders/field_local.hlsl');
const start = shader.indexOf('[numthreads(64,1,1)]\nvoid CSImportScalar('), end = shader.indexOf('groupshared uint4 groupMetric', start);
check('previous field numerical kernels preserved', start >= 0 && end > start && shader.slice(0, start) + shader.slice(end) === oldShader);
const oldSolver = normalize(capturedRoot + 'engine/native/smoke/gpu.cpp');
const solver = normalize('engine/native/smoke/gpu.cpp').replace('ID3D12Fence* GpuSmokeFireSolver::timeline() const{return impl_?impl_->timeline.p:nullptr;}\n', '');
check('smoke numerical backend unchanged except timeline accessor', solver === oldSolver);
const version = fs.readFileSync('VERSION', 'utf8').trim(), commit = execFileSync('git', ['rev-parse', '--short=12', 'HEAD'], { encoding: 'utf8' }).trim();
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
const directory = path.join('reports', version, commit, stamp + '-smoke-fields-final');fs.mkdirSync(directory, { recursive: true });
const result = {
  schema: 'nativegpu.smoke-fields-finalization/1', ok: checks.every(c => c.ok), receipts,
  frozenComparisons, currentComparisons, checks, changes, shaderCount: current.length,
  preservedFailures: ['20261026T004104Z', '20261026T004215Z', '20261026T004453Z'],
  fullRegressionRun: false,
  scope: 'Scoped source/binary/retained regression audit only. New bridge freezes before execution; prior field gates freeze after execution with build input checks. No current whole-host, renderer coverage, full B/D or A-G acceptance.',
};
const output = path.join(directory, 'smoke-fields-finalization.json');fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ ok: result.ok, frozenComparisons, currentComparisons, checks: checks.length, changes, output }));
if (!result.ok) { console.log(JSON.stringify(checks.filter(c => !c.ok))); process.exitCode = 1; }

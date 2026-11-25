import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

// Read-only evidence audit; never starts a build or a regression.
const files = process.argv.slice(2);
if (files.length !== 5) throw Error('Expected sharing, service, base host, coarse host and field transaction reports');
const root = process.cwd(), parse = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const checks = [], receipts = [], currentChanges = {};
const check = (name, ok) => checks.push({ name, ok: !!ok });
let frozenComparisons = 0;
for (const filename of files) {
  const r = parse(filename), directory = path.dirname(filename), build = r.hostBuild ?? r.build, host = !!r.hostBuild;
  check(filename + ' scoped result', r.ok && !r.fullRegressionRun);
  if (r.results) check(filename + ' actual GPU validation', r.results.ok && r.results.debugLayer && r.results.debugErrors === 0);
  const executable = host ? 'NativeGPU.exe' : fs.readdirSync(directory).find(p => p.endsWith('_tests.exe'));
  check(filename + ' frozen executable', hash(path.join(directory, executable)) === build.binaryHash.toLowerCase());
  const changes = [];
  for (const [source, expected] of Object.entries(build.sources)) {
    const relative = path.isAbsolute(source) ? path.relative(root, source) : source;
    const frozen = path.join(directory, host ? 'host-source-snapshot' : 'source-snapshot', relative);
    check(filename + ' frozen source ' + relative, fs.existsSync(frozen) && hash(frozen) === expected.toLowerCase());++frozenComparisons;
    if (!fs.existsSync(source) || hash(source) !== expected.toLowerCase()) changes.push(source);
  }
  if (r.inputs) for (const [source, expected] of Object.entries(r.inputs)) {
    const frozen = path.join(directory, 'asset-source-snapshot', source);
    check(filename + ' frozen fixture ' + source, fs.existsSync(frozen) && hash(frozen) === expected.toLowerCase());++frozenComparisons;
  }
  if (!host) check(filename + ' native inputs still current', changes.length === 0);
  if (r.sourceCapture) for (const [source, expected] of Object.entries(r.sourceCapture.sources)) {
    check(filename + ' captured source ' + source, hash(path.join(r.sourceCapture.snapshotRoot, source)) === expected);++frozenComparisons;
  }
  currentChanges[filename] = changes;
  receipts.push({ filename, binaryHash: build.binaryHash, frozenBeforeRun: !!(r.buildSnapshotFrozenBeforeRun || r.buildSnapshotFrozenBeforeRuns), groups: r.results?.groups, checks: r.results?.checks ?? r.checks.length, opticalChecks: r.optical?.checks.length });
}
const [native, service, base, coarse, fields] = files.map(parse);
check('six sharing groups and real comparisons', native.results.groups === 6 && native.results.checks === 240 && native.results.comparisons === 16);
check('original service contract retained', service.results.groups === 7 && service.results.checks === 694 && service.assetTests.tests === 14);
check('base 25 cases and optical checks', Object.keys(base.cases).length === 25 && base.optical.checks.length === 102 && base.optical.checks.every(c => c.ok) && base.checks.every(c => c.ok));
check('33 final optical cases after incident-cache refinement', Object.keys(coarse.cases).length === 33 && coarse.optical.checks.every(c => c.ok) && coarse.checks.every(c => c.ok));
check('field transaction delta uses final frozen host', fields.hostBuild.binaryHash === coarse.hostBuild.binaryHash && fields.checks.length === 8 && fields.checks.every(c => c.ok));
// A real small-volume failure required a quality-light cache refinement. The
// final 33-case run covers that change; sharing and compatibility code retain
// their original contracts apart from added capture telemetry.
const critical = ['renderer/gpu_effect_bridge.cpp', 'renderer/gpu_smoke_bridge.cpp', 'renderer/smoke_medium.hpp', 'renderer/lighting_quality.h', 'shaders/smoke_medium.hlsli', 'shaders/lighting_quality_common.hlsli', 'engine/native/smoke/sharing.cpp', 'engine/native/smoke/sharing.hpp', 'engine/native/smoke/service.cpp', 'engine/native/smoke/service.hpp'];
const normalize = (capture, file) => fs.readFileSync(path.join(capture.snapshotRoot, file), 'utf8').replaceAll('\r\n', '\n')
  .replace(/^.*std::ostringstream capture;.*$/m, '// Capture JSON fields')
  .replace(/^struct SmokeObservation .*$/m, '// Capture record')
  .replace(/smokeFrameSources\.push_back\(\{[^;]*\}\);/, 'smokeFrameSources.push_back(CAPTURE_METADATA);');
for (const file of critical) check('retained sharing/medium-data input ' + file, normalize(base.sourceCapture, file) === normalize(coarse.sourceCapture, file));
for (const file of [...critical,'renderer/lighting_quality.cpp','shaders/lighting_quality.hlsl','shaders/composite_media.hlsl']) check('current smoke entry file ' + file, hash(file) === coarse.sourceCapture.sources[file]);
const currentHeader=fs.readFileSync('renderer/d3d12_renderer_internal.h','utf8'),currentRenderer=fs.readFileSync('renderer/d3d12_renderer.cpp','utf8');
check('current renderer retains smoke service and appended light-cache descriptors',currentHeader.includes('gpuSmokeSharing;')&&currentHeader.includes('qualityFogIncident;')&&currentHeader.includes('DescriptorHeapSlots = SmokeIncidentSrv + 2;'));
const frame=currentRenderer.slice(currentRenderer.indexOf('bool Renderer::render('));
check('current smoke preparation precedes quality lighting',frame.indexOf('recordGpuEffects(')>=0&&frame.indexOf('recordGpuEffects(')<frame.indexOf('prepareSmokeMedia(')&&frame.indexOf('prepareSmokeMedia(')<frame.indexOf('prepareQuality(')&&frame.indexOf('finishSmokeMedia(')>frame.indexOf('compositeQuality('));
check('current DXBC fallback retains linear fog fix',currentRenderer.includes('toneMap==0&&(volumetric&1u)==0')&&currentRenderer.includes('((volumetric&3u)!=0?saturate(color)'));
check('final quality refinement is real code', fs.readFileSync(path.join(coarse.sourceCapture.snapshotRoot,'shaders/lighting_quality.hlsl'),'utf8').includes('float4 SmokeQualityPrefix('));
const workspaceChanges = [];
for (const [source, expected] of Object.entries(coarse.sourceCapture.sources)) if (!fs.existsSync(source) || hash(source) !== expected) workspaceChanges.push(source);
const version = fs.readFileSync('VERSION', 'utf8').trim(), commit = execFileSync('git', ['rev-parse', '--short=12', 'HEAD'], { encoding: 'utf8' }).trim();
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
const directory = path.join('reports', version, commit, stamp + '-smoke-sharing-final');fs.mkdirSync(directory, { recursive: true });
fs.copyFileSync('.incrementalTesting/simulation/smoke-sharing-audit.mjs',path.join(directory,'audit.mjs'));
const result = { preservedFailures:['20261026T055239Z','20261026T060711Z','20261026T061615Z-fc463c/smoke-host-guard-rejection.json'], auditToolHash:hash('.incrementalTesting/simulation/smoke-sharing-audit.mjs'), schema: 'nativegpu.smoke-sharing-finalization/1', ok: checks.every(c => c.ok), checks, frozenComparisons, receipts, currentChanges, workspaceChangesSinceFinalCapture: workspaceChanges, currentWholeHostValidated: false, fullRegressionRun: false, scope: 'Native automatic-sharing/service results, retained 25-case baseline plus 33 final optical cases after a real small-volume failure and froxel-incident/pixel-integration refinement; retained field transaction delta. Frozen hosts only; unrelated concurrent workspace changes and full B/D/A-G remain outside acceptance.' };
const output = path.join(directory, 'smoke-sharing-finalization.json');fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ ok: result.ok, checks: checks.length, frozenComparisons, workspaceChanges, output }));
if (!result.ok) { console.log(JSON.stringify(checks.filter(c => !c.ok))); process.exitCode = 1; }

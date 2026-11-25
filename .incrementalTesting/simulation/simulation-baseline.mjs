import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { mkdir, readdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
async function files(directory) {
  const entries = await readdir(path.join(root, directory), { withFileTypes: true });
  const groups = await Promise.all(entries.map((entry) => {
    const relative = `${directory}/${entry.name}`;
    return entry.isDirectory() ? files(relative) : [relative];
  }));
  return groups.flat();
}
const inputs = (await Promise.all(['engine', 'renderer', 'host', 'shaders', 'tools', '.incrementalTesting', 'tests', 'editor']
  .map(files))).flat().filter((name) => /\.(cpp|hpp|h|c|hlsl|hlsli|mjs|json|ps1|html|css)$/.test(name)).sort();
const sources = Object.fromEntries(await Promise.all(inputs.map(async (name) =>
  [name, hash(await readFile(path.join(root, name)))])));
const identity = {};
for (const [name, args] of [['commit', ['rev-parse', 'HEAD']], ['changes', ['status', '--porcelain']]]) {
  try { identity[name] = execFileSync('git', args, { cwd: root, encoding: 'utf8', windowsHide: true }).trim(); }
  catch { identity[name] = 'unavailable'; }
}
let builtSources = {};
try { builtSources = JSON.parse((await readFile(path.join(root, 'build/source-inputs.json'), 'utf8')).replace(/^\uFEFF/, '')); }
catch { /* Missing build provenance is explicitly reported below. */ }
const mismatches = [];
for (const [absolute, expected] of Object.entries(builtSources)) {
  try { if (hash(await readFile(absolute)).toUpperCase() !== expected.toUpperCase()) mismatches.push(absolute); }
  catch { mismatches.push(absolute); }
}
const declared = new Set(Object.keys(builtSources).map((name) => path.resolve(name).toLowerCase()));
const unlistedNativeInputs = Object.keys(sources).filter((name) =>
  /^(engine\/native|renderer|host|shaders)\//.test(name) && /\.(cpp|hpp|h|hlsl|hlsli|json)$/.test(name)
  && !declared.has(path.resolve(root, name).toLowerCase()));
const report = {
  schema: 'nativegpu.simulation-baseline/1', capturedAtUtc: new Date().toISOString(),
  version: (await readFile(path.join(root, 'VERSION'), 'utf8')).trim(), node: process.version,
  identity, sourceFingerprint: hash(JSON.stringify(sources)), sources,
  build: { manifestPresent: Object.keys(builtSources).length > 0, mismatches, unlistedNativeInputs },
  scope: { particles: 'CPU effect instances + GPU unlit sprites; independent GPU per-emitter SoA/alive-dead/indirect simulation core; GPU asset instance manager and renderer bridge pending; legacy dense smoke/fire retained',
    fields: 'CPU regional/source/LEF reference; D3D12 immutable sparse read snapshots, Q8 trilinear and budgeted read halos; solver writes and game integration pending',
    cloth: 'CPU XPBD reference, cooked assets and spatial wind field adapter; GPU solver/render/host pending',
    water: 'CPU analytic/shallow/spectrum-sum reference and source-tagged authority query projection; PBF/GPU/render/host pending' },
  tests: { rerunByThisCapture: false, notes: 'Build/test logs are separate evidence, not inferred from file hashes.' },
};
const stamp = report.capturedAtUtc.replace(/[:.]/g, '-');
const directory = path.join(root, 'reports', report.version, String(identity.commit).slice(0, 12), stamp, 'simulation');
await mkdir(directory, { recursive: true });
const destination = path.join(directory, 'baseline.json');
await writeFile(destination, `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify({ path: destination, fingerprint: report.sourceFingerprint,
  buildManifestPresent: report.build.manifestPresent, buildMismatches: mismatches.length,
  unlistedNativeInputs: unlistedNativeInputs.length }));

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const root = process.cwd();
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
const relative = `build/smoke-snapshots/${stamp}`, destination = path.join(root, relative);
if (fs.existsSync(destination)) throw Error('Snapshot destination already exists');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const inputs = new Map();
function scan(directory) {
  for (const entry of fs.readdirSync(path.join(root, directory), { withFileTypes: true })) {
    const file = directory + '/' + entry.name;
    if (entry.isDirectory()) scan(file);
    else if (/\.(cpp|c|h|hpp|hlsl|hlsli)$/.test(entry.name)) inputs.set(file, null);
  }
}
for (const folder of ['host', 'renderer', 'engine/native', 'engine/audio', 'third_party', 'shaders']) scan(folder);
for (const file of ['AGENTS.md', 'VERSION', 'shaders/manifest.json', 'shaders/manifest.schema.json', 'tools/build-effects-host.ps1', 'tools/compile-shaders.ps1', 'tools/build-gameplay.ps1', 'tools/bootstrap-dxc.ps1', '.incrementalTesting/rendering/smoke-host-snapshot.mjs']) if (fs.existsSync(path.join(root, file))) inputs.set(file, null);
fs.mkdirSync(destination, { recursive: true });
function copy(file) {
  const bytes = fs.readFileSync(path.join(root, file)), target = path.join(destination, file);
  fs.mkdirSync(path.dirname(target), { recursive: true }); fs.writeFileSync(target, bytes); inputs.set(file, hash(bytes));
}
for (const file of inputs.keys()) copy(file);
let changed = [];
for (let attempt = 0; attempt < 3; attempt++) {
  changed = [...inputs].filter(([file, sha]) => hash(fs.readFileSync(path.join(root, file))) !== sha).map(([file]) => file);
  if (!changed.length) break;
  if (attempt < 2) for (const file of changed) copy(file);
}
if (changed.length) throw Error('Sources still changing during snapshot: ' + changed.join(', '));
// Share only the pinned read-only compiler installation, never build outputs.
fs.symlinkSync(path.join(root, '.tools'), path.join(destination, '.tools'), 'junction');
const capture = { schema: 'nativegpu.host-source-capture/1', capturedAt: new Date().toISOString(), originalRoot: root, snapshotRoot: destination, scope: 'Frozen host dependencies only; no build/regression performed by capture.', sources: Object.fromEntries(inputs) };
fs.writeFileSync(path.join(destination, 'source-capture.json'), JSON.stringify(capture, null, 2) + '\n');
fs.writeFileSync('build/smoke-host/snapshot-location.json', JSON.stringify({ snapshotRoot: destination, buildDirectory: relative + '/build/smoke-host', capture: relative + '/source-capture.json' }, null, 2) + '\n');
console.log(JSON.stringify({ snapshotRoot: destination, files: inputs.size, matchedWorkspaceAtCapture: true }));

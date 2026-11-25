import { spawn } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import * as fs from 'node:fs/promises';
import sharp from 'sharp';

// Native scene-preview contract: draft override, clear, simulate, stop and orthographic
// view, verified against real frame pixels. It needs a D3D12 device, a visible desktop
// window and a built build/viewport-host/nativegpu_viewport_host.exe, so it is a manual
// incremental entry (`npm run test:editor-scene`) rather than part of the default suite.
const root = path.resolve(import.meta.dirname, '../..');
const project = path.join(root, 'build/viewport-host/scene-preview-smoke-project');
const exe = path.join(root, 'build/viewport-host/nativegpu_viewport_host.exe');
const out = path.join(root, 'build/viewport-host/scene-preview-smoke');
const pipeName = `scene-preview-smoke-${process.pid}`;
// The engine's .screen scenes assume the engine's own asset layout. A managed project
// owns its assets, so the smoke builds the same layout the editor creates: textures
// and builtin character beside the scene.
await fs.rm(project, { recursive: true, force: true });
await fs.mkdir(path.join(project, '.screen'), { recursive: true });
await fs.cp(path.join(root, 'assets/builtin'), path.join(project, 'assets/builtin'), { recursive: true });
await fs.cp(path.join(root, 'assets/builtin/textures'), path.join(project, 'assets/textures'), { recursive: true });
await fs.cp(path.join(root, 'assets/gameplay'), path.join(project, 'assets/gameplay'), { recursive: true });
await fs.cp(path.join(root, 'build/effects'), path.join(project, 'build/effects'), { recursive: true });
const scene = JSON.parse(await fs.readFile(path.join(root, '.screen/basic.scene.json'), 'utf8'));
await fs.writeFile(path.join(project, '.screen/basic.scene.json'), JSON.stringify(scene));
const crate = scene.entities.find(entity => entity.name === '落下的物理方块');
if (!crate) throw new Error('dynamic prop missing from the smoke scene');
await fs.mkdir(out, { recursive: true });

const pipeServer = net.createServer();
await new Promise((resolve, reject) => {
  pipeServer.once('error', reject);
  pipeServer.listen(`\\\\.\\pipe\\${pipeName}`, resolve);
});
const child = spawn(exe, ['--parent', '0', '--pipe', pipeName, '--project', project, '--novsync',
  '--cooked', path.join(root, 'build/vegetation/cooked'),
  '--textures', path.join(root, 'build/vegetation/textures')], {
  cwd: project, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: false,
});
child.stderr.setEncoding('utf8');
child.stderr.on('data', chunk => { if (process.env.SMOKE_VERBOSE) process.stderr.write(chunk); });

let pending = '';
const acks = [];
const waiters = [];
child.stdout.setEncoding('utf8');
child.stdout.on('data', chunk => {
  pending += chunk;
  for (;;) {
    const newline = pending.indexOf('\n');
    if (newline < 0) break;
    const line = pending.slice(0, newline).trim();
    pending = pending.slice(newline + 1);
    if (!line) continue;
    if (process.env.SMOKE_VERBOSE) process.stderr.write(`[host] ${line}\n`);
    if (!line.startsWith('ack=1;')) continue;
    const fields = {};
    for (const pair of line.split(';')) {
      const index = pair.indexOf('=');
      if (index > 0) fields[pair.slice(0, index)] = pair.slice(index + 1);
    }
    acks.push(fields);
    for (const waiter of waiters.splice(0)) waiter(fields);
  }
});
child.on('exit', (code, signal) => {
  if (process.env.SMOKE_VERBOSE) process.stderr.write(`[host] exit code=${code} signal=${signal}\n`);
});

const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
let socket = null;
pipeServer.on('connection', connection => { socket = connection; });
for (let attempt = 0; attempt < 100 && !socket; attempt += 1) await sleep(100);
if (!socket) throw new Error('host did not connect to the pipe');
let requestId = 0;
async function send(fields) {
  const req = ++requestId;
  socket.write(`req=${req};${fields}\n`);
  const deadline = Date.now() + 15000;
  for (;;) {
    const index = acks.findIndex(ack => Number(ack.req) === req);
    if (index >= 0) {
      const [found] = acks.splice(index, 1);
      if (found.state !== 'ok') throw new Error(`${fields}: ${found.err}`);
      return found;
    }
    if (Date.now() > deadline) throw new Error(`timeout waiting for ${fields}`);
    await new Promise(resolve => waiters.push(resolve));
  }
}
async function capture(name) {
  const file = path.join(out, `${name}.png`);
  await fs.rm(file, { force: true });
  const ack = await send(`cmd=capture;rev=1;path=${file.replace(/\\/g, '/')}`);
  await fs.access(file);
  return file;
}
async function raw(file) {
  return sharp(file).removeAlpha().raw().toBuffer({ resolveWithObject: true });
}
function meanDifference(a, b) {
  if (a.info.width !== b.info.width || a.info.height !== b.info.height) return 255;
  let sum = 0;
  for (let index = 0; index < a.data.length; index += 1) sum += Math.abs(a.data[index] - b.data[index]);
  return sum / a.data.length;
}

try {
  await send('cmd=open;rev=1;mode=scene;scene=.screen/basic.scene.json;play=edit');
  await sleep(500);
  const edit = await capture('edit');
  const editRaw = await raw(edit);

  await send(`cmd=preview;rev=1;entity=${crate.id};px=${crate.position[0]};py=${crate.position[1] + 3};pz=${crate.position[2]};sx=${crate.scale[0]};sy=${crate.scale[1]};sz=${crate.scale[2]};active=1`);
  await sleep(200);
  const draft = await capture('draft');
  const draftDiff = meanDifference(editRaw, await raw(draft));

  await send('cmd=previewclear;rev=1');
  await sleep(200);
  const cleared = await capture('cleared');
  const clearedDiff = meanDifference(editRaw, await raw(cleared));

  await send('cmd=play;rev=1;play=simulate;pause=0');
  await sleep(2200);
  const simulated = await capture('simulated');
  const simulatedDiff = meanDifference(editRaw, await raw(simulated));

  await send('cmd=play;rev=1;play=edit;pause=0');
  await sleep(300);
  const restored = await capture('restored');
  const restoredDiff = meanDifference(editRaw, await raw(restored));

  await send('cmd=camera;rev=1;view=top');
  await sleep(200);
  const top = await capture('top');
  const topDiff = meanDifference(editRaw, await raw(top));

  const report = {
    // The scene occupies a small part of the frame, so the useful signal is that a
    // draft/simulation changes many more pixels than the same view captured twice.
    success: draftDiff > clearedDiff * 3 && clearedDiff < 0.1 && simulatedDiff > restoredDiff * 3 && restoredDiff < 0.1 && topDiff > 5,
    meanDifferences: { draft: draftDiff, cleared: clearedDiff, simulated: simulatedDiff, restored: restoredDiff, top: topDiff },
    files: { edit, draft, cleared, simulated, restored, top },
  };
  await fs.writeFile(path.join(out, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
  if (!report.success) process.exitCode = 1;
} catch (error) {
  console.error(error.stack);
  process.exitCode = 1;
} finally {
  child.kill();
  socket.destroy();
  pipeServer.close();
  await new Promise(resolve => {
    if (child.exitCode !== null || child.signalCode) { resolve(); return; }
    child.once('exit', resolve);
    setTimeout(resolve, 3000);
  });
  // The copied assets are only needed while the host runs; the screenshots and
  // report stay as evidence.
  await fs.rm(project, { recursive: true, force: true }).catch(() => {});
}

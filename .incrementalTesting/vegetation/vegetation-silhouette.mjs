import { spawn } from 'node:child_process';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// Fixed-camera silhouette baseline. The realism task asks for "no-texture black silhouettes, at
// least 8 azimuths and 2 pitches, compared against a target board" before any morphology change can
// be judged — and 24 rounds of R1 work ran without it, which is why every improvement so far could
// only be argued numerically.
//
// This uses the native viewport host's own `--screenshot` path rather than a second renderer: the
// images then come from the same shader, the same cooked package and the same digest the editor
// shows, so a later comparison cannot be blamed on a different pipeline.
//
// The depth view is used as the silhouette stand-in: it renders surface shape with no albedo, which
// is the property a silhouette is for. It is recorded as such in the manifest — calling it a
// "silhouette" in the data would overstate what was rendered.

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const HOST = path.join(root, 'build/viewport-host/nativegpu_viewport_host.exe');
const IMAGES = path.join(root, 'reports/vegetation-realism/images/silhouette');
const MANIFEST = path.join(root, 'reports/vegetation-realism/silhouette.json');

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const index = args.indexOf(name);
  return index >= 0 && args[index + 1] !== undefined ? args[index + 1] : fallback;
};
const species = option('--species', 'oak_broadleaf');
const yawStep = Number(option('--yaw-step', '45'));
const pitches = option('--pitches', '0.10,0.55').split(',').map(Number);
const distances = Number(option('--distance', '0'));
const view = option('--view', 'depth');
const frames = Number(option('--frames', '6'));
const instances = Number(option('--instances', '1'));

function render(yaw, pitch, file) {
  return new Promise((resolve) => {
    const child = spawn(HOST, [
      '--cooked', 'build/vegetation/cooked',
      '--species', species,
      '--instances', String(instances),
      '--view', view,
      // A pinned clock keeps two builds comparable: the canopy moves on the wind clock, so an
      // unpinned screenshot differs between runs for reasons that have nothing to do with shape.
      // The flag is `--time`; `--seconds` does not exist and made the host reject the whole command
      // line, which is why the first run produced eight missing files instead of eight images.
      '--time', '0',
      '--frames', String(frames),
      '--screenshot', file,
      '--yaw', String(yaw),
      '--pitch', String(pitch),
      ...(distances > 0 ? ['--distance', String(distances)] : []),
    ], { windowsHide: true });
    let stdout = '';
    child.stdout.on('data', (chunk) => { stdout += chunk; });
    child.on('error', (error) => resolve({ ok: false, error: error.message, stdout }));
    child.on('exit', (code) => resolve({ ok: code === 0, code, stdout }));
  });
}

const digestOf = (stdout) => {
  const match = /digest=([0-9a-f]+)/.exec(stdout);
  return match ? match[1] : null;
};

async function main() {
  await fs.mkdir(IMAGES, { recursive: true });
  const shots = [];
  let digest = null;
  for (const pitch of pitches) {
    for (let yaw = 0; yaw < 360; yaw += yawStep) {
      const name = `${species}-${view}-yaw${String(yaw).padStart(3, '0')}-pitch${String(pitch).replace('.', '')}.png`;
      const file = path.join(IMAGES, name);
      const result = await render(yaw, pitch, file);
      const size = await fs.stat(file).then((stat) => stat.size).catch(() => 0);
      if (!digest) digest = digestOf(result.stdout);
      shots.push({
        yaw, pitch, view, file: path.relative(root, file).split(path.sep).join('/'),
        bytes: size, ok: result.ok && size > 0,
        digest: digestOf(result.stdout),
        error: result.ok ? null : (result.error ?? `host exited ${result.code}`),
      });
      process.stdout.write(`yaw=${String(yaw).padStart(3)} pitch=${pitch} ${size > 0 ? 'ok' : 'MISSING'} ${size} bytes\n`);
    }
  }
  const manifest = {
    schema: 'nativegpu.vegetation-silhouette/1',
    generatedAt: new Date().toISOString(),
    species,
    view,
    // Stated plainly: this is the depth view, not a black-outline pass. It answers "what shape does
    // the surface cover" and nothing about material.
    note: 'depth view used as silhouette stand-in; no albedo, no texture',
    camera: { yawStep, pitches, distanceMetres: distances || 'auto', frames, instances, seconds: 0 },
    host: path.relative(root, HOST).split(path.sep).join('/'),
    packageDigest: digest,
    shots,
    complete: shots.every((shot) => shot.ok),
  };
  await fs.writeFile(MANIFEST, `${JSON.stringify(manifest, null, 2)}\n`);
  console.log(JSON.stringify({
    manifest: path.relative(root, MANIFEST).split(path.sep).join('/'),
    shots: shots.length, failed: shots.filter((shot) => !shot.ok).length, digest,
  }));
  if (!manifest.complete) process.exitCode = 1;
}

await main();

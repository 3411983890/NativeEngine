#!/usr/bin/env node
// World render check: cook a world and prove the native viewport draws it.
//
// The plan is explicit that matching source strings is not evidence of native integration.
// This script therefore produces a real frame from the real host, then checks the pixels:
// a capture that is uniformly one colour means the draw silently produced nothing, which is
// exactly the failure a "the code path exists" test would miss.
//
//   node .incrementalTesting/rendering/world/world-render-check.mjs --project <dir> [--world <id>]
//
// Writes reports/world-generation/<world>-<manifest>.png and a JSON summary next to it.

import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..', '..');
const HOST = path.join(root, 'build', 'viewport-host', 'nativegpu_viewport_host.exe');

function parseArguments(argv) {
  const options = {};
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index];
    if (!token.startsWith('--')) continue;
    const next = argv[index + 1];
    if (next === undefined || next.startsWith('--')) options[token.slice(2)] = true;
    else { options[token.slice(2)] = next; index += 1; }
  }
  return options;
}

function run(command, args, { cwd = root, timeout = 300000, hideWindow = true } = {}) {
  return new Promise((resolve, reject) => {
    // The host presents into a real window and advances `--frames` from its message loop.
    // CREATE_NO_WINDOW (windowsHide) suppresses the paint messages the loop waits for, so
    // the render step has to let the window exist even though nobody is looking at it.
    const child = spawn(command, args, { cwd, windowsHide: hideWindow, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => { child.kill(); reject(new Error('timeout')); }, timeout);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.on('close', code => { clearTimeout(timer); resolve({ code, stdout, stderr }); });
  });
}

// Minimal PNG reader: enough to measure a capture without pulling in an image library.
// Handles 8-bit RGB/RGBA, non-interlaced, which is what renderer/png_writer.cpp emits.
function decodePng(buffer) {
  if (buffer.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  let offset = 8;
  let width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];
  while (offset < buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    offset += 12 + length;
  }
  if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6)) {
    throw new Error(`unsupported PNG (depth ${bitDepth}, colour type ${colorType})`);
  }
  const channels = colorType === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const pixels = Buffer.alloc(height * stride);
  let previous = Buffer.alloc(stride);
  let cursor = 0;
  for (let row = 0; row < height; row += 1) {
    const filter = raw[cursor];
    cursor += 1;
    const line = Buffer.from(raw.subarray(cursor, cursor + stride));
    cursor += stride;
    for (let index = 0; index < stride; index += 1) {
      const a = index >= channels ? line[index - channels] : 0;
      const b = previous[index];
      const c = index >= channels ? previous[index - channels] : 0;
      let value = line[index];
      if (filter === 1) value += a;
      else if (filter === 2) value += b;
      else if (filter === 3) value += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        value += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      }
      line[index] = value & 0xff;
    }
    line.copy(pixels, row * stride);
    previous = line;
  }
  return { width, height, channels, pixels };
}

function analyze(image) {
  const { width, height, channels, pixels } = image;
  const seen = new Map();
  let sum = 0, sumSquared = 0, count = 0;
  // Sample on a grid rather than every pixel: a 960x540 frame is 500k pixels and the
  // question ("is this one flat colour?") does not need all of them.
  const stepX = Math.max(1, Math.floor(width / 160));
  const stepY = Math.max(1, Math.floor(height / 90));
  for (let y = 0; y < height; y += stepY) {
    for (let x = 0; x < width; x += stepX) {
      const index = (y * width + x) * channels;
      const key = `${pixels[index]},${pixels[index + 1]},${pixels[index + 2]}`;
      seen.set(key, (seen.get(key) ?? 0) + 1);
      const luma = 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2];
      sum += luma;
      sumSquared += luma * luma;
      count += 1;
    }
  }
  const mean = sum / count;
  const variance = sumSquared / count - mean * mean;
  const sorted = [...seen.values()].sort((a, b) => b - a);
  return {
    samples: count,
    distinctColors: seen.size,
    dominantShare: (sorted[0] ?? 0) / count,
    meanLuma: Number(mean.toFixed(2)),
    lumaStdDev: Number(Math.sqrt(Math.max(0, variance)).toFixed(2)),
  };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const projectRoot = path.resolve(options.project ?? path.join(process.env.USERPROFILE ?? '.', 'Documents', 'nativeEngineProject', 'WorldSample'));
  const worldId = options.world ?? 'ancient-valley';

  const reportDir = path.join(projectRoot, 'reports', 'world-generation');
  await fs.mkdir(reportDir, { recursive: true });

  console.log('world: cooking…');
  const cook = await run(process.execPath, [path.join(root, 'tools', 'world.mjs'), 'cook', '--project', projectRoot, '--world', worldId]);
  if (cook.code !== 0) {
    console.error(cook.stdout, cook.stderr);
    throw new Error('cook failed');
  }
  const manifestMatch = /manifest\s+([a-f0-9]{64})/.exec(cook.stdout);
  if (!manifestMatch) throw new Error('the cook did not report a manifest hash');
  const manifestHash = manifestMatch[1];
  const cooked = `assets/world-cooked/${manifestHash}`;

  const screenshot = path.join(reportDir, `${worldId}-${manifestHash.slice(0, 12)}.png`);
  const frames = Number(options.frames ?? 20);
  console.log(`world: rendering ${frames} frames…`);
  const host = await run(HOST, [
    '--parent', '0',
    '--project', projectRoot,
    '--world', cooked,
    '--frames', String(frames),
    '--novsync',
    '--screenshot', screenshot,
    '--width', String(options.width ?? 960),
    '--height', String(options.height ?? 540),
  ], { timeout: 180000, hideWindow: false });
  if (host.code !== 0) {
    console.error(host.stdout, host.stderr);
    throw new Error(`the native host exited with ${host.code}`);
  }

  const statsLine = /world stats: (.*)/.exec(host.stdout);
  const stats = {};
  if (statsLine) {
    for (const pair of statsLine[1].trim().split(/\s+/)) {
      const [key, value] = pair.split('=');
      if (key && value !== undefined) stats[key] = Number(value);
    }
  }

  const image = decodePng(await fs.readFile(screenshot));
  const analysis = analyze(image);

  const problems = [];
  if (!stats.chunks) problems.push('no chunk became resident');
  if (!stats.drawn) problems.push('no chunk was drawn');
  if (!stats.instances) problems.push('no instance was drawn');
  if (analysis.distinctColors < 24) problems.push(`only ${analysis.distinctColors} distinct colours: the frame is effectively blank`);
  if (analysis.dominantShare > 0.96) problems.push(`${(analysis.dominantShare * 100).toFixed(1)}% of the frame is one colour`);
  if (analysis.lumaStdDev < 3) problems.push('the frame has almost no luminance variation');

  const summary = {
    schema: 'nativegpu.world-render-evidence/1',
    worldId,
    manifestHash,
    frames,
    size: [image.width, image.height],
    stats,
    analysis,
    screenshot: path.relative(projectRoot, screenshot).split(path.sep).join('/'),
    ok: problems.length === 0,
    problems,
  };
  const summaryPath = path.join(reportDir, `${worldId}-${manifestHash.slice(0, 12)}.json`);
  await fs.writeFile(summaryPath, JSON.stringify(summary, null, 2), 'utf8');

  console.log(`world ${worldId} manifest ${manifestHash.slice(0, 12)}`);
  console.log(`  chunks ${stats.chunks ?? 0} drawn ${stats.drawn ?? 0} instances ${stats.instances ?? 0} triangles ${stats.tris ?? 0}`);
  console.log(`  colours ${analysis.distinctColors}, dominant ${(analysis.dominantShare * 100).toFixed(1)}%, luma σ ${analysis.lumaStdDev}`);
  console.log(`  capture ${summary.screenshot}`);
  console.log(`  report  ${path.relative(projectRoot, summaryPath).split(path.sep).join('/')}`);
  if (problems.length) {
    for (const problem of problems) console.error(`  FAIL ${problem}`);
    process.exit(1);
  }
  console.log('  render evidence OK');
}

main().catch(error => {
  console.error(error.stack ?? error.message);
  process.exit(1);
});

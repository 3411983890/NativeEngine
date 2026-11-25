#!/usr/bin/env node
// WAV -> .ngsnd importer and bank writer.
//
//   node tools/audio.mjs import --source assets/fogtown/audio --out assets/audio
//   node tools/audio.mjs inspect assets/audio/bank.json
//
// Import is offline and deterministic: the same WAV always produces the same bytes,
// so the bank hash can go into the run-package manifest.

import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, extname, join, relative, resolve, sep } from 'node:path';

const SAMPLE_RATE = 48000;
const TAPS = 24;
const PHASES = 64;

function fail(message) {
  console.error(`audio: ${message}`);
  process.exit(1);
}

function readWav(path) {
  const bytes = readFileSync(path);
  if (bytes.length < 44 || bytes.toString('ascii', 0, 4) !== 'RIFF' || bytes.toString('ascii', 8, 12) !== 'WAVE') {
    fail(`${path} is not a RIFF/WAVE file`);
  }
  let format = 0, channels = 0, sampleRate = 0, bits = 0, dataOffset = 0, dataBytes = 0;
  let offset = 12;
  while (offset + 8 <= bytes.length) {
    const id = bytes.toString('ascii', offset, offset + 4);
    const size = bytes.readUInt32LE(offset + 4);
    const body = offset + 8;
    if (body + size > bytes.length) break;
    if (id === 'fmt ' && size >= 16) {
      format = bytes.readUInt16LE(body);
      channels = bytes.readUInt16LE(body + 2);
      sampleRate = bytes.readUInt32LE(body + 4);
      bits = bytes.readUInt16LE(body + 14);
      if (format === 0xfffe && size >= 26) format = bytes.readUInt16LE(body + 24);
    } else if (id === 'data') {
      dataOffset = body;
      dataBytes = size;
    }
    offset = body + size + (size & 1);
  }
  if (!dataOffset || !channels || channels > 2) fail(`${path} has no usable fmt/data chunk`);
  if (format !== 1 && format !== 3) fail(`${path} uses an unsupported encoding (${format})`);
  const width = bits / 8;
  const frames = Math.floor(dataBytes / (width * channels));
  const samples = new Float32Array(frames * channels);
  let peak = 0;
  for (let i = 0; i < samples.length; i += 1) {
    const p = dataOffset + i * width;
    let value = 0;
    if (format === 3 && bits === 32) value = bytes.readFloatLE(p);
    else if (format === 1 && bits === 16) value = bytes.readInt16LE(p) / 32768;
    else if (format === 1 && bits === 24) {
      value = ((bytes[p + 2] << 24) | (bytes[p + 1] << 16) | (bytes[p] << 8)) >> 8;
      value /= 8388608;
    } else if (format === 1 && bits === 32) value = bytes.readInt32LE(p) / 2147483648;
    else fail(`${path} uses unsupported bit depth ${bits}`);
    samples[i] = value;
    peak = Math.max(peak, Math.abs(value));
  }
  return { channels, sampleRate, samples, peak };
}

function polyTable(ratio) {
  const cutoff = 0.5 * Math.min(1, 1 / ratio) * 0.94;
  const table = Array.from({ length: PHASES }, () => new Float32Array(TAPS));
  for (let phase = 0; phase < PHASES; phase += 1) {
    const frac = phase / PHASES;
    let sum = 0;
    for (let tap = 0; tap < TAPS; tap += 1) {
      const x = tap - (TAPS / 2 - 1) - frac;
      const sinc = Math.abs(x) < 1e-9 ? 1 : Math.sin(2 * Math.PI * cutoff * x) / (2 * Math.PI * cutoff * x);
      const window = 0.42 - 0.5 * Math.cos((2 * Math.PI * (tap + frac)) / TAPS)
        + 0.08 * Math.cos((4 * Math.PI * (tap + frac)) / TAPS);
      table[phase][tap] = sinc * window;
      sum += table[phase][tap];
    }
    if (Math.abs(sum) > 1e-9) for (let tap = 0; tap < TAPS; tap += 1) table[phase][tap] /= sum;
  }
  return table;
}

function resample(input, channels, sourceRate) {
  if (sourceRate === SAMPLE_RATE) return input;
  const ratio = SAMPLE_RATE / sourceRate;
  const table = polyTable(ratio);
  const inFrames = input.length / channels;
  const outFrames = Math.ceil(inFrames * ratio);
  const out = new Float32Array(outFrames * channels);
  const half = Math.floor(TAPS / 2);
  for (let n = 0; n < outFrames; n += 1) {
    const position = n / ratio;
    const base = Math.floor(position);
    const frac = position - base;
    const phase = Math.min(PHASES - 1, Math.round(frac * PHASES));
    for (let ch = 0; ch < channels; ch += 1) {
      let sum = 0;
      for (let tap = 0; tap < TAPS; tap += 1) {
        const index = base - half + 1 + tap;
        if (index < 0 || index >= inFrames) continue;
        sum += table[phase][tap] * input[index * channels + ch];
      }
      out[n * channels + ch] = sum;
    }
  }
  return out;
}

// ITU-R BS.1770-4 K-weighting, enough for a per-asset loudness tag.
function loudness(samples, channels) {
  const shelf = { b: [1.53512485958697, -2.69169618940638, 1.19839281085285], a: [-1.69065929318241, 0.73248077421585] };
  const highpass = { b: [1, -2, 1], a: [-1.99004745483398, 0.99007225036621] };
  const state = Array.from({ length: channels }, () => ({ s1: [0, 0, 0, 0], s2: [0, 0, 0, 0] }));
  const window = SAMPLE_RATE / 10;
  const energies = [];
  let energy = 0;
  let count = 0;
  for (let i = 0; i < samples.length / channels; i += 1) {
    for (let ch = 0; ch < channels; ch += 1) {
      const x = samples[i * channels + ch];
      const s = state[ch].s1;
      const y1 = shelf.b[0] * x + shelf.b[1] * s[0] + shelf.b[2] * s[1] - shelf.a[0] * s[2] - shelf.a[1] * s[3];
      s[1] = s[0]; s[0] = x; s[3] = s[2]; s[2] = y1;
      const t = state[ch].s2;
      const y2 = highpass.b[0] * y1 + highpass.b[1] * t[0] + highpass.b[2] * t[1] - highpass.a[0] * t[2] - highpass.a[1] * t[3];
      t[1] = t[0]; t[0] = y1; t[3] = t[2]; t[2] = y2;
      energy += y2 * y2;
    }
    count += 1;
    if (count === window) {
      energies.push(-0.691 + 10 * Math.log10(Math.max(1e-12, energy / count)));
      energy = 0;
      count = 0;
    }
  }
  const gated = energies.filter(value => value > -70);
  if (!gated.length) return -70;
  const mean = gated.reduce((sum, value) => sum + 10 ** (value / 10), 0) / gated.length;
  const relative = 10 * Math.log10(mean) - 10;
  const kept = gated.filter(value => value > relative);
  const final = kept.length ? kept : gated;
  return 10 * Math.log10(final.reduce((sum, value) => sum + 10 ** (value / 10), 0) / final.length);
}

function encodeNgsnd(sound) {
  const frames = sound.samples.length / sound.channels;
  const header = Buffer.alloc(44);
  header.write('NGS1', 0, 'ascii');
  header.writeUInt16LE(1, 4);
  header.writeUInt16LE(sound.channels, 6);
  header.writeUInt32LE(SAMPLE_RATE, 8);
  header.writeUInt32LE(frames, 12);
  header.writeUInt16LE(2, 16); // f32
  header.writeUInt16LE(sound.loop ? 1 : 0, 18);
  header.writeUInt32LE(0, 20);
  header.writeUInt32LE(sound.loop ? frames : 0, 24);
  header.writeFloatLE(sound.loudnessLufs, 28);
  header.writeFloatLE(sound.peakDbtp, 32);
  header.writeUInt32LE(44, 36);
  header.writeUInt32LE(frames * sound.channels * 4, 40);
  const data = Buffer.alloc(frames * sound.channels * 4);
  for (let i = 0; i < sound.samples.length; i += 1) data.writeFloatLE(sound.samples[i], i * 4);
  return Buffer.concat([header, data]);
}

function importDirectory(source, out, bankPath, namespace = 'fogtown') {
  const files = readdirSync(source)
    .filter(name => extname(name).toLowerCase() === '.wav')
    .sort();
  if (!files.length) fail(`no .wav files under ${source}`);
  mkdirSync(out, { recursive: true });
  const entries = [];
  for (const file of files) {
    const path = join(source, file);
    const wav = readWav(path);
    const samples = resample(wav.samples, wav.channels, wav.sampleRate);
    const name = `${namespace ? namespace+'/' : ''}${basename(file, extname(file))}`;
    const lufs = loudness(samples, wav.channels);
    const peakDbtp = wav.peak > 0 ? 20 * Math.log10(wav.peak) : -120;
    const sound = { channels: wav.channels, samples, loop: false, loudnessLufs: lufs, peakDbtp };
    const target = join(out, `${name}.ngsnd`);
    mkdirSync(dirname(target), { recursive: true });
    const bytes = encodeNgsnd(sound);
    writeFileSync(target, bytes);
    entries.push({
      name,
      file: relative(process.cwd(), target).split(sep).join('/'),
      channels: wav.channels,
      frames: samples.length / wav.channels,
      sampleRate: SAMPLE_RATE,
      loudnessLufs: Number(lufs.toFixed(2)),
      peakDbtp: Number(peakDbtp.toFixed(2)),
      sha256: createHash('sha256').update(bytes).digest('hex'),
    });
    console.log(`  ${name}  ${(bytes.length / 1024).toFixed(1)} KiB  ${lufs.toFixed(1)} LUFS  ${peakDbtp.toFixed(1)} dBTP`);
  }
  const bank = {
    version: 1,
    sampleRate: SAMPLE_RATE,
    sounds: entries,
    hash: createHash('sha256').update(entries.map(entry => entry.sha256).join('')).digest('hex'),
  };
  writeFileSync(bankPath, `${JSON.stringify(bank, null, 2)}\n`);
  console.log(`bank: ${bankPath} (${entries.length} sounds, ${bank.hash.slice(0, 12)})`);
}

function inspect(path) {
  const bank = JSON.parse(readFileSync(path, 'utf8'));
  console.log(`bank v${bank.version}, ${bank.sounds.length} sounds, hash ${bank.hash.slice(0, 12)}`);
  for (const sound of bank.sounds) {
    const size = statSync(sound.file).size;
    console.log(`  ${sound.name.padEnd(24)} ${String(sound.frames).padStart(7)} frames  ${sound.loudnessLufs} LUFS  ${(size / 1024).toFixed(1)} KiB`);
  }
}

const [command, ...rest] = process.argv.slice(2);
if (command === 'import') {
  let source = 'assets/fogtown/audio';
  let out = 'assets/audio';
  let bank = 'assets/audio/bank.json';
  let namespace = 'fogtown';
  for (let i = 0; i < rest.length; i += 1) {
    if (rest[i] === '--source') source = rest[++i];
    else if (rest[i] === '--out') out = rest[++i];
    else if (rest[i] === '--bank') bank = rest[++i];
    else if (rest[i] === '--namespace') {namespace=rest[++i];if(namespace==='none')namespace='';if(!/^[A-Za-z0-9_-]*$/.test(namespace))fail('invalid namespace');}
    else fail(`unknown argument ${rest[i]}`);
  }
  importDirectory(resolve(source), resolve(out), resolve(bank), namespace);
} else if (command === 'inspect') {
  if (!rest.length) fail('inspect needs a bank path');
  inspect(resolve(rest[0]));
} else {
  fail('usage: audio.mjs import [--source dir] [--out dir] [--bank file] | inspect <bank.json>');
}

#!/usr/bin/env node
// Deterministic procedural texture plates for the town scene.
//
// Every material is authored three times: albedo, tangent-space normal (derived from the
// same height field), and ORM (AO in R, roughness in G, metalness in B). The generator is
// intentionally seed-driven and tileable so re-running it produces byte-identical PNGs.
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { encodePng } from '../textures.mjs';

const SIZE = 256;

function clamp01(value) {
  return value < 0 ? 0 : value > 1 ? 1 : value;
}
function lerp(a, b, t) {
  return a + (b - a) * t;
}
function smoothstep(edge0, edge1, value) {
  const t = clamp01((value - edge0) / (edge1 - edge0));
  return t * t * (3 - 2 * t);
}
function hash2(x, y, seed) {
  let h = Math.imul(x | 0, 374761393) ^ Math.imul(y | 0, 668265263) ^ Math.imul(seed | 0, 1442695041);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
}
function wrap(value, period) {
  return ((value % period) + period) % period;
}
function noise2(x, y, period, seed) {
  const xi = Math.floor(x);
  const yi = Math.floor(y);
  const xf = x - xi;
  const yf = y - yi;
  const u = xf * xf * (3 - 2 * xf);
  const v = yf * yf * (3 - 2 * yf);
  const x0 = wrap(xi, period);
  const y0 = wrap(yi, period);
  const x1 = wrap(xi + 1, period);
  const y1 = wrap(yi + 1, period);
  const a = hash2(x0, y0, seed);
  const b = hash2(x1, y0, seed);
  const c = hash2(x0, y1, seed);
  const d = hash2(x1, y1, seed);
  return lerp(lerp(a, b, u), lerp(c, d, u), v);
}
function fbm(u, v, basePeriod, octaves, seed) {
  let sum = 0;
  let amplitude = 0.5;
  let period = basePeriod;
  let norm = 0;
  for (let octave = 0; octave < octaves; octave += 1) {
    sum += amplitude * noise2(u * period, v * period, period, seed + octave * 17);
    norm += amplitude;
    amplitude *= 0.5;
    period *= 2;
  }
  return sum / norm;
}
function ridged(u, v, basePeriod, octaves, seed) {
  return 1 - Math.abs(fbm(u, v, basePeriod, octaves, seed) * 2 - 1);
}

// Jittered grid used by cobbles and stones. Returns { edge, id } where edge is 0 at a
// cell boundary and 1 at the stone center.
function cell(u, v, columns, rows, seed) {
  const gx = u * columns;
  const gy = v * rows;
  const ix = Math.floor(gx);
  const iy = Math.floor(gy);
  let best = 1e9;
  let bestId = 0;
  for (let oy = -1; oy <= 1; oy += 1) {
    for (let ox = -1; ox <= 1; ox += 1) {
      const cx = ix + ox;
      const cy = iy + oy;
      const wx = wrap(cx, columns);
      const wy = wrap(cy, rows);
      const jx = 0.5 + (hash2(wx, wy, seed) - 0.5) * 0.55;
      const jy = 0.5 + (hash2(wx, wy, seed + 7) - 0.5) * 0.55;
      const dx = gx - (cx + jx);
      const dy = gy - (cy + jy);
      // A superellipse distance keeps cobbles squarish instead of perfectly round.
      const distance = Math.pow(Math.pow(Math.abs(dx), 4) + Math.pow(Math.abs(dy), 4), 0.25);
      if (distance < best) {
        best = distance;
        bestId = hash2(wx, wy, seed + 13);
      }
    }
  }
  const edge = smoothstep(0.60, 0.33, best);
  return { edge, id: bestId };
}

function brick(u, v, { rows = 12, columns = 6, base, mortar, seed = 1, variation = 0.16 }) {
  const row = Math.floor(v * rows);
  const offset = (row % 2) * 0.5;
  const uu = u * columns + offset;
  const column = Math.floor(uu);
  const localX = uu - column;
  const localY = v * rows - row;
  const mortarWidth = 0.045;
  const mortarLine = localX < mortarWidth || localY < mortarWidth * 0.9;
  const bevel = Math.min(
    smoothstep(0.0, 0.08, localX),
    smoothstep(1.0, 0.92, localX),
    smoothstep(0.0, 0.10, localY),
    smoothstep(1.0, 0.90, localY),
  );
  const tint = hash2(wrap(column, columns), row, seed);
  const grain = fbm(u, v, 32, 3, seed + 3);
  const color = base.map((channel) => channel * (1 - variation + tint * variation * 2) * (0.92 + grain * 0.16));
  return {
    color: mortarLine ? mortar : color,
    height: mortarLine ? 0.18 : 0.55 + bevel * 0.3 + grain * 0.1,
    roughness: mortarLine ? 0.98 : 0.86 + grain * 0.08,
    metalness: 0,
    ao: mortarLine ? 0.55 : 0.85 + bevel * 0.15,
  };
}

function plaster(u, v, { base, seed = 2 }) {
  const broad = fbm(u, v, 4, 4, seed);
  const fine = fbm(u, v, 48, 2, seed + 5);
  const stain = smoothstep(0.60, 0.86, broad) * 0.12;
  const crack = smoothstep(0.86, 0.96, ridged(u, v, 6, 4, seed + 9));
  const color = base.map((channel, index) => channel * (0.9 + broad * 0.12 - stain) + (index === 0 ? stain * 0.05 : 0));
  return {
    color,
    height: 0.55 + fine * 0.18 - crack * 0.3,
    roughness: 0.92 + fine * 0.05,
    metalness: 0,
    ao: 0.92 - crack * 0.25,
  };
}

function roofTiles(u, v, { base, seed = 3 }) {
  const rows = 10;
  const columns = 8;
  const row = Math.floor(v * rows);
  const localY = v * rows - row;
  const uu = u * columns + (row % 2) * 0.5;
  const column = Math.floor(uu);
  const localX = uu - column;
  const arch = Math.sin(localX * Math.PI);
  const overlap = smoothstep(0.0, 0.18, localY) * smoothstep(1.0, 0.82, localY);
  const tint = hash2(wrap(column, columns), row, seed);
  const grain = fbm(u, v, 24, 2, seed + 4);
  const groove = localY < 0.08 ? 0.72 : 1;
  const shade = 0.68 + arch * 0.34 + overlap * 0.18;
  return {
    color: base.map((channel) => channel * shade * (0.82 + tint * 0.24 + grain * 0.06) * groove),
    height: 0.35 + arch * 0.35 + overlap * 0.2,
    roughness: 0.78 + grain * 0.12,
    metalness: 0,
    ao: 0.62 + arch * 0.28 + overlap * 0.1,
  };
}

function woodPlanks(u, v, { base, seed = 4, planks = 6 }) {
  const uu = u * planks;
  const plank = Math.floor(uu);
  const localX = uu - plank;
  const seam = localX < 0.035 || localX > 0.965;
  const grain = fbm(u * 1.0, v * 0.25, 16, 4, seed);
  const rings = 0.5 + 0.5 * Math.sin((v * 26 + grain * 6 + hash2(plank, 0, seed) * 8) * Math.PI);
  const knot = smoothstep(0.12, 0.0, Math.hypot((localX - 0.5) * 2.4, (v - 0.62) * 6)) * 0.4;
  return {
    color: base.map((channel) => channel * (0.72 + rings * 0.22 + grain * 0.12 - knot)),
    height: seam ? 0.15 : 0.55 + rings * 0.15 - knot * 0.2,
    roughness: 0.66 + grain * 0.12,
    metalness: 0,
    ao: seam ? 0.55 : 0.95 - knot * 0.2,
  };
}

function metalPanels(u, v, { base, seed = 5 }) {
  const panels = 3;
  const seamX = Math.abs(((u * panels) % 1) - 0.0) < 0.02 || Math.abs(((u * panels) % 1) - 1.0) < 0.02;
  const seamY = Math.abs(((v * panels) % 1) - 0.0) < 0.02;
  const brushed = fbm(u * 0.4, v * 24, 64, 2, seed) * 0.5 + fbm(u * 8, v * 96, 128, 1, seed + 1) * 0.5;
  const rivetX = Math.abs(((u * panels * 4) % 1) - 0.5) < 0.06;
  const rivetY = Math.abs(((v * panels * 4) % 1) - 0.5) < 0.06;
  const rivet = rivetX && rivetY ? 1 : 0;
  return {
    color: base.map((channel) => channel * (0.72 + brushed * 0.35 - (seamX || seamY ? 0.2 : 0))),
    height: (seamX || seamY ? 0.25 : 0.6) + rivet * 0.25 + brushed * 0.05,
    roughness: 0.32 + brushed * 0.2 + (seamX || seamY ? 0.2 : 0),
    metalness: 1,
    ao: seamX || seamY ? 0.6 : 0.95,
  };
}

function cobble(u, v, { base, seed = 6 }) {
  const { edge, id } = cell(u, v, 7, 7, seed);
  const grain = fbm(u, v, 32, 3, seed + 2);
  const gap = smoothstep(0.15, 0.55, edge);
  const shade = 0.55 + id * 0.35 + grain * 0.12;
  return {
    color: [
      base[0] * shade * gap + 0.30 * (1 - gap),
      base[1] * shade * gap + 0.30 * (1 - gap),
      base[2] * shade * gap + 0.29 * (1 - gap),
    ],
    height: 0.2 + edge * 0.55 + grain * 0.1,
    roughness: 0.9 + grain * 0.06,
    metalness: 0,
    ao: 0.45 + gap * 0.5,
  };
}

function stoneBlocks(u, v, { base, seed = 7 }) {
  const rows = 6;
  const columns = 4;
  const row = Math.floor(v * rows);
  const offset = (row % 2) * 0.5;
  const uu = u * columns + offset;
  const column = Math.floor(uu);
  const localX = uu - column;
  const localY = v * rows - row;
  const mortar = localX < 0.02 || localY < 0.025;
  const tint = hash2(wrap(column, columns), row, seed);
  const grain = fbm(u, v, 24, 3, seed + 6);
  return {
    color: base.map((channel) => (mortar ? 0.42 : channel * (0.78 + tint * 0.28 + grain * 0.1))),
    height: mortar ? 0.2 : 0.55 + tint * 0.15 + grain * 0.1,
    roughness: mortar ? 0.95 : 0.88 + grain * 0.06,
    metalness: 0,
    ao: mortar ? 0.55 : 0.92,
  };
}

function grass(u, v, { base, seed = 8 }) {
  const clumps = fbm(u, v, 8, 4, seed);
  const blades = fbm(u * 40, v * 6, 64, 2, seed + 3);
  const dry = smoothstep(0.58, 0.8, fbm(u, v, 3, 3, seed + 5));
  return {
    color: [
      lerp(base[0], 0.55, dry * 0.5) * (0.7 + clumps * 0.4),
      lerp(base[1], 0.52, dry * 0.4) * (0.72 + blades * 0.3 + clumps * 0.2),
      lerp(base[2], 0.22, dry * 0.4) * (0.7 + blades * 0.25),
    ],
    height: 0.4 + blades * 0.35 + clumps * 0.2,
    roughness: 0.95,
    metalness: 0,
    ao: 0.8 + clumps * 0.2,
  };
}

function water(u, v, { base, seed = 9 }) {
  const ripple = fbm(u, v, 6, 3, seed);
  const sparkle = smoothstep(0.72, 0.9, fbm(u, v, 48, 2, seed + 4));
  return {
    color: [
      base[0] * (0.60 + ripple * 0.35) + sparkle * 0.15,
      base[1] * (0.68 + ripple * 0.32) + sparkle * 0.18,
      base[2] * (0.78 + ripple * 0.28) + sparkle * 0.2,
    ],
    height: 0.5 + ripple * 0.2,
    roughness: 0.08 + ripple * 0.05,
    metalness: 0,
    ao: 1,
  };
}

function windowPane(u, v, { seed = 10 }) {
  const border = Math.min(u, 1 - u, v, 1 - v);
  const frame = border < 0.10;
  const cross = Math.abs(u - 0.5) < 0.025 || Math.abs(v - 0.5) < 0.025;
  const glass = !frame && !cross;
  const reflection = 0.55 + 0.35 * smoothstep(0.0, 0.7, v) + fbm(u, v, 16, 2, seed) * 0.1;
  return {
    color: glass ? [1.0 * reflection, 0.74 * reflection, 0.36 * reflection] : [0.16, 0.14, 0.12],
    height: frame || cross ? 0.8 : 0.5,
    roughness: glass ? 0.12 : 0.6,
    metalness: 0,
    ao: glass ? 1 : 0.8,
    emissive: glass ? 0.85 : 0,
  };
}

function foliage(u, v, { base, seed = 11 }) {
  const clump = fbm(u, v, 10, 4, seed);
  const leaf = fbm(u * 2, v * 2, 64, 2, seed + 3);
  const highlight = smoothstep(0.62, 0.82, clump + leaf * 0.2);
  return {
    color: base.map((channel) => channel * (0.55 + clump * 0.4 + highlight * 0.25)),
    height: 0.35 + clump * 0.4 + leaf * 0.15,
    roughness: 0.9,
    metalness: 0,
    ao: 0.6 + clump * 0.35,
  };
}

function lampGlow(u, v, { seed = 12 }) {
  const distance = Math.hypot((u - 0.5) * 2, (v - 0.5) * 2);
  const halo = smoothstep(1.0, 0.1, distance);
  return {
    color: [1.0, 0.82 + halo * 0.12, 0.48 + halo * 0.2],
    height: 0.5,
    roughness: 0.18,
    metalness: 0,
    ao: 1,
    emissive: 0.6 + halo * 0.4,
  };
}

function awning(u, v, { base, seed = 13 }) {
  const stripe = Math.floor(u * 8) % 2 === 0;
  const weave = fbm(u * 4, v * 48, 64, 1, seed);
  return {
    color: (stripe ? base : [0.92, 0.88, 0.78]).map((channel) => channel * (0.9 + weave * 0.12)),
    height: 0.5 + weave * 0.12,
    roughness: 0.82,
    metalness: 0,
    ao: 0.95,
  };
}

const MATERIALS = [
  {
    name: 'brick_red',
    kind: brick,
    options: {
      base: [0.62, 0.24, 0.16],
      mortar: [0.68, 0.64, 0.56],
      rows: 12,
      columns: 6,
      seed: 21,
      variation: 0.2,
    },
    normalStrength: 1.6,
  },
  {
    name: 'brick_grey',
    kind: brick,
    options: {
      base: [0.46, 0.44, 0.41],
      mortar: [0.66, 0.63, 0.57],
      rows: 12,
      columns: 6,
      seed: 22,
      variation: 0.14,
    },
    normalStrength: 1.6,
  },
  {
    name: 'plaster',
    kind: plaster,
    options: { base: [0.86, 0.78, 0.64], seed: 23 },
    normalStrength: 0.9,
  },
  {
    name: 'roof_tile',
    kind: roofTiles,
    options: { base: [0.48, 0.16, 0.12], seed: 24 },
    normalStrength: 1.8,
  },
  {
    name: 'wood',
    kind: woodPlanks,
    options: { base: [0.46, 0.29, 0.16], seed: 25, planks: 6 },
    normalStrength: 1.6,
  },
  {
    name: 'metal',
    kind: metalPanels,
    options: { base: [0.46, 0.5, 0.55], seed: 26 },
    normalStrength: 1.0,
  },
  {
    name: 'cobble',
    kind: cobble,
    options: { base: [0.5, 0.5, 0.48], seed: 27 },
    normalStrength: 1.0,
  },
  {
    name: 'stone',
    kind: stoneBlocks,
    options: { base: [0.66, 0.64, 0.58], seed: 28 },
    normalStrength: 1.6,
  },
  {
    name: 'grass',
    kind: grass,
    options: { base: [0.30, 0.55, 0.20], seed: 29 },
    normalStrength: 1.2,
  },
  {
    name: 'water',
    kind: water,
    options: { base: [0.10, 0.30, 0.38], seed: 30 },
    normalStrength: 1.6,
  },
  {
    name: 'window',
    kind: windowPane,
    options: { seed: 31 },
    normalStrength: 1.0,
  },
  {
    name: 'foliage',
    kind: foliage,
    options: { base: [0.32, 0.58, 0.22], seed: 32 },
    normalStrength: 1.2,
  },
  {
    name: 'lamp',
    kind: lampGlow,
    options: { seed: 33 },
    normalStrength: 0.4,
  },
  {
    name: 'awning',
    kind: awning,
    options: { base: [0.62, 0.12, 0.1], seed: 34 },
    normalStrength: 0.8,
  },
];

function writeSemantic(directory, name, semantic, pixels) {
  writeFileSync(join(directory, `${name}.${semantic}.png`), encodePng({ width: SIZE, height: SIZE, data: pixels }));
}

function normalFromHeight(height) {
  const out = Buffer.alloc(SIZE * SIZE * 4);
  for (let y = 0; y < SIZE; y += 1) {
    for (let x = 0; x < SIZE; x += 1) {
      const left = height[y * SIZE + wrap(x - 1, SIZE)];
      const right = height[y * SIZE + wrap(x + 1, SIZE)];
      const up = height[wrap(y - 1, SIZE) * SIZE + x];
      const down = height[wrap(y + 1, SIZE) * SIZE + x];
      let nx = -(right - left) * 2;
      let ny = -(down - up) * 2;
      let nz = 1;
      const length = Math.hypot(nx, ny, nz) || 1;
      nx /= length;
      ny /= length;
      nz /= length;
      const offset = (y * SIZE + x) * 4;
      out[offset] = Math.round((nx * 0.5 + 0.5) * 255);
      out[offset + 1] = Math.round((ny * 0.5 + 0.5) * 255);
      out[offset + 2] = Math.round((nz * 0.5 + 0.5) * 255);
      out[offset + 3] = 255;
    }
  }
  return out;
}

function generateMaterial(directory, spec) {
  const albedo = Buffer.alloc(SIZE * SIZE * 4);
  const orm = Buffer.alloc(SIZE * SIZE * 4);
  const height = new Float32Array(SIZE * SIZE);
  for (let y = 0; y < SIZE; y += 1) {
    for (let x = 0; x < SIZE; x += 1) {
      const u = (x + 0.5) / SIZE;
      const v = (y + 0.5) / SIZE;
      const sample = spec.kind(u, v, spec.options);
      const offset = (y * SIZE + x) * 4;
      albedo[offset] = Math.round(clamp01(sample.color[0]) * 255);
      albedo[offset + 1] = Math.round(clamp01(sample.color[1]) * 255);
      albedo[offset + 2] = Math.round(clamp01(sample.color[2]) * 255);
      albedo[offset + 3] = 255;
      orm[offset] = Math.round(clamp01(sample.ao) * 255);
      orm[offset + 1] = Math.round(clamp01(sample.roughness) * 255);
      orm[offset + 2] = Math.round(clamp01(sample.metalness) * 255);
      // ORM's alpha channel carries emissive coverage; the ORM is otherwise three-channel.
      orm[offset + 3] = Math.round(clamp01(sample.emissive ?? 0) * 255);
      height[y * SIZE + x] = sample.height * spec.normalStrength;
    }
  }
  writeSemantic(directory, spec.name, 'albedo', albedo);
  writeSemantic(directory, spec.name, 'normal', normalFromHeight(height));
  writeSemantic(directory, spec.name, 'orm', orm);
  return { name: spec.name, bytes: albedo.length + orm.length + SIZE * SIZE * 4 };
}

function writeFlat(directory) {
  const albedo = Buffer.alloc(SIZE * SIZE * 4, 255);
  const normal = Buffer.alloc(SIZE * SIZE * 4);
  const orm = Buffer.alloc(SIZE * SIZE * 4);
  for (let i = 0; i < SIZE * SIZE; i += 1) {
    const offset = i * 4;
    normal[offset] = 128;
    normal[offset + 1] = 128;
    normal[offset + 2] = 255;
    normal[offset + 3] = 255;
    orm[offset] = 255;
    orm[offset + 1] = 255;
    orm[offset + 2] = 255;
    orm[offset + 3] = 0;
  }
  writeSemantic(directory, 'flat', 'albedo', albedo);
  writeSemantic(directory, 'flat', 'normal', normal);
  writeSemantic(directory, 'flat', 'orm', orm);
}

function main() {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
  const directory = join(root, 'assets', 'source', 'textures');
  mkdirSync(directory, { recursive: true });
  const generated = MATERIALS.map((spec) => generateMaterial(directory, spec));
  writeFlat(directory);
  const total = generated.reduce((sum, entry) => sum + entry.bytes, 0) + SIZE * SIZE * 4 * 3;
  writeFileSync(join(directory, 'sources.json'), `${JSON.stringify({
    schema: 'nativegpu.texture-sources/1',
    size: SIZE,
    materials: generated.map((entry) => entry.name),
    rawBytes: total,
  }, null, 2)}\n`);
  console.log(`generated ${generated.length} material sets + flat defaults in ${directory}`);
}

main();

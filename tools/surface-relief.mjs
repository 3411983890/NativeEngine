#!/usr/bin/env node
// Offline surface-relief baker: composes stone and mud height into one
// versioned R16 height asset in ground coordinates, and (later stages) bakes
// the displaced ground mesh. Artifacts stay inside the user project.
//
//   node tools/surface-relief.mjs bake --project <project> [--resolution 2048]
//        [--stone-depth .025] [--mud-thickness .008] [--mode pom]
//   node tools/surface-relief.mjs inspect --project <project>
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  NGTEX_FORMAT_R16, makeMipsR16, readNgtex, r16Buffer, writeNgtex,
} from './textures.mjs';
import { compileTextureMapping } from '../engine/material/texture-mapping.mjs';
import { migrateLayeredMaterialToV2, normalizeLayeredMaterial } from '../engine/material/layered.mjs';
import { applyMaterialPatch } from './material.mjs';
import { commitSceneUpdate } from '../engine/scene/index.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DEFAULT_STONE_DEPTH = .025;
const DEFAULT_MUD_THICKNESS = .008;

function parseArgs(argv) {
  const options = { command: argv[0] ?? 'bake' };
  for (let index = 1; index < argv.length; index += 1) {
    const arg = argv[index];
    if (!arg.startsWith('--')) throw new Error(`unexpected argument ${arg}`);
    const key = arg.slice(2);
    const value = argv[index + 1] && !argv[index + 1].startsWith('--') ? argv[++index] : 'true';
    options[key] = value;
  }
  return options;
}
function numberOption(options, key, fallback, min, max) {
  const value = options[key] === undefined ? fallback : Number(options[key]);
  if (!Number.isFinite(value) || value < min || value > max) throw new Error(`--${key} must be in [${min}, ${max}]`);
  return value;
}
function projectScene(project) {
  const file = join(project, '.screen', 'main.scene.json');
  if (!existsSync(file)) throw new Error(`scene not found: ${file}`);
  const scene = JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
  return { file, scene };
}
function layeredMaterial(scene, name) {
  const materials = Object.entries(scene.materials ?? {}).filter(([, material]) => material.layered);
  if (name) {
    const found = scene.materials?.[name];
    if (!found?.layered) throw new Error(`material ${name} has no layered authoring`);
    return { name, material: found };
  }
  if (materials.length !== 1) throw new Error(`project has ${materials.length} layered materials; pass --material`);
  return { name: materials[0][0], material: materials[0][1] };
}

// --- texture sampling ------------------------------------------------------
function wrapCoordinate(value, mode) {
  if (mode === 'clamp') return Math.min(1, Math.max(0, value));
  if (mode === 'mirror') {
    const folded = Math.abs(value) % 2;
    return folded > 1 ? 2 - folded : folded;
  }
  return value - Math.floor(value);
}
function loadHeightSampler(textureDirectory, name, semantic, channel) {
  const file = join(textureDirectory, `${name}.${semantic}.ngtex`);
  if (!existsSync(file)) throw new Error(`missing cooked height texture ${file}`);
  const bytes = readFileSync(file);
  const container = readNgtex(bytes, file);
  if (container.semantic !== semantic) throw new Error(`${file}: expected ${semantic} semantic`);
  const first = container.mips[0];
  const read = container.format === NGTEX_FORMAT_R16
    ? (x, y) => first.data.readUInt16LE((y * first.width + x) * 2) / 65535
    : (x, y) => first.data[(y * first.width + x) * 4 + channel] / 255;
  const { width, height } = first;
  return (u, v, wrapS, wrapT) => {
    const x = wrapCoordinate(u, wrapS) * width - .5;
    const y = wrapCoordinate(v, wrapT) * height - .5;
    const x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
    const wrapIndex = (value, limit, mode) => mode === 'clamp'
      ? Math.min(limit - 1, Math.max(0, value))
      : ((value % limit) + limit) % limit;
    const ix0 = wrapIndex(x0, width, wrapS), ix1 = wrapIndex(x0 + 1, width, wrapS);
    const iy0 = wrapIndex(y0, height, wrapT), iy1 = wrapIndex(y0 + 1, height, wrapT);
    const a = read(ix0, iy0) * (1 - fx) + read(ix1, iy0) * fx;
    const b = read(ix0, iy1) * (1 - fx) + read(ix1, iy1) * fx;
    return a * (1 - fy) + b * fy;
  };
}
function sampled(slot, u, v) {
  if ((slot?.uv?.set ?? 0) !== 0) throw new Error(`surface-relief bake requires UV set 0 (found ${slot.uv.set})`);
  const mapping = compileTextureMapping(slot?.uv?.mapping);
  const mappedU = mapping.row0[0] * u + mapping.row0[1] * v + mapping.row0[2];
  const mappedV = mapping.row1[0] * u + mapping.row1[1] * v + mapping.row1[2];
  return [mappedU, mappedV, slot?.uv?.mapping?.wrapS ?? 'repeat', slot?.uv?.mapping?.wrapT ?? 'repeat'];
}
function channelIndex(channel) { return { r: 0, g: 1, b: 2, a: 3 }[channel ?? 'r'] ?? 0; }
function layerById(normalized, id) {
  const layer = normalized.layers.find((entry) => entry.id === id);
  if (!layer) throw new Error(`layer ${id} is missing`);
  return layer;
}
function compositeSettings(normalized, options) {
  const configured = normalized.surfaceRelief?.displacementBake?.composite ?? null;
  const baseLayer = options['base-layer'] ?? configured?.baseLayer ?? normalized.layers[0].id;
  const overlayDefault = normalized.layers.find((layer) => /mud|wet|soil/i.test(layer.id))?.id ?? normalized.layers[1]?.id;
  const overlayLayer = options['overlay-layer'] ?? configured?.overlayLayer ?? overlayDefault;
  if (!overlayLayer) throw new Error('a composite height bake needs an overlay layer');
  const overlay = layerById(normalized, overlayLayer);
  const maskDefault = normalized.layers.find((layer) => layer.blend.texture)?.id ?? overlayLayer;
  const overlayMaskLayer = options['mask-layer'] ?? configured?.overlayMaskLayer ?? maskDefault;
  return {
    baseLayer, overlayLayer, overlayMaskLayer,
    resolution: Math.round(numberOption(options, 'resolution', configured?.resolution ?? 2048, 16, 8192)),
    groundMetres: numberOption(options, 'ground', configured?.groundMetres ?? 16, .25, 4096),
    stoneDepth: numberOption(options, 'stone-depth', DEFAULT_STONE_DEPTH, 0, .5),
    mudThickness: numberOption(options, 'mud-thickness', DEFAULT_MUD_THICKNESS, 0, .5),
    mode: options.mode ?? normalized.surfaceRelief?.mode ?? 'pom',
  };
}

export function bakeCompositeHeight({ project, scenePath, material, options = {} }) {
  const { file, scene } = projectScene(project);
  const target = layeredMaterial(scene, material);
  const normalized = normalizeLayeredMaterial(target.material.layered);
  const settings = compositeSettings(normalized, options);
  if (!(settings.stoneDepth > 0)) throw new Error('stone depth must be positive');
  const textures = join(project, 'assets', 'textures');
  const base = layerById(normalized, settings.baseLayer);
  const overlay = layerById(normalized, settings.overlayLayer);
  const maskLayer = layerById(normalized, settings.overlayMaskLayer);
  const heightSlot = base.textures.height;
  if (!heightSlot) throw new Error(`base layer ${base.id} has no height texture`);
  const overlayHeight = overlay.textures.height;
  if (!overlayHeight) throw new Error(`overlay layer ${overlay.id} has no height texture`);
  const maskRef = maskLayer.blend.texture;
  if (!maskRef) throw new Error(`mask layer ${maskLayer.id} has no blend texture`);
  const readStone = loadHeightSampler(textures, heightSlot.name, 'height', channelIndex(heightSlot.channel));
  const readMud = loadHeightSampler(textures, overlayHeight.name, 'height', channelIndex(overlayHeight.channel));
  const readMask = loadHeightSampler(textures, maskRef.name, 'mask', channelIndex(maskRef.channel));
  const identity = {
    baker: 2,
    scene: scenePath ?? file, material: target.name, resolution: settings.resolution, groundMetres: settings.groundMetres,
    stoneDepth: settings.stoneDepth, mudThickness: settings.mudThickness, base: settings.baseLayer, overlay: settings.overlayLayer, mask: settings.overlayMaskLayer,
    heights: [[heightSlot.name, 'height'], [overlayHeight.name, 'height'], [maskRef.name, 'mask']].map(([name, semantic]) => createHash('sha256').update(readFileSync(join(textures, `${name}.${semantic}.ngtex`))).digest('hex')),
  };
  const cacheKey = createHash('sha256').update(JSON.stringify(identity)).digest('hex');
  const outputName = 'ground_composite';
  const outputFile = join(textures, `${outputName}.height.ngtex`);
  const reportFile = join(project, 'reports', 'surface-relief', 'bake.json');
  if (options['skip-cache'] !== 'true' && existsSync(outputFile) && existsSync(reportFile)) {
    try {
      const report = JSON.parse(readFileSync(reportFile, 'utf8'));
      if (report.cacheKey === cacheKey) return { cacheKey, cacheHit: true, outputFile, reportFile, report, settings };
    } catch { /* rebake */ }
  }
  const resolution = settings.resolution;
  const total = settings.stoneDepth + settings.mudThickness;
  const samples = new Uint16Array(resolution * resolution);
  const blend = overlay.blend;
  let minimum = 1, maximum = 0;
  for (let y = 0; y < resolution; y += 1) {
    const v = ((y + .5) / resolution * 2 - 1) * settings.groundMetres * .5;
    for (let x = 0; x < resolution; x += 1) {
      const u = ((x + .5) / resolution * 2 - 1) * settings.groundMetres * .5;
      const [su, sv, sws, swt] = sampled(heightSlot, u, v);
      const [mu, mv, mws, mwt] = sampled(overlayHeight, u, v);
      const [ku, kv, kws, kwt] = sampled(maskRef, u, v);
      const stone = readStone(su, sv, sws, swt);
      const mud = readMud(mu, mv, mws, mwt);
      const raw = readMask(ku, kv, kws, kwt);
      const mask = Math.min(1, Math.max(0, ((blend.invert ? 1 - raw : raw) * blend.gain + blend.bias)));
      const coverage = Math.min(1, Math.max(0, blend.weight * mask));
      // Stone is the base surface; mud tops up from it and never penetrates it.
      const stoneMetres = settings.stoneDepth * stone;
      const mudMetres = settings.mudThickness * mud * coverage;
      const height = Math.max(stoneMetres, stoneMetres + mudMetres) / total;
      const value = Math.max(0, Math.min(65535, Math.round(height * 65535)));
      samples[y * resolution + x] = value;
      if (value < minimum) minimum = value;
      if (value > maximum) maximum = value;
    }
  }
  const mips = makeMipsR16({ width: resolution, height: resolution, data: samples }).map((mip) => ({ width: mip.width, height: mip.height, data: r16Buffer(mip) }));
  const container = writeNgtex({ mips, semantic: 'height', format: NGTEX_FORMAT_R16, srgb: false });
  writeFileSync(outputFile, container);
  const report = {
    schema: 'nativegpu.surface-relief-bake/1', cacheKey, resolution, groundMetres: settings.groundMetres,
    stoneDepthMetres: settings.stoneDepth, mudThicknessMetres: settings.mudThickness, totalDepthMetres: total,
    referenceHeight: maximum / 65535,
    layers: { base: settings.baseLayer, overlay: settings.overlayLayer, mask: settings.overlayMaskLayer },
    output: `${outputName}.height.ngtex`, bytes: container.length, mipCount: mips.length,
    range: { min: minimum / 65535, max: maximum / 65535 }, identity,
    note: 'Average mips are means, not conservative bounds. Raw 16-bit samples remain in mip 0.',
  };
  mkdirSync(dirname(reportFile), { recursive: true });
  writeFileSync(reportFile, `${JSON.stringify(report, null, 2)}\n`);
  return { cacheKey, cacheHit: false, outputFile, reportFile, report, settings };
}

function groundEntity(scene, material) {
  const entity = (scene.entities ?? []).find((entry) => entry.kind === 'ground' && entry.material === material);
  if (!entity) throw new Error(`no ground entity uses material ${material}`);
  return entity;
}

/**
 * Deterministic static displacement mesh in the entity's normalized local space.
 * Grid resolution is capped by the loader (1M vertices / 3M indices) and the
 * 64 MiB mesh budget; 512 cells per 16 m is 3.9 cm, so millimetre joints are
 * not claimed to be geometrically resolved.
 */
export function bakeDisplacementMesh({ project, scenePath, material, bake, options = {} }) {
  const { scene } = projectScene(project);
  const entity = groundEntity(scene, material);
  const [scaleX, scaleY, scaleZ] = entity.scale;
  if (!(scaleX > 0 && scaleY > 0 && scaleZ > 0)) throw new Error('ground entity scale must be positive');
  if (Math.abs(scaleX - scaleZ) > 1e-6) throw new Error('displacement bake requires an isotropic XZ ground scale');
  const ground = bake.settings.groundMetres;
  const cells = Math.round(numberOption(options, 'cells', 512, 8, 1024));
  if ((cells + 1) * (cells + 1) > 950000) throw new Error('--cells exceeds the vertex budget');
  if (cells * cells * 6 > 2900000) throw new Error('--cells exceeds the index budget');
  const textures = join(project, 'assets', 'textures');
  const sample = loadHeightSampler(textures, 'ground_composite', 'height', 0);
  const amplitude = bake.report.totalDepthMetres;
  const reference = bake.report.referenceHeight;
  const half = ground * .5;
  const height = (x, z) => (sample((x + half) / ground, (z + half) / ground, 'clamp', 'clamp') - reference) * amplitude;
  const step = ground / cells;
  const vertices = [];
  for (let iz = 0; iz <= cells; iz += 1) {
    const z = (iz / cells - .5) * ground;
    for (let ix = 0; ix <= cells; ix += 1) {
      const x = (ix / cells - .5) * ground;
      const h = height(x, z);
      const gx = (height(x + step, z) - height(x - step, z)) / (2 * step);
      const gz = (height(x, z + step) - height(x, z - step)) / (2 * step);
      const length = Math.hypot(-gx, 1, -gz);
      const nx = -gx / length, ny = 1 / length, nz = -gz / length;
      const lx = nx * scaleX, ly = ny * scaleY, lz = nz * scaleZ;
      const localLength = Math.hypot(lx, ly, lz) || 1;
      const round = (value) => Math.round(value * 1e6) / 1e6;
      vertices.push([
        round((ix / cells - .5) * ground / scaleX), round(.5 + h / scaleY), round((iz / cells - .5) * ground / scaleZ),
        round(lx / localLength), round(ly / localLength), round(lz / localLength), round(x), round(z),
      ]);
    }
  }
  const indices = [];
  for (let iz = 0; iz < cells; iz += 1) {
    for (let ix = 0; ix < cells; ix += 1) {
      const a = iz * (cells + 1) + ix, b = a + 1, c = a + cells + 1, d = c + 1;
      indices.push(a, c, b, b, c, d);
    }
  }
  // Closed outer rim down to the slab bottom, meeting the surrounding base box.
  const boundary = [];
  for (let ix = 0; ix <= cells; ix += 1) boundary.push(ix);
  for (let iz = 1; iz <= cells; iz += 1) boundary.push(iz * (cells + 1) + cells);
  for (let ix = cells - 1; ix >= 0; ix -= 1) boundary.push(cells * (cells + 1) + ix);
  for (let iz = cells - 1; iz >= 1; iz -= 1) boundary.push(iz * (cells + 1));
  const round = (value) => Math.round(value * 1e6) / 1e6;
  for (let i = 0; i < boundary.length; i += 1) {
    const a = boundary[i], b = boundary[(i + 1) % boundary.length];
    const pa = vertices[a], pb = vertices[b];
    const wx = pa[0] * scaleX, wz = pa[2] * scaleZ;
    const outward = Math.abs(wx) >= half - 1e-5 ? [Math.sign(wx), 0, 0] : [0, 0, Math.sign(wz)];
    const add = (position) => {
      vertices.push([round(position[0]), round(position[1]), round(position[2]), outward[0], 0, outward[2], round(position[0] * scaleX), round(position[2] * scaleZ)]);
      return vertices.length - 1;
    };
    const topA = add([pa[0], pa[1], pa[2]]), topB = add([pb[0], pb[1], pb[2]]);
    const bottomB = add([pb[0], -.5, pb[2]]), bottomA = add([pa[0], -.5, pa[2]]);
    indices.push(topA, topB, bottomB, topA, bottomB, bottomA);
  }
  const text = `${JSON.stringify({ schema: 'nativegpu.mesh/1', vertices, indices })}\n`;
  const bytes = Buffer.from(text);
  if (bytes.length > 64 * 1024 * 1024) throw new Error(`displacement mesh is ${bytes.length} bytes; reduce --cells`);
  const relative = 'meshes/ground_surface_relief.mesh.json';
  const output = join(project, 'assets', relative);
  mkdirSync(dirname(output), { recursive: true });
  writeFileSync(output, bytes);
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  let min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (const vertex of vertices) for (let axis = 0; axis < 3; axis += 1) { min[axis] = Math.min(min[axis], vertex[axis]); max[axis] = Math.max(max[axis], vertex[axis]); }
  const reportFile = join(project, 'reports', 'surface-relief', 'mesh.json');
  const report = {
    schema: 'nativegpu.surface-relief-mesh/1', file: relative, sha256, cells, cellsPerMetre: cells / ground,
    metresPerCell: ground / cells, vertices: vertices.length, triangles: indices.length / 3,
    bytes: bytes.length, bounds: { min, max }, scale: entity.scale, entity: entity.id,
    relief: { amplitudeMetres: amplitude, referenceHeight: reference, minimumMetres: (0 - reference) * amplitude, maximumMetres: (1 - reference) * amplitude },
    lod: { levels: 1, note: 'Runtime mesh LOD selection is not available; LOD0 is used.' },
    limitations: ['Raytraced shadows still use the box proxy until the BLAS is built from convex meshes.', 'Millimetre joint detail is below the 3.9 cm mesh resolution at 512 cells/16 m.'],
  };
  mkdirSync(dirname(reportFile), { recursive: true });
  writeFileSync(reportFile, `${JSON.stringify(report, null, 2)}\n`);
  return { file: relative, sha256, report, reportFile };
}

export async function applyDisplacementScene({ project, material, mesh }) {
  const { file, scene } = projectScene(project);
  const entity = groundEntity(scene, material);
  const next = structuredClone(scene);
  const target = next.entities.find((entry) => entry.id === entity.id);
  target.mesh = { file: mesh.file, sha256: mesh.sha256 };
  await commitSceneUpdate(file, { projectId: scene.projectId, expectedRevision: scene.revision }, () => next);
  const saved = JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
  return { revision: saved.revision, entity: entity.id, mesh: target.mesh };
}

/**
 * Error-constrained collision height. The visual mesh may have fine joints;
 * collision smooths them. The first resolution meeting the error budget wins.
 */
export function bakeCollisionHeight({ project, bake, options = {} }) {
  const ground = bake.settings.groundMetres;
  const half = ground * .5;
  const textures = join(project, 'assets', 'textures');
  const sample = loadHeightSampler(textures, 'ground_composite', 'height', 0);
  const amplitude = bake.report.totalDepthMetres, reference = bake.report.referenceHeight;
  const height = (x, z) => (sample((x + half) / ground, (z + half) / ground, 'clamp', 'clamp') - reference) * amplitude;
  const budget = numberOption(options, 'collision-error', .005, 1e-6, .25);
  const resolutions = [Math.round(numberOption(options, 'collision-cells', 1024, 32, 2048))];
  let chosen = null;
  for (const cells of resolutions) {
    const samples = new Float64Array((cells + 1) * (cells + 1));
    let minimum = Infinity, maximum = -Infinity;
    for (let z = 0; z <= cells; z += 1) for (let x = 0; x <= cells; x += 1) {
      const value = height((x / cells - .5) * ground, (z / cells - .5) * ground);
      samples[z * (cells + 1) + x] = value;
      minimum = Math.min(minimum, value); maximum = Math.max(maximum, value);
    }
    const sampleGrid = (x, z) => {
      const fx = Math.min(cells, Math.max(0, (x / ground + .5) * cells));
      const fz = Math.min(cells, Math.max(0, (z / ground + .5) * cells));
      const x0 = Math.floor(fx), z0 = Math.floor(fz), tx = fx - x0, tz = fz - z0;
      const x1 = Math.min(cells, x0 + 1), z1 = Math.min(cells, z0 + 1);
      const a = samples[z0 * (cells + 1) + x0] * (1 - tx) + samples[z0 * (cells + 1) + x1] * tx;
      const b = samples[z1 * (cells + 1) + x0] * (1 - tx) + samples[z1 * (cells + 1) + x1] * tx;
      return a * (1 - tz) + b * tz;
    };
    let error = 0;
    const checks = Math.max(256, cells * 3);
    for (let z = 0; z < checks; z += 1) for (let x = 0; x < checks; x += 1) {
      const wx = ((x + .5) / checks - .5) * ground, wz = ((z + .5) / checks - .5) * ground;
      error = Math.max(error, Math.abs(sampleGrid(wx, wz) - height(wx, wz)));
    }
    chosen = { cells, samples, minimum, maximum, error };
    if (error <= budget) break;
  }
  const normalized = new Uint16Array((chosen.cells + 1) * (chosen.cells + 1));
  const span = Math.max(1e-9, chosen.maximum - chosen.minimum);
  for (let i = 0; i < normalized.length; i += 1) normalized[i] = Math.max(0, Math.min(65535, Math.round((chosen.samples[i] - chosen.minimum) / span * 65535)));
  const mips = makeMipsR16({ width: chosen.cells + 1, height: chosen.cells + 1, data: normalized }).map((mip) => ({ width: mip.width, height: mip.height, data: r16Buffer(mip) }));
  const bytes = writeNgtex({ mips, semantic: 'height', format: NGTEX_FORMAT_R16, srgb: false });
  const relative = 'textures/ground_collision.height.ngtex';
  const output = join(project, 'assets', relative);
  mkdirSync(dirname(output), { recursive: true });
  writeFileSync(output, bytes);
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  const reportFile = join(project, 'reports', 'surface-relief', 'collision.json');
  const report = {
    schema: 'nativegpu.surface-relief-collision/1', file: relative, sha256,
    origin: [-half, -half], size: [ground, ground], resolution: [chosen.cells + 1, chosen.cells + 1],
    minHeightMetres: chosen.minimum, maxHeightMetres: chosen.maximum,
    maxErrorMetres: chosen.error, errorBudgetMetres: budget, budgetMet: chosen.error <= budget, bytes: bytes.length, mipCount: mips.length,
    note: 'Collision mips average heights for streaming; they are not conservative bounds.',
  };
  mkdirSync(dirname(reportFile), { recursive: true });
  writeFileSync(reportFile, `${JSON.stringify(report, null, 2)}\n`);
  return { report, reportFile, output };
}

export async function applyHeightfieldScene({ project, material, collision }) {
  const { file, scene } = projectScene(project);
  const entity = groundEntity(scene, material);
  const next = structuredClone(scene);
  const target = next.entities.find((entry) => entry.id === entity.id);
  target.heightfield = {
    file: collision.report.file, sha256: collision.report.sha256, enabled: true,
    origin: collision.report.origin, size: collision.report.size,
    minHeightMetres: collision.report.minHeightMetres, maxHeightMetres: collision.report.maxHeightMetres,
    maxErrorMetres: collision.report.maxErrorMetres,
  };
  await commitSceneUpdate(file, { projectId: scene.projectId, expectedRevision: scene.revision }, () => next);
  const saved = JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
  return { revision: saved.revision, entity: entity.id, heightfield: target.heightfield };
}

export async function applyCompositeMaterial({ project, scenePath, material, bake, options = {} }) {  const { file, scene } = projectScene(project);
  const target = layeredMaterial(scene, material);
  const migrated = migrateLayeredMaterialToV2(target.material.layered);
  const total = bake.settings.stoneDepth + bake.settings.mudThickness;
  migrated.surfaceRelief = {
    ...migrated.surfaceRelief,
    mode: bake.settings.mode,
    heightSource: { kind: 'texture', texture: { name: 'ground_composite', uv: { set: 0, mapping: { offset: [.5, .5], scale: [1 / bake.settings.groundMetres, 1 / bake.settings.groundMetres], rotation: 0, wrapS: 'clamp', wrapT: 'clamp', magFilter: 'linear', minFilter: 'linear-mipmap-linear' } } } },
    amplitudeMetres: total,
    referenceHeight: bake.report.referenceHeight,
    displacementBake: { ...migrated.surfaceRelief.displacementBake, composite: { resolution: bake.settings.resolution, groundMetres: bake.settings.groundMetres, baseLayer: bake.settings.baseLayer, overlayLayer: bake.settings.overlayLayer, overlayMaskLayer: bake.settings.overlayMaskLayer } },
    ...(options['fade-start'] !== undefined ? { fadeStartMetres: Number(options['fade-start']) } : {}),
    ...(options['fade-end'] !== undefined ? { fadeEndMetres: Number(options['fade-end']) } : {}),
    ...(options['self-shadow'] === 'true' ? { selfShadow: true } : {}),
    ...(options['steps'] !== undefined ? { minSteps: Math.max(1, Math.round(Number(options.steps) / 2)), maxSteps: Math.max(1, Math.round(Number(options.steps))) } : {}),
  };
  return applyMaterialPatch({ scenePath: scenePath ?? file, material: target.name, changes: { layered: migrated }, expectedRevision: scene.revision, root: project });
}

async function main(argv) {
  const options = parseArgs(argv);
  const project = resolve(options.project ?? process.cwd());
  if (!existsSync(project)) throw new Error(`project not found: ${project}`);
  if (options.command === 'bake') {
    const bake = bakeCompositeHeight({ project, scenePath: options.scene, material: options.material, options });
    let applied = null;
    if (options.apply !== 'false') applied = await applyCompositeMaterial({ project, scenePath: options.scene, material: options.material, bake, options });
    console.log(JSON.stringify({ ok: true, cacheHit: bake.cacheHit, report: bake.report, applied: applied ? { revision: applied.revision } : null }, null, 2));
    return;
  }
  if (options.command === 'mesh') {
    const bake = bakeCompositeHeight({ project, scenePath: options.scene, material: options.material, options: { ...options, mode: 'bakedDisplacement' } });
    const applied = await applyCompositeMaterial({ project, scenePath: options.scene, material: options.material, bake, options: { ...options, mode: 'bakedDisplacement' } });
    const mesh = bakeDisplacementMesh({ project, scenePath: options.scene, material: options.material, bake, options });
    const scene = await applyDisplacementScene({ project, material: options.material, mesh });
    console.log(JSON.stringify({ ok: true, cacheHit: bake.cacheHit, materialRevision: applied.revision, sceneRevision: scene.revision, entity: scene.entity, mesh: mesh.report }, null, 2));
    return;
  }
  if (options.command === 'collision') {
    const bake = bakeCompositeHeight({ project, scenePath: options.scene, material: options.material, options });
    const collision = bakeCollisionHeight({ project, bake, options });
    const scene = await applyHeightfieldScene({ project, material: options.material, collision });
    console.log(JSON.stringify({ ok: true, sceneRevision: scene.revision, entity: scene.entity, collision: collision.report }, null, 2));
    return;
  }
  if (options.command === 'inspect') {
    const reportFile = join(project, 'reports', 'surface-relief', 'bake.json');
    if (!existsSync(reportFile)) throw new Error('no bake report; run bake first');
    console.log(readFileSync(reportFile, 'utf8'));
    return;
  }
  throw new Error(`unknown command ${options.command}`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch((error) => { console.error(error.message); process.exitCode = 1; });
}

export { root };

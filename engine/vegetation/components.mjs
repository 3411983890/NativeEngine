// Plant recipe components.
//
// A species used to be exactly two things: wood (the branch graph) and leaf cards (fronds).
// Everything the extension plan calls V02/V03/V05 — a persimmon's fruit, a loquat's panicle,
// a rice plant's seed head, a maple's samara, a water lily's floating pad — had nowhere to
// live. The only options were to pretend the fruit was a leaf card (wrong material, wrong
// silhouette, wrong wind weight) or to bake it into the mesh outside the species cook (no
// stable ids, no LOD subset, no stage filter, invisible to the editor).
//
// This module is the third layer: a *component* is an authored part with its own type,
// material, size, attachment rule and stage filter, placed on a branch of the graph with a
// stable identity. It is deliberately a layer over the existing graph/leaf systems rather
// than a second geometry pipeline:
//
//   branch graph  → host arc position → component attachment → component geometry
//
// The identity rule is the one the plan states in §3.1: the random stream of an attachment is
// derived from (plant seed, component id, host id, index). Changing how many fruit a species
// carries therefore cannot re-roll the crown, the leaves, or another component's parts.
//
// Scope, stated plainly: these are generated parts. An imported mesh component is supported as
// an *instance adapter* (`type: 'imported'` with geometry handed in by the caller) because the
// plan asks for "几何实例挂接" and explicitly forbids a second decoder here. Types the plan
// lists but this module does not implement yet are rejected by name instead of being accepted
// and ignored — a declared-but-unconsumed enum is the failure mode §3.1 warns about.

import { add, arcLengthOf, cross, dot, mul, normalize, sampleBranch, sub } from './branch-graph.mjs';
import { Stream, deriveSeed } from './random.mjs';

export const COMPONENT_SCHEMA = 'nativegpu.vegetation-components/1';

// Implemented. Each one has a real emitter below and a measured triangle cost.
export const COMPONENT_TYPES = new Set([
  'fruit', 'flower', 'seedHead', 'pod', 'compoundLeaf', 'floatingLeaf', 'culm', 'imported',
]);

// Named in the plan, owned by another layer today. Rejected with a pointer instead of silently
// producing nothing, so an author cannot believe a part was baked when it was dropped.
const REDIRECTED_TYPES = new Map([
  ['leaf', 'use a frond in generators.fronds (leaf cards are placed and LOD-reduced there)'],
  ['frond', 'use a frond in generators.fronds'],
  ['stem', 'use generators.levels — stems are branch graph levels'],
  ['branch', 'use generators.levels — branches are branch graph levels'],
  ['root', 'not implemented; roots have no emitter yet'],
  ['surfacePatch', 'not implemented; moss/algae patches need a surface-decal pass, not an instanced part'],
]);

const ID = /^[a-z][a-z0-9_-]{0,63}$/;
const TAU = Math.PI * 2;
const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

export class ComponentError extends Error {
  constructor(code, message) { super(message); this.name = 'ComponentError'; this.code = code; }
}
function fail(message, code = 'NG-E-VEG-COMPONENT') { throw new ComponentError(code, message); }

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);
function num(value, low, high, label) {
  if (!isNumber(value) || value < low || value > high) fail(`${label} must be a finite number in [${low},${high}]`);
  return value;
}
function integer(value, low, high, label) {
  if (!Number.isInteger(value) || value < low || value > high) fail(`${label} must be an integer in [${low},${high}]`);
  return value;
}
// Range fields accept a scalar (a fixed value) or a [min,max] pair, the same shorthand the
// generator layer already uses, so an author does not have to learn a second convention.
function rangeOf(value, low, high, label, fallback) {
  const source = value ?? fallback;
  const pair = Array.isArray(source) ? source : [source, source];
  if (pair.length !== 2) fail(`${label} must be a number or a [min,max] pair`);
  num(pair[0], low, high, `${label}[0]`);
  num(pair[1], low, high, `${label}[1]`);
  if (pair[0] > pair[1]) fail(`${label} is inverted`);
  return [pair[0], pair[1]];
}

// ---------------------------------------------------------------- normalization
const PHYLLOTAXIS = { alternate: Math.PI, opposite: Math.PI, whorled: TAU / 3, spiral: 2.399963 };
const DISTRIBUTIONS = new Set(['along', 'tip', 'node']);

function normalizeAttach(entry, index) {
  const attach = entry.attach ?? {};
  if (typeof attach !== 'object' || Array.isArray(attach)) fail(`components[${index}].attach must be an object`);
  const distribution = attach.distribution ?? 'along';
  if (!DISTRIBUTIONS.has(distribution)) fail(`components[${index}].attach.distribution must be along, tip or node`);
  const hostLevels = attach.hostLevels ?? entry.hostLevels ?? [1];
  if (!Array.isArray(hostLevels) || hostLevels.length === 0) fail(`components[${index}].attach.hostLevels must be a non-empty array`);
  const levels = hostLevels.map((value) => integer(Math.round(Number(value)), 0, 8, `components[${index}].attach.hostLevels[]`));
  const phyllotaxis = attach.phyllotaxis ?? 'alternate';
  if (!Object.hasOwn(PHYLLOTAXIS, phyllotaxis)) fail(`components[${index}].attach.phyllotaxis must be alternate, opposite, whorled or spiral`);
  return {
    distribution,
    hostLevels: [...new Set(levels)].sort((a, b) => a - b),
    arc: rangeOf(attach.arc, 0, 1, `components[${index}].attach.arc`, [0.35, 1]),
    countPerHost: rangeOf(attach.countPerHost, 1, 512, `components[${index}].attach.countPerHost`, [1, 1]),
    spacingMetres: num(attach.spacingMetres ?? 0.1, 0.001, 10, `components[${index}].attach.spacingMetres`),
    phyllotaxis,
    whorlCount: integer(attach.whorlCount ?? 2, 1, 12, `components[${index}].attach.whorlCount`),
    spreadRadians: num(attach.spreadRadians ?? 1.1, 0, Math.PI, `components[${index}].attach.spreadRadians`),
    upBias: num(attach.upBias ?? 0, -1, 1, `components[${index}].attach.upBias`),
    droop: num(attach.droop ?? 0.1, -1, 2, `components[${index}].attach.droop`),
    scaleRange: rangeOf(attach.scaleRange, 0.05, 8, `components[${index}].attach.scaleRange`, [0.85, 1.15]),
    offsetMetres: num(attach.offsetMetres ?? 0, -0.5, 0.5, `components[${index}].attach.offsetMetres`),
    // A floating pad does not sit where its stem happens to be: it sits on the water plane the
    // author declares. Absent means "use the host surface", which is every other type.
    planeHeightMetres: attach.planeHeightMetres === undefined ? null : num(attach.planeHeightMetres, -1e4, 1e4, `components[${index}].attach.planeHeightMetres`),
  };
}

// Per-type shape. Every number that ends up as geometry is validated here, so an emitter can
// read its fields without re-checking, and a bad number fails at load instead of at bake.
function normalizeShape(entry, index, sizeMetres) {
  const shape = entry.shape ?? {};
  if (typeof shape !== 'object' || Array.isArray(shape)) fail(`components[${index}].shape must be an object`);
  switch (entry.type) {
    case 'fruit':
      return {
        form: shape.form === 'cluster' ? 'cluster' : 'ellipsoid',
        aspect: num(shape.aspect ?? 1, 0.2, 3, `components[${index}].shape.aspect`),
        rings: integer(shape.rings ?? 4, 2, 12, `components[${index}].shape.rings`),
        segments: integer(shape.segments ?? 6, 3, 16, `components[${index}].shape.segments`),
      };
    case 'flower':
      return {
        petals: integer(shape.petals ?? 5, 3, 16, `components[${index}].shape.petals`),
        cup: num(shape.cup ?? 0.25, 0, 1, `components[${index}].shape.cup`),
        centreFraction: num(shape.centreFraction ?? 0.18, 0.02, 0.6, `components[${index}].shape.centreFraction`),
        // Petal length as a fraction of the declared size, so `sizeMetres` stays the flower's
        // diameter and the shape only says how the petals divide it.
        petalLengthFraction: num(shape.petalLengthFraction ?? 0.5, 0.1, 1, `components[${index}].shape.petalLengthFraction`),
        petalWidthFraction: num(shape.petalWidthFraction ?? 0.3, 0.05, 1, `components[${index}].shape.petalWidthFraction`),
      };
    case 'seedHead':
      return {
        form: shape.form === 'panicle' ? 'panicle' : 'spike',
        lengthMetres: num(shape.lengthMetres ?? Math.max(0.02, sizeMetres * 0.25), 0.01, 4, `components[${index}].shape.lengthMetres`),
        spikelets: integer(shape.spikelets ?? 24, 2, 512, `components[${index}].shape.spikelets`),
        spikeletLengthMetres: num(shape.spikeletLengthMetres ?? 0.012, 0.002, 0.4, `components[${index}].shape.spikeletLengthMetres`),
        spikeletWidthMetres: num(shape.spikeletWidthMetres ?? 0.005, 0.001, 0.2, `components[${index}].shape.spikeletWidthMetres`),
        lateralAngleRadians: num(shape.lateralAngleRadians ?? 0.55, 0, 1.5, `components[${index}].shape.lateralAngleRadians`),
        lateralLengthMetres: num(shape.lateralLengthMetres ?? 0.05, 0.005, 1, `components[${index}].shape.lateralLengthMetres`),
        droop: num(shape.droop ?? 0.35, 0, 2, `components[${index}].shape.droop`),
        rachisRadiusMetres: num(shape.rachisRadiusMetres ?? 0.002, 0.0005, 0.05, `components[${index}].shape.rachisRadiusMetres`),
      };
    case 'pod':
      return {
        lengthMetres: num(shape.lengthMetres ?? Math.max(0.02, sizeMetres), 0.005, 2, `components[${index}].shape.lengthMetres`),
        widthMetres: num(shape.widthMetres ?? Math.max(0.005, sizeMetres * 0.22), 0.002, 1, `components[${index}].shape.widthMetres`),
        curve: num(shape.curve ?? 0.15, -1, 1, `components[${index}].shape.curve`),
        spans: integer(shape.spans ?? 4, 2, 12, `components[${index}].shape.spans`),
      };
    case 'compoundLeaf':
      return {
        rachisLengthMetres: num(shape.rachisLengthMetres ?? Math.max(0.03, sizeMetres), 0.01, 3, `components[${index}].shape.rachisLengthMetres`),
        leafletPairs: integer(shape.leafletPairs ?? 4, 1, 24, `components[${index}].shape.leafletPairs`),
        leafletLengthMetres: num(shape.leafletLengthMetres ?? Math.max(0.01, sizeMetres * 0.35), 0.002, 1, `components[${index}].shape.leafletLengthMetres`),
        leafletWidthMetres: num(shape.leafletWidthMetres ?? Math.max(0.004, sizeMetres * 0.14), 0.001, 0.6, `components[${index}].shape.leafletWidthMetres`),
        leafletAngleRadians: num(shape.leafletAngleRadians ?? 0.9, 0.05, 1.5, `components[${index}].shape.leafletAngleRadians`),
        terminalLeaflet: shape.terminalLeaflet !== false,
        rachisRadiusMetres: num(shape.rachisRadiusMetres ?? 0.0015, 0.0003, 0.03, `components[${index}].shape.rachisRadiusMetres`),
      };
    case 'floatingLeaf':
      return {
        radiusMetres: num(shape.radiusMetres ?? Math.max(0.01, sizeMetres * 0.5), 0.005, 2, `components[${index}].shape.radiusMetres`),
        notchRadians: num(shape.notchRadians ?? 0.42, 0, 2, `components[${index}].shape.notchRadians`),
        segments: integer(shape.segments ?? 12, 4, 32, `components[${index}].shape.segments`),
        rimLift: num(shape.rimLift ?? 0.02, 0, 0.5, `components[${index}].shape.rimLift`),
      };
    case 'culm':
      return {
        nodeSpacingMetres: num(shape.nodeSpacingMetres ?? 0.25, 0.02, 2, `components[${index}].shape.nodeSpacingMetres`),
        ringHeightMetres: num(shape.ringHeightMetres ?? 0.012, 0.001, 0.2, `components[${index}].shape.ringHeightMetres`),
        ringScale: num(shape.ringScale ?? 1.08, 1, 2, `components[${index}].shape.ringScale`),
        segments: integer(shape.segments ?? 8, 3, 16, `components[${index}].shape.segments`),
      };
    case 'imported':
      if (typeof shape.assetId !== 'string' || !ID.test(shape.assetId)) fail(`components[${index}].shape.assetId is not a valid identifier`);
      return {
        assetId: shape.assetId,
        digest: typeof shape.digest === 'string' ? shape.digest : null,
        // The adapter maps the imported mesh's own up axis onto the attachment frame. 1 = the
        // asset is authored Y-up like the engine; 0 = authored along +X.
        upAxis: shape.upAxis ?? 'y',
      };
    default:
      return {};
  }
}

function normalizeOne(entry, index, materialIds, seen) {
  if (!entry || typeof entry !== 'object' || Array.isArray(entry)) fail(`components[${index}] must be an object`);
  const id = String(entry.id ?? '');
  if (!ID.test(id)) fail(`components[${index}].id is not a valid identifier`);
  if (seen.has(id)) fail(`components declares id ${id} twice`);
  seen.add(id);
  const type = String(entry.type ?? '');
  if (REDIRECTED_TYPES.has(type)) {
    fail(`components[${index}] (${id}) uses type '${type}', which this layer does not implement: ${REDIRECTED_TYPES.get(type)}`);
  }
  if (!COMPONENT_TYPES.has(type)) {
    fail(`components[${index}] (${id}) has unknown type '${type}'; implemented types are ${[...COMPONENT_TYPES].join(', ')}`);
  }
  const material = entry.material ?? null;
  if (material !== null) {
    if (typeof material !== 'string') fail(`components[${index}].material must be a material id`);
    if (!materialIds.has(material)) fail(`components[${index}] (${id}) references unknown material ${material}`);
  }
  const sizeMetres = Array.isArray(entry.sizeMetres) ? (entry.sizeMetres[0] + entry.sizeMetres[1]) / 2 : (entry.sizeMetres ?? 0.05);
  const colour = entry.colour ?? entry.color ?? null;
  if (colour !== null && (!Array.isArray(colour) || colour.length < 3)) fail(`components[${index}].colour must be [r,g,b]`);
  const stages = entry.stages ?? null;
  if (stages !== null && (!Array.isArray(stages) || stages.length === 0 || stages.some((value) => typeof value !== 'string'))) {
    fail(`components[${index}].stages must be a non-empty array of stage names`);
  }
  const lodLevels = entry.lodLevels ?? null;
  if (lodLevels !== null && (!Array.isArray(lodLevels) || lodLevels.length === 0)) fail(`components[${index}].lodLevels must be a non-empty array`);
  return {
    id, type, material,
    sizeMetres: rangeOf(entry.sizeMetres, 0.002, 4, `components[${index}].sizeMetres`, 0.05),
    colour: colour === null ? null : [clamp(colour[0] | 0, 0, 255), clamp(colour[1] | 0, 0, 255), clamp(colour[2] | 0, 0, 255)],
    wind: num(entry.wind ?? 0.5, 0, 1, `components[${index}].wind`),
    stages: stages === null ? null : [...new Set(stages)],
    lodLevels: lodLevels === null ? null : [...new Set(lodLevels.map((value) => integer(Math.round(Number(value)), 0, 3, `components[${index}].lodLevels[]`)))].sort(),
    attach: normalizeAttach(entry, index),
    shape: normalizeShape(entry, index, sizeMetres),
  };
}

export function normalizeComponents(spec) {
  const source = spec?.components;
  if (source === undefined || source === null) return null;
  if (!Array.isArray(source)) fail('species.components must be an array');
  if (source.length > 32) fail('species.components declares more than 32 parts');
  const materialIds = new Set((spec.materials ?? []).map((material) => material.id));
  const seen = new Set();
  const components = source.map((entry, index) => normalizeOne(entry, index, materialIds, seen));
  return {
    schema: COMPONENT_SCHEMA,
    components,
    byId: new Map(components.map((component) => [component.id, component])),
    // Every imported asset this species depends on, so a cook can publish the dependency list
    // instead of discovering a missing mesh at load time.
    assets: components
      .filter((component) => component.type === 'imported')
      .map((component) => ({ id: component.shape.assetId, digest: component.shape.digest })),
  };
}

// ---------------------------------------------------------------- placement
function frameFrom(direction) {
  const axis = normalize(direction);
  const reference = Math.abs(axis[1]) > 0.9 ? [1, 0, 0] : [0, 1, 0];
  const side = normalize(cross(axis, reference));
  return { axis, side, up: normalize(cross(axis, side)) };
}

// Azimuth of attachment `index` on a host, per phyllotaxis. `whorled` puts `whorlCount` parts at
// the same height before advancing, which is what a node of leaves actually does; `opposite`
// alternates between two opposite sides with a quarter turn between nodes.
function azimuthFor(attach, index, phase) {
  switch (attach.phyllotaxis) {
    case 'opposite': return phase + (index % 2) * Math.PI;
    case 'whorled': return phase + (index % attach.whorlCount) * (TAU / attach.whorlCount) + Math.floor(index / attach.whorlCount) * (TAU / (attach.whorlCount * 2));
    case 'spiral': return phase + index * PHYLLOTAXIS.spiral;
    default: return phase + index * Math.PI;
  }
}

// Positions along a host for `count` attachments. `node` distributes by a real metre spacing
// (bamboo internodes, a grain ear's nodes) and deliberately ignores `countPerHost`: "a node every
// 25 cm" is a spacing, and making the author also state how many nodes a culm has would let the
// two disagree. `tip` clusters everything at the tip (a fruit cluster, a terminal panicle);
// `along` spreads evenly inside the authored arc window.
function arcPositions(attach, count, hostArc, rng) {
  if (attach.distribution === 'tip') return new Array(count).fill(hostArc);
  if (attach.distribution === 'node') {
    const spacing = Math.max(0.01, attach.spacingMetres);
    const total = Math.min(512, Math.floor(hostArc / spacing) + 1);
    return Array.from({ length: total }, (_, index) => Math.min(hostArc, index * spacing));
  }
  const [low, high] = attach.arc;
  const span = hostArc * (high - low);
  const start = hostArc * low;
  return Array.from({ length: count }, (_, index) => {
    const even = count === 1 ? 0.62 : (index + 0.5) / count;
    const jitter = Math.min(0.35 / count, 0.02);
    return clamp(start + span * clamp(even + rng.span(-jitter, jitter), 0, 1), 0, hostArc);
  });
}

// The whole placement pass. Returns parts in a deterministic order and a per-component summary
// that the cook publishes, so "the species has fruit" is a counted fact and not a claim.
export function placeComponents({ graph, components, seed, stage = null, quality = null, assets = null }) {
  if (!components) return { parts: [], perComponent: [], assets: [] };
  const fraction = clamp(quality?.components ?? 1, 0, 1);
  const lodLevel = quality?.level ?? null;
  const parts = [];
  const perComponent = [];
  for (const component of components.components) {
    const summary = {
      id: component.id, type: component.type, material: component.material,
      parts: 0, hosts: 0, triangles: 0, skippedStage: false, skippedLod: false, lodRejected: 0,
      assets: component.type === 'imported' ? [component.shape.assetId] : [],
    };
    // Stage is a filter, not a switch inside the emitter: an autumn species and a bare winter
    // species are two assets, and the cook must say which one it baked.
    if (component.stages && !component.stages.includes(stage)) {
      summary.skippedStage = true;
      perComponent.push(summary);
      continue;
    }
    if (component.lodLevels && lodLevel !== null && !component.lodLevels.includes(lodLevel)) {
      summary.skippedLod = true;
      perComponent.push(summary);
      continue;
    }
    if (component.type === 'imported' && assets && !assets[component.shape.assetId]) {
      fail(`component ${component.id} needs imported asset ${component.shape.assetId}, which was not supplied`, 'NG-E-VEG-ASSET');
    }
    const levels = new Set(component.attach.hostLevels);
    const hosts = graph.branches.filter((branch) => levels.has(branch.level));
    for (const host of hosts) {
      const rng = new Stream(deriveSeed(seed, `components/${component.id}/${host.id}`), 'components');
      const hostArc = Math.max(1e-3, arcLengthOf(host));
      const count = Math.round(rng.span(component.attach.countPerHost[0], component.attach.countPerHost[1]));
      if (count <= 0) continue;
      const phase = rng.span(0, TAU);
      const arcs = arcPositions(component.attach, count, hostArc, rng);
      let hostParts = 0;
      for (let index = 0; index < arcs.length; ++index) {
        const arc = arcs[index];
        const sample = sampleBranch(host, arc);
        const azimuth = azimuthFor(component.attach, index, phase);
        const radial = normalize(add(
          mul(sample.normal, Math.cos(azimuth)),
          mul(sample.binormal, Math.sin(azimuth))));
        // Outward first, then the authored spread away from the host tangent, then gravity.
        // Stated as a direction sum rather than an angle ladder so a hanging fruit and an
        // upright flower are the same code with different signs.
        const spread = component.attach.spreadRadians;
        const direction = normalize(add(
          add(mul(radial, Math.sin(spread)), mul(sample.tangent, Math.cos(spread))),
          [0, component.attach.upBias - component.attach.droop, 0]));
        let position = add(sample.position, mul(radial, sample.radius + component.attach.offsetMetres));
        if (component.attach.planeHeightMetres !== null) position = [position[0], component.attach.planeHeightMetres, position[2]];
        // Stable survival rank for the LOD subset. Drawn from this part's own stream so a
        // coarser LOD removes whole parts and never moves a kept one.
        const keep = rng.unit();
        if (keep >= fraction) { ++summary.lodRejected; continue; }
        const scale = rng.span(component.attach.scaleRange[0], component.attach.scaleRange[1]);
        const size = rng.span(component.sizeMetres[0], component.sizeMetres[1]);
        parts.push({
          id: `${component.id}/${host.id}/${index}`,
          componentId: component.id,
          type: component.type,
          material: component.material,
          hostBranchId: host.id,
          hostLevel: host.level,
          hostArcMetres: arc,
          position,
          direction,
          radial,
          scale,
          sizeMetres: size,
          wind: component.wind * (0.6 + 0.4 * clamp(arc / hostArc, 0, 1)),
          keep,
          colour: component.colour,
          shape: component.shape,
          geometry: component.type === 'imported' ? (assets?.[component.shape.assetId] ?? null) : null,
          frame: frameFrom(direction),
        });
        ++hostParts;
      }
      if (hostParts > 0) { ++summary.hosts; summary.parts += hostParts; }
    }
    perComponent.push(summary);
  }
  return { parts, perComponent, assets: components.assets };
}

// ---------------------------------------------------------------- emitters
// Every emitter writes into a MeshBuilder-shaped object (begin/vertex/triangle/quad/end) and
// returns its triangle count. Winding follows the convention in branch-mesh: a quad is ordered
// around-then-along, so an explicitly computed outward normal and the face winding agree.
function colourOf(part, fallback) {
  const base = part.colour ?? fallback;
  return [base[0], base[1], base[2], 255];
}

function emitFruit(mesh, part, ctx) {
  const { axis, side, up } = part.frame;
  const radius = part.sizeMetres * 0.5 * part.scale;
  const { rings, segments, aspect } = part.shape;
  const centre = add(part.position, mul(axis, radius * aspect * 0.85));
  const colour = colourOf(part, [196, 96, 40]);
  mesh.begin(ctx.materialId);
  // Only the *interior* latitudes become rings. A latitude at phi = 0 or pi has zero radius, so
  // emitting it as a full ring would put `segments` coincident vertices there and add zero-area
  // triangles to every fruit; the two ends are single vertices with a fan instead.
  const rows = [];
  for (let ring = 1; ring < rings; ++ring) {
    const phi = Math.PI * (1 - ring / rings);
    const ringRadius = Math.sin(phi) * radius;
    const height = Math.cos(phi) * radius * aspect;
    const row = [];
    for (let segment = 0; segment < segments; ++segment) {
      const theta = (segment / segments) * TAU;
      const normal = normalize(add(add(mul(side, Math.cos(theta) * Math.sin(phi)), mul(up, Math.sin(theta) * Math.sin(phi))), mul(axis, Math.cos(phi))));
      const position = add(add(centre, mul(axis, height)), add(mul(side, Math.cos(theta) * ringRadius), mul(up, Math.sin(theta) * ringRadius)));
      row.push(mesh.vertex(position, normal, [segment / segments, ring / rings], colour, part.wind));
    }
    rows.push(row);
  }
  let triangles = 0;
  for (let ring = 0; ring + 1 < rows.length; ++ring) {
    for (let segment = 0; segment < segments; ++segment) {
      const next = (segment + 1) % segments;
      mesh.quad(rows[ring][segment], rows[ring][next], rows[ring + 1][next], rows[ring + 1][segment]);
      triangles += 2;
    }
  }
  const bottom = mesh.vertex(sub(centre, mul(axis, radius * aspect)), mul(axis, -1), [0.5, 0], colour, part.wind);
  const top = mesh.vertex(add(centre, mul(axis, radius * aspect)), axis, [0.5, 1], colour, part.wind);
  for (let segment = 0; segment < segments; ++segment) {
    const next = (segment + 1) % segments;
    mesh.triangle(rows[0][next], rows[0][segment], bottom); triangles += 1;
    mesh.triangle(rows[rows.length - 1][segment], rows[rows.length - 1][next], top); triangles += 1;
  }
  mesh.end();
  return triangles;
}

function emitFlower(mesh, part, ctx) {
  const { axis, side, up } = part.frame;
  const diameter = part.sizeMetres * part.scale;
  const petalLength = diameter * part.shape.petalLengthFraction;
  const petalWidth = diameter * part.shape.petalWidthFraction;
  const colour = colourOf(part, [232, 214, 232]);
  const centreColour = colourOf(part, [246, 226, 150]);
  const petals = part.shape.petals;
  const centre = add(part.position, mul(axis, petalLength * 0.02));
  mesh.begin(ctx.materialId);
  let triangles = 0;
  // Centre first: a small fan standing slightly proud of the petals, so a flower seen from
  // above is not a hole.
  const centreRing = [];
  const centreRadius = diameter * part.shape.centreFraction;
  for (let segment = 0; segment < 6; ++segment) {
    const theta = (segment / 6) * TAU;
    const radial = add(mul(side, Math.cos(theta)), mul(up, Math.sin(theta)));
    centreRing.push(mesh.vertex(add(add(centre, mul(radial, centreRadius)), mul(axis, 0.004 * diameter)),
      axis, [segment / 6, 0.5], centreColour, part.wind));
  }
  const centreTip = mesh.vertex(add(centre, mul(axis, 0.01 * diameter)), axis, [0.5, 1], centreColour, part.wind);
  for (let segment = 0; segment < 6; ++segment) {
    mesh.triangle(centreRing[segment], centreRing[(segment + 1) % 6], centreTip); triangles += 1;
  }
  // Petals: a tapered triangle pair per petal, tilted up by `cup` so the flower is a shallow
  // bowl. The tip is one vertex, not a collapsed row of three: three coincident vertices would
  // add two zero-area triangles per petal and break tangent generation.
  for (let petal = 0; petal < petals; ++petal) {
    const theta = (petal / petals) * TAU + 0.3;
    const radial = add(mul(side, Math.cos(theta)), mul(up, Math.sin(theta)));
    const across = add(mul(side, -Math.sin(theta)), mul(up, Math.cos(theta)));
    const tip = add(add(centre, mul(radial, petalLength)), mul(axis, petalLength * part.shape.cup));
    const base = add(centre, mul(radial, centreRadius * 0.7));
    const left = add(base, mul(across, petalWidth * 0.5));
    const right = sub(base, mul(across, petalWidth * 0.5));
    const normal = normalize(add(mul(axis, 1), mul(radial, -0.15)));
    const iLeft = mesh.vertex(left, normal, [0, 0], colour, part.wind);
    const iBase = mesh.vertex(base, normal, [0.5, 0], colour, part.wind);
    const iRight = mesh.vertex(right, normal, [1, 0], colour, part.wind);
    const iTip = mesh.vertex(tip, normal, [0.5, 1], colour, part.wind);
    mesh.triangle(iLeft, iBase, iTip); triangles += 1;
    mesh.triangle(iBase, iRight, iTip); triangles += 1;
  }
  mesh.end();
  return triangles;
}

function emitSeedHead(mesh, part, ctx) {
  const { axis, side, up } = part.frame;
  const shape = part.shape;
  const length = shape.lengthMetres * part.scale;
  const colour = colourOf(part, [186, 178, 108]);
  const base = part.position;
  mesh.begin(ctx.materialId);
  let triangles = 0;
  // Rachis: a three-sided prism. Two triangles per side per span is enough for a 2 mm axis and
  // keeps an ear of rice under a few hundred triangles.
  const spans = 3;
  const rachisRadius = shape.rachisRadiusMetres;
  const rows = [];
  for (let span = 0; span <= spans; ++span) {
    const t = span / spans;
    const centre = add(base, mul(axis, length * t));
    const taper = 1 - 0.6 * t;
    const row = [];
    for (let column = 0; column < 3; ++column) {
      const theta = (column / 3) * TAU;
      const radial = add(mul(side, Math.cos(theta)), mul(up, Math.sin(theta)));
      row.push(mesh.vertex(add(centre, mul(radial, rachisRadius * taper)), radial, [column / 3, t], colour, part.wind));
    }
    rows.push(row);
  }
  for (let span = 0; span < spans; ++span) {
    for (let column = 0; column < 3; ++column) {
      const next = (column + 1) % 3;
      mesh.quad(rows[span][column], rows[span][next], rows[span + 1][next], rows[span + 1][column]);
      triangles += 2;
    }
  }
  // Spikelets. A spike sits them directly on the rachis alternating sides; a panicle puts each
  // one on a lateral stalk that leaves the rachis at an angle and droops — that difference is
  // what separates foxtail from rice, so it is a form and not a texture.
  const spikelets = shape.spikelets;
  for (let index = 0; index < spikelets; ++index) {
    const t = (index + 0.5) / spikelets;
    const theta = index * 2.399963 + (shape.form === 'spike' ? (index % 2) * Math.PI : 0);
    const radial = add(mul(side, Math.cos(theta)), mul(up, Math.sin(theta)));
    const onRachis = add(base, mul(axis, length * t));
    let anchor = onRachis;
    if (shape.form === 'panicle') {
      const stalkDirection = normalize(add(add(mul(radial, Math.sin(shape.lateralAngleRadians)), mul(axis, Math.cos(shape.lateralAngleRadians))), [0, -shape.droop * t, 0]));
      const stalkLength = shape.lateralLengthMetres * (0.5 + 0.5 * Math.sin(Math.PI * t));
      const stalkTip = add(onRachis, mul(stalkDirection, stalkLength));
      // The stalk itself: a two-sided ribbon, which is all a 2 mm stem needs.
      const ribbon = normalize(cross(stalkDirection, [0, 1, 0]));
      const w = rachisRadius * 0.8;
      const a = mesh.vertex(add(onRachis, mul(ribbon, w)), stalkDirection, [0, t], colour, part.wind);
      const b = mesh.vertex(sub(onRachis, mul(ribbon, w)), stalkDirection, [1, t], colour, part.wind);
      const c = mesh.vertex(sub(stalkTip, mul(ribbon, w)), stalkDirection, [1, t + 0.1], colour, part.wind);
      const d = mesh.vertex(add(stalkTip, mul(ribbon, w)), stalkDirection, [0, t + 0.1], colour, part.wind);
      mesh.quad(a, b, c, d); triangles += 2;
      anchor = stalkTip;
    }
    // The spikelet: a lens along the stalk, tapered at both ends.
    const spikeletDirection = normalize(add(mul(radial, 0.55), add(mul(axis, 0.45), [0, -shape.droop * 0.5, 0])));
    const lateral = normalize(cross(spikeletDirection, axis));
    const half = shape.spikeletLengthMetres * 0.5;
    const width = shape.spikeletWidthMetres * 0.5;
    const tipA = add(anchor, mul(spikeletDirection, half));
    const tipB = sub(anchor, mul(spikeletDirection, half));
    const left = add(anchor, mul(lateral, width));
    const right = sub(anchor, mul(lateral, width));
    const normal = normalize(cross(lateral, spikeletDirection));
    const ia = mesh.vertex(tipA, normal, [0.5, 1], colour, part.wind);
    const il = mesh.vertex(left, normal, [0, 0.5], colour, part.wind);
    const ib = mesh.vertex(tipB, normal, [0.5, 0], colour, part.wind);
    const ir = mesh.vertex(right, normal, [1, 0.5], colour, part.wind);
    mesh.quad(ia, il, ib, ir); triangles += 2;
  }
  mesh.end();
  return triangles;
}

function emitPod(mesh, part, ctx) {
  const { axis, side, up } = part.frame;
  const shape = part.shape;
  const length = shape.lengthMetres * part.scale;
  const width = shape.widthMetres * part.scale;
  const colour = colourOf(part, [138, 158, 92]);
  mesh.begin(ctx.materialId);
  // A pod is a lens: zero width at both ends, widest around 55%. Only the interior spans become
  // four-column rings; the ends are single vertices, for the same reason the fruit poles are.
  const centreAt = (t) => add(add(part.position, mul(axis, length * t)), mul(up, shape.curve * length * t * t));
  const rings = [];
  for (let span = 1; span < shape.spans; ++span) {
    const t = span / shape.spans;
    const profile = Math.pow(Math.sin(Math.PI * Math.pow(t, 0.9)), 0.7);
    const centre = centreAt(t);
    const half = width * 0.5 * profile;
    rings.push([
      add(centre, mul(side, half)),
      add(centre, mul(up, half * 0.55)),
      sub(centre, mul(side, half)),
      sub(centre, mul(up, half * 0.55)),
    ].map((position, column) => {
      const normal = normalize(sub(position, centre));
      return mesh.vertex(position, normal[0] === 0 && normal[1] === 0 && normal[2] === 0 ? up : normal, [column / 4, t], colour, part.wind);
    }));
  }
  const rootTip = mesh.vertex(centreAt(0), mul(axis, -1), [0.5, 0], colour, part.wind);
  const farTip = mesh.vertex(centreAt(1), axis, [0.5, 1], colour, part.wind);
  let triangles = 0;
  for (let column = 0; column < 4; ++column) {
    const next = (column + 1) % 4;
    mesh.triangle(rings[0][next], rings[0][column], rootTip); triangles += 1;
    mesh.triangle(rings[rings.length - 1][column], rings[rings.length - 1][next], farTip); triangles += 1;
  }
  for (let ring = 0; ring + 1 < rings.length; ++ring) {
    for (let column = 0; column < 4; ++column) {
      const next = (column + 1) % 4;
      mesh.quad(rings[ring][column], rings[ring][next], rings[ring + 1][next], rings[ring + 1][column]);
      triangles += 2;
    }
  }
  mesh.end();
  return triangles;
}

function emitCompoundLeaf(mesh, part, ctx) {
  const { axis, side, up } = part.frame;
  const shape = part.shape;
  const length = shape.rachisLengthMetres * part.scale;
  const colour = colourOf(part, [86, 132, 52]);
  mesh.begin(ctx.materialId);
  let triangles = 0;
  // Rachis as a thin three-sided prism, same reason as the seed head.
  const spans = 3;
  const rows = [];
  for (let span = 0; span <= spans; ++span) {
    const t = span / spans;
    const centre = add(part.position, mul(axis, length * t));
    const row = [];
    for (let column = 0; column < 3; ++column) {
      const theta = (column / 3) * TAU;
      const radial = add(mul(side, Math.cos(theta)), mul(up, Math.sin(theta)));
      row.push(mesh.vertex(add(centre, mul(radial, shape.rachisRadiusMetres)), radial, [column / 3, t], colour, part.wind));
    }
    rows.push(row);
  }
  for (let span = 0; span < spans; ++span) {
    for (let column = 0; column < 3; ++column) {
      const next = (column + 1) % 3;
      mesh.quad(rows[span][column], rows[span][next], rows[span + 1][next], rows[span + 1][column]);
      triangles += 2;
    }
  }
  // Leaflets: opposite pairs along the rachis plus an optional terminal one. Each leaflet is a
  // base point, one mid row and a tip point — three rows where the ends collapse, so the blade
  // keeps a point at both ends without emitting zero-area quads there.
  const emitLeaflet = (anchor, direction, lateral, lengthMetres, widthMetres) => {
    const normal = normalize(cross(lateral, direction));
    const at = (t) => add(anchor, mul(direction, lengthMetres * t));
    const base = mesh.vertex(at(0), normal, [0.5, 0], colour, part.wind);
    const mid = [];
    for (let column = -1; column <= 1; ++column) {
      const t = 0.45;
      const width = widthMetres * 0.5 * Math.pow(Math.sin(Math.PI * Math.pow(t, 0.85)), 0.9);
      mid.push(mesh.vertex(add(at(t), mul(lateral, width * column)), normal, [(column + 1) / 2, t], colour, part.wind));
    }
    const tip = mesh.vertex(at(1), normal, [0.5, 1], colour, part.wind);
    mesh.triangle(base, mid[0], mid[1]); triangles += 1;
    mesh.triangle(base, mid[1], mid[2]); triangles += 1;
    mesh.triangle(mid[1], tip, mid[0]); triangles += 1;
    mesh.triangle(mid[2], tip, mid[1]); triangles += 1;
  };
  const pairs = shape.leafletPairs;
  for (let pair = 0; pair < pairs; ++pair) {
    const t = (pair + 0.6) / (pairs + (shape.terminalLeaflet ? 0.8 : 0.2));
    const anchor = add(part.position, mul(axis, length * Math.min(1, t)));
    for (const sign of [1, -1]) {
      const direction = normalize(add(add(mul(axis, Math.cos(shape.leafletAngleRadians)), mul(side, sign * Math.sin(shape.leafletAngleRadians))), [0, -0.12, 0]));
      const lateral = normalize(cross(direction, up));
      emitLeaflet(anchor, direction, lateral, shape.leafletLengthMetres * part.scale * (1 - 0.25 * t), shape.leafletWidthMetres * part.scale);
    }
  }
  if (shape.terminalLeaflet) {
    const anchor = add(part.position, mul(axis, length));
    emitLeaflet(anchor, axis, side, shape.leafletLengthMetres * part.scale * 0.8, shape.leafletWidthMetres * part.scale);
  }
  mesh.end();
  return triangles;
}

function emitFloatingLeaf(mesh, part, ctx) {
  const shape = part.shape;
  const radius = shape.radiusMetres * part.scale;
  const colour = colourOf(part, [96, 140, 72]);
  const segments = shape.segments;
  // Flat in the XZ plane at the declared water line, regardless of the stem's direction: a pad
  // lies on the water, it does not follow its petiole.
  const centre = [part.position[0], part.position[1], part.position[2]];
  const normal = [0, 1, 0];
  mesh.begin(ctx.materialId);
  const ring = [];
  const start = shape.notchRadians * 0.5;
  const sweep = TAU - shape.notchRadians;
  for (let segment = 0; segment <= segments; ++segment) {
    const theta = start + (segment / segments) * sweep;
    const lift = radius * shape.rimLift * Math.pow(Math.abs(Math.sin(theta * 2)), 2);
    ring.push(mesh.vertex(
      [centre[0] + Math.cos(theta) * radius, centre[1] + lift, centre[2] + Math.sin(theta) * radius],
      normal, [0.5 + Math.cos(theta) * 0.5, 0.5 + Math.sin(theta) * 0.5], colour, part.wind));
  }
  const hub = mesh.vertex(centre, normal, [0.5, 0.5], colour, part.wind);
  let triangles = 0;
  for (let segment = 0; segment < segments; ++segment) {
    mesh.triangle(hub, ring[segment + 1], ring[segment]); triangles += 1;
  }
  mesh.end();
  return triangles;
}

function emitCulm(mesh, part, ctx) {
  // Bamboo is a culm with nodes, not a trunk with branches: the node is the feature, and it is
  // a band that swells slightly out of the culm surface. Emitting it as a separate component
  // keeps the branch sweep (bark UV, wind weights, LOD) untouched.
  const host = part.host;
  const shape = part.shape;
  const colour = colourOf(part, [148, 160, 88]);
  const segments = shape.segments;
  const halfHeight = shape.ringHeightMetres * 0.5;
  const sample = sampleBranch(host, part.hostArcMetres);
  mesh.begin(ctx.materialId);
  const rows = [];
  for (const offset of [-halfHeight, 0, halfHeight]) {
    const centre = add(sample.position, mul(sample.tangent, offset));
    const bulge = offset === 0 ? shape.ringScale : shape.ringScale - 0.02;
    const row = [];
    for (let segment = 0; segment < segments; ++segment) {
      const theta = (segment / segments) * TAU;
      const radial = add(mul(sample.normal, Math.cos(theta)), mul(sample.binormal, Math.sin(theta)));
      row.push(mesh.vertex(add(centre, mul(radial, sample.radius * bulge)), radial, [segment / segments, offset === 0 ? 0.5 : (offset < 0 ? 0 : 1)], colour, sample.wind));
    }
    rows.push(row);
  }
  let triangles = 0;
  for (let row = 0; row < 2; ++row) {
    for (let segment = 0; segment < segments; ++segment) {
      const next = (segment + 1) % segments;
      mesh.quad(rows[row][segment], rows[row][next], rows[row + 1][next], rows[row + 1][segment]);
      triangles += 2;
    }
  }
  mesh.end();
  return triangles;
}

function emitImported(mesh, part, ctx) {
  const geometry = part.geometry;
  if (!geometry || !geometry.positions || !geometry.indices) {
    fail(`component ${part.componentId} has no imported geometry for part ${part.id}`, 'NG-E-VEG-ASSET');
  }
  const { axis, side, up } = part.frame;
  const scale = part.scale;
  const colour = colourOf(part, [180, 180, 180]);
  mesh.begin(ctx.materialId);
  const base = mesh.positions.length / 3;
  const positions = geometry.positions, normals = geometry.normals, uvs = geometry.uvs;
  for (let index = 0; index < positions.length / 3; ++index) {
    const local = [positions[index * 3], positions[index * 3 + 1], positions[index * 3 + 2]];
    // The adapter maps the asset's own axes onto the attachment frame: local +Y is the
    // attachment direction, so a hanging fruit authored Y-up still hangs down.
    const world = add(part.position, add(add(mul(axis, local[1] * scale), mul(side, local[0] * scale)), mul(up, local[2] * scale)));
    const normal = normals
      ? normalize(add(add(mul(axis, normals[index * 3 + 1]), mul(side, normals[index * 3])), mul(up, normals[index * 3 + 2])))
      : axis;
    mesh.vertex(world, normal, uvs ? [uvs[index * 2], uvs[index * 2 + 1]] : [0, 0], colour, part.wind);
  }
  for (let index = 0; index < geometry.indices.length; index += 3) {
    mesh.triangle(base + geometry.indices[index], base + geometry.indices[index + 1], base + geometry.indices[index + 2]);
  }
  mesh.end();
  return geometry.indices.length / 3;
}

const EMITTERS = {
  fruit: emitFruit, flower: emitFlower, seedHead: emitSeedHead, pod: emitPod,
  compoundLeaf: emitCompoundLeaf, floatingLeaf: emitFloatingLeaf, culm: emitCulm,
  imported: emitImported,
};

// Emits one placed part. `ctx` carries the resolved material id; `part.host` is required by
// emitters that read the host surface (culm).
export function emitComponentPart(mesh, part, ctx) {
  const emitter = EMITTERS[part.type];
  if (!emitter) fail(`no emitter for component type ${part.type}`);
  return emitter(mesh, part, ctx);
}

// Emits every part and returns the per-component triangle counts. Kept next to the emitters so
// the reported cost and the emitted geometry cannot drift apart.
export function emitComponents(mesh, parts, { materialOf, graph }) {
  const triangles = new Map();
  for (const part of parts) {
    if (part.type === 'culm') part.host = graph.byId.get(part.hostBranchId);
    const count = emitComponentPart(mesh, part, { materialId: materialOf(part) });
    triangles.set(part.componentId, (triangles.get(part.componentId) ?? 0) + count);
  }
  return triangles;
}

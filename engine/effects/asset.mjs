import { createHash } from 'node:crypto';
import { canonicalJson as canonical } from '../assets/canonical.mjs';

export const EFFECT_SCHEMA = 'nativegpu.effect/1';
export const COOKED_EFFECT_SCHEMA = 'nativegpu.effect-cook/2';
const COMPILER = 'nativegpu.effect-compiler/2';
const ID = /^[a-z][a-z0-9_-]{0,63}$/;
const TYPES = Object.freeze({ float: 1, float2: 2, float3: 3, float4: 4, uint: 1 });
const CHANNELS = new Set(['density', 'temperatureKelvin', 'windVelocity', 'waterVelocity', 'moisture']);
const RENDERERS = new Set(['sprite', 'ribbon', 'beam', 'mesh', 'light', 'decal', 'volumetric']);
const MODULES = new Set(['initialize', 'acceleration', 'drag', 'colorOverLife', 'sizeOverLife', 'curlNoise', 'fieldCoupling', 'velocityField', 'fieldDeposit']);
const BLENDS = new Set(['additive', 'premultiplied', 'cutout']);

export class EffectError extends Error {
  constructor(code, message) { super(message); this.name = 'EffectError'; this.code = code; }
}
function requireValue(ok, message, code = 'NG-E-EFFECT-ASSET') {
  if (!ok) throw new EffectError(code, message);
}
function object(value, label) {
  requireValue(value && typeof value === 'object' && !Array.isArray(value)
    && (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null), `${label} must be an object`);
}
function keys(value, required, optional, label) {
  object(value, label);
  for (const key of required) requireValue(Object.hasOwn(value, key), `${label}.${key} is required`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) requireValue(allowed.has(key), `${label}.${key} is unsupported`);
}
function integer(value, low, high, label) {
  requireValue(Number.isSafeInteger(value) && value >= low && value <= high, `${label} must be an integer in [${low},${high}]`);
}
function scalar(value, low, high, label) {
  requireValue(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high,
    `${label} must be finite and in [${low},${high}]`);
}
function vector(value, length, low, high, label) {
  requireValue(Array.isArray(value) && value.length === length, `${label} requires ${length} values`);
  for (let i = 0; i < length; ++i) requireValue(Object.hasOwn(value, i), `${label} contains a missing entry`);
  value.forEach((v, i) => scalar(v, low, high, `${label}[${i}]`));
}
function array(value, max, label, min = 0) {
  requireValue(Array.isArray(value) && value.length >= min && value.length <= max, `${label} must have ${min}–${max} entries`);
  for (let i = 0; i < value.length; ++i) requireValue(Object.hasOwn(value, i), `${label} contains a missing entry`);
}
function identifier(value, label) { requireValue(typeof value === 'string' && ID.test(value), `${label} is not a valid identifier`); }
function reference(value, label) {
  requireValue(typeof value === 'string' && value.length <= 240 && value.split('/').every((part) =>
    /^[A-Za-z0-9_-][A-Za-z0-9_.-]*$/.test(part) && part !== '..' && part !== '.'), `${label} must be an asset-relative path`);
}
function curve(value, width, high, label) {
  array(value, 64, label, 2);
  let previous = -1;
  value.forEach((key, i) => {
    keys(key, ['t', 'value'], [], `${label}[${i}]`);
    scalar(key.t, 0, 1, `${label}[${i}].t`);
    requireValue(key.t > previous, `${label} times must strictly increase`);
    previous = key.t;
    vector(key.value, width, 0, high, `${label}[${i}].value`);
  });
  requireValue(value[0].t === 0 && value.at(-1).t === 1, `${label} must cover normalized lifetime [0,1]`);
}
export function sampleCurve(keys, t) {
  // Validate the sample coordinate even for an already compiled curve.
  scalar(t, 0, 1, 'curve sample');
  const right = keys.findIndex((key) => key.t >= t);
  if (right <= 0) return [...keys[0].value];
  if (keys[right].t === t) return [...keys[right].value];
  const a = keys[right - 1], b = keys[right];
  const fraction = (t - a.t) / (b.t - a.t);
  return a.value.map((value, index) => value + (b.value[index] - value) * fraction);
}

function validateShape(shape, label) {
  object(shape, label);
  if (shape.type === 'point') keys(shape, ['type'], [], label);
  else if (shape.type === 'sphere') { keys(shape, ['type', 'radius'], [], label); scalar(shape.radius, 0.0001, 10000, label); }
  else if (shape.type === 'cone') {
    keys(shape, ['type', 'radius', 'angleRadians'], [], label);
    scalar(shape.radius, 0.0001, 10000, `${label}.radius`); scalar(shape.angleRadians, 0, Math.PI, `${label}.angleRadians`);
  } else if (shape.type === 'box') { keys(shape, ['type', 'extent'], [], label); vector(shape.extent, 3, 0.0001, 10000, label); }
  else if (shape.type === 'line') { keys(shape, ['type', 'end'], [], label); vector(shape.end, 3, -10000, 10000, label); }
  else if (shape.type === 'mesh') { keys(shape, ['type', 'mesh'], [], label); reference(shape.mesh, `${label}.mesh`); }
  else requireValue(false, `${label}.type is unsupported`);
}

function moduleContract(module, label) {
  keys(module, ['id', 'type', 'params'], ['after'], label);
  identifier(module.id, `${label}.id`);
  requireValue(MODULES.has(module.type), `${label}.type is unsupported`);
  if (module.after !== undefined) {
    array(module.after, 32, `${label}.after`);
    module.after.forEach((id) => identifier(id, `${label}.after`));
    requireValue(new Set(module.after).size === module.after.length, `${label}.after has duplicates`);
  }
  const p = module.params;
  const contract = { reads: [], writes: [], attributes: {} };
  if (module.type === 'initialize') {
    keys(p, ['velocity', 'color', 'size'], [], `${label}.params`);
    vector(p.velocity, 3, -10000, 10000, 'velocity'); vector(p.color, 4, 0, 64, 'color'); vector(p.size, 2, 0, 10000, 'size');
    scalar(p.color[3], 0, 1, 'color alpha');
    contract.writes = ['velocity', 'color', 'size']; contract.attributes = { color: 'float4', size: 'float2' };
  } else if (module.type === 'acceleration') {
    keys(p, ['value'], [], `${label}.params`); vector(p.value, 3, -10000, 10000, 'acceleration');
    contract.reads = ['velocity']; contract.writes = ['velocity'];
  } else if (module.type === 'drag') {
    keys(p, ['coefficient'], [], `${label}.params`); scalar(p.coefficient, 0, 10000, 'drag coefficient');
    contract.reads = ['velocity']; contract.writes = ['velocity'];
  } else if (module.type === 'colorOverLife' || module.type === 'sizeOverLife') {
    keys(p, ['keys'], [], `${label}.params`);
    const color = module.type === 'colorOverLife';
    curve(p.keys, color ? 4 : 2, color ? 64 : 10000, `${label}.params.keys`);
    if (color) for (const key of p.keys) scalar(key.value[3], 0, 1, 'curve alpha');
    contract.reads = ['age', 'lifetime']; contract.writes = [color ? 'color' : 'size'];
    contract.attributes = color ? { color: 'float4' } : { size: 'float2' };
  } else if (module.type === 'curlNoise') {
    keys(p, ['amplitude', 'frequency'], [], `${label}.params`);
    scalar(p.amplitude, 0, 10000, 'noise amplitude'); scalar(p.frequency, 0.0001, 10000, 'noise frequency');
    contract.reads = ['position', 'velocity']; contract.writes = ['velocity'];
  } else if (module.type === 'velocityField') {
    keys(p, ['channel', 'response'], [], `${label}.params`);
    requireValue(['windVelocity', 'waterVelocity'].includes(p.channel), 'velocityField requires a velocity channel');
    scalar(p.response, 0, 10000, 'velocity response per second');
    contract.reads = ['position', 'velocity']; contract.writes = ['velocity'];
  } else if (module.type === 'fieldDeposit') {
    keys(p, ['channel', 'quantum'], [], `${label}.params`);
    requireValue(['density', 'moisture'].includes(p.channel), 'fieldDeposit requires a nonnegative scalar channel');
    integer(p.quantum, 1, 0xFFFFFFFF, 'field deposit quantum');
    contract.reads = ['position']; contract.fieldWrites = [p.channel]; contract.phase = 'postSimulation';
  } else {
    keys(p, ['readChannels', 'writeChannels'], [], `${label}.params`);
    for (const direction of ['readChannels', 'writeChannels']) {
      array(p[direction], 5, `${label}.${direction}`);
      requireValue(new Set(p[direction]).size === p[direction].length, 'duplicate field channel');
      for (const c of p[direction]) requireValue(CHANNELS.has(c), `unsupported field channel ${c}`);
    }
    requireValue(p.readChannels.length + p.writeChannels.length > 0, 'field coupling must declare access');
    contract.reads = ['position']; contract.writes = p.readChannels.some((c) => c.endsWith('Velocity')) ? ['velocity'] : [];
  }
  return contract;
}

function orderedModules(modules, label) {
  array(modules, 32, label, 1);
  const byId = new Map(), contracts = new Map();
  modules.forEach((module, i) => {
    const contract = moduleContract(module, `${label}[${i}]`);
    requireValue(!byId.has(module.id), `${label} has duplicate ids`);
    byId.set(module.id, module); contracts.set(module.id, contract);
  });
  const output = [], active = new Set(), visited = new Set();
  function visit(id) {
    requireValue(byId.has(id), `unknown module dependency ${id}`);
    requireValue(!active.has(id), `module dependency cycle at ${id}`);
    if (visited.has(id)) return;
    active.add(id);
    for (const dependency of byId.get(id).after ?? []) visit(dependency);
    active.delete(id); visited.add(id); output.push(byId.get(id));
  }
  // Explicit dependencies constrain the authored stable array order.
  for (const module of modules) visit(module.id);
  for (const module of output) if (module.type !== 'fieldDeposit') {
    requireValue((module.after ?? []).every(id => byId.get(id).type !== 'fieldDeposit'),
      'simulation modules cannot depend on a postSimulation field deposit');
  }
  requireValue(output[0].type === 'initialize' && output.filter((m) => m.type === 'initialize').length === 1,
    `${label} needs exactly one initialize module, executed first`);
  return { output, contracts };
}

export function canonicalJson(value) {
  requireValue(value !== undefined && (typeof value !== 'number' || Number.isFinite(value)), 'non-JSON value');
  return canonical(value);
}
const digest = (value) => createHash('sha256').update(canonicalJson(value)).digest('hex');
const align = (value) => Math.ceil(value / 16) * 16;

function compileSmoke(asset) {
  const c = asset.smoke;
  keys(c, ['dimensions', 'cellMetres', 'originOffset', 'channels', 'sources', 'tailTicks'], ['parameters', 'unitsPerSource', 'optics'], 'smoke');
  array(c.dimensions, 3, 'smoke.dimensions', 3); c.dimensions.forEach(d => integer(d, 2, 128, 'smoke dimension'));
  scalar(c.cellMetres, .01, 10, 'smoke.cellMetres'); vector(c.originOffset, 3, -100000, 100000, 'smoke.originOffset');
  integer(c.tailTicks, 0, 432000, 'smoke.tailTicks');
  array(c.channels, 3, 'smoke.channels', 1); const names = ['density', 'temperatureKelvin', 'fuel'];
  requireValue(new Set(c.channels).size === c.channels.length && c.channels.every(n => names.includes(n)), 'invalid smoke channels');
  const defaults = { ambientKelvin: 293.15, maxSpeed: 8, courant: .5, densityDecay: .05, cooling: .4, fuelDecay: 0, burnRate: 2,
    ignitionKelvin: 700, heatPerFuel: 800, sootPerFuel: .3, maxDensity: 100, maxTemperature: 4000, maxFuel: 100,
    buoyancy: .01, smokeWeight: .1, vorticity: .2, pressureRelaxation: 2 / 3, pressureIterations: 48, maxSubsteps: 16, macCormack: true };
  keys(c.parameters === undefined ? {} : c.parameters, [], Object.keys(defaults), 'smoke.parameters');
  const parameters = { ...defaults, ...c.parameters };
  for (const name of ['densityDecay', 'cooling', 'fuelDecay', 'burnRate', 'heatPerFuel', 'sootPerFuel', 'buoyancy', 'smokeWeight', 'vorticity']) scalar(parameters[name], 0, 10000, name);
  scalar(parameters.ambientKelvin, 100, 1000, 'ambientKelvin'); scalar(parameters.maxSpeed, Number.MIN_VALUE, 100, 'maxSpeed');
  scalar(parameters.courant, Number.MIN_VALUE, .5, 'courant'); scalar(parameters.maxDensity, Number.MIN_VALUE, 10000, 'maxDensity');
  scalar(parameters.maxFuel, Number.MIN_VALUE, 10000, 'maxFuel'); scalar(parameters.maxTemperature, 100, 10000, 'maxTemperature');
  scalar(parameters.ignitionKelvin, 100, 10000, 'ignitionKelvin'); scalar(parameters.pressureRelaxation, Number.MIN_VALUE, 1, 'pressureRelaxation');
  integer(parameters.pressureIterations, 1, 256, 'pressureIterations'); integer(parameters.maxSubsteps, 1, 64, 'maxSubsteps');
  requireValue(typeof parameters.macCormack === 'boolean', 'macCormack must be boolean');
  for (const name of Object.keys(parameters)) if (typeof parameters[name] === 'number') parameters[name] = Math.fround(parameters[name]);
  const cellMetres = Math.fround(c.cellMetres);
  requireValue(parameters.maxSpeed > 0 && parameters.courant > 0 && parameters.maxDensity > 0 && parameters.maxFuel > 0 && parameters.pressureRelaxation > 0,
    'smoke parameter underflows float storage');
  requireValue(parameters.maxTemperature > parameters.ambientKelvin && parameters.ignitionKelvin >= parameters.ambientKelvin && parameters.ignitionKelvin <= parameters.maxTemperature, 'smoke temperature bounds');
  requireValue(Math.ceil(Math.fround(1 / 120) * parameters.maxSpeed / Math.fround(cellMetres * parameters.courant)) <= parameters.maxSubsteps, 'smoke fixed tick exceeds CFL budget');
  const unitsPerSource = c.unitsPerSource === undefined ? [1000000, 1000, 1000000] : c.unitsPerSource; array(unitsPerSource, 3, 'smoke.unitsPerSource', 3);
  unitsPerSource.forEach(v => integer(v, 1, 1000000000, 'smoke source units'));
  array(c.sources, 32, 'smoke.sources', 1); const used = new Set(), totals = [0, 0, 0];
  const sources = c.sources.map(source => {
    keys(source, ['emitter', 'densityMassRate', 'heatVolumeRate', 'fuelMassRate'], [], 'smoke source');
    identifier(source.emitter, 'smoke source emitter'); const emitter = asset.emitters.findIndex(e => e.id === source.emitter);
    requireValue(emitter >= 0 && !used.has(emitter), 'unknown or duplicate smoke source emitter'); used.add(emitter);
    const quantum = ['densityMassRate', 'heatVolumeRate', 'fuelMassRate'].map((name, a) => {
      scalar(source[name], 0, 1000000, name); const q = Math.round(source[name] * unitsPerSource[a]);
      requireValue(Number.isSafeInteger(q) && q <= 0xFFFFFFFF && (source[name] === 0 || q > 0), 'smoke source quantum overflow/underflow');
      requireValue(q === 0 || c.channels.includes(names[a]), 'smoke source requires an enabled channel');
      totals[a] += q * asset.maxAlive; requireValue(Number.isSafeInteger(totals[a]) && totals[a] <= 0xFFFFFFFF, 'smoke conservative capacity × quantum sum exceeds UInt32', 'NG-E-EFFECT-BUDGET');
      return q;
    });
    requireValue(quantum.some(q => q > 0), 'smoke source quantizes to zero'); return { emitter, quantum };
  });
  for (let a = 0; a < 3; ++a) requireValue(totals[a] / (unitsPerSource[a] * cellMetres ** 3) <= 1000000, 'smoke source per-cell rate bound', 'NG-E-EFFECT-BUDGET');
  const optics = { extinctionPerDensity: 1, scatteringAlbedo: [.65, .65, .65], emissionPerDensity: .05, emissionStartKelvin: 700, ...c.optics };
  if (c.optics !== undefined) keys(c.optics, [], ['extinctionPerDensity', 'scatteringAlbedo', 'emissionPerDensity', 'emissionStartKelvin'], 'smoke.optics');
  scalar(optics.extinctionPerDensity, 0, 1000, 'smoke extinction area per mass'); scalar(optics.emissionPerDensity, 0, 1000, 'smoke emission per mass/path');
  scalar(optics.emissionStartKelvin, 100, 10000, 'smoke emission Kelvin'); vector(optics.scatteringAlbedo, 3, 0, 1, 'smoke scattering albedo');
  for (const name of ['extinctionPerDensity', 'emissionPerDensity', 'emissionStartKelvin']) optics[name] = Math.fround(optics[name]);
  optics.scatteringAlbedo = optics.scatteringAlbedo.map(Math.fround);
  return { dimensions: [...c.dimensions], cellMetres, originOffset: c.originOffset.map(Math.fround), channels: c.channels.reduce((bits, n) => bits | (1 << names.indexOf(n)), 0), parameters, unitsPerSource: [...unitsPerSource], tailTicks: c.tailTicks, sources, optics };
}

export function compileEffect(asset) {
  keys(asset, ['schema', 'id', 'revision', 'seed', 'durationTicks', 'loop', 'maxAlive', 'bounds', 'budget', 'emitters', 'events'],
    ['field', 'smoke'], 'effect');
  requireValue(asset.schema === EFFECT_SCHEMA, 'unsupported effect schema');
  identifier(asset.id, 'effect.id'); integer(asset.revision, 0, 0xFFFFFFFF, 'revision');
  integer(asset.seed, 0, 0xFFFFFFFF, 'seed'); integer(asset.durationTicks, 1, 120 * 3600, 'durationTicks');
  requireValue(typeof asset.loop === 'boolean', 'loop must be boolean'); integer(asset.maxAlive, 1, 1000000, 'maxAlive');
  keys(asset.bounds, ['min', 'max'], [], 'bounds');
  vector(asset.bounds.min, 3, -100000, 100000, 'bounds.min'); vector(asset.bounds.max, 3, -100000, 100000, 'bounds.max');
  requireValue(asset.bounds.min.every((v, i) => v < asset.bounds.max[i]), 'bounds must have positive extents');
  keys(asset.budget, ['maxSpawnPerTick', 'maxEventsPerTick', 'maxDepth'], [], 'budget');
  integer(asset.budget.maxSpawnPerTick, 1, 1000000, 'maxSpawnPerTick');
  integer(asset.budget.maxEventsPerTick, 0, 65536, 'maxEventsPerTick'); integer(asset.budget.maxDepth, 0, 8, 'maxDepth');
  if (asset.field !== undefined) {
    keys(asset.field, ['level', 'channels', 'dimensions', 'voxelMetres'], [], 'field');
    requireValue(['local', 'worldVisible'].includes(asset.field.level), 'unsupported field level');
    array(asset.field.channels, 5, 'field.channels', 1);
    requireValue(new Set(asset.field.channels).size === asset.field.channels.length, 'duplicate field channel');
    for (const c of asset.field.channels) requireValue(CHANNELS.has(c), `unsupported field channel ${c}`);
    vector(asset.field.dimensions, 3, 8, 256, 'field.dimensions');
    for (const d of asset.field.dimensions) requireValue(Number.isInteger(d) && d % 8 === 0, 'field dimensions must be multiples of 8');
    scalar(asset.field.voxelMetres, 0.001, 100, 'field.voxelMetres');
  }
  array(asset.emitters, 32, 'emitters', 1);
  array(asset.events, 32, 'events');
  const dependencies = new Set(), effectDependencies = new Set(), ids = new Set(), depositQuanta = new Map();
  const emitters = asset.emitters.map((emitter, emitterIndex) => {
    const label = `emitters[${emitterIndex}]`;
    keys(emitter, ['id', 'shape', 'position', 'ratePerSecond', 'bursts', 'lifetimeTicks', 'localSpace', 'modules', 'renderers'], [], label);
    identifier(emitter.id, `${label}.id`); requireValue(!ids.has(emitter.id), 'duplicate emitter id'); ids.add(emitter.id);
    validateShape(emitter.shape, `${label}.shape`);
    if (emitter.shape.type === 'mesh') dependencies.add(emitter.shape.mesh);
    vector(emitter.position, 3, -100000, 100000, `${label}.position`);
    scalar(emitter.ratePerSecond, 0, 1000000, 'ratePerSecond');
    requireValue(Math.round(emitter.ratePerSecond * 1000) / 1000 === emitter.ratePerSecond,
      'ratePerSecond supports at most three decimal places');
    requireValue(typeof emitter.localSpace === 'boolean', 'localSpace must be boolean');
    array(emitter.lifetimeTicks, 2, 'lifetimeTicks', 2);
    emitter.lifetimeTicks.forEach((v) => integer(v, 1, 120 * 3600, 'lifetimeTicks'));
    requireValue(emitter.lifetimeTicks[0] <= emitter.lifetimeTicks[1], 'lifetime range reversed');
    array(emitter.bursts, 256, 'bursts');
    let previous = -1;
    for (const burst of emitter.bursts) {
      keys(burst, ['tick', 'count'], [], 'burst'); integer(burst.tick, 0, asset.durationTicks - 1, 'burst.tick');
      integer(burst.count, 1, 1000000, 'burst.count'); requireValue(burst.tick > previous, 'burst ticks must strictly increase');
      previous = burst.tick;
    }
    const { output, contracts } = orderedModules(emitter.modules, `${label}.modules`);
    const attributes = new Map(Object.entries({ position: 'float3', velocity: 'float3', age: 'uint', lifetime: 'uint', spawnId: 'uint' }));
    for (const module of output) {
      const contract = contracts.get(module.id);
      for (const name of contract.reads) requireValue(attributes.has(name), `${module.id} reads uninitialized ${name}`);
      for (const [name, type] of Object.entries(contract.attributes)) attributes.set(name, type);
      if (module.type === 'fieldCoupling') {
        requireValue(asset.field !== undefined, 'fieldCoupling requires a field declaration');
        for (const c of [...module.params.readChannels, ...module.params.writeChannels])
          requireValue(asset.field.channels.includes(c), `fieldCoupling accesses unleased ${c}`);
      }
      if (module.type === 'velocityField') {
        requireValue(asset.field !== undefined, 'velocityField requires a field declaration');
        requireValue(asset.field.channels.includes(module.params.channel), `velocityField accesses unleased ${module.params.channel}`);
      }
      if (module.type === 'fieldDeposit') {
        requireValue(asset.field !== undefined && asset.field.channels.includes(module.params.channel), 'fieldDeposit requires a declared field channel');
        const quantum = (depositQuanta.get(module.params.channel) ?? 0) + module.params.quantum;
        requireValue(quantum <= Math.floor(0xFFFFFFFF / asset.maxAlive), 'fieldDeposit conservative capacity × quantum sum exceeds UInt32', 'NG-E-EFFECT-BUDGET');
        depositQuanta.set(module.params.channel, quantum);
      }
    }
    array(emitter.renderers, 6, `${label}.renderers`, 1);
    for (const renderer of emitter.renderers) {
      object(renderer, 'renderer'); requireValue(RENDERERS.has(renderer.type), 'unsupported renderer');
      if (['sprite', 'ribbon', 'beam', 'decal'].includes(renderer.type)) {
        keys(renderer, ['type', 'texture', 'blend', 'lit'], [], 'renderer');
        reference(renderer.texture, 'renderer.texture'); dependencies.add(renderer.texture);
        requireValue(BLENDS.has(renderer.blend), 'unsupported blend mode'); requireValue(typeof renderer.lit === 'boolean', 'lit must be boolean');
        if (['ribbon', 'beam'].includes(renderer.type)) attributes.set('ribbonId', 'uint');
      } else if (renderer.type === 'mesh') {
        keys(renderer, ['type', 'mesh', 'material'], [], 'renderer');
        reference(renderer.mesh, 'renderer.mesh'); reference(renderer.material, 'renderer.material');
        dependencies.add(renderer.mesh); dependencies.add(renderer.material); attributes.set('rotation', 'float4');
      } else if (renderer.type === 'light') {
        keys(renderer, ['type', 'intensity', 'range'], [], 'renderer');
        scalar(renderer.intensity, 0, 100000, 'light intensity'); scalar(renderer.range, 0.01, 10000, 'light range');
      } else {
        keys(renderer, ['type'], [], 'renderer');
        requireValue(asset.field?.channels.includes('density'), 'volumetric renderer requires leased density');
      }
    }
    let offset = 0;
    const layout = [...attributes].map(([name, type]) => {
      const bytes = align(TYPES[type] * 4 * asset.maxAlive);
      const attribute = { name, type, offset, bytes }; offset += bytes; return attribute;
    });
    return { id: emitter.id, rateMilli: Math.round(emitter.ratePerSecond * 1000), layout, poolBytes: offset,
      modules: output.map((module) => ({ ...structuredClone(module), access: contracts.get(module.id) })) };
  });
  for (const event of asset.events) {
    keys(event, ['on', 'spawn', 'limit', 'delayTicks'], [], 'event');
    requireValue(['spawn', 'collision', 'death', 'threshold', 'timeline'].includes(event.on), 'unsupported event type');
    identifier(event.spawn, 'event.spawn'); integer(event.limit, 1, 65536, 'event.limit');
    integer(event.delayTicks, 0, 120 * 3600, 'event.delayTicks'); effectDependencies.add(event.spawn);
    requireValue(event.spawn !== asset.id, 'self-spawning effect is not allowed');
    requireValue(asset.budget.maxDepth > 0 && asset.budget.maxEventsPerTick > 0, 'events require nonzero event/depth budget');
  }
  const smoke = asset.smoke === undefined ? undefined : compileSmoke(asset);
  const poolBytes = emitters.reduce((sum, e) => sum + e.poolBytes, 0);
  requireValue(poolBytes <= 512 * 1024 * 1024, 'effect pool exceeds 512 MiB authoring limit', 'NG-E-EFFECT-BUDGET');
  const source = JSON.parse(canonicalJson(asset));
  const compiled = { schema: COOKED_EFFECT_SCHEMA, compiler: COMPILER, source, emitters,
    dependencies: [...dependencies].sort(), effectDependencies: [...effectDependencies].sort(), poolBytes };
  if (smoke) compiled.smoke = smoke;
  const layoutHash = digest(emitters.map((e) => ({ id: e.id, layout: e.layout })));
  // Native readers verify these exact UTF-8 bytes; they do not reproduce a JS
  // floating-point JSON formatter to authenticate the payload.
  return { ...compiled, layoutHash, payloadJson: canonicalJson(compiled), hash: digest(compiled) };
}

export function compileEffectLibrary(assets) {
  array(assets, 4096, 'effect library', 1);
  const compiled = assets.map(compileEffect), byId = new Map();
  for (const effect of compiled) {
    requireValue(!byId.has(effect.source.id), `duplicate library id ${effect.source.id}`);
    byId.set(effect.source.id, effect);
  }
  const active = new Set(), visited = new Set();
  function visit(id) {
    requireValue(byId.has(id), `missing subeffect ${id}`);
    requireValue(!active.has(id), `subeffect cycle at ${id}`);
    if (visited.has(id)) return;
    active.add(id);
    for (const child of byId.get(id).effectDependencies) visit(child);
    active.delete(id); visited.add(id);
  }
  for (const id of byId.keys()) visit(id);
  const result = [...compiled].sort((a, b) => a.source.id < b.source.id ? -1 : a.source.id > b.source.id ? 1 : 0);
  return { effects: result, hash: digest(result.map((e) => ({ id: e.source.id, hash: e.hash }))) };
}

export function previewEffectPatch(asset, expectedRevision, changes) {
  compileEffect(asset);
  requireValue(expectedRevision === asset.revision, 'effect revision conflict', 'NG-E-EFFECT-REVISION');
  object(changes, 'changes');
  const next = structuredClone(asset);
  for (const [pointer, value] of Object.entries(changes)) {
    requireValue(/^\/(seed|bounds|budget|emitters|field|smoke)(\/[^/]+)*$/.test(pointer), `unsupported effect patch path ${pointer}`);
    const segments = pointer.slice(1).split('/');
    requireValue(segments.every((part) => !['__proto__', 'prototype', 'constructor'].includes(part)), 'unsafe patch path');
    let target = next;
    for (const part of segments.slice(0, -1)) {
      requireValue(target && typeof target === 'object' && Object.hasOwn(target, part), `missing patch path ${pointer}`);
      target = target[part];
    }
    const last = segments.at(-1);
    requireValue(target && typeof target === 'object' && Object.hasOwn(target, last), `missing patch path ${pointer}`);
    target[last] = structuredClone(value);
  }
  next.revision += 1;
  const before = compileEffect(asset), after = compileEffect(next);
  return { asset: next, compiled: after, migration: before.layoutHash === after.layoutHash && canonicalJson(before.smoke ?? null) === canonicalJson(after.smoke ?? null) ? 'preserve-layout' : 'reset-required',
    // Compatible memory layout alone does not promise preserving all solver history.
    runtimeApplied: false, beforeHash: before.hash, afterHash: after.hash };
}

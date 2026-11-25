import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import {AsyncLocalStorage} from 'node:async_hooks';

const ASSETS = fileURLToPath(new URL('../../assets/', import.meta.url));
const assetContext=new AsyncLocalStorage();
export const currentGameplayAssets=()=>assetContext.getStore()??ASSETS;
// Each asynchronous scene operation owns its dependency root; concurrent editor
// requests cannot replace another scene's component or behavior catalog.
export const withGameplayAssets=(assets,operation)=>assetContext.run(assets??currentGameplayAssets(),operation);
export function findGameplayAssets(file) {
  let directory=path.dirname(path.resolve(file));
  while(true){
    if(fs.existsSync(path.join(directory,'gameplay/components.json')))return fs.realpathSync(directory);
    if(fs.existsSync(path.join(directory,'assets/gameplay/components.json')))return fs.realpathSync(path.join(directory,'assets'));
    const parent=path.dirname(directory);if(parent===directory)return ASSETS;directory=parent;
  }
}
const safeName = name => typeof name === 'string' && name.length <= 128 && /^[A-Za-z0-9_-]+(?:\/[A-Za-z0-9_-]+)*$/.test(name);
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
function fail(message) { const error = new Error(message); error.code = 'NG-E-STATE-SCHEMA'; throw error; }
export function normalizeValue(schema, value, location = 'value', depth = 0) {
  if (depth > 16) fail(`${location}: nesting limit`);
  if (schema.oneOf) {
    const matches = [];
    for (const option of schema.oneOf) try { matches.push(normalizeValue(option, value, location, depth + 1)); } catch { /* try variant */ }
    if (matches.length !== 1) fail(`${location}: expected exactly one variant`);
    return matches[0];
  }
  const type = schema.type;
  if (!(type === 'object' ? object(value) : type === 'array' ? Array.isArray(value)
    : type === 'integer' ? Number.isSafeInteger(value) : type === 'number' ? typeof value === 'number' && Number.isFinite(value)
      : typeof value === type)) fail(`${location}: expected ${type}`);
  if (schema.enum && !schema.enum.some(option => JSON.stringify(option) === JSON.stringify(value))) fail(`${location}: invalid enum`);
  if (typeof value === 'number' && (value < (schema.minimum ?? -1e15) || value > (schema.maximum ?? 1e15))) fail(`${location}: numeric bounds`);
  if (typeof value === 'string' && Buffer.byteLength(value) > (schema.maxLength ?? 4096)) fail(`${location}: string length`);
  if (Array.isArray(value)) {
    if (value.length < (schema.minItems ?? 0) || value.length > (schema.maxItems ?? 256)) fail(`${location}: array length`);
    return value.map((item, index) => normalizeValue(schema.items, item, `${location}[${index}]`, depth + 1));
  }
  if (object(value)) {
    if (Object.keys(value).length > 256) fail(`${location}: field limit`);
    const out = Object.create(null);
    for (const [key, item] of Object.entries(value)) {
      const field = Object.hasOwn(schema.properties ?? {}, key) ? schema.properties[key] : schema.additionalProperties;
      if (!object(field)) fail(`${location}: unknown field ${key}`);
      out[key] = normalizeValue(field, item, `${location}.${key}`, depth + 1);
    }
    for (const [key, field] of Object.entries(schema.properties ?? {})) {
      if (!Object.hasOwn(out, key) && Object.hasOwn(field, 'default')) out[key] = normalizeValue(field, field.default, `${location}.${key}`, depth + 1);
    }
    for (const key of schema.required ?? []) if (!Object.hasOwn(out, key)) fail(`${location}: required ${key}`);
    return out;
  }
  return value;
}
function readAsset(assets, relative) {
  const root = fs.realpathSync(assets), file = fs.realpathSync(path.join(root, relative));
  const rel = path.relative(root, file);
  if (rel.startsWith('..') || path.isAbsolute(rel) || fs.statSync(file).size > 256 * 1024) fail('Gameplay dependency escaped assets or exceeds 256 KiB');
  return fs.readFileSync(file, 'utf8');
}
export function componentCatalog(assets = currentGameplayAssets()) { return JSON.parse(readAsset(assets, 'gameplay/components.json')); }
export function behaviorDescriptor(script, assets = currentGameplayAssets()) {
  if (!safeName(script)) fail('Invalid behavior script path');
  const descriptor = JSON.parse(readAsset(assets, `scripts/${script}.behavior.json`));
  if (descriptor.apiVersion !== 1 || !Number.isInteger(descriptor.version) || descriptor.version < 1) fail('Unsupported behavior API/version');
  if (descriptor.dependencies !== undefined && (!Array.isArray(descriptor.dependencies) || descriptor.dependencies.length > 128 || !descriptor.dependencies.every(safeName))) fail('Invalid behavior dependencies');
  return descriptor;
}
export function validateComponents(components, assets = currentGameplayAssets()) {
  if (!object(components)) fail('components must be an object');
  const catalog = componentCatalog(assets), out = Object.create(null);
  for (const [name, data] of Object.entries(components)) {
    if (!Object.hasOwn(catalog, name)) fail(`Unknown component ${name}`);
    out[name] = normalizeValue(catalog[name].schema, data, name);
  }
  const seen = new Set();
  for (const instance of out.behavior?.instances ?? []) {
    if (!instance.id || seen.has(instance.id)) fail('Invalid/duplicate behavior instance ID');
    seen.add(instance.id);
    const descriptor = behaviorDescriptor(instance.script, assets);
    normalizeValue(descriptor.properties, instance.properties ?? {}, instance.script);
  }
  return out;
}
export function gameplayDependencies(scene, assets = currentGameplayAssets()) {
  const scripts = new Set();
  for (const entity of scene.entities) {
    if (entity.components?.behavior) for (const b of entity.components.behavior.instances) scripts.add(b.script);
    else {
      const inferred = { key: 'gameplay/pickup', door: 'gameplay/locked_door', goal: 'gameplay/goal' }[entity.kind];
      if (inferred) scripts.add(inferred);
    }
  }
  const resolving = new Set(), resolved = new Set();
  function resolve(script) {
    if (resolved.has(script)) return;
    if (resolving.has(script) || resolving.size >= 64 || scripts.size > 512) fail('Behavior dependency cycle/budget');
    resolving.add(script);
    for (const dependency of behaviorDescriptor(script, assets).dependencies ?? []) { scripts.add(dependency); resolve(dependency); }
    resolving.delete(script); resolved.add(script);
  }
  for (const script of scripts) resolve(script);
  const files = ['gameplay/components.json', ...[...scripts].sort().flatMap(script => [`scripts/${script}.behavior.json`, `scripts/${script}.lua`])];
  return files.map(file => {
    const data = readAsset(assets, file);
    return { path: file, sha256: createHash('sha256').update(data).digest('hex'), bytes: Buffer.byteLength(data), data };
  });
}
export function gameplayCatalog(assets = currentGameplayAssets()) {
  const scripts = [];
  function visit(dir, prefix = '') {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a,b) => a.name.localeCompare(b.name))) {
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) visit(path.join(dir, entry.name), `${prefix}${entry.name}/`);
      else if (entry.name.endsWith('.behavior.json')) {
        const script = `${prefix}${entry.name.slice(0,-14)}`;
        scripts.push({ script, ...behaviorDescriptor(script, assets) });
      }
    }
  }
  visit(path.join(assets, 'scripts'));
  return { apiVersion: 1, components: componentCatalog(assets), behaviors: scripts };
}

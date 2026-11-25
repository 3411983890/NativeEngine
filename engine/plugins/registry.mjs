// Editor plugin registry.
//
// The plan calls the hair generator and the garment binder "plugins" in the sense of
// registerable, disableable editor feature modules with an AI tool adapter — not external
// Codex plugins. This registry is that concept, kept deliberately small:
//
//   * a plugin declares an id, a version, the actions it owns and its capabilities,
//   * a disabled plugin is not dispatchable, and its tools are not advertised,
//   * capabilities are data, so "can this project do hair?" is answered by the registry
//     rather than by trying a call and seeing whether it throws.

export class PluginError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'PluginError';
    this.code = code;
  }
}

const PLUGIN_ID = /^[a-z][a-z0-9-]{0,31}$/;

export const HAIR_PLUGIN = Object.freeze({
  id: 'hair-generator',
  version: '1.0.0',
  title: '图片驱动发型生成器',
  actions: [
    'hair.capabilities', 'hair.reference.register', 'hair.analysis.reconcile', 'hair.create',
    'hair.load', 'hair.patch', 'hair.undo', 'hair.redo', 'hair.capture', 'hair.validate',
    'hair.export', 'hair.selection',
  ],
  tools: [
    'hair_reference_register', 'hair_create', 'hair_patch', 'hair_capture', 'hair_validate',
  ],
  capabilities: {
    sourceSchema: 'nativegpu.hairstyle/1',
    cookedSchema: 'nativegpu.hair-cook/1',
    realScalpBinding: true,
    regionEditing: true,
    regionLocks: true,
    stableGuideIds: true,
    referenceImages: true,
    multiViewConflicts: true,
    lodLevels: 4,
    braidBundles: true,
    debugChannels: true,
    screenshotSelection: 'depth-and-id-buffer',
    gpuRenderPath: false,
    notes: [
      'texture/strand rendering must be validated with a native capture before it is claimed as done',
      'the engine does not run a vision model; analysis is supplied by the caller',
    ],
  },
});

export const GARMENT_PLUGIN = Object.freeze({
  id: 'garment-weights',
  version: '1.0.0',
  title: '角色服装权重与布料',
  actions: [
    'garment.inspect', 'garment.bind', 'garment.physics', 'garment.validate', 'garment.simulate',
    'weights.export', 'weights.patch', 'weights.binary',
  ],
  tools: ['garment_inspect', 'garment_bind', 'garment_physics_patch', 'garment_validate', 'weights_export', 'weights_patch'],
  capabilities: {
    arbitraryTriangleMesh: true,
    gridClothPreserved: true,
    regionAwareTransfer: true,
    lowConfidenceReporting: true,
    explicitMirrorMap: true,
    stableVertexIds: true,
    topologyHashGuard: true,
    dynamicBlend: true,
    maxDisplacementChannel: true,
    anchors: true,
    boneDrivenProxies: true,
    meshPenetrationMeasurement: true,
    selfCollision: false,
    layerCollision: false,
    trueSweptCcd: false,
    notes: [
      'long skirts and wide sleeves cannot be certified without self-collision',
      'proxy collision is a capsule/sphere approximation; mesh penetration is measured separately',
    ],
  },
});

const registry = new Map();

export function registerPlugin(plugin) {
  if (!plugin || typeof plugin !== 'object') throw new PluginError('NG-E-PLUGIN', 'plugin must be an object');
  if (!PLUGIN_ID.test(String(plugin.id))) throw new PluginError('NG-E-PLUGIN', `invalid plugin id ${plugin.id}`);
  if (registry.has(plugin.id)) throw new PluginError('NG-E-PLUGIN', `plugin ${plugin.id} is already registered`);
  if (!Array.isArray(plugin.actions) || plugin.actions.length === 0) {
    throw new PluginError('NG-E-PLUGIN', `plugin ${plugin.id} declares no actions`);
  }
  registry.set(plugin.id, { ...plugin, enabled: plugin.enabled !== false, capabilities: { ...plugin.capabilities } });
  return registry.get(plugin.id);
}

export function setPluginEnabled(id, enabled) {
  const plugin = registry.get(id);
  if (!plugin) throw new PluginError('NG-E-PLUGIN', `unknown plugin ${id}`);
  plugin.enabled = Boolean(enabled);
  return plugin;
}

export function getPlugin(id) {
  const plugin = registry.get(id);
  if (!plugin) return null;
  // A plugin that is registered but disabled still exists; reporting it as absent would
  // make "why did the tool disappear" impossible to answer.
  return { ...plugin, actionCount: plugin.actions.length, toolCount: plugin.tools?.length ?? 0 };
}

export function listPlugins() {
  return [...registry.values()].map((plugin) => getPlugin(plugin.id));
}

// Which plugin owns an action. Used by the editor to refuse a call into a disabled
// feature instead of half-running it.
export function pluginForAction(action) {
  for (const plugin of registry.values()) {
    if (plugin.actions.includes(action)) return plugin;
  }
  return null;
}

export function isActionEnabled(action) {
  const plugin = pluginForAction(action);
  return plugin ? plugin.enabled : false;
}

export function enabledTools() {
  const tools = [];
  for (const plugin of registry.values()) {
    if (!plugin.enabled) continue;
    for (const tool of plugin.tools ?? []) tools.push({ plugin: plugin.id, tool });
  }
  return tools;
}

// The two feature modules this repository ships. Registration happens at module load so
// the registry is complete before any tool is listed.
if (!registry.has(HAIR_PLUGIN.id)) registerPlugin(HAIR_PLUGIN);
if (!registry.has(GARMENT_PLUGIN.id)) registerPlugin(GARMENT_PLUGIN);

export const PLUGINS = registry;

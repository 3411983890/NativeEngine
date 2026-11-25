// Minimal typed material graph (v0).
//
// The graph is deliberately small: texture references, scalar constants, multiply, lerp and
// one output node. It constant-folds scalar math into the flat material record consumed by
// the runtime, so the GPU never sees a graph. This is the first step toward MIR-backed graph
// authoring; it is not a general shader DAG.
import { compileMir, materialToMir, validateMir } from './mir.mjs';
import {validateTextureMappings} from './texture-mapping.mjs';
import {MAX_IOR} from './ior.mjs';

export const GRAPH_SCHEMA = 'nativegpu.material-graph/1';
const TEXTURE_PORTS = new Set(['albedo', 'normal', 'orm', 'emissive', 'occlusion', 'coat', 'coatRoughness', 'coatNormal']);
const SCALAR_PORTS = new Set(['opacity', 'ior', 'uvScale', 'roughness', 'metalness', 'wetness', 'wear',
  'clearcoat', 'clearcoatRoughness', 'alphaCutoff', 'emissiveStrength', 'occlusionStrength']);
const NODE_TYPES = new Set(['texture', 'scalar', 'multiply', 'lerp', 'output']);
const MAX_NODES = 32;

export class GraphError extends Error {
  constructor(message) {
    super(message);
    this.name = 'GraphError';
  }
}

function fail(message) {
  throw new GraphError(message);
}

function scalarRange(port) {
  return port === 'uvScale' ? [0.05, 1000] : port==='ior'?[0,MAX_IOR]:port==='emissiveStrength'?[0,1000000]:[0, 1];
}

function numberParam(node, key) {
  const value = node.params?.[key];
  if (!Number.isFinite(value)) fail(`node ${node.id}: params.${key} must be finite`);
  return value;
}

export function validateGraph(graph) {
  if (!graph || typeof graph !== 'object' || Array.isArray(graph)) fail('graph must be an object');
  if(graph.textureMappings!==undefined)validateTextureMappings(graph.textureMappings);
  if(graph.unlit!==undefined&&typeof graph.unlit!=='boolean')fail('graph.unlit must be boolean');
  if (graph.schema !== GRAPH_SCHEMA) fail(`graph.schema must be ${GRAPH_SCHEMA}`);
  if(graph.alphaMode!==undefined&&!['OPAQUE','MASK','BLEND'].includes(graph.alphaMode))fail('graph.alphaMode must be OPAQUE, MASK or BLEND');
  if(graph.emissiveFactor!==undefined&&(!Array.isArray(graph.emissiveFactor)||graph.emissiveFactor.length!==3||graph.emissiveFactor.some(v=>!Number.isFinite(v)||v<0||v>1)))fail('graph.emissiveFactor must contain three numbers in [0,1]');
  if (typeof graph.name !== 'string' || !/^[A-Za-z0-9_.-]{1,64}$/.test(graph.name)) fail('graph.name is invalid');
  if (!Array.isArray(graph.nodes) || graph.nodes.length === 0 || graph.nodes.length > MAX_NODES) {
    fail(`graph.nodes must contain 1..${MAX_NODES} nodes`);
  }
  const ids = new Set();
  let outputs = 0;
  for (const node of graph.nodes) {
    if (!node || typeof node.id !== 'string' || !node.id) fail('every node needs an id');
    if (ids.has(node.id)) fail(`duplicate node id: ${node.id}`);
    ids.add(node.id);
    if (!NODE_TYPES.has(node.type)) fail(`node ${node.id}: unsupported type ${node.type}`);
    if (node.type === 'output') ++outputs;
    if (node.type === 'texture') {
      const name = node.params?.name;
      if (typeof name !== 'string' || !/^[A-Za-z0-9_.-]{1,64}$/.test(name)) {
        fail(`node ${node.id}: texture name is invalid`);
      }
    }
    if (node.type === 'scalar') numberParam(node, 'value');
  }
  if (outputs !== 1) fail('graph must contain exactly one output node');
  if (!Array.isArray(graph.links)) fail('graph.links must be an array');
  const outgoing = new Map();
  for (const link of graph.links) {
    if (!link || typeof link.from !== 'string' || typeof link.to !== 'string') fail('link needs from/to');
    if (!ids.has(link.from) || !ids.has(link.to)) fail(`link references unknown node: ${link.from} -> ${link.to}`);
    const from = graph.nodes.find((node) => node.id === link.from);
    const to = graph.nodes.find((node) => node.id === link.to);
    if (from.type === 'output') fail('output node cannot be a link source');
    if (to.type === 'texture' || to.type === 'scalar') fail(`${to.type} node cannot receive links`);
    const port = link.toPort ?? (to.type === 'output' ? 'albedo' : 'a');
    if (!outgoing.has(`${link.to}:${port}`)) outgoing.set(`${link.to}:${port}`, 0);
    outgoing.set(`${link.to}:${port}`, outgoing.get(`${link.to}:${port}`) + 1);
  }
  for (const [key, count] of outgoing) {
    if (count > 1) fail(`input ${key} has ${count} links; at most one is allowed`);
  }
  // Cycle detection.
  const state = new Map();
  const visit = (id) => {
    if (state.get(id) === 1) fail(`graph contains a cycle at ${id}`);
    if (state.get(id) === 2) return;
    state.set(id, 1);
    for (const link of graph.links.filter((candidate) => candidate.from === id)) visit(link.to);
    state.set(id, 2);
  };
  for (const node of graph.nodes) visit(node.id);
  return true;
}

function inputNode(graph, node, port) {
  const link = graph.links.find((candidate) => candidate.to === node.id &&
    (candidate.toPort ?? 'a') === port);
  return link ? graph.nodes.find((candidate) => candidate.id === link.from) : null;
}

function evaluateScalar(graph, node, stack = new Set()) {
  if (stack.has(node.id)) fail(`cycle evaluating ${node.id}`);
  stack.add(node.id);
  let value;
  if (node.type === 'scalar') {
    value = numberParam(node, 'value');
  } else if (node.type === 'multiply' || node.type === 'lerp') {
    const a = inputNode(graph, node, 'a');
    const b = inputNode(graph, node, 'b');
    if (!a || !b) fail(`node ${node.id}: a and b inputs are required`);
    const av = evaluateScalar(graph, a, stack);
    const bv = evaluateScalar(graph, b, stack);
    if (node.type === 'multiply') {
      value = av * bv;
    } else {
      const t = inputNode(graph, node, 't');
      if (!t) fail(`node ${node.id}: t input is required`);
      const tv = Math.max(0, Math.min(1, evaluateScalar(graph, t, stack)));
      value = av + (bv - av) * tv;
    }
  } else {
    fail(`node ${node.id}: ${node.type} is not a scalar node`);
  }
  stack.delete(node.id);
  return value;
}

/** Compile a graph to a flat material object and MIR. */
export function compileGraph(graph) {
  validateGraph(graph);
  const output = graph.nodes.find((node) => node.type === 'output');
  const material = {};
  if(graph.unlit!==undefined)material.unlit=graph.unlit;
  for (const link of graph.links.filter((candidate) => candidate.to === output.id)) {
    const port = link.toPort ?? 'albedo';
    const source = graph.nodes.find((node) => node.id === link.from);
    if (TEXTURE_PORTS.has(port)) {
      if (source.type !== 'texture') fail(`port ${port} requires a texture node`);
      material[port] = source.params.name;
    } else if (SCALAR_PORTS.has(port)) {
      if (source.type === 'texture') fail(`port ${port} requires a scalar node`);
      const value = evaluateScalar(graph, source);
      const [low, high] = scalarRange(port);
      if (!Number.isFinite(value) || value < low || value > high) {
        fail(`port ${port} value ${value} is outside [${low}, ${high}]`);
      }
      material[port] = value;
    } else {
      fail(`output port ${port} is not supported`);
    }
  }
  if(graph.alphaMode!==undefined)material.alphaMode=graph.alphaMode;
  if(graph.textureMappings!==undefined)material.textureMappings=structuredClone(graph.textureMappings);
  if(graph.emissiveFactor!==undefined)material.emissiveFactor=structuredClone(graph.emissiveFactor);
  const mir = materialToMir(graph.name, material);
  validateMir(mir);
  return {
    material,
    mir,
    compiled: compileMir(mir),
    hash: compileMir(mir).hash,
  };
}

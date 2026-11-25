import test from 'node:test';
import assert from 'node:assert/strict';
import { compileGraph, validateGraph, GRAPH_SCHEMA } from '../engine/material/graph.mjs';

function baseGraph() {
  return {
    schema: GRAPH_SCHEMA,
    name: 'graph_cobble',
    nodes: [
      { id: 'albedo', type: 'texture', params: { name: 'cobble' } },
      { id: 'normal', type: 'texture', params: { name: 'cobble' } },
      { id: 'orm', type: 'texture', params: { name: 'cobble' } },
      { id: 'wet', type: 'scalar', params: { value: 0.35 } },
      { id: 'scale', type: 'scalar', params: { value: 2 } },
      { id: 'a', type: 'scalar', params: { value: 0.8 } },
      { id: 'b', type: 'scalar', params: { value: 0.4 } },
      { id: 't', type: 'scalar', params: { value: 0.5 } },
      { id: 'lerp', type: 'lerp' },
      { id: 'out', type: 'output' },
    ],
    links: [
      { from: 'albedo', to: 'out', toPort: 'albedo' },
      { from: 'normal', to: 'out', toPort: 'normal' },
      { from: 'orm', to: 'out', toPort: 'orm' },
      { from: 'wet', to: 'out', toPort: 'wetness' },
      { from: 'scale', to: 'out', toPort: 'uvScale' },
      { from: 'a', to: 'lerp', toPort: 'a' },
      { from: 'b', to: 'lerp', toPort: 'b' },
      { from: 't', to: 'lerp', toPort: 't' },
      { from: 'lerp', to: 'out', toPort: 'roughness' },
    ],
  };
}

test('graph compiles textures and constant-folded scalar nodes', () => {
  const result = compileGraph(baseGraph());
  assert.equal(result.material.albedo, 'cobble');
  assert.equal(result.material.wetness, 0.35);
  assert.equal(result.material.uvScale, 2);
  assert.ok(Math.abs(result.material.roughness - 0.6) < 1e-9);
  assert.equal(result.hash.length, 64);
});

test('graph rejects cycles, duplicate ids and unknown ports', () => {
  const cyclic = baseGraph();
  cyclic.links.push({ from: 'lerp', to: 'lerp', toPort: 'a' });
  assert.throws(() => validateGraph(cyclic), /cycle|at most one/);
  const duplicate = baseGraph();
  duplicate.nodes.push({ id: 'albedo', type: 'scalar', params: { value: 1 } });
  assert.throws(() => validateGraph(duplicate), /duplicate/);
  const badPort = baseGraph();
  badPort.links.push({ from: 'wet', to: 'out', toPort: 'metallic' });
  assert.throws(() => compileGraph(badPort), /not supported/);
});

test('graph rejects scalar out of range and texture on scalar port', () => {
  const outOfRange = baseGraph();
  outOfRange.nodes.find((node) => node.id === 'wet').params.value = 2;
  assert.throws(() => compileGraph(outOfRange), /outside/);
  const wrongKind = baseGraph();
  wrongKind.links.find((link) => link.toPort === 'wetness').from = 'albedo';
  assert.throws(() => compileGraph(wrongKind), /requires a scalar/);
});
test('graph carries alpha-mask mode and a typed cutoff into MIR',()=>{
  const graph=baseGraph();graph.alphaMode='MASK';graph.links.push({from:'wet',to:'out',toPort:'alphaCutoff'});
  const result=compileGraph(graph);assert.equal(result.material.alphaCutoff,.35);assert.equal(result.compiled.material.alphaMode,'MASK');assert.ok(result.compiled.features.includes('alpha-mask'));
  graph.alphaMode='INVALID';assert.throws(()=>validateGraph(graph),/OPAQUE, MASK or BLEND/);
});

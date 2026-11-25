import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { validateScene, buildScene } from '../engine/scene/index.mjs';
import { gameplayDependencies } from '../engine/scene/gameplay.mjs';
const root=path.resolve(import.meta.dirname,'..');
const scenePath=path.join(root,'.screen/fogtown.scene.json');
const read=async file=>JSON.parse(await fs.readFile(file,'utf8'));

test('Fogtown content uses registered components and valid behavior dependencies',async()=>{
 const scene=await read(scenePath);assert.doesNotThrow(()=>validateScene(scene));
 const dependencies=gameplayDependencies(scene,path.join(root,'assets'));
 assert.ok(dependencies.length>=11);
 const player=scene.entities.find(e=>e.kind==='player');assert.equal(player.components.camera.mode,'firstPerson');
 const actions=scene.entities.flatMap(e=>e.components?.behavior?.instances??[]).filter(b=>b.script==='fogtown/action');
 assert.deepEqual(new Set(actions.map(b=>b.properties.action)),new Set(['briefing','tool','cache','demolish','repair','node','start','relay']));
 const plate=scene.entities.find(e=>e.name==='黄色压力板');assert.equal(plate.components.behavior.instances[0].properties.weights.length,1);
 assert.equal(scene.entities.filter(e=>e.components?.behavior?.instances?.some(b=>b.script==='fogtown/door'&&b.properties.flag==='plate')).length,2);
});

test('Fogtown content-addressed package locks gameplay scripts',async()=>{
 const output=path.join(root,'outputs/fogtown/content-test');
 const result=await buildScene(scenePath,output);
 assert.ok(result.packageDirectory);const manifest=await read(path.join(result.packageDirectory,'manifest.json'));
 const serialized=JSON.stringify(manifest);assert.match(serialized,/fogtown\/quest/);assert.match(serialized,/sha256/);
});

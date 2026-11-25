import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { validateScene } from '../engine/scene/index.mjs';
const original=JSON.parse(readFileSync('.screen/town.scene.json','utf8'));

test('lighting quality supports independent controls and preserves scene validation',()=>{
  const scene=structuredClone(original);
  scene.lighting={...scene.lighting,quality:2,maxShadowLights:2,localShadows:true,
    ibl:true,ddgi:false,cloudShadows:true,temporal:false,
    fogVolumes:[{center:[0,1,2],extent:[2,3,4],density:0.03}]};
  const light=scene.entities.find(e=>e.kind==='light').light;
  Object.assign(light,{castSurfaceShadow:false,castVolumetricShadow:true,volumetricIntensity:0});
  const result=validateScene(scene);
  assert.equal(result.valid,true,result.errors.join('\n'));
});
test('quality contracts reject fractional budgets and nonboolean rendering controls',()=>{
  for(const patch of [{quality:0.5},{quality:3},{maxShadowLights:4.5},{maxShadowLights:5},
    {localShadows:'yes'},{ddgi:1},{temporal:null},{cloudShadows:0},{keyDirection:[0,0,0]},
    {fogVolumes:[{center:[0,0,0],extent:[0,1,1],density:1}]},{fogVolumes:Array(9).fill({})}]) {
    const scene=structuredClone(original);Object.assign(scene.lighting,patch);
    assert.equal(validateScene(scene).valid,false,JSON.stringify(patch));
  }
  for(const patch of [{castSurfaceShadow:1},{castVolumetricShadow:'false'},
    {volumetricIntensity:-1},{volumetricIntensity:11}]) {
    const scene=structuredClone(original);
    Object.assign(scene.entities.find(e=>e.kind==='light').light,patch);
    assert.equal(validateScene(scene).valid,false,JSON.stringify(patch));
  }
});

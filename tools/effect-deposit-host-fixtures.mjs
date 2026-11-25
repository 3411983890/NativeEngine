import fs from 'node:fs/promises';
import path from 'node:path';
import {cookEffectLibrary} from './effects.mjs';
const directory=path.resolve('build/effects-deposit-host');await fs.mkdir(directory,{recursive:true});
const scene=JSON.parse(await fs.readFile('.screen/courtyard.scene.json','utf8'));delete scene.audio;scene.name='GPU field deposit host';scene.entities=scene.entities.filter(e=>['ground','player','key','door','goal'].includes(e.kind));scene.lighting={ambient:[.22,.24,.28],skyColor:[.04,.06,.09],keyDirection:[.3,.7,1],keyColor:[1,.95,.9],keyIntensity:2,toneMap:true,exposure:1,quality:0};await fs.writeFile(path.join(directory,'preview.scene.json'),JSON.stringify(scene,null,2)+'\n');
const template=JSON.parse(await fs.readFile('assets/effects/sprite_demo.effect.json','utf8'));
const world=structuredClone(template);world.id='deposit_world';world.field={level:'worldVisible',channels:['density'],dimensions:[32,32,32],voxelMetres:.25};world.emitters[0].modules.push({id:'density',type:'fieldDeposit',after:['fade'],params:{channel:'density',quantum:1000}});
const local=structuredClone(world);local.id='deposit_local';local.field.level='local';
const pulse=structuredClone(world);pulse.id='deposit_pulse';pulse.durationTicks=1;pulse.loop=false;pulse.maxAlive=8;pulse.budget.maxSpawnPerTick=4;pulse.emitters[0].ratePerSecond=0;pulse.emitters[0].bursts=[{tick:0,count:4}];pulse.emitters[0].lifetimeTicks=[2,2];
const base=structuredClone(template);base.id='deposit_base';const bad=structuredClone(world);bad.id='deposit_bad_grid';bad.field.voxelMetres=.3;
const admission=structuredClone(world);admission.id='deposit_admission';admission.field.dimensions=[8,8,8];admission.field.voxelMetres=1;
for(const [folder,assets] of [['effects-source',[world,local,pulse]],['base-source',[base]],['bad-source',[bad]],['mixed-source',[base,bad]],['admission-source',[admission]]]){
  const source=path.join(directory,folder);await fs.mkdir(source,{recursive:true});for(const asset of assets)await fs.writeFile(path.join(source,asset.id+'.effect.json'),JSON.stringify(asset,null,2)+'\n');
  await cookEffectLibrary({sourceDirectory:source,outputDirectory:path.join(directory,folder.replace('source','cooked'))});
}
const unicode=path.join(directory,'沉积库');await fs.mkdir(unicode,{recursive:true});for(const file of await fs.readdir(path.join(directory,'effects-cooked')))await fs.copyFile(path.join(directory,'effects-cooked',file),path.join(unicode,file));
console.log(JSON.stringify({directory}));

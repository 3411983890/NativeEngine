import fs from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { compileEffect } from '../engine/effects/asset.mjs';

export function depositFixture(){
  const emitter=(id,count,quantum)=>({id,shape:{type:'point'},position:[.25,.25,.25],ratePerSecond:0,
    bursts:[{tick:0,count}],lifetimeTicks:[12,12],localSpace:true,
    modules:[{id:'initial',type:'initialize',params:{velocity:[0,0,0],color:[1,1,1,1],size:[.1,.1]}},
      {id:'deposit',type:'fieldDeposit',after:['initial'],params:{channel:'density',quantum}}],
    renderers:[{type:'sprite',texture:'test.png',blend:'additive',lit:false}]});
  return {schema:'nativegpu.effect/1',id:'deposit_fixture',revision:0,seed:19,durationTicks:30,loop:false,maxAlive:8,
    bounds:{min:[-1,-1,-1],max:[7,7,7]},budget:{maxSpawnPerTick:8,maxEventsPerTick:0,maxDepth:0},
    field:{level:'worldVisible',channels:['density'],dimensions:[8,8,8],voxelMetres:1},
    emitters:[emitter('a',2,100),emitter('b',1,50)],events:[]};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href){
  const directory=path.resolve('build/effects-deposits');await fs.mkdir(directory,{recursive:true});
  const source=depositFixture(),compiled=compileEffect(source);
  await fs.writeFile(path.join(directory,'fixture.effect.json'),JSON.stringify(source,null,2)+'\n');
  await fs.writeFile(path.join(directory,'fixture.cooked.json'),JSON.stringify(compiled,null,2)+'\n');
  process.stdout.write(JSON.stringify({fixture:path.join(directory,'fixture.cooked.json'),hash:compiled.hash})+'\n');
}

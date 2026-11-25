import * as fs from 'node:fs/promises';
import path from 'node:path';
import {cookClothLibrary} from './cloth.mjs';
const baseDirectory=path.resolve(process.argv[2]??'build/cloth-gpu');
const sourceDirectory=path.join(baseDirectory,'source');await fs.mkdir(sourceDirectory,{recursive:true});
const asset=JSON.parse(await fs.readFile('assets/cloth/flag_banner.cloth.json','utf8'));asset.id='solver_patch';asset.mesh={type:'grid',columns:4,rows:4,widthMetres:1,heightMetres:1};asset.pins=[{column:0,row:3},{column:3,row:3}];asset.lod=[{maxDistanceMetres:20,columns:4,rows:4}];asset.budget={maxVertices:16,maxSubsteps:4,iterations:8};
await fs.writeFile(path.join(sourceDirectory,'solver_patch.cloth.json'),`${JSON.stringify(asset,null,2)}\n`);
const result=await cookClothLibrary({sourceDirectory,outputDirectory:path.join(baseDirectory,'fixtures')});console.log(JSON.stringify({hash:result.packageHash}));
// Explicit legacy fixtures exercise the /1 distance solver; angular compliance
// has different units and is never numerically converted to distance compliance.
const legacyDirectory=path.join(baseDirectory,'legacy-source');await fs.mkdir(legacyDirectory,{recursive:true});
for(const name of (await fs.readdir('assets/cloth')).filter(n=>n.endsWith('.cloth.json')).sort()){
  const legacy=JSON.parse(await fs.readFile(path.join('assets/cloth',name),'utf8'));
  legacy.schema='nativegpu.cloth/1';delete legacy.material.bendAngleCompliance;legacy.material.bendCompliance=4e-5;
  await fs.writeFile(path.join(legacyDirectory,name),`${JSON.stringify(legacy,null,2)}\n`);
}
await cookClothLibrary({sourceDirectory:legacyDirectory,outputDirectory:path.join(baseDirectory,'legacy')});

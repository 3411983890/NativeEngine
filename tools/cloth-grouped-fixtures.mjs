import * as fs from 'node:fs/promises';
import path from 'node:path';
import {cookClothLibrary} from './cloth.mjs';
const directory=path.resolve('build/cloth-grouped');
const source=path.join(directory,'source');await fs.mkdir(source,{recursive:true});
const base=JSON.parse(await fs.readFile('assets/cloth/flag_banner.cloth.json','utf8'));
for(const [id,columns,rows] of [['group_limit',32,16],['dispatch_fallback',33,16]]){
  const asset=structuredClone(base);asset.id=id;asset.mesh={type:'grid',columns,rows,widthMetres:1.2,heightMetres:.8};asset.pins=[{column:0,row:rows-1},{column:columns-1,row:rows-1}];
  asset.lod=[{maxDistanceMetres:20,columns,rows}];asset.budget={maxVertices:columns*rows,maxSubsteps:4,iterations:8};
  await fs.writeFile(path.join(source,`${id}.cloth.json`),`${JSON.stringify(asset,null,2)}\n`);
}
await cookClothLibrary({sourceDirectory:source,outputDirectory:path.join(directory,'fixtures')});
await cookClothLibrary({sourceDirectory:'assets/cloth',outputDirectory:path.join(directory,'cooked')});
console.log(JSON.stringify({directory}));

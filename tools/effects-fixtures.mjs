import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { encodePng, cookDirectory } from './textures.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const source=path.join(root,'assets/source/effects');
const out=path.join(root,'assets/effects/textures');
mkdirSync(source,{recursive:true});
const size=64,data=Buffer.alloc(size*size*4);
for(let y=0;y<size;y++)for(let x=0;x<size;x++) {
  const radius=Math.hypot((x+.5-size/2)/(size/2),(y+.5-size/2)/(size/2));
  const alpha=Math.max(0,1-radius*radius)**3;
  const i=(y*size+x)*4;data[i]=data[i+1]=data[i+2]=255;data[i+3]=Math.round(alpha*255);
}
const bytes=encodePng({width:size,height:size,data});
const file=path.join(source,'reference_glow.albedo.png');
if(existsSync(file)&&!readFileSync(file).equals(bytes))throw new Error('Authored reference glow differs; preserve it and choose a new fixture name before regenerating.');
if(!existsSync(file))writeFileSync(file,bytes);
cookDirectory({source,out,size,format:'rgba8'});
console.log('Effect glow fixture: 64x64 RGBA8 with alpha and full mip chain.');

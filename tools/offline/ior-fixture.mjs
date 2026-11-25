import fs from 'node:fs/promises';
import path from 'node:path';
import {writeTransparencyFixture} from './transparency-fixture.mjs';
export async function writeIorFixture(directory,{ior=1.5,character=false,metalness=0,clearcoat=0,alphaMode='OPAQUE'}={}){
  const file=await writeTransparencyFixture(directory,{character,unlit:false,layers:[{color:[.25,.25,.25,alphaMode==='BLEND'?.6:1],mode:alphaMode}]}),doc=JSON.parse(await fs.readFile(file));
  doc.extensionsRequired=[...(doc.extensionsRequired??[]),'KHR_materials_ior'];
  for(const material of doc.materials){material.alphaMode=alphaMode;material.pbrMetallicRoughness.metallicFactor=metalness;material.pbrMetallicRoughness.roughnessFactor=1;material.extensions={...(material.extensions??{}),KHR_materials_ior:{ior},...(clearcoat?{KHR_materials_clearcoat:{clearcoatFactor:clearcoat,clearcoatRoughnessFactor:.2}}:{})};}
  const output=path.join(directory,'ior.gltf');await fs.writeFile(output,JSON.stringify(doc)+'\n');return output;
}

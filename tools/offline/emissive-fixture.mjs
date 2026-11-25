import fs from 'node:fs/promises';
import path from 'node:path';
import {writeCutoutAssetFixture} from '../asset-fixtures.mjs';

// Original cards use black base color, so their visible RGB can only come from
// independent emission. The last two share a color and differ only in strength.
export async function writeEmissiveFixture(directory){
  const file=await writeCutoutAssetFixture(false,directory),doc=JSON.parse(await fs.readFile(file,'utf8'));
  const colors=[[.4,0,0],[0,.4,0],[0,0,.4],[.02,.08,.16],[.02,.08,.16]],strengths=[1,1,1,1,4],primitive=doc.meshes[0].primitives[0];
  doc.materials=colors.map((emissiveFactor,i)=>({doubleSided:true,pbrMetallicRoughness:{baseColorFactor:[0,0,0,1],metallicFactor:0,roughnessFactor:1},emissiveFactor,extensions:{KHR_materials_emissive_strength:{emissiveStrength:strengths[i]}}}));
  doc.meshes=colors.map((_,material)=>({primitives:[{...primitive,material}]}));doc.nodes=colors.map((_,mesh)=>({name:'Emission '+mesh,mesh,translation:[(mesh-2)*2.5,0,0]}));doc.scenes=[{nodes:colors.map((_,i)=>i)}];doc.extensionsRequired=['KHR_materials_emissive_strength'];doc.extensionsUsed=[...doc.extensionsRequired];
  const output=path.join(directory,'emission.gltf');await fs.writeFile(output,JSON.stringify(doc)+'\n');return output;
}

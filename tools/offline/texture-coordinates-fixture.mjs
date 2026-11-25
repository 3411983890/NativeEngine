import fs from 'node:fs/promises';
import path from 'node:path';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';
import {writeTextureMappingFixture,writeMappingCharacterFixture} from './texture-mapping-fixture.mjs';

export async function writeMultiUvFixture(directory,{character=false,semantic='albedo',baked=false,all=false,sourceSet=7}={}) {
  const source=character?await writeMappingCharacterFixture(directory):await writeTextureMappingFixture(directory,{semantic});
  const original=await readGltf(source),doc=structuredClone(original.document),slots=['albedo','normal','orm','emissive','occlusion'];
  const append=rows=>{
    const bytes=Buffer.alloc(rows.length*8);rows.flat().forEach((v,i)=>bytes.writeFloatLE(v,i*4));
    const buffer=doc.buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;
    doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,componentType:5126,type:'VEC2',count:rows.length});return index;
  };
  for(const mesh of doc.meshes)for(const primitive of mesh.primitives){
    const uv=readAccessor(original,primitive.attributes.TEXCOORD_0,'VEC2');
    for(const set of all?[1,2,3,4,5]:[sourceSet]){
      // Keep the reference UV0 tangent orientation: generated neutral normal
      // maps have finite 8-bit precision, even when the source has no normal map.
      const values=uv.map(([u,v])=>[.06*(set%5)+.4*u,.12+.6*v]);
      primitive.attributes[`TEXCOORD_${set}`]=append(values);
      if(baked)primitive.attributes.TEXCOORD_0=primitive.attributes[`TEXCOORD_${set}`];
    }
  }
  for(const material of doc.materials){
    material.pbrMetallicRoughness={baseColorFactor:[.6,.6,.6,1],metallicFactor:0,roughnessFactor:1};delete material.normalTexture;delete material.emissiveTexture;delete material.emissiveFactor;delete material.occlusionTexture;
    for(const [i,slot]of slots.entries())if(all||semantic===slot){
      const info={index:0,texCoord:baked?0:42,extensions:{KHR_texture_transform:{offset:[.13,.23],scale:[1.4,-.9],rotation:.27,...(baked?{}:{texCoord:all?i+1:sourceSet})}}};
      if(slot==='albedo')material.pbrMetallicRoughness.baseColorTexture=info;
      if(slot==='normal')material.normalTexture=info;
      if(slot==='orm'){material.pbrMetallicRoughness.metallicRoughnessTexture=info;material.pbrMetallicRoughness.metallicFactor=1;}
      if(slot==='emissive'){material.emissiveTexture=info;material.emissiveFactor=all?[.15,.15,.15]:[1,1,1];if(!all)material.pbrMetallicRoughness.baseColorFactor=[0,0,0,1];}
      if(slot==='occlusion')material.occlusionTexture={...info,strength:.7};
    }
  }
  doc.extensionsRequired=['KHR_texture_transform'];const file=path.join(directory,'multi-uv.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}

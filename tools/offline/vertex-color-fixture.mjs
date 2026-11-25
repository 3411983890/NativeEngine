import fs from 'node:fs/promises';
import path from 'node:path';
import {writeCutoutAssetFixture,writeCharacterFixture} from '../asset-fixtures.mjs';
import {writeMultiUvFixture} from './texture-coordinates-fixture.mjs';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';

export async function writeVertexColorFixture(directory,{character=false,unlit=true,masked=false,componentType=5126,rgb=false,constant,multiUv=false,baked=false}={}){
  const source=multiUv?await writeMultiUvFixture(directory,{character,all:true}):character?await writeCharacterFixture(directory):await writeCutoutAssetFixture(masked,directory);
  const original=await readGltf(source),doc=structuredClone(original.document),components=rgb?3:4,size=componentType===5121?1:componentType===5123?2:4,max=componentType===5121?255:65535;
  for(const mesh of doc.meshes)for(const primitive of mesh.primitives){
    const uv=readAccessor(original,primitive.attributes.TEXCOORD_0,'VEC2'),bytes=Buffer.alloc(uv.length*components*size);
    uv.forEach(([u,v],i)=>{const row=constant??[u,v,.2+.6*(1-u),masked?u:1];for(let k=0;k<components;k++){const at=(i*components+k)*size;if(componentType===5126)bytes.writeFloatLE(row[k],at);else if(size===1)bytes.writeUInt8(Math.round(row[k]*max),at);else bytes.writeUInt16LE(Math.round(row[k]*max),at);}});
    const buffer=doc.buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,type:rgb?'VEC3':'VEC4',componentType,count:uv.length,...(componentType===5126?{}:{normalized:true})});if(!baked)primitive.attributes.COLOR_0=index;
  }
  for(const material of doc.materials){
    if(!multiUv)material.pbrMetallicRoughness={baseColorFactor:baked?constant:[1,1,1,1],metallicFactor:0,roughnessFactor:1};
    if(unlit)material.extensions={...(material.extensions??{}),KHR_materials_unlit:{}};
    if(masked){material.alphaMode='MASK';material.alphaCutoff=.5;}
  }
  if(unlit)doc.extensionsRequired=[...new Set([...(doc.extensionsRequired??[]),'KHR_materials_unlit'])];
  const file=path.join(directory,'vertex-colors.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}

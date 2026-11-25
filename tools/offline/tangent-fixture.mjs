import fs from 'node:fs/promises';
import path from 'node:path';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';
import {writeVertexColorFixture} from './vertex-color-fixture.mjs';
import {encodePng} from '../textures.mjs';

export async function writeTangentFixture(directory,{character=false,angle=0,sign=1,componentType=5126,without=false,doubleSided=false,scale,constantNormal,normalTransform,blend=false,normalMap=true}={}){
  const input=await writeVertexColorFixture(directory,{character,unlit:false,multiUv:true}),source=await readGltf(input),doc=structuredClone(source.document);
  const length=v=>Math.hypot(...v),unit=v=>v.map(n=>n/length(v)),cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
  for(const mesh of doc.meshes)for(const primitive of mesh.primitives){
    const normals=primitive.attributes.NORMAL===undefined?readAccessor(source,primitive.attributes.POSITION,'VEC3').map(()=>[0,0,1]):readAccessor(source,primitive.attributes.NORMAL,'VEC3'),size=componentType===5120?1:componentType===5122?2:4,bytes=Buffer.alloc(normals.length*4*size);
    if(primitive.attributes.NORMAL===undefined){const raw=Buffer.alloc(normals.length*12);normals.flat().forEach((v,i)=>raw.writeFloatLE(v,i*4));const buffer=doc.buffers.length,view=doc.bufferViews.length;primitive.attributes.NORMAL=doc.accessors.length;doc.buffers.push({byteLength:raw.length,uri:'data:application/octet-stream;base64,'+raw.toString('base64')});doc.bufferViews.push({buffer,byteLength:raw.length});doc.accessors.push({bufferView:view,componentType:5126,type:'VEC3',count:normals.length});}
    normals.forEach((value,i)=>{const n=unit(value),axis=Math.abs(n[0])<.8?[1,0,0]:[0,1,0],dot=n.reduce((sum,v,k)=>sum+v*axis[k],0),t=unit(axis.map((v,k)=>v-dot*n[k])),b=cross(n,t),row=[...t.map((v,k)=>v*Math.cos(angle)+b[k]*Math.sin(angle)),sign];row.forEach((v,k)=>{const at=(i*4+k)*size;if(size===4)bytes.writeFloatLE(v,at);else if(size===2)bytes.writeInt16LE(Math.round(v*32767),at);else bytes.writeInt8(Math.round(v*127),at);});});
    const buffer=doc.buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,componentType,type:'VEC4',count:normals.length,...(size===4?{}:{normalized:true})});if(!without)primitive.attributes.TANGENT=index;
  }
  for(const material of doc.materials)material.doubleSided=doubleSided;
  if(constantNormal){const png=encodePng({width:2,height:2,data:Buffer.from(Array.from({length:4},()=>[...constantNormal,255]).flat())}),image=doc.images.length,texture=doc.textures.length;doc.images.push({uri:'data:image/png;base64,'+png.toString('base64')});doc.textures.push({source:image});for(const material of doc.materials){material.pbrMetallicRoughness={baseColorFactor:[.7,.7,.7,1],metallicFactor:0,roughnessFactor:1};delete material.emissiveTexture;delete material.emissiveFactor;delete material.occlusionTexture;material.normalTexture={index:texture,texCoord:2,...(normalTransform?{extensions:{KHR_texture_transform:normalTransform}}:{})};}}
  if(componentType!==5126){doc.extensionsUsed=[...new Set([...(doc.extensionsUsed??[]),'KHR_mesh_quantization'])];doc.extensionsRequired=[...new Set([...(doc.extensionsRequired??[]),'KHR_mesh_quantization'])];}
  if(scale)for(const node of doc.nodes)if(node.mesh!==undefined)node.scale=scale;
  if(!normalMap)for(const material of doc.materials)delete material.normalTexture;
  if(blend)for(const material of doc.materials){material.alphaMode='BLEND';material.pbrMetallicRoughness.baseColorFactor[3]=.5;}
  const file=path.join(directory,'tangents.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}

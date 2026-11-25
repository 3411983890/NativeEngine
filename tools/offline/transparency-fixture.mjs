import fs from 'node:fs/promises';
import path from 'node:path';
import {encodePng} from '../textures.mjs';
import {writeVertexColorFixture} from './vertex-color-fixture.mjs';

// Original, freely reusable panels. z and slope are expressed in engine coordinates.
export async function writeTransparencyFixture(directory,{layers=[{color:[1,0,0,.5],z:.25},{color:[0,0,1,.5],z:-.25}],character=false,textureAlpha=255,vertexAlpha=1,unlit=true}={}){
  await fs.mkdir(directory,{recursive:true});
  if(character){
    const file=await writeVertexColorFixture(directory,{character:true,constant:[1,1,1,vertexAlpha],unlit}),doc=JSON.parse(await fs.readFile(file));
    doc.materials.forEach((m,i)=>{m.alphaMode='BLEND';m.pbrMetallicRoughness.baseColorFactor=layers[i%layers.length].color;});
    await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
  }
  const doc={asset:{version:'2.0',generator:'nativegpu-original-transparency-fixture'},scene:0,scenes:[{nodes:[]}],nodes:[],meshes:[],materials:[],accessors:[],bufferViews:[],buffers:[],extensionsUsed:unlit?['KHR_materials_unlit']:[]};
  const append=(values,type,components)=>{const bytes=Buffer.alloc(values.length*4);values.forEach((v,i)=>bytes.writeFloatLE(v,i*4));const buffer=doc.buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,type,componentType:5126,count:values.length/components});return index;};
  if(textureAlpha!==255){const data=Buffer.from([255,255,255,textureAlpha]);doc.images=[{uri:'data:image/png;base64,'+encodePng({width:1,height:1,data}).toString('base64')}];doc.textures=[{source:0}];}
  for(const [i,layer]of layers.entries()){
    const positions=[[-1,0],[1,0],[1,2],[-1,2]],p=positions.flatMap(([x,y])=>[x,y,-((layer.z??0)+x*(layer.slope??0))]);
    const attributes={POSITION:append(p,'VEC3',3),TEXCOORD_0:append([0,1,1,1,1,0,0,0],'VEC2',2)};
    if(vertexAlpha!==1)attributes.COLOR_0=append(positions.flatMap(()=>[1,1,1,vertexAlpha]),'VEC4',4);
    const bytes=Buffer.from([0,0,1,0,2,0,0,0,2,0,3,0]),buffer=doc.buffers.length,view=doc.bufferViews.length,indices=doc.accessors.length;doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,type:'SCALAR',componentType:5123,count:6});
    doc.nodes.push({name:'Panel '+i,mesh:i});doc.scenes[0].nodes.push(i);doc.meshes.push({primitives:[{attributes,indices,material:i}]});
    doc.materials.push({name:'Layer '+i,alphaMode:layer.mode??'BLEND',doubleSided:true,...(unlit?{extensions:{KHR_materials_unlit:{}}}:{}),pbrMetallicRoughness:{baseColorFactor:layer.color,metallicFactor:0,roughnessFactor:1,...(doc.textures?{baseColorTexture:{index:0}}:{})}});
  }
  const file=path.join(directory,'layers.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}

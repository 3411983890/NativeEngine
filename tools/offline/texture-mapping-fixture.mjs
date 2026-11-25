import fs from 'node:fs/promises';
import path from 'node:path';
import {writeCutoutAssetFixture,writeCharacterFixture} from '../asset-fixtures.mjs';
import {encodePng} from '../textures.mjs';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';
export async function writeMappingCharacterFixture(directory,{baked=false,semantic='emissive'}={}){
  const source=await writeCharacterFixture(directory),original=await readGltf(source),doc=structuredClone(original.document),transform={offset:[.2,.7],scale:[1.3,-.8],rotation:.31};
  doc.images=[{uri:'data:image/png;base64,'+encodePng({width:2,height:2,data:Buffer.from([240,20,30,255,20,220,50,255,30,60,240,255,230,200,20,255])}).toString('base64')}];doc.textures=[{source:0,sampler:0}];doc.samplers=[{wrapS:33648,wrapT:33071,minFilter:9987,magFilter:9729}];
  for(const material of doc.materials){material.pbrMetallicRoughness={baseColorFactor:[0,0,0,1],metallicFactor:1,roughnessFactor:1};material.emissiveFactor=[1,1,1];material.emissiveTexture={index:0,...(baked?{}:{extensions:{KHR_texture_transform:transform}})};}
  if(semantic==='occlusion')for(const material of doc.materials){material.pbrMetallicRoughness={baseColorFactor:[.7,.7,.7,1],metallicFactor:0,roughnessFactor:1};material.occlusionTexture={...material.emissiveTexture,strength:.6};delete material.emissiveTexture;delete material.emissiveFactor;}
  if(baked)for(const mesh of doc.meshes)for(const primitive of mesh.primitives){const uv=readAccessor(original,primitive.attributes.TEXCOORD_0,'VEC2'),data=Buffer.alloc(uv.length*8),c=Math.cos(transform.rotation),s=Math.sin(transform.rotation);uv.forEach(([u,v],i)=>{const x=u*transform.scale[0],y=v*transform.scale[1];data.writeFloatLE(c*x-s*y+transform.offset[0],i*8);data.writeFloatLE(s*x+c*y+transform.offset[1],i*8+4);});const buffer=doc.buffers.length,view=doc.bufferViews.length,accessor=doc.accessors.length;doc.buffers.push({byteLength:data.length,uri:'data:application/octet-stream;base64,'+data.toString('base64')});doc.bufferViews.push({buffer,byteLength:data.length});doc.accessors.push({bufferView:view,componentType:5126,type:'VEC2',count:uv.length});primitive.attributes.TEXCOORD_0=accessor;}
  else doc.extensionsRequired=['KHR_texture_transform'];
  const output=path.join(directory,'character-mapping.gltf');await fs.writeFile(output,JSON.stringify(doc)+'\n');return output;
}
export async function writeTextureMappingFixture(directory,{semantic='albedo',baked=false,transform={offset:[.25,.75],scale:[2,-.5],rotation:Math.PI/2},sampler={wrapS:33071,wrapT:33648,magFilter:9728,minFilter:9987}}={}){
  const file=await writeCutoutAssetFixture(false,directory),doc=JSON.parse(await fs.readFile(file)),pixels=Buffer.alloc(8*8*4);
  for(let y=0;y<8;y++)for(let x=0;x<8;x++){const p=(y*8+x)*4;if(semantic==='normal')pixels.set([x<4?180:76,y<4?160:96,240,255],p);else if(semantic==='orm')pixels.set([x<4?60:255,y<4?70:240,x%2?0:255,255],p);else pixels.set([x<4?240:20,y<4?200:30,(x+y)%2?210:40,255],p);}
  doc.images=[{uri:'data:image/png;base64,'+encodePng({width:8,height:8,data:pixels}).toString('base64')}];doc.textures=[{source:0,sampler:0}];doc.samplers=[sampler];
  const material={doubleSided:true,pbrMetallicRoughness:{baseColorFactor:[.7,.7,.7,1],metallicFactor:1,roughnessFactor:1}},info={index:0};
  if(semantic==='albedo')material.pbrMetallicRoughness.baseColorTexture=info;
  else if(semantic==='orm')material.pbrMetallicRoughness.metallicRoughnessTexture=info;
  else if(semantic==='normal')material.normalTexture=info;
  else if(semantic==='occlusion'){material.occlusionTexture={...info,strength:.6};material.pbrMetallicRoughness.metallicFactor=0;}
  else{material.pbrMetallicRoughness.baseColorFactor=[0,0,0,1];material.emissiveTexture=info;material.emissiveFactor=[1,1,1];}
  if(!baked){info.extensions={KHR_texture_transform:transform};doc.extensionsRequired=['KHR_texture_transform'];}
  else{
    const binary=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),view=doc.bufferViews[doc.accessors[doc.meshes[0].primitives[0].attributes.TEXCOORD_0].bufferView],offset=view.byteOffset??0,c=Math.cos(transform.rotation??0),s=Math.sin(transform.rotation??0),scale=transform.scale??[1,1],translation=transform.offset??[0,0];
    // Independent reference: change source UV vertices before import. No runtime
    // material mapping is present in the baked control.
    for(let i=0;i<4;i++){const x=binary.readFloatLE(offset+i*8)*scale[0],y=binary.readFloatLE(offset+i*8+4)*scale[1];binary.writeFloatLE(c*x-s*y+translation[0],offset+i*8);binary.writeFloatLE(s*x+c*y+translation[1],offset+i*8+4);}
    doc.buffers[0].uri='data:application/octet-stream;base64,'+binary.toString('base64');
  }
  // info is attached before the extension is filled for all standard slots.
  if(semantic==='occlusion')material.occlusionTexture={...info,strength:.6};
  doc.materials=[material];const output=path.join(directory,'mapping.gltf');await fs.writeFile(output,JSON.stringify(doc)+'\n');return output;
}

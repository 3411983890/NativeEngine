import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {randomUUID} from 'node:crypto';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {sha256Hex} from '../../engine/assets/canonical.mjs';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';
import {importAsset} from '../../engine/assets/import-service.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkPublicMapping({download=false}={}){
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const provenance=JSON.parse(await fs.readFile(path.join(root,'tests/fixtures/texture-mapping-public.json'))),cache=path.join(root,'build/asset-mapping-fixtures/public'),output=path.join(root,'build/asset-import-smoke/mapping/public');await fs.mkdir(output,{recursive:true});
  for(const entry of provenance.files){const file=path.join(cache,entry.file);let bytes=await fs.readFile(file).catch(e=>{if(e.code!=='ENOENT')throw e;return null;});if(!bytes&&download){const response=await fetch(entry.url,{signal:AbortSignal.timeout(60000)});assert.ok(response.ok);bytes=Buffer.from(await response.arrayBuffer());assert.equal(bytes.length,entry.bytes);assert.equal(sha256Hex(bytes),entry.sha256);await fs.mkdir(path.dirname(file),{recursive:true});await fs.writeFile(file,bytes,{flag:'wx'});}assert.ok(bytes,'Public mapping asset is missing; use --download once');assert.equal(bytes.length,entry.bytes);assert.equal(sha256Hex(bytes),entry.sha256);}
  const results=[];
  for(const model of provenance.models){
    const source=path.join(cache,model.entry),asset=await importAsset({source,output:path.join(output,'imports'),size:256}),parts=asset.manifest.parts,min=[0,1,2].map(k=>Math.min(...parts.map(p=>p.position[k]-p.scale[k]/2))),max=[0,1,2].map(k=>Math.max(...parts.map(p=>p.position[k]+p.scale[k]/2))),center=min.map((v,k)=>(v+max[k])/2),distance=Math.max(max[1]-min[1],(max[0]-min[0])*.75)*1.2+3;
    const camera=[center[0],center[1],min[2]-distance,...center].join(','),base=['--smoke','--no-stress','--frames','120','--render-size','800x600','--character',path.join(root,'build/animation/mannequin/character.json'),'--animation-features','off','--audio','off','--camera',camera,'--screenshot-frame','100'];
    const render=async(name,directory)=>{const report=path.join(output,name+'.json'),screenshot=path.join(output,name+'.png');await run(path.join(root,'build/assets-host/NativeGPU.exe'),[...base,'--asset',directory,'--report',report,'--screenshot',screenshot],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});assert.equal(JSON.parse(await fs.readFile(report)).success,true);return sharp(screenshot).removeAlpha().raw().toBuffer();};
    const pixels=await render(model.name,asset.directory);let colored=0;for(let p=0;p<pixels.length;p+=3)if(Math.max(...pixels.subarray(p,p+3))-Math.min(...pixels.subarray(p,p+3))>20)colored++;assert.ok(colored>1000,model.name+' must be visible');
    let comparison=null,greenPassPixels=null;
    if(model.name==='TextureSettingsTest'){
      let red=0,green=0;for(let p=0;p<pixels.length;p+=3){const [r,g,b]=pixels.subarray(p,p+3);if(r>25&&r>g*1.8&&r>b*1.8)red++;if(g>25&&g>r*1.8&&g>b*1.8)green++;}
      assert.equal(red,0,'public sampler test must not expose red failure symbols');assert.ok(green>1000,'public sampler test must show green pass symbols');greenPassPixels=green;
    }
    if(model.name==='TextureTransformTest'){
      const variants=path.join(output,'baked-'+randomUUID());await fs.cp(path.dirname(source),variants,{recursive:true});const original=await readGltf(source),doc=structuredClone(original.document);
      for(const mesh of doc.meshes)for(const primitive of mesh.primitives){const info=doc.materials[primitive.material]?.pbrMetallicRoughness?.baseColorTexture,transform=info?.extensions?.KHR_texture_transform;if(!transform)continue;const index=primitive.attributes['TEXCOORD_'+(transform.texCoord??info.texCoord??0)],uv=readAccessor(original,index,'VEC2'),c=Math.cos(transform.rotation??0),s=Math.sin(transform.rotation??0),scale=transform.scale??[1,1],offset=transform.offset??[0,0],data=Buffer.alloc(uv.length*8);uv.forEach(([u,v],i)=>{const x=u*scale[0],y=v*scale[1];data.writeFloatLE(c*x-s*y+offset[0],i*8);data.writeFloatLE(s*x+c*y+offset[1],i*8+4);});const buffer=doc.buffers.length,view=doc.bufferViews.length,accessor=doc.accessors.length;doc.buffers.push({byteLength:data.length,uri:'data:application/octet-stream;base64,'+data.toString('base64')});doc.bufferViews.push({buffer,byteLength:data.length});doc.accessors.push({bufferView:view,componentType:5126,type:'VEC2',count:uv.length});primitive.attributes.TEXCOORD_0=accessor;}
      for(const m of doc.materials){const info=m.pbrMetallicRoughness?.baseColorTexture;if(info?.extensions?.KHR_texture_transform){delete info.extensions.KHR_texture_transform;if(!Object.keys(info.extensions).length)delete info.extensions;info.texCoord=0;}}
      doc.extensionsRequired=doc.extensionsRequired?.filter(e=>e!=='KHR_texture_transform');const bakedFile=path.join(variants,'baked.gltf');await fs.writeFile(bakedFile,JSON.stringify(doc));const baked=await importAsset({source:bakedFile,output:path.join(output,'imports'),size:256}),control=await render(model.name+'-baked',baked.directory);let maximum=0,total=0;for(let p=0;p<pixels.length;p++){const delta=Math.abs(pixels[p]-control[p]);maximum=Math.max(maximum,delta);total+=delta;}comparison={maxChannelDelta:maximum,meanChannelDelta:total/pixels.length};assert.ok(maximum<=5&&comparison.meanChannelDelta<.15,'public transform differs from independently baked source vertices');
    }
    results.push({model:model.name,importId:asset.manifest.id,coloredPixels:colored,comparison,greenPassPixels});await fs.copyFile(path.join(cache,model.name,'README.md'),path.join(output,model.name+'-LICENSE.md'));
  }
  const evidence={success:true,models:results};await fs.writeFile(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2));await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkPublicMapping({download:process.argv.includes('--download')}).then(v=>console.log(JSON.stringify(v))).catch(e=>{console.error(e.stderr||e.stack);process.exitCode=1;});

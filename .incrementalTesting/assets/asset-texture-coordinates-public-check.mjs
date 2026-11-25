import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {randomUUID} from 'node:crypto';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {prepareAssetHost} from '../../tools/asset-host-scope.mjs';
import {sha256Hex} from '../../engine/assets/canonical.mjs';
import {readGltf} from '../../engine/assets/gltf-document.mjs';
import {importAsset} from '../../engine/assets/import-service.mjs';
import {buildScene} from '../../engine/scene/index.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkPublicTextureCoordinates({download=false,context}={}){
  context??=await prepareAssetHost();await context.assertStable();
  const output=path.join(root,'build/asset-import-smoke/texture-coordinates/public'),cache=path.join(root,'build/asset-multi-uv-fixtures/public'),provenance=JSON.parse(await fs.readFile(path.join(root,'tests/fixtures/texture-coordinates-public.json')));await fs.mkdir(output,{recursive:true});
  for(const entry of provenance.files){
    const file=path.join(cache,entry.file);let bytes=await fs.readFile(file).catch(error=>{if(error.code!=='ENOENT')throw error;return null;});
    if(!bytes&&download){const response=await fetch(entry.url,{signal:AbortSignal.timeout(60000)});assert.ok(response.ok);bytes=Buffer.from(await response.arrayBuffer());assert.equal(bytes.length,entry.bytes);assert.equal(sha256Hex(bytes),entry.sha256);await fs.mkdir(path.dirname(file),{recursive:true});await fs.writeFile(file,bytes,{flag:'wx'});}
    assert.ok(bytes,'Public multi-UV asset is missing; use --download once');assert.equal(bytes.length,entry.bytes);assert.equal(sha256Hex(bytes),entry.sha256);
  }
  const source=path.join(cache,'MultiUVTest.glb'),asset=await importAsset({source,output:path.join(output,'imports'),size:512});assert.ok(asset.manifest.parts.some(p=>p.textureCoordinates?.emissive===1));
  // A deliberately wrong UV0 variant must visibly differ from the original.
  const original=await readGltf(source),doc=structuredClone(original.document),work=path.join(output,'control-'+randomUUID());await fs.mkdir(work);
  doc.buffers=original.buffers.map(bytes=>({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')}));for(const material of doc.materials)if(material.emissiveTexture)material.emissiveTexture.texCoord=0;
  const wrongFile=path.join(work,'wrong-uv0.gltf');await fs.writeFile(wrongFile,JSON.stringify(doc));const wrong=await importAsset({source:wrongFile,output:path.join(output,'imports'),size:512});
  const base=['--smoke','--no-stress','--frames','240','--render-size','640x480','--character',path.join(root,'build/animation/mannequin/character.json'),'--animation-features','off','--audio','off','--camera','4,3,-6,0,0,0','--screenshot-frame','200'];
  const render=async(name,args,cwd=root)=>{await context.assertStable();const report=path.join(output,name+'.json'),image=path.join(output,name+'.png');await run(context.executable,[...base,...args,'--report',report,'--screenshot',image],{cwd,windowsHide:true,timeout:120000,maxBuffer:32768});const data=JSON.parse(await fs.readFile(report));assert.equal(data.success,true);return {data,pixels:await sharp(image).removeAlpha().raw().toBuffer()};};
  const record=path.join(output,'record.csv'),on=await render('original',['--asset',asset.directory,'--record',record]),control=await render('wrong-uv0',['--asset',wrong.directory]);let changed=0,maximum=0;
  for(let p=0;p<on.pixels.length;p+=3){const delta=Math.max(...[0,1,2].map(k=>Math.abs(on.pixels[p+k]-control.pixels[p+k])));maximum=Math.max(maximum,delta);if(delta>=2)changed++;}assert.ok(changed>500,'Second UV set must visibly affect the public model');
  const built=await buildScene(path.join(asset.directory,'preview.scene.json'),path.join(output,'packages')),packaged=await render('packaged',['--scene',path.join(built.packageDirectory,'scene.json'),'--textures',path.join(built.packageDirectory,'textures'),'--replay',record,'--replay-strict'],built.packageDirectory);assert.equal(packaged.data.replay.mismatches,0);assert.deepEqual(on.pixels,packaged.pixels);assert.equal(on.data.determinism.finalHash,packaged.data.determinism.finalHash);await context.assertStable();
  for(const name of ['README.md','KHRONOS-LOGO-LICENSE.txt'])await fs.copyFile(path.join(cache,name),path.join(output,name));await fs.writeFile(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
  const evidence={success:true,host:context.fingerprint,model:provenance.model,sourceSha256:provenance.files.find(f=>f.file.endsWith('.glb')).sha256,importId:asset.manifest.id,changedPixels:changed,maxChannelDelta:maximum,identicalPackagePixels:true,strictReplayFrames:240};await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkPublicTextureCoordinates({download:process.argv.includes('--download')}).then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.stderr||error.stack);process.exitCode=1;});

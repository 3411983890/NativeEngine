// Optional public-content acceptance case. Network access is explicit and is not
// part of test:assets or the default native incremental entry point.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {readGltf,requireAsset} from '../../engine/assets/gltf-document.mjs';
import {sha256Hex,canonicalJson} from '../../engine/assets/canonical.mjs';
import {importAsset,appendAnimation,verifyImport} from '../../engine/assets/import-service.mjs';

export async function preparePublicCharacter({download=false,directory='build/asset-import-smoke/public'}={}) {
  directory=path.resolve(directory);await fs.mkdir(directory,{recursive:true});
  const model=path.join(directory,'CesiumMan.glb'),provenance=path.join(directory,'source.json');
  async function fetchBytes(url) {
    const response=await fetch(url,{signal:AbortSignal.timeout(30000),headers:{'User-Agent':'NativeGPU-asset-acceptance'}});
    requireAsset(response.ok,`Public sample download failed: HTTP ${response.status}`,'IO');
    const bytes=Buffer.from(await response.arrayBuffer());requireAsset(bytes.length<=16*1024*1024,'Public acceptance sample exceeds 16 MiB','BUDGET');return bytes;
  }
  if(download) {
    const commit=JSON.parse((await fetchBytes('https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/commits/main')).toString()).sha;
    requireAsset(/^[a-f0-9]{40}$/.test(commit),'Invalid public sample revision');
    const base=`https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/${commit}/`,url=base+'Models/CesiumMan/glTF-Binary/CesiumMan.glb',bytes=await fetchBytes(url);
    const metadata=await fetchBytes(base+'Models/CesiumMan/metadata.json'),notice=await fetchBytes(base+'LICENSES/LicenseRef-LegalMark-Cesium.txt');
    await fs.writeFile(model,bytes);await fs.writeFile(path.join(directory,'metadata.json'),metadata);await fs.writeFile(path.join(directory,'Cesium-logo-notice.txt'),notice);
    await fs.writeFile(provenance,canonicalJson({title:'Cesium Man',creator:'Cesium',year:2017,license:'CC-BY-4.0',licenseUrl:'https://creativecommons.org/licenses/by/4.0/legalcode',url,commit,sha256:sha256Hex(bytes)})+'\n');
  }
  const source=JSON.parse(await fs.readFile(provenance,'utf8')),bytes=await fs.readFile(model);requireAsset(sha256Hex(bytes)===source.sha256,'Cached public source changed','HASH');
  const parent=await importAsset({source:model,output:path.join(directory,'imports'),size:256}),parsed=await readGltf(model),doc=structuredClone(parsed.document);
  delete doc.meshes;delete doc.skins;delete doc.materials;delete doc.images;delete doc.textures;delete doc.samplers;
  for(const node of doc.nodes){delete node.mesh;delete node.skin;delete node.weights;}
  doc.buffers=parsed.buffers.map(buffer=>({byteLength:buffer.length,uri:'data:application/octet-stream;base64,'+buffer.toString('base64')}));
  doc.animations=[doc.animations[0]];doc.animations[0].name='ExternalTake';
  const motion=path.join(directory,'CesiumMan-motion.gltf');await fs.writeFile(motion,canonicalJson(doc)+'\n');
  const appended=await appendAnimation({directory:parent.directory,source:motion,options:{prefix:'public_',mode:'exact'}});await verifyImport(appended.directory);
  const result={success:true,source,parent:parent.directory,directory:appended.directory,bones:appended.manifest.bones,clips:appended.manifest.clips,previewClip:appended.manifest.previewClip};
  await fs.writeFile(path.join(directory,'acceptance.json'),canonicalJson(result)+'\n');return result;
}
export async function verifyPublicCharacter(options={}) {
  const result=await preparePublicCharacter(options),directory=path.dirname(path.dirname(result.directory)),run=promisify(execFile);
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{windowsHide:true,timeout:60000,maxBuffer:32768});
  const exe=path.resolve('build/assets-host/NativeGPU.exe'),report=path.join(directory,'native.json'),replayReport=path.join(directory,'replay.json'),record=path.join(directory,'record.csv');
  const base=['--smoke','--no-stress','--asset',result.directory,'--frames','120','--animation-features','off','--audio','off'];
  await run(exe,[...base,'--screenshot-frame','30','--screenshot',path.join(directory,'native.png'),'--record',record,'--report',report],{windowsHide:true,timeout:120000,maxBuffer:32768});
  await run(exe,[...base,'--replay',record,'--replay-strict','--report',replayReport],{windowsHide:true,timeout:120000,maxBuffer:32768});
  const native=JSON.parse(await fs.readFile(report,'utf8')),replay=JSON.parse(await fs.readFile(replayReport,'utf8'));
  requireAsset(native.success&&replay.success&&native.animation.bones===result.bones&&native.animation.clipName===result.previewClip&&native.probes.skinning.draws>=120,'Public sample did not execute its appended animation');
  requireAsset(native.determinism.finalHash===replay.determinism.finalHash,'Public sample strict replay digest differs');
  const evidence={...result,native:{report,replayReport,screenshot:path.join(directory,'native.png'),draws:native.probes.skinning.draws,hash:native.determinism.finalHash,replayHash:replay.determinism.finalHash}};
  await fs.writeFile(path.join(directory,'acceptance.json'),canonicalJson(evidence)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
  const action=process.argv.includes('--native')?verifyPublicCharacter:preparePublicCharacter;
  action({download:process.argv.includes('--download')}).then(result=>console.log(JSON.stringify(result))).catch(error=>{console.error(error);process.exitCode=1;});
}

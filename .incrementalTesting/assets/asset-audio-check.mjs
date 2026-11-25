import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {randomUUID} from 'node:crypto';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import assert from 'node:assert/strict';
import {importAsset} from '../../engine/assets/import-service.mjs';
import {loadScene,buildScene} from '../../engine/scene/index.mjs';
import {writeAudioFixture} from '../../tools/offline/audio-fixture.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkAssetAudio(){
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  const output=path.join(root,'build/asset-import-smoke/audio');await fs.mkdir(output,{recursive:true});
  const asset=await importAsset({source:await writeAudioFixture(path.join(output,'tone.wav')),output:path.join(output,'imports')}),sceneFile=path.join(asset.directory,'preview.scene.json');
  const host=path.join(root,'build/assets-host'),compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'),object=name=>path.join(host,'objects',name+'.o');
  const objects=['engine_native_game.cpp','engine_native_physics.cpp','engine_native_animation.cpp','engine_native_animation_features.cpp','engine_native_animation_asset.cpp','engine_native_determinism.cpp','engine_native_assets_sha256.cpp','host_scene_loader.cpp','engine_audio_audio.cpp'].map(object);
  const test=path.join(host,'asset_audio_tests.exe');await run(compiler,['-std=c++17','-O2','-static','tests/native_asset_audio.cpp',...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const contracts=JSON.parse((await run(test,[sceneFile],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768})).stdout);
  const legacy=path.join(host,'audio_asset_regression.exe');await run(compiler,['-std=c++17','-O2','-static','tests/native_audio.cpp',object('engine_audio_audio.cpp'),object('engine_audio_audio_asset.cpp'),'-o',legacy],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const legacyOutput=(await run(legacy,[],{cwd:output,windowsHide:true,timeout:120000,maxBuffer:32768})).stdout;
  const exe=path.join(host,'NativeGPU.exe'),record=path.join(output,'record.csv'),base=['--smoke','--no-stress','--frames','240','--character',path.join(root,'build/animation/mannequin/character.json'),'--animation-features','off','--audio','offline','--audio-quality','spatial','--audio-layout','stereo','--camera','6,4,9,0,1,0'];
  const reports=[];for(const replay of [false,true]){
    const report=path.join(output,replay?'replay.json':'preview.json');await run(exe,[...base,'--asset',asset.directory,'--report',report,...(replay?['--replay',record,'--replay-strict']:['--record',record])],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});const data=JSON.parse(await fs.readFile(report));
    assert.equal(data.success,true);assert.equal(data.audio.error,'');assert.equal(data.audio.sceneSounds,1);assert.ok(data.audio.positionUpdates>100);assert.ok(data.audio.mixer.peakMax>0);reports.push(data);
  }
  assert.equal(reports[1].replay.mismatches,0);assert.equal(reports[0].audio.mixer.pcmHash,reports[1].audio.mixer.pcmHash);
  const packaged=await buildScene(sceneFile,path.join(output,'packages')),packageReport=path.join(output,'packaged.json');
  await run(exe,[...base,'--scene',path.join(packaged.packageDirectory,'scene.json'),'--report',packageReport],{cwd:packaged.packageDirectory,windowsHide:true,timeout:120000,maxBuffer:32768});const packageData=JSON.parse(await fs.readFile(packageReport));assert.equal(packageData.success,true);assert.equal(packageData.audio.error,'');assert.equal(packageData.audio.mixer.pcmHash,reports[0].audio.mixer.pcmHash);
  const broken=path.join(output,'damaged-'+randomUUID());await fs.cp(packaged.packageDirectory,broken,{recursive:true});const scene=await loadScene(path.join(broken,'scene.json'));await fs.appendFile(path.join(broken,Object.values(scene.audio.sounds)[0].file),'changed');
  const rejected=path.join(output,'rejected.json');await assert.rejects(run(exe,[...base,'--scene',path.join(broken,'scene.json'),'--report',rejected],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768}));const failure=JSON.parse(await fs.readFile(rejected));assert.equal(failure.success,false);assert.match(JSON.stringify(failure),/Audio dependency changed/);
  const second=await importAsset({source:await writeAudioFixture(path.join(output,'stereo.wav'),{channels:2}),output:path.join(output,'imports')}),hot=await loadScene(sceneFile),hotFile=path.join(output,'hot-'+randomUUID()+'.scene.json'),ready=path.join(output,'ready-'+randomUUID()+'.png'),hotReport=path.join(output,'hot.json');
  for(const ref of Object.values(hot.audio.sounds))ref.file=path.relative(output,path.join(asset.directory,ref.file)).split(path.sep).join('/');await fs.writeFile(hotFile,JSON.stringify(hot));
  let finished=false;const running=run(exe,[...base,'--frames','600','--watch','--scene',hotFile,'--screenshot-frame','1','--screenshot',ready,'--report',hotReport],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768}).finally(()=>{finished=true;});
  const deadline=Date.now()+30000;while(true){try{await fs.stat(ready);break;}catch(e){if(e.code!=='ENOENT')throw e;}if(finished||Date.now()>deadline){await running;throw new Error('Audio hot reload did not reach first frame');}await new Promise(resolve=>setTimeout(resolve,25));}
  const ref=second.manifest.sounds.sound;hot.audio.sounds={stereo:{file:path.relative(output,path.join(second.directory,ref.file)).split(path.sep).join('/'),sha256:ref.sha256}};hot.audio.emitters[0].sound='stereo';hot.audio.masterGainDb=-8;hot.revision++;const temporary=hotFile+'.next';await fs.writeFile(temporary,JSON.stringify(hot));await fs.rename(temporary,hotFile);await running;
  const hotData=JSON.parse(await fs.readFile(hotReport));assert.equal(hotData.success,true);assert.equal(hotData.audio.error,'');assert.ok(hotData.audio.sceneReloads>=2);assert.equal(hotData.audio.sceneSounds,1);assert.equal(hotData.audio.bank.sounds,1);assert.equal(hotData.audio.masterGainDb,-8);
  const evidence={success:true,contracts,legacyAudio:legacyOutput.trim().split(/\r?\n/).at(-1),preview:true,packaged:true,tamperRejected:true,strictReplay:true,bankHotReload:true,pcmHash:reports[0].audio.mixer.pcmHash,peakMax:reports[0].audio.mixer.peakMax};await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))checkAssetAudio().then(v=>console.log(JSON.stringify(v))).catch(e=>{console.error(e.stderr||e.stack);process.exitCode=1;});

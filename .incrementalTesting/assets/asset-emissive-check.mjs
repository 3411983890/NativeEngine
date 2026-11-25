import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {writeEmissiveFixture} from '../../tools/offline/emissive-fixture.mjs';
import {writeCharacterFixture} from '../../tools/asset-fixtures.mjs';
import {importAsset} from '../../engine/assets/import-service.mjs';
import {loadScene,buildScene} from '../../engine/scene/index.mjs';
import {sha256Hex} from '../../engine/assets/canonical.mjs';
import {prepareAssetHost} from '../../tools/asset-host-scope.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
export async function checkEmissionCharacter({context}={}){
  context??=await prepareAssetHost();await context.assertStable();
  const output=path.join(root,'build/asset-import-smoke/emissive'),source=await writeCharacterFixture(path.join(output,'character-source')),doc=JSON.parse(await fs.readFile(source));
  doc.materials.forEach((material,i)=>{material.pbrMetallicRoughness.baseColorFactor=[0,0,0,1];material.emissiveFactor=i?[0,0,.6]:[.6,0,0];material.extensions={KHR_materials_emissive_strength:{emissiveStrength:1}};});doc.extensionsRequired=['KHR_materials_emissive_strength'];await fs.writeFile(source,JSON.stringify(doc));
  const asset=await importAsset({source,output:path.join(output,'imports'),size:64}),report=path.join(output,'character.json'),screenshot=path.join(output,'character.png');
  await run(context.executable,['--smoke','--no-stress','--asset',asset.directory,'--frames','120','--render-size','640x480','--animation-features','off','--audio','off','--camera','0,3,6,0,1,0','--screenshot-frame','100','--screenshot',screenshot,'--report',report],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
  const data=JSON.parse(await fs.readFile(report)),pixels=await sharp(screenshot).removeAlpha().raw().toBuffer();assert.equal(data.success,true);assert.equal(data.probes.skinning.bindingsLastFrame,1);assert.ok(data.probes.skinning.draws>0);let red=0,blue=0;for(let p=0;p<pixels.length;p+=3){if(pixels[p]>pixels[p+1]+40&&pixels[p]>pixels[p+2]+40)red++;if(pixels[p+2]>pixels[p]+40&&pixels[p+2]>pixels[p+1]+40)blue++;}assert.ok(red>50&&blue>50,'both skinned material parts must show independent emission');
  await context.assertStable();const evidence={success:true,host:context.fingerprint,redPixels:red,bluePixels:blue,skinnedDraws:data.probes.skinning.draws};await fs.writeFile(path.join(output,'character-acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
export async function checkAssetEmission({publicContent=false,download=false,context}={}){
  context??=await prepareAssetHost();const host=context.directory;
  const output=path.join(root,'build/asset-import-smoke/emissive');await fs.mkdir(output,{recursive:true});const asset=await importAsset({source:await writeEmissiveFixture(path.join(output,'source')),output:path.join(output,'imports'),size:64}),sceneFile=path.join(asset.directory,'preview.scene.json'),scene=await loadScene(sceneFile);
  const variants=path.join(output,'variants-'+randomUUID());await fs.cp(asset.directory,variants,{recursive:true});
  const invalid=[];for(const changes of [{emissiveFactor:[1,2,0]},{emissiveStrength:-1},{emissiveStrength:1000001},{emissive:'../escape'},{emissiveFactor:[0,0]}]){const changed=structuredClone(scene);Object.assign(changed.materials.m1,changes);const file=path.join(variants,'invalid-'+invalid.length+'.json');await fs.writeFile(file,JSON.stringify(changed));invalid.push(file);}
  const compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'),objects=['engine_native_game.cpp','engine_native_physics.cpp','engine_native_animation.cpp','engine_native_animation_features.cpp','engine_native_animation_asset.cpp','engine_native_determinism.cpp','host_scene_loader.cpp','engine_native_assets_sha256.cpp'].map(n=>path.join(host,'objects',n+'.o')),test=path.join(host,'asset_emissive_tests.exe');
  const legacy=structuredClone(scene);for(const material of Object.values(legacy.materials)){delete material.emissive;delete material.emissiveFactor;delete material.emissiveStrength;}const legacyFile=path.join(variants,'legacy.scene.json');await fs.writeFile(legacyFile,JSON.stringify(legacy));
  await context.assertHeaders();await run(compiler,['-std=c++17','-O2','-static',await context.contractSource('tests/native_asset_emissive.cpp'),...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});await context.assertHeaders();const contracts=JSON.parse((await run(test,[sceneFile,legacyFile,...invalid],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768})).stdout);
  const exe=path.join(host,'NativeGPU.exe'),base=['--smoke','--no-stress','--frames','240','--render-size','640x480','--character',path.join(root,'build/animation/mannequin/character.json'),'--animation-features','off','--audio','off','--camera','0,2.5,16,0,1,0','--screenshot-frame','200'];
  const render=async(name,args,cwd=root)=>{await context.assertStable();const screenshot=path.join(output,name+'.png'),report=path.join(output,name+'.json');await run(exe,[...base,...args,'--report',report,'--screenshot',screenshot],{cwd,windowsHide:true,timeout:120000,maxBuffer:32768});const result=JSON.parse(await fs.readFile(report));assert.equal(result.success,true);assert.equal(result.capabilities.materialBindless,true);return {report:result,pixels:await sharp(screenshot).removeAlpha().raw().toBuffer()};};
  const record=path.join(output,'record.csv'),on=await render('on',['--asset',asset.directory,'--record',record]),dark=structuredClone(scene);for(const material of Object.values(dark.materials))if(material.emissive)material.emissiveStrength=0;const darkFile=path.join(variants,'dark.scene.json');await fs.writeFile(darkFile,JSON.stringify(dark));const off=await render('off',['--scene',darkFile,'--textures',path.join(variants,'textures')]);
  const counts=[0,0,0],columns=Array.from({length:640},()=>({pixels:0,sum:0}));for(let p=0;p<on.pixels.length;p+=3){const a=on.pixels.subarray(p,p+3),b=off.pixels.subarray(p,p+3);if(Math.max(...a.map((v,c)=>Math.abs(v-b[c])))<20)continue;const channel=a.indexOf(Math.max(...a));if(a[channel]>a[(channel+1)%3]+30&&a[channel]>a[(channel+2)%3]+30)counts[channel]++;const column=columns[(p/3)%640];column.pixels++;column.sum+=a[0]+a[1]+a[2];}assert.ok(counts.every(n=>n>200),'black base must still show independent red, green and blue emission');
  const groups=[];for(let x=0;x<640;x++){if(columns[x].pixels<5)continue;let group=groups.at(-1);if(!group||x>group.end+1){group={start:x,end:x,pixels:0,sum:0};groups.push(group);}group.end=x;group.pixels+=columns[x].pixels;group.sum+=columns[x].sum;}assert.equal(groups.length,5,'five separate emissive cards visible');
  // The +Z camera in the engine's left-handed view sees positive world X on
  // the left. Reverse screen order to compare the fixture's authored order.
  const brightness=groups.reverse().map(g=>g.sum/g.pixels/3);assert.ok(brightness[4]>brightness[3]*1.2,'strength multiplier must visibly brighten the same RGB color');
  const built=await buildScene(sceneFile,path.join(output,'packages')),packaged=await render('packaged',['--scene',path.join(built.packageDirectory,'scene.json'),'--textures',path.join(built.packageDirectory,'textures'),'--replay',record,'--replay-strict'],built.packageDirectory);assert.equal(packaged.report.replay.mismatches,0);assert.equal(on.report.determinism.finalHash,packaged.report.determinism.finalHash);assert.deepEqual(on.pixels,packaged.pixels);
  let publicEvidence=null;
  if(publicContent){
    const provenance=JSON.parse(await fs.readFile(path.join(root,'tests/fixtures/emissive-public.json'))),directory=path.join(root,'build/asset-emissive-fixtures/public');
    await fs.mkdir(directory,{recursive:true});
    for(const entry of provenance.files){
      const file=path.join(directory,entry.file);let bytes=await fs.readFile(file).catch(e=>{if(e.code!=='ENOENT')throw e;return null;});
      if(!bytes&&download){const response=await fetch(entry.url,{signal:AbortSignal.timeout(60000)});assert.ok(response.ok);bytes=Buffer.from(await response.arrayBuffer());assert.equal(sha256Hex(bytes),entry.sha256);assert.equal(bytes.length,entry.bytes);await fs.writeFile(file,bytes,{flag:'wx'});}
      assert.ok(bytes,'Public emission fixture missing; use --download once');assert.equal(sha256Hex(bytes),entry.sha256);assert.equal(bytes.length,entry.bytes);
    }
    const publicAsset=await importAsset({source:path.join(directory,'EmissiveStrengthTest.glb'),output:path.join(output,'imports'),size:64});
    const strengths=Object.values(publicAsset.manifest.materials).filter(m=>m.emissive).map(m=>m.emissiveStrength);assert.deepEqual([...new Set(strengths)].sort((a,b)=>a-b),[1,2,4,8,16]);
    const publicCamera=['--camera','0,8,16,0,0,0'];
    const publicOn=await render('public',['--asset',publicAsset.directory,...publicCamera]),publicDark=await loadScene(path.join(publicAsset.directory,'preview.scene.json')),publicWork=path.join(output,'public-variants-'+randomUUID());
    await fs.cp(publicAsset.directory,publicWork,{recursive:true});for(const m of Object.values(publicDark.materials))if(m.emissive)m.emissiveStrength=0;
    const publicDarkFile=path.join(publicWork,'dark.scene.json');await fs.writeFile(publicDarkFile,JSON.stringify(publicDark));
    const publicOff=await render('public-off',['--scene',publicDarkFile,'--textures',path.join(publicWork,'textures'),...publicCamera]);
    const columns=Array.from({length:640},()=>({pixels:0,sum:0}));
    for(let p=0;p<publicOn.pixels.length;p+=3){if(Math.max(...[0,1,2].map(c=>Math.abs(publicOn.pixels[p+c]-publicOff.pixels[p+c])))<20)continue;const column=columns[(p/3)%640];column.pixels++;column.sum+=publicOn.pixels[p]+publicOn.pixels[p+1]+publicOn.pixels[p+2];}
    const groups=[];for(let x=0;x<640;x++){if(columns[x].pixels<5)continue;let g=groups.at(-1);if(!g||x>g.end+1){g={end:x,pixels:0,sum:0};groups.push(g);}g.end=x;g.pixels+=columns[x].pixels;g.sum+=columns[x].sum;}
    assert.equal(groups.length,5,'five public emission cubes must be visible');
    const parts=publicAsset.manifest.parts.filter(p=>publicAsset.manifest.materials[p.material].emissive).sort((a,b)=>a.position[0]-b.position[0]);
    const measured=groups.reverse().map((g,i)=>({strength:publicAsset.manifest.materials[parts[i].material].emissiveStrength,brightness:g.sum/g.pixels/3})).sort((a,b)=>a.strength-b.strength);
    for(let i=1;i<measured.length;i++)assert.ok(measured[i].brightness>measured[i-1].brightness,'public emission cube brightness must follow strength');
    await fs.writeFile(path.join(output,'public-provenance.json'),JSON.stringify(provenance,null,2));await fs.copyFile(path.join(directory,'README.md'),path.join(output,'PUBLIC-LICENSE.md'));
    publicEvidence={model:provenance.model,importId:publicAsset.manifest.id,strengths,measured};
  }
  let legacyReplay=null;
  const oldReportFile=path.join(root,'build/asset-import-smoke/textures/packaged.json');
  const oldReport=await fs.readFile(oldReportFile,'utf8').then(JSON.parse).catch(e=>{if(e.code!=='ENOENT')throw e;return null;});
  if(oldReport){
    const oldRoot=path.dirname(oldReport.scene),oldRecord=path.join(output,'legacy-current.csv'),legacyArgs=['--scene',oldReport.scene,'--textures',path.join(oldRoot,'textures'),'--camera','5,4.5,7,0,1.5,0','--screenshot-frame','100'];
    // Replay headers deliberately bind a build fingerprint. Record the unchanged
    // old scene with this build; never rewrite or bypass a prior build's header.
    const currentLegacy=await render('legacy-record',[...legacyArgs,'--record',oldRecord],oldRoot);
    const result=await render('legacy',[...legacyArgs,'--replay',oldRecord,'--replay-strict'],oldRoot);
    assert.equal(result.report.replay.mismatches,0);assert.equal(result.report.determinism.finalHash,currentLegacy.report.determinism.finalHash);
    assert.deepEqual(result.pixels,currentLegacy.pixels,'Old packages must replay exactly with this build');
    // Render improvements can change historical screenshots from another build.
    // Keep that comparison visible, while recording/replay equality remains exact.
    const historical=await sharp(path.join(root,'build/asset-import-smoke/textures/packaged.png')).removeAlpha().raw().toBuffer();assert.equal(historical.length,result.pixels.length);let max=0,changed=0;for(let i=0;i<historical.length;i++){const delta=Math.abs(historical[i]-result.pixels[i]);max=Math.max(max,delta);changed+=delta?1:0;}legacyReplay={frames:240,identicalPixels:true,hash:result.report.determinism.finalHash,previousBuildHash:oldReport.determinism.finalHash,previousBuildPixels:{maxChannelDelta:max,changedChannels:changed}};
  }
  const character=await checkEmissionCharacter({context});await context.assertStable();
  const evidence={success:true,host:context.fingerprint,contracts,independentRgbPixels:counts,cardBrightness:brightness,character,packaged:true,identicalPackagePixels:true,strictReplayFrames:240,legacyReplay,public:publicEvidence};await fs.writeFile(path.join(output,publicContent?'public-acceptance.json':'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))(process.argv.includes('--character-only')?checkEmissionCharacter():checkAssetEmission({publicContent:process.argv.includes('--public'),download:process.argv.includes('--download')})).then(v=>console.log(JSON.stringify(v))).catch(e=>{console.error(e.stderr||e.stack);process.exitCode=1;});

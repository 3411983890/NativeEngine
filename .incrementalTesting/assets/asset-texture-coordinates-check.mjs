import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import {prepareAssetHost} from '../../tools/asset-host-scope.mjs';
import {writeMultiUvFixture} from '../../tools/offline/texture-coordinates-fixture.mjs';
import {importAsset} from '../../engine/assets/import-service.mjs';
import {readGltf,readAccessor} from '../../engine/assets/gltf-document.mjs';
import {loadScene,buildScene} from '../../engine/scene/index.mjs';
import {sha256Hex} from '../../engine/assets/canonical.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..'),run=promisify(execFile);
async function proveSkinUvDeformation(context,output){
  const results=[];await fs.mkdir(output,{recursive:true});
  for(const multi of [false,true]){
    const source=await writeMultiUvFixture(path.join(output,'skin-source-'+multi),{character:true,all:multi,baked:!multi}),asset=await importAsset({source,output:path.join(output,'imports'),size:128}),view=path.join(output,'skin-view-'+multi+'-'+randomUUID());await fs.cp(asset.directory,view,{recursive:true});
    const scenePath=path.join(view,'preview.scene.json'),scene=await loadScene(scenePath),player=scene.entities.find(e=>e.kind==='player');player.position[2]=0;await fs.writeFile(scenePath,JSON.stringify(scene));
    const pair=[];for(const shifted of [false,true]){
      if(shifted){const headerPath=path.join(view,'character/character.json'),header=JSON.parse(await fs.readFile(headerPath));for(const bone of header.bones)bone.inverseBind[12]+=.5;const bytes=Buffer.from(JSON.stringify(header));await fs.writeFile(headerPath,bytes);player.character.sha256=sha256Hex(bytes);await fs.writeFile(scenePath,JSON.stringify(scene));}
      const name='skin-'+multi+'-'+shifted,report=path.join(output,name+'.json'),image=path.join(output,name+'.png');await context.assertStable();
      await run(context.executable,['--smoke','--no-stress','--scene',scenePath,'--textures',path.join(view,'textures'),'--character',path.join(root,'build/animation/mannequin/character.json'),'--frames','120','--render-size','640x480','--animation-features','off','--audio','off','--camera','0,3,6,0,1,0','--screenshot-frame','100','--report',report,'--screenshot',image],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});
      const data=JSON.parse(await fs.readFile(report));assert.equal(data.success,true);assert.ok(data.probes.skinning.draws>0);pair.push({data,pixels:await sharp(image).removeAlpha().raw().toBuffer()});
    }
    const a=pair[0].data.animation.instances[0],b=pair[1].data.animation.instances[0];assert.deepEqual(a.position,b.position);assert.equal(a.geometry,b.geometry);assert.equal(a.clip,b.clip);assert.deepEqual(a.morphWeights,b.morphWeights);assert.notEqual(a.poseHash,b.poseHash); // Report hash includes the actual inverse-bind palette.
    let changed=0;for(let p=0;p<pair[0].pixels.length;p+=3)if(Math.max(...[0,1,2].map(k=>Math.abs(pair[0].pixels[p+k]-pair[1].pixels[p+k])))>=2)changed++;
    assert.ok(changed>1000,'Actual skin palette changes must move pixels for '+(multi?'multi-UV':'legacy')+' character');results.push({multiUv:multi,paletteChangedPixels:changed,skinnedDraws:pair[0].data.probes.skinning.draws});
  }
  await context.assertStable();const evidence={success:true,host:context.fingerprint,results};await fs.writeFile(path.join(output,'skinning.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
export async function checkSkinUvDeformation({context}={}){return proveSkinUvDeformation(context??await prepareAssetHost(),path.join(root,'build/asset-import-smoke/texture-coordinates'));}
export async function checkTextureCoordinates({context}={}){
  context??=await prepareAssetHost();await context.assertStable();const output=path.join(root,'build/asset-import-smoke/texture-coordinates');await fs.mkdir(output,{recursive:true});const imports=path.join(output,'imports');
  // Compile against the checked ABI before the longer image cases. Subsequent
  // workspace edits cannot mix new headers into this already linked test binary.
  const host=context.directory,compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'),objects=['engine_native_game.cpp','engine_native_physics.cpp','engine_native_animation.cpp','engine_native_animation_features.cpp','engine_native_animation_asset.cpp','engine_native_determinism.cpp','host_scene_loader.cpp','engine_native_assets_sha256.cpp'].map(n=>path.join(host,'objects',n+'.o')),test=path.join(host,'asset_texture_coordinates_tests.exe');
  await context.assertHeaders();await run(compiler,['-std=c++17','-O2','-static',await context.contractSource('tests/native_asset_texture_coordinates.cpp'),...objects,path.join(host,'gameplay/nativegpu-gameplay.a'),'-lbcrypt','-o',test],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});await context.assertHeaders();
  const base=['--smoke','--no-stress','--frames','120','--render-size','640x480','--character',path.join(root,'build/animation/mannequin/character.json'),'--animation-features','off','--audio','off','--screenshot-frame','100'];
  const render=async(name,args,character=false,cwd=root)=>{
    await context.assertStable();const report=path.join(output,name+'.json'),image=path.join(output,name+'.png');
    await run(context.executable,[...base,'--camera',character?'0,3,6,0,1,0':'0,1,5,0,1,0',...args,'--report',report,'--screenshot',image],{cwd,windowsHide:true,timeout:120000,maxBuffer:32768});
    const data=JSON.parse(await fs.readFile(report));assert.equal(data.success,true);if(character){assert.ok(data.probes.skinning.draws>0);assert.equal(data.probes.skinning.bindingsLastFrame,1);}return {data,pixels:await sharp(image).removeAlpha().raw().toBuffer()};
  };
  const preview=async(asset,name,character,semantic)=>{
    if(!character)return ['--asset',asset.directory];
    const view=path.join(output,'views',name);await fs.cp(asset.directory,view,{recursive:true});
    const file=path.join(view,'preview.scene.json'),scene=await loadScene(file);scene.entities.find(e=>e.kind==='player').position[2]=0;
    // Isolate sampling for slots other than normals. UV0 control changes also
    // affect the tangent basis of quantized, generated neutral normal textures.
    if(semantic!=='normal')scene.lighting={ambient:[.7,.7,.7],keyIntensity:0};
    await fs.writeFile(file,JSON.stringify(scene));return ['--scene',file,'--textures',path.join(view,'textures')];
  };
  const results=[];
  for(const character of [true,false])for(const semantic of ['albedo','normal','orm','emissive','occlusion']){
    const pair=[];for(const baked of [false,true]){const name=(character?'character-':'static-')+semantic+(baked?'-control':''),source=await writeMultiUvFixture(path.join(output,name),{character,semantic,baked}),asset=await importAsset({source,output:imports,size:128});pair.push(await render(name,await preview(asset,name,character,semantic),character));}
    let max=0,total=0;for(let p=0;p<pair[0].pixels.length;p++){const delta=Math.abs(pair[0].pixels[p]-pair[1].pixels[p]);max=Math.max(max,delta);total+=delta;}
    const result={character,semantic,maxChannelDelta:max,meanChannelDelta:total/pair[0].pixels.length,...(character?{skinnedDraws:pair[0].data.probes.skinning.draws}:{})};result.passed=max<=3&&result.meanChannelDelta<.15;results.push(result);
    await fs.writeFile(path.join(output,'render-progress.json'),JSON.stringify({host:context.fingerprint,results},null,2)+'\n');
  }
  const combined=[],expected={};
  for(const character of [false,true]){
    const source=await writeMultiUvFixture(path.join(output,'combined-'+character),{character,all:true}),asset=await importAsset({source,output:imports,size:128}),input=await readGltf(source),attributes=input.document.meshes[0].primitives[0].attributes;
    expected[character?'character':'static']=Array.from({length:5},(_,i)=>readAccessor(input,attributes[`TEXCOORD_${i+1}`],'VEC2'));combined.push(asset);
    await render('combined-'+character,await preview(asset,'combined-'+character,character,'normal'),character);
  }
  const expectedFile=path.join(output,'source-uvs.json');await fs.writeFile(expectedFile,JSON.stringify(expected));
  const work=path.join(output,'negative-'+randomUUID());await fs.cp(combined[0].directory,work,{recursive:true});const scene=await loadScene(path.join(work,'preview.scene.json')),invalid=[];
  const entity=scene.entities.find(e=>e.mesh),mesh=JSON.parse(await fs.readFile(path.join(work,entity.mesh.file)));
  for(const edit of [m=>m.vertices[0].pop(),m=>m.vertices[0][8]=1000001,m=>m.schema='nativegpu.mesh/3']){
    const bad=structuredClone(mesh);edit(bad);const bytes=Buffer.from(JSON.stringify(bad)),name='invalid-'+invalid.length+'.mesh.json';await fs.writeFile(path.join(work,name),bytes);
    const copy=structuredClone(scene);Object.assign(copy.entities.find(e=>e.mesh).mesh,{file:name,sha256:sha256Hex(bytes)});const file=path.join(work,'invalid-'+invalid.length+'.scene.json');await fs.writeFile(file,JSON.stringify(copy));invalid.push(file);
  }
  const legacySource=await writeMultiUvFixture(path.join(output,'legacy-character'),{character:true,baked:true}),legacy=await importAsset({source:legacySource,output:imports,size:128});
  const contracts=JSON.parse((await run(test,[path.join(combined[0].directory,'preview.scene.json'),path.join(combined[1].directory,'character/character.json'),path.join(legacy.directory,'character/character.json'),expectedFile,...invalid],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768})).stdout);
  const record=path.join(output,'record.csv'),viewArgs=await preview(combined[1],'record',true,'normal'),original=await render('record',[...viewArgs,'--frames','240','--screenshot-frame','200','--record',record],true),built=await buildScene(viewArgs[1],path.join(output,'packages')),packaged=await render('packaged',['--scene',path.join(built.packageDirectory,'scene.json'),'--textures',path.join(built.packageDirectory,'textures'),'--frames','240','--screenshot-frame','200','--replay',record,'--replay-strict'],true,built.packageDirectory);
  assert.equal(packaged.data.replay.mismatches,0);assert.deepEqual(original.pixels,packaged.pixels);assert.equal(original.data.determinism.finalHash,packaged.data.determinism.finalHash);await context.assertStable();
  assert.ok(results.every(result=>result.passed),'Source UV controls differ: '+JSON.stringify(results.filter(result=>!result.passed)));
  const skinDeformation=await proveSkinUvDeformation(context,output);
  const evidence={success:true,host:context.fingerprint,contracts,results,skinDeformation,combinedSlots:5,identicalPackagePixels:true,strictReplayFrames:240};await fs.writeFile(path.join(output,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');return evidence;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))(process.argv.includes('--skin-probe-only')?checkSkinUvDeformation():checkTextureCoordinates()).then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.stderr||error.stack);process.exitCode=1;});

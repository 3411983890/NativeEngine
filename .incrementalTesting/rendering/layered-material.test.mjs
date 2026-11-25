import test from 'node:test';
import assert from 'node:assert/strict';
import { LAYERED_SCHEMA, normalizeLayeredMaterial, compileLayeredMaterial, heightCoverage, evaluateLayerCoverage, blendLayerSamples, evaluateLayeredMaterial } from '../../engine/material/layered.mjs';
import { compileLayeredMaterialAssets } from '../../engine/material/layered-assets.mjs';
import { encodeLayerGpuRecords, LAYER_GPU_BYTES } from '../../engine/material/layered-gpu.mjs';
import { makeMips, readNgtex, writeNgtex, textureName, encodePng, decodeFirstMip } from '../../tools/textures.mjs';
import { importAsset, verifyImport } from '../../engine/assets/import-service.mjs';
import { existsSync, mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { validateScene, buildScene, undoSceneFile } from '../../engine/scene/index.mjs';
import sharp from 'sharp';
import { patchMaterial, applyMaterialPatch } from '../../tools/material.mjs';
import { sha256Hex } from '../../engine/assets/canonical.mjs';
import { fresnelSchlick, evaluateCoatClosure, evaluateParameterMix, physicalLayerIdentity } from '../../engine/material/physical-layers.mjs';

const root=path.resolve(import.meta.dirname,'../..');
const compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
const dxc=path.join(root,'.tools/dxc/1.9.2609/bin/x64/dxc.exe');

const make=()=>({schema:LAYERED_SCHEMA,name:'brick_moss',layers:[
  {id:'brick',textures:{albedo:{name:'brick'},normal:{name:'brick'},height:{name:'brick'}}},
  {id:'moss',uv:{set:2,mapping:{scale:[2,3]}},textures:{albedo:{name:'moss'},normal:{name:'moss',uv:{set:1,mapping:{rotation:.3}}},height:{name:'moss'}},blend:{mode:'height',source:'textureMask',texture:{name:'mask'},channel:'g',weight:.75,width:.2}},
]});
const hashes=()=>Object.fromEntries(['albedo/brick','normal/brick','height/brick','albedo/moss','normal/moss','height/moss','mask/mask'].map((key,i)=>[key,String(i+1).repeat(64)]));
const options=()=>({hashes:hashes(),shaderFingerprint:'a'.repeat(64),uvSets:3});
const sample=(v,normal=[0,0,1])=>({albedo:[v,v,v],normal,roughness:v,metallic:v,ao:v,emissive:[v,0,0],height:v});

// Native picture harness for the layered path. Ground geometry is a 12-unit built-in
// box, so a layer map spanning the whole surface uses scale 1/12 with a 0.5 offset.
const fit12=()=>({scale:[1/12,1/12],offset:[.5,.5]});
const solidSampler=v=>({width:1,height:1,data:Buffer.from([v,v,v,255])});
const halfSampler=(left,right)=>{const data=Buffer.alloc(8*8*4);for(let y=0;y<8;y++)for(let x=0;x<8;x++){const o=(y*8+x)*4,v=x<4?left:right;data[o]=data[o+1]=data[o+2]=v;data[o+3]=255;}return{width:8,height:8,data};};
function writeLayeredTexture(dir,name,semantic,image,srgb=false){mkdirSync(path.join(dir,'textures'),{recursive:true});writeFileSync(path.join(dir,'textures',`${name}.${semantic}.ngtex`),writeNgtex({mips:makeMips(image,semantic),semantic,srgb}));}
// v5 mesh: UV0 is constant, textureUvs[0] splits left/right and layerUvs[0] is
// constant white. Layer slots select these streams by index.
function writeUpperStreamQuadMesh(dir){
  const vertex=(x,z,textureU,layerU)=>[x,0,z,0,1,0,.25,.25,textureU,.5,.5,.5,.5,.5,.5,.5,1,1,1,1,0,0,0,0,layerU,.5,.5,.5,.5,.5];
  const mesh={schema:'nativegpu.mesh/5',vertices:[vertex(-.5,-.5,0,.9),vertex(.5,-.5,1,.9),vertex(.5,.5,1,.9),vertex(-.5,.5,0,.9)],indices:[0,2,1,0,3,2]};
  const bytes=Buffer.from(JSON.stringify(mesh)+'\n'),file='meshes/streams.mesh.json';
  mkdirSync(path.join(dir,'meshes'),{recursive:true});writeFileSync(path.join(dir,file),bytes);
  return{file,sha256:sha256Hex(bytes)};}
function layeredMeshScene(mesh,layered){const entity=(id,kind,position,scale,extra={})=>({id,kind,name:kind,position,scale,color:[1,1,1],collider:false,...extra});
  return{schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'uv streams',revision:0,
    settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},
    entities:[entity('a1e00000-0000-4000-8000-000000000002','player',[-20,0,0],[.6,1.8,.6]),
      entity('a1e00000-0000-4000-8000-000000000003','ground',[0,-.5,0],[30,.5,30]),
      entity('a1e00000-0000-4000-8000-000000000004','prop',[0,0,0],[4,1,4],{material:'test',mesh})],
    materials:{default:{albedo:'fallback'},test:{albedo:'fallback',layered}}};}
function layeredGroundScene(layered){const entity=(id,kind,position,scale)=>({id,kind,name:kind,position,scale,color:[1,1,1],collider:true});
  return{schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'layer picture',revision:0,
    settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},
    entities:[entity('a1e00000-0000-4000-8000-000000000002','player',[-20,0,0],[.6,1.8,.6]),{...entity('a1e00000-0000-4000-8000-000000000003','ground',[0,-.2,0],[12,.4,12]),material:'test'}],
    materials:{test:{albedo:'fallback',layered}}};}
function renderLayeredScene(dir,tag,scene,camera='0,10,0.001,0,0,0',texturesDir=path.join(dir,'textures'),extraArgs=[]){const file=path.join(dir,`scene-${tag}.json`),report=path.join(dir,`report-${tag}.json`),png=path.join(dir,`frame-${tag}.png`);
  writeFileSync(file,JSON.stringify(scene));
  execFileSync(process.env.NG_LAYER_HOST??path.join(root,'build/assets-host/NativeGPU.exe'),['--smoke','--no-stress','--scene',file,'--textures',texturesDir,'--frames','4','--render-size','320x240','--camera',camera,'--mesh-shaders','off',...extraArgs,'--screenshot-frame','1','--screenshot',png,'--report',report,'--audio','off'],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
  return{report:JSON.parse(readFileSync(report,'utf8').replace(/^\uFEFF/,'')),png};}
async function channelMeans(png){const{data,info}=await sharp(png).removeAlpha().raw().toBuffer({resolveWithObject:true});
  const mean=(x0,x1)=>{const sum=[0,0,0];let n=0;for(let y=90;y<150;y++)for(let x=x0;x<x1;x++){const o=(y*info.width+x)*3;for(let c=0;c<3;c++)sum[c]+=data[o+c];n++;}return sum.map(v=>Math.round(v/n));};
  return{left:mean(120,150),right:mean(170,200)};}
async function regionMean(png){const{data,info}=await sharp(png).removeAlpha().raw().toBuffer({resolveWithObject:true});
  const sum=[0,0,0];let n=0;for(let y=60;y<180;y++)for(let x=80;x<240;x++){const o=(y*info.width+x)*3;for(let c=0;c<3;c++)sum[c]+=data[o+c];n++;}
  return sum.map(v=>Math.round(v/n));}
async function differingPixels(a,b,threshold){const first=await sharp(a).removeAlpha().raw().toBuffer(),second=await sharp(b).removeAlpha().raw().toBuffer();
  let count=0;for(let i=0;i<first.length;i+=3){const delta=(Math.abs(first[i]-second[i])+Math.abs(first[i+1]-second[i+1])+Math.abs(first[i+2]-second[i+2]))/3;if(delta>threshold)count++;}return count;}
// Ambient ground scene for surface-map effects; the light variant gives the
// roughness pair a specular highlight to compare.
function layeredMapScene(layered,{light=false}={}){const entity=(id,kind,position,scale,extra={})=>({id,kind,name:kind,position,scale,color:[1,1,1],collider:kind!=='light',...extra});
  const entities=[entity('a1e00000-0000-4000-8000-000000000002','player',[-20,0,0],[.6,1.8,.6]),{...entity('a1e00000-0000-4000-8000-000000000003','ground',[0,-.2,0],[12,.4,12]),material:'test'}];
  if(light)entities.push({...entity('a1e00000-0000-4000-8000-000000000005','light',[0,2,-1],[1,1,1]),light:{color:[1,1,1],intensity:4,range:20}});
  return{schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'map pictures',revision:0,
    settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},entities,materials:{test:{albedo:'fallback',layered}}};}

test('scene validates normalized layer data and packages all unique texture dependencies',async()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-scene-'));
  try {
    mkdirSync(path.join(dir,'textures'));
    const entity=(id,kind)=>({id,kind,name:kind,position:[0,0,0],scale:[1,1,1],color:[1,1,1],collider:true});
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'layered',layers:[{id:'base',textures:{albedo:{name:'red'},height:{name:'h'}}},{id:'top',textures:{albedo:{name:'blue'}},blend:{source:'textureMask',texture:{name:'mask'}}}]});
    const scene={schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'Layer test',revision:0,settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},entities:[entity('a1e00000-0000-4000-8000-000000000002','player'),{...entity('a1e00000-0000-4000-8000-000000000003','ground'),material:'test'}],materials:{test:{albedo:'fallback',layered}}};
    const valid=validateScene(scene);assert.equal(valid.valid,true,JSON.stringify(valid.errors));
    const raw={schema:LAYERED_SCHEMA,name:'layered',layers:[{id:'base'},{id:'top',blend:{weight:.4}}]};
    const patched=patchMaterial(scene,'test',{layered:raw});assert.deepEqual(patched.scene.materials.test.layered,normalizeLayeredMaterial(raw));
    assert.equal(patched.scene.revision,1);assert.equal(patchMaterial(patched.scene,'test',{layered:null}).scene.materials.test.layered,undefined);
    const bad=structuredClone(scene);bad.materials.test.layered.layers[1].textures.albedo.uv.set=8;assert.match(validateScene(bad).errors.join(' '),/UV sets 0-7/);
    const streamed=structuredClone(scene);streamed.materials.test.layered.layers[1].textures.albedo.uv.set=1;assert.equal(validateScene(streamed).valid,true,JSON.stringify(validateScene(streamed).errors));
    const image={width:1,height:1,data:Buffer.from([64,64,64,255])};
    for(const [name,semantic] of [['fallback','albedo'],['fallback','normal'],['fallback','orm'],['red','albedo'],['blue','albedo'],['h','height'],['mask','mask']])writeFileSync(path.join(dir,'textures',`${name}.${semantic}.ngtex`),writeNgtex({mips:makeMips(image,semantic),semantic,srgb:semantic==='albedo'}));
    const file=path.join(dir,'scene.json');writeFileSync(file,JSON.stringify(scene));
    await applyMaterialPatch({scenePath:file,material:'test',changes:{layered:raw},root:dir,expectedRevision:0});
    assert.equal(JSON.parse(readFileSync(file)).revision,1);
    await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1,assetsDirectory:path.join(root,'assets')});
    const undone=JSON.parse(readFileSync(file));assert.deepEqual(undone.materials.test.layered,layered);
    const missing=structuredClone(raw);missing.layers[1].textures={height:{name:'missing'}};
    await assert.rejects(applyMaterialPatch({scenePath:file,material:'test',changes:{layered:missing},root:dir,expectedRevision:undone.revision}),/not cooked/);
    const result=await buildScene(file,path.join(dir,'packages'),{assetsDirectory:path.join(root,'assets')});
    const manifest=JSON.parse(readFileSync(path.join(result.packageDirectory,'manifest.json')));
    for(const name of ['red.albedo','blue.albedo','h.height','mask.mask'])assert.ok(manifest.dependencies.some(v=>v.path===`textures/${name}.ngtex`));
    const saved=JSON.parse(readFileSync(path.join(result.packageDirectory,'scene.json')));assert.deepEqual(saved.materials.test.layered,layered);
    const maskFile=path.join(dir,'textures','mask.mask.ngtex');
    writeFileSync(maskFile,writeNgtex({mips:makeMips(image,'mask'),semantic:'mask',srgb:true}));
    await assert.rejects(buildScene(file,path.join(dir,'packages'),{assetsDirectory:path.join(root,'assets')}),/semantic or color space mismatch/);
    writeFileSync(maskFile,writeNgtex({mips:makeMips(image,'height'),semantic:'height'}));
    await assert.rejects(buildScene(file,path.join(dir,'packages'),{assetsDirectory:path.join(root,'assets')}),/semantic or color space mismatch/);
    writeFileSync(maskFile,Buffer.concat([writeNgtex({mips:makeMips(image,'mask'),semantic:'mask'}),Buffer.from([1])]));
    await assert.rejects(buildScene(file,path.join(dir,'packages'),{assetsDirectory:path.join(root,'assets')}),/invalid dimensions, mip chain or payload/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('layered scene draws different coverage on the native renderer', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-scene-'));
  try {
    mkdirSync(path.join(dir,'textures'));const image={width:1,height:1,data:Buffer.from([255,255,255,255])};
    for(const semantic of ['albedo','normal','orm'])writeFileSync(path.join(dir,'textures',`fallback.${semantic}.ngtex`),writeNgtex({mips:makeMips(image,semantic),semantic,srgb:semantic==='albedo'}));
    const entity=(id,kind,position,scale)=>({id,kind,name:kind,position,scale,color:[1,1,1],collider:true});
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'layered',layers:[{id:'red',baseColor:[1,.03,.03]},{id:'blue',baseColor:[.03,.03,1],blend:{weight:0}}]});
    const scene={schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'Layer draw',revision:0,settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},entities:[entity('a1e00000-0000-4000-8000-000000000002','player',[-5,0,0],[.6,1.8,.6]),{...entity('a1e00000-0000-4000-8000-000000000003','ground',[0,-.2,0],[12,.4,12])},{...entity('a1e00000-0000-4000-8000-000000000004','prop',[0,1,0],[2,2,2]),material:'test'}],materials:{test:{albedo:'fallback',layered}}};
    const file=path.join(dir,'scene.json'),exe=process.env.NG_LAYER_HOST??path.join(root,'build/assets-host/NativeGPU.exe'),images=[];
    for(const weight of [0,1]){
      scene.materials.test.layered.layers[1].blend.weight=weight;writeFileSync(file,JSON.stringify(scene));
      const report=path.join(dir,`report${weight}.json`),png=path.join(dir,`frame${weight}.png`);
      execFileSync(exe,['--smoke','--no-stress','--scene',file,'--textures',path.join(dir,'textures'),'--frames','4','--render-size','320x240','--camera','4,3,6,0,1,0','--mesh-shaders','off','--screenshot-frame','1','--screenshot',png,'--report',report,'--audio','off'],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
      assert.equal(JSON.parse(readFileSync(report,'utf8').replace(/^\uFEFF/,'' )).success,true);
      images.push(await sharp(png).removeAlpha().raw().toBuffer());
    }
    let changed=0,redToBlue=0;for(let i=0;i<images[0].length;i+=3){if(Math.abs(images[0][i]-images[1][i])+Math.abs(images[0][i+2]-images[1][i+2])>20)changed++;if(images[0][i]>images[0][i+2]*1.3&&images[1][i+2]>images[1][i]*1.3)redToBlue++;}
    assert.ok(changed>200&&redToBlue>100,`expected visible red-to-blue coverage change: changed=${changed}, redToBlue=${redToBlue}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

test('texture mask blends per pixel on the native renderer', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-mask-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'split','mask',halfSampler(0,255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'mask',layers:[
      {id:'red',baseColor:[1,.03,.03]},
      {id:'blue',baseColor:[.03,.03,1],blend:{source:'textureMask',texture:{name:'split',uv:{mapping:fit12()}},channel:'r',weight:1}},
    ]});
    const {report,png}=renderLayeredScene(dir,'mask',layeredGroundScene(layered));assert.equal(report.success,true);
    assert.equal(report.capabilities.layeredMaterials.activeLayers,2);assert.equal(report.capabilities.layeredMaterials.textureSamples,1);
    assert.equal(report.capabilities.layeredMaterials.uniqueTextures,1);assert.ok(report.capabilities.layeredMaterials.textureBytes>0);
    assert.equal(report.capabilities.layeredMaterials.maxLayers,4);
    const {left,right}=await channelMeans(png);
    assert.ok(left[2]>left[0]+30,`masked half must show the overlay, not the base: ${left}`);
    assert.ok(right[0]>right[2]+30,`unmasked half must show the base: ${right}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('height mode steers per-pixel coverage on the native renderer', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-height-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'low','height',halfSampler(0,255));
    writeLayeredTexture(dir,'high','height',halfSampler(255,0));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'height',layers:[
      {id:'red',baseColor:[1,.03,.03],textures:{height:{name:'low',uv:{mapping:fit12()}}}},
      {id:'blue',baseColor:[.03,.03,1],textures:{height:{name:'high',uv:{mapping:fit12()}}},blend:{mode:'height',source:'constant',weight:.5,width:1}},
    ]});
    const {report,png}=renderLayeredScene(dir,'height',layeredGroundScene(layered));assert.equal(report.success,true);
    const {left,right}=await channelMeans(png);
    assert.ok(left[0]>left[2]+30,`top height below the base must leave the base visible: ${left}`);
    assert.ok(right[2]>right[0]+30,`top height above the base must expose the overlay: ${right}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('four layers composite in stack order and skip zero weights', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-four-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'split','mask',halfSampler(0,255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'four',layers:[
      {id:'base',baseColor:[1,.03,.03]},
      {id:'green',baseColor:[.03,1,.03],blend:{source:'constant',weight:1}},
      {id:'grey',baseColor:[.5,.5,.5],blend:{source:'constant',weight:0}},
      {id:'yellow',baseColor:[1,1,.03],blend:{source:'textureMask',texture:{name:'split',uv:{mapping:fit12()}},channel:'r',weight:1}},
    ]});
    const {report,png}=renderLayeredScene(dir,'four',layeredGroundScene(layered));assert.equal(report.success,true);
    const {left,right}=await channelMeans(png);
    assert.ok(left[0]>right[0]+30&&left[0]>60&&left[1]>60,`top layer must replace the green overlay: ${left} ${right}`);
    assert.ok(right[1]>right[0]+30,`zero-weight layer must not contribute and green must show: ${right}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('independent layer maps drive the native surface', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-maps-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'ao-black','occlusion',solidSampler(0));
    writeLayeredTexture(dir,'ao-split','occlusion',solidSampler(255));
    writeLayeredTexture(dir,'emissive-white','emissive',solidSampler(255),true);
    writeLayeredTexture(dir,'metal-white','metallic',solidSampler(255));
    writeLayeredTexture(dir,'metal-split','metallic',solidSampler(64));
    writeLayeredTexture(dir,'rough-black','roughness',solidSampler(0));
    writeLayeredTexture(dir,'rough-white','roughness',solidSampler(255));
    writeLayeredTexture(dir,'rough-split','roughness',solidSampler(128));
    writeLayeredTexture(dir,'normal-tilt','normal',{width:1,height:1,data:Buffer.from([255,128,180,255])});
    writeLayeredTexture(dir,'orm-packed','orm',{width:1,height:1,data:Buffer.from([255,128,64,255])});
    const make=layer=>normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'maps',layers:[{id:'base',baseColor:[.9,.5,.3],...layer}]});
    const render=async(tag,layer,{light=false,camera='0,10,0.001,0,0,0'}={})=>{const{report,png}=renderLayeredScene(dir,tag,layeredMapScene(make(layer),{light}),camera);assert.equal(report.success,true);return png;};
    const plain=await regionMean(await render('plain',{}));
    const emissive=await regionMean(await render('emissive',{textures:{emissive:{name:'emissive-white'}},emissiveFactor:[0,0,1],emissiveStrength:2}));
    assert.ok(emissive[2]>plain[2]+100&&emissive[2]>200,`emissive map must add light: ${emissive} vs ${plain}`);
    const ao=await regionMean(await render('ao',{textures:{occlusion:{name:'ao-black'}},occlusionStrength:1}));
    assert.ok(ao[0]<plain[0]-30,`black occlusion map must darken: ${ao} vs ${plain}`);
    const metal=await regionMean(await render('metal',{textures:{metallic:{name:'metal-white'}},metallic:1}));
    assert.ok(metal[0]<plain[0]-50,`full metallic must remove diffuse: ${metal} vs ${plain}`);
    const normal=await regionMean(await render('normal',{textures:{normal:{name:'normal-tilt'}}}));
    assert.ok(normal[0]<plain[0]-30,`tilted normal must change lighting: ${normal} vs ${plain}`);
    const rough0=await render('rough0',{baseColor:[.03,.03,.03],textures:{roughness:{name:'rough-black'}}},{light:true,camera:'0,4,4,0,0,0'});
    const rough1=await render('rough1',{baseColor:[.03,.03,.03],textures:{roughness:{name:'rough-white'}}},{light:true,camera:'0,4,4,0,0,0'});
    assert.ok(await differingPixels(rough0,rough1,30)>150,'roughness must change the specular highlight');
    const packed=await regionMean(await render('orm-packed',{textures:{orm:{name:'orm-packed'}},metallic:1}));
    const split=await regionMean(await render('orm-split',{textures:{occlusion:{name:'ao-split'},roughness:{name:'rough-split'},metallic:{name:'metal-split'}},metallic:1}));
    assert.ok(packed.reduce((sum,v,i)=>sum+Math.abs(v-split[i]),0)/3<=4,`packed ORM must match independent maps: ${packed} vs ${split}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('normal mapping follows mirrored UVs and falls back on degenerate derivatives', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-normal-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    // A uniformly +X-tilted normal map: mirroring the UVs must keep the
    // world-space tilt direction, so the mirrored render matches the upright one.
    writeLayeredTexture(dir,'tilt','normal',{width:1,height:1,data:Buffer.from([255,128,180,255])});
    const layer=scale=>normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'normals',layers:[{id:'base',baseColor:[.9,.5,.3],
      ...(scale?{textures:{normal:{name:'tilt',uv:{mapping:{scale,offset:[.5,.5]}}}}}:{})}]});
    const render=async(tag,scale)=>{const{report,png}=renderLayeredScene(dir,tag,layeredMapScene(layer(scale)));assert.equal(report.success,true);return png;};
    const upright=await render('upright',[1/12,1/12]),mirrorPng=await render('mirrored',[-1/12,1/12]);
    const uprightMean=await regionMean(upright),flatMean=await regionMean(await render('flat',null));
    assert.ok(flatMean[0]>uprightMean[0]+30,`the tilted normal must darken the lit ground: ${uprightMean} vs ${flatMean}`);
    const mirroredMean=await regionMean(mirrorPng);
    assert.ok(mirroredMean[0]>uprightMean[0]+40,`mirrored UVs must flip the tangent basis, so the +X tilt becomes -X: ${uprightMean} vs ${mirroredMean}`);
    const degenerateMean=await regionMean(await render('degenerate',[0,0]));
    assert.ok(flatMean.every((v,i)=>Math.abs(v-degenerateMean[i])<=6),`degenerate UVs must fall back to the geometric normal: ${flatMean} vs ${degenerateMean}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('normal mips keep unit length after filtering',()=>{
  const data=Buffer.alloc(4*4*4);for(let y=0;y<4;y++)for(let x=0;x<4;x++){const o=(y*4+x)*4;const nx=x%2?0.6:-0.6,nz=0.8;
    data[o]=Math.round((nx*0.5+0.5)*255);data[o+1]=128;data[o+2]=Math.round((nz*0.5+0.5)*255);data[o+3]=255;}
  const mips=makeMips({width:4,height:4,data},'normal');
  assert.ok(mips.length>=3);
  for(const mip of mips)for(let i=0;i<mip.data.length;i+=4){
    const nx=mip.data[i]/255*2-1,ny=mip.data[i+1]/255*2-1,nz=mip.data[i+2]/255*2-1;
    assert.ok(Math.abs(Math.hypot(nx,ny,nz)-1)<=0.03,`normal mip ${mip.width}x${mip.height} is not renormalized`);
  }
});
test('layers sharing one texture upload it once', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-dedup-native-'));
  try {
    writeLayeredTexture(dir,'shared','albedo',solidSampler(180),true);
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'dedup',layers:[
      {id:'base',textures:{albedo:{name:'shared'}}},
      {id:'top',textures:{albedo:{name:'shared'}},blend:{source:'constant',weight:.4}},
    ]});
    const {report}=renderLayeredScene(dir,'dedup',layeredGroundScene(layered));assert.equal(report.success,true);
    assert.equal(report.capabilities.layeredMaterials.textureSamples,2);
    assert.equal(report.capabilities.layeredMaterials.uniqueTextures,1);
    assert.ok(report.capabilities.layeredMaterials.textureBytes>0);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('inactive layers are eliminated from the GPU table but stay authored', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-inactive-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'inactive',layers:[
      {id:'base',baseColor:[1,.03,.03]},
      {id:'off',enabled:false,baseColor:[.03,1,.03],textures:{albedo:{name:'fallback'}}},
      {id:'zero',baseColor:[.03,.03,1],blend:{source:'constant',weight:0}},
    ]});
    const {report,png}=renderLayeredScene(dir,'inactive',layeredGroundScene(layered));assert.equal(report.success,true);
    assert.equal(report.capabilities.layeredMaterials.authoredLayers,3);assert.equal(report.capabilities.layeredMaterials.activeLayers,1);
    const {left,right}=await channelMeans(png);
    assert.ok(left[0]>left[2]+30&&right[0]>right[2]+30,'fully covered base must ignore inactive overlays');
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('preview debug views expose coverage, height and layer albedo', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-debug-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'split','mask',halfSampler(0,255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'debug',layers:[
      {id:'red',baseColor:[1,.03,.03]},
      {id:'blue',baseColor:[.03,.03,1],blend:{source:'textureMask',texture:{name:'split',uv:{mapping:fit12()}},channel:'r',weight:1}},
    ]});
    const scene=layeredGroundScene(layered);
    const grab=async(tag,args)=>{const{report,png}=renderLayeredScene(dir,tag,scene,'0,10,0.001,0,0,0',path.join(dir,'textures'),args);assert.equal(report.success,true);return channelMeans(png);};
    const coverage=await grab('dbg-mask',['--layer-debug','mask:1']);
    assert.ok(coverage.left[0]>200&&coverage.right[0]<60,`mask debug must be black/white: ${coverage.left} ${coverage.right}`);
    const albedo=await grab('dbg-albedo',['--layer-debug','albedo:1']);
    assert.ok(albedo.left[2]>albedo.left[0]+50,`layer albedo debug must show the selected layer: ${albedo.left}`);
    const height=await grab('dbg-height',['--layer-debug','height']);
    for(const region of [height.left,height.right]){assert.ok(region[0]>100&&Math.abs(region[0]-region[2])<12,`height debug must be grayscale: ${region}`);}
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('layer slots sample the selected mesh UV streams', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-uvstream-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'split','mask',halfSampler(0,255));
    const mesh=writeUpperStreamQuadMesh(dir);
    const make=set=>normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'streams',layers:[
      {id:'red',baseColor:[1,.03,.03]},
      {id:'blue',baseColor:[.03,.03,1],blend:{source:'textureMask',channel:'r',weight:1,texture:{name:'split',uv:{set}}}},
    ]});
    const render=async(set,tag)=>{const{report,png}=renderLayeredScene(dir,tag,layeredMeshScene(mesh,make(set)),'0,6,0.001,0,0,0');assert.equal(report.success,true);return channelMeans(png);};
    const uv0=await render(0,'set0');
    assert.ok(uv0.left[0]>uv0.left[2]+30&&uv0.right[0]>uv0.right[2]+30,`constant UV0 must sample one mask texel: ${uv0.left} ${uv0.right}`);
    const uv1=await render(1,'set1');
    assert.ok(uv1.left[2]>uv1.left[0]+30&&uv1.right[0]>uv1.right[2]+30,`texture UV stream 1 must split coverage: ${uv1.left} ${uv1.right}`);
    const uv5=await render(5,'set5');
    assert.ok(uv5.left[2]>uv5.left[0]+30&&uv5.right[2]>uv5.right[0]+30,`layer UV stream must select its own texels: ${uv5.left} ${uv5.right}`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('native loader rejects layered UV sets it cannot bind instead of falling back to UV0', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-uvset-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'uvset',layers:[{id:'base',baseColor:[1,.03,.03],textures:{albedo:{name:'fallback',uv:{set:1}}}}]});
    let failed=false;
    try{renderLayeredScene(dir,'uvset',layeredGroundScene(layered));}catch{failed=true;}
    assert.equal(failed,true,'the built-in ground carries no extra UV streams, so the draw must be rejected');
    assert.match(JSON.parse(readFileSync(path.join(dir,'report-uvset.json'),'utf8').replace(/^\uFEFF/,'')).error,/selects mesh UV streams this draw does not carry/);
    const outOfRange=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'uvrange',layers:[{id:'base',baseColor:[1,.03,.03],textures:{albedo:{name:'fallback',uv:{set:8}}}}]});
    failed=false;
    try{renderLayeredScene(dir,'uvrange',layeredGroundScene(outOfRange));}catch{failed=true;}
    assert.equal(failed,true,'UV sets outside [0, 7] must be rejected while loading');
    assert.match(JSON.parse(readFileSync(path.join(dir,'report-uvrange.json'),'utf8').replace(/^\uFEFF/,'')).error,/UV set must be in \[0, 7\]/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

function layeredCharacterScene(dir,layered){const header=readFileSync(path.join(root,'build/animation/mannequin/character.json')),binary=readFileSync(path.join(root,'build/animation/mannequin/animation.bin'));
  writeFileSync(path.join(dir,'character.json'),header);writeFileSync(path.join(dir,'animation.bin'),binary);
  const entity=(id,kind,position,scale,extra={})=>({id,kind,name:kind,position,scale,color:[1,1,1],collider:kind!=='light',...extra});
  return{schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'skinned layers',revision:0,
    settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},
    entities:[entity('a1e00000-0000-4000-8000-000000000002','player',[0,0,0],[1,1,1], {character:{file:'character.json',sha256:sha256Hex(header),binarySha256:sha256Hex(binary),anchor:'feet'},material:'default'}),
      entity('a1e00000-0000-4000-8000-000000000003','ground',[0,-.05,0],[20,.1,20],{material:'flat'})],
    materials:{default:{albedo:'fallback',layered},flat:{albedo:'fallback'}}};}
test('skinned characters evaluate the layered material', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-skin-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    const make=weight=>normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'skin',layers:[
      {id:'red',baseColor:[1,.03,.03]},
      {id:'blue',baseColor:[.03,.03,1],blend:{source:'constant',weight}},
    ]});
    const render=async(tag,weight)=>{const{report,png}=renderLayeredScene(dir,tag,layeredCharacterScene(dir,make(weight)),'0,1.2,3,0,0.9,0');assert.equal(report.success,true);return sharp(png).removeAlpha().raw().toBuffer();};
    const base=await render('skin0',0),covered=await render('skin1',1);
    const width=320;let red=0,blue=0,flipped=0;
    for(let y=40;y<200;y++)for(let x=110;x<210;x++){const o=(y*width+x)*3;if(base[o]>base[o+2]*1.4)red++;if(covered[o+2]>covered[o]*1.4)blue++;if(base[o]>base[o+2]*1.4&&covered[o+2]>covered[o]*1.4)flipped++;}
    assert.ok(red>100,`the skinned character must show the base layer: ${red} red pixels`);
    assert.ok(flipped>100,`the skinned character must evaluate the overlay layer: ${flipped} flipped, ${blue} blue`);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('packaged builds render the same layered surface as the source scene', {skip:process.env.NG_LAYER_SCENE_TEST!=='1'||process.platform!=='win32'},async()=>{
  const dir=mkdtempSync(path.join(root,'build','layered-package-native-'));
  try {
    writeLayeredTexture(dir,'fallback','albedo',solidSampler(255),true);
    writeLayeredTexture(dir,'fallback','normal',solidSampler(128));
    writeLayeredTexture(dir,'fallback','orm',solidSampler(255));
    writeLayeredTexture(dir,'split','mask',halfSampler(0,255));
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'mask',layers:[
      {id:'red',baseColor:[1,.03,.03]},
      {id:'blue',baseColor:[.03,.03,1],blend:{source:'textureMask',texture:{name:'split',uv:{mapping:fit12()}},channel:'r',weight:1}},
    ]});
    const scene=layeredGroundScene(layered),file=path.join(dir,'scene.json');
    writeFileSync(file,JSON.stringify(scene));
    const built=await buildScene(file,path.join(dir,'packages'),{assetsDirectory:path.join(root,'assets')});
    const source=await renderLayeredScene(dir,'source',scene);
    const packaged=await renderLayeredScene(dir,'packaged',structuredClone(scene),'0,10,0.001,0,0,0',path.join(built.packageDirectory,'textures'));
    assert.equal(packaged.report.success,true);
    assert.ok(await differingPixels(source.png,packaged.png,16)<200,'the packaged build must render the same layered coverage');
    const regions=await channelMeans(packaged.png);
    assert.ok(regions.left[2]>regions.left[0]+30&&regions.right[0]>regions.right[2]+30,`packaged coverage must remain per-pixel: ${regions.left} ${regions.right}`);
    assert.deepEqual(JSON.parse(readFileSync(path.join(built.packageDirectory,'scene.json'))).materials.test.layered,layered);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('layer stack edits round-trip through scene saves and undo',async()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-stack-'));
  try {
    mkdirSync(path.join(dir,'textures'));
    const entity=(id,kind)=>({id,kind,name:kind,position:[0,0,0],scale:[1,1,1],color:[1,1,1],collider:true});
    const layer=(id,color,extra={})=>({id,baseColor:color,...extra});
    const layered=normalizeLayeredMaterial({schema:LAYERED_SCHEMA,name:'stack',layers:[layer('base',[1,.1,.1]),layer('moss',[.1,.6,.1],{blend:{source:'constant',weight:.3}})]});
    const scene={schemaVersion:2,projectId:'a1e00000-0000-4000-8000-000000000001',name:'Layer stack',revision:0,settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},entities:[entity('a1e00000-0000-4000-8000-000000000002','player'),{...entity('a1e00000-0000-4000-8000-000000000003','ground'),material:'test'}],materials:{test:{albedo:'fallback',layered}}};
    const file=path.join(dir,'scene.json');writeFileSync(file,JSON.stringify(scene));
    const grow=structuredClone(layered);grow.layers.push(layer('rust',[.5,.25,.1],{blend:{source:'constant',weight:.2}}));
    await applyMaterialPatch({scenePath:file,material:'test',changes:{layered:grow},root:dir,expectedRevision:0});
    let saved=JSON.parse(readFileSync(file));assert.deepEqual(saved.materials.test.layered.layers.map(v=>v.id),['base','moss','rust']);
    const reorder=structuredClone(saved.materials.test.layered);[reorder.layers[1],reorder.layers[2]]=[reorder.layers[2],reorder.layers[1]];
    await applyMaterialPatch({scenePath:file,material:'test',changes:{layered:reorder},root:dir,expectedRevision:1});
    saved=JSON.parse(readFileSync(file));assert.deepEqual(saved.materials.test.layered.layers.map(v=>v.id),['base','rust','moss']);
    const disable=structuredClone(saved.materials.test.layered);disable.layers[1].enabled=false;
    await applyMaterialPatch({scenePath:file,material:'test',changes:{layered:disable},root:dir,expectedRevision:2});
    saved=JSON.parse(readFileSync(file));assert.equal(saved.materials.test.layered.layers[1].enabled,false);
    const promote=structuredClone(saved.materials.test.layered);[promote.layers[0],promote.layers[1]]=[promote.layers[1],promote.layers[0]];
    await assert.rejects(applyMaterialPatch({scenePath:file,material:'test',changes:{layered:promote},root:dir,expectedRevision:3}),/base layer/);
    await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:3,assetsDirectory:path.join(root,'assets')});
    saved=JSON.parse(readFileSync(file));assert.deepEqual(saved.materials.test.layered.layers.map(v=>v.id),['base','rust','moss']);assert.equal(saved.materials.test.layered.layers[1].enabled,true);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('GPU layer records preserve slot descriptors, mappings, channels and blend settings',()=>{
  const value=make(),textures=Object.fromEntries(Object.keys(hashes()).map((key,i)=>[key,{index:i+10,bc5:key==='normal/moss'}]));
  const {buffer,layerCount}=encodeLayerGpuRecords(value,{textures,uvSets:3});
  assert.equal(layerCount,2);assert.equal(buffer.length,2*LAYER_GPU_BYTES);
  assert.equal(buffer.readUInt32LE(12),10);assert.equal(buffer.readUInt32LE(3*48+12),0xffffffff);
  const top=LAYER_GPU_BYTES;assert.equal(buffer.readUInt32LE(top+32),2);
  assert.equal(buffer.readUInt32LE(top+48+32),1);assert.equal(buffer.readUInt32LE(top+48+40),1);
  assert.equal(buffer.readUInt32LE(top+8*48+36),1);assert.equal(buffer.readUInt32LE(top+504),1);assert.equal(buffer.readUInt32LE(top+508),2);
  assert.equal(buffer.readFloatLE(top+484),.75);assert.equal(buffer.readFloatLE(top),2);assert.equal(buffer.readFloatLE(top+20),3);
  const noNormal={...textures};delete noNormal['normal/moss'];assert.throws(()=>encodeLayerGpuRecords(value,{textures:noNormal,uvSets:3}),/missing GPU descriptor/);
  assert.throws(()=>encodeLayerGpuRecords(value,{textures,uvSets:2}),/UV set 2 is unavailable/);
  value.layers[1].blend={source:'vertexColor',channel:'b'};const vertex=encodeLayerGpuRecords(value,{textures,uvSets:3});assert.equal(vertex.buffer.readUInt32LE(top+8*48+36),2);
});
test('native normalized layer conversion agrees with GPU encoding', {skip:process.platform!=='win32'||!existsSync(compiler)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-asset-native-'));
  try {
    const material=normalizeLayeredMaterial(make()),file=path.join(dir,'material.json'),exe=path.join(dir,'asset.exe');writeFileSync(file,JSON.stringify(material));
    execFileSync(compiler,['-std=c++17','-O2','-static','.incrementalTesting/rendering/layered-asset-native.cpp','-o',exe],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
    const native=JSON.parse(execFileSync(exe,[file],{encoding:'utf8',windowsHide:true,timeout:30000}));
    const textures=Object.fromEntries(Object.keys(hashes()).map((key,i)=>[key,{index:i+10}])),encoded=encodeLayerGpuRecords(material,{textures,uvSets:3}).buffer;
    for(let layer=0;layer<native.length;layer++)for(let slot=0;slot<9;slot++){
      const offset=layer*LAYER_GPU_BYTES+slot*48,n=native[layer].slots[slot];
      if(n.name){assert.equal(n.uv,encoded.readUInt32LE(offset+32));assert.equal(n.sampler,encoded.readUInt32LE(offset+28));assert.equal(n.channel,encoded.readUInt32LE(offset+36));for(let j=0;j<3;j++){assert.ok(Math.abs(n.row0[j]-encoded.readFloatLE(offset+j*4))<1e-6);assert.ok(Math.abs(n.row1[j]-encoded.readFloatLE(offset+16+j*4))<1e-6);}}
    }
    assert.equal(native[1].source,encoded.readUInt32LE(LAYER_GPU_BYTES+504));assert.equal(native[1].flags,encoded.readUInt32LE(LAYER_GPU_BYTES+508));
    material.layers[1].id=material.layers[0].id;writeFileSync(file,JSON.stringify(material));assert.throws(()=>execFileSync(exe,[file],{windowsHide:true,stdio:'pipe'}));
  } finally {rmSync(dir,{recursive:true,force:true});}
});

test('file-backed material compilation checks actual textures, semantics, colors and locked content',async()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-closure-'));
  try {
    const value={schema:LAYERED_SCHEMA,name:'closure',layers:[{id:'base',textures:{albedo:{name:'brick'},height:{name:'h'}}},{id:'overlay',textures:{albedo:{name:'brick'}},blend:{source:'textureMask',texture:{name:'mask'}}}]};
    const image={width:1,height:1,data:Buffer.from([80,90,100,255])};
    const create=(name,semantic,srgb=false)=>writeFileSync(path.join(dir,`${name}.${semantic}.ngtex`),writeNgtex({mips:makeMips(image,semantic),semantic,srgb}));
    create('brick','albedo',true);create('h','height');create('mask','mask');
    const args={textureDirectory:dir,shaderFingerprint:'a'.repeat(64),uvSets:1};
    const first=await compileLayeredMaterialAssets(value,args);assert.equal(first.dependencies.length,3);
    assert.equal((await compileLayeredMaterialAssets(value,{...args,expectedDependencies:first.dependencies})).hash,first.hash);
    image.data[0]=81;create('mask','mask');
    await assert.rejects(compileLayeredMaterialAssets(value,{...args,expectedDependencies:first.dependencies}),/content changed/);
    assert.notEqual((await compileLayeredMaterialAssets(value,args)).hash,first.hash);
    await assert.rejects(compileLayeredMaterialAssets(value,{...args,expectedDependencies:first.dependencies.slice(1)}),/does not match/);
    create('mask','mask',true);await assert.rejects(compileLayeredMaterialAssets(value,args),/requires linear/);
    writeFileSync(path.join(dir,'mask.mask.ngtex'),readFileSync(path.join(dir,'h.height.ngtex')));
    await assert.rejects(compileLayeredMaterialAssets(value,args),/has semantic height/);
    rmSync(path.join(dir,'mask.mask.ngtex'));await assert.rejects(compileLayeredMaterialAssets(value,args),/missing texture/);
  } finally { rmSync(dir,{recursive:true,force:true}); }
});

test('layer normalization preserves independent per-slot UV and leaves its input unchanged',()=>{
  const input=make(),snapshot=structuredClone(input),normalized=normalizeLayeredMaterial(input);
  assert.deepEqual(input,snapshot);assert.deepEqual(normalized.layers[1].uv.mapping.scale,[2,3]);
  assert.equal(normalized.layers[1].textures.albedo.uv.set,2);assert.equal(normalized.layers[1].textures.normal.uv.set,1);
  assert.equal(normalized.layers[1].blend.texture.uv.set,2);assert.equal(normalized.layers[0].normalStrength,1);
});
test('compiler locks all texture identities, layer order, UVs, shader version and preserves runtime mask',()=>{
  const value=make(),first=compileLayeredMaterial(value,options()),reordered=structuredClone(value);
  assert.deepEqual(first.activeLayers,['brick','moss']);assert.deepEqual(first.identity.uvSets,[0,1,2]);assert.equal(first.dependencies.length,7);
  assert.equal(first.material.layers[1].blend.source,'textureMask');
  reordered.layers[1].textures.normal.uv.set=2;assert.notEqual(compileLayeredMaterial(reordered,options()).hash,first.hash);
  const changed=options();changed.hashes['height/moss']='f'.repeat(64);assert.notEqual(compileLayeredMaterial(value,changed).hash,first.hash);
  changed.hashes['height/moss']=hashes()['height/moss'];changed.shaderFingerprint='b'.repeat(64);assert.notEqual(compileLayeredMaterial(value,changed).hash,first.hash);
  const reversed={...value,layers:[{...value.layers[1],blend:undefined},{...value.layers[0]}]};delete reversed.layers[0].blend;
  assert.notEqual(compileLayeredMaterial(reversed,options()).hash,first.hash);
});
test('invalid source references, duplicate layers, unknown fields and unavailable UVs fail explicitly',()=>{
  for(const mutate of [v=>v.layers.push({...v.layers[0]}),v=>v.layers[0].enabled=false,v=>v.layers[1].textures.height.name='../escape',v=>v.layers[1].blend.source='unknown',v=>v.layers[1].blend.width=0,v=>v.layers[1].textures.albedo.uv={set:16},v=>v.layers[1].unknown=5]){
    const v=make();mutate(v);assert.throws(()=>normalizeLayeredMaterial(v),{code:'NG-E-MAT-LAYER'});
  }
  const missing=options();delete missing.hashes['mask/mask'];assert.throws(()=>compileLayeredMaterial(make(),missing),/missing valid content hash/);
  const limited=options();limited.uvSets=2;assert.throws(()=>compileLayeredMaterial(make(),limited),/UV set 2 is unavailable/);
});
test('constant-zero and disabled overlays are excluded without losing authored dependency identity',()=>{
  const v=make();v.layers[1].blend={source:'constant',weight:0};const opts=options(),result=compileLayeredMaterial(v,opts);
  assert.deepEqual(result.activeLayers,['brick']);assert.equal(result.dependencies.length,6);
  v.layers[1].enabled=false;assert.deepEqual(compileLayeredMaterial(v,opts).activeLayers,['brick']);
  const changed=options();changed.hashes['height/moss']='f'.repeat(64);assert.notEqual(compileLayeredMaterial(v,changed).hash,compileLayeredMaterial(v,opts).hash);
});
test('coverage has exact endpoints, neutral equal heights and bounded finite output',()=>{
  for(const base of [-1000,0,1000])for(const top of [-1000,0,1000])for(const width of [.0001,.2,1000]){
    assert.equal(heightCoverage(0,base,top,width),0);assert.equal(heightCoverage(1,base,top,width),1);
    const value=heightCoverage(.3,base,top,width);assert.ok(Number.isFinite(value)&&value>=0&&value<=1);
  }
  assert.equal(heightCoverage(.3,.5,.5,.2),.3);
  assert.equal(evaluateLayerCoverage({mode:'height',source:'textureMask',texture:{name:'mask'},weight:.8},0,0,1),0);
  assert.ok(evaluateLayerCoverage({source:'vertexColor',weight:.5,invert:true},.2,0,0)>.39);
  assert.throws(()=>heightCoverage(.2,0,0,0),{code:'NG-E-MAT-LAYER'});
});
test('linear PBR and normal reference blending handles endpoints and antipodal normals',()=>{
  const a=sample(.1,[0,0,1]),b=sample(.9,[0,0,-1]);assert.deepEqual(blendLayerSamples(a,b,0),a);assert.deepEqual(blendLayerSamples(a,b,1),b);
  const mid=blendLayerSamples(a,b,.5);assert.equal(mid.roughness,.5);assert.deepEqual(mid.normal,[0,0,-1]);assert.ok(Math.abs(Math.hypot(...mid.normal)-1)<1e-8);
  assert.throws(()=>blendLayerSamples(sample(.2,[0,0,0]),b,.5),/must not be zero/);
});
test('height and independent surface maps use versioned linear NGTX semantics and linear mips',()=>{
  const image={width:2,height:2,data:Buffer.from([0,0,0,0,255,255,255,255,255,255,255,255,0,0,0,0])};
  for(const semantic of ['height','roughness','metallic']){
    const mips=makeMips(image,semantic);assert.deepEqual([...mips[1].data],[128,128,128,128]);
    const bytes=writeNgtex({mips,semantic}),cooked=readNgtex(bytes);assert.equal(cooked.semantic,semantic);assert.equal(cooked.srgb,false);
    assert.deepEqual(textureName(`rock.${semantic}.png`),{name:'rock',semantic});
    assert.throws(()=>writeNgtex({mips,semantic,srgb:true}),/requires linear storage/);
    const invalid=Buffer.from(bytes);invalid.writeUInt16LE(1,14);assert.throws(()=>readNgtex(invalid),/requires linear storage/);
  }
});
test('reference samples two layers with distinct UVs, masks, height and normal frames',()=>{
  const value=make(),calls=[],frames=[];
  const colors={'albedo/brick':[.8,.2,.1,1],'albedo/moss':[.1,.7,.2,1],'normal/brick':[.5,.5,1,1],'normal/moss':[.5,.5,1,1],'height/brick':[.4,0,0,1],'height/moss':[.4,0,0,1],'mask/mask':[0,.5,0,1]};
  const result=evaluateLayeredMaterial(value,{uvSets:[[.1,.2],[.3,.4],[.2,.1]],sampleTexture:q=>{calls.push(q);return colors[`${q.semantic}/${q.name}`];},normalToWorld:q=>{frames.push(q.uvSet);return q.normal;}});
  assert.deepEqual(frames,[0,1]);assert.deepEqual(calls.find(c=>c.name==='mask').uv,[.4,.30000000000000004]);
  assert.ok(Math.abs(result.albedo[0]-(.8*.625+.1*.375))<1e-7);assert.deepEqual(result.normal,[0,0,1]);
  assert.throws(()=>evaluateLayeredMaterial(value,{uvSets:[[0,0],[0,0],[0,0]],sampleTexture:q=>colors[`${q.semantic}/${q.name}`]}),/normalToWorld is required/);
  assert.throws(()=>evaluateLayeredMaterial(value,{uvSets:[[0,0]],sampleTexture:q=>colors[`${q.semantic}/${q.name}`],normalToWorld:q=>q.normal}),/uvSets\[2\]/);
});
test('zero coverage avoids sampling hidden overlay while vertex color and scalar factors alter output',()=>{
  const value={schema:LAYERED_SCHEMA,name:'paint',layers:[{id:'base',baseColor:[1,0,0]},{id:'paint',baseColor:[0,0,1],blend:{source:'vertexColor',channel:'g',weight:.5},textures:{albedo:{name:'paint'}}}]};
  let samples=0;const context={uvSets:[[0,0]],sampleTexture:()=>{samples++;return [1,1,1,1];},vertexColor:[1,0,1,1]};
  assert.deepEqual(evaluateLayeredMaterial(value,context).albedo,[1,0,0]);assert.equal(samples,0);
  context.vertexColor=[1,1,1,1];assert.deepEqual(evaluateLayeredMaterial(value,context).albedo,[.5,0,.5]);assert.equal(samples,1);
});
test('native loader accepts the three linear data semantics and rejects sRGB corruption', {skip:process.platform!=='win32'||!existsSync(compiler)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-texture-'));
  try {
    const good=path.join(dir,'good'),bad=path.join(dir,'bad'),exe=path.join(dir,'native.exe');mkdirSync(good);mkdirSync(bad);
    const image={width:2,height:2,data:Buffer.from([0,0,0,255,255,255,255,255,255,255,255,255,0,0,0,255])};
    for(const semantic of ['height','roughness','metallic'])writeFileSync(path.join(good,`map.${semantic}.ngtex`),writeNgtex({mips:makeMips(image,semantic),semantic}));
    const invalid=writeNgtex({mips:makeMips(image,'height'),semantic:'height'});invalid.writeUInt16LE(1,14);writeFileSync(path.join(bad,'bad.height.ngtex'),invalid);
    execFileSync(compiler,['-std=c++17','-O2','-static','-municode','.incrementalTesting/rendering/layered-texture-native.cpp','host/texture_asset.cpp','-o',exe],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
    execFileSync(exe,[good,'accept'],{windowsHide:true,timeout:30000});execFileSync(exe,[bad,'reject'],{windowsHide:true,timeout:30000});
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('data-map imports preserve linear bytes, immutable identity and a visible color preview',async()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-import-'));
  try {
    const source=path.join(dir,'data.png'),pixels=Buffer.alloc(16*16*4);for(let i=0;i<pixels.length;i+=4){pixels[i]=pixels[i+1]=pixels[i+2]=64;pixels[i+3]=255;}
    writeFileSync(source,encodePng({width:16,height:16,data:pixels}));
    for(const semantic of ['height','mask','roughness','metallic']){
      const asset=await importAsset({source,output:path.join(dir,'imports'),size:16,semantic});await verifyImport(asset.directory);
      const map=readNgtex(readFileSync(path.join(asset.directory,`textures/texture.${semantic}.ngtex`)));
      const preview=readNgtex(readFileSync(path.join(asset.directory,'textures/preview.albedo.ngtex')));
      assert.equal(map.semantic,semantic);assert.equal(map.srgb,false);assert.equal(map.mips[0].data[0],64);
      assert.equal(preview.srgb,true);assert.ok(decodeFirstMip(preview)[0]>120);
      assert.equal(asset.manifest.semantic,semantic);
    }
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('native height and normal reference agrees with the JS contract', {skip:process.platform!=='win32'||!existsSync(compiler)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-math-'));
  try {
    const exe=path.join(dir,'math.exe');
    execFileSync(compiler,['-std=c++17','-O2','-static','.incrementalTesting/rendering/layered-math-native.cpp','-o',exe],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
    const lines=execFileSync(exe,[],{windowsHide:true,encoding:'utf8',timeout:30000}).trim().split(/\r?\n/);assert.equal(lines.length,336);
    for(const line of lines){const [coverage,bottom,top,width,bias,result]=line.split(' ').map(Number);assert.ok(Math.abs(result-heightCoverage(coverage,bottom,top,width,bias))<.00001,line);}
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('shared layered shader helpers compile as a shader model 6.6 compute shader', {skip:process.platform!=='win32'||!existsSync(dxc)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-shader-'));
  try {
    const output=path.join(dir,'shader.dxil');
    execFileSync(dxc,['-T','cs_6_6','-E','CSMain','-Fo',output,'.incrementalTesting/rendering/layered-shader-contract.hlsl'],{cwd:root,windowsHide:true,timeout:30000,stdio:'pipe'});
    assert.ok(readFileSync(output).length>0);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('independently sampled layered surface compiles as an SM 6.6 pixel shader', {skip:process.platform!=='win32'||!existsSync(dxc)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-pixel-'));
  try {
    const output=path.join(dir,'shader.dxil');
    execFileSync(dxc,['-T','ps_6_6','-E','PSMain','-Fo',output,'.incrementalTesting/rendering/layered-sampling-contract.hlsl'],{cwd:root,windowsHide:true,timeout:30000,stdio:'pipe'});
    assert.ok(readFileSync(output).length>0);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('production material pixel variants compile with layered material records', {skip:process.platform!=='win32'||!existsSync(dxc)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-prod-'));
  try {
    for(const [entry,defines] of [['PSMain',[]],['PSCapture',[]],['PSMain',['PASS_INLINE']]]){
      const output=path.join(dir,`${entry}${defines.join('')}.dxil`);
      execFileSync(dxc,['-T','ps_6_6','-E',entry,'-D','PASS_PS',...defines.flatMap(v=>['-D',v]),'-Fo',output,'shaders/material.hlsl'],{cwd:root,windowsHide:true,timeout:30000,stdio:'pipe'});
      assert.ok(readFileSync(output).length>0);
    }
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('physical coat closure separates coverage, transmission and Fresnel energy',()=>{
  const base={albedo:[.6,.4,.2],roughness:.9},coat={albedo:[1,1,1],roughness:.05,f0:.04};
  const view=[0,0,1],light=[0,0,1];
  const none=evaluateCoatClosure({base,coat,viewDirection:view,lightDirection:light,coverage:0});
  assert.equal(none.reflectedShare,0);assert.equal(none.transmittedShare,1);assert.deepEqual(none.baseDiffuse,base.albedo);
  const normal=evaluateCoatClosure({base,coat,viewDirection:view,lightDirection:light,coverage:1});
  assert.ok(Math.abs(normal.reflectedShare-.04)<1e-9);
  assert.ok(Math.abs(normal.reflectedShare+normal.transmittedShare-1)<1e-9);
  const hidden=evaluateCoatClosure({base,coat,viewDirection:view,lightDirection:light,transmission:0});
  assert.equal(hidden.transmittedShare,0);assert.deepEqual(hidden.baseDiffuse,[0,0,0]);
  const grazing=evaluateCoatClosure({base,coat,viewDirection:[1,-1,0],lightDirection:light,coverage:1});
  assert.ok(grazing.reflectedShare>normal.reflectedShare);
  assert.ok(Math.abs(fresnelSchlick(.04,1)-.04)<1e-12&&fresnelSchlick(.04,0)===1);
  for(const bad of [()=>evaluateCoatClosure({}),()=>evaluateCoatClosure({base,coat,viewDirection:[0,0,0],lightDirection:light}),()=>evaluateCoatClosure({base,coat,viewDirection:view,lightDirection:light,coverage:2})])assert.throws(bad,{code:'NG-E-MAT-PHYSICAL'});
});
test('coat closure keeps two reflection peaks that parameter mixing cannot express',()=>{
  const bottom={albedo:[.5,.5,.5],roughness:.9},top={albedo:[.5,.5,.5],roughness:.05};
  const mix=evaluateParameterMix({bottom,top,coverage:.5,viewDirection:[0,0,1],lightDirection:[0,0,1]});
  const coat=evaluateCoatClosure({base:bottom,coat:{...top,f0:.04},viewDirection:[0,0,1],lightDirection:[0,0,1],coverage:1});
  assert.ok(Math.abs(mix.roughness-.475)<1e-9);
  assert.ok(coat.coatSpecular>mix.specular*3,'the smooth coat lobe must stay separate from the mixed lobe');
  assert.equal(physicalLayerIdentity().coverageSeparateFromTransmission,true);
});
test('physical coat shader contract compiles as an SM 6.6 compute shader', {skip:process.platform!=='win32'||!existsSync(dxc)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-physical-shader-'));
  try {
    const output=path.join(dir,'shader.dxil');
    execFileSync(dxc,['-T','cs_6_6','-E','CSMain','-Fo',output,'.incrementalTesting/rendering/physical-shader-contract.hlsl'],{cwd:root,windowsHide:true,timeout:30000,stdio:'pipe'});
    assert.ok(readFileSync(output).length>0);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
test('hardware GPU layered height and normals match native reference', {skip:process.env.NG_LAYER_GPU_TEST!=='1'||process.platform!=='win32'||!existsSync(dxc)||!existsSync(compiler)},()=>{
  const dir=mkdtempSync(path.join(tmpdir(),'ng-layer-gpu-'));
  try {
    const shader=path.join(dir,'shader.dxil'),exe=path.join(dir,'gpu.exe');
    execFileSync(dxc,['-T','cs_6_6','-E','CSMain','-Fo',shader,'.incrementalTesting/rendering/layered-shader-contract.hlsl'],{cwd:root,windowsHide:true,timeout:30000,stdio:'pipe'});
    execFileSync(compiler,['-std=c++17','-O2','-static','.incrementalTesting/rendering/layered-shader-gpu.cpp','-ld3d12','-ldxgi','-ldxguid','-o',exe],{cwd:root,windowsHide:true,timeout:120000,stdio:'pipe'});
    const output=execFileSync(exe,[shader],{cwd:root,windowsHide:true,timeout:30000,encoding:'utf8'});
    assert.match(output,/GPU layered reference: 336 cases/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

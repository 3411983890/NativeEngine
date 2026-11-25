import * as fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { canonicalJson, sha256Hex } from './canonical.mjs';
import { AssetError, requireAsset, readGltf } from './gltf-document.mjs';
import { cookStaticModel, decodeImage, cookModelMaterials, textureLimit } from './model-import.mjs';
import { cookDirectory, encodePng, readNgtex } from '../../tools/textures.mjs';
import { exportAnimation } from '../../tools/offline/animation.mjs';
import { cookClothLibrary } from '../../tools/cloth.mjs';
import { cookWaterLibrary } from '../../tools/water.mjs';
import { cookEffectLibrary } from '../../tools/effects.mjs';
import { loadScene, validateScene, commitSceneUpdate, sceneAssets, SceneError } from '../scene/index.mjs';
import { CONVERTIBLE_MODELS, CONVERTIBLE_AUDIO, convertModel, convertAudio } from './converters.mjs';
import { retargetMotion } from './animation-retarget.mjs';
import { readBvhMotion } from './bvh-motion.mjs';
import {coordinateTransform} from './coordinates.mjs';
import {fitCharacterBounds} from './character-bounds.mjs';
import {inspectNgsnd,AUDIO_BYTES_LIMIT} from './audio-data.mjs';
import {cookSceneImport} from './scene-import.mjs';
import {decodeEnvironment,resizeEnvironment,integrateEnvironmentSH,encodeEnvironment,inspectEnvironment,environmentPreview} from './environment-data.mjs';
import {isFloatingEnvironmentHeader} from './environment-container.mjs';
import {sceneResourceRoot} from '../scene/paths.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
export const ASSET_CAPABILITIES = Object.freeze({
  model: {inputs:['.gltf','.glb','.obj',...CONVERTIBLE_MODELS],runtime:true},
  character: {inputs:['.gltf','.glb'],runtime:true,limits:'Merged skins/meshes/materials; 256 palette bones; uniform bone scale'},
  animation: {inputs:['.gltf','.glb','.fbx','.dae','.bvh'],runtime:true,requires:'Append to a character package; exact or rest-pose retargeting'},
  texture: {inputs:['.png','.jpg','.jpeg','.webp','.tiff','.tif','.avif','.dds','.ktx2','.basis','.tga','.bmp'],runtime:true,limits:'2D SDR; albedo/normal/ORM/emissive/occlusion and coating binding; height, mask, roughness and metallic are stored as linear data for layered materials; HDR maps, arrays/cubes and raw ETC2/ASTC need adapters'},
  environment: {inputs:['.hdr','.rgbe','.exr','.dds','.ktx2'],runtime:true,limits:'2:1 linear Rec.709 panorama; 256 MiB decoded source; floating-point sky, diffuse SH and GGX environment reflections'},
  audio: {inputs:['.wav',...CONVERTIBLE_AUDIO],runtime:true},
  effect: {inputs:['.effect.json'],runtime:true},
  cloth: {inputs:['.cloth.json'],runtime:'CPU library'},
  water: {inputs:['.water.json'],runtime:'library'},
  scene: {inputs:['.scene.json'],runtime:true},
  font: {inputs:['.ttf','.otf','.woff','.woff2'],runtime:false},
});
const writeJson=(file,data)=>fs.writeFile(file,canonicalJson(data)+'\n');
const previewIdentity=scene=>({...scene,projectId:null,entities:scene.entities.map(entity=>({...entity,id:null})),...(scene.audio?{audio:{...scene.audio,emitters:scene.audio.emitters.map(e=>({...e,entity:`entity:${scene.entities.findIndex(entity=>entity.id===e.entity)}`}))}}:{})});
const previewUuid=(id,n)=>{const hash=sha256Hex(`${id}:${n}`);return `${hash.slice(0,8)}-${hash.slice(8,12)}-4${hash.slice(13,16)}-8${hash.slice(17,20)}-${hash.slice(20,32)}`;};
const assertScene=(scene,assetsDirectory)=>{const result=validateScene(scene,{assetsDirectory});requireAsset(result.valid,`Imported scene is invalid: ${result.errors.join('; ')}`);};
const run=(tool,args,cwd)=>new Promise((resolve,reject)=>{
  const child=spawn(process.execPath,[path.join(root,tool),...args],{cwd,windowsHide:true,shell:false,stdio:['ignore','pipe','pipe']});
  let output='';for(const stream of [child.stdout,child.stderr])stream.on('data',b=>{output=(output+b).slice(-32768);});
  child.once('error',reject);child.once('exit',code=>code===0?resolve(output):reject(new AssetError('CONVERT',output)));
});
async function allFiles(directory,prefix='') {
  const result=[];
  for(const item of (await fs.readdir(directory,{withFileTypes:true})).sort((a,b)=>a.name.localeCompare(b.name))) {
    if(item.name.startsWith('.'))continue;
    const name=prefix+item.name,full=path.join(directory,item.name);
    requireAsset(!item.isSymbolicLink(), 'Asset package cannot contain symlinks','PATH');
    if(item.isDirectory())result.push(...await allFiles(full,name+'/'));
    else {const bytes=await fs.readFile(full);result.push({file:name,bytes:bytes.length,sha256:sha256Hex(bytes)});}
  }
  return result;
}
function baseScene() {
  return {schemaVersion:2,projectId:randomUUID(),name:'外部资产预览',revision:0,settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},
    entities:[{id:randomUUID(),name:'玩家',kind:'player',position:[0,0,5],scale:[.6,1.8,.6],color:[1,1,1],collider:true},
      {id:randomUUID(),name:'地面',kind:'ground',position:[0,-.1,0],scale:[100,.2,100],color:[.5,.5,.5],material:'m0',collider:true}]};
}
function modelEntities(manifest,prefix='',collider=false) {
  const instance=randomUUID();
  return manifest.parts.map(part=>({id:randomUUID(),name:part.name.slice(0,128),kind:'prop',position:part.position,scale:part.scale,color:[1,1,1],collider,
    ...(manifest.id&&prefix&&part.source?{imported:{asset:manifest.id,part:part.source.key,instance}}:{}),
    mesh:{file:prefix+part.mesh,sha256:part.sha256},material:prefix?`${manifest.id.slice(0,12)}_${part.material}`:part.material}));
}
export async function inspectAsset(file) {
  const stat=await fs.stat(file);requireAsset(stat.isFile()&&stat.size<=256*1024*1024,'Expected a file no larger than 256 MiB','BUDGET');
  const lower=file.toLowerCase();
  let details={},kind=Object.entries(ASSET_CAPABILITIES).find(([k,v])=>!['character','animation'].includes(k)&&v.inputs.some(ext=>lower.endsWith(ext)))?.[0];
  if(kind==='texture'&&/\.(dds|ktx2)$/.test(lower)){const input=await fs.open(file,'r');try{const prefix=Buffer.alloc(160),{bytesRead}=await input.read(prefix,0,prefix.length,0);if(isFloatingEnvironmentHeader(prefix.subarray(0,bytesRead)))kind='environment';}finally{await input.close();}}
  if(!kind&&lower.endsWith('.json')&&stat.size<=16*1024*1024){try{const document=JSON.parse(await fs.readFile(file,'utf8'));if(document.schemaVersion&&Array.isArray(document.entities))kind='scene';}catch(error){if(!(error instanceof SyntaxError))throw error;}}
  if(lower.endsWith('.bvh'))kind='animation';
  if(/\.(gltf|glb)$/.test(lower)){
    const {document}=await readGltf(file),hasMesh=document.nodes?.some(n=>n.mesh!==undefined);
    kind=!hasMesh&&document.animations?.length?'animation':document.skins?.length||document.animations?.length||document.meshes?.some(m=>m.primitives?.some(p=>p.targets?.length))?'character':'model';
    if(document.animations?.length)details={clips:document.animations.map((a,i)=>a.name??`clip_${i}`),bones:(document.nodes??[]).map((n,i)=>({node:i,name:n.name??`node_${i}`}))};
  }
  requireAsset(kind,`Unsupported asset format: ${path.extname(file)}. FBX, proprietary engine packages and simulation caches need format adapters.`,'UNSUPPORTED');
  return {kind,file:path.resolve(file),bytes:stat.size,capability:ASSET_CAPABILITIES[kind],...details};
}

async function publishImport(staging,output,payload,{sourceName,sourceHash,options}) {
  const kind=payload.kind,files=await allFiles(staging),identity={version:2,kind,sourceName,sourceHash,options,payload,files:files.filter(f=>f.file!=='preview.scene.json')};
  if(['model','character','audio','texture','environment'].includes(kind))identity.previewScene=previewIdentity(JSON.parse(await fs.readFile(path.join(staging,'preview.scene.json'),'utf8')));
  if(kind==='scene')identity.previewScene=JSON.parse(await fs.readFile(path.join(staging,'preview.scene.json'),'utf8'));
  const id=sha256Hex(canonicalJson(identity)),manifest={...payload,schema:'nativegpu.import/1',assetSchema:payload.schema,id,identity,identityJson:canonicalJson(identity),sourceName,sourceHash,files};
  if(identity.previewScene&&kind!=='scene') {
    const scene=JSON.parse(await fs.readFile(path.join(staging,'preview.scene.json'),'utf8'));
    const ids=new Map(scene.entities.map((e,i)=>[e.id,previewUuid(id,i)]));
    if(scene.audio)for(const emitter of scene.audio.emitters)emitter.entity=ids.get(emitter.entity);
    scene.projectId=previewUuid(id,'project');scene.entities.forEach((e,i)=>e.id=previewUuid(id,i));await writeJson(path.join(staging,'preview.scene.json'),scene);manifest.files=await allFiles(staging);
  }
  await writeJson(path.join(staging,'import.json'),manifest);
  const target=path.join(output,id);
  try {await fs.rename(staging,target);}catch(error){
    if(!['EEXIST','ENOTEMPTY','EPERM'].includes(error.code))throw error;
    const existing=await verifyImport(target);requireAsset(canonicalJson(existing)===canonicalJson(manifest),'Existing immutable import differs','HASH');
  }
  return {manifest,directory:target,manifestPath:path.join(target,'import.json')};
}

// Content-addressed publish. Failed imports never expose a partial live package.
export async function importAsset({source,output=path.join(root,'assets/imported'),size='auto',semantic='albedo',kind,coordinates,expectedKind}={}) {
  requireAsset(typeof source==='string'&&source.length>0,'Choose a source asset','ARGUMENT');
  // `auto` keeps every source map at its authored resolution (capped at 4096) and
  // preserves its aspect ratio. A number is a hard cap on the longest edge.
  const limit=textureLimit(size);size=limit;
  const inspected=await inspectAsset(source);kind=kind??inspected.kind;
  let conversion=coordinateTransform(coordinates);
  requireAsset(conversion.isDefault||['model','character'].includes(kind),'Coordinate overrides require a model or character asset','ARGUMENT');
  requireAsset(Object.hasOwn(ASSET_CAPABILITIES,kind),'Unknown asset type','ARGUMENT');
  source=await fs.realpath(source);const originalHash=sha256Hex(await fs.readFile(source));output=path.resolve(output);await fs.mkdir(output,{recursive:true});
  const staging=path.join(output,`.import-${randomUUID()}`);await fs.mkdir(staging);
  let payload;
  try {
    let processingSource=source,convertedWarnings=[];
    if(CONVERTIBLE_MODELS.includes(path.extname(source).toLowerCase())) {
      const converted=await convertModel(source,path.join(staging,'converted'));
      processingSource=converted.file;convertedWarnings=converted.warnings??[];
      // A converted FBX carries its own unit and axis declaration; apply it only
      // when the caller did not override coordinates explicitly.
      if(conversion.isDefault&&converted.coordinates)conversion=coordinateTransform(converted.coordinates);
      const rechecked=await inspectAsset(processingSource);kind=rechecked.kind;
    }
    if(kind==='model') {
      payload={kind,...await cookStaticModel(processingSource,staging,{size,coordinates:conversion.config})};
      const scene=baseScene();scene.materials=payload.materials;scene.entities.push(...modelEntities(payload));
      // The preview floor follows the model's lower bound, including assets
      // authored below the source origin. Scene attachment keeps source poses.
      const lower=Math.min(...payload.parts.map(part=>part.position[1]-part.scale[1]/2));
      scene.entities[1].position[1]=lower-.1;scene.entities[0].position[1]=lower;
      assertScene(scene);await writeJson(path.join(staging,'preview.scene.json'),scene);
    } else if(kind==='character') {
      const result=await exportAnimation(processingSource,path.join(staging,'character'),{normalize:true,coordinates:conversion.config});
      const asset=await readGltf(processingSource),model={asset,materials:asset.document.materials??[],warnings:[]};
      const materials=await cookModelMaterials(model,staging,{size});
      const headerPath=path.join(staging,'character/character.json'),header=JSON.parse(await fs.readFile(headerPath,'utf8'));
      const states=['idle','walk','run','jump','fall'],mapping=asset.document.extras?.nativegpu?.controller??{};
      const controller=states.map(state=>mapping[state]??header.clips.find(c=>c.name.toLowerCase().includes(state))?.name??header.clips[0].name);
      requireAsset(controller.every(name=>header.clips.some(c=>c.name===name)),'Controller maps to an unknown clip');
      const findBone=aliases=>header.bones.find(b=>aliases.includes(b.name.toLowerCase().replace(/[^a-z0-9]/g,'')))?.name??'';
      const attachments={chest:findBone(['chest','spine2','mixamorigspine2']),rightHand:findBone(['righthand','mixamorigrighthand']),...asset.document.extras?.nativegpu?.attachments};
      requireAsset(Object.values(attachments).every(name=>name===''||header.bones.some(b=>b.name===name)),'Unknown character attachment');
      header.controller=controller;header.attachments=attachments;await writeJson(headerPath,header);
      payload={kind,schema:'nativegpu.character-import/1',character:'character/character.json',material:header.mesh.parts?.[0]?.material??materials.names[0],materials:materials.materials,materialSources:materials.materialSources,parts:header.mesh.parts??[],
        bones:result.bones,clips:result.clips,controller,attachments,warnings:[...(result.warnings??[]),...states.flatMap((state,i)=>mapping[state]!==undefined||controller[i].toLowerCase().includes(state)?[]:[`No ${state} binding was provided; preview uses ${controller[i]}. Configure extras.nativegpu.controller for locomotion.`])]};
      const scene=baseScene();scene.materials={...materials.materials,default:materials.materials[payload.material]};
      const headerBytes=await fs.readFile(headerPath),binaryBytes=await fs.readFile(path.join(staging,'character/animation.bin')),fit=fitCharacterBounds(header,binaryBytes,{mode:asset.document.skins?.length?'humanoid':'bounds'}),player=scene.entities.find(e=>e.kind==='player');
      player.scale=[fit.radius*2,fit.height,fit.radius*2];player.character={file:'character/character.json',sha256:sha256Hex(headerBytes),binarySha256:sha256Hex(binaryBytes),anchor:'feet',offset:fit.offset,
        controller:{orbitDistance:7*Math.max(fit.height/1.8,(fit.bounds.max[0]-fit.bounds.min[0])/1.8,(fit.bounds.max[2]-fit.bounds.min[2])/1.8)}};
      assertScene(scene);await writeJson(path.join(staging,'preview.scene.json'),scene);
      for(const [slot,name]of Object.entries(attachments))if(!name)payload.warnings.push(`No ${slot} attachment was found; configure extras.nativegpu.attachments for gameplay attachments.`);
    } else if(kind==='texture') {
      requireAsset(['albedo','normal','orm','emissive','occlusion','coat','coatRoughness','coatNormal','height','mask','roughness','metallic'].includes(semantic),'Invalid texture semantic','ARGUMENT');
      const image=await decodeImage(await fs.readFile(source),{semantic:semantic==='coatNormal'?'normal':semantic==='coatRoughness'?'orm':semantic,extension:path.extname(source)}),input=path.join(staging,'source-textures');await fs.mkdir(input);
      await fs.writeFile(path.join(input,`texture.${semantic}.png`),encodePng(image));
      const defaults={albedo:[190,190,190,255],normal:[128,128,255,255],orm:[255,255,0,0]};
      for(const [slot,color]of Object.entries(defaults))await fs.writeFile(path.join(input,`preview.${slot}.png`),encodePng({width:1,height:1,data:Buffer.from(color)}));
      if(['height','mask','roughness','metallic'].includes(semantic)) {
        // Convert the linear data to sRGB solely for the color preview; keep
        // the original channel bytes untouched in the actual data texture.
        const pixels=Buffer.from(image.data);
        for(let i=0;i<pixels.length;i+=4)for(let c=0;c<3;c++){const x=pixels[i+c]/255;pixels[i+c]=Math.round(255*(x<=.0031308?12.92*x:1.055*x**(1/2.4)-.055));}
        await fs.writeFile(path.join(input,'preview.albedo.png'),encodePng({width:image.width,height:image.height,data:pixels}));
      }
      const {manifest}=cookDirectory({source:input,out:path.join(staging,'textures'),size,preserveAspect:true});
      const scene=baseScene();scene.name='外部贴图预览';scene.materials={m0:{albedo:'preview',normal:'preview',orm:'preview',roughness:.8,metalness:0},m1:{albedo:semantic==='albedo'?'texture':'preview',normal:semantic==='normal'?'texture':'preview',orm:semantic==='orm'?'texture':'preview',roughness:1,metalness:1,...(semantic==='albedo'&&image.data.some((v,i)=>i%4===3&&v<255)?{alphaMode:'MASK',alphaCutoff:.5}:{})}};
      if(semantic==='emissive'){scene.materials.m1.albedo='preview';scene.materials.m1.emissive='texture';scene.materials.m1.emissiveFactor=[1,1,1];scene.materials.m1.emissiveStrength=1;}
      if(semantic==='occlusion'){scene.materials.m1.occlusion='texture';scene.materials.m1.occlusionStrength=1;scene.materials.m1.metalness=0;}
      if(['coat','coatRoughness','coatNormal'].includes(semantic)){Object.assign(scene.materials.m1,{[semantic]:'texture',clearcoat:1,clearcoatRoughness:.25,metalness:0});}
      scene.entities.push({id:randomUUID(),name:'贴图样方',kind:'prop',position:[0,1.5,0],scale:[3,3,3],color:[1,1,1],material:'m1',collider:false});assertScene(scene);await writeJson(path.join(staging,'preview.scene.json'),scene);
      payload={kind,schema:'nativegpu.texture-import/1',semantic,textures:manifest.textures.filter(t=>t.name==='texture'),preview:true,warnings:[]};
    } else if(kind==='environment') {
      const decoded=await decodeEnvironment(await fs.readFile(source),{extension:path.extname(source).toLowerCase()}),image=resizeEnvironment(decoded,size),bytes=encodeEnvironment(image,integrateEnvironmentSH(decoded)),environment='environment/sky.ngenv',environmentSha256=sha256Hex(bytes);await fs.mkdir(path.join(staging,'environment'));
      await fs.writeFile(path.join(staging,environment),bytes);await fs.writeFile(path.join(staging,'preview.png'),encodePng(environmentPreview(resizeEnvironment(image,Math.min(512,image.width)))));
      const scene=baseScene();delete scene.entities[1].material;scene.name='外部环境预览';scene.lighting={environment:{file:environment,sha256:environmentSha256},quality:1,toneMap:true,ambient:[0,0,0],keyIntensity:0,cloudCoverage:0};
      scene.entities.push({id:randomUUID(),name:'环境光样方',kind:'prop',position:[0,1.5,0],scale:[3,3,3],color:[1,1,1],material:{albedo:[.5,.5,.5],roughness:.5,metalness:0,pattern:'checker'},collider:false});assertScene(scene);await writeJson(path.join(staging,'preview.scene.json'),scene);
      payload={kind,schema:'nativegpu.environment-import/1',environment,environmentSha256,width:image.width,height:image.height,sourceFormat:decoded.sourceFormat,preview:true,warnings:decoded.warnings};
    } else if(kind==='audio') {
      const input=path.join(staging,'source');await fs.mkdir(input);
      if(path.extname(source).toLowerCase()==='.wav')await fs.copyFile(source,path.join(input,'sound.wav'));
      else await convertAudio(source,path.join(input,'sound.wav'));
      await run('tools/audio.mjs',['import','--source',input,'--out',path.join(staging,'audio'),'--bank',path.join(staging,'audio/bank.json'),'--namespace','none'],staging);
      // The existing bank format resolves sound paths from the process working directory.
      // Publish a package-relative bank below; launch consumers with package root as cwd.
      const bank=JSON.parse(await fs.readFile(path.join(staging,'audio/bank.json'),'utf8'));
      for(const sound of bank.sounds)sound.file=`audio/${sound.name}.ngsnd`;
      await writeJson(path.join(staging,'audio/bank.json'),bank);
      const sounds={};for(const sound of bank.sounds){const bytes=await fs.readFile(path.join(staging,sound.file));inspectNgsnd(bytes);requireAsset(sha256Hex(bytes)===sound.sha256,'Cooked sound hash differs from bank','HASH');sounds[sound.name]={file:sound.file,sha256:sound.sha256};}
      const scene=baseScene();delete scene.entities[1].material;
      scene.audio={sounds,emitters:bank.sounds.map((sound,i)=>({id:`preview_${i}`,entity:scene.entities[0].id,sound:sound.name,bus:'music',spatial:false,loop:true,occlusion:false,gainDb:0}))};
      assertScene(scene);await writeJson(path.join(staging,'preview.scene.json'),scene);
      payload={kind,schema:'nativegpu.audio-import/1',bank:'audio/bank.json',workingDirectory:'.',sounds,warnings:[]};
    } else if(['cloth','water','effect'].includes(kind)) {
      const input=path.join(staging,'source');await fs.mkdir(input);await fs.copyFile(source,path.join(input,path.basename(source)));
      const options={sourceDirectory:input,outputDirectory:path.join(staging,kind)};
      if(kind==='cloth')await cookClothLibrary(options);
      if(kind==='water')await cookWaterLibrary(options);
      if(kind==='effect') {
        const original=JSON.parse(await fs.readFile(source,'utf8'));
        const refs=(original.emitters??[]).flatMap(e=>(e.renderers??[]).flatMap(r=>[r.texture,r.mesh].filter(Boolean)));
        let assetRoot=path.dirname(source);
        while(!(await Promise.all(refs.map(ref=>fs.stat(path.join(assetRoot,ref)).then(s=>s.isFile()).catch(()=>false)))).every(Boolean)) {
          const parent=path.dirname(assetRoot);requireAsset(parent!==assetRoot,'Effect dependencies are missing','DEPENDENCY');assetRoot=parent;
        }
        const library=await cookEffectLibrary({...options,assetRoot});
        for(const [ref,hash]of Object.entries(library.dependencies)) {
          const dependency=await fs.realpath(path.join(assetRoot,ref)),relative=path.relative(await fs.realpath(assetRoot),dependency);
          requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Effect dependency escapes source','PATH');
          const bytes=await fs.readFile(dependency);requireAsset(sha256Hex(bytes)===hash,'Effect dependency changed during import','HASH');
          const target=path.join(staging,'assets',ref);await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,bytes);
        }
      }
      payload={kind,schema:`nativegpu.${kind}-import/1`,library:`${kind}/library.json`,warnings:kind==='cloth'?['Cloth currently has a CPU library; garment authoring and GPU cloth are pending.']:[]};
    } else if(kind==='animation') {
      throw new AssetError('ARGUMENT','Choose a target character and use append-animation to import a motion source.');
    } else if(kind==='scene') {
      const cooked=await cookSceneImport(source,staging);
      await writeJson(path.join(staging,'scene.scene.json'),cooked.scene);await writeJson(path.join(staging,'preview.scene.json'),cooked.scene);
      payload={kind,schema:'nativegpu.scene-import/1',scene:'scene.scene.json',preservePreviewIds:true,dependencies:cooked.dependencies,warnings:[]};
    } else {
      throw new AssetError('UNSUPPORTED',`${kind} requires its runtime renderer; copying a source file would not make it usable.`);
    }
    requireAsset(sha256Hex(await fs.readFile(source))===originalHash,'Source changed during import; retry with a stable source','HASH');
    if(convertedWarnings.length)payload.warnings=[...new Set([...convertedWarnings,...(payload.warnings??[])])];
    if(!conversion.isDefault)payload.coordinates=conversion.config;
    requireAsset(expectedKind===undefined||payload.kind===expectedKind,'Reimport cannot change the asset kind','ARGUMENT');
    return await publishImport(staging,output,payload,{sourceName:path.basename(source),sourceHash:originalHash,options:{size,semantic,...(!conversion.isDefault?{coordinates:conversion.config}:{})}});
  } finally {await fs.rm(staging,{recursive:true,force:true});}
}
export async function appendAnimation({directory,source,output,options={}}={}) {
  requireAsset(typeof directory==='string'&&typeof source==='string','Choose a target character package and a motion source','ARGUMENT');
  requireAsset(options&&typeof options==='object'&&!Array.isArray(options),'Motion options must be an object','ARGUMENT');
  directory=await fs.realpath(directory);const parent=await verifyImport(directory);
  requireAsset(parent.kind==='character','Motion target must be a character package','ARGUMENT');
  await inspectAsset(source);source=await fs.realpath(source);const sourceHash=sha256Hex(await fs.readFile(source));
  requireAsset(['.gltf','.glb','.fbx','.dae','.bvh'].includes(path.extname(source).toLowerCase()),'Motion source must be glTF, GLB, FBX, DAE or BVH','UNSUPPORTED');
  output=path.resolve(output??path.dirname(directory));await fs.mkdir(output,{recursive:true});const staging=path.join(output,`.import-${randomUUID()}`);await fs.mkdir(staging);
  try {
    for(const file of parent.files){const target=path.join(staging,file.file);await fs.mkdir(path.dirname(target),{recursive:true});await fs.copyFile(path.join(directory,file.file),target);}
    let processingSource=source,convertedWarnings=[],motionDefaults;
    if(['.fbx','.dae'].includes(path.extname(source).toLowerCase())) {
      const converted=await convertModel(source,path.join(staging,'motion-converted'));
      processingSource=converted.file;convertedWarnings=converted.warnings??[];motionDefaults=converted.coordinates;
    }
    const asset=path.extname(source).toLowerCase()==='.bvh'?await readBvhMotion(source):await readGltf(processingSource),headerPath=path.join(staging,'character/character.json'),header=JSON.parse(await fs.readFile(headerPath,'utf8')),binary=await fs.readFile(path.join(staging,'character/animation.bin'));
    const effective={...options,prefix:options.prefix??path.basename(source,path.extname(source)).replace(/[^\p{L}\p{N}_.-]/gu,'_')+'_'};
    const motionCoordinates=coordinateTransform(options.coordinates??motionDefaults);if(motionCoordinates.isDefault)delete effective.coordinates;else effective.coordinates=motionCoordinates.config;
    const result=retargetMotion(asset,header,binary,effective);
    requireAsset(Buffer.byteLength(canonicalJson(result.header))<=4*1024*1024,'Appended animation header exceeds 4 MiB','BUDGET');
    await writeJson(headerPath,result.header);await fs.writeFile(path.join(staging,'character/animation.bin'),result.binary);
    const preview={...result.header,controller:Array(5).fill(result.addedClips[0]),clips:result.header.clips.map(clip=>({...clip,matchable:false}))};
    requireAsset(Buffer.byteLength(canonicalJson(preview))<=4*1024*1024,'Motion preview header exceeds 4 MiB','BUDGET');
    await writeJson(path.join(staging,'character/preview.json'),preview);await writeJson(path.join(staging,'character/retarget.json'),result.report);
    const scenePath=path.join(staging,'preview.scene.json'),scene=JSON.parse(await fs.readFile(scenePath,'utf8')),actor=scene.entities.find(e=>e.kind==='player');
    if(actor.character){actor.character.file='character/preview.json';actor.character.sha256=sha256Hex(await fs.readFile(path.join(staging,'character/preview.json')));actor.character.binarySha256=sha256Hex(result.binary);await writeJson(scenePath,scene);}
    const payload={...parent.identity.payload,displayName:`${parent.displayName??parent.sourceName} + ${path.basename(source)}`,clips:result.header.clips.length,controller:result.header.controller,
      previewCharacter:'character/preview.json',previewClip:result.addedClips[0],retarget:'character/retarget.json',parentAsset:parent.id,
      warnings:[...new Set([...(parent.warnings??[]),...convertedWarnings,...result.warnings])],addedClips:result.addedClips};
    requireAsset(sha256Hex(await fs.readFile(source))===sourceHash,'Motion source changed during import','HASH');
    await verifyImport(directory);
    return await publishImport(staging,output,payload,{sourceName:path.basename(source),sourceHash,options:{...parent.identity.options,motion:effective}});
  }finally{await fs.rm(staging,{recursive:true,force:true});}
}
export async function verifyImport(directory) {
  const manifest=JSON.parse(await fs.readFile(path.join(directory,'import.json'),'utf8'));
  requireAsset(manifest.schema==='nativegpu.import/1'&&/^[a-f0-9]{64}$/.test(manifest.id)&&Array.isArray(manifest.files),'Invalid import manifest');
  requireAsset(sha256Hex(canonicalJson(manifest.identity))===manifest.id,'Import manifest identity changed','HASH');
  if(manifest.identity.version>=2)requireAsset(manifest.identityJson===canonicalJson(manifest.identity),'Import identity encoding changed','HASH');
  const {assetSchema,identity,files}=manifest;
  const payload=Object.fromEntries(Object.keys(identity.payload).map(key=>[key,key==='schema'?assetSchema:manifest[key]]));
  requireAsset(canonicalJson(payload)===canonicalJson(identity.payload),'Import metadata changed','HASH');
  requireAsset(canonicalJson(files.filter(f=>f.file!=='preview.scene.json'))===canonicalJson(identity.files),'Import file manifest changed','HASH');
  const base=await fs.realpath(directory);
  const packageFiles=new Set();
  for(const item of manifest.files) {
    requireAsset(typeof item.file==='string'&&!/[\\:]/.test(item.file)&&!item.file.startsWith('/')&&!item.file.split('/').includes('..'),'Invalid package path','PATH');
    requireAsset(item.file.length>0&&!packageFiles.has(item.file),'Duplicate or empty package file','PATH');packageFiles.add(item.file);
    const file=await fs.realpath(path.join(base,item.file)),relative=path.relative(base,file);
    requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Package symlink escapes root','PATH');
    const bytes=await fs.readFile(file);requireAsset(bytes.length===item.bytes&&sha256Hex(bytes)===item.sha256,`Imported file changed: ${item.file}`,'HASH');
  }
  if(['model','character','scene'].includes(manifest.kind)||manifest.kind==='audio'&&manifest.sounds||manifest.kind==='texture'&&manifest.preview)requireAsset(packageFiles.has('preview.scene.json'),'Preview scene is missing from the immutable file manifest','DEPENDENCY');
  if(manifest.kind==='scene') {
    requireAsset(manifest.scene==='scene.scene.json'&&manifest.preservePreviewIds===true&&packageFiles.has(manifest.scene),'Scene entry is missing from import identity','DEPENDENCY');
    const preview=JSON.parse(await fs.readFile(path.join(base,'preview.scene.json'),'utf8')),entry=JSON.parse(await fs.readFile(path.join(base,manifest.scene),'utf8'));
    requireAsset(canonicalJson(preview)===canonicalJson(identity.previewScene)&&canonicalJson(entry)===canonicalJson(preview),'Scene preview differs from import identity','HASH');
  }
  if(manifest.kind==='character') {
    requireAsset(packageFiles.has('character/character.json')&&packageFiles.has('character/animation.bin'),'Character payload is missing from the immutable file manifest','DEPENDENCY');
    if(manifest.previewCharacter!==undefined)requireAsset(manifest.previewCharacter==='character/preview.json'&&packageFiles.has(manifest.previewCharacter),'Character preview is missing from the immutable file manifest','DEPENDENCY');
  }
  if(manifest.kind==='environment'){requireAsset(manifest.environment==='environment/sky.ngenv'&&packageFiles.has(manifest.environment),'Environment payload is missing from the immutable file manifest','DEPENDENCY');const bytes=await fs.readFile(path.join(base,manifest.environment));requireAsset(sha256Hex(bytes)===manifest.environmentSha256,'Environment identity differs','HASH');inspectEnvironment(bytes);}
  if(identity.version>=2&&(['model','character','environment'].includes(manifest.kind)||manifest.kind==='audio'&&manifest.sounds||manifest.kind==='texture'&&manifest.preview)) {
    const scene=JSON.parse(await fs.readFile(path.join(base,'preview.scene.json'),'utf8'));
    if(manifest.kind==='environment')requireAsset(scene.lighting?.environment?.file===manifest.environment&&scene.lighting?.environment?.sha256===manifest.environmentSha256,'Environment preview differs from payload','HASH');
    requireAsset(canonicalJson(previewIdentity(scene))===canonicalJson(identity.previewScene)&&scene.projectId===previewUuid(manifest.id,'project')&&scene.entities.every((e,i)=>e.id===previewUuid(manifest.id,i)),
      'Preview scene differs from import identity','HASH');
  }
  return manifest;
}
export async function listImports(directory=path.join(root,'assets/imported')) {
  let names;try{names=await fs.readdir(directory);}catch(e){if(e.code==='ENOENT')return [];throw e;}
  const results=[];for(const id of names.filter(n=>/^[a-f0-9]{64}$/.test(n)).sort()) {
    try{const item=await verifyImport(path.join(directory,id));const clips=item.kind==='character'?JSON.parse(await fs.readFile(path.join(directory,id,'character/character.json'),'utf8')).clips.map(c=>c.name):undefined;
      results.push({id,kind:item.kind,name:item.displayName??item.sourceName,warnings:item.warnings,...(clips?{clips}: {}),...(item.kind==='texture'?{semantic:item.semantic,preview:!!item.preview}:{})});}
    catch(error){results.push({id,error:error.message});}
  }return results;
}
export async function checkoutSceneImport({directory,outputDirectory}) {
  const manifest=await verifyImport(directory);requireAsset(manifest.kind==='scene','Choose an imported scene package','ARGUMENT');
  const parent=path.resolve(outputDirectory);await fs.mkdir(parent,{recursive:true});const target=path.join(parent,`${manifest.id.slice(0,16)}-${randomUUID()}`),staging=path.join(parent,`.scene-checkout-${randomUUID()}`);await fs.mkdir(staging);
  try {
    for(const entry of manifest.files) {
      if(entry.file==='preview.scene.json')continue;
      const bytes=await fs.readFile(path.join(directory,entry.file));requireAsset(bytes.length===entry.bytes&&sha256Hex(bytes)===entry.sha256,'Scene source changed during checkout','HASH');
      const file=path.join(staging,entry.file===manifest.scene?'main.scene.json':entry.file);await fs.mkdir(path.dirname(file),{recursive:true});await fs.writeFile(file,bytes);
    }
    await loadScene(path.join(staging,'main.scene.json'));await fs.rename(staging,target);
    return {directory:target,scenePath:path.join(target,'main.scene.json')};
  }finally{await fs.rm(staging,{recursive:true,force:true});}
}
const inside=(parent,file)=>{const relative=path.relative(parent,file);return relative!==''&&!relative.startsWith('..')&&!path.isAbsolute(relative);};
function assertAttachRevision(scene,expectedRevision) {
  if(!Number.isSafeInteger(expectedRevision)||expectedRevision!==scene.revision)throw new SceneError('REVISION_CONFLICT','Scene revision changed');
}
// Scene references are confined to their own directory. A catalog import from
// elsewhere is copied as a verified immutable package before committing refs.
export async function planImportAttachment(directory,parent,manifest) {
  const lexical=path.resolve(directory),source=await fs.realpath(lexical);
  requireAsset(!inside(parent,lexical)||inside(parent,source),'Import package symlink must stay inside the scene directory','PATH');
  if(inside(parent,source))return {directory:source,relative:path.relative(parent,source).split(path.sep).join('/'),publish:async()=>{}};
  const relative=`imported/${manifest.id}`,target=path.join(parent,relative);
  return {directory:source,relative,publish:async()=>{
    const container=path.dirname(target);await fs.mkdir(container,{recursive:true});
    requireAsset(inside(parent,await fs.realpath(container)),'Import destination escapes scene directory','PATH');
    const checkTarget=async()=>{
      const stat=await fs.lstat(target);requireAsset(stat.isDirectory()&&!stat.isSymbolicLink()&&inside(parent,await fs.realpath(target)),'Import destination escapes scene directory','PATH');
      requireAsset(canonicalJson(await verifyImport(target))===canonicalJson(manifest),'Import destination conflicts with immutable content','HASH');
    };
    try{await checkTarget();return;}catch(error){if(error.code!=='ENOENT')throw error;}
    const staging=path.join(container,`.attach-${randomUUID()}`);await fs.mkdir(staging);
    try {
      for(const entry of manifest.files){
        const file=await fs.realpath(path.join(source,entry.file));requireAsset(inside(source,file),'Import dependency escapes source directory','PATH');
        const bytes=await fs.readFile(file);requireAsset(bytes.length===entry.bytes&&sha256Hex(bytes)===entry.sha256,'Imported dependency changed during attachment','HASH');
        const output=path.join(staging,entry.file);await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,bytes);
      }
      await writeJson(path.join(staging,'import.json'),manifest);await verifyImport(staging);
      try{await fs.rename(staging,target);}catch(error){if(!['EEXIST','ENOTEMPTY','EPERM'].includes(error.code))throw error;await checkTarget();}
    }finally{await fs.rm(staging,{recursive:true,force:true});}
  }};
}
export async function attachModel({directory,scenePath,expectedRevision,collider=false,characterOptions={}}) {
  requireAsset(typeof collider==='boolean','collider must be boolean','ARGUMENT');
  const manifest=await verifyImport(directory);requireAsset(['model','character'].includes(manifest.kind),'Scene attachment requires a model or character','UNSUPPORTED');
  requireAsset(characterOptions&&typeof characterOptions==='object'&&!Array.isArray(characterOptions),'Character options must be an object','ARGUMENT');
  const allowed=['replacePlayer','clip','speed','timeOffset','yaw','modelScale','offset','position','rootMotion','fitCollider','anchor','controller','rig'];
  requireAsset(Object.keys(characterOptions).every(key=>allowed.includes(key)),'Unknown character attachment option','ARGUMENT');
  requireAsset(manifest.kind==='character'||Object.keys(characterOptions).length===0,'Character options require a character asset','ARGUMENT');
  requireAsset(characterOptions.replacePlayer===undefined||typeof characterOptions.replacePlayer==='boolean','replacePlayer must be boolean','ARGUMENT');
  const scene=await loadScene(scenePath),parent=await fs.realpath(sceneResourceRoot(scenePath));assertAttachRevision(scene,expectedRevision);
  const attachment=await planImportAttachment(directory,parent,manifest),{relative}=attachment;directory=attachment.directory;
  const materialPrefix=manifest.id.slice(0,12)+'_',textureDir=path.join(parent,'textures');
  let entity;
  if(manifest.kind==='character') {
    const header=JSON.parse(await fs.readFile(path.join(directory,'character/character.json'),'utf8')),options=characterOptions;
    requireAsset(options.clip===undefined||options.clip===''||header.clips.some(clip=>clip.name===options.clip),'Unknown character clip','ARGUMENT');
    const file=manifest.files.find(f=>f.file==='character/character.json'),binary=manifest.files.find(f=>f.file==='character/animation.bin');
    const reference={file:`${relative}/character/character.json`,sha256:file.sha256,binarySha256:binary.sha256,
      materials:Object.fromEntries((header.mesh.parts??[]).map(part=>[part.material,materialPrefix+part.material]))};
    for(const key of ['clip','speed','timeOffset','yaw','offset','rootMotion','controller','rig'])if(options[key]!==undefined)reference[key]=options[key];
    reference.anchor=options.anchor??'feet';
    if(options.modelScale!==undefined)reference.scale=options.modelScale;
    entity=options.replacePlayer?structuredClone(scene.entities.find(e=>e.kind==='player')):
      {id:randomUUID(),name:(manifest.displayName??manifest.sourceName).slice(0,128),kind:'prop',position:[0,0,0],scale:[.6,1.8,.6],color:[1,1,1],collider};
    requireAsset(entity,'Scene has no player to replace','ARGUMENT');
    if(options.position!==undefined)entity.position=options.position;
    delete entity.mesh;delete entity.destructible;delete entity.material;entity.character=reference;entity.color=[1,1,1];entity.imported={asset:manifest.id,part:sha256Hex('character-root'),instance:randomUUID()};
    const fit=options.fitCollider??(options.replacePlayer?'humanoid':'bounds');requireAsset(fit===false||['bounds','humanoid'].includes(fit),'fitCollider must be false, bounds or humanoid','ARGUMENT');
    if(fit){const fitted=fitCharacterBounds(header,await fs.readFile(path.join(directory,'character/animation.bin')),{mode:fit,scale:reference.scale,yaw:reference.yaw});entity.scale=entity.kind==='player'?[fitted.radius*2,fitted.height,fitted.radius*2]:fitted.dimensions;if(reference.anchor==='center'&&entity.kind!=='player')fitted.offset[1]-=fitted.height/(2*(reference.scale?.[1]??1));if(options.offset===undefined)reference.offset=fitted.offset;}
  }
  const update=next=>{
    next.schemaVersion=2;next.materials??={};
    for(const [name,material]of Object.entries(manifest.materials))next.materials[materialPrefix+name]??={...material,albedo:materialPrefix+material.albedo,normal:materialPrefix+material.normal,orm:materialPrefix+material.orm,...Object.fromEntries(['emissive','occlusion','coat','coatRoughness','coatNormal'].filter(key=>material[key]).map(key=>[key,materialPrefix+material[key]]))};
    if(entity){if(characterOptions.replacePlayer)next.entities=next.entities.map(e=>e.kind==='player'?entity:e);else next.entities.push(entity);}
    else next.entities.push(...modelEntities(manifest,relative+'/',collider));return next;
  };
  assertScene(update(structuredClone(scene)),sceneAssets(scene));
  await fs.mkdir(textureDir,{recursive:true});
  requireAsset(inside(parent,await fs.realpath(textureDir)),'Texture directory escapes scene','PATH');
  for(const file of manifest.files.filter(f=>f.file.startsWith('textures/')&&f.file.endsWith('.ngtex'))) {
    const target=path.join(textureDir,materialPrefix+path.basename(file.file)),bytes=await fs.readFile(path.join(directory,file.file));
    requireAsset(bytes.length===file.bytes&&sha256Hex(bytes)===file.sha256,'Imported texture changed during attachment','HASH');
    try{await fs.writeFile(target,bytes,{flag:'wx'});}catch(e){if(e.code!=='EEXIST')throw e;requireAsset(inside(parent,await fs.realpath(target)),'Texture file escapes scene','PATH');requireAsset(sha256Hex(await fs.readFile(target))===file.sha256,'Texture destination conflict','HASH');}
  }
  await attachment.publish();
  return commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},update);
}
export async function attachAudio({directory,scenePath,expectedRevision,options={}}) {
  requireAsset(options&&typeof options==='object'&&!Array.isArray(options)&&Object.keys(options).every(k=>['entity','sound','id','trigger','bus','gainDb','minDistance','maxDistance','loop','occlusion','spatial','offset','pitch'].includes(k)),'Unknown audio attachment option','ARGUMENT');
  const manifest=await verifyImport(directory);requireAsset(manifest.kind==='audio','Choose an imported audio package','ARGUMENT');
  const scene=await loadScene(scenePath),parent=await fs.realpath(sceneResourceRoot(scenePath));assertAttachRevision(scene,expectedRevision);
  const attachment=await planImportAttachment(directory,parent,manifest),{relative,directory:actual}=attachment;
  requireAsset(manifest.files.some(f=>f.file===manifest.bank),'Audio bank is missing from immutable manifest','HASH');
  const bank=JSON.parse(await fs.readFile(path.join(actual,manifest.bank),'utf8')),sound=bank.sounds.find(s=>s.name===(options.sound??bank.sounds[0]?.name));requireAsset(sound,'Unknown imported sound','ARGUMENT');
  const declared=manifest.files.find(f=>f.file===sound.file);requireAsset(declared&&declared.sha256===sound.sha256,'Sound is missing from immutable manifest','HASH');
  requireAsset(declared.bytes<=AUDIO_BYTES_LIMIT,'Sound exceeds 64 MiB','BUDGET');inspectNgsnd(await fs.readFile(path.join(actual,declared.file)));
  const entity=options.entity??scene.entities.find(e=>e.kind==='player').id,name=`${manifest.id.slice(0,16)}/${sound.name}`;
  const emitter={id:options.id??randomUUID(),entity,sound:name,bus:'sfx',spatial:true,loop:false,occlusion:true,...options};emitter.sound=name;
  const update=next=>{
    next.schemaVersion=2;next.audio??={};next.audio.sounds??={};next.audio.emitters??=[];
    const reference={file:`${relative}/${sound.file}`,sha256:sound.sha256};
    requireAsset(!Object.hasOwn(next.audio.sounds,name)||canonicalJson(next.audio.sounds[name])===canonicalJson(reference),'Conflicting scene sound name','HASH');
    next.audio.sounds[name]=reference;next.audio.emitters.push(emitter);return next;
  };
  assertScene(update(structuredClone(scene)),sceneAssets(scene));await attachment.publish();
  return commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},update);
}
export async function attachTexture({directory,scenePath,expectedRevision,material}) {
  const manifest=await verifyImport(directory);requireAsset(manifest.kind==='texture','Choose a texture import','ARGUMENT');
  requireAsset(['albedo','normal','orm','emissive','occlusion','coat','coatRoughness','coatNormal'].includes(manifest.semantic),'Texture semantic cannot bind to a material','UNSUPPORTED');
  const scene=await loadScene(scenePath);if(!Number.isSafeInteger(expectedRevision)||expectedRevision!==scene.revision)throw new SceneError('REVISION_CONFLICT','Scene revision changed');requireAsset(typeof material==='string'&&Object.hasOwn(scene.materials??{},material),'Choose an existing named material','ARGUMENT');
  const source=manifest.textures.find(t=>t.name==='texture'&&t.semantic===manifest.semantic);requireAsset(source,'Imported texture semantic is missing','DEPENDENCY');
  const entry=manifest.files.find(f=>f.file===`textures/texture.${manifest.semantic}.ngtex`);requireAsset(entry,'Cooked texture is missing from immutable manifest','DEPENDENCY');
  const parent=await fs.realpath(sceneResourceRoot(scenePath)),textureDir=path.join(parent,'textures');await fs.mkdir(textureDir,{recursive:true});const actual=await fs.realpath(textureDir),relative=path.relative(parent,actual);requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Texture directory escapes scene','PATH');
  const name=manifest.id.slice(0,16)+'_texture',target=path.join(actual,`${name}.${manifest.semantic}.ngtex`),bytes=await fs.readFile(path.join(directory,entry.file));requireAsset(bytes.length===entry.bytes&&sha256Hex(bytes)===entry.sha256,'Imported texture changed during attach','HASH');
  requireAsset(readNgtex(bytes).semantic===manifest.semantic,'Cooked texture semantic differs from import metadata','FORMAT');
  try{await fs.writeFile(target,bytes,{flag:'wx'});}catch(error){if(error.code!=='EEXIST')throw error;const resolved=await fs.realpath(target),inside=path.relative(parent,resolved);requireAsset(inside&&!inside.startsWith('..')&&!path.isAbsolute(inside),'Texture file escapes scene','PATH');requireAsset(sha256Hex(await fs.readFile(resolved))===entry.sha256,'Texture destination conflicts with immutable content','HASH');}
  return commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},next=>{requireAsset(Object.hasOwn(next.materials??{},material),'Material was removed','ARGUMENT');const target=next.materials[material];target[manifest.semantic]=name;if(['coat','coatRoughness','coatNormal'].includes(manifest.semantic)){target.clearcoat??=1;target.clearcoatRoughness??=.25;}return next;});
}

export async function attachEnvironment({directory,scenePath,expectedRevision,options={}}={}){
  requireAsset(options&&typeof options==='object'&&!Array.isArray(options)&&Object.keys(options).every(k=>['intensity','rotation'].includes(k)),'Unknown environment option','ARGUMENT');
  for(const [key,lo,hi]of [['intensity',0,10000],['rotation',-3600,3600]])requireAsset(options[key]===undefined||Number.isFinite(options[key])&&options[key]>=lo&&options[key]<=hi,`Invalid environment ${key}`,'ARGUMENT');
  const manifest=await verifyImport(directory);requireAsset(manifest.kind==='environment','Choose an environment import','ARGUMENT');const scene=await loadScene(scenePath);assertAttachRevision(scene,expectedRevision);
  const parent=await fs.realpath(sceneResourceRoot(scenePath)),plan=await planImportAttachment(directory,parent,manifest),ref={file:plan.relative+'/'+manifest.environment,sha256:manifest.environmentSha256,...options};
  const proposed=structuredClone(scene);proposed.lighting??={};proposed.lighting.environment=ref;assertScene(proposed,sceneAssets(scene));await plan.publish();
  return commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},next=>{next.lighting??={};next.lighting.environment=ref;next.lighting.quality??=1;next.lighting.toneMap??=true;next.lighting.cloudCoverage??=0;return next;});
}
export async function fitSceneCharacter({scenePath,entityId,expectedRevision,mode,alignModel=true}={}) {
  requireAsset(typeof alignModel==='boolean','alignModel must be boolean','ARGUMENT');
  const scene=await loadScene(scenePath),entity=scene.entities.find(e=>e.id===entityId);requireAsset(entity?.character,'Choose a scene character to fit','ARGUMENT');
  const ref=entity.character,root=await fs.realpath(sceneResourceRoot(scenePath)),file=await fs.realpath(path.join(root,ref.file)),relative=path.relative(root,file);
  requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Character fitting source escapes scene directory','PATH');
  const binaryFile=await fs.realpath(path.join(path.dirname(file),'animation.bin')),binaryRelative=path.relative(root,binaryFile);
  requireAsset(binaryRelative&&!binaryRelative.startsWith('..')&&!path.isAbsolute(binaryRelative),'Character fitting binary escapes scene directory','PATH');
  requireAsset((await fs.stat(file)).size<=4*1024*1024&&(await fs.stat(binaryFile)).size<=64*1024*1024,'Fitting source exceeds character budgets','BUDGET');
  const header=await fs.readFile(file),binary=await fs.readFile(binaryFile);requireAsset(sha256Hex(header)===ref.sha256&&sha256Hex(binary)===ref.binarySha256,'Character fitting dependency changed; reimport asset','HASH');
  const fit=fitCharacterBounds(JSON.parse(header),binary,{mode:mode??(entity.kind==='player'?'humanoid':'bounds'),scale:ref.scale,yaw:ref.yaw});entity.scale=entity.kind==='player'?[fit.radius*2,fit.height,fit.radius*2]:fit.dimensions;
  ref.anchor??='feet';if(ref.anchor==='center'&&entity.kind!=='player')fit.offset[1]-=fit.height/(2*(ref.scale?.[1]??1));
  if(alignModel)ref.offset=fit.offset;
  const changed=await commitSceneUpdate(scenePath,{projectId:scene.projectId,expectedRevision},()=>scene);return {scene:changed,fit};
}

import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {fitCharacterBounds} from '../engine/assets/character-bounds.mjs';
import {importAsset,attachModel,fitSceneCharacter,appendAnimation} from '../engine/assets/import-service.mjs';
import {writeCharacterFixture,writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {loadScene,saveScene,undoSceneFile,commitSceneUpdate,validateScene} from '../engine/scene/index.mjs';
import {createEditorService} from '../editor/service.mjs';
import {main as cli} from '../tools/assets.mjs';

const near=(a,b)=>{a.forEach((v,i)=>assert.ok(Math.abs(v-b[i])<.0003,`${a} != ${b}`));};
async function fixture(t){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'nativegpu-fit-'));t.after(()=>fs.rm(dir,{recursive:true,force:true}));const imported=await importAsset({source:await writeCharacterFixture(path.join(dir,'source')),output:path.join(dir,'assets/imported'),size:64});
  const scene=await loadScene(path.join(imported.directory,'preview.scene.json'));delete scene.materials;for(const entity of scene.entities){delete entity.character;delete entity.material;}const file=path.join(dir,'assets/main.scene.json');await saveScene(file,scene);
  const header=JSON.parse(await fs.readFile(path.join(imported.directory,'character/character.json'),'utf8')),binary=await fs.readFile(path.join(imported.directory,'character/animation.bin'));return {dir,file,imported,header,binary};}
test('rest-pose fitting includes independent inverse binds and authored morph defaults',async t=>{
  const {header,binary}=await fixture(t),bounds=fitCharacterBounds(header,binary),human=fitCharacterBounds(header,binary,{mode:'humanoid'});
  assert.ok(bounds.height>1.5&&bounds.height<2.5);assert.ok(bounds.dimensions[0]>2);near(human.dimensions,[human.height/3,human.height,human.height/3]);
  const clear=structuredClone(header);clear.defaultMorphWeights=clear.morphs.map(()=>0);const neutral=fitCharacterBounds(clear,binary);assert.ok(bounds.height>neutral.height);
  const shape=structuredClone(header);for(const bone of shape.bones)bone.inverseBind[12]+=1;near([fitCharacterBounds(shape,binary).bounds.max[0]],[bounds.bounds.max[0]+1]);
});
test('model scale and yaw determine fitted extents and inverse-transformed ground alignment',async t=>{
  const {header,binary}=await fixture(t),base=fitCharacterBounds(header,binary),scaled=fitCharacterBounds(header,binary,{scale:[2,2,2]}),rotated=fitCharacterBounds(header,binary,{yaw:Math.PI/2});near(scaled.dimensions,base.dimensions.map(v=>v*2));near(rotated.dimensions,[base.dimensions[2],base.dimensions[1],base.dimensions[0]]);
  near(scaled.offset,base.offset);const c=Math.cos(Math.PI/2),s=Math.sin(Math.PI/2),o=rotated.offset,delta=[c*o[0]-s*o[2],o[1],s*o[0]+c*o[2]];near(delta,[-(rotated.bounds.min[0]+rotated.bounds.max[0])/2,-rotated.bounds.min[1],-(rotated.bounds.min[2]+rotated.bounds.max[2])/2]);
});
test('attach fits players and feet-anchored NPCs, with explicit manual and center alternatives',async t=>{
  const {file,imported,header,binary}=await fixture(t),npc=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:0,collider:true}),n=npc.entities.at(-1),fit=fitCharacterBounds(header,binary);near(n.scale,fit.dimensions);assert.equal(n.character.anchor,'feet');near(n.character.offset,fit.offset);
  const playerScene=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:1,characterOptions:{replacePlayer:true,modelScale:[2,2,2],controller:{stepHeight:0,eyeHeight:2},rig:{turnInPlace:false,turnCooldown:.5}}}),p=playerScene.entities.find(e=>e.kind==='player');near(p.scale,fitCharacterBounds(header,binary,{scale:[2,2,2],mode:'humanoid'}).dimensions);assert.equal(p.character.controller.eyeHeight,2);assert.equal(p.character.rig.turnInPlace,false);assert.equal(p.character.rig.turnCooldown,.5);
  const manual=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:2,characterOptions:{fitCollider:false,offset:[1,2,3]}});near(manual.entities.at(-1).scale,[.6,1.8,.6]);near(manual.entities.at(-1).character.offset,[1,2,3]);
  const center=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:3,characterOptions:{anchor:'center'}});assert.equal(center.entities.at(-1).character.anchor,'center');assert.ok(Math.abs(center.entities.at(-1).character.offset[1]-(fit.offset[1]-fit.height/2))<.0003);
});
test('scene refitting supports revision, undo, hash guards and controller schema validation',async t=>{
  const {file,imported}=await fixture(t),scene=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}}),player=scene.entities.find(e=>e.kind==='player');
  await commitSceneUpdate(file,{projectId:scene.projectId,expectedRevision:1},next=>{next.entities.find(e=>e.id===player.id).character.scale=[3,3,3];return next;});
  const fitted=await fitSceneCharacter({scenePath:file,entityId:player.id,expectedRevision:2,mode:'humanoid'});near(fitted.scene.entities.find(e=>e.id===player.id).scale,player.scale.map(v=>v*3));
  await assert.rejects(fitSceneCharacter({scenePath:file,entityId:player.id,expectedRevision:2}),e=>e.code==='REVISION_CONFLICT');await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:3});near((await loadScene(file)).entities.find(e=>e.id===player.id).scale,player.scale);
  const changed=structuredClone(fitted.scene),p=changed.entities.find(e=>e.id===player.id);for(const controller of [{stepHeight:-1},{eyeHeight:Infinity},{orbitDistance:0},{unknown:1}]){p.character.controller=controller;assert.equal(validateScene(changed).valid,false);}
  for(const rig of [{turnThreshold:-1},{maxSlopeCosine:2},{footLock:'yes'},{unknown:1},{soleRoll:2},{maxPelvisShift:-0.1}]){p.character.controller=fitted.scene.entities.find(e=>e.id===player.id).character.controller;p.character.rig=rig;assert.equal(validateScene(changed).valid,false);}
  // The contact solver knobs are part of the instance override surface.
  { const allowed={...fitted.scene.entities.find(e=>e.id===player.id)};allowed.character={...allowed.character,rig:{soleRoll:.4,maxPelvisShift:.05}};const probe={...changed,entities:[allowed,...changed.entities.filter(e=>e.id!==player.id)]};assert.equal(validateScene(probe).valid,true); }
  await fs.appendFile(path.join(imported.directory,'character/animation.bin'),' ');await assert.rejects(fitSceneCharacter({scenePath:file,entityId:player.id,expectedRevision:4}),e=>e.code==='NG-E-ASSET-HASH');
});
test('character previews use fitted dimensions and appended previews keep verified clip references',async t=>{
  const {dir,imported,header,binary}=await fixture(t),scene=await loadScene(path.join(imported.directory,'preview.scene.json')),player=scene.entities.find(e=>e.kind==='player');near(player.scale,fitCharacterBounds(header,binary,{mode:'humanoid'}).dimensions);assert.equal(player.character.file,'character/character.json');
  const explicit=await importAsset({source:path.join(dir,'source/multi-skin.gltf'),output:path.join(dir,'assets/imported'),size:64,coordinates:{unit:'m',yawDegrees:360}});assert.equal(explicit.manifest.id,imported.manifest.id);
  const motion=await writeMotionFixture(path.join(dir,'motion')),appended=await appendAnimation({directory:imported.directory,source:motion.source});const preview=await loadScene(path.join(appended.directory,'preview.scene.json')),ref=preview.entities.find(e=>e.kind==='player').character;
  assert.equal(ref.file,'character/preview.json');assert.equal(ref.sha256,appended.manifest.files.find(f=>f.file===ref.file).sha256);assert.equal(ref.binarySha256,appended.manifest.files.find(f=>f.file==='character/animation.bin').sha256);
});
test('editor and CLI refit existing scene references through the same transaction',async t=>{
  const {dir,file,imported}=await fixture(t),scene=await attachModel({directory:imported.directory,scenePath:file,expectedRevision:0}),entity=scene.entities.at(-1),service=createEditorService({root:dir});t.after(()=>service.close());
  const result=await service.call('asset_fit_character',{scenePath:'assets/main.scene.json',entityId:entity.id,expectedRevision:1,mode:'humanoid'});assert.equal(result.ok,true,JSON.stringify(result));near(result.result.scene.entities.at(-1).scale,[result.result.fit.radius*2,result.result.fit.height,result.result.fit.radius*2]);
  const fitted=await cli(['fit-character','--scene',file,'--entity',entity.id,'--revision','2']);assert.equal(fitted.scene.revision,3);assert.equal(fitted.fit.mode,'bounds');
});
test('malformed fitting inputs reject instead of publishing invalid collider dimensions',async t=>{
  const {header,binary,file,imported}=await fixture(t);for(const options of [{mode:'capsule'},{scale:[0,1,1]},{yaw:NaN}])assert.throws(()=>fitCharacterBounds(header,binary,options));
  const corrupt=structuredClone(header);corrupt.mesh.vertices=100001;assert.throws(()=>fitCharacterBounds(corrupt,binary));assert.throws(()=>fitCharacterBounds(header,binary.subarray(0,64)));
  for(const characterOptions of [{replacePlayer:true,anchor:'center'},{controller:{stepHeight:1}},{fitCollider:'guess'}])await assert.rejects(attachModel({directory:imported.directory,scenePath:file,expectedRevision:0,characterOptions}));
});

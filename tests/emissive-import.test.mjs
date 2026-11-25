import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {importAsset,verifyImport,attachModel,attachTexture} from '../engine/assets/import-service.mjs';
import {loadScene,validateScene,buildScene,undoSceneFile} from '../engine/scene/index.mjs';
import {materialToMir,mirToMaterial,compileMir,validateMir} from '../engine/material/mir.mjs';
import {compileGraph} from '../engine/material/graph.mjs';
import {patchMaterial,materialInstance} from '../tools/material.mjs';
import {encodePng,readNgtex,decodeFirstMip} from '../tools/textures.mjs';
import {writeEmissiveFixture} from '../tools/offline/emissive-fixture.mjs';
import {writeCharacterFixture} from '../tools/asset-fixtures.mjs';
import {createEditorService} from '../editor/service.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'ng-emission-'));try{await fn(directory);}finally{await fs.rm(directory,{force:true,recursive:true});}}
test('colored emission and strength import as independent linear factors with no base-color tint or ORM leakage',()=>fixture(async dir=>{
  const source=await writeEmissiveFixture(path.join(dir,'source')),result=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(result.directory);
  for(let i=1;i<=5;i++){const material=result.manifest.materials['m'+i];assert.equal(material.emissive,'m'+i);assert.equal(material.emissiveStrength,i===5?4:1);const image=decodeFirstMip(readNgtex(await fs.readFile(path.join(result.directory,`textures/m${i}.emissive.ngtex`))));assert.ok(image[0]>250);const orm=decodeFirstMip(readNgtex(await fs.readFile(path.join(result.directory,`textures/m${i}.orm.ngtex`))));assert.equal(orm[3],0);}
  assert.deepEqual(result.manifest.materials.m1.emissiveFactor,[.4,0,0]);assert.equal(result.manifest.materials.m0.emissive,undefined);
  const build=await buildScene(path.join(result.directory,'preview.scene.json'),path.join(dir,'packages'));assert.equal(build.manifest.dependencies.filter(d=>d.path.endsWith('.emissive.ngtex')).length,5);
}));
test('character import preserves RGB emissive textures, alpha-independent emission and required strength extension',()=>fixture(async dir=>{
  const source=await writeCharacterFixture(path.join(dir,'source')),doc=JSON.parse(await fs.readFile(source));doc.images=[{uri:'data:image/png;base64,'+encodePng({width:1,height:1,data:Buffer.from([20,90,240,0])}).toString('base64')}];doc.textures=[{source:0}];doc.extensionsRequired=['KHR_materials_emissive_strength'];for(const m of doc.materials){m.emissiveFactor=[.2,.5,.8];m.emissiveTexture={index:0};m.extensions={KHR_materials_emissive_strength:{emissiveStrength:12}};}await fs.writeFile(source,JSON.stringify(doc));const result=await importAsset({source,output:path.join(dir,'assets/imported'),size:64});assert.equal(result.manifest.kind,'character');assert.equal(result.manifest.materials.m1.emissiveStrength,12);
  const image=decodeFirstMip(readNgtex(await fs.readFile(path.join(result.directory,'textures/m1.emissive.ngtex'))));assert.ok(image[2]>220&&image[0]<30);assert.equal(image[3],255);
  const file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,await fs.readFile(path.join(result.directory,'preview.scene.json')));await fs.cp(path.join(result.directory,'textures'),path.join(dir,'assets/textures'),{recursive:true});const attached=await attachModel({directory:result.directory,scenePath:file,expectedRevision:0});assert.equal(attached.materials[result.manifest.id.slice(0,12)+'_m1'].emissive,result.manifest.id.slice(0,12)+'_m1');
}));
test('standalone emission binds in the editor, packages its dependency, and shares material undo',()=>fixture(async dir=>{
  const source=path.join(dir,'emission.png');await fs.writeFile(source,encodePng({width:1,height:1,data:Buffer.from([0,20,255,255])}));const asset=await importAsset({source,semantic:'emissive',output:path.join(dir,'assets/imported'),size:64});assert.deepEqual(asset.manifest.warnings,[]);const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));assert.equal(scene.materials.m1.emissive,'texture');const file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,JSON.stringify(scene));await fs.cp(path.join(asset.directory,'textures'),path.join(dir,'assets/textures'),{recursive:true});const service=createEditorService({root:dir});try{const result=await service.call('asset_attach',{id:asset.manifest.id,scenePath:'assets/main.scene.json',expectedRevision:0,material:'m0'});assert.equal(result.ok,true,JSON.stringify(result));}finally{await service.close();}
  const attached=await loadScene(file);assert.match(attached.materials.m0.emissive,/_texture$/);const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith('_texture.emissive.ngtex')));await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal((await loadScene(file)).materials.m0.emissive,undefined);
}));
test('emission MIR and graph retain optional RGB factors, high strength and stable old material defaults',()=>{
  const material={albedo:'flat',normal:'flat',orm:'flat',emissive:'neon',emissiveFactor:[.1,.5,.9],emissiveStrength:16},mir=materialToMir('neon',material),compiled=compileMir(mir);assert.deepEqual(mirToMaterial(mir).emissiveFactor,material.emissiveFactor);assert.equal(compiled.material.emissiveStrength,16);assert.ok(compiled.features.includes('emissive-map'));const legacy=compileMir(materialToMir('old',{albedo:'flat'}));assert.equal(legacy.material.emissive,undefined);assert.equal(legacy.material.emissiveFactor,undefined);
  const graph={schema:'nativegpu.material-graph/1',name:'neon',emissiveFactor:[.1,.5,.9],nodes:[{id:'tex',type:'texture',params:{name:'neon'}},{id:'strength',type:'scalar',params:{value:16}},{id:'output',type:'output'}],links:[{from:'tex',to:'output',toPort:'emissive'},{from:'strength',to:'output',toPort:'emissiveStrength'}]};assert.equal(compileGraph(graph).compiled.material.emissiveStrength,16);const invalid=structuredClone(mir);invalid.emission.factor=[1,2,0];assert.throws(()=>validateMir(invalid),/factor/);
});
test('material edits validate emission, support disconnecting the slot and do not alias RGB arrays',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeEmissiveFixture(path.join(dir,'source')),output:path.join(dir,'imports'),size:64}),scene=await loadScene(path.join(asset.directory,'preview.scene.json')),factor=[.1,.2,.3],patch=patchMaterial(scene,'m1',{emissiveFactor:factor,emissiveStrength:100});factor[0]=1;assert.equal(patch.after.emissiveFactor[0],.1);assert.equal(patchMaterial(scene,'m1',{emissive:null}).after.emissive,undefined);
  const instance=materialInstance(scene,'m1','copy');instance.scene.materials.copy.emissiveFactor[0]=0;assert.equal(instance.scene.materials.m1.emissiveFactor[0],.4);
  for(const changes of [{emissiveFactor:[1,2,0]},{emissiveFactor:[0,0]},{emissiveStrength:Infinity},{emissiveStrength:-1},{emissiveStrength:1000001},{emissive:'../bad'}])assert.throws(()=>patchMaterial(scene,'m1',changes));const invalid=structuredClone(scene);invalid.materials.m1.emissiveFactor=[1,0,NaN];assert.equal(validateScene(invalid).valid,false);
}));
test('invalid source emission refuses publication and zero-strength materials remain editable',()=>fixture(async dir=>{
  const source=await writeEmissiveFixture(path.join(dir,'source')),doc=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports');doc.materials[0].extensions.KHR_materials_emissive_strength.emissiveStrength=1000001;await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}),/strength/);assert.deepEqual(await fs.readdir(output),[]);for(const malformed of [null,[],3]){const changed=structuredClone(doc);changed.materials[0].extensions.KHR_materials_emissive_strength=malformed;await fs.writeFile(source,JSON.stringify(changed));await assert.rejects(importAsset({source,output,size:64}),/emissive/);assert.deepEqual(await fs.readdir(output),[]);}doc.materials[0].extensions.KHR_materials_emissive_strength.emissiveStrength=0;await fs.writeFile(source,JSON.stringify(doc));assert.equal((await importAsset({source,output,size:64})).manifest.materials.m1.emissiveStrength,0);
}));

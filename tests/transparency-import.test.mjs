import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {readNgtex,decodeFirstMip} from '../tools/textures.mjs';
import {writeTransparencyFixture} from '../tools/offline/transparency-fixture.mjs';
import {importAsset,verifyImport,attachModel} from '../engine/assets/import-service.mjs';
import {loadScene,validateScene,buildScene,undoSceneFile} from '../engine/scene/index.mjs';
import {materialToMir,compileMir} from '../engine/material/mir.mjs';
import {compileGraph} from '../engine/material/graph.mjs';
import {patchMaterial,materialInstance} from '../tools/material.mjs';
import {createEditorService} from '../editor/service.mjs';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-transparency-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}

test('BLEND preserves floating factor separately from texture and vertex alpha on static and skinned imports',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const asset=await importAsset({source:await writeTransparencyFixture(path.join(dir,'source-'+character),{character,textureAlpha:128,vertexAlpha:.25,layers:[{color:[.2,.4,.6,.1234567]}]}),output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);
    for(const material of Object.values(asset.manifest.materials)){if(material.alphaMode!=='BLEND')continue;assert.equal(material.opacity,.1234567);const rgba=decodeFirstMip(readNgtex(await fs.readFile(path.join(asset.directory,'textures',material.albedo+'.albedo.ngtex'))));assert.equal(rgba[3],character?255:128);}
    if(character){const header=JSON.parse(await fs.readFile(path.join(asset.directory,'character/character.json'))),bytes=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(bytes.readFloatLE(header.mesh.colorOffset+12),.25);}
    else{const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.equal(mesh.vertices[0][19],.25);}
    const target=path.join(dir,'attached-'+character,'main.scene.json'),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.materials;scene.entities=scene.entities.filter(e=>e.kind==='player'||e.kind==='ground');for(const e of scene.entities){delete e.material;delete e.character;}await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,JSON.stringify(scene));await attachModel({directory:asset.directory,scenePath:target,expectedRevision:0});const attached=await loadScene(target);assert.ok(Object.values(attached.materials).some(m=>m.alphaMode==='BLEND'&&m.opacity===.1234567));const built=await buildScene(target,path.join(dir,'attached-packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith(character?'animation.bin':'.mesh.json')));
  }
}));

test('BLEND and opacity survive MIR, graph, material instances, editor undo and packages',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeTransparencyFixture(path.join(dir,'source')),output:path.join(dir,'assets/imported'),size:64}),work=path.join(dir,'assets/editable');await fs.cp(asset.directory,work,{recursive:true});const file=path.join(work,'preview.scene.json'),scene=await loadScene(file);
  const mir=compileMir(materialToMir('m1',scene.materials.m1));assert.equal(mir.material.opacity,.5);assert.ok(mir.features.includes('alpha-blend'));
  const graph=compileGraph({schema:'nativegpu.material-graph/1',name:'glass',alphaMode:'BLEND',nodes:[{id:'alpha',type:'scalar',params:{value:.3}},{id:'out',type:'output'}],links:[{from:'alpha',to:'out',toPort:'opacity'}]});assert.equal(graph.compiled.material.opacity,.3);
  assert.equal(materialInstance(scene,'m1','copy').scene.materials.copy.alphaMode,'BLEND');assert.equal(patchMaterial(scene,'m1',{opacity:.7}).after.opacity,.7);
  const service=createEditorService({root:dir});try{const changed=await service.call('material_patch',{scenePath:path.relative(dir,file).replaceAll('\\','/'),material:'m1',changes:{opacity:.75},expectedRevision:0,apply:true});assert.equal(changed.ok,true,JSON.stringify(changed));}finally{await service.close();}
  const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal(undone.scene.materials.m1.opacity,.5);
  const built=await buildScene(file,path.join(dir,'packages')),packed=JSON.parse(await fs.readFile(path.join(built.packageDirectory,'scene.json')));assert.equal(packed.materials.m1.alphaMode,'BLEND');assert.equal(packed.materials.m1.opacity,.5);
  for(const opacity of [-.1,1.1,null,'0.5']){const bad=structuredClone(scene);bad.materials.m1.opacity=opacity;assert.equal(validateScene(bad).valid,false);assert.throws(()=>patchMaterial(scene,'m1',{opacity}));const invalid=materialToMir('m1',scene.materials.m1);invalid.params.opacity=opacity;assert.throws(()=>compileMir(invalid));}
}));

test('OBJ MTL d and Tr import opacity in either order with Kd, rejecting malformed values',()=>fixture(async dir=>{
  const source=path.join(dir,'panel.obj');await fs.writeFile(source,'mtllib panel.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl glass\nf 1 2 3\n');
  for(const lines of ['d .25\nKd .2 .4 .6','Kd .2 .4 .6\nTr .75','Tr .75 # transparency\nKd .2 .4 .6']){await fs.writeFile(path.join(dir,'panel.mtl'),'newmtl glass\n'+lines+'\n');const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.equal(asset.manifest.materials.m1.alphaMode,'BLEND');assert.equal(asset.manifest.materials.m1.opacity,.25);}
  for(const value of ['-1','1.1','nan','-halo .5']){await fs.writeFile(path.join(dir,'panel.mtl'),'newmtl glass\nd '+value+'\n');await assert.rejects(importAsset({source,output:path.join(dir,'bad'),size:64}),/opacity/);assert.deepEqual(await fs.readdir(path.join(dir,'bad')),[]);}
}));

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {MeshoptEncoder} from 'meshoptimizer/encoder';
import {writeVertexColorFixture} from '../tools/offline/vertex-color-fixture.mjs';
import {writeMotionFixture} from '../tools/asset-fixtures.mjs';
import {readGltf,readAccessor} from '../engine/assets/gltf-document.mjs';
import {importAsset,verifyImport,attachModel,appendAnimation} from '../engine/assets/import-service.mjs';
import {loadScene,buildScene,validateScene,undoSceneFile} from '../engine/scene/index.mjs';
import {compileMir,materialToMir} from '../engine/material/mir.mjs';
import {compileGraph} from '../engine/material/graph.mjs';
import {createEditorService} from '../editor/service.mjs';
import {patchMaterial,materialInstance} from '../tools/material.mjs';

async function fixture(fn){const directory=await fs.mkdtemp(path.join(os.tmpdir(),'ng-color-unlit-'));try{await fn(directory);}finally{await fs.rm(directory,{recursive:true,force:true});}}

test('static RGB/RGBA and normalized byte/short vertex colors remain linear, duplicate back faces and retain alpha coverage',()=>fixture(async dir=>{
  for(const rgb of [true,false])for(const componentType of [5121,5123,5126]){
    const source=await writeVertexColorFixture(path.join(dir,`${rgb}-${componentType}`),{rgb,componentType,masked:true}),input=await readGltf(source),asset=await importAsset({source,output:path.join(dir,'imports'),size:64});await verifyImport(asset.directory);
    assert.equal(asset.manifest.materials.m1.unlit,true);assert.equal(asset.manifest.materials.m1.alphaMode,'MASK');const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh))),primitive=input.document.meshes[0].primitives[0],values=readAccessor(input,primitive.attributes.COLOR_0);
    assert.equal(mesh.schema,'nativegpu.mesh/3');assert.equal(mesh.vertices[0].length,20);assert.equal(mesh.vertices.length,values.length*2);
    for(let i=0;i<values.length;i++){const expected=rgb?[...values[i],1]:values[i];assert.deepEqual(mesh.vertices[i].slice(16),expected);assert.deepEqual(mesh.vertices[i+values.length].slice(16),expected);}
  }
}));

test('colored characters retain independent UVs, morphs, color sidecars, fitting and appended motions in scene packages',()=>fixture(async dir=>{
  const source=await writeVertexColorFixture(path.join(dir,'source'),{character:true,multiUv:true,unlit:false,componentType:5123}),input=await readGltf(source),asset=await importAsset({source,output:path.join(dir,'assets/imported'),size:64}),header=JSON.parse(await fs.readFile(path.join(asset.directory,'character/character.json'))),binary=await fs.readFile(path.join(asset.directory,'character/animation.bin')),values=readAccessor(input,input.document.meshes[0].primitives[0].attributes.COLOR_0);
  assert.equal(header.mesh.colorOffset,header.mesh.textureUvOffset+header.mesh.textureUvBytes);assert.equal(header.mesh.colorBytes,header.mesh.vertices*16);assert.equal(header.mesh.tangentOffset,header.mesh.colorOffset+header.mesh.colorBytes);assert.equal(header.mesh.tangentBytes,header.mesh.vertices*16);assert.equal(header.mesh.indexOffset,header.mesh.vertices*128);assert.ok(header.morphs.length>0);
  for(let i=0;i<values.length;i++)for(let k=0;k<4;k++)assert.equal(binary.readFloatLE(header.mesh.colorOffset+i*16+k*4),Math.fround(values[i][k]));
  const motion=await writeMotionFixture(path.join(dir,'motion')),appended=await appendAnimation({directory:asset.directory,source:motion.source,output:path.join(dir,'assets/imported')}),newHeader=JSON.parse(await fs.readFile(path.join(appended.directory,'character/character.json'))),newBinary=await fs.readFile(path.join(appended.directory,'character/animation.bin'));assert.deepEqual(newHeader.mesh,header.mesh);assert.deepEqual(newBinary.subarray(0,header.mesh.indexOffset),binary.subarray(0,header.mesh.indexOffset));
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.materials;for(const e of scene.entities){delete e.material;delete e.character;}const file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,JSON.stringify(scene));const attached=await attachModel({directory:appended.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});assert.ok(attached.entities[0].scale.every(v=>v>0));const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.endsWith('animation.bin')));
}));

test('unlit ignores PBR fallback maps, strengths and unused coordinate requirements while retaining base color mappings',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const source=await writeVertexColorFixture(path.join(dir,'source-'+character),{character}),doc=JSON.parse(await fs.readFile(source));doc.images=[{uri:'unused-fallback.png'}];doc.textures=[{source:0}];for(const material of doc.materials){material.normalTexture={index:0,texCoord:999};material.occlusionTexture={index:0,texCoord:999,strength:.3};material.emissiveTexture={index:0,texCoord:999};material.emissiveFactor=[1,1,1];material.pbrMetallicRoughness.metallicRoughnessTexture={index:0,texCoord:999};material.extensions.KHR_materials_clearcoat={clearcoatTexture:{index:0}};}await fs.writeFile(source,JSON.stringify(doc));
    const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});for(const mat of Object.values(asset.manifest.materials))if(mat.unlit){assert.equal(mat.emissive,undefined);assert.equal(mat.occlusion,undefined);assert.equal(mat.textureMappings,undefined);}assert.ok(!(asset.manifest.dependencies??[]).some(d=>String(d).includes('unused-fallback')));
  }
}));

test('unlit boolean survives material editing, MIR, graph, instances, editor undo and dependency packaging',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeVertexColorFixture(path.join(dir,'source')),output:path.join(dir,'assets/imported'),size:64}),file=path.join(dir,'assets/main.scene.json'),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));await fs.cp(path.join(asset.directory,'textures'),path.join(dir,'assets/textures'),{recursive:true});await fs.cp(path.join(asset.directory,'meshes'),path.join(dir,'assets/meshes'),{recursive:true});await fs.writeFile(file,JSON.stringify(scene));
  assert.ok(compileMir(materialToMir('m1',scene.materials.m1)).features.includes('unlit'));assert.equal(compileMir(materialToMir('m1',{albedo:'flat',unlit:false})).material.unlit,false);assert.equal(compileGraph({schema:'nativegpu.material-graph/1',name:'unlit',unlit:true,nodes:[{id:'out',type:'output'}],links:[]}).compiled.material.unlit,true);assert.equal(materialInstance(scene,'m1','copy').scene.materials.copy.unlit,true);assert.equal(patchMaterial(scene,'m1',{unlit:false}).after.unlit,false);
  const service=createEditorService({root:dir});try{const applied=await service.call('material_patch',{scenePath:'assets/main.scene.json',material:'m1',changes:{unlit:false},expectedRevision:0,apply:true});assert.equal(applied.ok,true,JSON.stringify(applied));}finally{await service.close();}const undone=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal(undone.scene.materials.m1.unlit,true);const built=await buildScene(file,path.join(dir,'packages'));assert.equal(JSON.parse(await fs.readFile(path.join(built.packageDirectory,'scene.json'))).materials.m1.unlit,true);
  for(const value of [null,0,'true']){assert.throws(()=>patchMaterial(scene,'m1',{unlit:value}));const bad=structuredClone(scene);bad.materials.m1.unlit=value;assert.equal(validateScene(bad).valid,false);assert.throws(()=>compileMir({...materialToMir('m1',scene.materials.m1),unlit:value}));}
}));

test('invalid color types, ranges, counts and unlit extension objects reject before publishing',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const source=await writeVertexColorFixture(path.join(dir,'source-'+character),{character}),original=JSON.parse(await fs.readFile(source)),output=path.join(dir,'imports-'+character),colorIndex=original.meshes[0].primitives[0].attributes.COLOR_0;
    for(const edit of [doc=>doc.accessors[colorIndex].count--,doc=>doc.accessors[colorIndex].type='VEC2',doc=>doc.accessors[colorIndex].componentType=5122,doc=>doc.accessors[colorIndex].normalized=true,doc=>doc.accessors[colorIndex].normalized=0,doc=>doc.materials[0].extensions.KHR_materials_unlit=null,doc=>doc.materials[0].extensions.KHR_materials_unlit=[]]){const doc=structuredClone(original);edit(doc);await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}));assert.deepEqual(await fs.readdir(output),[]);}
    const doc=structuredClone(original),view=doc.bufferViews[doc.accessors[colorIndex].bufferView],bytes=Buffer.from(doc.buffers[view.buffer].uri.split(',')[1],'base64');bytes.writeFloatLE(1.1,0);doc.buffers[view.buffer].uri='data:application/octet-stream;base64,'+bytes.toString('base64');await fs.writeFile(source,JSON.stringify(doc));await assert.rejects(importAsset({source,output,size:64}),/color/);assert.deepEqual(await fs.readdir(output),[]);
  }
}));

test('meshopt compressed normalized vertex colors retain exact channels on static meshes and characters',()=>fixture(async dir=>{
  await MeshoptEncoder.ready;
  for(const character of [false,true]){
    const source=await writeVertexColorFixture(path.join(dir,'source-'+character),{character,componentType:5121}),doc=JSON.parse(await fs.readFile(source)),index=doc.meshes[0].primitives[0].attributes.COLOR_0,spec=doc.accessors[index],view=doc.bufferViews[spec.bufferView],input=Buffer.from(doc.buffers[view.buffer].uri.split(',')[1],'base64'),compressed=Buffer.from(MeshoptEncoder.encodeGltfBuffer(input,spec.count,4,'ATTRIBUTES')),buffer=doc.buffers.length,newView=doc.bufferViews.length;
    doc.buffers.push({byteLength:compressed.length,uri:'data:application/octet-stream;base64,'+compressed.toString('base64')},{byteLength:input.length,extensions:{EXT_meshopt_compression:{fallback:true}}});doc.bufferViews.push({buffer:buffer+1,byteLength:input.length,byteStride:4,extensions:{EXT_meshopt_compression:{buffer,byteLength:compressed.length,byteStride:4,count:spec.count,mode:'ATTRIBUTES'}}});spec.bufferView=newView;doc.extensionsRequired.push('EXT_meshopt_compression');await fs.writeFile(source,JSON.stringify(doc));
    const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});
    if(character){const header=JSON.parse(await fs.readFile(path.join(asset.directory,'character/character.json'))),binary=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(binary.readFloatLE(header.mesh.colorOffset+8),Math.fround(input[2]/255));}
    else{const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.deepEqual(mesh.vertices[0].slice(16),Array.from(input.subarray(0,4),v=>v/255));}
  }
}));

test('sparse normalized RGB colors preserve zero fill, overrides and implicit opaque alpha',()=>fixture(async dir=>{
  for(const character of [false,true]){
    const source=await writeVertexColorFixture(path.join(dir,'source-'+character),{character,rgb:true,componentType:5121}),doc=JSON.parse(await fs.readFile(source)),index=doc.meshes[0].primitives[0].attributes.COLOR_0,spec=doc.accessors[index],buffer=doc.buffers.length,view=doc.bufferViews.length,bytes=Buffer.from([1,0,0,0,64,128,192]);
    doc.buffers.push({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')});doc.bufferViews.push({buffer,byteLength:1},{buffer,byteOffset:4,byteLength:3});delete spec.bufferView;spec.sparse={count:1,indices:{bufferView:view,componentType:5121},values:{bufferView:view+1}};await fs.writeFile(source,JSON.stringify(doc));
    const asset=await importAsset({source,output:path.join(dir,'imports'),size:64});
    if(character){const header=JSON.parse(await fs.readFile(path.join(asset.directory,'character/character.json'))),binary=await fs.readFile(path.join(asset.directory,'character/animation.bin'));assert.equal(binary.readFloatLE(header.mesh.colorOffset),0);assert.equal(binary.readFloatLE(header.mesh.colorOffset+12),1);assert.equal(binary.readFloatLE(header.mesh.colorOffset+16),Math.fround(64/255));}
    else{const mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.deepEqual(mesh.vertices[0].slice(16),[0,0,0,1]);assert.deepEqual(mesh.vertices[1].slice(16),[64/255,128/255,192/255,1]);}
  }
}));

test('OBJ XYZ RGB vertex colors retain linear channels, negative indices and white defaults for mixed vertices',()=>fixture(async dir=>{
  const source=path.join(dir,'colors.obj');await fs.writeFile(source,'v 0 0 0 1 0 0\nv 1 0 0 0 .5 0\nv 0 1 0\nf -3 -2 -1\n');const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),mesh=JSON.parse(await fs.readFile(path.join(asset.directory,asset.manifest.parts[0].mesh)));assert.equal(mesh.schema,'nativegpu.mesh/3');assert.deepEqual(mesh.vertices.map(v=>v.slice(16)),[[1,0,0,1],[0,.5,0,1],[1,1,1,1]]);
  await fs.writeFile(source,'v 0 0 0 1.1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n');await assert.rejects(importAsset({source,output:path.join(dir,'invalid'),size:64}),/color/);assert.deepEqual(await fs.readdir(path.join(dir,'invalid')),[]);
}));

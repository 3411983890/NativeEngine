import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {writeAssetFixture,writeCharacterFixture} from '../asset-fixtures.mjs';
import {writeAudioFixture} from './audio-fixture.mjs';
import {importAsset,attachModel,attachAudio} from '../../engine/assets/import-service.mjs';
import {loadScene,commitSceneUpdate} from '../../engine/scene/index.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
export async function writeSceneFixture(directory,{marker=.35,fracture=false,importDirectory}={}) {
  await fs.mkdir(path.join(directory,'gameplay'),{recursive:true});await fs.mkdir(path.join(directory,'scripts/vendor'),{recursive:true});
  await fs.copyFile(path.join(root,'assets/gameplay/components.json'),path.join(directory,'gameplay/components.json'));
  const descriptor={apiVersion:1,version:1,update:true,dependencies:['vendor/library'],properties:{type:'object',properties:{marker:{type:'number',default:marker}},additionalProperties:false},state:{type:'object',properties:{},additionalProperties:false}};
  await fs.writeFile(path.join(directory,'scripts/vendor/move.behavior.json'),JSON.stringify(descriptor));
  await fs.writeFile(path.join(directory,'scripts/vendor/move.lua'),"return {on_update=function(ctx) ctx:set(ctx.entity,'renderable',{offset={0,ctx.properties.marker,0}}) end}\n");
  await fs.writeFile(path.join(directory,'scripts/vendor/library.behavior.json'),JSON.stringify({...descriptor,dependencies:[],update:false}));await fs.writeFile(path.join(directory,'scripts/vendor/library.lua'),'return {}\n');
  const out=importDirectory??path.join(directory,'imported'),character=await importAsset({source:await writeCharacterFixture(path.join(directory,'source-character')),output:out,size:64});
  const scene=await loadScene(path.join(character.directory,'preview.scene.json'));delete scene.materials;for(const entity of scene.entities){delete entity.character;delete entity.material;}
  const file=path.join(directory,'external.scene.json');await fs.writeFile(file,JSON.stringify(scene));
  await attachModel({directory:character.directory,scenePath:file,expectedRevision:0,characterOptions:{replacePlayer:true}});
  const model=await importAsset({source:await writeAssetFixture(path.join(directory,'source-model')),output:out,size:64});await attachModel({directory:model.directory,scenePath:file,expectedRevision:1});
  const audio=await importAsset({source:await writeAudioFixture(path.join(directory,'tone.wav')),output:out});await attachAudio({directory:audio.directory,scenePath:file,expectedRevision:2,options:{loop:true,spatial:false,bus:'music'}});
  const current=await loadScene(file);await commitSceneUpdate(file,{projectId:current.projectId,expectedRevision:3},next=>{next.entities.at(-1).components={behavior:{instances:[{id:'vendor',script:'vendor/move',properties:{marker}}]}};return next;});
  if(fracture){const authored=JSON.parse(await fs.readFile(path.join(root,'.screen/bonded-wall.scene.json'))),wall=authored.entities.find(e=>e.destructible?.voronoi);wall.position=[7,1.6,-3];await fs.mkdir(path.join(directory,'voronoi'),{recursive:true});await fs.copyFile(path.join(root,'assets',wall.destructible.voronoi),path.join(directory,wall.destructible.voronoi));await commitSceneUpdate(file,{projectId:current.projectId,expectedRevision:4},next=>{next.entities.push(wall);return next;});}
  return file;
}

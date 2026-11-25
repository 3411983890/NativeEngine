#!/usr/bin/env node
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs/promises';
import {reimportAsset} from '../engine/assets/reimport.mjs';
import {updateSceneImport} from '../engine/assets/reimport-scene.mjs';
import { ASSET_CAPABILITIES, importAsset, inspectAsset, listImports, verifyImport, attachModel, attachAudio, attachTexture, attachEnvironment, appendAnimation, fitSceneCharacter, checkoutSceneImport } from '../engine/assets/import-service.mjs';

export async function main(args) {
  const [command,...rest]=args,options={};
  for(let i=0;i<rest.length;i+=2){if(!rest[i].startsWith('--')||rest[i+1]===undefined||Object.hasOwn(options,rest[i]))throw new Error('Expected unique --option value pairs');options[rest[i]]=rest[i+1];}
  // `--size auto` (the default) keeps authored texture resolution; a number caps
  // the longest edge. `parseSize` keeps the string form intact for the validator.
  const parseSize=value=>/^(auto|source|0)$/i.test(String(value))?'auto':Number(value);
  const allowed=['--in','--out','--size','--semantic','--kind','--scene','--revision','--collider','--target','--options','--prefix','--entity','--material'];
  if(Object.keys(options).some(k=>!allowed.includes(k)))throw new Error('Unknown asset option');
  if(command==='capabilities')return ASSET_CAPABILITIES;
  if(command==='inspect')return inspectAsset(options['--in']);
  if(command==='list')return listImports(options['--out']);
  if(command==='verify')return verifyImport(options['--in']);
  if(command==='reimport')return reimportAsset({directory:options['--target'],source:options['--in'],output:options['--out'],...(options['--size']!==undefined?{size:parseSize(options['--size'])}:{})});
  if(command==='update')return updateSceneImport({previousDirectory:options['--target'],directory:options['--in'],scenePath:options['--scene'],expectedRevision:Number(options['--revision']),...(options['--entity']?{entityIds:[options['--entity']]}:{})});
  if(command==='open-scene')return checkoutSceneImport({directory:options['--in'],outputDirectory:options['--out']??'assets/scenes'});
  if(command==='append-animation') {
    let motion={};if(options['--options']){const bytes=await fs.readFile(options['--options']);if(bytes.length>65536)throw new Error('Motion options exceed 64 KiB');motion=JSON.parse(bytes);}
    if(options['--prefix']!==undefined)motion={...motion,prefix:options['--prefix']};
    return appendAnimation({directory:options['--target'],source:options['--in'],output:options['--out'],options:motion});
  }
  if(command==='attach') {
    if((await verifyImport(options['--in'])).kind==='texture')return attachTexture({directory:options['--in'],scenePath:options['--scene'],expectedRevision:Number(options['--revision']),material:options['--material']});
    let characterOptions={};if(options['--options']){const bytes=await fs.readFile(options['--options']);if(bytes.length>65536)throw new Error('Character options exceed 64 KiB');characterOptions=JSON.parse(bytes);}
    if((await verifyImport(options['--in'])).kind==='audio')return attachAudio({directory:options['--in'],scenePath:options['--scene'],expectedRevision:Number(options['--revision']),options:characterOptions});
    if((await verifyImport(options['--in'])).kind==='environment')return attachEnvironment({directory:options['--in'],scenePath:options['--scene'],expectedRevision:Number(options['--revision']),options:characterOptions});
    return attachModel({directory:options['--in'],scenePath:options['--scene'],expectedRevision:Number(options['--revision']),collider:options['--collider']==='true',characterOptions});
  }
  if(command==='fit-character') {
    let fit={};if(options['--options']){const bytes=await fs.readFile(options['--options']);if(bytes.length>65536)throw new Error('Fit options exceed 64 KiB');fit=JSON.parse(bytes);}
    if(!fit||typeof fit!=='object'||Array.isArray(fit)||Object.keys(fit).some(k=>!['mode','alignModel'].includes(k)))throw new Error('Unknown fit option');
    return fitSceneCharacter({scenePath:options['--scene'],entityId:options['--entity'],expectedRevision:Number(options['--revision']),...fit});
  }
  if(command==='import') {
    let extra={};if(options['--options']){const bytes=await fs.readFile(options['--options']);if(bytes.length>65536)throw new Error('Import options exceed 64 KiB');extra=JSON.parse(bytes);}
    if(!extra||typeof extra!=='object'||Array.isArray(extra)||Object.keys(extra).some(k=>!['coordinates','size','semantic','kind'].includes(k)))throw new Error('Unknown import option');
    return importAsset({...extra,source:options['--in'],output:options['--out'],size:options['--size']===undefined?(extra.size===undefined?'auto':extra.size):parseSize(options['--size']),semantic:options['--semantic']??extra.semantic,kind:options['--kind']??extra.kind});
  }
  throw new Error('Use assets import --in source [--size auto|16..4096] [--semantic albedo|normal|orm|emissive|occlusion|coat|coatRoughness|coatNormal|height|mask|roughness|metallic] | reimport --in updatedSource --target previousPackage | update --in newPackage --target previousPackage --scene scene --revision N [--entity UUID] | append-animation --in motion --target characterPackage [--options file.json] | inspect | list | verify | open-scene --in package --out assets/scenes | attach --in package --scene scene --revision N [--material materialName] | fit-character --scene scene --entity UUID --revision N | capabilities');
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))main(process.argv.slice(2)).then(result=>console.log(JSON.stringify({ok:true,result}))).catch(error=>{console.error(JSON.stringify({ok:false,error:{code:error.code??'NG-E-ASSET',message:error.message}}));process.exitCode=1;});

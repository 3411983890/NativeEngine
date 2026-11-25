import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {randomUUID} from 'node:crypto';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {sha256Hex} from '../engine/assets/canonical.mjs';
import {bindAssetHostScope} from './asset-host-scope.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),run=promisify(execFile);
async function files(directory){const result=[];for(const item of await fs.readdir(path.join(root,directory),{withFileTypes:true})){const name=directory+'/'+item.name;if(item.isDirectory())result.push(...await files(name));else if(item.isFile())result.push(name);else throw Error('Unexpected linked native source: '+name);}return result;}
export async function captureAssetHostSource(){
  const id=randomUUID(),sourceRoot=path.join(root,'build/asset-host-inputs',id),directory=path.join(root,'build/assets-host','frozen-'+id);await fs.mkdir(sourceRoot,{recursive:true});
  const names=(await Promise.all(['host','renderer','engine/native','engine/audio','third_party','shaders','tests'].map(files))).flat();for(const name of await fs.readdir(path.join(root,'tools')))if(name.endsWith('.ps1'))names.push('tools/'+name);names.push('VERSION');names.sort();const hashes={};
  for(const name of names){const bytes=await fs.readFile(path.join(root,name)),target=path.join(sourceRoot,name);await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,bytes,{flag:'wx'});hashes[name]=sha256Hex(bytes);}
  // Reject a copy assembled while another task changed any captured input.
  // The later build retains its own complete before/after source hash checks.
  for(const name of names)if(sha256Hex(await fs.readFile(path.join(root,name)))!==hashes[name])throw Error('Source changed while capturing asset host inputs: '+name);
  const provenance={origin:root,sourceRoot,directory,files:hashes};await fs.writeFile(path.join(sourceRoot,'source-snapshot.json'),JSON.stringify(provenance,null,2)+'\n');await fs.symlink(path.join(root,'.tools'),path.join(sourceRoot,'.tools'),'junction');return provenance;
}
export async function buildFrozenAssetHost({capture}={}){
  capture??=await captureAssetHostSource();const {sourceRoot,directory}=capture;await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File',path.join(sourceRoot,'tools/build-effects-host.ps1'),'-OutputDirectory',path.relative(sourceRoot,directory)],{cwd:sourceRoot,windowsHide:true,timeout:900000,maxBuffer:32768});
  await fs.copyFile(path.join(sourceRoot,'source-snapshot.json'),path.join(directory,'source-snapshot.json'));const context=await bindAssetHostScope(directory);if(context.sourceRoot!==sourceRoot)throw Error('Frozen asset host points to a different source tree');await context.assertHeaders();return context;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))buildFrozenAssetHost().then(c=>console.log(JSON.stringify({directory:c.directory,sourceRoot:c.sourceRoot,...c.fingerprint}))).catch(e=>{console.error(e.stderr||e.stack);process.exitCode=1;});

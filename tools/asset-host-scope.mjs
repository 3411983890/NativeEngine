import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {sha256Hex} from '../engine/assets/canonical.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),run=promisify(execFile);
export async function prepareAssetHost(){
  // A top-level check must begin from a host matching the current inputs. Nested
  // cases then stay on this exact artifact; parallel work can keep editing source.
  await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory','build/assets-host','-SkipBuild'],{cwd:root,windowsHide:true,timeout:60000,maxBuffer:32768});
  return bindAssetHostScope(path.join(root,'build/assets-host'));
}
export async function bindAssetHostScope(directory){
  const executable=path.join(directory,'NativeGPU.exe'),manifest=path.join(directory,'host-inputs.json'),bytes=await fs.readFile(manifest),inputs=JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/,'')),fingerprint={binarySha256:sha256Hex(await fs.readFile(executable)),inputsSha256:sha256Hex(bytes)};
  if(fingerprint.binarySha256!==inputs.binaryHash.toLowerCase())throw Error('Asset host binary differs from checked inputs');
  const main=Object.keys(inputs.sources).find(file=>file.replaceAll('\\','/').endsWith('/host/main.cpp')),sourceRoot=main?path.dirname(path.dirname(main)):root;
  const assertStable=async()=>{if(sha256Hex(await fs.readFile(executable))!==fingerprint.binarySha256||sha256Hex(await fs.readFile(manifest))!==fingerprint.inputsSha256)throw Error('Asset host artifact changed during the verification scope');};
  const assertSources=async()=>{await assertStable();for(const [file,hash]of Object.entries(inputs.sources))if(sha256Hex(await fs.readFile(file))!==hash.toLowerCase())throw Error('Asset host source differs from checked inputs: '+file);};
  await assertSources();
  // Native contracts link cached host objects. Recheck their source-tree headers
  // so each contract keeps the ABI of the exact host snapshot.
  const assertHeaders=async()=>{await assertStable();for(const [file,hash]of Object.entries(inputs.sources))if(/\.(h|hpp)$/i.test(file)&&sha256Hex(await fs.readFile(file))!==hash.toLowerCase())throw Error('Asset host header changed before native contract compilation: '+file);};
  // Capture current tests separately; engine includes use the host source tree,
  // including a fixed snapshot of a concurrently edited worktree.
  const contracts=[];
  const contractSource=async file=>{
    if(!/^tests\/native_[a-z0-9_]+\.cpp$/.test(file))throw Error('Invalid native contract source');
    await assertHeaders();const contractRoot=path.join(directory,'contracts',randomUUID());await fs.mkdir(path.join(contractRoot,'tests'),{recursive:true});
    // Tests may evolve after the native source snapshot was captured. Freeze the
    // current contract separately, while all engine includes resolve to the
    // exact host source tree through directory junctions.
    const hashes={},names=[file,...(await fs.readdir(path.join(root,'tests'))).filter(n=>/\.(h|hpp)$/.test(n)).map(n=>'tests/'+n)];
    for(const name of names){const bytes=await fs.readFile(path.join(root,name));hashes[name]=sha256Hex(bytes);await fs.writeFile(path.join(contractRoot,name),bytes,{flag:'wx'});}
    for(const name of names)if(sha256Hex(await fs.readFile(path.join(root,name)))!==hashes[name])throw Error('Native contract changed during capture: '+name);
    for(const name of ['engine','host','renderer','third_party'])if(await fs.stat(path.join(sourceRoot,name)).then(s=>s.isDirectory()).catch(e=>{if(e.code==='ENOENT')return false;throw e;}))await fs.symlink(path.join(sourceRoot,name),path.join(contractRoot,name),'junction');
    const record={file,sourceRoot,contractRoot,files:hashes};await fs.writeFile(path.join(contractRoot,'contract-inputs.json'),JSON.stringify(record,null,2)+'\n');contracts.push(record);return path.join(contractRoot,file);
  };
  return {directory,executable,fingerprint,sourceRoot,contractSource,contracts,assertStable,assertHeaders,assertSources};
}

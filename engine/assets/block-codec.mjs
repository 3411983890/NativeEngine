import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawn,execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {randomUUID} from 'node:crypto';
import {sha256Hex,canonicalJson} from './canonical.mjs';
import {requireAsset,AssetError} from './gltf-document.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..'),run=promisify(execFile);let pending;
async function buildDecoder(){
  const compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
  const inputs=['tools/texture-block-decoder.cpp','third_party/bcdec/bcdec.h','third_party/bcdec/source.json'],sources=[];
  for(const name of inputs)sources.push({file:name,sha256:sha256Hex(await fs.readFile(path.join(root,name)))});
  let compilerBytes;try{compilerBytes=await fs.readFile(compiler);}catch(e){if(e.code==='ENOENT')throw new AssetError('CONVERT','BC texture decoding needs the project LLVM toolchain. Run tools/bootstrap.ps1 once, then import again.');throw e;}
  const compilerHash=sha256Hex(compilerBytes),flags=['-std=c++17','-O2','-static'],key=sha256Hex(canonicalJson({sources,compilerHash,flags})),parent=path.join(root,'build/assets-host/texture-codec'),target=path.join(parent,key),exe=path.join(target,'decoder.exe');
  const verify=async()=>{const manifest=JSON.parse(await fs.readFile(path.join(target,'inputs.json'),'utf8'));requireAsset(manifest.key===key&&sha256Hex(await fs.readFile(exe))===manifest.binaryHash,'Texture decoder cache differs','HASH');return exe;};
  try{return await verify();}catch(e){if(e.code!=='ENOENT')throw e;}
  await fs.mkdir(parent,{recursive:true});const staging=path.join(parent,'.build-'+randomUUID());await fs.mkdir(staging);
  try{await run(compiler,[...flags,path.join(root,inputs[0]),'-o',path.join(staging,'decoder.exe')],{cwd:root,windowsHide:true,timeout:120000,maxBuffer:32768});await fs.writeFile(path.join(staging,'inputs.json'),JSON.stringify({key,sources,compilerHash,binaryHash:sha256Hex(await fs.readFile(path.join(staging,'decoder.exe')))}));
    try{await fs.rename(staging,target);}catch(e){if(!['EEXIST','ENOTEMPTY','EPERM'].includes(e.code))throw e;}return await verify();
  }finally{await fs.rm(staging,{recursive:true,force:true});}
}
export async function decodeBlocks(bytes,width,height,format){
  pending??=buildDecoder().catch(e=>{pending=undefined;throw e;});const exe=await pending,expected=width*height*(format%10===6?16:4);
  requireAsset(Number.isSafeInteger(expected)&&expected>0&&expected<=256*1024*1024,'Decoded block texture exceeds 256 MiB','BUDGET');
  return new Promise((resolve,reject)=>{
    const child=spawn(exe,[String(format),String(width),String(height)],{windowsHide:true,shell:false,stdio:['pipe','pipe','pipe']}),chunks=[];let count=0,error='';
    const timer=setTimeout(()=>{child.kill();reject(new AssetError('CONVERT','Texture block decode exceeded 120 seconds'));},120000);
    child.stdout.on('data',b=>{count+=b.length;if(count>expected){child.kill();return;}chunks.push(b);});child.stderr.on('data',b=>error=(error+b).slice(-8192));child.stdin.on('error',()=>{});
    child.once('error',e=>{clearTimeout(timer);reject(e);});child.once('close',code=>{clearTimeout(timer);if(code===0&&count===expected)resolve(Buffer.concat(chunks));else reject(new AssetError('CONVERT',error||'Texture block decode failed'));});child.stdin.end(bytes);
  });
}

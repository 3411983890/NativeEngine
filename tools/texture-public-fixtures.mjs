import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {sha256Hex} from '../engine/assets/canonical.mjs';
import {requireAsset} from '../engine/assets/gltf-document.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
export async function publicTextureFixtures({download=false}={}){
  const manifest=JSON.parse(await fs.readFile(path.join(root,'tests/fixtures/texture-public.json'),'utf8')),directory=path.join(root,'build/asset-texture-fixtures/public');await fs.mkdir(directory,{recursive:true});
  const fetchFile=async(file,url,hash,size)=>{const target=path.join(directory,file);let bytes=await fs.readFile(target).catch(e=>{if(e.code!=='ENOENT')throw e;return null;});
    if(!bytes&&download){const response=await fetch(url,{signal:AbortSignal.timeout(60000)});requireAsset(response.ok,'Public texture download failed','DOWNLOAD');bytes=Buffer.from(await response.arrayBuffer());requireAsset(bytes.length<=1024*1024&&sha256Hex(bytes)===hash&&(!size||bytes.length===size),'Public texture download hash differs','HASH');await fs.writeFile(target,bytes,{flag:'wx'});}
    requireAsset(bytes&&sha256Hex(bytes)===hash&&(!size||bytes.length===size),'Public texture fixture is missing/changed; run with --download once','HASH');return target;
  };
  const files=[];for(const record of manifest.files)files.push({...record,path:await fetchFile(record.file,record.url,record.sha256,record.bytes)});await fetchFile('LICENSE',manifest.licenseUrl,manifest.licenseSha256);return {directory,manifest,files};
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))publicTextureFixtures({download:process.argv.includes('--download')}).then(v=>console.log(JSON.stringify({success:true,directory:v.directory,files:v.files.length,revision:v.manifest.commit}))).catch(e=>{console.error(e.message);process.exitCode=1;});

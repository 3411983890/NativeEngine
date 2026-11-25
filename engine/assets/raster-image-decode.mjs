import {spawn} from 'node:child_process';
import ffmpeg from 'ffmpeg-static';
import {requireAsset,AssetError} from './gltf-document.mjs';
const tga=b=>b.length>=18&&b[1]<=1&&[1,2,3,9,10,11].includes(b[2])&&[8,15,16,24,32].includes(b[16]);
export const isLegacyRaster=(bytes,extension='')=>bytes.subarray(0,2).toString('ascii')==='BM'||/tga|targa|bmp/i.test(extension)||tga(bytes);
export async function decodeLegacyRaster(bytes,{extension=''}={}) {
  requireAsset(bytes.length<=256*1024*1024,'Raster source exceeds 256 MiB','BUDGET');let width,height,codec;
  if(bytes.subarray(0,2).toString('ascii')==='BM'){
    requireAsset(bytes.length>=26,'Truncated BMP header');const header=bytes.readUInt32LE(14);requireAsset([12,40,52,56,64,108,124].includes(header)&&bytes.length>=14+header,'Unsupported BMP header');width=header===12?bytes.readUInt16LE(18):bytes.readInt32LE(18);height=header===12?bytes.readUInt16LE(20):Math.abs(bytes.readInt32LE(22));codec='bmp';
  }else{requireAsset(tga(bytes),'Invalid TGA header');width=bytes.readUInt16LE(12);height=bytes.readUInt16LE(14);codec='targa';}
  const size=width*height*4;requireAsset(width>0&&height>0&&width<=16384&&height<=16384&&size<=256*1024*1024,'Raster decoded dimensions exceed 256 MiB','BUDGET');requireAsset(ffmpeg,'FFmpeg raster decoder is unavailable','CONVERT');
  const data=await new Promise((resolve,reject)=>{const child=spawn(ffmpeg,['-nostdin','-hide_banner','-loglevel','error','-f','image2pipe','-c:v',codec,'-frame_size',String(bytes.length),'-i','pipe:0','-frames:v','1','-f','rawvideo','-pix_fmt','rgba','pipe:1'],{windowsHide:true,shell:false,stdio:['pipe','pipe','pipe']}),chunks=[];let count=0,error='';const timer=setTimeout(()=>{child.kill();reject(new AssetError('CONVERT','Raster decode exceeded 120 seconds'));},120000);
    child.stdout.on('data',chunk=>{count+=chunk.length;if(count>size){child.kill();return;}chunks.push(chunk);});child.stderr.on('data',b=>error=(error+b).slice(-8192));child.stdin.on('error',()=>{});child.once('error',e=>{clearTimeout(timer);reject(e);});child.once('close',code=>{clearTimeout(timer);if(code===0&&count===size)resolve(Buffer.concat(chunks));else reject(new AssetError('CONVERT',error||'Raster decoded dimensions differ'));});child.stdin.end(bytes);
  });return {width,height,data};
}

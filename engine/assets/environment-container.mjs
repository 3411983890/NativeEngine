import {requireAsset} from './gltf-document.mjs';
export const ENVIRONMENT_HEADER=160,ENVIRONMENT_LIMIT=256*1024*1024,ENVIRONMENT_RADIANCE_MAX=1e12;
export const environmentDimensions=(w,h)=>requireAsset(Number.isSafeInteger(w)&&Number.isSafeInteger(h)&&w===h*2&&h>0&&w<=16384&&w*h*16<=ENVIRONMENT_LIMIT,'Environment must be a bounded 2:1 equirectangular panorama','BUDGET');
export function isFloatingEnvironmentHeader(bytes){
  if(bytes.length>=148&&bytes.subarray(0,4).toString()==='DDS '&&bytes.subarray(84,88).toString()==='DX10')return [2,10,95,96].includes(bytes.readUInt32LE(128))&&bytes.readUInt32LE(12)>0&&bytes.readUInt32LE(16)===bytes.readUInt32LE(12)*2;
  if(bytes.length>=80&&bytes.subarray(0,12).equals(Buffer.from([171,75,84,88,32,50,48,187,13,10,26,10])))return [97,109,143,144].includes(bytes.readUInt32LE(12))&&bytes.readUInt32LE(24)>0&&bytes.readUInt32LE(20)===bytes.readUInt32LE(24)*2;
  return false;
}
// Source decoders are editor dependencies; packages only need this validator.
export function inspectEnvironment(bytes){
  requireAsset(bytes.length>=ENVIRONMENT_HEADER&&bytes.subarray(0,4).toString()==='NGE1'&&bytes.readUInt32LE(4)===1,'Invalid NGE1 environment');const width=bytes.readUInt32LE(8),height=bytes.readUInt32LE(12);environmentDimensions(width,height);
  requireAsset(bytes.readUInt32LE(16)===4&&bytes.readUInt32LE(20)===ENVIRONMENT_HEADER&&bytes.readUInt32LE(24)===width*height*16&&bytes.readUInt32LE(28)===0&&bytes.subarray(140,160).every(v=>v===0)&&bytes.length===ENVIRONMENT_HEADER+width*height*16,'Invalid NGE1 layout');const diffuseSH=Array.from({length:9},(_,i)=>Array.from({length:3},(_,c)=>bytes.readFloatLE(32+(i*3+c)*4)));requireAsset(diffuseSH.flat().every(v=>Number.isFinite(v)&&Math.abs(v)<=1e14),'Invalid environment diffuse SH');
  for(let at=ENVIRONMENT_HEADER;at<bytes.length;at+=16){for(let c=0;c<3;c++){const value=bytes.readFloatLE(at+c*4);requireAsset(Number.isFinite(value)&&value>=0&&value<=ENVIRONMENT_RADIANCE_MAX,'Invalid environment radiance');}requireAsset(bytes.readFloatLE(at+12)===1,'Invalid environment alpha');}return {width,height,diffuseSH,data:bytes.subarray(ENVIRONMENT_HEADER)};
}

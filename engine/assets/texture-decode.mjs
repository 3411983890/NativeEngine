import fs from 'node:fs/promises';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';
import {inflateSync} from 'node:zlib';
import {ZSTDDecoder} from 'three/examples/jsm/libs/zstddec.module.js';
import {decodeBlocks} from './block-codec.mjs';
import {requireAsset,AssetError} from './gltf-document.mjs';

const KTX2=Buffer.from([0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,13,10,26,10]);
const limit=256*1024*1024;
const dimensions=(w,h,bpp=4)=>requireAsset(Number.isSafeInteger(w)&&Number.isSafeInteger(h)&&w>0&&h>0&&w<=16384&&h<=16384&&w*h*bpp<=limit,'Texture dimensions exceed 256 MiB decoded budget','BUDGET');
const range=(bytes,start,size)=>requireAsset(Number.isSafeInteger(start)&&Number.isSafeInteger(size)&&start>=0&&size>=0&&start+size<=bytes.length,'Texture range exceeds its container');
const u64=(bytes,offset)=>{range(bytes,offset,8);const n=bytes.readBigUInt64LE(offset);requireAsset(n<=BigInt(Number.MAX_SAFE_INTEGER),'Texture offset exceeds safe integer range');return Number(n);};
let basisPromise,zstdPromise;
async function basisModule(){
  basisPromise??=(async()=>{const file=fileURLToPath(import.meta.resolve('three/examples/jsm/libs/basis/basis_transcoder.js'));
    // Execute only the pinned package's trusted CommonJS wrapper. Asset bytes are
    // passed to WASM; they are never evaluated as JavaScript and no URL is fetched.
    const context={module:{exports:{}},exports:{},console:{log(){},error(){}},WebAssembly,TextDecoder,TextEncoder,setTimeout,clearTimeout,URL};
    vm.runInNewContext(await fs.readFile(file,'utf8'),context,{filename:file,timeout:5000});const api=await context.module.exports({wasmBinary:await fs.readFile(file.replace(/\.js$/,'.wasm')),print(){},printErr(){}});api.initializeBasis();return api;
  })().catch(e=>{basisPromise=undefined;throw e;});return basisPromise;
}
function finish(image,{semantic='albedo',allowHdr=false}={}){
  requireAsset(!image.hdr||allowHdr,'HDR texture requires a floating-point environment/material workflow','UNSUPPORTED');if(image.hdr){if(image.premultiplied)for(let p=0;p<image.data.length;p+=16){const alpha=image.data.readFloatLE(p+12);requireAsset(Number.isFinite(alpha)&&alpha>=0&&alpha<=1,'Invalid floating-point premultiplied alpha');for(let c=0;c<3;c++)image.data.writeFloatLE(alpha?image.data.readFloatLE(p+c*4)/alpha:0,p+c*4);}return {...image,premultiplied:false};}
  const {data,width,height}=image,color=semantic==='albedo'||semantic==='emissive';
  for(let p=0;p<data.length;p+=4){
    if(semantic==='normal'&&image.channels===2){const x=data[p]/127.5-1,y=data[p+1]/127.5-1;data[p+2]=Math.round((Math.sqrt(Math.max(0,1-x*x-y*y))*.5+.5)*255);}
    for(let c=0;c<3;c++){
      if(!image.premultiplied&&(!color||image.colorSpace!=='linear'))continue;
      let value=data[p+c]/255;if(color&&image.colorSpace==='srgb')value=value<=.04045?value/12.92:((value+.055)/1.055)**2.4;
      if(image.premultiplied)value=data[p+3]?Math.min(1,value*255/data[p+3]):0;
      if(color)value=value<=.0031308?value*12.92:1.055*value**(1/2.4)-.055;
      data[p+c]=Math.round(Math.max(0,Math.min(1,value))*255);
    }
  }
  return {...image,width,height,data,colorSpace:color?'srgb':'linear',premultiplied:false};
}
// DXGI formats shared by DDS and the corresponding Vulkan KTX2 formats below.
const dxgi=new Map([
  [28,{channels:4,bytes:4}],[29,{channels:4,bytes:4,srgb:true}],[87,{channels:4,bytes:4,bgra:true}],[91,{channels:4,bytes:4,bgra:true,srgb:true}],
  [88,{channels:4,bytes:4,bgra:true,opaque:true}],[93,{channels:4,bytes:4,bgra:true,opaque:true,srgb:true}],
  [24,{channels:4,bytes:4,masks:[0x3ff,0xffc00,0x3ff00000,0xc0000000]}],
  [85,{channels:4,bytes:2,masks:[0xf800,0x7e0,0x1f,0]}],[86,{channels:4,bytes:2,masks:[0x7c00,0x3e0,0x1f,0x8000]}],[115,{channels:4,bytes:2,masks:[0xf00,0xf0,0xf,0xf000]}],
  [63,{channels:1,bytes:1,snorm8:true}],[51,{channels:2,bytes:2,snorm8:true}],[31,{channels:4,bytes:4,snorm8:true}],
  [61,{channels:1,bytes:1}],[49,{channels:2,bytes:2}],[56,{channels:1,bytes:2,unorm16:true}],[35,{channels:2,bytes:4,unorm16:true}],[11,{channels:4,bytes:8,unorm16:true}],
  [71,{bc:1}],[72,{bc:1,srgb:true}],[74,{bc:2}],[75,{bc:2,srgb:true}],[77,{bc:3}],[78,{bc:3,srgb:true}],
  [80,{bc:4,channels:1}],[81,{bc:14,channels:1}],[83,{bc:5,channels:2}],[84,{bc:15,channels:2}],
  [95,{bc:6,hdr:true}],[96,{bc:16,hdr:true}],[98,{bc:7}],[99,{bc:7,srgb:true}],
  [10,{channels:4,bytes:8,float16:true,hdr:true}],[2,{channels:4,bytes:16,float32:true,hdr:true}],
]);
const blockBytes=format=>[1,4].includes(format%10)?8:16;
const levelSize=(w,h,f)=>f.bc?Math.ceil(w/4)*Math.ceil(h/4)*blockBytes(f.bc):w*h*f.bytes;
function half(bits){const sign=bits&0x8000?-1:1,e=bits>>10&31,m=bits&1023;return e===31?(m?NaN:sign*Infinity):sign*(e?2**(e-15)*(1+m/1024):2**(-14)*(m/1024));}
async function rawPixels(bytes,w,h,format,pitch=w*format.bytes){
  dimensions(w,h,format.hdr?16:4);if(format.bc){if(format.bc===7)for(let i=0;i<bytes.length;i+=16)requireAsset(bytes[i]!==0,'Invalid BC7 block mode');const out=await decodeBlocks(bytes,w,h,format.bc);if(format.opaque)for(let i=3;i<out.length;i+=4)out[i]=255;return out;}
  const out=Buffer.alloc(w*h*(format.hdr?16:4));
  requireAsset(pitch>=w*format.bytes&&bytes.length>=pitch*h,'Truncated or invalid texture row pitch');
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){
    const at=y*pitch+x*format.bytes,p=(y*w+x)*(format.hdr?16:4);
    if(format.hdr){for(let c=0;c<4;c++){const value=c>=format.channels?(c===3?1:0):format.float16?half(bytes.readUInt16LE(at+c*2)):bytes.readFloatLE(at+c*4);requireAsset(Number.isFinite(value),'Nonfinite HDR texture sample');out.writeFloatLE(value,p+c*4);}continue;}
    if(format.masks){let value=0;for(let i=0;i<format.bytes;i++)value+=bytes[at+i]*2**(i*8);for(let c=0;c<4;c++){const mask=format.masks[c]>>>0;if(!mask){out[p+c]=c===3?255:0;continue;}let shift=0;while(((mask>>>shift)&1)===0&&shift<32)shift++;const maximum=mask>>>shift;requireAsset(((maximum+1)&maximum)===0,'Noncontiguous DDS channel mask');out[p+c]=Math.round((((value>>>shift)&maximum)>>>0)/maximum*255);}}
    else {for(let c=0;c<format.channels;c++)out[p+c]=format.unorm16?Math.round(bytes.readUInt16LE(at+c*2)/257):format.snorm8?Math.round((Math.max(-1,bytes.readInt8(at+c)/127)*.5+.5)*255):bytes[at+c];if(format.channels<4)out[p+3]=255;}
    if(format.bgra)[out[p],out[p+2]]=[out[p+2],out[p]];if(format.opaque)out[p+3]=255;
  }return out;
}
export function isContainerTexture(bytes){return bytes.subarray(0,4).toString('ascii')==='DDS '||bytes.subarray(0,12).equals(KTX2)||bytes[0]===0x73&&bytes[1]===0x42;}
export async function decodeDds(bytes,options={}){
  requireAsset(bytes.length>=128&&bytes.subarray(0,4).toString('ascii')==='DDS '&&bytes.readUInt32LE(4)===124&&bytes.readUInt32LE(76)===32,'Invalid DDS header');
  const width=bytes.readUInt32LE(16),height=bytes.readUInt32LE(12),depth=bytes.readUInt32LE(24),mips=Math.max(1,bytes.readUInt32LE(28)),caps2=bytes.readUInt32LE(112);dimensions(width,height);
  requireAsset(depth<=1&&!(caps2&0x20fe00),'DDS cube/volume textures require an environment or layered texture component','UNSUPPORTED');requireAsset(mips<=Math.floor(Math.log2(Math.max(width,height)))+1,'Invalid DDS mip count');
  const flags=bytes.readUInt32LE(80),fourcc=bytes.subarray(84,88).toString('ascii');let offset=128,format,premultiplied=false,alphaMode=0;
  if(flags&4){
    if(fourcc==='DX10'){range(bytes,128,20);const code=bytes.readUInt32LE(128);requireAsset(bytes.readUInt32LE(132)===3&&bytes.readUInt32LE(140)===1&&!(bytes.readUInt32LE(136)&4),'DDS arrays/cubes/volumes require a layered component','UNSUPPORTED');alphaMode=bytes.readUInt32LE(144)&7;requireAsset(alphaMode<=4,'Invalid DDS alpha mode');premultiplied=alphaMode===2;format=dxgi.get(code);offset=148;}
    else {const bc={DXT1:1,DXT2:2,DXT3:2,DXT4:3,DXT5:3,ATI1:4,BC4U:4,BC4S:14,ATI2:5,BC5U:5,BC5S:15}[fourcc];if(bc)format={bc,channels:bc%10===4?1:bc%10===5?2:4,srgb:['albedo','emissive'].includes(options.semantic??'albedo')};premultiplied=fourcc==='DXT2'||fourcc==='DXT4';}
  }else if(flags&0x40){const bits=bytes.readUInt32LE(88);requireAsset([16,24,32].includes(bits),'Unsupported DDS RGB bit depth','UNSUPPORTED');format={bytes:bits/8,channels:4,masks:[92,96,100,104].map(p=>bytes.readUInt32LE(p)),srgb:['albedo','emissive'].includes(options.semantic??'albedo')};let used=0;for(const mask of format.masks){requireAsset(!(used&mask),'Overlapping DDS channel masks');used=(used|mask)>>>0;}requireAsset(used!==0&&(bits===32||(used>>>bits)===0),'DDS channel mask exceeds its pixel size');}
  else if(flags&0x20000){const bits=bytes.readUInt32LE(88);if(bits===8)format={bytes:1,channels:1,masks:[255,255,255,0],srgb:['albedo','emissive'].includes(options.semantic??'albedo')};}
  requireAsset(format,'Unsupported DDS pixel format','UNSUPPORTED');dimensions(width,height,format.hdr?16:4);
  // Legacy DDS writers may DWORD-align every RGB row, while modern writers
  // pack lower mips tightly. The exact container length selects the layout.
  const declaredPitch=!format.bc&&(bytes.readUInt32LE(8)&8)?bytes.readUInt32LE(20):width*format.bytes;
  if(!format.bc)requireAsset(declaredPitch>=width*format.bytes,'Truncated or invalid texture row pitch');
  let lowerAlignment=1;
  if(!format.bc&&mips>1){const chainSize=alignment=>{let total=declaredPitch*height;for(let mip=1;mip<mips;mip++)total+=Math.ceil(Math.max(1,width>>mip)*format.bytes/alignment)*alignment*Math.max(1,height>>mip);return total;};
    if(chainSize(1)!==bytes.length-offset&&declaredPitch%4===0&&chainSize(4)===bytes.length-offset)lowerAlignment=4;
  }
  let baseBytes,basePitch;for(let mip=0;mip<mips;mip++){
    const w=Math.max(1,width>>mip),h=Math.max(1,height>>mip);let pitch=w*format.bytes;
    if(!format.bc)pitch=mip===0?declaredPitch:Math.ceil(pitch/lowerAlignment)*lowerAlignment;
    const size=format.bc?levelSize(w,h,format):pitch*h;range(bytes,offset,size);if(!mip){baseBytes=bytes.subarray(offset,offset+size);basePitch=pitch;}offset+=size;
  }
  requireAsset(offset===bytes.length,'DDS payload length differs from its mip chain');const data=await rawPixels(baseBytes,width,height,format,basePitch);
  if(alphaMode===3){if(format.hdr)for(let p=12;p<data.length;p+=16)data.writeFloatLE(1,p);else for(let p=3;p<data.length;p+=4)data[p]=255;}
  return finish({width,height,data,channels:format.channels??4,hdr:!!format.hdr,colorSpace:format.srgb?'srgb':'linear',premultiplied,sourceFormat:'DDS'},options);
}
const vk=new Map([[37,28],[43,29],[44,87],[50,91],[9,61],[16,49],[70,56],[77,35],[91,11],[97,10],[109,2],
  [10,63],[17,51],[38,31],[4,85],[8,86],[1000340000,115],[64,24],
  [131,71],[132,72],[133,71],[134,72],[135,74],[136,75],[137,77],[138,78],[139,80],[140,81],[141,83],[142,84],[143,95],[144,96],[145,98],[146,99]].map(([v,d])=>[v,dxgi.get(d)]));
for(const code of [131,132])vk.set(code,{...vk.get(code),opaque:true});
vk.set(23,{channels:3,bytes:3});vk.set(29,{channels:3,bytes:3,srgb:true});vk.set(15,{...dxgi.get(61),srgb:true});vk.set(22,{...dxgi.get(49),srgb:true});
for(const [code,masks]of [[2,[0xf000,0xf00,0xf0,0xf]],[3,[0xf0,0xf00,0xf000,0xf]],[5,[0x1f,0x7e0,0xf800,0]],[6,[0xf800,0x7c0,0x3e,1]],[7,[0x3e,0x7c0,0xf800,1]],[1000340001,[0xf,0xf0,0xf00,0xf000]]])vk.set(code,{channels:4,bytes:2,masks});
vk.set(51,{...dxgi.get(28)});vk.set(52,{...dxgi.get(31)});vk.set(57,{...dxgi.get(29)});vk.set(58,{channels:4,bytes:4,masks:[0x3ff00000,0xffc00,0x3ff,0xc0000000]});
function metadata(bytes,start,length){const out={};let at=start;while(at<start+length){range(bytes,at,4);const n=bytes.readUInt32LE(at);at+=4;requireAsset(n>1&&at+n<=start+length,'Invalid KTX metadata');const value=bytes.subarray(at,at+n),split=value.indexOf(0);requireAsset(split>0,'Invalid KTX metadata key');const key=value.toString('utf8',0,split);requireAsset(!Object.hasOwn(out,key),'Duplicate KTX metadata key');out[key]=value.subarray(split+1).toString('utf8').replace(/\0+$/,'');at+=Math.ceil(n/4)*4;}requireAsset(at===start+length,'Invalid KTX metadata padding');return out;}
async function basisPixels(bytes,width,height,ktx=true){
  const api=await basisModule(),file=ktx?new api.KTX2File(new Uint8Array(bytes)):new api.BasisFile(new Uint8Array(bytes));
  try{requireAsset(!ktx||file.isValid(),'Invalid Basis/KTX2 payload');requireAsset(ktx?file.getWidth()===width&&file.getHeight()===height:file.getNumImages()===1,'Basis texture dimensions/layers differ');
    if(ktx){requireAsset(file.getLayers()<=1&&file.getFaces()===1,'Basis arrays/cubes require a layered component','UNSUPPORTED');requireAsset(!file.isHDR(),'HDR Basis textures require a floating-point workflow','UNSUPPORTED');}
    requireAsset(file.startTranscoding(),'Basis texture failed to start transcoding');const size=ktx?file.getImageTranscodedSizeInBytes(0,0,0,13):file.getImageTranscodedSizeInBytes(0,0,13);requireAsset(size===width*height*4,'Basis RGBA dimensions differ','BUDGET');const output=new Uint8Array(size),ok=ktx?file.transcodeImage(output,0,0,0,13,0,-1,-1):file.transcodeImage(output,0,0,13,0,0);requireAsset(ok,'Basis texture transcode failed');return Buffer.from(output);
  }finally{file.close();file.delete();}
}
export async function decodeKtx2(bytes,options={}){
  requireAsset(bytes.length>=104&&bytes.subarray(0,12).equals(KTX2),'Invalid KTX2 header');const width=bytes.readUInt32LE(20),height=bytes.readUInt32LE(24),levels=Math.max(1,bytes.readUInt32LE(40)),supercompression=bytes.readUInt32LE(44);dimensions(width,height);
  requireAsset(bytes.readUInt32LE(28)===0&&bytes.readUInt32LE(32)<=1&&bytes.readUInt32LE(36)===1,'KTX2 arrays/cubes/volumes require a layered component','UNSUPPORTED');requireAsset(levels<=Math.floor(Math.log2(Math.max(width,height)))+1&&[0,1,2,3].includes(supercompression),'Invalid KTX2 mip/supercompression');range(bytes,80,levels*24);
  const ranges=[[0,80+levels*24]],addRange=(offset,length)=>{if(!length)return;range(bytes,offset,length);requireAsset(!ranges.some(([a,b])=>offset<b&&offset+length>a),'Overlapping KTX2 container ranges');ranges.push([offset,offset+length]);};
  const dfd=bytes.readUInt32LE(48),dfdSize=bytes.readUInt32LE(52),kvd=bytes.readUInt32LE(56),kvdSize=bytes.readUInt32LE(60),sgd=u64(bytes,64),sgdSize=u64(bytes,72);requireAsset(dfdSize>=28,'Missing KTX2 data format descriptor');addRange(dfd,dfdSize);addRange(kvd,kvdSize);addRange(sgd,sgdSize);requireAsset(bytes.readUInt32LE(dfd)===dfdSize&&bytes.readUInt16LE(dfd+10)<=dfdSize-4,'Invalid KTX2 descriptor size');
  const transfer=bytes[dfd+14],premultiplied=!!(bytes[dfd+15]&1),format=vk.get(bytes.readUInt32LE(12)),mips=[];requireAsset([1,2].includes(transfer),'Unsupported KTX2 transfer function','UNSUPPORTED');
  if(format)requireAsset((transfer===2)===!!format.srgb,'KTX2 transfer function contradicts its Vulkan format');
  requireAsset(bytes.readUInt32LE(dfd+4)===0&&bytes.readUInt16LE(dfd+8)===2&&bytes.readUInt16LE(dfd+10)>=24,'Unsupported KTX2 basic descriptor','UNSUPPORTED');
  if(format?.hdr)requireAsset(bytes[dfd+12]===(format.bc?133:1),'KTX2 floating color model contradicts its Vulkan format');
  if(format){const typeSize=format.bc?1:format.masks?format.bytes:format.float16||format.unorm16?2:format.float32?4:1;requireAsset(bytes.readUInt32LE(16)===typeSize,'KTX2 type size contradicts its Vulkan format');requireAsset(bytes[dfd+16]===(format.bc?3:0)&&bytes[dfd+17]===(format.bc?3:0)&&bytes[dfd+18]===0&&bytes[dfd+19]===0&&bytes[dfd+20]===(format.bc?blockBytes(format.bc):format.bytes)&&bytes.subarray(dfd+21,dfd+28).every(v=>v===0),'KTX2 descriptor block shape contradicts its Vulkan format');}
  const meta=metadata(bytes,kvd,kvdSize),orientation=meta.KTXorientation??'rd',swizzle=meta.KTXswizzle??'rgba';requireAsset(/^[rl][du]$/.test(orientation)&&/^[rgba01]{4}$/.test(swizzle),'Unsupported KTX2 orientation/swizzle','UNSUPPORTED');
  if(options.basisMaterial){const color=['albedo','emissive'].includes(options.semantic??'albedo'),model=bytes[dfd+12];requireAsset(bytes.readUInt32LE(12)===0&&((model===163&&supercompression===1)||(model===166&&[0,2].includes(supercompression))),'KHR_texture_basisu requires ETC1S/BasisLZ or UASTC KTX2');requireAsset(width%4===0&&height%4===0&&bytes.readUInt32LE(32)===0&&orientation==='rd'&&swizzle==='rgba'&&!premultiplied,'KHR_texture_basisu material image layout is incompatible');requireAsset(transfer===(color?2:1)&&bytes[dfd+13]===(color?1:0),'KHR_texture_basisu color space does not match the material slot');}
  for(let mip=0;mip<levels;mip++){const at=80+mip*24,offset=u64(bytes,at),length=u64(bytes,at+8),rawLength=u64(bytes,at+16);requireAsset(length>0&&rawLength<=limit,'Invalid KTX2 level size','BUDGET');addRange(offset,length);if(format){requireAsset(rawLength===levelSize(Math.max(1,width>>mip),Math.max(1,height>>mip),format),'KTX2 uncompressed mip size differs');if(supercompression===0)requireAsset(length===rawLength,'KTX2 stored mip length differs');}mips.push({offset,length,rawLength});}
  let data,hdr=false,channels=4;if(bytes.readUInt32LE(12)===0){requireAsset([0,1,2].includes(supercompression),'Unsupported Basis supercompression');data=await basisPixels(bytes,width,height);}
  else {requireAsset(format&&supercompression!==1,'Unsupported KTX2 Vulkan format','UNSUPPORTED');for(let level=0;level<mips.length;level++){const mip=mips[level];let raw=bytes.subarray(mip.offset,mip.offset+mip.length);
      if(supercompression===3)raw=inflateSync(raw,{maxOutputLength:mip.rawLength});else if(supercompression===2){zstdPromise??=(async()=>{const decoder=new ZSTDDecoder();await decoder.init();return decoder;})();raw=Buffer.from((await zstdPromise).decode(raw,mip.rawLength));}
      requireAsset(raw.length===mip.rawLength,'KTX2 decompressed level length differs');if(level===0)data=await rawPixels(raw,width,height,format);
    }hdr=!!format.hdr;channels=format.channels??4;
  }
  {const stride=hdr?16:4,mapped=Buffer.alloc(data.length);for(let y=0;y<height;y++)for(let x=0;x<width;x++){const from=((orientation[1]==='u'?height-y-1:y)*width+(orientation[0]==='l'?width-x-1:x))*stride,to=(y*width+x)*stride;for(let c=0;c<4;c++){const channel='rgba'.indexOf(swizzle[c]),value=swizzle[c]==='0'?0:swizzle[c]==='1'?(hdr?1:255):hdr?data.readFloatLE(from+channel*4):data[from+channel];if(hdr)mapped.writeFloatLE(value,to+c*4);else mapped[to+c]=value;}}data=mapped;}
  return finish({width,height,data,hdr,channels,colorSpace:transfer===2?'srgb':'linear',colorPrimaries:bytes[dfd+13],premultiplied,sourceFormat:'KTX2'},options);
}
export async function decodeContainerTexture(bytes,options={}){
  requireAsset(bytes.length<=limit,'Encoded texture exceeds 256 MiB','BUDGET');
  try{if(bytes.subarray(0,4).toString('ascii')==='DDS ')return await decodeDds(bytes,options);if(bytes.subarray(0,12).equals(KTX2))return await decodeKtx2(bytes,options);
    requireAsset(bytes[0]===0x73&&bytes[1]===0x42&&bytes.length>=77,'Unrecognized texture container');const api=await basisModule(),file=new api.BasisFile(new Uint8Array(bytes));let width,height;try{requireAsset(file.getNumImages()===1,'Basis arrays require a layered component','UNSUPPORTED');width=file.getImageWidth(0,0);height=file.getImageHeight(0,0);dimensions(width,height);}finally{file.close();file.delete();}return finish({width,height,data:await basisPixels(bytes,width,height,false),channels:4,colorSpace:'srgb',sourceFormat:'Basis'},options);
  }catch(error){if(error instanceof AssetError)throw error;throw new AssetError('TEXTURE',error.message);}
}

import {createDefaultContainer,write} from 'three/examples/jsm/libs/ktx-parse.module.js';
import {deflateSync} from 'node:zlib';
import {readFileSync} from 'node:fs';
export function basisFixture(kind='etc1s',{linear=false}={}){const fixture=JSON.parse(readFileSync(new URL('../../tests/fixtures/texture-basis.json',import.meta.url))),bytes=Buffer.from(fixture.variants[kind].base64,'base64');if(linear&&kind!=='basis'){const dfd=bytes.readUInt32LE(48);bytes[dfd+13]=0;bytes[dfd+14]=1;}return bytes;}
export function rgbaFixture(width=4,height=4){const data=Buffer.alloc(width*height*4);for(let y=0;y<height;y++)for(let x=0;x<width;x++){const p=(y*width+x)*4;data[p]=x%2?230:40;data[p+1]=y%2?50:190;data[p+2]=70;data[p+3]=255;}return {width,height,data};}
export function ddsFixture({width=4,height=4,data,bc,dxgi,signed=false,mips}={}){
  const dx10=dxgi!==undefined,header=Buffer.alloc(dx10?148:128),levels=mips??[data??rgbaFixture(width,height).data];header.write('DDS ');header.writeUInt32LE(124,4);header.writeUInt32LE(0x1007|(bc?0x80000:8)|(levels.length>1?0x20000:0),8);header.writeUInt32LE(height,12);header.writeUInt32LE(width,16);header.writeUInt32LE(bc?levels[0].length:width*4,20);header.writeUInt32LE(levels.length,28);header.writeUInt32LE(32,76);header.writeUInt32LE(dx10||bc?4:0x41,80);header.writeUInt32LE(0x1000|(levels.length>1?0x400008:0),108);
  if(dx10){header.write('DX10',84);header.writeUInt32LE(dxgi,128);header.writeUInt32LE(3,132);header.writeUInt32LE(1,140);}
  else if(bc)header.write({1:'DXT1',2:'DXT3',3:'DXT5',4:signed?'BC4S':'ATI1',5:signed?'BC5S':'ATI2'}[bc],84);
  else{header.writeUInt32LE(32,88);[0xff,0xff00,0xff0000,0xff000000].forEach((mask,i)=>header.writeUInt32LE(mask,92+i*4));}
  return Buffer.concat([header,...levels]);
}
export function ktxFixture(image=rgbaFixture(),{linear=false,zlib=false,orientation='rd',swizzle='rgba'}={}){
  const container=createDefaultContainer();Object.assign(container,{vkFormat:linear?37:43,pixelWidth:image.width,pixelHeight:image.height,levelCount:1,supercompressionScheme:zlib?3:0});
  Object.assign(container.dataFormatDescriptor[0],{colorModel:1,transferFunction:linear?1:2,bytesPlane:[4,0,0,0,0,0,0,0],samples:[0,1,2,15].map((channelType,c)=>({bitOffset:c*8,bitLength:7,channelType,samplePosition:[0,0,0,0],sampleLower:0,sampleUpper:255}))});
  container.keyValue={KTXorientation:orientation,KTXswizzle:swizzle};container.levels=[{levelData:zlib?deflateSync(image.data):image.data,uncompressedByteLength:image.data.length}];return Buffer.from(write(container));
}
export function tgaFixture(image=rgbaFixture(),{rle=false,top=true}={}){
  const header=Buffer.alloc(18);header[2]=rle?10:2;header.writeUInt16LE(image.width,12);header.writeUInt16LE(image.height,14);header[16]=32;header[17]=8|(top?32:0);const pixels=Buffer.alloc(image.data.length);
  for(let y=0;y<image.height;y++)for(let x=0;x<image.width;x++){const p=(y*image.width+x)*4,q=((top?y:image.height-y-1)*image.width+x)*4;pixels[p]=image.data[q+2];pixels[p+1]=image.data[q+1];pixels[p+2]=image.data[q];pixels[p+3]=image.data[q+3];}
  if(!rle)return Buffer.concat([header,pixels]);const packets=[];for(let p=0;p<pixels.length;p+=128*4){const part=pixels.subarray(p,Math.min(pixels.length,p+128*4));packets.push(Buffer.from([part.length/4-1]),part);}return Buffer.concat([header,...packets]);
}
export function bmpFixture(image=rgbaFixture()){
  const pitch=Math.ceil(image.width*3/4)*4,out=Buffer.alloc(54+pitch*image.height);out.write('BM');out.writeUInt32LE(out.length,2);out.writeUInt32LE(54,10);out.writeUInt32LE(40,14);out.writeInt32LE(image.width,18);out.writeInt32LE(image.height,22);out.writeUInt16LE(1,26);out.writeUInt16LE(24,28);out.writeUInt32LE(pitch*image.height,34);
  for(let y=0;y<image.height;y++)for(let x=0;x<image.width;x++){const p=(y*image.width+x)*4,q=54+(image.height-y-1)*pitch+x*3;out[q]=image.data[p+2];out[q+1]=image.data[p+1];out[q+2]=image.data[p];}return out;
}

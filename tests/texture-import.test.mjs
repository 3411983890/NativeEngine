import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {decodeImage} from '../engine/assets/model-import.mjs';
import {importAsset,verifyImport,attachTexture} from '../engine/assets/import-service.mjs';
import {loadScene,buildScene,undoSceneFile} from '../engine/scene/index.mjs';
import {decodeBlocks} from '../engine/assets/block-codec.mjs';
import {encodeBc1,encodeBc3,encodeBc5,encodeBc7} from '../tools/bc.mjs';
import {rgbaFixture,ddsFixture,ktxFixture,tgaFixture,bmpFixture,basisFixture} from '../tools/offline/texture-fixture.mjs';
import {writeAssetFixture,writeCharacterFixture} from '../tools/asset-fixtures.mjs';
import {createEditorService} from '../editor/service.mjs';
import {main} from '../tools/assets.mjs';
import {createDefaultContainer,write} from 'three/examples/jsm/libs/ktx-parse.module.js';
import {deflateSync} from 'node:zlib';

async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-texture-import-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}
test('DDS RGBA, DX10 linear color and native BC1/3/5/7 decode with semantic channels',async()=>{
  const image=rgbaFixture();assert.deepEqual((await decodeImage(ddsFixture(image))).data,image.data);
  const linear=await decodeImage(ddsFixture({...image,dxgi:28,data:Buffer.alloc(64,128)}));assert.equal(linear.data[0],188);assert.equal(linear.data[3],128);
  for(const [bc,encode,dxgi]of [[1,encodeBc1,undefined],[3,encodeBc3,undefined],[5,encodeBc5,undefined],[7,encodeBc7,99]]){const block=encode(image),dds=ddsFixture({...image,data:block,bc,dxgi}),decoded=await decodeImage(dds,{semantic:bc===5?'normal':'albedo'});assert.equal(decoded.data.length,64);assert.ok(decoded.data.some(v=>v));}
});
test('external BC7 supports every partition mode and BC2 alpha survives decode',async()=>{
  for(let mode=0;mode<8;mode++){const block=Buffer.alloc(16);block[0]=1<<mode;const decoded=await decodeBlocks(block,4,4,7);assert.deepEqual(Array.from(decoded.subarray(0,4)),[0,0,0,mode<4?255:0]);}
  const block=Buffer.alloc(16,255);block.writeUInt16LE(0xf800,8);block.writeUInt16LE(0,10);block.fill(0,12);const decoded=await decodeImage(ddsFixture({bc:2,data:block}));assert.deepEqual(Array.from(decoded.data.subarray(0,4)),[255,0,0,255]);
});
test('legacy RGB DDS accepts tight and DWORD-aligned lower mip rows without accepting truncated chains',async()=>{
  const image=rgbaFixture(3,3);
  for(const alignment of [1,4]){
    const header=ddsFixture().subarray(0,128);header.writeUInt32LE(0x2100f,8);header.writeUInt32LE(3,12);header.writeUInt32LE(3,16);header.writeUInt32LE(12,20);header.writeUInt32LE(2,28);header.writeUInt32LE(0x40,80);header.writeUInt32LE(24,88);header.writeUInt32LE(0,104);
    const pixels=Buffer.alloc(36),mip=Buffer.alloc(alignment===1?3:4,70);
    for(let y=0;y<3;y++)for(let x=0;x<3;x++)image.data.copy(pixels,y*12+x*3,(y*3+x)*4,(y*3+x)*4+3);
    const bytes=Buffer.concat([header,pixels,mip]);assert.deepEqual((await decodeImage(bytes)).data,image.data);
    await assert.rejects(decodeImage(bytes.subarray(0,bytes.length-2)),/range|length/);
  }
});
test('KTX2 rejects contradictory descriptors and corrupt lower compressed mips',async()=>{
  for(const mutate of [b=>b.writeUInt32LE(2,16),b=>b.writeUInt16LE(3,b.readUInt32LE(48)+8),b=>{b[b.readUInt32LE(48)+16]=3;}]){
    const bytes=ktxFixture();mutate(bytes);await assert.rejects(decodeImage(bytes),/descriptor|type size|block shape/);
  }
  const c=createDefaultContainer();Object.assign(c,{vkFormat:43,pixelWidth:4,pixelHeight:4,levelCount:2,supercompressionScheme:3});Object.assign(c.dataFormatDescriptor[0],{colorModel:1,transferFunction:2,bytesPlane:[4,0,0,0,0,0,0,0]});c.levels=[{levelData:deflateSync(rgbaFixture().data),uncompressedByteLength:64},{levelData:Buffer.from([1,2,3,4]),uncompressedByteLength:16}];await assert.rejects(decodeImage(Buffer.from(write(c))),/header|inflate|data/);
});
test('signed BC5 reconstructs normal Z and block edge crops do not overwrite valid pixels',async()=>{
  const normal=await decodeImage(ddsFixture({bc:5,signed:true,data:Buffer.alloc(16)}),{semantic:'normal'});assert.deepEqual(Array.from(normal.data.subarray(0,4)),[128,128,255,255]);
  const image=rgbaFixture(7,3),bytes=encodeBc7(image),decoded=await decodeBlocks(bytes,7,3,7);assert.equal(decoded.length,7*3*4);assert.ok(decoded[0]!==decoded[4]);
});
test('packed Vulkan/DXGI channels, premultiplied alpha and signed data preserve their declared meaning',async()=>{
  for(const [format,value]of [[2,0xf00f],[3,0x00ff],[4,0xf800],[5,0x001f],[6,0xf801],[7,0x003f],[8,0xfc00],[1000340000,0xff00],[1000340001,0xf00f]]){
    const c=createDefaultContainer(),bytes=Buffer.alloc(2);bytes.writeUInt16LE(value);Object.assign(c,{vkFormat:format,typeSize:2,pixelWidth:1,pixelHeight:1,levelCount:1});Object.assign(c.dataFormatDescriptor[0],{colorModel:1,transferFunction:1,bytesPlane:[2,0,0,0,0,0,0,0]});c.levels=[{levelData:bytes,uncompressedByteLength:2}];assert.deepEqual(Array.from((await decodeImage(Buffer.from(write(c)),{semantic:'orm'})).data),[255,0,0,255],'Vulkan packed format '+format);
  }
  const pixels=Buffer.alloc(64);for(let p=0;p<pixels.length;p+=4){pixels.fill(64,p,p+3);pixels[p+3]=128;}const premultiplied=ddsFixture({dxgi:28,data:pixels});premultiplied.writeUInt32LE(2,144);const decoded=await decodeImage(premultiplied);assert.equal(decoded.data[0],188);assert.equal(decoded.data[3],128);
  const signed=ddsFixture({dxgi:51,data:Buffer.alloc(32)});signed.writeUInt32LE(8,20);assert.deepEqual(Array.from((await decodeImage(signed,{semantic:'normal'})).data.subarray(0,4)),[128,128,255,255]);
});
test('KTX2 RGBA, zlib, orientation, swizzles and linear data retain source meaning',async()=>{
  const image=rgbaFixture();assert.deepEqual((await decodeImage(ktxFixture(image))).data,image.data);assert.deepEqual((await decodeImage(ktxFixture(image,{zlib:true}))).data,image.data);
  const flipped=await decodeImage(ktxFixture(image,{orientation:'lu',swizzle:'bgra'}));const p=image.data.length-4;assert.deepEqual(Array.from(flipped.data.subarray(0,4)),[image.data[p+2],image.data[p+1],image.data[p],255]);
  const data={width:2,height:2,data:Buffer.alloc(16,128)};assert.equal((await decodeImage(ktxFixture(data,{linear:true}))).data[0],188);assert.equal((await decodeImage(ktxFixture(data,{linear:true}),{semantic:'orm'})).data[0],128);
});
test('TGA raw/RLE and bottom origin, and BMP padded rows decode without a system image application',async()=>{
  const image=rgbaFixture(3,5);for(const bytes of [tgaFixture(image),tgaFixture(image,{rle:true}),tgaFixture(image,{rle:true,top:false}),bmpFixture(image)])assert.deepEqual((await decodeImage(bytes)).data,image.data);
});
test('malformed ranges, cubemaps, arrays, invalid BC7 and HDR usage fail explicitly',async()=>{
  const dds=ddsFixture();for(const size of [0,4,100,127,dds.length-1])await assert.rejects(decodeImage(dds.subarray(0,size)));
  const cube=Buffer.from(dds);cube.writeUInt32LE(0xfe00,112);await assert.rejects(decodeImage(cube),/cube/);
  const badKtx=ktxFixture();badKtx.writeBigUInt64LE(4n,80);await assert.rejects(decodeImage(badKtx),/Overlapping/);
  const invalid=ddsFixture({dxgi:98,bc:7,data:Buffer.alloc(16)});await assert.rejects(decodeImage(invalid),/BC7/);
  const hdr=ddsFixture({dxgi:95,bc:6,data:Buffer.alloc(16)});await assert.rejects(decodeImage(hdr),/HDR/);
});
test('standalone textures have verified previews, material attachment, undo and dependency packages',()=>fixture(async dir=>{
  const source=path.join(dir,'source.dds');await fs.writeFile(source,ddsFixture());const asset=await importAsset({source,output:path.join(dir,'assets/imported'),size:64});await verifyImport(asset.directory);assert.equal(asset.manifest.preview,true);assert.equal(asset.manifest.textures.length,1);
  const again=await importAsset({source,output:path.join(dir,'assets/imported'),size:64});assert.equal(again.manifest.id,asset.manifest.id);
  const scene=await loadScene(path.join(asset.directory,'preview.scene.json')),file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,JSON.stringify(scene));await fs.cp(path.join(asset.directory,'textures'),path.join(dir,'assets/textures'),{recursive:true});
  const attached=await attachTexture({directory:asset.directory,scenePath:file,expectedRevision:0,material:'m0'});assert.match(attached.materials.m0.albedo,/_texture$/);const built=await buildScene(file,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path.includes('_texture.albedo.ngtex')));
  await assert.rejects(attachTexture({directory:asset.directory,scenePath:file,expectedRevision:0,material:'m0'}),e=>e.code==='REVISION_CONFLICT');const undo=await undoSceneFile(file,{projectId:scene.projectId,expectedRevision:1});assert.equal(undo.scene.materials.m0.albedo,scene.materials.m0.albedo);
}));
test('glTF required compressed texture extensions work in static and character import without fallback images',()=>fixture(async dir=>{
  for(const character of [false,true]){const source=await(character?writeCharacterFixture:writeAssetFixture)(path.join(dir,character?'character':'model')),doc=JSON.parse(await fs.readFile(source));doc.images=[{uri:'data:image/ktx2;base64,'+basisFixture(character?'uastc':'etc1s').toString('base64')}];doc.textures=[{extensions:{KHR_texture_basisu:{source:0}}}];doc.extensionsRequired=[...(doc.extensionsRequired??[]),'KHR_texture_basisu'];for(const mat of doc.materials??[])(mat.pbrMetallicRoughness??={}).baseColorTexture={index:0};await fs.writeFile(source,JSON.stringify(doc));const imported=await importAsset({source,output:path.join(dir,'imports'),size:64});assert.equal(imported.manifest.kind,character?'character':'model');}
}));
test('texture attachment rejects destination conflicts and directory links without changing the scene',()=>fixture(async dir=>{
  const source=path.join(dir,'source.dds');await fs.writeFile(source,ddsFixture());const asset=await importAsset({source,output:path.join(dir,'imports'),size:64}),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));
  const sceneDir=path.join(dir,'assets');await fs.mkdir(sceneDir);const file=path.join(sceneDir,'main.scene.json'),original=JSON.stringify(scene);await fs.writeFile(file,original);const textureDir=path.join(sceneDir,'textures');await fs.mkdir(textureDir);
  const target=path.join(textureDir,asset.manifest.id.slice(0,16)+'_texture.albedo.ngtex');await fs.writeFile(target,'unrelated existing content');await assert.rejects(attachTexture({directory:asset.directory,scenePath:file,expectedRevision:0,material:'m0'}),/conflicts/);assert.equal(await fs.readFile(file,'utf8'),original);assert.equal(await fs.readFile(target,'utf8'),'unrelated existing content');
  const linkedDir=path.join(dir,'linked');await fs.mkdir(linkedDir);const linkedFile=path.join(linkedDir,'main.scene.json');await fs.writeFile(linkedFile,original);await fs.symlink(textureDir,path.join(linkedDir,'textures'),'junction');await assert.rejects(attachTexture({directory:asset.directory,scenePath:linkedFile,expectedRevision:0,material:'m0'}),/escapes/);assert.equal(await fs.readFile(linkedFile,'utf8'),original);
}));
test('ETC1S, UASTC+Zstd and Basis decode locally; glTF validates the actual Basis format and material color space',async()=>{
  for(const kind of ['etc1s','uastc','basis']){const decoded=await decodeImage(basisFixture(kind));assert.equal(decoded.width,8);assert.equal(decoded.height,8);assert.ok(decoded.data.some(v=>v>128));}
  await assert.rejects(decodeImage(ktxFixture(),{basisMaterial:true}),/requires ETC1S/);await assert.rejects(decodeImage(basisFixture(),{semantic:'normal',basisMaterial:true}),/color space/);
  const normal=await decodeImage(basisFixture('uastc',{linear:true}),{semantic:'normal',basisMaterial:true});assert.equal(normal.width,8);
});
test('GLB embedded KTX2 views and required DDS extensions select compressed images without reading fallbacks',()=>fixture(async dir=>{
  const source=await writeAssetFixture(path.join(dir,'model')),doc=JSON.parse(await fs.readFile(source));doc.images=[{uri:'missing.png'},{uri:'data:image/vnd-ms.dds;base64,'+ddsFixture().toString('base64')}];doc.textures=[{source:0,extensions:{MSFT_texture_dds:{source:1}}}];doc.extensionsRequired=['MSFT_texture_dds'];await fs.writeFile(source,JSON.stringify(doc));assert.equal((await importAsset({source,output:path.join(dir,'imports'),size:64})).manifest.kind,'model');
  const geometry=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),offset=Math.ceil(geometry.length/4)*4,texture=basisFixture('uastc'),binary=Buffer.alloc(Math.ceil((offset+texture.length)/4)*4);geometry.copy(binary);texture.copy(binary,offset);doc.buffers=[{byteLength:binary.length}];doc.images=[{bufferView:doc.bufferViews.length,mimeType:'image/ktx2'}];doc.bufferViews.push({buffer:0,byteOffset:offset,byteLength:texture.length});doc.textures=[{extensions:{KHR_texture_basisu:{source:0}}}];doc.extensionsRequired=['KHR_texture_basisu'];
  const json=Buffer.from(JSON.stringify(doc)),jsonBytes=Buffer.alloc(Math.ceil(json.length/4)*4,32);json.copy(jsonBytes);const glb=Buffer.alloc(28+jsonBytes.length+binary.length);glb.writeUInt32LE(0x46546c67,0);glb.writeUInt32LE(2,4);glb.writeUInt32LE(glb.length,8);glb.writeUInt32LE(jsonBytes.length,12);glb.writeUInt32LE(0x4e4f534a,16);jsonBytes.copy(glb,20);glb.writeUInt32LE(binary.length,20+jsonBytes.length);glb.writeUInt32LE(0x004e4942,24+jsonBytes.length);binary.copy(glb,28+jsonBytes.length);const file=path.join(dir,'model.glb');await fs.writeFile(file,glb);assert.equal((await importAsset({source:file,output:path.join(dir,'imports'),size:64})).manifest.kind,'model');
}));
test('CLI and editor bind texture imports through the same scene revision contract',()=>fixture(async dir=>{
  const source=path.join(dir,'source.ktx2');await fs.writeFile(source,ktxFixture());const asset=await importAsset({source,output:path.join(dir,'assets/imported'),size:64}),scene=await loadScene(path.join(asset.directory,'preview.scene.json')),file=path.join(dir,'assets/main.scene.json');await fs.writeFile(file,JSON.stringify(scene));await fs.cp(path.join(asset.directory,'textures'),path.join(dir,'assets/textures'),{recursive:true});
  await main(['attach','--in',asset.directory,'--scene',file,'--revision','0','--material','m0']);const service=createEditorService({root:dir});try{const result=await service.call('asset_attach',{id:asset.manifest.id,scenePath:'assets/main.scene.json',expectedRevision:1,material:'m1'});assert.equal(result.ok,true,JSON.stringify(result));}finally{await service.close();}
}));

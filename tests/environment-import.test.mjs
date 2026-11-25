import test from 'node:test';
import assert from 'node:assert/strict';
import {deflateSync} from 'node:zlib';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {importAsset,verifyImport,attachEnvironment,checkoutSceneImport,inspectAsset} from '../engine/assets/import-service.mjs';
import {loadScene,buildScene,validateScene,undoSceneFile} from '../engine/scene/index.mjs';
import {createEditorService} from '../editor/service.mjs';
import {decodeEnvironment,decodeRgbe,inspectExr,integrateEnvironmentSH,resizeEnvironment,encodeEnvironment,inspectEnvironment,environmentPreview} from '../engine/assets/environment-data.mjs';
import {environmentPixels,encodeRgbeFixture,encodeExrFixture,writeEnvironmentFixture,encodeFloatDdsFixture,encodeFloatKtxFixture} from '../tools/offline/environment-fixture.mjs';
async function fixture(fn){const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ng-environment-'));try{await fn(dir);}finally{await fs.rm(dir,{recursive:true,force:true});}}

test('RGBE flat/RLE panoramas preserve HDR energy and canonical orientation for every signed axis order',async()=>{
  const original=environmentPixels();for(const orientation of ['-Y +X','-Y -X','+Y +X','+Y -X','+X -Y','+X +Y','-X -Y','-X +Y'])for(const rle of [false,true]){const decoded=await decodeEnvironment(encodeRgbeFixture(original,{orientation,rle}),{extension:'.hdr'});assert.deepEqual(decoded.data,original.data);assert.equal(decoded.data[0],8);}
  for(const bytes of [Buffer.from('#?RADIANCE\nFORMAT=invalid\n\n-Y 8 +X 16\n'),encodeRgbeFixture(original,{rle:true}).subarray(0,-1),Buffer.concat([encodeRgbeFixture(original),Buffer.from([0])])])await assert.rejects(decodeEnvironment(bytes,{extension:'.hdr'}));
});
test('HDR environment imports preserve floating data, canonical previews and deterministic identities across HDR/EXR sources',()=>fixture(async dir=>{
  const hdr=await writeEnvironmentFixture(path.join(dir,'hdr')),exr=await writeEnvironmentFixture(path.join(dir,'exr'),{exr:true,zip:true}),output=path.join(dir,'imports'),first=await importAsset({source:hdr,output,size:64}),again=await importAsset({source:hdr,output,size:64}),equivalent=await importAsset({source:exr,output,size:64});assert.equal(first.manifest.id,again.manifest.id);assert.equal(first.manifest.kind,'environment');assert.equal(first.manifest.environmentSha256,equivalent.manifest.environmentSha256);await verifyImport(first.directory);
  const image=inspectEnvironment(await fs.readFile(path.join(first.directory,first.manifest.environment)));assert.equal(image.data.readFloatLE(0),8);const previewBytes=await fs.readFile(path.join(first.directory,'preview.scene.json')),preview=await loadScene(path.join(first.directory,'preview.scene.json'));assert.equal(preview.lighting.environment.sha256,first.manifest.environmentSha256);assert.equal(preview.lighting.toneMap,true);assert.ok((await fs.readFile(path.join(first.directory,'preview.png'))).length>16);
  for(const change of [s=>s.lighting.environment.intensity=2,s=>s.lighting.environment.file='hidden.ngenv']){const bad=structuredClone(preview);change(bad);await fs.writeFile(path.join(first.directory,'preview.scene.json'),JSON.stringify(bad));await assert.rejects(verifyImport(first.directory),/changed|identity|payload/);await fs.writeFile(path.join(first.directory,'preview.scene.json'),previewBytes);await verifyImport(first.directory);}
}));

test('environment editor attachment relocates a global catalog, preserves settings and undo, and builds source-independent scenes',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeEnvironmentFixture(path.join(dir,'source')),output:path.join(dir,'assets/imported'),size:64}),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.lighting;const target=path.join(dir,'assets/scenes/nested/main.scene.json');await fs.mkdir(path.dirname(target),{recursive:true});await fs.writeFile(target,JSON.stringify(scene));
  const service=createEditorService({root:dir});try{const applied=await service.call('asset_attach',{id:asset.manifest.id,scenePath:'assets/scenes/nested/main.scene.json',expectedRevision:0,environmentOptions:{intensity:.75,rotation:180}});assert.equal(applied.ok,true,JSON.stringify(applied));}finally{await service.close();}
  const attached=await loadScene(target),ref=attached.lighting.environment;assert.equal(ref.intensity,.75);assert.equal(ref.rotation,180);assert.equal(ref.file,'imported/'+asset.manifest.id+'/'+asset.manifest.environment);await fs.rename(path.join(dir,'assets/imported'),path.join(dir,'removed-catalog'));
  const built=await buildScene(target,path.join(dir,'packages'));assert.ok(built.manifest.dependencies.some(d=>d.path===ref.file&&d.sha256===ref.sha256));
  const imported=await importAsset({source:path.join(built.packageDirectory,'scene.json'),output:path.join(dir,'scene-imports'),size:64}),opened=await checkoutSceneImport({directory:imported.directory,outputDirectory:path.join(dir,'opened')}),moved=await loadScene(opened.scenePath);assert.match(moved.lighting.environment.file,/^environment\/[a-f0-9]{32}\.ngenv$/);assert.equal(moved.lighting.environment.sha256,ref.sha256);assert.equal(moved.lighting.environment.rotation,180);await buildScene(opened.scenePath,path.join(dir,'rebuilt'));
  const undone=await undoSceneFile(target,{projectId:scene.projectId,expectedRevision:1});assert.equal(undone.scene.lighting,undefined);
}));

test('environment attachment and packaging reject invalid parameters, stale revisions, hashes and escaped dependencies atomically',()=>fixture(async dir=>{
  const asset=await importAsset({source:await writeEnvironmentFixture(path.join(dir,'source')),output:path.join(dir,'catalog'),size:64}),scene=await loadScene(path.join(asset.directory,'preview.scene.json'));delete scene.lighting;const target=path.join(dir,'assets/main.scene.json');await fs.mkdir(path.dirname(target));await fs.writeFile(target,JSON.stringify(scene));
  for(const options of [{intensity:-1},{intensity:10001},{rotation:3601},{rotation:null},{extra:1}]){await assert.rejects(attachEnvironment({directory:asset.directory,scenePath:target,expectedRevision:0,options}));assert.equal((await loadScene(target)).revision,0);assert.equal(await fs.stat(path.join(dir,'assets/imported')).then(()=>true).catch(()=>false),false);}
  await assert.rejects(attachEnvironment({directory:asset.directory,scenePath:target,expectedRevision:1}));assert.equal(await fs.stat(path.join(dir,'assets/imported')).then(()=>true).catch(()=>false),false);
  await attachEnvironment({directory:asset.directory,scenePath:target,expectedRevision:0});const attached=await loadScene(target);for(const edit of [s=>s.lighting.environment.file='../escape.ngenv',s=>s.lighting.environment.sha256='0'.repeat(64),s=>s.lighting.environment.intensity=Infinity,s=>s.lighting.environment.rotation='0']){const bad=structuredClone(attached);edit(bad);if(bad.lighting.environment.sha256==='0'.repeat(64)){await assert.rejects(buildScene(bad,path.join(dir,'packages'),{sceneDirectory:path.dirname(target)}),/changed/);}else assert.equal(validateScene(bad).valid,false);}
  const original=await fs.readFile(path.join(dir,'assets',attached.lighting.environment.file)),outside=path.join(dir,'outside');await fs.mkdir(outside);await fs.writeFile(path.join(outside,'sky.ngenv'),original);await fs.symlink(outside,path.join(dir,'assets/escape'),'junction');attached.lighting.environment.file='escape/sky.ngenv';await assert.rejects(buildScene(attached,path.join(dir,'packages'),{sceneDirectory:path.dirname(target)}),/escaped/);
}));

test('legacy RGBE runs, EXR chromaticities, negative radiance and ignored alpha have explicit linear meaning',async()=>{
  const header=Buffer.from('#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8 +X 16\n'),legacy=Buffer.concat([header,...Array.from({length:8},()=>Buffer.from([128,64,32,131,1,1,1,15]))]),rle=Buffer.concat([header,...Array.from({length:8},()=>Buffer.from([2,2,0,16,144,128,144,64,144,32,144,131]))]);assert.deepEqual(decodeRgbe(legacy).data,environmentPixels(16,8,{constant:[4,2,1]}).data);assert.deepEqual(decodeRgbe(rle).data,decodeRgbe(legacy).data);assert.throws(()=>decodeRgbe(Buffer.concat([header,Buffer.from([1,1,1,1])])));
  const primaries=[.7347,.2653,0,1,.0001,-.077,.32168,.33767],white=await decodeEnvironment(encodeExrFixture(environmentPixels(16,8,{constant:[1,1,1]}),{primaries}),{extension:'.exr'});for(const v of white.data.subarray(0,3))assert.ok(Math.abs(v-1)<.001);
  const image=environmentPixels(16,8,{constant:[-1,2,4]});for(let p=3;p<image.data.length;p+=4)image.data[p]=.5;const decoded=await decodeEnvironment(encodeExrFixture(image),{extension:'.exr'});assert.deepEqual(Array.from(decoded.data.subarray(0,4)),[0,2,4,1]);assert.equal(decoded.warnings.length,2);
});
test('EXR HALF/FLOAT, ZIP/raw chunks and shifted data windows retain north-to-south HDR pixels',async()=>{
  const original=environmentPixels();for(const half of [false,true])for(const zip of [false,true])for(const origin of [[0,0],[-4,17]]){const bytes=encodeExrFixture(original,{half,zip,origin}),info=inspectExr(bytes);assert.equal(info.width,16);const image=await decodeEnvironment(bytes,{extension:'.exr'});assert.deepEqual(image.data,original.data);}
  for(const half of [false,true])for(const zip of [false,true]){const image=await decodeEnvironment(encodeExrFixture(original,{half,zip,channels:['Y']}),{extension:'.exr'});for(let p=0;p<image.data.length;p+=4)assert.deepEqual(Array.from(image.data.subarray(p,p+4)),[original.data[p],original.data[p],original.data[p],1]);}
});
test('floating DDS/KTX2 panoramas auto-detect as environments, retaining HALF/FLOAT, premultiplication and orientation',()=>fixture(async dir=>{
  const image=environmentPixels();let reference;
  for(const half of [false,true])for(const premultiplied of [false,true])for(const extension of ['.dds','.ktx2']){const options={half,premultiplied,alpha:premultiplied?.5:1,zip:true},bytes=extension==='.dds'?encodeFloatDdsFixture(image,options):encodeFloatKtxFixture(image,options),file=path.join(dir,'float-'+half+'-'+premultiplied+extension);await fs.writeFile(file,bytes);assert.equal((await inspectAsset(file)).kind,'environment');const decoded=await decodeEnvironment(bytes,{extension});assert.deepEqual(decoded.data,image.data);const asset=await importAsset({source:file,output:path.join(dir,'imports'),size:64});reference??=asset.manifest.environmentSha256;assert.equal(asset.manifest.environmentSha256,reference);}
  const oriented=await decodeEnvironment(encodeFloatKtxFixture(image,{orientation:'lu',swizzle:'bgr1',zip:true}),{extension:'.ktx2'});for(let y=0;y<image.height;y++)for(let x=0;x<image.width;x++){const p=(y*image.width+x)*4,q=((image.height-1-y)*image.width+image.width-1-x)*4;assert.deepEqual(Array.from(oriented.data.subarray(p,p+4)),[image.data[q+2],image.data[q+1],image.data[q],1]);}
  const opaque=await decodeEnvironment(encodeFloatDdsFixture(image,{alpha:.25,opaque:true}),{extension:'.dds'});assert.deepEqual(opaque.data,image.data);
  const result=await promisify(execFile)(process.execPath,['tools/assets.mjs','import','--in',path.join(dir,'float-false-false.ktx2'),'--out',path.join(dir,'cli'),'--size','64'],{windowsHide:true});assert.equal(JSON.parse(result.stdout).result.manifest.kind,'environment');
}));
test('floating KTX2 environment chromaticities convert wide gamuts to linear Rec.709 and reject unknown declarations',async()=>{
  const gamuts=new Map([[4,[.708,.292,.170,.797,.131,.046,.3127,.3290]],[6,[.7347,.2653,0,1,.0001,-.077,.32168,.33767]],[7,[.713,.293,.165,.830,.128,.044,.32168,.33767]],[10,[.680,.320,.265,.690,.150,.060,.3127,.3290]],[11,[.640,.330,.210,.710,.150,.060,.3127,.3290]]]);
  for(const [primaries,coordinates]of gamuts){const source=environmentPixels(16,8,{constant:[2,.5,1]}),ktx=await decodeEnvironment(encodeFloatKtxFixture(source,{primaries}),{extension:'.ktx2'}),exr=await decodeEnvironment(encodeExrFixture(source,{primaries:coordinates}),{extension:'.exr'});for(let i=0;i<ktx.data.length;i++)assert.ok(Math.abs(ktx.data[i]-exr.data[i])<.00001);const white=await decodeEnvironment(encodeFloatKtxFixture(environmentPixels(16,8,{constant:[2,2,2]}),{primaries}),{extension:'.ktx2'});for(const v of white.data.subarray(0,3))assert.ok(Math.abs(v-2)<.001);if(primaries===4){const expected=[2.9543115,.309,1.0321385];for(let c=0;c<3;c++)assert.ok(Math.abs(ktx.data[c]-expected[c])<.001);}}
  const xyz=await decodeEnvironment(encodeFloatKtxFixture(environmentPixels(16,8,{constant:[.95047,1,1.08883]}),{primaries:5}),{extension:'.ktx2'});for(const v of xyz.data.subarray(0,3))assert.ok(Math.abs(v-1)<.00001);
  const unspecified=await decodeEnvironment(encodeFloatKtxFixture(environmentPixels(),{primaries:0}),{extension:'.ktx2'});assert.deepEqual(unspecified.data,environmentPixels().data);assert.match(unspecified.warnings[0],/unspecified/);await assert.rejects(decodeEnvironment(encodeFloatKtxFixture(environmentPixels(),{primaries:255}),{extension:'.ktx2'}),/primaries/);
  const badModel=encodeFloatKtxFixture(environmentPixels());badModel[badModel.readUInt32LE(48)+12]=2;await assert.rejects(decodeEnvironment(badModel,{extension:'.ktx2'}),/color model/);
});

test('environment SH conserves exact constant irradiance and reduction averages linear spherical energy',()=>{
  const image=environmentPixels(16,8,{constant:[2,3,4]}),sh=integrateEnvironmentSH(image);for(const n of [[1,0,0],[0,1,0],[0,0,1],[-1,0,0]]){const [x,y,z]=n,basis=[.282095,.488603*y,.488603*z,.488603*x,1.092548*x*y,1.092548*y*z,.315392*(3*z*z-1),1.092548*x*z,.546274*(x*x-y*y)];for(let c=0;c<3;c++){const value=sh.reduce((sum,row,i)=>sum+row[c]*basis[i],0);assert.ok(Math.abs(value-[2,3,4][c]*Math.PI)<.0001);}}
  const smaller=resizeEnvironment(image,4);assert.equal(smaller.width,4);assert.equal(smaller.height,2);for(let p=0;p<smaller.data.length;p+=4)assert.deepEqual(Array.from(smaller.data.subarray(p,p+4)),[2,3,4,1]);
  const preview=environmentPreview(image);assert.ok(preview.data[0]<preview.data[1]&&preview.data[1]<preview.data[2]&&preview.data[2]<255,'Display preview must tone map rather than clip HDR channels');
});
test('NGE1 stores float radiance and convolved SH, rejecting header, ranges, padding and nonfinite payloads',()=>{
  const original=environmentPixels(),bytes=encodeEnvironment(original),decoded=inspectEnvironment(bytes);assert.equal(decoded.width,16);assert.equal(decoded.data.readFloatLE(0),8);assert.deepEqual(decoded.diffuseSH,integrateEnvironmentSH(original).map(row=>row.map(Math.fround)));
  for(const edit of [b=>b.writeUInt32LE(3,4),b=>b.writeUInt32LE(100000,8),b=>b[140]=1,b=>b.writeFloatLE(NaN,160),b=>b.writeFloatLE(-1,160),b=>b.writeFloatLE(.5,172),b=>b.writeFloatLE(Infinity,32)]){const bad=Buffer.from(bytes);edit(bad);assert.throws(()=>inspectEnvironment(bad));}assert.throws(()=>inspectEnvironment(bytes.subarray(0,-1)));
});
test('EXR preflight rejects huge/deep/multipart sources, corrupt offsets, unknown color layers and ZIP bombs',async()=>{
  const bytes=encodeExrFixture(environmentPixels(),{zip:true});for(const edit of [b=>b.writeUInt32LE(2|0x1000,4),b=>b.writeUInt32LE(2|0x800,4),b=>b[b.indexOf(Buffer.from('dataWindow\0box2i\0'))+18]=255]){const bad=Buffer.from(bytes);edit(bad);assert.throws(()=>inspectExr(bad));}
  await assert.rejects(decodeEnvironment(bytes.subarray(0,-3),{extension:'.exr'}));const bad=encodeExrFixture(environmentPixels());bad.writeFloatLE(NaN,bad.length-4);await assert.rejects(decodeEnvironment(bad,{extension:'.exr'}),/Nonfinite/);
  const dw=Buffer.from('dataWindow\0box2i\0'),large=Buffer.from(bytes),window=large.indexOf(dw)+dw.length+4;large.writeInt32LE(32767,window+8);large.writeInt32LE(16383,window+12);assert.throws(()=>inspectExr(large),/bounded/);
  const layers=Buffer.from(bytes);layers[layers.indexOf(Buffer.from('R\0'))]='Q'.charCodeAt(0);assert.throws(()=>inspectExr(layers),/requires RGB/);
  let table=8;while(bytes[table]!==0){table=bytes.indexOf(0,table)+1;table=bytes.indexOf(0,table)+1;const n=bytes.readUInt32LE(table);table+=4+n;}table++;const offset=Number(bytes.readBigUInt64LE(table)),bomb=deflateSync(Buffer.alloc(65536)),corrupt=Buffer.concat([bytes.subarray(0,offset+8),bomb]);corrupt.writeUInt32LE(bomb.length,offset+4);assert.throws(()=>inspectExr(corrupt),/ZIP/);
  const overlap=Buffer.from(bytes);overlap.writeBigUInt64LE(8n,table);assert.throws(()=>inspectExr(overlap),/overlaps/);
});

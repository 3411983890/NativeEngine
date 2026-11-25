// Original two-part textured model for the external import incremental smoke.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { encodePng } from './textures.mjs';
import { makeAnimationFixture } from './offline/animation-fixture.mjs';
import { readAccessor } from '../engine/assets/gltf-document.mjs';
import { MeshoptEncoder } from 'meshoptimizer/encoder';
export async function writeAssetFixture(directory='build/asset-import-fixture') {
  await fs.mkdir(directory,{recursive:true});
  const p=[-1,0,-1,1,0,-1,1,0,1,-1,0,1,0,2,0],uv=[0,1,1,1,1,0,0,0,.5,.5],indices=[0,4,1,1,4,2,2,4,3,3,4,0,0,1,2,0,2,3];
  const binary=Buffer.alloc(p.length*4+uv.length*4+indices.length*2);let offset=0;
  for(const value of [...p,...uv]){binary.writeFloatLE(value,offset);offset+=4;}
  for(const value of indices){binary.writeUInt16LE(value,offset);offset+=2;}
  const png=encodePng({width:2,height:2,data:Buffer.from([255,255,255,255,40,40,40,255,40,40,40,255,255,255,255,255])});
  const doc={asset:{version:'2.0',generator:'nativegpu-original-external-asset-fixture'},scene:0,scenes:[{nodes:[0,1]}],
    nodes:[{name:'橙色金字塔',translation:[-2,0,-1],mesh:0},{name:'蓝色金字塔',translation:[2,0,-1],scale:[.75,1.5,.75],mesh:1}],
    meshes:[0,1].map(material=>({primitives:[{attributes:{POSITION:0,TEXCOORD_0:1},indices:2,material}]})),
    materials:[[1,.35,.08,1],[.08,.5,1,1]].map(baseColorFactor=>({doubleSided:true,pbrMetallicRoughness:{baseColorFactor,baseColorTexture:{index:0},metallicFactor:0,roughnessFactor:.7}})),
    images:[{uri:'data:image/png;base64,'+png.toString('base64')}],textures:[{source:0}],
    accessors:[{bufferView:0,type:'VEC3',componentType:5126,count:5},{bufferView:1,type:'VEC2',componentType:5126,count:5},{bufferView:2,type:'SCALAR',componentType:5123,count:indices.length}],
    bufferViews:[{buffer:0,byteOffset:0,byteLength:60},{buffer:0,byteOffset:60,byteLength:40},{buffer:0,byteOffset:100,byteLength:indices.length*2}],
    buffers:[{byteLength:binary.length,uri:'data:application/octet-stream;base64,'+binary.toString('base64')}]};
  const file=path.join(directory,'pyramids.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}
export async function writeCharacterFixture(directory='build/asset-import-fixture') {
  await fs.mkdir(directory,{recursive:true});
  const doc=makeAnimationFixture(),bytes=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64');
  const original={document:structuredClone(doc),buffers:[bytes]},primitive=doc.meshes[0].primitives[0];
  function append(rows,type) {
    const base=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64'),padding=Buffer.alloc((4-base.length%4)%4),data=Buffer.alloc(rows.length*rows[0].length*4);rows.flat().forEach((v,i)=>data.writeFloatLE(v,i*4));
    const all=Buffer.concat([base,padding,data]),view=doc.bufferViews.length,index=doc.accessors.length;
    doc.bufferViews.push({buffer:0,byteOffset:base.length+padding.length,byteLength:data.length});doc.accessors.push({bufferView:view,type,componentType:5126,count:rows.length});doc.buffers[0]={byteLength:all.length,uri:'data:application/octet-stream;base64,'+all.toString('base64')};return index;
  }
  const positions=readAccessor(original,primitive.attributes.POSITION,'VEC3');
  const first=structuredClone(primitive),second=structuredClone(primitive);
  first.attributes.POSITION=append(positions.map(p=>[p[0]-.8,p[1],p[2]]),'VEC3');first.material=0;
  second.attributes.POSITION=append(positions.map(p=>[p[0]+.8,p[1],p[2]]),'VEC3');second.material=1;
  doc.meshes=[{primitives:[first]},{primitives:[second]}];
  const binds=readAccessor(original,doc.skins[0].inverseBindMatrices,'MAT4').map(m=>m.map((v,i)=>i===12?v+.15:v));doc.skins.push({...doc.skins[0],inverseBindMatrices:append(binds,'MAT4')});
  doc.nodes.push({name:'SecondSkinnedMesh',mesh:1,skin:1});doc.scenes[0].nodes.push(doc.nodes.length-1);
  const delta=positions.map(p=>[0,p[1]>1.3?.18:0,0]);
  for(const mesh of doc.meshes){mesh.primitives[0].targets=[{POSITION:append(delta,'VEC3')}];mesh.extras={targetNames:['expression']};mesh.weights=[.2];}
  const input=append([[0],[1]],'SCALAR'),output=append([[0],[1]],'SCALAR');
  for(const animation of doc.animations) {
    animation.channels.push({sampler:animation.samplers.length,target:{node:doc.nodes.length-2,path:'weights'}});
    animation.samplers.push({input,output,interpolation:'LINEAR'});
  }
  doc.materials=[[1,.08,.03,1],[.03,.2,1,1]].map(baseColorFactor=>({doubleSided:true,pbrMetallicRoughness:{baseColorFactor,metallicFactor:0,roughnessFactor:.7}}));
  const file=path.join(directory,'multi-skin.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}
export async function writeCompressedAssetFixture(directory='build/asset-import-fixture/compressed') {
  const file=await writeAssetFixture(directory),doc=JSON.parse(await fs.readFile(file,'utf8')),input=Buffer.from(doc.buffers[0].uri.split(',')[1],'base64');
  await MeshoptEncoder.ready;const chunks=[];let offset=0;
  for(const [i,view]of doc.bufferViews.entries()) {
    const accessor=doc.accessors[i],stride=i===0?12:i===1?8:2,mode=i===2?'TRIANGLES':'ATTRIBUTES';
    const bytes=Buffer.from(MeshoptEncoder.encodeGltfBuffer(input.subarray(view.byteOffset,view.byteOffset+view.byteLength),accessor.count,stride,mode,0));
    view.buffer=1;view.extensions={EXT_meshopt_compression:{buffer:0,byteOffset:offset,byteLength:bytes.length,byteStride:stride,count:accessor.count,mode}};
    chunks.push(bytes);offset+=bytes.length;
  }
  const bytes=Buffer.concat(chunks);doc.buffers=[{byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')},{byteLength:input.length,extensions:{EXT_meshopt_compression:{fallback:true}}}];
  doc.extensionsUsed=['EXT_meshopt_compression'];doc.extensionsRequired=['EXT_meshopt_compression'];
  const compressed=path.join(directory,'pyramids-meshopt.gltf');await fs.writeFile(compressed,JSON.stringify(doc)+'\n');return compressed;
}
export async function writeCutoutAssetFixture(masked,directory='build/asset-import-fixture/cutout') {
  await fs.mkdir(directory,{recursive:true});
  const binary=Buffer.alloc(48+32+12);[ -1,0,0, 1,0,0, 1,2,0, -1,2,0, 0,1, 1,1, 1,0, 0,0 ].forEach((v,i)=>binary.writeFloatLE(v,i*4));
  [0,1,2,0,2,3].forEach((v,i)=>binary.writeUInt16LE(v,80+i*2));
  const pixels=Buffer.alloc(16*16*4);for(let y=0;y<16;y++)for(let x=0;x<16;x++)pixels.set([255,50,15,x<8?0:255],(y*16+x)*4);
  const png=encodePng({width:16,height:16,data:pixels}),doc={asset:{version:'2.0'},scene:0,scenes:[{nodes:[0]}],nodes:[{name:'Alpha panel',mesh:0}],
    meshes:[{primitives:[{attributes:{POSITION:0,TEXCOORD_0:1},indices:2,material:0}]}],materials:[{alphaMode:masked?'MASK':'OPAQUE',alphaCutoff:.5,doubleSided:true,pbrMetallicRoughness:{baseColorTexture:{index:0},metallicFactor:0,roughnessFactor:1}}],
    images:[{uri:'data:image/png;base64,'+png.toString('base64')}],textures:[{source:0}],buffers:[{byteLength:binary.length,uri:'data:application/octet-stream;base64,'+binary.toString('base64')}],
    bufferViews:[{buffer:0,byteOffset:0,byteLength:48},{buffer:0,byteOffset:48,byteLength:32},{buffer:0,byteOffset:80,byteLength:12}],accessors:[{bufferView:0,count:4,componentType:5126,type:'VEC3'},{bufferView:1,count:4,componentType:5126,type:'VEC2'},{bufferView:2,count:6,componentType:5123,type:'SCALAR'}]};
  const file=path.join(directory,masked?'mask.gltf':'opaque.gltf');await fs.writeFile(file,JSON.stringify(doc)+'\n');return file;
}
export async function writeMotionFixture(directory='build/asset-import-fixture/motion') {
  const characterSource=await writeCharacterFixture(directory),doc=JSON.parse(await fs.readFile(characterSource,'utf8'));
  doc.extras={nativegpu:{controller:Object.fromEntries(['idle','walk','run','jump','fall'].map(state=>[state,'idle']))}};
  await fs.writeFile(characterSource,JSON.stringify(doc)+'\n');
  const positions=Buffer.alloc(24),rotations=Buffer.alloc(32),times=Buffer.alloc(8);
  [0,0,0,50,0,0].forEach((v,i)=>positions.writeFloatLE(v,i*4));[0,0,0,1,0,0,Math.SQRT1_2,Math.SQRT1_2].forEach((v,i)=>rotations.writeFloatLE(v,i*4));times.writeFloatLE(1,4);
  const bytes=Buffer.concat([positions,rotations,times]),motion=makeAnimationFixture();motion.nodes.pop();motion.nodes.forEach(node=>{node.name='Vendor:'+node.name;node.translation=node.translation.map(v=>v*100);});
  motion.meshes=[];motion.skins=[];motion.scenes=[{nodes:[0]}];motion.buffers=[{byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')}];
  motion.bufferViews=[{buffer:0,byteLength:24},{buffer:0,byteOffset:24,byteLength:32},{buffer:0,byteOffset:56,byteLength:8}];
  motion.accessors=[{bufferView:0,type:'VEC3',componentType:5126,count:2},{bufferView:1,type:'VEC4',componentType:5126,count:2},{bufferView:2,type:'SCALAR',componentType:5126,count:2}];
  motion.animations=[{name:'Raise',channels:[{target:{node:0,path:'translation'},sampler:0},{target:{node:4,path:'rotation'},sampler:1}],samplers:[{input:2,output:0},{input:2,output:1}]}];
  const source=path.join(directory,'vendor.gltf');await fs.writeFile(source,JSON.stringify(motion)+'\n');
  const bvhSource=path.join(directory,'walk.bvh');await fs.writeFile(bvhSource,`HIERARCHY
ROOT Hips { OFFSET 0 82 0 CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
 JOINT Spine2 { OFFSET 0 34 0 CHANNELS 3 Zrotation Xrotation Yrotation
  JOINT Head { OFFSET 0 43 0 CHANNELS 3 Zrotation Xrotation Yrotation End Site { OFFSET 0 10 0 } }
 }
}
MOTION
Frames: 3
Frame Time: 0.5
0 0 0 0 0 0 0 0 0 0 0 0
25 0 0 0 0 0 45 0 0 0 0 0
50 0 0 0 0 0 90 0 0 0 0 0
`);return {characterSource,source,bvhSource};
}
/**
 * Original ASCII FBX fixture: one textured quad plus a second untextured material.
 *
 * Written by hand so the import tests do not depend on a vendor file. `unit` and
 * `axes` reproduce the two document settings Assimp drops, which is exactly what
 * the FBX metadata reader has to recover.
 */
export async function writeFbxFixture(directory='build/asset-import-fixture/fbx',options={}) {
  const {unitScaleFactor=100,upAxis=1,frontAxis=2,texture=true,textureName='wood.png'}=options;
  await fs.mkdir(directory,{recursive:true});
  const png=encodePng({width:4,height:4,data:Buffer.from(Array.from({length:16},(_,i)=>[i*16,(i%4)*60,255-i*16,255]).flat())});
  if(texture)await fs.writeFile(path.join(directory,textureName),png);
  const source=`; FBX 7.4.0 project file
; ----------------------------------------------------

FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",${upAxis}
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",${frontAxis}
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",${unitScaleFactor}
	}
}
Objects:  {
	Geometry: 100, "Geometry::Plane", "Mesh" {
		Vertices: *12 {
			a: -1,0,-1,1,0,-1,1,0,1,-1,0,1
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *12 {
				a: 0,1,0,0,1,0,0,1,0,0,1,0
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "UVChannel_1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *4 {
				a: 0,1,2,3
			}
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygon"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {
				a: 0
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	Model: 200, "Model::Plane", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
		}
		Shading: T
		Culling: "CullingOff"
	}
	Material: 300, "Material::Wood", "" {
		Version: 102
		ShadingModel: "phong"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.8,0.6,0.4
			P: "DiffuseFactor", "Number", "", "A",1
			P: "SpecularFactor", "Number", "", "A",0.2
			P: "ShininessExponent", "Number", "", "A",20
			P: "EmissiveColor", "Color", "", "A",0,0,0
		}
	}
	Texture: 400, "Texture::WoodDiffuse", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::WoodDiffuse"
		FileName: "C:\\\\library\\\\wood.png"
		RelativeFilename: "${textureName}"
		ModelUVTranslation: 0,0
		ModelUVScaling: 1,1
		Texture_Alpha_Source: "None"
	}
}
Connections:  {
	C: "OO",100,200
	C: "OO",200,0
	C: "OO",300,200
	C: "OP",400,300, "DiffuseColor"
}
`;
  const file=path.join(directory,'plane.fbx');await fs.writeFile(file,source);return {file,texturePath:path.join(directory,textureName)};
}
if(process.argv[1]?.endsWith('asset-fixtures.mjs'))console.log(await writeAssetFixture(process.argv[2]));

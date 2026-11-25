import {requireAsset,AssetError} from './gltf-document.mjs';

const meshoptNames=['EXT_meshopt_compression','KHR_meshopt_compression'];
const integer=(v,lo,hi)=>Number.isSafeInteger(v)&&v>=lo&&v<=hi;
const componentTypes={5120:['DracoInt8Array','GetAttributeInt8ForAllPoints',1,'writeInt8'],5121:['DracoUInt8Array','GetAttributeUInt8ForAllPoints',1,'writeUInt8'],
  5122:['DracoInt16Array','GetAttributeInt16ForAllPoints',2,'writeInt16LE'],5123:['DracoUInt16Array','GetAttributeUInt16ForAllPoints',2,'writeUInt16LE'],
  5125:['DracoUInt32Array','GetAttributeUInt32ForAllPoints',4,'writeUInt32LE'],5126:['DracoFloat32Array','GetAttributeFloatForAllPoints',4,'writeFloatLE']};
let dracoReady;
export async function decodeGltfCompression(asset) {
  const {document:doc,buffers}=asset,handled=new Set();let decodedBytes=0;
  function allocate(bytes) {
    requireAsset(integer(bytes,1,256*1024*1024)&&decodedBytes+bytes<=256*1024*1024,'Decoded glTF geometry exceeds 256 MiB','BUDGET');
    decodedBytes+=bytes;return Buffer.alloc(bytes);
  }
  function slice(buffer,offset,length) {
    requireAsset(buffers[buffer]&&integer(offset,0,buffers[buffer].length)&&integer(length,1,buffers[buffer].length-offset),'Compressed geometry exceeds its buffer');
    return buffers[buffer].subarray(offset,offset+length);
  }
  function appendBuffer(bytes) {const index=buffers.length;buffers.push(bytes);doc.buffers.push({byteLength:bytes.length});return index;}
  function appendAccessor(bytes,spec) {
    const buffer=appendBuffer(bytes),view=doc.bufferViews.length,index=doc.accessors.length;
    doc.bufferViews.push({buffer,byteOffset:0,byteLength:bytes.length});doc.accessors.push({...spec,bufferView:view,byteOffset:0});delete doc.accessors[index].sparse;return index;
  }
  try {
    for(const view of doc.bufferViews??[])for(const name of meshoptNames) {
      const ext=view.extensions?.[name];if(!ext)continue;
      requireAsset(integer(ext.count,1,3000000)&&integer(ext.byteStride,1,252)&&ext.count*ext.byteStride===view.byteLength,'Invalid meshopt decoded layout');
      requireAsset(['ATTRIBUTES','TRIANGLES','INDICES'].includes(ext.mode)&&['NONE','OCTAHEDRAL','QUATERNION','EXPONENTIAL','COLOR'].includes(ext.filter??'NONE'),'Invalid meshopt mode/filter');
      if(ext.mode==='ATTRIBUTES')requireAsset(ext.byteStride%4===0,'Meshopt attribute stride must be aligned');
      else requireAsset([2,4].includes(ext.byteStride)&&(ext.mode!=='TRIANGLES'||ext.count%3===0)&&(ext.filter??'NONE')==='NONE','Invalid meshopt index layout');
      const input=slice(ext.buffer,ext.byteOffset??0,ext.byteLength),output=allocate(view.byteLength);
      const {MeshoptDecoder}=await import('meshoptimizer/decoder');await MeshoptDecoder.ready;
      requireAsset(MeshoptDecoder.supported,'WebAssembly meshopt decoder is unavailable','UNSUPPORTED');
      MeshoptDecoder.decodeGltfBuffer(output,ext.count,ext.byteStride,input,ext.mode,ext.filter??'NONE');
      view.buffer=appendBuffer(output);view.byteOffset=0;
      if(ext.mode==='ATTRIBUTES')view.byteStride=ext.byteStride;else delete view.byteStride;
      delete view.extensions[name];handled.add(name);
    }
    for(const mesh of doc.meshes??[])for(const primitive of mesh.primitives??[]) {
      const ext=primitive.extensions?.KHR_draco_mesh_compression;if(!ext)continue;
      requireAsset((primitive.mode??4)===4&&ext.attributes&&typeof ext.attributes==='object','Draco import requires a triangle mesh');
      const view=doc.bufferViews?.[ext.bufferView];requireAsset(view,'Missing Draco bufferView');
      const input=slice(view.buffer,view.byteOffset??0,view.byteLength);
      for(const index of Object.values(primitive.attributes??{}))requireAsset(integer(doc.accessors?.[index]?.count,1,1000000),'Draco accessor exceeds vertex budget','BUDGET');
      dracoReady??=import('draco3dgltf').then(({default:draco})=>draco.createDecoderModule({}));
      const module=await dracoReady,decoder=new module.Decoder(),buffer=new module.DecoderBuffer(),geometry=new module.Mesh();let status;
      try {
        buffer.Init(input,input.length);requireAsset(decoder.GetEncodedGeometryType(buffer)===module.TRIANGULAR_MESH,'Draco point clouds are not triangle surfaces','UNSUPPORTED');
        status=decoder.DecodeBufferToMesh(buffer,geometry);requireAsset(status.ok(),`Draco decode failed: ${status.error_msg()}`);
        const count=geometry.num_points(),faces=geometry.num_faces();requireAsset(integer(count,1,1000000)&&integer(faces,1,1000000),'Draco decoded geometry exceeds budget','BUDGET');
        for(const [semantic,id]of Object.entries(ext.attributes)) {
          const spec=doc.accessors?.[primitive.attributes?.[semantic]],type=componentTypes[spec?.componentType],components={SCALAR:1,VEC2:2,VEC3:3,VEC4:4}[spec?.type];
          requireAsset(spec&&type&&components&&spec.count===count&&integer(id,0,2147483647),'Draco attribute/accessor mismatch');
          const attribute=decoder.GetAttributeByUniqueId(geometry,id);
          requireAsset(attribute?.ptr&&attribute.num_components()===components,'Draco attribute is missing or has the wrong width');
          const output=allocate(count*components*type[2]),values=new module[type[0]]();
          try {
            requireAsset(decoder[type[1]](geometry,attribute,values)&&values.size()===count*components,'Draco attribute decode failed');
            for(let i=0;i<values.size();i++){const value=values.GetValue(i);requireAsset(Number.isFinite(value),'Non-finite Draco attribute');output[type[3]](value,i*type[2]);}
          }finally{module.destroy(values);}
          primitive.attributes[semantic]=appendAccessor(output,spec);
        }
        const spec=primitive.indices===undefined?{type:'SCALAR',componentType:5125,count:faces*3}:doc.accessors?.[primitive.indices];
        requireAsset(spec?.type==='SCALAR'&&[5121,5123,5125].includes(spec.componentType)&&spec.count===faces*3&&!spec.normalized,'Invalid Draco indices');
        const type=componentTypes[spec.componentType],output=allocate(spec.count*type[2]),face=new module.DracoInt32Array();
        try{for(let i=0;i<faces;i++){requireAsset(decoder.GetFaceFromMesh(geometry,i,face),'Draco triangle decode failed');for(let k=0;k<3;k++){const value=face.GetValue(k);requireAsset(integer(value,0,count-1),'Draco index out of range');output[type[3]](value,(i*3+k)*type[2]);}}}finally{module.destroy(face);}
        primitive.indices=appendAccessor(output,spec);delete primitive.extensions.KHR_draco_mesh_compression;
        if(!Object.keys(primitive.extensions).length)delete primitive.extensions;
        handled.add('KHR_draco_mesh_compression');
      }finally{if(status)module.destroy(status);module.destroy(geometry);module.destroy(buffer);module.destroy(decoder);}
    }
    for(const key of ['extensionsUsed','extensionsRequired'])if(doc[key])doc[key]=doc[key].filter(name=>!handled.has(name));
    return {...asset,decodedCompression:[...handled].sort(),decodedBytes};
  }catch(error){if(error instanceof AssetError)throw error;throw new AssetError('FORMAT',`Compressed geometry decode failed: ${error.message}`);}
}

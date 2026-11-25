import {multiplyMatrix,readAccessor} from '../../engine/assets/gltf-document.mjs';

// Encode a canonical metre/Y-up source as centimetre/Z-up/-Y-front data.
// Kept explicit and independent of the production coordinate helper.
export function encodeCentimeterZUp(asset) {
  const doc=structuredClone(asset.document),buffers=asset.buffers.map(b=>Buffer.from(b));
  const position=v=>[v[0]*100,-v[2]*100,v[1]*100],direction=v=>[v[0],-v[2],v[1]],rotation=q=>[q[0],-q[2],q[1],q[3]];
  const basis=[100,0,0,0,0,0,100,0,0,-100,0,0,0,0,0,1],inverse=[.01,0,0,0,0,0,-.01,0,0,.01,0,0,0,0,0,1];
  const matrix=m=>multiplyMatrix(basis,multiplyMatrix(m,inverse)),jobs=new Map();
  const job=(index,type,convert)=>{if(index!==undefined&&!jobs.has(index))jobs.set(index,{type,convert});};
  for(const node of doc.nodes??[]) {
    if(node.matrix)node.matrix=matrix(node.matrix);
    else{if(node.translation)node.translation=position(node.translation);if(node.rotation)node.rotation=rotation(node.rotation);if(node.scale)node.scale=[node.scale[0],node.scale[2],node.scale[1]];}
  }
  for(const mesh of doc.meshes??[])for(const primitive of mesh.primitives??[]) {
    job(primitive.attributes.POSITION,'VEC3',position);job(primitive.attributes.NORMAL,'VEC3',direction);
    job(primitive.attributes.TANGENT,'VEC4',v=>[...direction(v),v[3]]);
    for(const target of primitive.targets??[]){job(target.POSITION,'VEC3',position);job(target.NORMAL,'VEC3',direction);}
  }
  for(const skin of doc.skins??[])job(skin.inverseBindMatrices,'MAT4',matrix);
  for(const animation of doc.animations??[]) {
    for(const channel of animation.channels??[])if(['translation','rotation'].includes(channel.target.path))job(animation.samplers[channel.sampler].output,channel.target.path==='rotation'?'VEC4':'VEC3',channel.target.path==='rotation'?rotation:position);
    if(animation.extras?.nativegpu?.motion?.velocity)animation.extras.nativegpu.motion.velocity=position(animation.extras.nativegpu.motion.velocity);
  }
  const feature=doc.extras?.nativegpu;
  for(const leg of feature?.rig?.legs??[]){if(leg.pole)leg.pole=position(leg.pole);if(leg.soleHeight!==undefined)leg.soleHeight*=100;}
  for(const region of feature?.softRegions??[]){region.center=position(region.center);if(region.radius!==undefined)region.radius*=100;if(region.maxOffset!==undefined)region.maxOffset*=100;}
  doc.buffers=doc.buffers??[];doc.bufferViews??=[];doc.accessors??=[];
  // New dense float accessors also support sparse or quantized public inputs.
  for(const [index,{type,convert}]of jobs) {
    const values=readAccessor(asset,index,type).map(convert),bytes=Buffer.alloc(values.length*values[0].length*4);values.flat().forEach((v,i)=>bytes.writeFloatLE(v,i*4));
    const buffer=buffers.length,view=doc.bufferViews.length;buffers.push(bytes);doc.buffers.push({byteLength:bytes.length});doc.bufferViews.push({buffer,byteLength:bytes.length});
    const min=values[0].map(()=>Infinity),max=values[0].map(()=>-Infinity);for(let row=0;row<values.length;row++)for(let i=0;i<min.length;i++){const value=bytes.readFloatLE((row*min.length+i)*4);min[i]=Math.min(min[i],value);max[i]=Math.max(max[i],value);}
    doc.accessors[index]={componentType:5126,type,count:values.length,bufferView:view,min,max};
  }
  doc.buffers=buffers.map(bytes=>({byteLength:bytes.length,uri:'data:application/octet-stream;base64,'+bytes.toString('base64')}));
  return doc;
}

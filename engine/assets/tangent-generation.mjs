import {requireAsset,readAccessor} from './gltf-document.mjs';
import {sourceTextureCoordinate} from './texture-coordinates.mjs';
import {effectiveSourceMaterial} from './material-source.mjs';
let modulePromise;
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const unit=v=>{const length=Math.hypot(...v);return length>1e-12?v.map(x=>x/length):[0,1,0];};
function triangles(raw,mode){requireAsset([4,5,6].includes(mode),'Normal-mapped primitive must use triangles','UNSUPPORTED');if(mode===4){requireAsset(raw.length%3===0,'Incomplete tangent triangles');return raw;}const out=[];for(let i=2;i<raw.length;i++)out.push(...(mode===6?[raw[0],raw[i-1],raw[i]]:i%2===0?[raw[i-2],raw[i-1],raw[i]]:[raw[i-1],raw[i-2],raw[i]]));return out;}
async function generate(p,n,uv){modulePromise??=import('three/examples/jsm/libs/mikktspace.module.js').then(async m=>{await m.ready;return m;});const result=(await modulePromise).generateTangents(new Float32Array(p.flat()),new Float32Array(n.flat()),new Float32Array(uv.flat()));requireAsset(result.length===p.length*4,'MikkTSpace output size differs');return Array.from({length:p.length},(_,i)=>{const row=Array.from(result.subarray(i*4,i*4+4));
  // Collapsed UV/geometry corners have no defined tangent. MikkTSpace can
  // return a finite zero vector there; choose a stable orthogonal frame.
  if(row.every(Number.isFinite)&&Math.abs(row[3])===1&&Math.hypot(...row.slice(0,3))<1e-12){const normal=unit(n[i]),axis=Math.abs(normal[0])<.8?[1,0,0]:[0,1,0];row.splice(0,3,...unit(cross(axis,normal)));}
  requireAsset(row.every(Number.isFinite)&&Math.abs(row[3])===1&&Math.abs(Math.hypot(...row.slice(0,3))-1)<.02,'MikkTSpace produced an invalid frame');return [...row.slice(0,3),-row[3]];});}

// Generate in glTF source space, before either importer changes axes or skins.
// A source vertex may split at a tangent or morph seam; all attributes, weights
// and shape deltas use the same corner remapping.
export async function generateMissingTangents(asset){
  const source=asset.document;let doc,buffers;const warnings=[...(asset.tangentWarnings??[])],vertexRemaps={...(asset.tangentVertexRemaps??{})};
  const selected=new Set(),visited=new Set(),visit=index=>{if(visited.has(index))return;const node=source.nodes?.[index];if(!node)return;visited.add(index);if(node.mesh!==undefined)selected.add(node.mesh);for(const child of node.children??[])visit(child);};for(const index of source.scenes?.[source.scene??0]?.nodes??[])visit(index);
  const append=(rows,type,componentType=5126,normalized=false)=>{const sizes={5120:1,5121:1,5122:2,5123:2,5125:4,5126:4},methods={5120:'writeInt8',5121:'writeUInt8',5122:'writeInt16LE',5123:'writeUInt16LE',5125:'writeUInt32LE',5126:'writeFloatLE'},max={5120:127,5121:255,5122:32767,5123:65535},size=sizes[componentType];requireAsset(size,'Unsupported generated attribute type');const bytes=Buffer.alloc(rows.length*rows[0].length*size);rows.forEach((row,i)=>row.forEach((v,k)=>bytes[methods[componentType]](normalized?Math.round(v*max[componentType]):v,(i*row.length+k)*size)));const buffer=buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;buffers.push(bytes);doc.buffers.push({byteLength:bytes.length});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,type,componentType,count:rows.length,...(normalized?{normalized:true}:{})});return index;};
  for(const [mi,mesh]of (source.meshes??[]).entries()){
    if(!selected.has(mi))continue;requireAsset(Array.isArray(mesh?.primitives),'Selected mesh has no primitives');
    for(const [pi,primitive]of mesh.primitives.entries()){
    const attrs=primitive.attributes??{},material=effectiveSourceMaterial(source.materials?.[primitive.material]),info=material?.normalTexture??material?.extensions?.KHR_materials_clearcoat?.clearcoatNormalTexture;
    if(attrs.NORMAL!==undefined&&(!info||attrs.TANGENT!==undefined))continue;
    if(!doc){doc=structuredClone(source);buffers=[...asset.buffers];doc.accessors??=[];doc.bufferViews??=[];doc.buffers??=[];}
    const p=readAccessor(asset,attrs.POSITION,'VEC3'),raw=primitive.indices===undefined?p.map((_,i)=>i):readAccessor(asset,primitive.indices,'SCALAR').flat();requireAsset(raw.every(i=>Number.isSafeInteger(i)&&i>=0&&i<p.length),'Invalid tangent triangle index');const indices=triangles(raw,primitive.mode??4),set=sourceTextureCoordinate(info),uvIndex=attrs['TEXCOORD_'+set];if(info)requireAsset(uvIndex!==undefined,`Material requires missing TEXCOORD_${set} UV stream`,'DEPENDENCY');const uv=info?readAccessor(asset,uvIndex,'VEC2'):[];requireAsset(!info||uv.length===p.length,'Tangent UV count differs');
    const targets=primitive.targets??[],authoredNormals=attrs.NORMAL!==undefined;requireAsset(indices.length>=3&&targets.length<=16&&indices.length*(48+targets.length*(authoredNormals?16:28))<=256*1024*1024,'MikkTSpace working buffers exceed 256 MiB','BUDGET');
    for(const target of targets)requireAsset(target.TANGENT===undefined||attrs.TANGENT!==undefined,'Tangent morph requires a base tangent');
    if(attrs.TANGENT!==undefined&&!authoredNormals)warnings.push('Tangents without authored normals are ignored as required by glTF.');
    const n=authoredNormals?readAccessor(asset,attrs.NORMAL,'VEC3').map(unit):null;requireAsset(!n||n.length===p.length,'Tangent normal count differs');
    const corners=indices.map(i=>p[i]),cornerUv=info?indices.map(i=>uv[i]):[];
    const flat=positions=>{const normals=[];for(let i=0;i<positions.length;i+=3){const a=positions[i],b=positions[i+1],c=positions[i+2],normal=unit(cross(b.map((v,k)=>v-a[k]),c.map((v,k)=>v-a[k])));normals.push(normal,normal,normal);}return normals;};
    const baseN=n?indices.map(i=>n[i]):flat(corners),baseT=info?await generate(corners,baseN,cornerUv):null,morphs=[];
    for(const target of targets){const values={};requireAsset(Object.keys(target).length>0&&Object.keys(target).every(k=>['POSITION','NORMAL','TANGENT'].includes(k)),'Unsupported character morph attribute','UNSUPPORTED');for(const [semantic,index]of Object.entries(target)){if(!authoredNormals&&['NORMAL','TANGENT'].includes(semantic))continue;const spec=source.accessors?.[index];requireAsset(spec?.type==='VEC3'&&spec.componentType===5126&&!spec.normalized,'Morph tangent inputs require float VEC3');values[semantic]=readAccessor(asset,index,'VEC3');requireAsset(values[semantic].length===p.length,'Morph tangent input count differs');}
      const posed=indices.map((v,i)=>corners[i].map((x,k)=>x+(values.POSITION?.[v]?.[k]??0))),posedN=n?indices.map((v,i)=>unit(baseN[i].map((x,k)=>x+(values.NORMAL?.[v]?.[k]??0)))):flat(posed),posedT=info?await generate(posed,posedN,cornerUv):null;requireAsset(!posedT||posedT.every((t,i)=>t[3]===baseT[i][3]),'A generated tangent morph changes handedness; split or rebake this folded shape','UNSUPPORTED');morphs.push({values,n:posedN,t:posedT});
    }
    const remap=new Map(),sourceVertices=[],cornerVertices=[],outIndices=[];
    for(let corner=0;corner<indices.length;corner++){const key=indices[corner]+':'+JSON.stringify([baseN[corner],baseT?.[corner]??null,...morphs.map(m=>[m.n[corner],m.t?.[corner]??null])]);let vertex=remap.get(key);if(vertex===undefined){vertex=sourceVertices.length;remap.set(key,vertex);sourceVertices.push(indices[corner]);cornerVertices.push(corner);}outIndices.push(vertex);}
    requireAsset(sourceVertices.length>0&&sourceVertices.length<=1000000,'Generated tangent vertex budget exceeded','BUDGET');
    vertexRemaps[mi+':'+pi]={vertices:sourceVertices,sourceCount:p.length};
    const out=doc.meshes[mi].primitives[pi];out.attributes={};
    for(const [semantic,index]of Object.entries(attrs)){if(semantic==='NORMAL'||semantic==='TANGENT')continue;const spec=source.accessors[index],values=readAccessor(asset,index,spec.type);requireAsset(values.length===p.length,'Generated tangent attribute count differs');out.attributes[semantic]=append(sourceVertices.map(i=>values[i]),spec.type,spec.componentType,spec.normalized??false);}
    out.attributes.NORMAL=append(cornerVertices.map(i=>baseN[i]),'VEC3');if(baseT)out.attributes.TANGENT=append(cornerVertices.map(i=>baseT[i]),'VEC4');out.indices=append(outIndices.map(i=>[i]),'SCALAR',5125);out.mode=4;
    if(targets.length)out.targets=morphs.map(m=>{const result={};for(const [semantic,values]of Object.entries(m.values))if(semantic!=='NORMAL'&&semantic!=='TANGENT')result[semantic]=append(sourceVertices.map(i=>values[i]),'VEC3');if(n&&m.values.NORMAL)result.NORMAL=append(sourceVertices.map(i=>m.values.NORMAL[i]),'VEC3');else if(!n)result.NORMAL=append(cornerVertices.map(i=>m.n[i].map((v,k)=>v-baseN[i][k])),'VEC3');if(m.t)result.TANGENT=append(cornerVertices.map(i=>m.t[i].slice(0,3).map((v,k)=>v-baseT[i][k])),'VEC3');return result;});
    const extra=sourceVertices.length-p.length;warnings.push(`${info?'MikkTSpace generated normal-map tangents':'Flat normals generated'} for mesh ${mi}, primitive ${pi}${extra>0?`; split ${extra} seam vertices`:''}.`);
    }
  }
  return doc?{...asset,document:doc,buffers,tangentVertexRemaps:vertexRemaps,tangentWarnings:[...new Set(warnings)]}:asset;
}

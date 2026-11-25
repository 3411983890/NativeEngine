import { requireAsset, readAccessor, nodeMatrix, identityMatrix } from './gltf-document.mjs';
import {coordinateTransform} from './coordinates.mjs';
import {materialTextureCoordinates} from './texture-coordinates.mjs';
import {readVertexColors,readVertexTangents} from './material-source.mjs';
import {sourceIdentities} from './source-identity.mjs';

const normalize=v=>{const n=Math.hypot(...v);return n>1e-10?v.map(x=>x/n):[0,1,0];};
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];

// Convert an affine, shear-free node to TRS before handing it to the existing
// quantized animation compiler. Reflections and shear require a separate rig bake.
export function decomposeCharacterNode(node,conversion=coordinateTransform()) {
  const m=nodeMatrix(node),s=[0,1,2].map(i=>Math.hypot(...m.slice(i*4,i*4+3)));
  requireAsset(s.every(v=>v>1e-8&&v<=100),'Invalid character node scale');
  const c=[0,1,2].map(i=>m.slice(i*4,i*4+3).map(v=>v/s[i]));
  const dot=(a,b)=>a.reduce((sum,v,i)=>sum+v*b[i],0);
  requireAsset(Math.abs(dot(c[0],c[1]))<1e-5&&Math.abs(dot(c[0],c[2]))<1e-5&&Math.abs(dot(c[1],c[2]))<1e-5&&dot(cross(c[0],c[1]),c[2])>.99999,
    'Character rig contains shear or a reflected scale; bake this transform in the source tool','UNSUPPORTED');
  const r=[c[0][0],c[1][0],c[2][0],c[0][1],c[1][1],c[2][1],c[0][2],c[1][2],c[2][2]],trace=r[0]+r[4]+r[8];
  let q;
  if(trace>0){const k=Math.sqrt(trace+1)*2;q=[(r[7]-r[5])/k,(r[2]-r[6])/k,(r[3]-r[1])/k,k/4];}
  else if(r[0]>r[4]&&r[0]>r[8]){const k=Math.sqrt(1+r[0]-r[4]-r[8])*2;q=[k/4,(r[1]+r[3])/k,(r[2]+r[6])/k,(r[7]-r[5])/k];}
  else if(r[4]>r[8]){const k=Math.sqrt(1+r[4]-r[0]-r[8])*2;q=[(r[1]+r[3])/k,k/4,(r[5]+r[7])/k,(r[2]-r[6])/k];}
  else{const k=Math.sqrt(1+r[8]-r[0]-r[4])*2;q=[(r[2]+r[6])/k,(r[5]+r[7])/k,k/4,(r[3]-r[1])/k];}
  const length=Math.hypot(...q);q=q.map(v=>v/length);
  return {translation:conversion.position(m.slice(12,15)),rotation:conversion.rotation(q),scale:s};
}

function surfaceIndices(asset,primitive,count) {
  const raw=primitive.indices===undefined?Array.from({length:count},(_,i)=>i):readAccessor(asset,primitive.indices,'SCALAR').flat();
  requireAsset(raw.every(i=>Number.isSafeInteger(i)&&i>=0&&i<count),'Character index is out of range');
  const mode=primitive.mode??4,indices=[];
  requireAsset([4,5,6].includes(mode),'Character primitive must be a triangle surface','UNSUPPORTED');
  if(mode===4){requireAsset(raw.length%3===0,'Incomplete character triangle');return raw;}
  for(let i=2;i<raw.length;i++)indices.push(...(mode===6?[raw[0],raw[i-1],raw[i]]:i%2===0?[raw[i-2],raw[i-1],raw[i]]:[raw[i-1],raw[i-2],raw[i]]));
  return indices;
}
function generatedNormals(positions,indices) {
  const result=positions.map(()=>[0,0,0]);
  for(let i=0;i<indices.length;i+=3){const [a,b,c]=indices.slice(i,i+3),n=cross(positions[b].map((v,k)=>v-positions[a][k]),positions[c].map((v,k)=>v-positions[a][k]));for(const index of [a,b,c])for(let k=0;k<3;k++)result[index][k]+=n[k];}
  return result.map(normalize);
}

// Unify scene meshes and skin palettes without changing the authored source. A
// joint shared by skins with different inverse binds gets an identity child bone;
// that child follows the same animated joint while retaining its own bind matrix.
export function normalizeCharacterSource(asset,{coordinates}={}) {
  const conversion=coordinateTransform(coordinates);
  const source=asset.document,doc=structuredClone(source),warnings=[...(asset.tangentWarnings??[])],parts=[];
  requireAsset(!(source.extensionsRequired??[]).some(e=>!['KHR_materials_clearcoat','KHR_materials_ior','KHR_materials_emissive_strength','KHR_materials_unlit','KHR_texture_transform','KHR_mesh_quantization','KHR_texture_basisu','MSFT_texture_dds','EXT_texture_webp','EXT_texture_avif'].includes(e)),
    'Character requires an unsupported glTF extension','UNSUPPORTED');
  requireAsset(Array.isArray(source.nodes)&&source.nodes.length>0&&source.nodes.length<=10000,'Invalid character scene node budget','BUDGET');
  const scene=source.scenes?.[source.scene??0];requireAsset(scene?.nodes?.length,'Character has no selected scene');
  const identities=sourceIdentities(source);
  const parents=source.nodes.map(()=>-1),order=[],visited=new Set(),active=new Set();
  source.nodes.forEach((node,i)=>{for(const child of node.children??[]){requireAsset(Number.isSafeInteger(child)&&child>=0&&source.nodes[child]&&parents[child]===-1&&child!==i,'Invalid node parent');parents[child]=i;}});
  function walk(i){requireAsset(source.nodes[i]&&!visited.has(i)&&!active.has(i),'Cycle or duplicate scene node');active.add(i);visited.add(i);order.push(i);for(const c of source.nodes[i].children??[])walk(c);active.delete(i);}
  for(const i of scene.nodes){requireAsset(parents[i]===-1,'Scene root has a parent');walk(i);}
  const meshNodes=order.filter(i=>source.nodes[i].mesh!==undefined);
  requireAsset(meshNodes.length>0,'Character has no mesh');
  const needed=new Set();
  function include(i){requireAsset(Number.isSafeInteger(i)&&i>=0&&visited.has(i),'Skin joint is outside the selected scene');while(i!==-1&&!needed.has(i)){needed.add(i);i=parents[i];}}
  for(const i of meshNodes){const node=source.nodes[i];if(node.skin!==undefined){const skin=source.skins?.[node.skin];requireAsset(skin?.joints?.length&&new Set(skin.joints).size===skin.joints.length,'Invalid skin joints');for(const joint of skin.joints)include(joint);}else include(i);}
  const boneSources=order.filter(i=>needed.has(i)),roots=boneSources.filter(i=>parents[i]===-1),nodes=[],inverse=[],mapping=new Map(),names=new Set();
  const unique=name=>{let result=name,index=1;while(names.has(result))result=`${name}_${index++}`;names.add(result);return result;};
  if(roots.length>1){nodes.push({name:unique('__asset_root'),children:[]});inverse.push(identityMatrix());}
  for(const i of boneSources){const sourceNode=source.nodes[i],name=unique(sourceNode.name??`bone_${i}`);if(name!==sourceNode.name&&sourceNode.name)warnings.push(`Duplicate bone name ${sourceNode.name} was renamed to ${name}.`);mapping.set(i,nodes.length);nodes.push({name,...decomposeCharacterNode(sourceNode,conversion),children:[]});inverse.push(identityMatrix());}
  for(const i of boneSources){const parent=parents[i]===-1?(roots.length>1?0:-1):mapping.get(parents[i]);if(parent>=0)nodes[parent].children.push(mapping.get(i));}
  const skeletonRoot=roots.length>1?0:mapping.get(roots[0]),palettes=new Map();
  const paletteIndex=(joint,bind)=>{
    const existing=palettes.get(joint)??[],match=existing.find(item=>item.bind.every((v,k)=>v===bind[k]));if(match)return match.index;
    let index=mapping.get(joint);requireAsset(index!==undefined,'Missing joint ancestor');
    if(existing.length){const parent=index;index=nodes.length;nodes.push({name:unique(`${nodes[parent].name}__skin_${existing.length}`),children:[]});inverse.push(bind);nodes[parent].children.push(index);}
    else inverse[index]=bind;
    existing.push({index,bind});palettes.set(joint,existing);return index;
  };
  const position=[],normal=[],uv=[],jointValues=[],weights=[],indices=[],morphByName=new Map(),sourceRanges=[];
  const slotUvs=Array.from({length:4},()=>[]);let hasTextureUvs=false;
  const layerUvs=Array.from({length:3},()=>[]);let hasLayerUvs=false;
  const colors=[];let hasVertexColors=false;
  const tangents=[];let hasTangents=false;
  const nodeMorphs=new Map(),morphDefaults=new Map(),morphNameCounts=new Map();
  for(const ni of meshNodes) {
    const mesh=source.meshes?.[source.nodes[ni].mesh],count=mesh?.primitives?.[0]?.targets?.length??0;
    requireAsset(count<=16,'Source mesh exceeds 16 morph targets','BUDGET');
    requireAsset(mesh?.primitives?.every(p=>(p.targets?.length??0)===count),'Morph target count differs between mesh primitives');
    const names=Array.from({length:count},(_,mi)=>mesh.extras?.targetNames?.[mi]??`morph_${mi}`);
    requireAsset(names.every(n=>typeof n==='string'&&n.length>0)&&new Set(names).size===names.length,'Invalid morph target names');
    nodeMorphs.set(ni,names);for(const name of names)morphNameCounts.set(name,(morphNameCounts.get(name)??0)+1);
  }
  const reservedMorphNames=new Set(morphNameCounts.keys()),usedMorphNames=new Set();
  for(const [ni,names]of nodeMorphs) {
    const node=source.nodes[ni],defaults=node.weights??source.meshes[node.mesh].weights??names.map(()=>0);
    requireAsset(defaults.length===names.length&&defaults.every(v=>Number.isFinite(v)&&Math.abs(v)<=8),'Invalid default morph weights');
    nodeMorphs.set(ni,names.map((name,mi)=>{
      let key=name;if(morphNameCounts.get(name)>1){key=`node_${ni}_${name}`;while(reservedMorphNames.has(key)||usedMorphNames.has(key))key+='_';}
      usedMorphNames.add(key);morphDefaults.set(key,defaults[mi]);return key;
    }));
  }
  let discardedWeight=0;
  for(const ni of meshNodes) {
    const node=source.nodes[ni],mesh=source.meshes?.[node.mesh];requireAsset(mesh?.primitives?.length,'Missing character mesh');
    const skin=node.skin===undefined?null:source.skins[node.skin];
    const binds=skin?(skin.inverseBindMatrices===undefined?skin.joints.map(identityMatrix):readAccessor(asset,skin.inverseBindMatrices,'MAT4')):null;
    if(skin)requireAsset(binds.length===skin.joints.length,'Inverse bind matrix count mismatch');
    const palette=skin?skin.joints.map((joint,i)=>paletteIndex(joint,conversion.transformMatrix(binds[i]))):[paletteIndex(ni,identityMatrix())];
    for(const [pi,primitive]of mesh.primitives.entries()) {
      requireAsset(!primitive.extensions?.KHR_draco_mesh_compression,'Character Draco geometry requires a decoder','UNSUPPORTED');
      const attrs=primitive.attributes;requireAsset(attrs?.POSITION!==undefined,'Character primitive has no positions');
      requireAsset(!Object.keys(attrs).some(k=>!['POSITION','NORMAL','TANGENT','COLOR_0','JOINTS_0','WEIGHTS_0','JOINTS_1','WEIGHTS_1'].includes(k)&&!/^TEXCOORD_(0|[1-9][0-9]*)$/.test(k)),
        'Unsupported character vertex attribute','UNSUPPORTED');
      const p=readAccessor(asset,attrs.POSITION,'VEC3'),local=surfaceIndices(asset,primitive,p.length),base=position.length,sourceCount=p.length;
      const n=attrs.NORMAL===undefined?generatedNormals(p,local):readAccessor(asset,attrs.NORMAL,'VEC3'),selected=materialTextureCoordinates(asset,attrs,source.materials?.[primitive.material],p.length),t=selected.uv,color=readVertexColors(asset,attrs,p.length);
      const tangent=readVertexTangents(asset,attrs,p.length);hasTangents ||= !!tangent;
      if(attrs.TANGENT!==undefined&&attrs.NORMAL===undefined)warnings.push('Tangents without authored normals are ignored as required by glTF.');
      hasVertexColors ||= !!color;
      hasTextureUvs ||= !!selected.extraUvs;const extra=selected.extraUvs??Array.from({length:4},()=>t);
      hasLayerUvs ||= !!selected.layerUvs;const layers=selected.layerUvs??Array.from({length:3},()=>t);
      let j=skin?readAccessor(asset,attrs.JOINTS_0,'VEC4'):p.map(()=>[0,0,0,0]),w=skin?readAccessor(asset,attrs.WEIGHTS_0,'VEC4'):p.map(()=>[1,0,0,0]);
      requireAsset([n,t,j,w].every(a=>a.length===p.length),'Character vertex attribute count mismatch');
      const j1=attrs.JOINTS_1===undefined?null:readAccessor(asset,attrs.JOINTS_1,'VEC4'),w1=attrs.WEIGHTS_1===undefined?null:readAccessor(asset,attrs.WEIGHTS_1,'VEC4');
      requireAsset(Boolean(j1)===Boolean(w1)&&(!j1||(j1.length===p.length&&w1.length===p.length)),'Secondary skin weights do not match');
      const doubleSided=source.materials?.[primitive.material]?.doubleSided===true;
      if(doubleSided) {
        const front=[...local];for(let i=0;i<front.length;i+=3)local.push(sourceCount+front[i],sourceCount+front[i+2],sourceCount+front[i+1]);
        p.push(...p.map(v=>[...v]));n.push(...n.map(v=>v.map(x=>-x)));t.push(...t.map(v=>[...v]));j.push(...j.map(v=>[...v]));w.push(...w.map(v=>[...v]));
        if(j1){j1.push(...j1.map(v=>[...v]));w1.push(...w1.map(v=>[...v]));}
      }
      requireAsset(base+p.length<=100000,'Character exceeds 100000 vertices','BUDGET');
      for(let v=0;v<p.length;v++) {
        const influences=[...j[v],...(j1?.[v]??[])].map((joint,k)=>({joint,weight:[...w[v],...(w1?.[v]??[])][k],order:k}));
        requireAsset(influences.every(item=>Number.isSafeInteger(item.joint)&&item.joint>=0&&item.joint<palette.length&&Number.isFinite(item.weight)&&item.weight>=0),'Invalid skin influences');
        const total=influences.reduce((s,item)=>s+item.weight,0);requireAsset(total>1e-8,'Vertex has zero total skin weight');
        influences.sort((a,b)=>b.weight-a.weight||a.order-b.order);const selected=influences.slice(0,4),sum=selected.reduce((s,item)=>s+item.weight,0);
        discardedWeight=Math.max(discardedWeight,1-sum/total);
        jointValues.push(selected.map(item=>palette[item.joint]));weights.push(selected.map(item=>item.weight/sum));
        position.push(conversion.position(p[v]));normal.push(conversion.direction(n[v]));uv.push(t[v]);
        for(let slot=0;slot<4;slot++)slotUvs[slot].push(extra[slot][v%sourceCount]);
        for(let slot=0;slot<3;slot++)layerUvs[slot].push(layers[slot][v%sourceCount]);
        colors.push(color?.[v%sourceCount]??[1,1,1,1]);
        const authored=tangent?.[v%sourceCount];tangents.push(authored?[...conversion.direction(authored),authored[3]*conversion.determinant*(v>=sourceCount?-1:1)]:[0,0,0,0]);
      }
      for(let i=0;i<local.length;i+=3)indices.push(...(conversion.determinant<0?[base+local[i],base+local[i+2],base+local[i+1]]:[base+local[i],base+local[i+1],base+local[i+2]]));
      parts.push({source:identities.part(ni,primitive,pi),name:node.name??mesh.name??`mesh_${ni}`,firstIndex:indices.length-local.length,indexCount:local.length,material:`m${(primitive.material??-1)+1}`,...(selected.extraUvs||selected.layerUvs?{textureCoordinates:selected.textureCoordinates}:{})});
      sourceRanges.push({node:ni,mesh:node.mesh,primitive:pi,base,count:sourceCount,doubleSided,remap:asset.tangentVertexRemaps?.[node.mesh+':'+pi]});
      for(const [mi,target]of (primitive.targets??[]).entries()) {
        requireAsset(Object.keys(target).length>0&&Object.keys(target).every(k=>['POSITION','NORMAL','TANGENT'].includes(k)),'Unsupported character morph attribute','UNSUPPORTED');
        requireAsset(target.TANGENT===undefined||attrs.TANGENT!==undefined,'Tangent morph requires a base tangent');
        const zero=()=>Array.from({length:sourceCount},()=>[0,0,0]),name=nodeMorphs.get(ni)[mi],delta=target.POSITION===undefined?zero():readAccessor(asset,target.POSITION,'VEC3'),dn=target.NORMAL===undefined?zero():readAccessor(asset,target.NORMAL,'VEC3'),dt=target.TANGENT!==undefined&&tangent?readAccessor(asset,target.TANGENT,'VEC3'):null;
        requireAsset(delta.length===sourceCount&&dn.length===sourceCount&&(!dt||dt.length===sourceCount),'Character morph count mismatch');
        if(doubleSided){delta.push(...delta.map(v=>[...v]));dn.push(...dn.map(v=>v.map(x=>-x)));if(dt)dt.push(...dt.map(v=>[...v]));}
        if(!morphByName.has(name))morphByName.set(name,[]);morphByName.get(name).push({base,count:p.length,delta,dn,dt});
      }
    }
  }
  requireAsset(nodes.length<=256,'Merged skin palettes exceed 256 bones','BUDGET');
  requireAsset(parts.length<=256&&indices.length<=3000000&&morphByName.size<=16,'Character primitive/index/morph budget exceeded','BUDGET');
  const buffers=[...asset.buffers];doc.accessors??=[];doc.bufferViews??=[];doc.buffers??=[];
  function append(values,type,componentType=5126) {
    const components={SCALAR:1,VEC2:2,VEC3:3,VEC4:4,MAT4:16}[type],bytes=Buffer.alloc(values.length*components*(componentType===5123?2:4));
    values.forEach((row,i)=>row.forEach((v,k)=>{if(componentType===5123)bytes.writeUInt16LE(v,(i*components+k)*2);else if(componentType===5125)bytes.writeUInt32LE(v,(i*components+k)*4);else bytes.writeFloatLE(v,(i*components+k)*4);}));
    const buffer=buffers.length,view=doc.bufferViews.length,index=doc.accessors.length;buffers.push(bytes);doc.buffers.push({byteLength:bytes.length});doc.bufferViews.push({buffer,byteLength:bytes.length});doc.accessors.push({bufferView:view,componentType,type,count:values.length});return index;
  }
  const primitive={attributes:{POSITION:append(position,'VEC3'),NORMAL:append(normal,'VEC3'),TEXCOORD_0:append(uv,'VEC2'),JOINTS_0:append(jointValues,'VEC4',5123),WEIGHTS_0:append(weights,'VEC4')},indices:append(indices.map(v=>[v]),'SCALAR',5125),mode:4};
  if(hasTextureUvs)for(let slot=0;slot<4;slot++)primitive.attributes[`_NATIVEGPU_UV_${slot}`]=append(slotUvs[slot],'VEC2');
  if(hasLayerUvs)for(let slot=0;slot<3;slot++)primitive.attributes[`_NATIVEGPU_LAYERUV_${slot}`]=append(layerUvs[slot],'VEC2');
  if(hasVertexColors)primitive.attributes.COLOR_0=append(colors,'VEC4');
  if(hasTangents)primitive.attributes.TANGENT=append(tangents,'VEC4');
  const targetNames=[...morphByName.keys()];
  if(targetNames.length)primitive.targets=targetNames.map(name=>{
    const dp=position.map(()=>[0,0,0]),dn=position.map(()=>[0,0,0]),dt=position.map(()=>[0,0,0]),ranges=morphByName.get(name),hasTangent=ranges.some(range=>range.dt);
    for(const range of ranges)for(let i=0;i<range.count;i++){dp[range.base+i]=conversion.position(range.delta[i]);dn[range.base+i]=conversion.direction(range.dn[i]);if(range.dt)dt[range.base+i]=conversion.direction(range.dt[i]);}
    return {POSITION:append(dp,'VEC3'),NORMAL:append(dn,'VEC3'),...(hasTangent?{TANGENT:append(dt,'VEC3')}:{})};
  });
  const boneCount=nodes.length;nodes.push({name:unique('__character_mesh'),mesh:0,skin:0});
  doc.nodes=nodes;doc.meshes=[{primitives:[primitive],weights:targetNames.map(name=>morphDefaults.get(name)),extras:{targetNames}}];doc.skins=[{joints:Array.from({length:boneCount},(_,i)=>i),inverseBindMatrices:append(inverse,'MAT4')}];
  doc.scenes=[{nodes:[skeletonRoot,boneCount]}];doc.scene=0;delete doc.extensions;delete doc.extensionsUsed;delete doc.extensionsRequired;
  doc.animations=(source.animations??[]).map(animation=>{
    const output={...animation,samplers:[],channels:[],morphTracks:[]};
    for(const channel of animation.channels??[]) {
      if(channel.target?.path==='weights') {
        const names=nodeMorphs.get(channel.target.node);requireAsset(names?.length,'Morph animation references a node without morph targets');
        const spec=animation.samplers?.[channel.sampler];requireAsset(spec,'Missing morph animation sampler');
        output.morphTracks.push({targets:names.map(name=>targetNames.indexOf(name)),sampler:output.samplers.length});output.samplers.push({...spec,output:append(readAccessor(asset,spec.output,'SCALAR'),'SCALAR')});continue;
      }
      if(!mapping.has(channel.target?.node)){warnings.push('An animation channel outside the skin hierarchy was omitted.');continue;}
      const spec=animation.samplers?.[channel.sampler];requireAsset(spec,'Missing animation sampler');
      const type=channel.target.path==='rotation'?'VEC4':'VEC3',values=readAccessor(asset,spec.output,type).map(v=>channel.target.path==='translation'?conversion.position(v):channel.target.path==='rotation'?conversion.rotation(v):v);
      output.channels.push({...channel,sampler:output.samplers.length,target:{...channel.target,node:mapping.get(channel.target.node)}});output.samplers.push({...spec,output:append(values,type)});
    }
    return output;
  }).filter(animation=>animation.channels.length||animation.morphTracks.length);
  if(!doc.animations.length){doc.animations=[{name:'bind_pose',channels:[{target:{node:skeletonRoot,path:'translation'},sampler:0}],samplers:[{input:append([[0],[1]],'SCALAR'),output:append([nodes[skeletonRoot].translation??[0,0,0],nodes[skeletonRoot].translation??[0,0,0]],'VEC3')}]}];warnings.push('No skeletal animation was found; a bind-pose preview clip was created.');}
  const feature=doc.extras?.nativegpu;
  if(feature) {
    if(feature.rig)for(const chain of [...feature.rig.legs??[],...feature.rig.arms??[]]){if(chain.pole)chain.pole=conversion.position(chain.pole);if(chain.soleHeight!==undefined)chain.soleHeight*=conversion.scale;}
    for(const region of feature.softRegions??[]) {
      if(region.center)region.center=conversion.position(region.center);
      if(region.radius!==undefined)region.radius*=conversion.scale;if(region.maxOffset!==undefined)region.maxOffset*=conversion.scale;
      const range=sourceRanges.find(r=>(region.sourceMesh===undefined||r.mesh===region.sourceMesh)&&(region.sourcePrimitive===undefined||r.primitive===region.sourcePrimitive));requireAsset(range,'Soft region references an unknown mesh');
      region.vertices=region.vertices.flatMap(i=>{requireAsset(Number.isInteger(i)&&i>=0&&i<(range.remap?.sourceCount??range.count),'Invalid soft region vertex');const mapped=range.remap?range.remap.vertices.flatMap((source,index)=>source===i?[index]:[]):[i];requireAsset(mapped.length>0,'Soft region selects an unused source vertex');return mapped.flatMap(index=>range.doubleSided?[range.base+index,range.base+range.count+index]:[range.base+index]);});
    }
  }
  for(const animation of doc.animations){const motion=animation.extras?.nativegpu?.motion;if(motion){if(motion.velocity)motion.velocity=conversion.position(motion.velocity);if(motion.turnRate!==undefined)motion.turnRate=conversion.turnRate(motion.turnRate);}}
  if(discardedWeight>1e-8)warnings.push(`Skin influences were reduced to four per vertex; maximum discarded weight: ${discardedWeight.toFixed(6)}.`);
  const rootMotion=source.extras?.nativegpu?.rootMotion??(source.skins?.length?'extract':'preserve');
  requireAsset(['extract','preserve'].includes(rootMotion),'rootMotion must be extract or preserve');
  return {...asset,document:doc,buffers,parts,rootMotion,...(hasLayerUvs?{hasLayerUvs:true}:{}),...(hasTangents?{hasTangents:true}:{}),...(hasVertexColors?{hasVertexColors:true}:{}),...(hasTextureUvs?{hasTextureUvs:true}:{}),warnings:[...new Set(warnings)]};
}

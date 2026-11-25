import {canonicalJson,sha256Hex} from './canonical.mjs';
import {requireAsset} from './gltf-document.mjs';
const hash=value=>sha256Hex(canonicalJson(value));
function authoredId(value){const id=value?.extras?.nativegpu?.sourceId;requireAsset(id===undefined||typeof id==='string'&&id.trim().length>0&&id.length<=128&&!/[\x00-\x1f]/.test(id),'Invalid extras.nativegpu.sourceId');return id;}
// Provenance is independent of geometry bytes and numeric node ordering.
// Unnamed or repeated sibling names remain importable but are marked unstable.
export function sourceIdentities(document){
  const nodes=new Map(),seen=new Set(),explicit=new Set(),primitiveIds=new Map(),stack=[{indices:document.scenes?.[document.scene??0]?.nodes??[],parent:hash('source-root'),stable:true}];
  while(stack.length){const {indices,parent,stable}=stack.pop();requireAsset(Array.isArray(indices),'Invalid source node list');const counts=new Map();for(const index of indices){const name=document.nodes?.[index]?.name;if(typeof name==='string'&&name.trim())counts.set(name,(counts.get(name)??0)+1);}
    for(const index of indices){const node=document.nodes?.[index];requireAsset(Number.isSafeInteger(index)&&index>=0&&node&&!seen.has(index),'Invalid source identity hierarchy');seen.add(index);const id=authoredId(node),named=typeof node.name==='string'&&node.name.trim().length>0,unique=named&&counts.get(node.name)===1;
      if(id!==undefined){requireAsset(!explicit.has(id),'Duplicate node sourceId');explicit.add(id);}const key=hash(id!==undefined?['id',id]:[parent,unique?'name':'index',unique?node.name:index]),reliable=id!==undefined||stable&&unique;nodes.set(index,{key,stable:reliable,label:(named?node.name:'Node '+index).slice(0,128)});stack.push({indices:node.children??[],parent:key,stable:reliable});
    }
  }
  return {part(node,primitive,primitiveIndex){const owner=nodes.get(node);requireAsset(owner,'Mesh node is outside the selected source scene');const id=authoredId(primitive);if(id!==undefined){const pair=node+':'+id;requireAsset(!primitiveIds.has(pair)||primitiveIds.get(pair)===primitiveIndex,'Duplicate primitive sourceId');primitiveIds.set(pair,primitiveIndex);}return {key:hash({node:owner.key,primitive:id===undefined?primitiveIndex:'id:'+id}),stable:owner.stable,label:owner.label+' / '+(id??primitiveIndex)};}};
}
export function sourceMaterialIdentities(materials){
  const names=new Map(),ids=new Set();for(const material of materials)if(typeof material.name==='string'&&material.name.trim())names.set(material.name,(names.get(material.name)??0)+1);const result={m0:{key:hash('default-material'),stable:true,label:'Default'}};
  for(const [index,material]of materials.entries()){const id=authoredId(material);if(id!==undefined){requireAsset(!ids.has(id),'Duplicate material sourceId');ids.add(id);}const named=typeof material.name==='string'&&material.name.trim(),unique=named&&names.get(material.name)===1,sole=materials.length===1&&!named;result['m'+(index+1)]={key:hash(id!==undefined?['id',id]:unique?['name',material.name]:sole?['sole-material']:['index',index]),stable:!!(id!==undefined||unique||sole),label:(named?material.name:'Material '+index).slice(0,128)};}return result;
}
export function sourceObjectPart(name,material,index){return {key:hash({object:name,material:material??null,occurrence:index}),stable:false,label:String(name).slice(0,128)};}

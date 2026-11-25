import fs from 'node:fs/promises';
import path from 'node:path';
import {randomUUID} from 'node:crypto';
import {importAsset,verifyImport,appendAnimation} from './import-service.mjs';
import {canonicalJson,sha256Hex} from './canonical.mjs';
import {requireAsset} from './gltf-document.mjs';

function compareEntries(previous,next,identity,label){
  const matched=[],ambiguous=[],used=new Set(),keys=new Map(),beforeKeys=new Map(),beforeNames=new Map(),afterNames=new Map();
  for(const a of previous){const source=identity(a),name=label(a);if(source?.stable)beforeKeys.set(source.key,(beforeKeys.get(source.key)??0)+1);beforeNames.set(name,(beforeNames.get(name)??0)+1);}
  for(const [index,b]of next.entries()){const source=identity(b),name=label(b);if(source?.stable){const indices=keys.get(source.key)??[];indices.push(index);keys.set(source.key,indices);}const indices=afterNames.get(name)??[];indices.push(index);afterNames.set(name,indices);}
  for(const [before,a]of previous.entries()){
    const source=identity(a),candidates=source?.stable?keys.get(source.key)??[]:[],duplicate=source?.stable&&beforeKeys.get(source.key)>1;
    if(candidates.length===1&&!duplicate&&!used.has(candidates[0])){const after=candidates[0];used.add(after);matched.push({before,after,key:source.key,basis:'source-id'});continue;}
    if(previous.length===1&&next.length===1&&!used.has(0)){used.add(0);matched.push({before,after:0,basis:'single-entry'});continue;}
    // Legacy packages carry no source identity. Unique author names are useful
    // for a preview, but are explicitly labelled as a weaker correspondence.
    if(!source){const name=label(a),sameAfter=afterNames.get(name)??[];if(name&&beforeNames.get(name)===1&&sameAfter.length===1&&!used.has(sameAfter[0])){used.add(sameAfter[0]);matched.push({before,after:sameAfter[0],basis:'legacy-name'});continue;}}
    if(!source?.stable||candidates.length>1||duplicate)ambiguous.push({before,label:label(a),reason:!source?.stable?'Source identity is missing or ambiguous':'Source identity is duplicated'});
  }
  const mapped=new Set(matched.map(p=>p.before)),uncertain=new Set(ambiguous.map(p=>p.before));return {matched,added:next.flatMap((p,i)=>!used.has(i)?[{index:i,label:label(p)}]:[]),removed:previous.flatMap((p,i)=>!mapped.has(i)&&!uncertain.has(i)?[{index:i,label:label(p)}]:[]),ambiguous};
}
export function compareSourceImports(previous,next){
  requireAsset(previous.kind===next.kind,'Reimport cannot change the asset kind','ARGUMENT');
  const parts=compareEntries(previous.parts??[],next.parts??[],p=>p.source,p=>p.name),beforeMaterials=Object.keys(previous.materials??{}),afterMaterials=Object.keys(next.materials??{}),materials=compareEntries(beforeMaterials,afterMaterials,name=>null,name=>name);
  if(beforeMaterials.length||afterMaterials.length){const mapped=compareEntries(beforeMaterials.map(name=>({name,source:previous.materialSources?.[name]})),afterMaterials.map(name=>({name,source:next.materialSources?.[name]})),p=>p.source,p=>p.name);Object.assign(materials,mapped);for(const item of materials.matched){item.beforeName=beforeMaterials[item.before];item.afterName=afterMaterials[item.after];}}
  return {schema:'nativegpu.reimport-plan/1',previous:previous.id,next:next.id,kind:next.kind,unchanged:previous.id===next.id,sourceChanged:previous.sourceHash!==next.sourceHash,parts,materials,sceneReferencesUpdated:false};
}

// This publishes a new immutable version and a durable comparison plan. Scene
// application is a separate revision-checked operation; publishing never edits
// the previous package or any existing instance.
export async function reimportAsset({directory,source,output,size,coordinates}={}){
  requireAsset(typeof directory==='string'&&typeof source==='string','Choose the previous package and updated source','ARGUMENT');directory=await fs.realpath(directory);const previous=await verifyImport(directory);output=path.resolve(output??path.dirname(directory));let next;
  if(previous.kind==='character'&&previous.parentAsset){
    requireAsset(/^[a-f0-9]{64}$/.test(previous.parentAsset),'Invalid parent character identity','HASH');const parent=path.join(path.dirname(directory),previous.parentAsset);const manifest=await verifyImport(parent);requireAsset(manifest.id===previous.parentAsset&&manifest.kind==='character','The original parent character is required to update this appended motion','DEPENDENCY');requireAsset(size===undefined&&coordinates===undefined,'Motion version updates reuse their original retarget settings','ARGUMENT');next=await appendAnimation({directory:parent,source,output,options:previous.identity.options.motion??{}});
  }else{
    const options=previous.identity.options??{};next=await importAsset({source,output,expectedKind:previous.kind,size:size??options.size??'auto',semantic:options.semantic??previous.semantic??'albedo',coordinates:coordinates??options.coordinates});
  }
  requireAsset(next.manifest.kind===previous.kind,'Reimport changed the asset kind','ARGUMENT');await verifyImport(directory);const plan=compareSourceImports(previous,next.manifest),reports=path.join(output,'reimports');await fs.mkdir(reports,{recursive:true});const actualOutput=await fs.realpath(output),actualReports=await fs.realpath(reports),relative=path.relative(actualOutput,actualReports);requireAsset(relative&&!relative.startsWith('..')&&!path.isAbsolute(relative),'Reimport reports escape output directory','PATH');const report=path.join(actualReports,`${previous.id}-${next.manifest.id}.json`),bytes=Buffer.from(canonicalJson(plan)+'\n');
  const pending=path.join(actualReports,'.report-'+randomUUID());
  try{await fs.writeFile(pending,bytes,{flag:'wx'});try{await fs.link(pending,report);}catch(error){if(error.code!=='EEXIST')throw error;requireAsset(!(await fs.lstat(report)).isSymbolicLink(),'Reimport report cannot be a symlink','PATH');requireAsset(sha256Hex(await fs.readFile(report))===sha256Hex(bytes),'Reimport report conflicts with existing content','HASH');}}finally{await fs.rm(pending,{force:true});}
  return {...next,reimport:plan,reportPath:report};
}

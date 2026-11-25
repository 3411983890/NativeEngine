import {requireAsset} from './gltf-document.mjs';
import {decomposeCharacterNode} from './character-source.mjs';
import {add,sub,mul,rotate,product,quaternion,inverseQuaternion,globalTransforms,readAnimationTrack,sampleTrack} from './animation-sampling.mjs';
import {binaryHash} from '../../tools/offline/animation.mjs';
import {coordinateTransform} from './coordinates.mjs';
import {identifier,boneKey} from './bone-semantics.mjs';

const ownObject=value=>value&&typeof value==='object'&&!Array.isArray(value);
const uniform=value=>value.length===3&&value.every(n=>Number.isFinite(n)&&n>0)&&Math.max(...value)-Math.min(...value)<1e-5;
const distance=(a,b)=>Math.hypot(...sub(a,b));
const localOf=bone=>({translation:bone.rest.slice(0,3),rotation:quaternion(bone.rest.slice(3,7)),scale:bone.rest.slice(7)});
const relative=(transforms,node,ancestor)=>ancestor<0?transforms[node].translation:mul(rotate(inverseQuaternion(transforms[ancestor].rotation),sub(transforms[node].translation,transforms[ancestor].translation)),1/transforms[ancestor].scale);
function sourceHierarchy(asset) {
  const source=asset.document.nodes;requireAsset(Array.isArray(source)&&source.length>0&&source.length<=10000,'Motion source needs 1..10000 nodes','BUDGET');
  const nodes=source.map((node,i)=>({name:node.name??`node_${i}`,parent:-1})),order=[],state=new Uint8Array(nodes.length);
  for(const [i,node]of source.entries())for(const child of node.children??[]) {
    requireAsset(Number.isInteger(child)&&child>=0&&nodes[child]&&nodes[child].parent===-1&&child!==i,'Invalid motion hierarchy');nodes[child].parent=i;
  }
  function visit(i){requireAsset(state[i]!==1,'Cycle in motion hierarchy');if(state[i]===2)return;state[i]=1;if(nodes[i].parent>=0)visit(nodes[i].parent);state[i]=2;order.push(i);}
  nodes.forEach((_,i)=>visit(i));return {nodes,order};
}
function resolveSource(nodes,value) {
  if(Number.isInteger(value)){requireAsset(value>=0&&value<nodes.length,'Source bone index is out of range');return value;}
  requireAsset(typeof value==='string','Bone mappings require a source name or node index');
  const matches=nodes.flatMap((node,i)=>node.name===value?[i]:[]);requireAsset(matches.length===1,`Source bone ${value} is missing or ambiguous; use its node index`,'MAPPING');return matches[0];
}
function mappingFor(source,target,boneMap) {
  requireAsset(ownObject(boneMap),'boneMap must map target bone names to source names or node indices','ARGUMENT');
  for(const name of Object.keys(boneMap))requireAsset(target.some(bone=>bone.name===name),`Unknown target bone ${name}`,'MAPPING');
  const used=new Set();return target.map(bone=>{
    if(Object.hasOwn(boneMap,bone.name)&&boneMap[bone.name]===null)return -1;
    let index=-1;
    if(Object.hasOwn(boneMap,bone.name))index=resolveSource(source,boneMap[bone.name]);
    else if(!/__skin_\d+$/.test(bone.name)) {
      let matches=source.flatMap((node,i)=>node.name===bone.name?[i]:[]);
      if(!matches.length)matches=source.flatMap((node,i)=>identifier(node.name)===identifier(bone.name)?[i]:[]);
      if(!matches.length)matches=source.flatMap((node,i)=>boneKey(node.name)===boneKey(bone.name)?[i]:[]);
      requireAsset(matches.length<=1,`Ambiguous mapping for ${bone.name}: ${matches.map(i=>`${i}:${source[i].name}`).join(', ')}; provide boneMap`,'MAPPING');
      index=matches[0]??-1;
    }
    requireAsset(index<0||!used.has(index),`Source bone ${source[index]?.name} maps to multiple target bones`,'MAPPING');if(index>=0)used.add(index);return index;
  });
}
function morphTracksFor(asset,animation,header,morphMap,warnings) {
  const curves=[],seen=new Set(),candidates=[];
  for(const channel of animation.channels??[])if(channel.target?.path==='weights') {
    const node=asset.document.nodes[channel.target.node],mesh=asset.document.meshes?.[node?.mesh],count=mesh?.primitives?.[0]?.targets?.length??0;
    requireAsset(count>0&&mesh.primitives.every(p=>(p.targets?.length??0)===count),'Motion morph target count mismatch');
    const track=readAnimationTrack(asset,animation,channel,{weightCount:count});
    for(let i=0;i<count;i++)candidates.push({name:mesh.extras?.targetNames?.[i]??`morph_${i}`,node:channel.target.node,index:i,track});
  }
  for(const [i,target]of (header.morphs??[]).entries()) {
    const explicit=Object.hasOwn(morphMap,target.name),value=morphMap[target.name];if(explicit&&value===null)continue;
    const matches=candidates.filter(c=>explicit?value===`${c.node}:${c.name}`||value===c.name:c.name===target.name||identifier(c.name)===identifier(target.name.replace(/^node_\d+_/,'')));
    requireAsset(matches.length<=1,`Ambiguous morph ${target.name}; use morphMap with nodeIndex:targetName`,'MAPPING');
    if(matches.length){requireAsset(!seen.has(i),'Duplicate target morph animation');seen.add(i);curves.push({target:i,...matches[0]});}
    else if(explicit)requireAsset(false,`Unknown source morph ${value}`,'MAPPING');
  }
  const used=new Set(curves.map(c=>c.track));
  for(const candidate of candidates)if(!used.has(candidate.track))warnings.add(`Morph channel on source node ${candidate.node} has no matching target shape.`);
  return curves;
}
function safeSamples(header,binary) {
  requireAsset(header.format==='nativegpu.animation/2'&&header.sampleRate===60&&header.quantization===10000&&binary.length<=64*1024*1024&&binary.length===header.binaryBytes&&binaryHash(binary)===header.binaryHash,'Invalid target character animation package');
  requireAsset(Array.isArray(header.bones)&&header.bones.length>0&&header.bones.length<=256&&header.bones.every((b,i)=>typeof b.name==='string'&&Number.isInteger(b.parent)&&b.parent>=-1&&b.parent<i&&Array.isArray(b.rest)&&b.rest.length===10&&b.rest.every(Number.isFinite)&&uniform(b.rest.slice(7))),'Invalid target skeleton');
  requireAsset(header.bones.filter(b=>b.parent===-1).length===1&&new Set(header.bones.map(b=>b.name)).size===header.bones.length,'Invalid target skeleton roots or names');
  requireAsset(Array.isArray(header.clips)&&header.clips.length>0&&header.clips.length<=64&&Array.isArray(header.morphs)&&header.morphs.length<=16,'Invalid target clips or morph budget');
  const defaults=header.defaultMorphWeights??header.morphs.map((_,i)=>{
    const clip=header.clips[0],at=(clip.morphOffset??-1)+i*4;
    return clip.morphOffset!==undefined&&at>=0&&at+4<=binary.length?binary.readInt32LE(at)/10000:0;
  });
  requireAsset(defaults.length===header.morphs.length&&defaults.every(v=>Number.isFinite(v)&&Math.abs(v)<=8),'Invalid target default shape weights');return defaults;
}
export function retargetMotion(asset,targetHeader,targetBinary,options={}) {
  requireAsset(ownObject(options),'Motion options must be an object','ARGUMENT');
  const allowed=['boneMap','morphMap','translationScale','orientation','mode','clips','prefix','loop','controller','rootMotion','motionRoot','rootMotionAxes','coordinates'];
  requireAsset(Object.keys(options).every(key=>allowed.includes(key)),'Unknown motion import option','ARGUMENT');
  const conversion=coordinateTransform(options.coordinates),defaults=safeSamples(targetHeader,targetBinary),header=structuredClone(targetHeader),{nodes,order}=sourceHierarchy(asset),target=header.bones,targetOrder=target.map((_,i)=>i);
  const map=mappingFor(nodes,target,options.boneMap??{}),usedNodes=new Set();
  for(const source of map)for(let i=source;i>=0;i=nodes[i].parent)usedNodes.add(i);
  for(const i of usedNodes)Object.assign(nodes[i],decomposeCharacterNode(asset.document.nodes[i],conversion));
  const sourceOrder=order.filter(i=>usedNodes.has(i)),sourceRest=globalTransforms(nodes,sourceOrder,nodes),targetLocals=target.map(localOf),targetRest=globalTransforms(target,targetOrder,targetLocals),ancestors=[];
  const mode=options.mode??'rest-pose',orientation=quaternion(options.orientation??[0,0,0,1]),inverseOrientation=inverseQuaternion(orientation);
  requireAsset(['exact','rest-pose'].includes(mode),'Retarget mode must be exact or rest-pose','ARGUMENT');
  const lengths=[];
  for(let t=0;t<target.length;t++) {
    let parent=target[t].parent;while(parent>=0&&map[parent]<0)parent=target[parent].parent;ancestors[t]=parent;
    if(map[t]<0)continue;
    if(parent>=0){let sourceParent=nodes[map[t]].parent;while(sourceParent>=0&&sourceParent!==map[parent])sourceParent=nodes[sourceParent].parent;
      requireAsset(sourceParent===map[parent],`Bone mapping crosses hierarchy branches at ${target[t].name}`,'MAPPING');
      const a=distance(sourceRest[map[t]].translation,sourceRest[map[parent]].translation),b=distance(targetRest[t].translation,targetRest[parent].translation);if(a>1e-4&&b>1e-4)lengths.push(b/a);
    }
    if(mode==='exact') {
      const source=nodes[map[t]],rest=localOf(target[t]);
      requireAsset([...source.translation,...source.scale].every((v,i)=>Math.abs(v-[...rest.translation,...rest.scale][i])<1e-5)&&Math.abs(source.rotation.reduce((sum,v,i)=>sum+v*rest.rotation[i],0))>.99999&&
        (target[t].parent<0?source.parent<0:map[target[t].parent]===source.parent),`Exact skeleton mismatch at ${target[t].name}; use rest-pose retargeting`,'MAPPING');
    }
  }
  lengths.sort((a,b)=>a-b);const automatic=lengths.length?lengths[Math.floor(lengths.length/2)]:1,scale=options.translationScale===undefined||options.translationScale==='auto'?automatic:options.translationScale;
  requireAsset(Number.isFinite(scale)&&scale>0&&scale<=10000,'translationScale must be auto or a number in (0,10000]','ARGUMENT');
  if(mode==='exact')requireAsset(Math.abs(scale-1)<1e-6&&Math.abs(orientation[3])>.999999,'Exact mode requires the same scale and orientation; use rest-pose mode for conversion','ARGUMENT');
  const morphMap=options.morphMap??{};requireAsset(ownObject(morphMap)&&Object.keys(morphMap).every(name=>header.morphs.some(m=>m.name===name)),'Unknown target morph mapping','MAPPING');
  const axes=options.rootMotionAxes??'xz';requireAsset(['xz','xyz'].includes(axes),'rootMotionAxes must be xz or xyz','ARGUMENT');
  const carrier=options.motionRoot===false?-1:options.motionRoot===undefined?target.findIndex((bone,i)=>map[i]>=0&&boneKey(bone.name)==='hips'):target.findIndex(bone=>bone.name===options.motionRoot);
  if(options.motionRoot!==undefined&&options.motionRoot!==false)requireAsset(carrier>=0&&map[carrier]>=0,'motionRoot must name a mapped target bone','MAPPING');
  const animations=asset.document.animations;requireAsset(Array.isArray(animations)&&animations.length>0&&animations.length<=64,'Motion source has no animations','BUDGET');
  if(options.clips!==undefined)requireAsset(Array.isArray(options.clips)&&options.clips.length>0&&new Set(options.clips).size===options.clips.length&&options.clips.every(name=>animations.some(a=>a.name===name)),'Unknown or duplicate selected motion clip','ARGUMENT');
  const selected=animations.filter(a=>!options.clips||options.clips.includes(a.name));requireAsset(header.clips.length+selected.length<=64,'Appended clips exceed the 64 clip budget','BUDGET');
  const prefix=options.prefix??'';requireAsset(typeof prefix==='string'&&prefix.length<=128&&!/[\x00-\x1f]/.test(prefix),'Invalid clip prefix','ARGUMENT');
  const chunks=[targetBinary],warnings=new Set(),addedClips=[],stepNames=[],rootNames=[];let offset=targetBinary.length;
  for(const [ai,animation]of animations.entries()) {
    if(!selected.includes(animation))continue;
    requireAsset(!animation.extensions&&Array.isArray(animation.channels)&&animation.channels.length>0,'Empty or extended source animation');
    const tracks=[],seen=new Set();let duration=0;
    for(const channel of animation.channels) {
      requireAsset(Number.isInteger(channel.target?.node)&&nodes[channel.target.node],'Animation references an unknown source node');
      const key=`${channel.target.node}:${channel.target.path}`;requireAsset(!seen.has(key),'Duplicate source animation channel');seen.add(key);
      if(channel.target.path==='weights')continue;
      if(!usedNodes.has(channel.target.node)){warnings.add(`Animated source bone ${nodes[channel.target.node].name} is outside the mapped target hierarchy.`);continue;}
      const track=readAnimationTrack(asset,animation,channel,{conversion});duration=Math.max(duration,track.times.at(-1));tracks.push(track);
    }
    const morphs=morphTracksFor(asset,animation,header,morphMap,warnings);for(const m of morphs)duration=Math.max(duration,m.track.times.at(-1));
    requireAsset(tracks.some(t=>usedNodes.has(t.node))||morphs.length>0,`No animation channels in ${animation.name??ai} can reach the target`,'MAPPING');
    requireAsset(duration>0&&duration<=120,'Motion duration must be in (0,120] seconds','BUDGET');
    const frames=Math.ceil(duration*60)+1,transformBytes=frames*target.length*40,shapeBytes=frames*header.morphs.length*4;
    requireAsset(offset+transformBytes+shapeBytes<=64*1024*1024,'Appended animation package exceeds 64 MiB','BUDGET');
    const bytes=Buffer.alloc(transformBytes),morphBytes=Buffer.alloc(shapeBytes);
    const routeRoot=carrier>0&&(options.motionRoot!==undefined||tracks.some(t=>t.node===map[carrier]&&t.path==='translation'));
    const stepChannels=target.map((_,t)=>{
      let mask=0;for(const track of tracks)if(track.step){let node=map[t];while(node>=0&&node!==track.node)node=nodes[node].parent;if(node>=0)mask|={translation:1,rotation:2,scale:4}[track.path];}return mask;
    });
    if(routeRoot)stepChannels[0]|=stepChannels[carrier]&1;
    for(let f=0;f<frames;f++) {
      const time=Math.min(f/60,duration),locals=nodes.map(n=>({translation:n.translation,rotation:n.rotation,scale:n.scale}));
      for(const track of tracks)locals[track.node][track.path]=sampleTrack(track,time);
      const globals=globalTransforms(nodes,sourceOrder,locals),targetAnimated=[],output=[];
      for(let t=0;t<target.length;t++) {
        const rest=targetLocals[t],s=map[t],parent=target[t].parent,p=parent<0?null:targetAnimated[parent];let local={...rest};
        if(s>=0) {
          if(mode==='exact')local={...locals[s]};
          else {
            const delta=product(globals[s].rotation,inverseQuaternion(sourceRest[s].rotation)),worldRotation=quaternion(product(product(product(orientation,delta),inverseOrientation),targetRest[t].rotation));
            const a=ancestors[t],sa=a<0?-1:map[a],difference=sub(relative(globals,s,sa),relative(sourceRest,s,sa));
            const worldDelta=rotate(orientation,sa<0?difference:rotate(sourceRest[sa].rotation,mul(difference,sourceRest[sa].scale)));
            const targetParentRest=parent<0?null:targetRest[parent],translationDelta=targetParentRest?mul(rotate(inverseQuaternion(targetParentRest.rotation),worldDelta),scale/targetParentRest.scale):mul(worldDelta,scale);
            const worldScale=targetRest[t].scale*globals[s].scale/sourceRest[s].scale,localScale=worldScale/(p?.scale??1);
            local={translation:add(rest.translation,translationDelta),rotation:p?quaternion(product(inverseQuaternion(p.rotation),worldRotation)):worldRotation,scale:[localScale,localScale,localScale]};
          }
        }
        requireAsset(uniform(local.scale)&&local.scale.every(v=>v<=100),'Retargeted scale exceeds runtime limits','BUDGET');
        const commonScale=local.scale.reduce((a,b)=>a+b,0)/3;local.scale=[commonScale,commonScale,commonScale];
        output.push(local);
        targetAnimated[t]=p?{translation:add(p.translation,rotate(p.rotation,mul(local.translation,p.scale))),rotation:quaternion(product(p.rotation,local.rotation)),scale:p.scale*commonScale}:{translation:local.translation,rotation:local.rotation,scale:commonScale};
      }
      if(routeRoot) {
        const delta=sub(relative(targetAnimated,carrier,0),relative(targetRest,carrier,0));if(axes==='xz')delta[1]=0;
        const worldDelta=rotate(targetAnimated[0].rotation,mul(delta,targetAnimated[0].scale));output[0].translation=add(output[0].translation,worldDelta);
        const parent=targetAnimated[target[carrier].parent];output[carrier].translation=sub(output[carrier].translation,mul(rotate(inverseQuaternion(parent.rotation),worldDelta),1/parent.scale));
      }
      output.forEach((local,t)=>[...local.translation,...local.rotation,...local.scale].forEach((v,k)=>{
        const q=Math.round(v*10000);requireAsset(Number.isSafeInteger(q)&&q>=-2147483648&&q<=2147483647,'Retargeted transform overflow');bytes.writeInt32LE(q,(f*target.length+t)*40+k*4);
      }));
      const shape=[...defaults];for(const m of morphs)shape[m.target]=sampleTrack(m.track,time)[m.index];
      shape.forEach((v,i)=>{requireAsset(Number.isFinite(v)&&Math.abs(v)<=8,'Retargeted morph weight exceeds [-8,8]');morphBytes.writeInt32LE(Math.round(v*10000),(f*shape.length+i)*4);});
    }
    const name=prefix+(animation.name??`clip_${ai}`);requireAsset(name.length>0&&name.length<=256&&!header.clips.some(c=>c.name===name),`Clip name collision or invalid name: ${name}`,'MAPPING');
    const meta=animation.extras?.nativegpu??{},loop=options.loop??meta.loop??true,notifies=meta.notifies??[];
    requireAsset(typeof loop==='boolean'&&Array.isArray(notifies)&&notifies.length<=256&&notifies.every(n=>Number.isInteger(n.frame)&&n.frame>=0&&n.frame<frames&&typeof n.name==='string'&&n.name.length>0),'Invalid source clip loop/notify metadata');
    let stepMorphMask=0;for(const m of morphs)if(m.track.step)stepMorphMask|=1<<m.target;
    const motion=meta.motion;let metadata={};
    if(motion) {
      requireAsset(ownObject(motion)&&Array.isArray(motion.velocity??[0,0,0])&&(motion.velocity??[0,0,0]).length===3&&(motion.velocity??[0,0,0]).every(Number.isFinite)&&Number.isFinite(motion.turnRate??0),'Invalid motion matching metadata');
      const raw=motion.velocity??[0,0,0],velocity=mul(rotate(orientation,conversion.position(raw)),scale),turnRate=rotate(orientation,[0,conversion.turnRate(motion.turnRate??0),0])[1];
      metadata={matchable:motion.matchable??true,velocity,turnRate,tags:motion.tags??1,contacts:motion.contacts??[[0,.5],[.5,1]]};
      requireAsset(typeof metadata.matchable==='boolean'&&velocity.every(v=>Math.abs(v)<=1000)&&Math.abs(turnRate)<=20&&Number.isInteger(metadata.tags)&&metadata.tags>0&&metadata.tags<=65535&&Array.isArray(metadata.contacts)&&metadata.contacts.length===2&&metadata.contacts.every(c=>Array.isArray(c)&&c.length===2&&c.every(v=>Number.isFinite(v)&&v>=0&&v<=1)),'Retargeted motion metadata exceeds runtime limits');
    }
    const clip={name,frames,offset,loop,stepChannels,...metadata,notifies:[...notifies].sort((a,b)=>a.frame-b.frame||(a.name<b.name?-1:a.name>b.name?1:0)),
      ...(header.morphs.length?{morphTargets:header.morphs.length,morphOffset:offset+bytes.length,stepMorphMask}:{})};
    header.clips.push(clip);addedClips.push(name);if(routeRoot)rootNames.push(name);if(stepChannels.some(v=>v)||stepMorphMask)stepNames.push(name);chunks.push(bytes,morphBytes);offset+=bytes.length+morphBytes.length;
  }
  const states=['idle','walk','run','jump','fall'],controller=options.controller??{};
  requireAsset(ownObject(controller)&&Object.keys(controller).every(key=>states.includes(key)),'Unknown controller binding','ARGUMENT');
  header.controller??=states.map(state=>header.clips.find(c=>c.name===state)?.name??header.clips[0].name);
  states.forEach((state,i)=>{if(controller[state]!==undefined){requireAsset(header.clips.some(c=>c.name===controller[state]),`Unknown controller clip ${controller[state]}`,'ARGUMENT');header.controller[i]=controller[state];}});
  if(options.rootMotion!==undefined){requireAsset(['extract','preserve'].includes(options.rootMotion),'rootMotion must be extract or preserve','ARGUMENT');header.rootMotion=options.rootMotion;}
  header.defaultMorphWeights=defaults;const binary=Buffer.concat(chunks);header.binaryBytes=binary.length;header.binaryHash=binaryHash(binary);
  const report={schema:'nativegpu.animation-retarget/1',mode,translationScale:scale,scaleMode:options.translationScale===undefined||options.translationScale==='auto'?'auto':'explicit',orientation,
    rootMotionCarrier:carrier>=0?target[carrier].name:null,rootMotionAxes:axes,rootMotionClips:rootNames,...(!conversion.isDefault?{coordinates:conversion.config}:{}),
    mappings:target.flatMap((bone,i)=>map[i]<0?[]:[{target:bone.name,source:nodes[map[i]].name,sourceNode:map[i]}]),
    unmappedTargetBones:target.filter((bone,i)=>map[i]<0&&!/__skin_\d+$/.test(bone.name)).map(b=>b.name),addedClips,stepClips:stepNames};
  for(const name of report.unmappedTargetBones)warnings.add(`Target bone ${name} keeps its authored local rest transform.`);
  if(!lengths.length&&(options.translationScale===undefined||options.translationScale==='auto'))warnings.add('No matched bone lengths were available; translationScale defaults to 1.');
  return {header,binary,report,warnings:[...warnings],addedClips};
}

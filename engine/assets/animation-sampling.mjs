import {requireAsset,readAccessor} from './gltf-document.mjs';

export const add=(a,b)=>a.map((v,i)=>v+b[i]);
export const sub=(a,b)=>a.map((v,i)=>v-b[i]);
export const mul=(a,s)=>a.map(v=>v*s);
export const representableUniformScale=v=>v.length===3&&v.every(n=>Number.isFinite(n)&&n>0)&&
  (Math.max(...v)-Math.min(...v)<1e-6||v.every(n=>Math.round(n*10000)===Math.round(v[0]*10000)));
export function quaternion(q) {
  const length=Math.hypot(...q);requireAsset(q.length===4&&q.every(Number.isFinite)&&length>1e-8,'Invalid animation quaternion');return q.map(v=>v/length);
}
export const inverseQuaternion=q=>[-q[0],-q[1],-q[2],q[3]];
export function product(a,b) {
  const [x,y,z,w]=a,[u,v,s,t]=b;return [w*u+x*t+y*s-z*v,w*v-x*s+y*t+z*u,w*s+x*v-y*u+z*t,w*t-x*u-y*v-z*s];
}
export function rotate(q,v) {return product(product(q,[...v,0]),inverseQuaternion(q)).slice(0,3);}
export function slerp(a,b,t) {
  a=quaternion(a);b=quaternion(b);let dot=a.reduce((sum,v,i)=>sum+v*b[i],0);
  if(dot<0){dot=-dot;b=mul(b,-1);}dot=Math.min(1,dot);
  if(dot>.9995)return quaternion(a.map((v,i)=>v+(b[i]-v)*t));
  const angle=Math.acos(dot),denominator=Math.sin(angle);return a.map((v,i)=>(v*Math.sin((1-t)*angle)+b[i]*Math.sin(t*angle))/denominator);
}
export function readAnimationTrack(asset,animation,channel,{weightCount=0,mirror=false,conversion}={}) {
  const path=channel.target?.path,spec=animation.samplers?.[channel.sampler];
  requireAsset(['translation','rotation','scale','weights'].includes(path)&&spec&&!channel.extensions&&!spec.extensions,'Unsupported animation channel','UNSUPPORTED');
  const input=asset.document.accessors?.[spec.input],output=asset.document.accessors?.[spec.output];
  requireAsset(input?.componentType===5126,'Animation times must use floating-point seconds');
  const allowed=path==='rotation'||path==='weights'?[5120,5121,5122,5123,5126]:[5126];
  requireAsset(output&&allowed.includes(output.componentType)&&(output.componentType===5126||output.normalized),'Invalid animation output component type');
  const times=readAccessor(asset,spec.input,'SCALAR').flat(),interpolation=spec.interpolation??'LINEAR',cubic=interpolation==='CUBICSPLINE';
  requireAsset(times[0]>=0&&times.every((v,i)=>i===0||v>times[i-1]),'Animation key times must increase');
  requireAsset(['LINEAR','STEP','CUBICSPLINE'].includes(interpolation)&&(!cubic||times.length>=2),'Unsupported animation interpolation','UNSUPPORTED');
  let values=readAccessor(asset,spec.output,path==='weights'?'SCALAR':path==='rotation'?'VEC4':'VEC3');
  requireAsset(path!=='weights'||Number.isInteger(weightCount)&&weightCount>0&&weightCount<=16,'Weight animation requires morph targets');
  requireAsset(values.length===times.length*(cubic?3:1)*(path==='weights'?weightCount:1),'Animation key count mismatch');
  if(path==='weights')values=Array.from({length:values.length/weightCount},(_,i)=>values.slice(i*weightCount,(i+1)*weightCount).flat());
  if(conversion&&path==='translation')values=values.map(conversion.position);
  else if(mirror&&path==='translation')values=values.map(v=>[v[0],v[1],-v[2]]);
  if(conversion&&path==='rotation')values=values.map(conversion.rotation);
  else if(mirror&&path==='rotation')values=values.map(v=>[-v[0],-v[1],v[2],v[3]]);
  if(path==='rotation')values.forEach((v,i)=>{if(!cubic||i%3===1)quaternion(v);});
  if(path==='scale')values.forEach((v,i)=>{if(!cubic||i%3===1)requireAsset(representableUniformScale(v),'Animation scale must be positive and uniform','UNSUPPORTED');});
  return {node:channel.target.node,path,times,values,cubic,step:interpolation==='STEP'};
}
export function sampleTrack(track,time) {
  let lo=0,hi=track.times.length-1;
  while(lo<hi){const middle=Math.ceil((lo+hi)/2);if(track.times[middle]<=time)lo=middle;else hi=middle-1;}
  const a=lo,b=Math.min(a+1,track.times.length-1),u=track.step||a===b?0:Math.max(0,Math.min(1,(time-track.times[a])/(track.times[b]-track.times[a])));
  let value;
  if(track.cubic) {
    const h=track.times[b]-track.times[a],u2=u*u,u3=u2*u;
    value=track.values[a*3+1].map((v,k)=>(2*u3-3*u2+1)*v+(u3-2*u2+u)*h*track.values[a*3+2][k]+(-2*u3+3*u2)*track.values[b*3+1][k]+(u3-u2)*h*track.values[b*3][k]);
  }else value=track.path==='rotation'?slerp(track.values[a],track.values[b],u):track.values[a].map((v,k)=>v+(track.values[b][k]-v)*u);
  if(track.path==='scale'){requireAsset(representableUniformScale(value),'Sampled animation scale must be positive and uniform','UNSUPPORTED');const common=value.reduce((a,b)=>a+b,0)/3;return [common,common,common];}
  return track.path==='rotation'?quaternion(value):value;
}
export function globalTransforms(nodes,order,locals) {
  const output=[];
  for(const i of order) {
    const local=locals[i],parent=nodes[i].parent>=0?output[nodes[i].parent]:null;
    requireAsset(local.scale.length===3&&local.scale.every(v=>Number.isFinite(v)&&v>0)&&Math.max(...local.scale)-Math.min(...local.scale)<1e-5,'Retargeting requires positive uniform bone scale','UNSUPPORTED');
    const scale=local.scale.reduce((a,b)=>a+b,0)/3;output[i]=parent?{translation:add(parent.translation,rotate(parent.rotation,mul(local.translation,parent.scale))),rotation:quaternion(product(parent.rotation,local.rotation)),scale:parent.scale*scale}:{translation:[...local.translation],rotation:quaternion(local.rotation),scale};
  }
  return output;
}

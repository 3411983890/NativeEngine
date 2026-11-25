import {requireAsset,identityMatrix,multiplyMatrix} from './gltf-document.mjs';

const units={m:1,cm:.01,mm:.001,ft:.3048,in:.0254};
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const dot=(a,b)=>a.reduce((sum,v,i)=>sum+v*b[i],0);
const axis=value=>{requireAsset(typeof value==='string'&&/^[+-]?[xyz]$/i.test(value),'Coordinate axes must be signed X, Y or Z','ARGUMENT');return (value.startsWith('-')?'-':'+')+value.at(-1).toUpperCase();};
const vector=name=>['X','Y','Z'].map(key=>name.at(-1)===key?(name[0]==='-'?-1:1):0);

// A single change of basis is shared by geometry, local TRS, inverse binds,
// curve tangents and feature metadata. The engine's authored front is -Z.
export function coordinateTransform(options={}) {
  requireAsset(options&&typeof options==='object'&&!Array.isArray(options),'coordinates must be an object','ARGUMENT');
  requireAsset(Object.keys(options).every(key=>['unit','metersPerUnit','upAxis','forwardAxis','handedness','yawDegrees'].includes(key)),'Unknown coordinate option','ARGUMENT');
  requireAsset(options.unit===undefined||Object.hasOwn(units,options.unit),'unit must be m, cm, mm, ft or in','ARGUMENT');
  requireAsset(options.unit===undefined||options.metersPerUnit===undefined,'Choose unit or metersPerUnit, not both','ARGUMENT');
  const scale=options.metersPerUnit??units[options.unit??'m'];
  requireAsset(Number.isFinite(scale)&&scale>=.000001&&scale<=1000,'metersPerUnit must be in [0.000001,1000]','ARGUMENT');
  const upAxis=axis(options.upAxis??'+Y'),forwardAxis=axis(options.forwardAxis??'+Z'),handedness=options.handedness??'right';
  requireAsset(upAxis.at(-1)!==forwardAxis.at(-1),'Up and forward axes must be perpendicular','ARGUMENT');
  requireAsset(['right','left'].includes(handedness),'handedness must be right or left','ARGUMENT');
  const yaw=options.yawDegrees??0;requireAsset(Number.isFinite(yaw)&&Math.abs(yaw)<=3600,'yawDegrees must be in [-3600,3600]','ARGUMENT');
  const yawDegrees=((yaw%360)+360)%360,config={metersPerUnit:scale,upAxis,forwardAxis,handedness,yawDegrees};
  const isDefault=scale===1&&upAxis==='+Y'&&forwardAxis==='+Z'&&handedness==='right'&&yawDegrees===0;
  const up=vector(upAxis),front=vector(forwardAxis),determinant=handedness==='right'?-1:1;
  const right=cross(up,front).map(v=>handedness==='right'?v:-v),base=[right,up,front.map(v=>-v)];
  const angle=yawDegrees*Math.PI/180,c=Math.cos(angle),s=Math.sin(angle);
  const rows=[base[0].map((v,i)=>c*v-s*base[2][i]),base[1],base[0].map((v,i)=>s*v+c*base[2][i])].map(row=>row.map(v=>Math.abs(v)<1e-15?0:v));
  const direction=v=>isDefault?[v[0],v[1],-v[2]]:rows.map(row=>dot(row,v));
  const position=v=>isDefault?[v[0],v[1],-v[2]]:direction(v).map(n=>n*scale);
  const rotation=q=>isDefault?[-q[0],-q[1],q[2],q[3]]:[...direction(q.slice(0,3)).map(n=>n*determinant),q[3]];
  const matrix=identityMatrix(),inverse=identityMatrix();for(let col=0;col<3;col++)for(let row=0;row<3;row++){matrix[col*4+row]=rows[row][col]*scale;inverse[col*4+row]=rows[col][row]/scale;}
  const transformMatrix=m=>isDefault?m.map((v,i)=>v*((i%4===2?-1:1)*(Math.floor(i/4)===2?-1:1))):multiplyMatrix(matrix,multiplyMatrix(m,inverse));
  const turnRate=value=>determinant*direction(up.map(v=>v*value))[1];
  return {config,isDefault,scale,determinant,rows,matrix,inverse,position,direction,rotation,transformMatrix,turnRate};
}

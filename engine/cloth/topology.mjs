// Deterministic offline scheduling for triangular strain and four-point hinges.
// A batch includes every vertex read or written by its constraints, including pins.
const requireValue=(ok,message)=>{if(!ok)throw new Error(`NG-E-CLOTH-TOPOLOGY: ${message}`);};
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const length=a=>Math.hypot(...a);

export function colorConstraints(vertices,vertexCount){
  const used=Array.from({length:vertexCount},()=>new Set()),colors=[];
  for(let id=0;id<vertices.length;++id){
    const ids=vertices[id];requireValue(new Set(ids).size===ids.length,'repeated constraint vertex');
    requireValue(ids.every(v=>Number.isSafeInteger(v)&&v>=0&&v<vertexCount),'invalid constraint vertex');
    const forbidden=new Set(ids.flatMap(v=>[...used[v]]));let color=0;while(forbidden.has(color))++color;
    requireValue(color<128,'constraint coloring budget exceeded');
    (colors[color]??=[]).push(id);for(const v of ids)used[v].add(color);
  }
  return colors;
}

export function bakeTriangleTopology(positions,triangles){
  const membranes=[],hinges=[],edges=new Map(),adjacency=Array.from({length:positions.length},()=>[]);
  for(let id=0;id<triangles.length;++id){
    const t=triangles[id];requireValue(t.length===3&&new Set(t).size===3,'invalid triangle');
    const [a,b,c]=t.map(v=>positions[v]);requireValue(a&&b&&c,'triangle outside mesh');
    const e0=sub(b,a),e1=sub(c,a),det=e0[0]*e1[1]-e1[0]*e0[1],area=length(cross(e0,e1))*.5;
    requireValue(Number.isFinite(area)&&area>1e-12&&Math.abs(det)>1e-12,'degenerate membrane');
    // Grid authoring uses X/Y as material warp/weft, with no half-cell offset.
    membranes.push({inverse:[e1[1]/det,-e0[1]/det,-e1[0]/det,e0[0]/det],area});
    for(const v of t)adjacency[v].push(id);
    for(let i=0;i<3;++i){const a=t[i],b=t[(i+1)%3],opposite=t[(i+2)%3],key=a<b?`${a},${b}`:`${b},${a}`;
      const list=edges.get(key)??[];list.push({a,b,opposite});requireValue(list.length<=2,'non-manifold edge');edges.set(key,list);}
  }
  for(const adjacent of edges.values())if(adjacent.length===2){
    const [a,b]=adjacent;requireValue(a.a===b.b&&a.b===b.a,'inconsistent triangle winding');
    const vertices=[a.a,a.b,a.opposite,b.opposite],p=vertices.map(v=>positions[v]);const e=sub(p[1],p[0]),n0=cross(e,sub(p[2],p[0])),n1=cross(sub(p[3],p[0]),e);
    const scale=length(n0)*length(n1),sine=dot(cross(n0,n1),e)/(scale*length(e)),cosine=dot(n0,n1)/scale;
    hinges.push({vertices,angle:Math.atan2(sine,cosine)});
  }
  return {model:'triangle-xpbd/1',membranes,hinges,membraneColors:colorConstraints(triangles,positions.length),hingeColors:colorConstraints(hinges.map(h=>h.vertices),positions.length),adjacency};
}

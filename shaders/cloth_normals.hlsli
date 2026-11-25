float3 clothNormalPosition(uint id){
#if defined(CLOTH_BATCH)
return clothPosition(id);
#else
return vertices[id].position.xyz;
#endif
}
void updateClothNormal(uint id){
    if(id>=vertexCount)return;float3 normal=0,warp=0,weft=0;
    for(uint index=adjacency[id];index<adjacency[id+1];++index){Membrane m=membranes[adjacency[adjacencyBase+index]];float3 e0=clothNormalPosition(m.ids.y)-clothNormalPosition(m.ids.x),e1=clothNormalPosition(m.ids.z)-clothNormalPosition(m.ids.x);normal+=cross(e0,e1);warp+=(e0*m.inverse.x+e1*m.inverse.y)*m.data.x;weft+=(e0*m.inverse.z+e1*m.inverse.w)*m.data.x;}
    float size=length(normal);if(size<1e-9||any(!isfinite(normal))){normal=float3(0,0,1);countStat(10,1);}else normal/=size;float3 tangent=warp-normal*dot(normal,warp);size=length(tangent);if(size<1e-9)tangent=abs(normal.x)<.9?normalize(cross(float3(1,0,0),normal)):normalize(cross(float3(0,1,0),normal));else tangent/=size;
    vertices[id].normal=float4(normal,0);vertices[id].tangent=float4(tangent,dot(cross(normal,tangent),weft)<0?-1:1);
}

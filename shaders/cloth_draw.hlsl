cbuffer ClothDrawConstants:register(b0){
    column_major float4x4 vp;
    float4 cameraWarp,baseSheen,keyIntensity,keyWeft,ambientWeave;
    uint columns,rows,mode,reserved;float4 repeat;
};
struct PixelInput {float4 position:SV_POSITION;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;float4 tangent:TEXCOORD2;float2 uv:TEXCOORD3;};
#if defined(PASS_VS)
struct Vertex{float4 position,previous,velocity,normal,tangent;};
struct Membrane{uint4 vertices;float4 inverse,data;};
StructuredBuffer<Vertex> vertices:register(t0);
StructuredBuffer<Membrane> membranes:register(t1);
PixelInput VSMain(uint id:SV_VertexID){
    uint index=membranes[id/3].vertices[id%3];Vertex v=vertices[index];PixelInput o;
    o.position=mul(vp,float4(v.position.xyz,1));o.world=v.position.xyz;o.normal=v.normal.xyz;o.tangent=v.tangent;
    // U/V agree with the baked material +X/+Y axes and tangent handedness.
    o.uv=float2(index%columns,index/columns)/float2(columns-1,rows-1);return o;
}
#endif
#if defined(PASS_PS)
struct PixelOutput{float4 surface:SV_TARGET0;float4 key:SV_TARGET1;};
float3 safeNormal(float3 value,float3 fallback){float size=dot(value,value);return size>1e-12?value*rsqrt(size):fallback;}
PixelOutput PSMain(PixelInput p,bool front:SV_IsFrontFace){
    float3 n=safeNormal(p.normal,float3(0,0,1))*(front?1:-1);
    float3 t=safeNormal(p.tangent.xyz-n*dot(n,p.tangent.xyz),float3(1,0,0));
    float3 b=safeNormal(cross(n,t),float3(0,1,0))*p.tangent.w*(front?1:-1);
    PixelOutput o;o.key=0;
    if(mode!=0){o.surface=float4(mode==1?n*.5+.5:mode==2?float3(p.uv,0):t*.5+.5,1);return o;}
    float2 cycles=p.uv*repeat.xy;
    // Filter out subpixel threads instead of allowing distant weave to shimmer.
    float2 fade=saturate(1-2*fwidth(cycles));
    n=safeNormal(n+ambientWeave.w*(sin(cycles.x*6.2831853)*fade.x*t+sin(cycles.y*6.2831853)*fade.y*b),n);
    float3 v=safeNormal(cameraWarp.xyz-p.world,n),l=keyIntensity.xyz,h=safeNormal(v+l,n);
    float nl=saturate(dot(n,l)),nv=saturate(dot(n,v)),nh=saturate(dot(n,h));
    float ht=dot(h,t),hb=dot(h,b),direction=ht*ht/max(ht*ht+hb*hb,1e-6);
    // Directional interpolation is a bounded weave approximation, not a
    // normalized anisotropic NDF. Charlie + Neubelt give the soft sheen lobe.
    float rough=lerp(keyWeft.w,cameraWarp.w,direction),inverseRough=rcp(rough);
    float distribution=(2+inverseRough)*pow(max(1-nh*nh,0.0078125),inverseRough*.5)/6.2831853;
    float visibility=rcp(max(4*(nl+nv-nl*nv),1e-5));
    float3 sheen=sqrt(baseSheen.rgb)*baseSheen.w*distribution*visibility;
    o.surface=float4(baseSheen.rgb*ambientWeave.rgb,1);
    o.key=float4((baseSheen.rgb/3.14159265+sheen)*nl*keyWeft.rgb*keyIntensity.w,1);return o;
}
#endif

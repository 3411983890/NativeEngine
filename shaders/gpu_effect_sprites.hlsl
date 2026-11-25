cbuffer Constants : register(b0) {
    column_major float4x4 viewProjection;
    uint positionOffset; uint colorOffset; uint sizeOffset; uint sourceCapacity;
    float3 origin; uint localSpace;
    uint capacity; uint textureIndex; uint blendKind; uint batchOrdinal;
    uint sortK; uint sortJ; uint2 reserved;
    float4 cameraRight;
    float4 cameraUp;
};
struct Sprite {float4 positionWidth;float4 color;float4 shape;uint4 meta;};
#if defined(PASS_COMPUTE)
ByteAddressBuffer attributes : register(t0);
StructuredBuffer<uint> alive : register(t1);
ByteAddressBuffer sourceCounters : register(t2);
RWStructuredBuffer<Sprite> sprites : register(u0);
// count, additive, premultiplied, cutout, dropped, faults, padded, reserved...
RWByteAddressBuffer state : register(u1);
RWByteAddressBuffer dispatchArgs : register(u2);
RWByteAddressBuffer drawArgs : register(u3);
void barrierStat(uint offset,uint count) {
    uint old=state.Load(offset);
    while(true){uint value=old>0xffffffff-count?0xffffffff:old+count,seen;state.InterlockedCompareExchange(offset,old,value,seen);if(seen==old)return;old=seen;}
}
[numthreads(1,1,1)]
void CSReset(uint3 tid:SV_DispatchThreadID) {for(uint i=0;i<16;++i)state.Store(i*4,0);}
[numthreads(1,1,1)]
void CSPrepareSource(uint3 tid:SV_DispatchThreadID) {
    uint count=sourceCounters.Load(0);if(count>sourceCapacity){barrierStat(20,1);count=sourceCapacity;}
    dispatchArgs.Store3(0,uint3((count+63)/64,1,1));
}
[numthreads(64,1,1)]
void CSGather(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=min(sourceCounters.Load(0),sourceCapacity))return;
    uint slot=alive[tid.x];if(slot>=sourceCapacity){barrierStat(20,1);return;}
    uint index=state.Load(0);
    while(index<capacity){uint seen;state.InterlockedCompareExchange(0,index,index+1,seen);if(seen==index)break;index=seen;}
    if(index>=capacity){barrierStat(16,1);return;}
    Sprite s;float3 p=asfloat(attributes.Load3(positionOffset+slot*12))+(localSpace?origin:0);
    float2 size=asfloat(attributes.Load2(sizeOffset+slot*8));
    s.positionWidth=float4(p,size.x);s.color=asfloat(attributes.Load4(colorOffset+slot*16));
    float4 clip=mul(viewProjection,float4(p,1));float depth=abs(clip.w)>1e-20?clip.z/clip.w:0;
    s.shape=float4(size.y,0,clamp(depth,-1e30,1e30),0);s.meta=uint4(textureIndex,blendKind,batchOrdinal,slot);
    sprites[index]=s;uint ignored;state.InterlockedAdd(4+blendKind*4,1,ignored);
}
[numthreads(1,1,1)]
void CSFinish(uint3 tid:SV_DispatchThreadID) {
    uint count=state.Load(0),padded=1;while(padded<count)padded*=2;state.Store(24,padded);
    uint add=state.Load(4),alpha=state.Load(8),cutout=state.Load(12);
    // SV_InstanceID indexes the structured buffer relative to this draw. Keep
    // the GPU-computed segment offset explicit instead of relying on IA offsets.
    state.Store3(32,uint3(cutout+alpha,cutout,0));
    drawArgs.Store4(0,uint4(6,add,0,0));
    drawArgs.Store4(16,uint4(6,alpha,0,0));
    drawArgs.Store4(32,uint4(6,cutout,0,0));
    dispatchArgs.Store3(0,uint3((padded+63)/64,1,1));
}
[numthreads(64,1,1)]
void CSPad(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x;if(i>=state.Load(0)&&i<state.Load(24)){Sprite s=(Sprite)0;s.meta=0xffffffff;sprites[i]=s;}
}
[numthreads(1,1,1)]
void CSPrepareSort(uint3 tid:SV_DispatchThreadID) {
    uint padded=state.Load(24);dispatchArgs.Store3(0,uint3(sortK<=padded?(padded+63)/64:0,1,1));
}
uint rank(Sprite s){return s.meta.y==0xffffffff?3:2-s.meta.y;}
bool before(Sprite a,Sprite b) {
    uint ra=rank(a),rb=rank(b);if(ra!=rb)return ra<rb;
    if(ra==3)return false;
    if(a.meta.y==0&&a.meta.x!=b.meta.x)return a.meta.x<b.meta.x;
    if(a.shape.z!=b.shape.z)return a.meta.y==1?a.shape.z>b.shape.z:a.shape.z<b.shape.z;
    if(a.meta.z!=b.meta.z)return a.meta.z<b.meta.z;
    return a.meta.w<b.meta.w;
}
[numthreads(64,1,1)]
void CSSort(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x,j=i^sortJ,padded=state.Load(24);if(i>=padded||j>=padded||j<=i)return;
    Sprite a=sprites[i],b=sprites[j];bool ascending=(i&sortK)==0;
    if(ascending?before(b,a):before(a,b)){sprites[i]=b;sprites[j]=a;}
}
#endif
struct PixelInput {float4 position:SV_POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;nointerpolation uint texture:TEXCOORD1;nointerpolation uint blend:TEXCOORD2;};
#if defined(PASS_VS)
StructuredBuffer<Sprite> drawSprites : register(t3);
ByteAddressBuffer drawState : register(t2);
PixelInput VSMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    Sprite s=drawSprites[instance+drawState.Load(32+blendKind*4)];float2 uv=corners[vertex],offset=(uv-0.5)*float2(s.positionWidth.w,s.shape.x);
    float3 world=s.positionWidth.xyz+cameraRight.xyz*offset.x-cameraUp.xyz*offset.y;
    PixelInput o;o.position=mul(viewProjection,float4(world,1));o.uv=uv;o.color=s.color;o.texture=s.meta.x;o.blend=s.meta.y;return o;
}
#endif
#if defined(PASS_PS)
SamplerState spriteSampler : register(s0);
struct PixelOutput {float4 surface:SV_TARGET0;float4 keyLight:SV_TARGET1;};
PixelOutput PSMain(PixelInput input) {
    Texture2D texture=ResourceDescriptorHeap[NonUniformResourceIndex(input.texture)];float4 texel=texture.Sample(spriteSampler,input.uv);
    float alpha=saturate(texel.a*input.color.a);clip(alpha-(input.blend==2?0.5:0.0001));if(input.blend==2)alpha=1;
    PixelOutput o;o.surface=float4(texel.rgb*input.color.rgb*alpha,alpha);o.keyLight=float4(0,0,0,alpha);return o;
}
#endif

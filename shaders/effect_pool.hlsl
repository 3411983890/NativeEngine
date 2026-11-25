cbuffer PoolConstants : register(b0) {
    uint capacity; uint requested; uint ageLow; uint ageHigh;
    uint positionOffset; uint velocityOffset; uint ageOffset; uint lifetimeOffset;
    uint idOffset; uint colorOffset; uint sizeOffset; uint moduleCount;
    uint2 sequence; uint2 emitterSeed;
    float3 origin; uint localSpace;
    float3 emitterPosition; uint shape;
    float3 extent; float radius;
    float3 lineEnd; float angle;
    uint lifeMin; uint lifeMax; uint2 reserved0;
    uint instanceCapacity; uint instanceSpawns; uint globalCapacity; uint globalSpawns;
};
struct Module {uint4 meta;float4 vector;float4 color;float4 sizeCoeff;};
struct Key {float4 time;float4 value;};
StructuredBuffer<Module> modules : register(t0);
StructuredBuffer<Key> keys : register(t1);
RWByteAddressBuffer attributes : register(u0);
RWStructuredBuffer<uint> aliveIn : register(u1);
RWStructuredBuffer<uint> aliveOut : register(u2);
RWStructuredBuffer<uint> dead : register(u3);
RWByteAddressBuffer counters : register(u4);
RWByteAddressBuffer dispatchArgs : register(u5);
RWByteAddressBuffer drawArgs : register(u6);
RWByteAddressBuffer instanceBudget : register(u7);
RWByteAddressBuffer globalBudget : register(u8);
#if defined(FIELD_VELOCITY)
cbuffer VectorFieldConstants : register(b1) {
    #include "field_constants.hlsli"
    uint fieldTableDescriptor; uint3 fieldWind;
    uint3 fieldWater; uint fieldFilter;
    float3 fieldOrigin; float fieldInverseCell;
    float3 fieldWindScale; uint fieldFlags;
    float3 fieldWaterScale; uint fieldReserved;
};
#define FIELD_EXTERNAL_CONSTANTS
#define FIELD_PAGE_TABLE_DESCRIPTOR fieldTableDescriptor
#include "field_sampling.hlsli"
#endif
static const float dt=1.0/120.0;
uint2 add64(uint2 a,uint2 b) {uint lo=a.x+b.x;return uint2(lo,a.y+b.y+(lo<a.x));}
uint2 shift64(uint2 a,uint n) {return uint2((a.x>>n)|(a.y<<(32-n)),a.y>>n);}
uint2 product32(uint a,uint b) {
    uint a0=a&65535,a1=a>>16,b0=b&65535,b1=b>>16,c0=a1*b0,c1=a0*b1;
    return add64(add64(uint2(a0*b0,a1*b1),uint2(c0<<16,c0>>16)),uint2(c1<<16,c1>>16));
}
uint2 multiply64(uint2 a,uint2 b) {uint2 p=product32(a.x,b.x);p.y+=a.x*b.y+a.y*b.x;return p;}
uint2 mix64(uint2 value) {
    value=add64(value,uint2(0x7f4a7c15,0x9e3779b9));
    value=multiply64(value^shift64(value,30),uint2(0x1ce4e5b9,0xbf58476d));
    value=multiply64(value^shift64(value,27),uint2(0x133111eb,0x94d049bb));
    return value^shift64(value,31);
}
uint randomBits(uint2 seed,uint channel) {
    uint2 value=mix64(add64(seed,multiply64(uint2(channel,0),uint2(0x7f4a7c15,0x9e3779b9))));
    return value.y>>8;
}
float randomValue(uint2 seed,uint channel) {return randomBits(seed,channel)*(1.0/16777216.0);}
float3 unit(float3 v) {float l=length(v);return l>1e-8?v/l:float3(0,1,0);}
float4 curve(Module m,float t) {
    Key first=keys[m.meta.y];if(t<=first.time.x)return first.value;
    for(uint i=1;i<m.meta.z;++i) {
        Key a=keys[m.meta.y+i-1],b=keys[m.meta.y+i];
        if(t<=b.time.x)return t==b.time.x?b.value:lerp(a.value,b.value,(t-a.time.x)/(b.time.x-a.time.x));
    }
    return keys[m.meta.y+m.meta.z-1].value;
}
// Monotonic statistics have a saturating representation rather than wrapping
// and pretending a long-running effect emitted fewer particles.
void incrementStat(uint offset,uint count) {
    uint old=counters.Load(offset);
    while(true) {uint updated=old>0xffffffff-count?0xffffffff:old+count;uint seen;counters.InterlockedCompareExchange(offset,old,updated,seen);if(seen==old)return;old=seen;}
}
void fault() {incrementStat(32,1);}
void budgetStat(RWByteAddressBuffer budget,uint offset,uint count) {
    uint old=budget.Load(offset);
    while(true){uint updated=old>0xffffffff-count?0xffffffff:old+count;uint seen;budget.InterlockedCompareExchange(offset,old,updated,seen);if(seen==old)return;old=seen;}
}
void releaseBudget(RWByteAddressBuffer budget,uint count,uint statistic) {
    // The shared live count includes every pool; release happens before spawn.
    uint old;budget.InterlockedAdd(0,0-count,old);
    if(old<count)budgetStat(budget,20,1);
    budgetStat(budget,statistic,count);
}
void expire() {
    incrementStat(24,1);
    if(reserved0.y&1)releaseBudget(instanceBudget,1,12);
    if(reserved0.y&2)releaseBudget(globalBudget,1,12);
}
#if defined(FIELD_VELOCITY)
bool velocityField(float3 position,uint kind,out float3 velocity) {
    velocity=0;float3 lattice=(position-fieldOrigin)*fieldInverseCell;
    // Bounded float mapping avoids integer conversion overflow and records an
    // explicit unavailable sample outside the supported visual coordinate range.
    if(any(!isfinite(lattice))||any(abs(lattice)>8191.0)||(fieldFlags&kind)==0){incrementStat(40,1);incrementStat(56,1);return false;}
    int3 cell=(int3)floor(lattice);uint3 fraction=(uint3)floor((lattice-floor(lattice))*256.0);
    uint3 components=kind==1?fieldWind:fieldWater;float3 scales=kind==1?fieldWindScale:fieldWaterScale;
    FieldAnswer a=sampleField(cell,components.x,fraction,fieldFilter),b=sampleField(cell,components.y,fraction,fieldFilter),c=sampleField(cell,components.z,fraction,fieldFilter);
    if(a.status!=0||b.status!=0||c.status!=0){incrementStat(40,1);
        if(a.status==7||b.status==7||c.status==7)incrementStat(44,1);
        if(a.status==4||b.status==4||c.status==4)incrementStat(48,1);
        if((a.status!=0&&a.status!=4&&a.status!=5&&a.status!=7)||(b.status!=0&&b.status!=4&&b.status!=5&&b.status!=7)||(c.status!=0&&c.status!=4&&c.status!=5&&c.status!=7))incrementStat(56,1);
        return false;
    }
    velocity=float3(a.value,b.value,c.value)*scales;
    incrementStat(36,1);if(a.clamped||b.clamped||c.clamped)incrementStat(52,1);
    if(less64(maxAge,a.age)||less64(maxAge,b.age)||less64(maxAge,c.age))incrementStat(44,1);return true;
}
#endif
[numthreads(1,1,1)]
void CSBudget(uint3 tid:SV_DispatchThreadID) {
    if(reserved0.y&16){for(uint i=0;i<8;++i)instanceBudget.Store(i*4,0);}
    if(reserved0.y&32){for(uint i=0;i<8;++i)globalBudget.Store(i*4,0);}
    if(reserved0.y&(4|16))instanceBudget.Store(4,instanceSpawns);
    if(reserved0.y&(8|32))globalBudget.Store(4,globalSpawns);
}
[numthreads(64,1,1)]
void CSInit(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x;if(i<capacity)dead[i]=capacity-1-i;
    if(i==0){
        if(reserved0.y&64){uint count=counters.Load(0);if(reserved0.y&1)releaseBudget(instanceBudget,count,24);if(reserved0.y&2)releaseBudget(globalBudget,count,24);}
        for(uint j=0;j<16;++j)counters.Store(4*j,0);counters.Store(8,capacity);dispatchArgs.Store3(0,uint3(0,1,1));drawArgs.Store4(0,uint4(6,0,0,0));
    }
}
[numthreads(1,1,1)]
void CSPrepareSim(uint3 tid:SV_DispatchThreadID) {
    uint count=counters.Load(0);if(count>capacity){fault();count=capacity;}
    counters.Store(4,0);dispatchArgs.Store3(0,uint3((count+63)/64,1,1));
}
[numthreads(64,1,1)]
void CSSimulate(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x;if(i>=counters.Load(0))return;uint slot=aliveIn[i];if(slot>=capacity){fault();return;}
    uint age=attributes.Load(ageOffset+slot*4)+1,life=attributes.Load(lifetimeOffset+slot*4);
    if(age>=life) {
        uint index;counters.InterlockedAdd(8,1,index);if(index<capacity)dead[index]=slot;else fault();
        expire();return;
    }
    attributes.Store(ageOffset+slot*4,age);
    float3 p=asfloat(attributes.Load3(positionOffset+slot*12)),v=asfloat(attributes.Load3(velocityOffset+slot*12));
    float4 color=asfloat(attributes.Load4(colorOffset+slot*16));float2 size=asfloat(attributes.Load2(sizeOffset+slot*8));
    float t=(float)age/(float)life;
    // age modulo 1,000,000 is passed separately by the CPU in reserved0.x.
    float time=reserved0.x*dt;
    for(uint m=0;m<moduleCount;++m) {
        Module module=modules[m];
        if(module.meta.x==1)v+=module.vector.xyz*dt;
        else if(module.meta.x==2)v*=exp(-module.sizeCoeff.z*dt);
        else if(module.meta.x==3)color=curve(module,t);
        else if(module.meta.x==4)size=curve(module,t).xy;
        else if(module.meta.x==5){float3 q=p*module.sizeCoeff.w;v+=float3(cos(q.y-time),cos(q.z+time),cos(q.x+2*time))*(module.sizeCoeff.z*dt);}
#if defined(FIELD_VELOCITY)
        else if(module.meta.x==7){float3 target;if(velocityField(p+(localSpace?origin:0),module.meta.w,target))v=lerp(v,target,1-exp(-module.sizeCoeff.z*dt));}
#endif
    }
    p+=v*dt;
    if(any(!isfinite(p))||any(!isfinite(v))){uint index;counters.InterlockedAdd(8,1,index);if(index<capacity)dead[index]=slot;fault();expire();return;}
    attributes.Store3(positionOffset+slot*12,asuint(p));attributes.Store3(velocityOffset+slot*12,asuint(v));
    attributes.Store4(colorOffset+slot*16,asuint(color));attributes.Store2(sizeOffset+slot*8,asuint(size));
    uint index;counters.InterlockedAdd(4,1,index);if(index<capacity)aliveOut[index]=slot;else fault();
}
[numthreads(1,1,1)]
void CSPrepareSpawn(uint3 tid:SV_DispatchThreadID) {
    uint free=counters.Load(8),count=min(requested,free);
    if(reserved0.y&1)count=min(count,min(instanceCapacity-min(instanceCapacity,instanceBudget.Load(0)),instanceBudget.Load(4)));
    if(reserved0.y&2)count=min(count,min(globalCapacity-min(globalCapacity,globalBudget.Load(0)),globalBudget.Load(4)));
    // This one-thread reservation is serialized between emitters by a UAV
    // barrier; no CPU live-count readback participates in admission.
    if(reserved0.y&1){instanceBudget.Store(0,instanceBudget.Load(0)+count);instanceBudget.Store(4,instanceBudget.Load(4)-count);budgetStat(instanceBudget,8,count);budgetStat(instanceBudget,16,requested-count);}
    if(reserved0.y&2){globalBudget.Store(0,globalBudget.Load(0)+count);globalBudget.Store(4,globalBudget.Load(4)-count);budgetStat(globalBudget,8,count);budgetStat(globalBudget,16,requested-count);}
    counters.Store(8,free-count);counters.Store(12,count);counters.Store(16,free-count);
    incrementStat(28,requested-count);incrementStat(20,count);
    dispatchArgs.Store3(0,uint3((count+63)/64,1,1));
}
[numthreads(64,1,1)]
void CSSpawn(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x;if(i>=counters.Load(12))return;uint slot=dead[counters.Load(16)+i];if(slot>=capacity){fault();return;}
    uint2 ordinal=add64(sequence,uint2(i,0)),seed=emitterSeed^mix64(ordinal);
    float3 offset=0,velocity=modules[0].vector.xyz;
    if(shape==1) {
        float y=2*randomValue(seed,0)-1,r=sqrt(max(0.0,1-y*y)),a=randomValue(seed,1)*6.28318530718;
        offset=float3(r*cos(a),y,r*sin(a))*(pow(randomValue(seed,2),1.0/3.0)*radius);
    } else if(shape==3)offset=(float3(randomValue(seed,0),randomValue(seed,1),randomValue(seed,2))-0.5)*extent;
    else if(shape==4)offset=lineEnd*randomValue(seed,0);
    else if(shape==2) {
        float a=randomValue(seed,0)*6.28318530718,r=sqrt(randomValue(seed,1))*radius;offset=float3(cos(a)*r,0,sin(a)*r);
        float cosine=1-randomValue(seed,2)*(1-cos(angle)),sine=sqrt(max(0.0,1-cosine*cosine));
        float3 axis=unit(velocity),right=unit(cross(abs(axis.y)<0.9?float3(0,1,0):float3(1,0,0),axis)),up=cross(axis,right);
        velocity=(axis*cosine+(right*cos(a)+up*sin(a))*sine)*length(velocity);
    }
    float3 p=emitterPosition+offset+(localSpace?0:origin);
    // CPU uses double for this integer interval mapping. Float multiplication
    // can round a value just below an integer up, changing expiration by a tick.
    // Exact 24x19-bit multiplication preserves floor(bits*range/2^24).
    uint2 lifeProduct=product32(randomBits(seed,3),lifeMax-lifeMin+1);
    uint life=lifeMin+((lifeProduct.x>>24)|(lifeProduct.y<<8));
    float4 color=modules[0].color;float2 size=modules[0].sizeCoeff.xy;
    for(uint m=0;m<moduleCount;++m){Module module=modules[m];if(module.meta.x==3)color=curve(module,0);else if(module.meta.x==4)size=curve(module,0).xy;}
    attributes.Store3(positionOffset+slot*12,asuint(p));attributes.Store3(velocityOffset+slot*12,asuint(velocity));
    attributes.Store(ageOffset+slot*4,0);attributes.Store(lifetimeOffset+slot*4,life);attributes.Store(idOffset+slot*4,ordinal.x);
    attributes.Store4(colorOffset+slot*16,asuint(color));attributes.Store2(sizeOffset+slot*8,asuint(size));
    uint index;counters.InterlockedAdd(4,1,index);if(index<capacity)aliveOut[index]=slot;else fault();
}
[numthreads(1,1,1)]
void CSFinish(uint3 tid:SV_DispatchThreadID) {
    uint count=counters.Load(4);counters.Store(0,count);drawArgs.Store4(0,uint4(6,count,0,0));
}

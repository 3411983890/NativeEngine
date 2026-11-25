#if !defined(CLOTH_BATCH)
cbuffer ClothConstants:register(b0){
    uint vertexCount;uint triangleCount;uint hingeCount;uint batchStart;
    uint batchCount;uint flags;uint colliderCount;uint lambdaCount;
    float h;float damping;float dragCoefficient;float thickness;
    float stretchCompliance;float shearCompliance;float bendAngleCompliance;uint solverIterations;
    float3 origin;uint solverSubsteps;
    float3 translation;uint membraneColorCount;
    float3 uniformWind;uint adjacencyBase;
    uint hingeColorCount;uint colorScheduleBase;uint2 reserved2;
};
#endif
struct RestVertex{float4 positionWeight;float4 anchorMass;};
struct Vertex{float4 position;float4 previous;float4 velocity;float4 normal;float4 tangent;};
struct Membrane{uint4 ids;float4 inverse;float4 data;};
struct Hinge{uint4 ids;float4 data;};
struct Collider{float4 a;float4 b;};
#if defined(CLOTH_BATCH)
#include "cloth_batch_bindings.hlsli"
#else
StructuredBuffer<RestVertex> rest:register(t0);
StructuredBuffer<Membrane> membranes:register(t1);
StructuredBuffer<Hinge> hinges:register(t2);
StructuredBuffer<uint> schedule:register(t3);
StructuredBuffer<uint> adjacency:register(t4);
StructuredBuffer<Collider> colliders:register(t5);
RWStructuredBuffer<Vertex> vertices:register(u0);
RWStructuredBuffer<float> lambdas:register(u1);
RWByteAddressBuffer diagnostics:register(u2);
RWStructuredBuffer<float4> winds:register(u3);
RWStructuredBuffer<float4> windCandidate:register(u4);
RWStructuredBuffer<float4> aerodynamics:register(u5);
#endif
#if defined(CLOTH_FIELD_WIND)
cbuffer VectorFieldConstants:register(b1){
    #include "field_constants.hlsli"
    uint fieldTableDescriptor;uint3 fieldWind;
    uint3 fieldWater;uint fieldFilter;
    float3 fieldOrigin;float fieldInverseCell;
    float3 fieldWindScale;uint fieldFlags;
    float3 fieldWaterScale;uint fieldReserved;
};
#define FIELD_EXTERNAL_CONSTANTS
#define FIELD_PAGE_TABLE_DESCRIPTOR fieldTableDescriptor
#include "field_sampling.hlsli"
#endif
void countStat(uint word,uint amount){uint old=diagnostics.Load(word*4);while(true){uint value=old>0xffffffff-amount?0xffffffff:old+amount;uint seen;diagnostics.InterlockedCompareExchange(word*4,old,value,seen);if(seen==old)return;old=seen;}}
float3 windAt(uint id){return diagnostics.Load(36)!=0?winds[id].xyz:uniformWind;}
#if defined(CLOTH_GROUPED)
// 512 positions + velocities + 4608 multipliers = 30 KiB, within 32 KiB TGSM.
groupshared float groupX[512],groupY[512],groupZ[512],groupVX[512],groupVY[512],groupVZ[512];
groupshared float groupLambdas[4608];
// Bijection over [0,512): spread row-stride-32 vertical hinges across LDS banks.
uint groupSlot(uint id){return id^(id>>5);}
float3 clothPosition(uint id){uint slot=groupSlot(id);return float3(groupX[slot],groupY[slot],groupZ[slot]);}
void clothStorePosition(uint id,float3 p){uint slot=groupSlot(id);groupX[slot]=p.x;groupY[slot]=p.y;groupZ[slot]=p.z;}
float3 groupVelocity(uint id){uint slot=groupSlot(id);return float3(groupVX[slot],groupVY[slot],groupVZ[slot]);}
void groupStoreVelocity(uint id,float3 v){uint slot=groupSlot(id);groupVX[slot]=v.x;groupVY[slot]=v.y;groupVZ[slot]=v.z;}
float clothLambda(uint id){return groupLambdas[id];}
void clothStoreLambda(uint id,float lambda){groupLambdas[id]=lambda;}
#else
float3 clothPosition(uint id){return vertices[id].position.xyz;}
void clothStorePosition(uint id,float3 p){vertices[id].position.xyz=p;}
float clothLambda(uint id){return lambdas[id];}
void clothStoreLambda(uint id,float lambda){lambdas[id]=lambda;}
#endif
#include "cloth_constraints.hlsli"
// Both execution paths share pressure/integration arithmetic. Explicit precise
// results prevent different entry-point optimizations from contracting XPBD math.
#include "cloth_dynamics.hlsli"
#include "cloth_normals.hlsli"
[numthreads(64,1,1)]
void CSReset(uint3 tid:SV_DispatchThreadID){uint id=tid.x;if(id<lambdaCount)lambdas[id]=0;if(id<vertexCount){Vertex v=(Vertex)0;v.position=float4(rest[id].positionWeight.xyz+origin,1);v.previous=v.position;vertices[id]=v;winds[id]=0;windCandidate[id]=0;aerodynamics[id]=0;}if(id==0)for(uint i=0;i<16;++i)diagnostics.Store(i*4,0);}
[numthreads(64,1,1)]
void CSTeleport(uint3 tid:SV_DispatchThreadID){uint id=tid.x;if(id<vertexCount){Vertex v=vertices[id];v.position.xyz+=translation;v.previous.xyz+=translation;v.velocity=0;vertices[id]=v;winds[id]=0;windCandidate[id]=0;}if(id==0){diagnostics.Store(32,0);diagnostics.Store(36,0);}}
[numthreads(64,1,1)]
void CSResetLambda(uint3 tid:SV_DispatchThreadID){if(tid.x<lambdaCount)lambdas[tid.x]=0;}
[numthreads(1,1,1)]
void CSPrepareWind(uint3 tid:SV_DispatchThreadID){diagnostics.Store(32,0);if(flags&1)diagnostics.Store(36,0);}
#if defined(CLOTH_FIELD_WIND)
[numthreads(64,1,1)]
void CSSampleWind(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=vertexCount)return;countStat(2,1);float3 lattice=(vertices[id].position.xyz-fieldOrigin)*fieldInverseCell;bool known=false;float3 wind=0;
    if(all(isfinite(lattice))&&all(abs(lattice)<=8191)&&(fieldFlags&1)){
        int3 cell=(int3)floor(lattice);uint3 fraction=(uint3)floor((lattice-floor(lattice))*256);
        FieldAnswer a=sampleField(cell,fieldWind.x,fraction,fieldFilter),b=sampleField(cell,fieldWind.y,fraction,fieldFilter),c=sampleField(cell,fieldWind.z,fraction,fieldFilter);
        known=a.status==0&&b.status==0&&c.status==0;wind=float3(a.value,b.value,c.value)*fieldWindScale;known=known&&all(isfinite(wind))&&all(abs(wind)<=10000);
        if(a.status==7||b.status==7||c.status==7||(known&&(less64(maxAge,a.age)||less64(maxAge,b.age)||less64(maxAge,c.age))))countStat(6,1);
        if(known&&(a.clamped||b.clamped||c.clamped))countStat(7,1);
    }
    if(!known){countStat(3,1);uint old;diagnostics.InterlockedOr(32,1,old);}else windCandidate[id]=float4(wind,0);
}
#endif
[numthreads(64,1,1)]
void CSCommitWind(uint3 tid:SV_DispatchThreadID){uint id=tid.x;bool known=diagnostics.Load(32)==0;if(id<vertexCount&&known)winds[id]=windCandidate[id];if(id==0){if(known){diagnostics.Store(36,1);countStat(5,1);}else countStat(4,1);}}
[numthreads(64,1,1)]
void CSAerodynamics(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id<vertexCount)aerodynamics[id]=float4(clothAcceleration(id),0);
}
[numthreads(64,1,1)]
void CSIntegrate(uint3 tid:SV_DispatchThreadID){uint id=tid.x;if(id>=vertexCount)return;Vertex v=vertices[id];v.previous=v.position;v.velocity.xyz=clothIntegratedVelocity(id,v.velocity.xyz,aerodynamics[id].xyz);v.position.xyz=clothIntegratedPosition(id,v.position.xyz,v.velocity.xyz);vertices[id]=v;}
[numthreads(64,1,1)]
void CSMembrane(uint3 tid:SV_DispatchThreadID){
    if(tid.x<batchCount)solveClothMembrane(schedule[batchStart+tid.x]);
}
[numthreads(64,1,1)]
void CSHinge(uint3 tid:SV_DispatchThreadID){
    if(tid.x<batchCount)solveClothHinge(schedule[batchStart+tid.x]);
}
[numthreads(64,1,1)]
void CSContacts(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id<vertexCount)vertices[id].position.xyz=projectClothContacts(id,vertices[id].position.xyz);
}
[numthreads(64,1,1)]
void CSVelocity(uint3 tid:SV_DispatchThreadID){uint id=tid.x;if(id>=vertexCount)return;Vertex v=vertices[id];v.velocity=float4(clothProjectedVelocity(id,v.position.xyz,v.previous.xyz),0);if(any(!isfinite(v.position))||any(!isfinite(v.velocity))){countStat(0,1);v.position=float4(rest[id].positionWeight.xyz+origin,1);v.previous=v.position;v.velocity=0;}vertices[id]=v;}
[numthreads(64,1,1)]
void CSNormals(uint3 tid:SV_DispatchThreadID){updateClothNormal(tid.x);}
#if defined(CLOTH_GROUPED)
#include "cloth_grouped.hlsli"
#endif

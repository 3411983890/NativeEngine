cbuffer LocalConstants:register(b0){
    uint3 dimensions;uint voxels;
    int3 originFine;uint channelCount;
    uint finePerWorld;uint channelSlot;uint quantum;uint localSpace;
    float3 gridOrigin;float inverseFineCell;
    float3 particleOrigin;uint particleCapacity;
    uint positionOffset;uint channelMaximum;uint2 reserved;
    int3 coarseOrigin;uint coarseCount;
    uint3 coarseDimensions;uint reduction;
};
ByteAddressBuffer attributes:register(t0);
StructuredBuffer<uint> alive:register(t1);
ByteAddressBuffer particleCounters:register(t2);
RWByteAddressBuffer accumulators:register(u0);
RWByteAddressBuffer values:register(u1);
RWByteAddressBuffer diagnostics:register(u2);
RWByteAddressBuffer projected:register(u3);
RWStructuredBuffer<uint4> metricGroups:register(u4);
RWByteAddressBuffer metrics:register(u5);
uint2 lefAdd64(uint2 a,uint2 b){uint lo=a.x+b.x;return uint2(lo,a.y+b.y+(lo<a.x));}
uint2 lefSub64(uint2 a,uint2 b){return uint2(a.x-b.x,a.y-b.y-(a.x<b.x));}
uint2 lefShiftRight(uint2 a,uint n){return n==0?a:uint2((a.x>>n)|(a.y<<(32-n)),a.y>>n);}
uint2 lefShiftLeft(uint value,uint n){return n==0?uint2(value,0):uint2(value<<n,value>>(32-n));}
uint linearCell(uint3 p,uint3 size){return p.x+size.x*(p.y+size.y*p.z);}
uint3 cellFromIndex(uint id,uint3 size){return uint3(id%size.x,(id/size.x)%size.y,id/(size.x*size.y));}
uint threadIndex(uint3 tid){return tid.x+tid.y*(65535*64);}
void stat(uint word,uint amount){uint old=diagnostics.Load(word*4);while(true){bool overflow=old>0xffffffff-amount;uint value=overflow?0xffffffff:old+amount,seen;diagnostics.InterlockedCompareExchange(word*4,old,value,seen);if(seen==old){if(overflow){uint ignored;diagnostics.InterlockedOr(20,1,ignored);}return;}old=seen;}}
[numthreads(64,1,1)]
void CSClear(uint3 tid:SV_DispatchThreadID){uint id=threadIndex(tid);if(id<voxels*channelCount)accumulators.Store(id*4,0);if(id<32)diagnostics.Store(id*4,0);}
[numthreads(64,1,1)]
void CSParticleSplat(uint3 tid:SV_DispatchThreadID){
    uint id=threadIndex(tid),count=particleCounters.Load(0);if(id==0&&count>particleCapacity)stat(4,1);if(id>=min(count,particleCapacity))return;
    uint slot=alive[id];if(slot>=particleCapacity){stat(2,1);stat(24+channelSlot,quantum);return;}
    float3 p=asfloat(attributes.Load3(positionOffset+slot*12))+(localSpace!=0?particleOrigin:0);
    float3 lattice=(p-gridOrigin)*inverseFineCell;
    if(any(!isfinite(lattice))||any(abs(lattice)>1048576)){stat(2,1);stat(24+channelSlot,quantum);return;}
    int3 local=(int3)floor(lattice)-originFine;
    if(any(local<0)||any(local>=(int3)dimensions)){stat(1,1);stat(16+channelSlot,quantum);return;}
    uint address=(channelSlot*voxels+linearCell((uint3)local,dimensions))*4,old;
    // CPU admission proves the sum of all possible inputs fits uint32. There
    // is no per-add saturation and no shaderInt64 atomic requirement.
    accumulators.InterlockedAdd(address,quantum,old);stat(0,1);
}
[numthreads(64,1,1)]
void CSResolve(uint3 tid:SV_DispatchThreadID){uint id=threadIndex(tid);if(id>=voxels)return;uint value=accumulators.Load((channelSlot*voxels+id)*4);if(value>channelMaximum){stat(3,1);stat(8+channelSlot,value-channelMaximum);}values.Store((channelSlot*voxels+id)*4,min(value,channelMaximum));}
[numthreads(64,1,1)]
void CSImportScalar(uint3 tid:SV_DispatchThreadID){
    uint id=threadIndex(tid);if(id>=voxels)return;
    float value=asfloat(attributes.Load((positionOffset+id)*4));
    // CPU admission bounds maximum*quantum to 2^23-1. Invalid GPU data
    // contributes zero with an explicit count; it cannot overflow the cast.
    if(!isfinite(value)||value<0||value>asfloat(channelMaximum)){stat(2,1);return;}
    accumulators.Store(id*4,(uint)floor(value*(float)quantum+0.5));stat(0,1);
}
groupshared uint4 groupMetric[64];
void reduceMetric(uint lane){GroupMemoryBarrierWithGroupSync();for(uint stride=32;stride>0;stride>>=1){if(lane<stride){uint4 a=groupMetric[lane],b=groupMetric[lane+stride];groupMetric[lane]=uint4(lefAdd64(a.xy,b.xy),lefAdd64(a.zw,b.zw));}GroupMemoryBarrierWithGroupSync();}}
[numthreads(64,1,1)]
void CSProject(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    uint2 integral=0,reconstructed=0;uint id=threadIndex(tid);
    if(id<coarseCount){int3 start=(coarseOrigin+(int3)cellFromIndex(id,coarseDimensions))*(int)finePerWorld;uint maximum=0;
        for(uint z=0;z<finePerWorld;++z)for(uint y=0;y<finePerWorld;++y)for(uint x=0;x<finePerWorld;++x){int3 p=start+int3(x,y,z)-originFine;if(any(p<0)||any(p>=(int3)dimensions))continue;uint v=values.Load((channelSlot*voxels+linearCell((uint3)p,dimensions))*4);integral=lefAdd64(integral,uint2(v,0));maximum=max(maximum,v);}
        uint shift=firstbitlow(finePerWorld)*3;uint2 rounded=lefShiftRight(lefAdd64(integral,uint2((finePerWorld*finePerWorld*finePerWorld)/2,0)),shift);uint value=reduction==0?rounded.x:maximum;
        projected.Store(id*4,value);reconstructed=reduction==0?lefShiftLeft(value,shift):0;if(reduction!=0)integral=0;
    }
    groupMetric[lane]=uint4(integral,reconstructed);reduceMetric(lane);uint groupIndex=group.x+group.y*65535;if(lane==0&&groupIndex<(coarseCount+63)/64)metricGroups[groupIndex]=groupMetric[0];
}
[numthreads(64,1,1)]
void CSFinalize(uint lane:SV_GroupIndex){
    uint4 total=0;uint count=(coarseCount+63)/64;for(uint id=lane;id<count;id+=64){uint4 v=metricGroups[id];total=uint4(lefAdd64(total.xy,v.xy),lefAdd64(total.zw,v.zw));}
    groupMetric[lane]=total;reduceMetric(lane);if(lane==0){total=groupMetric[0];metrics.Store4(0,total);metrics.Store2(16,lefSub64(total.xy,total.zw));metrics.Store2(24,uint2(coarseCount,reduction));}
}

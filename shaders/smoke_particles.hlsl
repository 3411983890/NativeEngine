cbuffer SourceConstants:register(b0){
    uint3 dimensions;uint cells;uint3 offsets;uint scalarCount;
    float3 gridOrigin;float inverseCell;float3 particleOrigin;uint capacity;
    uint positionOffset;uint localSpace;uint densityQuantum;uint heatQuantum;
    uint fuelQuantum;uint3 pad0;float3 inverseRate;uint pad1;uint4 pad2;
};
ByteAddressBuffer attributes:register(t0);StructuredBuffer<uint> alive:register(t1);ByteAddressBuffer counters:register(t2);
RWByteAddressBuffer accumulators:register(u0);RWByteAddressBuffer rates:register(u1);RWByteAddressBuffer diagnostics:register(u2);
uint threadIndex(uint3 tid){return tid.x+tid.y*(65535*64);}
void count(uint word,uint value){uint old;diagnostics.InterlockedAdd(word*4,value,old);}
void loss(uint first,uint3 quantum){[unroll]for(uint a=0;a<3;a++)if(quantum[a])count(first+a,quantum[a]);}
[numthreads(64,1,1)]void CSClear(uint3 tid:SV_DispatchThreadID){uint i=threadIndex(tid);if(i<scalarCount)accumulators.Store(i*4,0);if(i<12)diagnostics.Store(i*4,0);}
[numthreads(64,1,1)]void CSSplat(uint3 tid:SV_DispatchThreadID){
    uint i=threadIndex(tid),n=counters.Load(0);if(i==0&&n>capacity)count(3,1);if(i>=min(n,capacity))return;uint3 quantum=uint3(densityQuantum,heatQuantum,fuelQuantum);uint slot=alive[i];if(slot>=capacity){count(2,1);loss(8,quantum);return;}
    float3 p=asfloat(attributes.Load3(positionOffset+slot*12))+(localSpace?particleOrigin:0);float3 cell=(p-gridOrigin)*inverseCell;if(any(!isfinite(cell))){count(2,1);loss(8,quantum);return;}if(any(cell<0)||any(cell>=float3(dimensions))){count(1,1);loss(4,quantum);return;}
    uint3 c=uint3(floor(cell));uint voxelIndex=(c.z*dimensions.y+c.y)*dimensions.x+c.x;[unroll]for(uint a=0;a<3;a++)if(quantum[a]&&offsets[a]!=0xffffffff){uint old;accumulators.InterlockedAdd((offsets[a]+voxelIndex)*4,quantum[a],old);}count(0,1);
}
[numthreads(64,1,1)]void CSResolve(uint3 tid:SV_DispatchThreadID){uint i=threadIndex(tid);if(i>=cells)return;[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff){uint address=(offsets[a]+i)*4;rates.Store(address,asuint(float(accumulators.Load(address))*inverseRate[a]));}}

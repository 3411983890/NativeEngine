#include "field_sampling.hlsli"
struct Query { int3 cell; uint channel; uint3 fraction; uint filter; };
StructuredBuffer<Query> queries : register(t1);
RWStructuredBuffer<FieldAnswer> answers : register(u0);
[numthreads(64,1,1)]
void CSFieldQuery(uint3 dispatch : SV_DispatchThreadID) {
    uint i=dispatch.x;if(i>=queryCount)return;
    Query q=queries[i];answers[i]=sampleField(q.cell,q.channel,q.fraction,q.filter);
}

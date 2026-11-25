// Shared indirect-work boundary. The legacy pool currently supplies its fixed
// configured capacity; a future alive-list pass will supply a GPU count instead.
// No CPU readback participates in generating or consuming these arguments.
cbuffer PassConstants : register(b0) {
    uint4 gIndices; // x raw argument UAV, w particle work count
    float4 gParams;
    float4 gBurst;
};
[numthreads(1, 1, 1)]
void CSBuildParticleDispatch(uint3 id : SV_DispatchThreadID) {
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[gIndices.x];
    arguments.Store3(0, uint3((gIndices.w + 63u) / 64u, 1u, 1u));
}

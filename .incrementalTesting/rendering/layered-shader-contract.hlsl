#include "../../shaders/material_layer_blend.hlsli"
RWStructuredBuffer<float4> result : register(u0);
[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 336) return;
    float coverage = (id.x % 7) / 6.0;
    float bottom = ((id.x / 7) % 4) * .25;
    float top = ((id.x / 28) % 4) * .25;
    float width = ((id.x / 112) + 1) * .2;
    float weight = LayerHeightCoverage(coverage, bottom, top, width, 0);
    result[id.x] = float4(LayerBlendNormal(float3(0, 0, 1), float3(0, 1, 0), coverage), weight);
}

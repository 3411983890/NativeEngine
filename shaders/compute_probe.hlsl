// Compute probe: one dispatch that proves two things at once.
//  1. B1: the renderer can run work on a real compute queue and read the result back.
//  2. B3: SM 6.6 ResourceDescriptorHeap indexing resolves descriptors by index, and the
//         result is identical to sampling the same descriptors through a descriptor table.
//
// The heap indices used here live above the bounded 8-slot material table on purpose: a
// bounded table cannot address them, so a match proves real heap indexing.
struct LightInput {
    float4 colorIntensity;
    float4 positionRange;
};

cbuffer ProbeConstants : register(b0) {
    uint heapBaseIndex;
    uint lightCount;
    uint corruptMode;
    uint padding;
    LightInput lights[4];
};

Texture2D<float4> table0 : register(t0);
Texture2D<float4> table1 : register(t1);
Texture2D<float4> table2 : register(t2);
Texture2D<float4> table3 : register(t3);
RWStructuredBuffer<float4> outResult : register(u0);

static const int2 kProbePixel = int2(4, 4);

float3 reduceLights() {
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint index = 0; index < lightCount; ++index) {
        sum += lights[index].colorIntensity.rgb * lights[index].colorIntensity.w;
    }
    return sum;
}

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
    // Row 0: the lighting reduction the CPU can compare against its own calculation.
    float3 lightSum = reduceLights();
    float scale = saturate(dot(lightSum, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
    if (corruptMode != 0) {
        scale = 0.5;
    }
    outResult[0] = float4(scale, lightSum);

    // Rows 1..8: table samples and heap samples of the same four textures.
    float4 tableSamples[4] = {
        table0.Load(int3(kProbePixel, 0)),
        table1.Load(int3(kProbePixel, 0)),
        table2.Load(int3(kProbePixel, 0)),
        table3.Load(int3(kProbePixel, 0))
    };
    // Untyped heap indexing must be assigned to a typed texture first.
    Texture2D<float4> heap0 = ResourceDescriptorHeap[heapBaseIndex + 0];
    Texture2D<float4> heap1 = ResourceDescriptorHeap[heapBaseIndex + 1];
    Texture2D<float4> heap2 = ResourceDescriptorHeap[heapBaseIndex + 2];
    Texture2D<float4> heap3 = ResourceDescriptorHeap[heapBaseIndex + 3];
    float4 heapSamples[4];
    heapSamples[0] = heap0.Load(int3(kProbePixel, 0));
    heapSamples[1] = heap1.Load(int3(kProbePixel, 0));
    heapSamples[2] = heap2.Load(int3(kProbePixel, 0));
    heapSamples[3] = heap3.Load(int3(kProbePixel, 0));
    for (uint sample = 0; sample < 4; ++sample) {
        outResult[1 + sample] = tableSamples[sample];
        outResult[5 + sample] = heapSamples[sample];
    }
}

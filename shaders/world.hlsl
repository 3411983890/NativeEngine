// World terrain and instanced component shading.
//
// One shader pair serves two draws:
//   * terrain chunks, where the model matrix is identity and the per-vertex colour comes
//     from the baked material weights;
//   * instanced components (grass, trees, rocks, buildings), where the model matrix comes
//     from the instance buffer.
//
// Keeping them in one pair is deliberate: a terrain chunk and the grass growing on it must
// receive the same key light, the same ambient and the same fog, or the seam between them
// is visible as a colour change rather than a silhouette.

cbuffer Frame : register(b0) {
    float4x4 gViewProjection;
    float4 gCameraTime;              // xyz camera, w seconds
    float4 gKeyDirectionIntensity;   // xyz direction to the light, w intensity
    float4 gKeyColorAmbient;         // rgb key colour, a ambient
    float4 gFogColorDensity;         // rgb fog colour, a density
    float4 gParams;                  // x useInstances, y alphaCutoff, z debugMode, w unused
    // Terrain vertices are stored chunk-local so an 8 km world keeps its float precision.
    // The instance path carries its own model matrix, so the origin only applies when
    // gParams.x is 0.
    float4 gChunkOrigin;
};

struct InstanceRecord {
    // Column-major, matching the engine's matrix convention: each float4 is one *column*.
    // The shader transposes once on load so the rest of the code can use mul(matrix, vector)
    // like every other shader here.
    float4 col0;
    float4 col1;
    float4 col2;
    float4 col3;
    float4 color;
};
StructuredBuffer<InstanceRecord> gInstances : register(t0);

struct VertexInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 color    : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world    : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float3 color    : TEXCOORD2;
};

VertexOutput VSMain(VertexInput input, uint instanceId : SV_InstanceID) {
    VertexOutput output;
    float4 local = float4(input.position, 1.0);
    float3 normal = input.normal;
    float3 color = input.color;
    if (gParams.x > 0.5) {
        InstanceRecord record = gInstances[instanceId];
        float4x4 model = transpose(float4x4(record.col0, record.col1, record.col2, record.col3));
        // Column-major matrices and column vectors: every other shader in this engine uses
        // mul(matrix, vector). Mixing the two conventions silently transposes the transform,
        // which produces a plausible-looking frame in the wrong place.
        local = mul(model, local);
        // Instances are uniformly scaled and yaw-rotated, so the upper 3x3 is a similarity
        // transform and the normal can be rotated without an inverse transpose.
        normal = normalize(mul(model, float4(normal, 0.0)).xyz);
        color *= record.color.rgb;
    }
    // Both paths are chunk-local so an 8 km world keeps its float precision; the chunk
    // origin is added once, here, after the instance transform.
    local.xyz += gChunkOrigin.xyz;
    output.world = local.xyz;
    output.normal = normal;
    output.color = color;
    output.position = mul(gViewProjection, local);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target {
    float3 normal = normalize(input.normal);
    float3 lightDirection = normalize(-gKeyDirectionIntensity.xyz);
    // Wrapped diffuse: a hard N·L term makes a low-poly cone read as a flat triangle fan.
    float lambert = saturate(dot(normal, lightDirection) * 0.5 + 0.5);
    float3 ambient = gKeyColorAmbient.rgb * gKeyColorAmbient.a;
    float3 lit = input.color * (ambient + gKeyColorAmbient.rgb * lambert * gKeyDirectionIntensity.w);

    // Debug views. 0 is shaded; the others exist so a seam or a missing chunk is obvious
    // instead of being blamed on the lighting.
    int debugMode = (int)gParams.z;
    if (debugMode == 1) {
        lit = input.color;
    } else if (debugMode == 2) {
        lit = normal * 0.5 + 0.5;
    } else if (debugMode == 3) {
        lit = frac(input.world * 0.125) * 0.5 + 0.25;
    }

    // Exponential height fog. The density is tiny; it exists to hide the far clip, not to
    // be a weather system.
    float distance = length(input.world - gCameraTime.xyz);
    float fog = 1.0 - exp(-distance * gFogColorDensity.a);
    lit = lerp(lit, gFogColorDensity.rgb, saturate(fog));
    return float4(lit, 1.0);
}

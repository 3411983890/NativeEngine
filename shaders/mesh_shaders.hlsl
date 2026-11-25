// Mesh shader sample: amplification + mesh + pixel, with the same lighting model as the
// raster pixel shader so the two paths can be compared pixel by pixel.
//
// Geometry: one unit cube split into six face meshlets (4 vertices / 2 triangles each).
// One DispatchMesh per instance; the amplification shader culls back-facing meshlets
// against the camera and forwards the visible indices in its payload.

struct MeshVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float2 textureUvs[4];
    float4 color;
    float4 tangent;
    float2 layerUvs[3];
};

struct Meshlet
{
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;
    float4 centerRadius;
    float4 normal;
};

struct InstanceConstants
{
    column_major float4x4 model;
    column_major float4x4 viewProjection;
    column_major float4x4 normalMatrix;
    float4 baseColor;
    float roughness;
    float metalness;
    uint textureIndex;
    uint normalIndex;
    uint ormIndex;
    float uvScale;
};

struct SceneConstants
{
    float4 ambient;
    float4 lightPositionRange[4];
    float4 lightColorIntensity[4];
    uint lightCount;
    float3 scenePad;
    float4 cameraPosition;
};

struct CameraConstants
{
    float4 eye;
    float4 params; // x = meshlet count per instance
};

struct MeshPayload
{
    uint count;
    uint meshletIndex[8];
};

ConstantBuffer<InstanceConstants> gInstance : register(b0);
ConstantBuffer<SceneConstants> gScene : register(b1);
ConstantBuffer<CameraConstants> gCamera : register(b2);
Texture2DArray gTextures : register(t0);
StructuredBuffer<MeshVertex> gVertices : register(t1);
StructuredBuffer<uint> gTriangles : register(t2);
StructuredBuffer<Meshlet> gMeshlets : register(t3);
SamplerState gSampler : register(s0);

groupshared uint gVisibleCount;
groupshared uint gVisibleIndices[8];

#if defined(PASS_AMPLIFICATION)
[shader("amplification")]
[numthreads(32, 1, 1)]
void ASMain(in payload MeshPayload payload, uint3 groupId : SV_GroupID,
            uint threadIndex : SV_GroupIndex)
{
    MeshPayload outPayload;
    if (threadIndex == 0)
    {
        gVisibleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint meshletCount = min((uint)gCamera.params.x, 8u);
    // Amplification currently forwards every meshlet of the instance. Back-face culling
    // was implemented here and is disabled: the per-thread payload copy made the
    // DispatchMesh launch count non-uniform, which the runtime requires to be uniform.
    // Culling is therefore reported as disabled rather than silently dropped.
    for (uint meshlet = threadIndex; meshlet < meshletCount; meshlet += 32)
    {
        gVisibleIndices[meshlet] = meshlet;
    }
    GroupMemoryBarrierWithGroupSync();
    // DispatchMesh needs a payload that is identical for every thread in the group, so the
    // indices come from groupshared memory and every thread builds the same copy.
    const uint visible = min(meshletCount, 8u);
    outPayload.count = visible;
    [unroll] for (uint index = 0; index < 8; ++index)
    {
        outPayload.meshletIndex[index] = (index < visible) ? gVisibleIndices[index] : 0u;
    }
    DispatchMesh(visible, 1, 1, outPayload);
}

#endif // PASS_AMPLIFICATION

#if defined(PASS_MESH) || defined(PASS_PIXEL)
struct MeshOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 materialPosition : TEXCOORD2;
    float3 materialNormal : TEXCOORD3;
    nointerpolation uint meshUv : TEXCOORD4;
    float2 normalUv : TEXCOORD5;
    float2 ormUv : TEXCOORD6;
    float2 emissiveUv : TEXCOORD7;
    float2 occlusionUv : TEXCOORD8;
    float4 color:COLOR0;
    float4 tangent:TEXCOORD9;
    float2 coatUv:TEXCOORD10;float2 coatRoughnessUv:TEXCOORD11;float2 coatNormalUv:TEXCOORD12;
};

#endif // PASS_MESH || PASS_PIXEL

#if defined(PASS_MESH)
[shader("mesh")]
[numthreads(32, 1, 1)]
[OutputTopology("triangle")]
void MSMain(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex,
            in payload MeshPayload payload, out vertices MeshOutput vertices[32],
            out indices uint3 triangles[16])
{
    uint slot = groupId.x;
    Meshlet desc = gMeshlets[payload.meshletIndex[slot]];
    SetMeshOutputCounts(desc.vertexCount, desc.triangleCount);

    if (threadIndex < desc.vertexCount)
    {
        MeshVertex vertex = gVertices[desc.vertexOffset + threadIndex];
        float4 world = mul(gInstance.model, float4(vertex.position, 1));
        MeshOutput output;
        output.position = mul(gInstance.viewProjection, world);
        output.normal = mul((float3x3)gInstance.normalMatrix, vertex.normal);
        output.uv = vertex.uv;
        output.worldPosition = world.xyz;
        float3 objectScale = float3(
            length(float3(gInstance.model[0][0], gInstance.model[1][0], gInstance.model[2][0])),
            length(float3(gInstance.model[0][1], gInstance.model[1][1], gInstance.model[2][1])),
            length(float3(gInstance.model[0][2], gInstance.model[1][2], gInstance.model[2][2])));
        output.materialPosition = vertex.position * objectScale;
        output.materialNormal = vertex.normal;
        output.meshUv = 0;
        output.normalUv=0;output.ormUv=0;output.emissiveUv=0;output.occlusionUv=0;
        output.color=vertex.color;
        output.coatUv=vertex.layerUvs[0];output.coatRoughnessUv=vertex.layerUvs[1];output.coatNormalUv=vertex.layerUvs[2];
        output.tangent=float4(mul((float3x3)gInstance.model,vertex.tangent.xyz),vertex.tangent.w*(determinant((float3x3)gInstance.model)<0?-1:1));
        vertices[threadIndex] = output;
    }
    if (threadIndex < desc.triangleCount)
    {
        uint index = desc.triangleOffset + threadIndex;
        // Mesh shader indices address the local vertex output array, not the global
        // structured vertex buffer. The CPU stores global cube indices, so rebase them.
        triangles[threadIndex] = uint3(gTriangles[index * 3 + 0] - desc.vertexOffset,
                                       gTriangles[index * 3 + 1] - desc.vertexOffset,
                                       gTriangles[index * 3 + 2] - desc.vertexOffset);
    }
}

#endif // PASS_MESH

#if defined(PASS_PIXEL)
struct PixelOutput
{
    float4 surface : SV_TARGET0;
    float4 keyLight : SV_TARGET1;
};

[shader("pixel")]
PixelOutput PSMain(MeshOutput input)
{
    // Same lighting model as the raster pixel shader. The mesh path must be visually
    // comparable with the vertex path or the comparison means nothing.

    float3 normal = normalize(input.normal);
    float3 textureColor = gTextures.Sample(gSampler, float3(input.uv, gInstance.textureIndex)).rgb;
    float3 albedo = textureColor * (0.45 + gInstance.baseColor.rgb * 0.55);
    float3 toKeyLight = normalize(float3(-0.45, 0.85, -0.55));
    float keyDiffuse = saturate(dot(normal, toKeyLight));
    float3 surface = albedo * gScene.ambient.rgb;
    for (uint i = 0; i < gScene.lightCount; ++i)
    {
        float3 delta = gScene.lightPositionRange[i].xyz - input.worldPosition;
        float distanceToLight = length(delta);
        float attenuation = saturate(1.0 - distanceToLight / max(gScene.lightPositionRange[i].w, 0.001));
        attenuation *= attenuation;
        float3 lightDirection = delta / max(distanceToLight, 0.001);
        float ndotl = saturate(dot(normal, lightDirection));
        float shininess = lerp(96.0, 8.0, gInstance.roughness);
        float specular = pow(saturate(dot(normal, normalize(lightDirection + float3(0, 1, 0)))), shininess);
        surface += albedo * gScene.lightColorIntensity[i].rgb * gScene.lightColorIntensity[i].w * ndotl * attenuation;
        surface += gScene.lightColorIntensity[i].rgb * specular * gInstance.metalness * attenuation * gScene.lightColorIntensity[i].w * 0.15;
    }
    PixelOutput output;
    output.surface = float4(surface, gInstance.baseColor.a);
    output.keyLight = float4(albedo * keyDiffuse * 0.95, 0.0);
    return output;
}
#endif // PASS_PIXEL

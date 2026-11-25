#include "d3d12_renderer_internal.h"

#include "batching.hpp"
#include "frustum.hpp"
#include "matrix_math.h"
#include "nativegpu_shaders.h"
#include "png_writer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nativegpu {

namespace {
const char* RasterShaderSource = R"hlsl(
cbuffer DrawConstants : register(b0) {
    column_major float4x4 model;
    column_major float4x4 viewProjection;
    column_major float4x4 normalMatrix;
    float4 baseColor;
    float roughness;
    float metalness;
    uint textureIndex;
    float pad0;
};
cbuffer SceneConstants : register(b1) {
    float4 ambient;
    float4 lightPositionRange[4];
    float4 lightColorIntensity[4];
    uint lightCount;
    float3 keyTint;    // keyColor * (keyIntensity / 1.30); see SceneConstants::keyTint
};
cbuffer SkinPalette : register(b2) { column_major float4x4 joints[256]; float4 morphWeights[4]; uint vertexCount; uint morphCount; uint2 skinPadding; };
struct SkinDelta { float3 position; float3 normal; };
struct MorphDelta { float3 position; float3 normal; float3 tangent; };
StructuredBuffer<MorphDelta> morphDeltas : register(t20);
StructuredBuffer<SkinDelta> softDeltas : register(t21);
Texture2DArray textures : register(t0);
SamplerState materialSampler : register(s0);
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PixelInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
};
struct PixelOutput {
    float4 surface : SV_TARGET0;   // ambient + scene point lights
    float4 keyLight : SV_TARGET1;  // key light only, multiplied by the DXR shadow mask later
};
PixelInput VSMain(VertexInput input) {
    PixelInput output;
    float4 world = mul(model, float4(input.position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)normalMatrix, input.normal);
    output.uv = input.uv;
    output.worldPosition = world.xyz;
    return output;
}
struct SkinInput {
    float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    uint4 indices : BLENDINDICES0; float4 weights : BLENDWEIGHT0;
};
PixelInput VSSkin(SkinInput input, uint vertexId : SV_VertexID) {
    [loop] for(uint m=0;m<morphCount;m++) {
        float w=morphWeights[m/4][m%4];
        if(w!=0) { MorphDelta d=morphDeltas[m*vertexCount+vertexId];input.position+=d.position*w;input.normal+=d.normal*w; }
    }
    float4 p=0; float3 n=0;
    [unroll] for(uint i=0;i<4;i++) {
        p += mul(joints[input.indices[i]],float4(input.position,1))*input.weights[i];
        n += mul((float3x3)joints[input.indices[i]],input.normal)*input.weights[i];
    }
    SkinDelta soft=softDeltas[vertexId];
    VertexInput vertex; vertex.position=p.xyz+soft.position; vertex.normal=normalize(normalize(n)+soft.normal); vertex.uv=input.uv;
    return VSMain(vertex);
}
PixelOutput PSMain(PixelInput input) {
    float3 normal = normalize(input.normal);
    float3 textureColor = textures.Sample(materialSampler, float3(input.uv, textureIndex)).rgb;
    // Entity color tints the procedural material instead of multiplying it away.
    float3 albedo = textureColor * (0.45 + baseColor.rgb * 0.55);
    float3 toKeyLight = normalize(float3(-0.45, 0.85, -0.55));
    float keyDiffuse = saturate(dot(normal, toKeyLight));
    float3 surface = albedo * ambient.rgb;
    for (uint i = 0; i < lightCount; ++i) {
        float3 delta = lightPositionRange[i].xyz - input.worldPosition;
        float distanceToLight = length(delta);
        float attenuation = saturate(1.0 - distanceToLight / max(lightPositionRange[i].w, 0.001));
        attenuation *= attenuation;
        float3 lightDirection = delta / max(distanceToLight, 0.001);
        float ndotl = saturate(dot(normal, lightDirection));
        float shininess = lerp(96.0, 8.0, roughness);
        float specular = pow(saturate(dot(normal, normalize(lightDirection + float3(0, 1, 0)))), shininess);
        surface += albedo * lightColorIntensity[i].rgb * lightColorIntensity[i].w * ndotl * attenuation;
        surface += lightColorIntensity[i].rgb * specular * metalness * attenuation * lightColorIntensity[i].w * 0.15;
    }
    PixelOutput output;
    output.surface = float4(surface, baseColor.a);
    // The key light carries most of the direct lighting so DXR shadows are clearly
    // visible; the raster fallback uses the identical expression with mask = 1.
    output.keyLight = float4(albedo * keyDiffuse * 0.95 * keyTint, 0.0);
    return output;
}
)hlsl";

// Cooked-material pixel shader. Geometry stays the unit cube in the legacy path; the
// material supplies three slices (albedo, tangent-space normal, ORM) and a world-space
// repeat size. UVs are planar per dominant world axis, so box buildings tile correctly
// without per-vertex UVs or tangent attributes.
const char* RasterShaderSourceTextured = R"hlsl(
cbuffer DrawConstants : register(b0) {
    column_major float4x4 model;
    column_major float4x4 viewProjection;
    column_major float4x4 normalMatrix;
    float4 baseColor;
    float roughness;
    float metalness;
    uint albedoIndex;
    uint normalIndex;
    uint ormIndex;
    float uvScale;
};
cbuffer SceneConstants : register(b1) {
    float4 ambient;
    float4 lightPositionRange[4];
    float4 lightColorIntensity[4];
    uint lightCount;
    float3 keyTint;    // keyColor * (keyIntensity / 1.30); see SceneConstants::keyTint
    float4 cameraPosition;
};
cbuffer SkinPalette : register(b2) { column_major float4x4 joints[256]; float4 morphWeights[4]; uint vertexCount; uint morphCount; uint2 skinPadding; };
struct SkinDelta { float3 position; float3 normal; };
struct MorphDelta { float3 position; float3 normal; float3 tangent; };
StructuredBuffer<MorphDelta> morphDeltas : register(t20);
StructuredBuffer<SkinDelta> softDeltas : register(t21);
Texture2DArray textures : register(t0);
SamplerState materialSampler : register(s0);

struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PixelInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
};
struct PixelOutput {
    float4 surface : SV_TARGET0;
    float4 keyLight : SV_TARGET1;
};

PixelInput VSMain(VertexInput input) {
    PixelInput output;
    float4 world = mul(model, float4(input.position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)normalMatrix, input.normal);
    output.uv = input.uv;
    output.worldPosition = world.xyz;
    return output;
}
struct SkinInput {
    float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    uint4 indices : BLENDINDICES0; float4 weights : BLENDWEIGHT0;
};
PixelInput VSSkin(SkinInput input, uint vertexId : SV_VertexID) {
    [loop] for(uint m=0;m<morphCount;m++) {
        float w=morphWeights[m/4][m%4];
        if(w!=0) { MorphDelta d=morphDeltas[m*vertexCount+vertexId];input.position+=d.position*w;input.normal+=d.normal*w; }
    }
    float4 p=0; float3 n=0;
    [unroll] for(uint i=0;i<4;i++) {
        p += mul(joints[input.indices[i]],float4(input.position,1))*input.weights[i];
        n += mul((float3x3)joints[input.indices[i]],input.normal)*input.weights[i];
    }
    SkinDelta soft=softDeltas[vertexId];
    VertexInput vertex; vertex.position=p.xyz+soft.position; vertex.normal=normalize(normalize(n)+soft.normal); vertex.uv=input.uv;
    return VSMain(vertex);
}

PixelOutput PSMain(PixelInput input) {
    float3 n = normalize(input.normal);
    float3 an = abs(n);
    float2 planar;
    float3 tangent;
    float3 bitangent;
    if (an.x >= an.y && an.x >= an.z) {
        planar = input.worldPosition.zy;
        tangent = float3(0, 1, 0);
        bitangent = float3(0, 0, 1);
    } else if (an.y >= an.z) {
        planar = input.worldPosition.xz;
        tangent = float3(1, 0, 0);
        bitangent = float3(0, 0, 1);
    } else {
        planar = input.worldPosition.xy;
        tangent = float3(1, 0, 0);
        bitangent = float3(0, 1, 0);
    }
    float2 uv = planar / max(uvScale, 0.001);
    float3 albedo = textures.Sample(materialSampler, float3(uv, albedoIndex)).rgb * baseColor.rgb;
    float3 sampledNormal = textures.Sample(materialSampler, float3(uv, normalIndex)).xyz * 2.0 - 1.0;
    sampledNormal.z = sqrt(saturate(1.0 - dot(sampledNormal.xy, sampledNormal.xy)));
    n = normalize(tangent * sampledNormal.x + bitangent * sampledNormal.y + n * sampledNormal.z);
    float4 orm = textures.Sample(materialSampler, float3(uv, ormIndex));
    float ao = saturate(orm.r);
    float rough = saturate(orm.g * roughness);
    float metal = saturate(orm.b * metalness);
    float emissive = orm.a;
    float3 specColor = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 view = normalize(cameraPosition.xyz - input.worldPosition);
    float shininess = lerp(160.0, 6.0, rough);

    float3 keyDirection = normalize(float3(-0.45, 0.85, -0.55));
    float keyDiffuse = saturate(dot(n, keyDirection));
    float keySpec = pow(saturate(dot(n, normalize(keyDirection + view))), shininess);
    // The legacy path keeps its fixed ambient look (image baselines depend on it). The
    // cooked path needs a daylight hemisphere plus a cool sky fill, otherwise a town of
    // vertical facades reads as a night scene under one overhead key.
    float3 hemisphere = lerp(float3(0.09, 0.11, 0.14), float3(0.34, 0.41, 0.52),
                            saturate(n.y * 0.5 + 0.5));
    float3 surface = albedo * (ambient.rgb * 0.40 + hemisphere * 0.15) * ao;
    surface += specColor * ambient.rgb * keySpec * ao * 0.18;
    // Emissive coverage rides in ORM.a, so lit windows and lamp heads do not need a
    // fourth texture or another root constant.
    surface += albedo * emissive * 1.5;
    for (uint i = 0; i < lightCount; ++i) {
        float3 delta = lightPositionRange[i].xyz - input.worldPosition;
        float distanceToLight = length(delta);
        float attenuation = saturate(1.0 - distanceToLight / max(lightPositionRange[i].w, 0.001));
        attenuation *= attenuation;
        float3 lightDirection = delta / max(distanceToLight, 0.001);
        float ndotl = saturate(dot(n, lightDirection));
        float specular = pow(saturate(dot(n, normalize(lightDirection + view))), shininess);
        float3 radiance = lightColorIntensity[i].rgb * lightColorIntensity[i].w * attenuation;
        surface += albedo * radiance * ndotl * (1.0 - metal * 0.45);
        surface += specColor * radiance * specular * (1.0 - rough) * 0.9;
    }
    PixelOutput output;
    // Distance haze ties the far end of the street to the sky instead of letting the
    // ground plane end in a hard line.
    float distanceToEye = length(cameraPosition.xyz - input.worldPosition);
    float fog = saturate((distanceToEye - 28.0) / 80.0) * 0.45;
    float3 fogColor = float3(0.22, 0.30, 0.40);
    surface = lerp(surface, fogColor, fog);
    float3 keyColor = (albedo * keyDiffuse * 1.30 + specColor * keySpec * 0.8) * ao * keyTint;
    output.surface = float4(surface, 1.0);
    output.keyLight = float4(lerp(keyColor, fogColor, fog), 1.0);
    return output;
}
)hlsl";

// The composite pass is deliberately compiled with D3DCompile (SM 5.1) from inline
// source: the base renderer must stay runnable when DXC is missing. DXC is only required
// for the ray tracing and mesh shader libraries, which D3DCompile cannot produce.
const char* CompositeShaderSource = R"hlsl(
struct CompositeVertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Texture2D<float4> baseColor : register(t0);
Texture2D<float4> keyLight : register(t1);
Texture2D<float> shadowVisibility : register(t2);
Texture2D<float> sceneDepth : register(t3);
struct TransparencyNode {float4 color;float depth;uint next;uint draw;uint primitive;};
Texture2D<uint> transparencyHeads : register(t4);
StructuredBuffer<TransparencyNode> transparencyNodes : register(t5);
bool TransparencyBefore(TransparencyNode a,TransparencyNode b){return a.depth<b.depth||(a.depth==b.depth&&(a.draw<b.draw||(a.draw==b.draw&&a.primitive<b.primitive)));}
bool TransparencyNext(uint head,float opaqueDepth,bool hasPrevious,TransparencyNode previous,out TransparencyNode result){
    bool found=false;result=(TransparencyNode)0;
    [loop]for(uint i=0;i<64&&head!=0xffffffffu;i++){TransparencyNode n=transparencyNodes[head];head=n.next;if(n.depth<=opaqueDepth&&(!hasPrevious||TransparencyBefore(previous,n))&&(!found||TransparencyBefore(n,result))){result=n;found=true;}}
    return found;
}
cbuffer CompositeConstants : register(b0) {
    uint debugView;
    uint toneMap;
    float exposure;
    uint volumetric;
};
struct CompositeLight {
    float4 positionRange;
    float4 colorIntensity;
    float4 directionCos;
    float4 params; // x = type (0 point, 1 spot), y = cosInner
};
cbuffer CompositeScene : register(b1) {
    column_major float4x4 invViewProjection;
    float4 cameraPosition;
    float4 fogParams;      // x density, y heightFalloff, z baseHeight, w maxDistance
    float4 fogColor;       // rgb color, w ambient scale
    float4 moonDirection;  // xyz direction to moon, w intensity
    float4 moonColor;
    float4 cloudParams;    // x time, y coverage, z height, w thickness
    CompositeLight fogLights[8];
    uint fogLightCount;
    float3 fogLightPadding;
};

float hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float noise3(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + float3(0, 0, 0));
    float n100 = hash13(i + float3(1, 0, 0));
    float n010 = hash13(i + float3(0, 1, 0));
    float n110 = hash13(i + float3(1, 1, 0));
    float n001 = hash13(i + float3(0, 0, 1));
    float n101 = hash13(i + float3(1, 0, 1));
    float n011 = hash13(i + float3(0, 1, 1));
    float n111 = hash13(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}
float fbm3(float3 p) {
    return 0.55 * noise3(p) + 0.30 * noise3(p * 2.03) + 0.15 * noise3(p * 4.01);
}

float3 SkyColor(float3 direction) {
    float up = saturate(direction.y);
    float3 color = lerp(float3(0.004, 0.007, 0.016), float3(0.018, 0.030, 0.060), up);
    // Stars: a sparse hash on the direction, faded near the horizon.
    float star = step(0.9975, hash13(floor(direction * 900.0)));
    color += star * smoothstep(0.08, 0.35, direction.y) * float3(0.65, 0.72, 0.9);
    // Moon disk.
    float moonDot = dot(direction, normalize(moonDirection.xyz));
    float moon = smoothstep(0.9994, 0.99985, moonDot);
    color += moonColor.rgb * moon * 6.0;
    color += moonColor.rgb * smoothstep(0.995, 0.9994, moonDot) * 0.05;

    if (direction.y > 0.02) {
        float t0 = (cloudParams.z - cameraPosition.y) / direction.y;
        float t1 = (cloudParams.z + cloudParams.w - cameraPosition.y) / direction.y;
        float tMax = min(t1, 450.0);
        if (tMax > t0) {
            const int steps = 14;
            float dt = (tMax - t0) / float(steps);
            float transmittance = 1.0;
            float3 scattered = 0.0;
            for (int i = 0; i < steps; ++i) {
                float3 p = cameraPosition.xyz + direction * (t0 + (float(i) + 0.5) * dt);
                float base = fbm3(p * 0.012 + float3(cloudParams.x * 0.01, 0, 0));
                float density = saturate((base - (1.0 - cloudParams.y)) * 3.0);
                float layer = saturate((p.y - cloudParams.z) / max(cloudParams.w, 1.0));
                density *= smoothstep(0.0, 0.2, layer) * smoothstep(1.0, 0.75, layer);
                float alpha = 1.0 - exp(-density * dt * 0.05);
                float3 lit = moonColor.rgb * 1.8 + float3(0.22, 0.24, 0.30);
                scattered += transmittance * alpha * lit;
                transmittance *= (1.0 - alpha);
            }
            color = color * transmittance + scattered;
        }
    }
    return color;
}

float3 ApplyFog(float3 color, float3 world, float3 direction) {
    float distance = length(world - cameraPosition.xyz);
    if (fogParams.w > 0.0) distance = min(distance, fogParams.w);
    const int steps = 12;
    float dt = distance / float(steps);
    float transmittance = 1.0;
    float3 scattered = 0.0;
    float3 moonDir = normalize(moonDirection.xyz);
    float phase = 0.5 + 0.5 * dot(direction, -moonDir);
    for (int i = 0; i < steps; ++i) {
        float3 p = cameraPosition.xyz + direction * ((float(i) + 0.5) * dt);
        float density = fogParams.x * exp(-max(0.0, p.y - fogParams.z) * fogParams.y);
        float stepTransmittance = exp(-density * dt);
        float3 radiance = fogColor.rgb * fogColor.w +
                          moonColor.rgb * moonDirection.w * phase * 0.35;
        // Headlights scatter in the fog: this is what turns a lit car into visible beams.
        uint lightCount = min(fogLightCount, 8u);
        for (uint l = 0; l < lightCount; ++l) {
            CompositeLight light = fogLights[l];
            float3 toLight = light.positionRange.xyz - p;
            float d = length(toLight);
            float range = min(max(light.positionRange.w, 0.001), 18.0);
            if (d >= range) continue;
            float attenuation = saturate(1.0 - d / range);
            attenuation *= attenuation;
            float3 lightToSurface = -toLight / max(d, 0.001);
            float cone = 1.0;
            if (light.params.x > 0.5) {
                float cosAngle = dot(lightToSurface, normalize(light.directionCos.xyz));
                float outer = light.directionCos.w;
                if (cosAngle < outer) continue;
                cone = saturate((cosAngle - outer) /
                                max(light.params.y - outer, 0.001));
            }
            float forward = pow(saturate(dot(lightToSurface,
                                              normalize(light.directionCos.xyz))), 6.0);
            float lightPhase = 0.15 + 0.85 * forward;
            radiance += light.colorIntensity.rgb * light.colorIntensity.w *
                        attenuation * cone * lightPhase * 0.04;
        }
        scattered += transmittance * (1.0 - stepTransmittance) * radiance;
        transmittance *= stepTransmittance;
    }
    return color * transmittance + scattered;
}

CompositeVertex VSMain(uint vertexId : SV_VertexID) {
    CompositeVertex output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
float4 PSMain(CompositeVertex input) : SV_Target {
    uint2 pixel = uint2(input.position.xy);
    float4 base = baseColor.Load(int3(pixel, 0));
    float4 key = keyLight.Load(int3(pixel, 0));
    float visibility = shadowVisibility.Load(int3(pixel, 0));
    // Diagnostic views used by the DXR shadow evidence: the mask itself, the isolated
    // key-light term, and the ambient surface term.
    if (debugView == 1) { return float4(visibility.xxx, 1.0); }
    if (debugView == 2) { return float4(key.rgb, 1.0); }
    if (debugView == 3) { return float4(base.rgb, 1.0); }
    if (debugView == 4) { return float4(1.0, 0.0, 1.0, 1.0); }
    float3 color = base.rgb + key.rgb * saturate(visibility);
    if ((volumetric&1u) != 0) {
        float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
        float4 nearWorld = mul(invViewProjection, float4(ndc, 0.0, 1.0));
        float4 farWorld = mul(invViewProjection, float4(ndc, 1.0, 1.0));
        nearWorld /= nearWorld.w;
        farWorld /= farWorld.w;
        float3 direction = normalize(farWorld.xyz - nearWorld.xyz);
        float depth = sceneDepth.Load(int3(pixel, 0));
        if (depth >= 1.0) {
            color = SkyColor(direction);
        } else {
            float4 world = mul(invViewProjection, float4(ndc, depth, 1.0));
            world /= world.w;
            color = ApplyFog(color, world.xyz, direction);
        }
    }
    if((volumetric&2u)!=0){
        if(toneMap==0&&(volumetric&1u)==0)color=base.rgb+key.rgb*saturate(visibility);
        uint head=transparencyHeads.Load(int3(pixel,0));float opaqueDepth=sceneDepth.Load(int3(pixel,0));
        float3 accumulated=0;float transmittance=1;bool hasPrevious=false;TransparencyNode previous=(TransparencyNode)0;
        [loop]for(uint layer=0;layer<64;layer++){
            TransparencyNode node;if(!TransparencyNext(head,opaqueDepth,hasPrevious,previous,node))break;previous=node;hasPrevious=true;
            float3 surface=node.color.rgb;
            if((volumetric&1u)!=0){float2 ndc=input.uv*float2(2,-2)+float2(-1,1);float4 world=mul(invViewProjection,float4(ndc,node.depth,1));world/=world.w;surface=ApplyFog(surface,world.xyz,normalize(world.xyz-cameraPosition.xyz));}
            accumulated+=surface*node.color.a*transmittance;transmittance*=1-node.color.a;if(transmittance==0)break;
        }
        color=accumulated+color*transmittance;
    }
    if (toneMap != 0) {
        // Cooked albedo is sRGB-decoded to linear; encode back to sRGB for display. A
        // plain gamma keeps saturation and shadow contrast, which ACES washed out.
        color = pow(saturate(color * exposure), 1.0 / 2.2);
    }
    // Forcing visibility to 1.0 reproduces the 0.3.0 raster image exactly.
    return float4(toneMap != 0 ? color : ((volumetric&3u)!=0?saturate(color):saturate(base.rgb + key.rgb * saturate(visibility))), base.a);
}
)hlsl";

const Vertex CubeVertices[] = {
    {-0.5f,-0.5f,-0.5f, 0,0,-1, 0,1}, {-0.5f, 0.5f,-0.5f, 0,0,-1, 0,0},
    { 0.5f, 0.5f,-0.5f, 0,0,-1, 1,0}, { 0.5f,-0.5f,-0.5f, 0,0,-1, 1,1},
    { 0.5f,-0.5f, 0.5f, 0,0, 1, 0,1}, { 0.5f, 0.5f, 0.5f, 0,0, 1, 0,0},
    {-0.5f, 0.5f, 0.5f, 0,0, 1, 1,0}, {-0.5f,-0.5f, 0.5f, 0,0, 1, 1,1},
    {-0.5f,-0.5f, 0.5f,-1,0, 0, 0,1}, {-0.5f, 0.5f, 0.5f,-1,0, 0, 0,0},
    {-0.5f, 0.5f,-0.5f,-1,0, 0, 1,0}, {-0.5f,-0.5f,-0.5f,-1,0, 0, 1,1},
    { 0.5f,-0.5f,-0.5f, 1,0, 0, 0,1}, { 0.5f, 0.5f,-0.5f, 1,0, 0, 0,0},
    { 0.5f, 0.5f, 0.5f, 1,0, 0, 1,0}, { 0.5f,-0.5f, 0.5f, 1,0, 0, 1,1},
    {-0.5f, 0.5f,-0.5f, 0,1, 0, 0,1}, {-0.5f, 0.5f, 0.5f, 0,1, 0, 0,0},
    { 0.5f, 0.5f, 0.5f, 0,1, 0, 1,0}, { 0.5f, 0.5f,-0.5f, 0,1, 0, 1,1},
    {-0.5f,-0.5f, 0.5f, 0,-1,0, 0,1}, {-0.5f,-0.5f,-0.5f, 0,-1,0, 0,0},
    { 0.5f,-0.5f,-0.5f, 0,-1,0, 1,0}, { 0.5f,-0.5f, 0.5f, 0,-1,0, 1,1}
};
const std::uint16_t CubeIndices[] = {
     0, 1, 2, 0, 2, 3,  4, 5, 6, 4, 6, 7,  8, 9,10, 8,10,11,
    12,13,14,12,14,15, 16,17,18,16,18,19, 20,21,22,20,22,23
};


Mat4 normalTransform(const Mat4& model) {
    const float a = model.m[0], b = model.m[4], c = model.m[8];
    const float d = model.m[1], e = model.m[5], f = model.m[9];
    const float g = model.m[2], h = model.m[6], i = model.m[10];
    const float determinant = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
    Mat4 result{};
    result.m[15] = 1.0f;
    if (std::abs(determinant) < 1.0e-12f) {
        result.m[0] = result.m[5] = result.m[10] = 1.0f;
        return result;
    }
    const float inv = 1.0f / determinant;
    result.m[0]=(e*i-f*h)*inv; result.m[4]=(f*g-d*i)*inv; result.m[8]=(d*h-e*g)*inv;
    result.m[1]=(c*h-b*i)*inv; result.m[5]=(a*i-c*g)*inv; result.m[9]=(b*g-a*h)*inv;
    result.m[2]=(b*f-c*e)*inv; result.m[6]=(c*d-a*f)*inv; result.m[10]=(a*e-b*d)*inv;
    return result;
}

bool compileEmbedded(const char* source, const char* entry, const char* target,
                     ID3DBlob** output, std::string& error) {
    ID3DBlob* log = nullptr;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT result = D3DCompile(source, std::strlen(source), "embedded_raster.hlsl", nullptr,
                                nullptr, entry, target, flags, 0, output, &log);
    if (FAILED(result)) {
        error = std::string("Compile ") + entry + " failed (" + detail::hresultText(result) + ")";
        if (log) {
            error += ": " + std::string(static_cast<char*>(log->GetBufferPointer()),
                                        log->GetBufferSize());
        }
    }
    detail::release(log);
    return SUCCEEDED(result);
}
} // namespace

namespace detail {

long g_liveObjects = 0;
unsigned long long g_objectsCreated = 0;

std::string hresultText(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return out.str();
}

bool checked(HRESULT hr, const char* action, std::string& error) {
    if (SUCCEEDED(hr)) return true;
    error = std::string(action) + " failed (" + hresultText(hr) + ")";
    return false;
}

std::string utf8(const wchar_t* input) {
    if (!input) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, &output[0], length, nullptr, nullptr);
    output.pop_back();
    return output;
}

std::string jsonString(const std::string& input) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : input) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 32) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch);
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    out << '"';
    return out.str();
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC bufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}

D3D12_RESOURCE_DESC textureDescription2D(unsigned width, unsigned height,
                                        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

std::wstring utf16(const char* input) {
    if (!input || !*input) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input, -1, &output[0], length);
    output.pop_back();
    return output;
}

} // namespace detail

using detail::checked;
using detail::jsonString;
using detail::release;
using detail::utf8;


D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Impl::cpuDescriptor(unsigned slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * srvStride;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::Impl::gpuDescriptor(unsigned slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * srvStride;
    return handle;
}

void Renderer::Impl::transitionTo(TrackedResource& tracked, D3D12_RESOURCE_STATES state,
                                  ID3D12GraphicsCommandList* list) {
    if (!tracked.resource || tracked.state == state) return;
    const D3D12_RESOURCE_BARRIER barrier =
        detail::transitionBarrier(tracked.resource, tracked.state, state);
    list->ResourceBarrier(1, &barrier);
    tracked.state = state;
}

bool Renderer::Impl::waitFor(UINT64 value, std::string& error) {
    if (!fence || !value) return true;
    UINT64 completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<UINT64>::max()) {
        error = "D3D12 device removed while waiting for the GPU (" +
                detail::hresultText(device->GetDeviceRemovedReason()) + ")";
        return false;
    }
    if (completed >= value) return true;
    if (!checked(fence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion", error)) {
        return false;
    }
    const DWORD result = WaitForSingleObject(fenceEvent, detail::FenceTimeoutMs);
    if (result != WAIT_OBJECT_0) {
        error = result == WAIT_TIMEOUT ? "GPU fence wait timed out after 5 seconds"
                                       : "GPU fence wait failed";
        return false;
    }
    completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<UINT64>::max() || completed < value) {
        error = "GPU fence did not complete; device status " +
                detail::hresultText(device->GetDeviceRemovedReason());
        return false;
    }
    return true;
}

bool Renderer::Impl::waitIdle(std::string& error) {
    if (!queue || !fence || !fenceEvent) return true;
    const UINT64 value = nextFence++;
    if (!checked(queue->Signal(fence, value), "Signal idle fence", error)) return false;
    return waitFor(value, error)&&collectTransparency(error);
}

bool Renderer::Impl::createBuffer(UINT64 size, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, ID3D12Resource** output,
                                  std::string& error) {
    return createBuffer(size, type, state, D3D12_RESOURCE_FLAG_NONE, output, error);
}

bool Renderer::Impl::createBuffer(UINT64 size, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                  ID3D12Resource** output, std::string& error) {
    auto heap = detail::heapProperties(type);
    auto description = detail::bufferDescription(size);
    description.Flags = flags;
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                 state, nullptr, IID_PPV_ARGS(output)),
                 "Create buffer", error)) {
        return false;
    }
    track(*output);
    return true;
}

bool Renderer::Impl::createTexture2D(unsigned textureWidth, unsigned textureHeight,
                                     DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags, const D3D12_CLEAR_VALUE* clear,
                                     TrackedResource& output, std::string& error) {
    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto description = detail::textureDescription2D(textureWidth, textureHeight, format, flags);
    ID3D12Resource* resource = nullptr;
    const std::string action = "Create render texture " + std::to_string(textureWidth) + "x" +
                               std::to_string(textureHeight) + " format " +
                               std::to_string(static_cast<int>(format)) + " flags " +
                               std::to_string(static_cast<unsigned>(flags)) +
                               " dim " + std::to_string(static_cast<int>(description.Dimension)) +
                               " mips " + std::to_string(description.MipLevels) +
                               " array " + std::to_string(description.DepthOrArraySize) +
                               " samples " + std::to_string(description.SampleDesc.Count) +
                               " quality " + std::to_string(description.SampleDesc.Quality) +
                               " layout " + std::to_string(static_cast<int>(description.Layout)) +
                               " state " + std::to_string(static_cast<unsigned>(state)) +
                               " clear " + (clear ? std::to_string(static_cast<int>(clear->Format))
                                                  : std::string("null"));
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                                 clear, IID_PPV_ARGS(&resource)),
                 action.c_str(), error)) {
        return false;
    }
    track(resource);
    output.resource = resource;
    output.state = state;
    return true;
}

bool Renderer::Impl::createDescriptorViews(std::string& error) {
    if(!shadowClearHeap){D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=detail::FrameCount;if(!detail::checked(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&shadowClearHeap)),"Create shadow clear descriptors",error))return false;track(shadowClearHeap);}
    srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        const unsigned base = detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
        depthView.Format = detail::DepthSrvFormat;
        depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthView.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(depth[index].resource, &depthView,
                                        cpuDescriptor(base + detail::FdsDepth));
        // Composite binds depth through its own table (the base/key/mask block is not
        // contiguous with FdsDepth).
        device->CreateShaderResourceView(depth[index].resource, &depthView,
                                        cpuDescriptor(base + detail::FdsDepthComposite));

        D3D12_SHADER_RESOURCE_VIEW_DESC baseView{};
        baseView.Format = detail::SceneColorFormat;
        baseView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        baseView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        baseView.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(sceneBaseColor[index].resource, &baseView,
                                        cpuDescriptor(base + detail::FdsBaseColor));

        D3D12_SHADER_RESOURCE_VIEW_DESC keyView = baseView;
        device->CreateShaderResourceView(sceneKeyLight[index].resource, &keyView,
                                        cpuDescriptor(base + detail::FdsKeyLight));

        D3D12_UNORDERED_ACCESS_VIEW_DESC maskUav{};
        maskUav.Format = detail::ShadowMaskFormat;
        maskUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(shadowMask[index].resource, nullptr, &maskUav,
                                         cpuDescriptor(base + detail::FdsShadowMaskUav));
        auto shadowCpu=shadowClearHeap->GetCPUDescriptorHandleForHeapStart();shadowCpu.ptr+=index*srvStride;device->CreateUnorderedAccessView(shadowMask[index].resource,nullptr,&maskUav,shadowCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC maskSrv{};
        maskSrv.Format = detail::ShadowMaskFormat;
        maskSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maskSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maskSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(shadowMask[index].resource, &maskSrv,
                                        cpuDescriptor(base + detail::FdsShadowMaskSrv));

        // A duplicate depth SRV placed next to the particle volume SRV, so the volumetric
        // pass can bind volume + depth through one contiguous descriptor table. Written
        // unconditionally: the view is harmless when particles are disabled.
        const unsigned particleBase =
            detail::ParticleFrameBase + index * detail::ParticleFrameStride;
        device->CreateShaderResourceView(depth[index].resource, &depthView,
                                        cpuDescriptor(particleBase + detail::PfsDepthSrv));
    }
    // Verification SRVs live above the frame blocks so the compute probe can sample heap
    // indices that the bounded 8-slot material table could never address.
    for (unsigned index = 0; index < detail::VerificationSrvCount; ++index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        const bool colorSource = index < 3;
        view.Format = colorSource ? detail::SceneColorFormat : detail::ShadowMaskFormat;
        ID3D12Resource* source = colorSource ? sceneBaseColor[index].resource
                                            : shadowMask[0].resource;
        device->CreateShaderResourceView(source, &view,
                                        cpuDescriptor(detail::VerificationSrvBase + index));
    }
    transparencyViews();
    return true;
}

bool Renderer::Impl::createTargets(std::string& error) {
    auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!checked(swapChain->GetBuffer(index, IID_PPV_ARGS(&renderTargets[index])),
                     "Get swap chain buffer", error)) {
            return false;
        }
        track(renderTargets[index]);
        device->CreateRenderTargetView(renderTargets[index], nullptr, rtvHandle);
        rtvHandle.ptr += rtvStride;
    }

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = detail::DepthDsvFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = detail::DepthDsvFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    auto dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!createTexture2D(width, height, detail::DepthResourceFormat,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &depthClear,
                             depth[index], error) ||
            !createTexture2D(width, height, detail::SceneColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, sceneBaseColor[index], error) ||
            !createTexture2D(width, height, detail::SceneColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, sceneKeyLight[index], error) ||
            !createTexture2D(width, height, detail::ShadowMaskFormat,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nullptr,
                             shadowMask[index], error) ||
            !createTexture2D(width, height, detail::ColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, composite[index], error)) {
            return false;
        }
        device->CreateDepthStencilView(depth[index].resource, &dsvDesc, dsvHandle);
        dsvHandle.ptr += dsvStride;

        D3D12_CPU_DESCRIPTOR_HANDLE targetHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        targetHandle.ptr +=
            static_cast<SIZE_T>(detail::RtvSlotBase + index * detail::RtvSlotsPerFrame) * rtvStride;
        device->CreateRenderTargetView(sceneBaseColor[index].resource, nullptr, targetHandle);
        targetHandle.ptr += rtvStride;
        device->CreateRenderTargetView(sceneKeyLight[index].resource, nullptr, targetHandle);
        targetHandle.ptr += rtvStride;
        device->CreateRenderTargetView(composite[index].resource, nullptr, targetHandle);
    }
    rtShadowTextureBytes = static_cast<UINT64>(width) * height * 2u * detail::FrameCount;
    return createDescriptorViews(error);
}

void Renderer::Impl::releaseTargets() {
    for (auto& resource : renderTargets) destroy(resource);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        destroy(depth[index].resource);
        depth[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(sceneBaseColor[index].resource);
        sceneBaseColor[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(sceneKeyLight[index].resource);
        sceneKeyLight[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(shadowMask[index].resource);
        shadowMask[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(composite[index].resource);
        composite[index].state = D3D12_RESOURCE_STATE_COMMON;
    }
}

namespace {

bool serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& description,
                            ID3D12Device* device, ID3D12RootSignature** output,
                            std::string& error) {
    ID3DBlob* signature = nullptr;
    ID3DBlob* diagnostics = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &signature, &diagnostics);
    if (FAILED(hr)) {
        error = "Serialize root signature failed (" + detail::hresultText(hr) + ")";
        if (diagnostics) {
            error += ": " + std::string(static_cast<char*>(diagnostics->GetBufferPointer()),
                                        diagnostics->GetBufferSize());
        }
        detail::release(diagnostics);
        detail::release(signature);
        return false;
    }
    detail::release(diagnostics);
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                     IID_PPV_ARGS(output));
    detail::release(signature);
    return detail::checked(hr, "Create root signature", error);
}

} // namespace

bool Renderer::Impl::createRasterRootSignature(std::string& error) {
    // t0 is the legacy material array; t4 is the cooked material record buffer. The table
    // is bound at SlotMaterialTable, so the record descriptor sits at offset 4.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = detail::SlotMaterialRecords;
    D3D12_DESCRIPTOR_RANGE deformationRange{};
    deformationRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    deformationRange.NumDescriptors=2;deformationRange.BaseShaderRegister=20;
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 58;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].Descriptor.RegisterSpace = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 2;
    parameters[2].DescriptorTable.pDescriptorRanges = ranges;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 2;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[4].DescriptorTable.NumDescriptorRanges=1;
    parameters[4].DescriptorTable.pDescriptorRanges=&deformationRange;
    parameters[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MinLOD = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 8;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 5;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
    if (!serializeRootSignature(description, device, &rasterRootSignature, error)) return false;
    track(rasterRootSignature);
    return true;
}

bool Renderer::Impl::createCompositeRootSignature(std::string& error) {
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (unsigned index = 0; index < 3; ++index) {
        ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[index].NumDescriptors = 1;
        ranges[index].BaseShaderRegister = index; // t0 base, t1 key, t2 mask
        ranges[index].OffsetInDescriptorsFromTableStart = index;
    }
    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors = 3;
    depthRange.BaseShaderRegister = 3;
    depthRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE smokeRange{};smokeRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;smokeRange.NumDescriptors=detail::MaxSmokeMedia;smokeRange.BaseShaderRegister=6;
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 3;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 1;
    parameters[2].Descriptor.RegisterSpace = 0;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &depthRange;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[4].DescriptorTable.NumDescriptorRanges=1;parameters[4].DescriptorTable.pDescriptorRanges=&smokeRange;parameters[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    description.NumParameters = 5;
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(description, device, &compositeRootSignature, error)) return false;
    track(compositeRootSignature);
    return true;
}

bool Renderer::Impl::createRasterPipeline(std::string& error) {
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    if (!compileEmbedded(RasterShaderSource, "VSMain", "vs_5_1", &vertexShader, error) ||
        !compileEmbedded(RasterShaderSource, "PSMain", "ps_5_1", &pixelShader, error)) {
        detail::release(vertexShader);
        detail::release(pixelShader);
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rasterRootSignature;
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    description.InputLayout = {elements, 3};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 2;
    description.RTVFormats[0] = detail::SceneColorFormat;
    description.RTVFormats[1] = detail::SceneColorFormat;
    description.DSVFormat = detail::DepthDsvFormat;
    description.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&rasterPipeline));
    detail::release(vertexShader);
    if (FAILED(hr)) {detail::release(pixelShader);return detail::checked(hr, "Create raster pipeline", error);}
    track(rasterPipeline);
    if(!compileEmbedded(RasterShaderSource,"VSSkin","vs_5_1",&vertexShader,error)){detail::release(pixelShader);return false;}
    D3D12_INPUT_ELEMENT_DESC skinElements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_UINT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };
    description.InputLayout={skinElements,5};
    description.VS={vertexShader->GetBufferPointer(),vertexShader->GetBufferSize()};
    // The LH view and D3D viewport use the same screen winding as our rigid path.
    description.RasterizerState.FrontCounterClockwise=FALSE;
    hr=device->CreateGraphicsPipelineState(&description,IID_PPV_ARGS(&skinPipeline));
    detail::release(vertexShader);detail::release(pixelShader);
    if(FAILED(hr))return detail::checked(hr,"Create skin pipeline",error);
    track(skinPipeline);
    return true;
}

bool Renderer::Impl::createTexturedPipeline(std::string& error) {
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    if (!compileEmbedded(RasterShaderSourceTextured, "VSMain", "vs_5_1", &vertexShader, error) ||
        !compileEmbedded(RasterShaderSourceTextured, "PSMain", "ps_5_1", &pixelShader, error)) {
        detail::release(vertexShader);
        detail::release(pixelShader);
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rasterRootSignature;
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    description.InputLayout = {elements, 3};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 2;
    description.RTVFormats[0] = detail::SceneColorFormat;
    description.RTVFormats[1] = detail::SceneColorFormat;
    description.DSVFormat = detail::DepthDsvFormat;
    description.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&texturedPipeline));
    detail::release(vertexShader);
    detail::release(pixelShader);
    if (FAILED(hr)) return detail::checked(hr, "Create textured pipeline", error);
    track(texturedPipeline);
    return true;
}

bool Renderer::Impl::createBindlessPipeline(std::string& error,bool inlineOnly) {
    if(!inlineOnly)materialBindless = false;
    auto& staticPipeline=inlineOnly?inlineEnvironmentPipeline:bindlessPipeline;
    auto& skinnedPipeline=inlineOnly?inlineEnvironmentSkinPipeline:bindlessSkinPipeline;
    if (!shaders::kMaterialVS.size || !shaders::kMaterialPS.size) {
        error = "cooked material shader blob is unavailable (DXC build missing)";
        return false;
    }
    auto makePipeline = [&](const D3D12_INPUT_ELEMENT_DESC* elements, UINT count,
                            ID3D12PipelineState** output, bool skinned, bool transparent=false) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rasterRootSignature;
        description.VS = skinned ? D3D12_SHADER_BYTECODE{shaders::kMaterialSkinVS.data,shaders::kMaterialSkinVS.size} : D3D12_SHADER_BYTECODE{shaders::kMaterialVS.data, shaders::kMaterialVS.size};
        description.PS = inlineOnly?D3D12_SHADER_BYTECODE{shaders::kMaterialInlineEnvironmentPS.data,shaders::kMaterialInlineEnvironmentPS.size}:transparent?D3D12_SHADER_BYTECODE{shaders::kMaterialCapturePS.data,shaders::kMaterialCapturePS.size}:D3D12_SHADER_BYTECODE{shaders::kMaterialPS.data, shaders::kMaterialPS.size};
        auto& blend = description.BlendState.RenderTarget[0];
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ZERO;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.SampleMask = UINT_MAX;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        description.RasterizerState.FrontCounterClockwise = FALSE;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        description.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        description.DepthStencilState.DepthEnable = TRUE;
        description.DepthStencilState.DepthWriteMask = transparent?D3D12_DEPTH_WRITE_MASK_ZERO:D3D12_DEPTH_WRITE_MASK_ALL;
        description.DepthStencilState.DepthFunc = transparent?D3D12_COMPARISON_FUNC_LESS_EQUAL:D3D12_COMPARISON_FUNC_LESS;
        description.DepthStencilState.StencilEnable = FALSE;
        description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
        description.InputLayout = {elements, count};
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = transparent?0:2;
        description.RTVFormats[0] = transparent?DXGI_FORMAT_UNKNOWN:detail::SceneColorFormat;
        description.RTVFormats[1] = transparent?DXGI_FORMAT_UNKNOWN:detail::SceneColorFormat;
        description.DSVFormat = detail::DepthDsvFormat;
        description.SampleDesc.Count = 1;
        HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(output));
        if (FAILED(hr)) error = "CreateGraphicsPipelineState failed (" + detail::hresultText(hr) + ")";
        return SUCCEEDED(hr);
    };
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32_FLOAT,0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",3,DXGI_FORMAT_R32G32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",4,DXGI_FORMAT_R32G32_FLOAT,0,56,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",5,DXGI_FORMAT_R32G32_FLOAT,0,96,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",6,DXGI_FORMAT_R32G32_FLOAT,0,104,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",7,DXGI_FORMAT_R32G32_FLOAT,0,112,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };
    constexpr UINT elementCount=sizeof(elements)/sizeof(elements[0]);
    if (!makePipeline(elements, elementCount, &staticPipeline, false) || !staticPipeline) {
        if (error.empty()) error = "Create bindless material pipeline failed (SM 6.6 or DXC blob rejected)";
        return false;
    }
    track(staticPipeline);
    D3D12_INPUT_ELEMENT_DESC skinElements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_UINT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,64,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32_FLOAT,0,72,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",3,DXGI_FORMAT_R32G32_FLOAT,0,80,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",4,DXGI_FORMAT_R32G32_FLOAT,0,88,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,112,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",5,DXGI_FORMAT_R32G32_FLOAT,0,128,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",6,DXGI_FORMAT_R32G32_FLOAT,0,136,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",7,DXGI_FORMAT_R32G32_FLOAT,0,144,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };
    constexpr UINT skinElementCount=sizeof(skinElements)/sizeof(skinElements[0]);
    if (!makePipeline(skinElements, skinElementCount, &skinnedPipeline, true) || !skinnedPipeline) {
        destroy(staticPipeline);
        error = "Create bindless skin pipeline failed";
        return false;
    }
    track(skinnedPipeline);
    if(inlineOnly)return true;
    if(!makePipeline(elements,elementCount,&transparentPipeline,false,true)||!makePipeline(skinElements,skinElementCount,&transparentSkinPipeline,true,true))return false;
    track(transparentPipeline);track(transparentSkinPipeline);
    materialBindless = true;
    return true;
}

bool Renderer::Impl::prepareSkin(unsigned index,const DrawItem& item,std::string& error) {
    using namespace engine::animation;
    if(index>=detail::SkinBindingCount||!item.skin||!item.palette||item.palette->size()!=item.skin->skeleton.bones.size()||item.palette->empty()||item.palette->size()>MaxBones){error="NG-E-ANIM-SKIN: missing palette";return false;}
    if(item.skin->vertices.empty()||item.skin->indices.empty()){error="NG-E-ANIM-SKIN: empty geometry";return false;}
    constexpr std::size_t budget=256ull*1024*1024;
    static_assert(sizeof(SkinVertex)==152,"Skin vertex layout changed");
    if((item.skin->hasLayerUvs||item.skin->hasTextureUvs||item.skin->hasVertexColors||item.skin->hasTangents)&&!materialBindless&&!inlineEnvironmentSkinPipeline){error="NG-E-MAT-VERTEX: multiple texture UVs, vertex colors and authored tangents require the SM 6.6 material pipeline";return false;}
    auto upload=[&](ID3D12Resource*& resource,const void* data,UINT64 bytes,bool allocate)->bool{
        if(allocate){destroy(resource);auto heap=detail::heapProperties(D3D12_HEAP_TYPE_UPLOAD);auto desc=detail::bufferDescription(bytes);
            if(!detail::checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&resource)),"Allocate skin upload",error))return false;track(resource);}
        void* mapped=nullptr;D3D12_RANGE noRead{0,0};if(!detail::checked(resource->Map(0,&noRead,&mapped),"Map skin upload",error))return false;
        std::memcpy(mapped,data,static_cast<size_t>(bytes));resource->Unmap(0,nullptr);return true;
    };
    struct GpuMorphDelta { V3 position,normal,tangent; };static_assert(sizeof(GpuMorphDelta)==36,"Morph GPU layout changed");
    auto key=item.skin->geometryHash.empty()?item.skin->contentHash:item.skin->geometryHash;
    if(key.empty())key="memory:"+std::to_string(reinterpret_cast<std::uintptr_t>(item.skin));
    auto found=std::find_if(skinGeometry.begin(),skinGeometry.end(),[&](const SkinGeometry& geometry){return geometry.key==key&&geometry.vertexCount==item.skin->vertices.size()&&geometry.indexCount==item.skin->indices.size()&&geometry.morphCount==item.skin->morphs.size();});
    if(found==skinGeometry.end()){
        const auto geometryBytes=item.skin->vertices.size()*(sizeof(SkinVertex)+sizeof(VertexDelta))+item.skin->indices.size()*4+std::max<std::size_t>(1,item.skin->vertices.size()*item.skin->morphs.size())*sizeof(GpuMorphDelta);
        if(skinGeometry.size()>=128||geometryBytes>budget-skinMemoryBytes){error="NG-E-ANIM-BUDGET: skin upload cache exceeds 256 MiB or 128 geometries";return false;}
        SkinGeometry geometry;geometry.key=key;geometry.vertexCount=static_cast<unsigned>(item.skin->vertices.size());geometry.indexCount=static_cast<unsigned>(item.skin->indices.size());geometry.morphCount=static_cast<unsigned>(item.skin->morphs.size());
        std::vector<GpuMorphDelta> morphData(std::max<size_t>(1,item.skin->vertices.size()*item.skin->morphs.size()));
        for(unsigned m=0;m<item.skin->morphs.size();++m)for(const auto& d:item.skin->morphs[m].deltas)morphData[m*item.skin->vertices.size()+d.vertex]={d.position,d.normal,d.tangent};
        const std::vector<VertexDelta> zero(item.skin->vertices.size());
        if(!upload(geometry.vertices,item.skin->vertices.data(),item.skin->vertices.size()*sizeof(SkinVertex),true)||
           !upload(geometry.indices,item.skin->indices.data(),item.skin->indices.size()*4,true)||
           !upload(geometry.morphs,morphData.data(),morphData.size()*sizeof(GpuMorphDelta),true)||
           !upload(geometry.zeroDeformation,zero.data(),zero.size()*sizeof(VertexDelta),true)) {
            destroy(geometry.vertices);destroy(geometry.indices);destroy(geometry.morphs);destroy(geometry.zeroDeformation);return false;
        }
        skinGeometry.push_back(geometry);found=skinGeometry.end()-1;skinMemoryBytes+=geometryBytes;skinGeometryBytes+=geometryBytes;++skinGeometryUploads;
    }
    skinGeometrySlot[index]=static_cast<unsigned>(found-skinGeometry.begin());
    auto srv=[&](ID3D12Resource* resource,unsigned count,unsigned slot,unsigned stride=sizeof(VertexDelta)){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Buffer.NumElements=count;view.Buffer.StructureByteStride=stride;device->CreateShaderResourceView(resource,&view,cpuDescriptor(slot));};
    srv(found->morphs,std::max(1u,found->vertexCount*found->morphCount),detail::SkinDeformSrvBase+index*2,sizeof(GpuMorphDelta));
    struct SkinConstants{std::array<Matrix,MaxBones> joints{};std::array<std::array<float,4>,4> weights{};unsigned vertexCount=0,morphCount=0,pad[2]{};};
    SkinConstants constants;std::copy(item.palette->begin(),item.palette->end(),constants.joints.begin());
    constants.vertexCount=static_cast<unsigned>(item.skin->vertices.size());constants.morphCount=static_cast<unsigned>(item.skin->morphs.size());
    if(item.morphWeights)for(unsigned i=0;i<MaxMorphTargets;++i)constants.weights[i/4][i%4]=(*item.morphWeights)[i];
    if(item.deformation){
        if(item.deformation->size()!=item.skin->vertices.size()){error="NG-E-ANIM-SKIN: deformation size mismatch";return false;}
        const auto bytes=item.deformation->size()*sizeof(VertexDelta),oldBytes=skinDeformationBytes[index];const bool allocate=!skinDeformation[index]||oldBytes!=bytes;
        if(bytes>budget-(skinMemoryBytes-oldBytes)){error="NG-E-ANIM-BUDGET: skin deformation exceeds 256 MiB";return false;}
        if(!upload(skinDeformation[index],item.deformation->data(),bytes,allocate))return false;
        if(allocate){skinMemoryBytes=skinMemoryBytes-oldBytes+bytes;skinDeformationBytes[index]=bytes;}
        srv(skinDeformation[index],found->vertexCount,detail::SkinDeformSrvBase+index*2+1);
    }else srv(found->zeroDeformation,found->vertexCount,detail::SkinDeformSrvBase+index*2+1);
    // CBV allocation is rounded to 256 bytes, with zero initialized tail bytes.
    std::array<unsigned char,((sizeof(SkinConstants)+255)/256)*256> bytes{};std::memcpy(bytes.data(),&constants,sizeof(constants));
    if(!skinPalette[index]&&bytes.size()>budget-skinMemoryBytes){error="NG-E-ANIM-BUDGET: skin palette exceeds 256 MiB";return false;}
    const bool allocate=!skinPalette[index];if(!upload(skinPalette[index],bytes.data(),bytes.size(),allocate))return false;
    if(allocate)skinMemoryBytes+=bytes.size();return true;
}

bool Renderer::Impl::createCompositePipeline(std::string& error) {
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    compositeMediaActive=false;D3D12_FEATURE_DATA_SHADER_MODEL mediaModel{D3D_SHADER_MODEL_6_6};
    const bool mediaShader=shaders::kDxcAvailable&&SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&mediaModel,sizeof(mediaModel)))&&mediaModel.HighestShaderModel>=D3D_SHADER_MODEL_6_6;
    if (!mediaShader && (!compileEmbedded(CompositeShaderSource, "VSMain", "vs_5_1", &vertexShader, error) ||
        !compileEmbedded(CompositeShaderSource, "PSMain", "ps_5_1", &pixelShader, error))) {
        detail::release(vertexShader);
        detail::release(pixelShader);
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = compositeRootSignature;
    description.VS = mediaShader?D3D12_SHADER_BYTECODE{shaders::kCompositeMediaVS.data,shaders::kCompositeMediaVS.size}:D3D12_SHADER_BYTECODE{vertexShader->GetBufferPointer(),vertexShader->GetBufferSize()};
    description.PS = mediaShader?D3D12_SHADER_BYTECODE{shaders::kCompositeMediaPS.data,shaders::kCompositeMediaPS.size}:D3D12_SHADER_BYTECODE{pixelShader->GetBufferPointer(),pixelShader->GetBufferSize()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = detail::ColorFormat;
    description.DSVFormat = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&compositePipeline));
    detail::release(vertexShader);
    detail::release(pixelShader);
    if (FAILED(hr)) return detail::checked(hr, "Create composite pipeline", error);
    compositeMediaActive=mediaShader;
    track(compositePipeline);
    return true;
}

bool Renderer::Impl::uploadTextureSet(const std::vector<TextureUpload>& configuredTextures, unsigned srvBase,
                                     std::vector<ID3D12Resource*>& cookedTextures, std::string& error) {
    cookedTextures.clear();
    cookedTextures.resize(configuredTextures.size(), nullptr);
    struct PendingUpload {
        ID3D12Resource* upload = nullptr;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        unsigned mips = 0;
        unsigned width = 0;
        unsigned height = 0;
        unsigned format = 0;
    };
    std::vector<PendingUpload> pending;
    pending.resize(configuredTextures.size());
    auto releasePending = [&] {
        for (PendingUpload& item : pending) destroy(item.upload);
    };
    auto dxgiFormat = [](unsigned format, bool srgb) -> DXGI_FORMAT {
        switch (format) {
        case 1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        case 2: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
        case 3: return DXGI_FORMAT_BC5_UNORM;
        case 4: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        case 5: return DXGI_FORMAT_R16_UNORM;
        default: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    };
    auto blockBytes = [](unsigned format) -> size_t { return format == 0 ? 4 : format == 5 ? 2 : (format == 1 ? 8 : 16); };
    for (size_t index = 0; index < configuredTextures.size(); ++index) {
        const TextureUpload& source = configuredTextures[index];
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = source.width;
        description.Height = source.height;
        description.DepthOrArraySize = 1;
        description.MipLevels = static_cast<UINT16>(source.mips);
        description.Format = dxgiFormat(source.format, source.srgb);
        description.SampleDesc.Count = 1;
        auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&cookedTextures[index])),
                     "Create cooked texture", error)) {
            releasePending();
            return false;
        }
        track(cookedTextures[index]);
        PendingUpload& item = pending[index];
        item.mips = source.mips;
        item.width = source.width;
        item.height = source.height;
        item.format = source.format;
        item.footprints.resize(source.mips);
        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(&description, 0, source.mips, 0, item.footprints.data(),
                                      nullptr, nullptr, &uploadSize);
        if (!createBuffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &item.upload, error)) {
            releasePending();
            return false;
        }
        unsigned char* pixels = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (!checked(item.upload->Map(0, &noRead, reinterpret_cast<void**>(&pixels)),
                     "Map cooked texture upload", error)) {
            releasePending();
            return false;
        }
        std::memset(pixels, 0, static_cast<size_t>(uploadSize));
        size_t sourceOffset = 0;
        const size_t bytesPerBlock = blockBytes(source.format);
        for (unsigned mip = 0; mip < source.mips; ++mip) {
            const unsigned width = std::max(1u, source.width >> mip);
            const unsigned height = std::max(1u, source.height >> mip);
            const unsigned blocksX = (source.format == 0 || source.format == 5) ? width : (width + 3) / 4;
            const unsigned blocksY = (source.format == 0 || source.format == 5) ? height : (height + 3) / 4;
            const size_t rowBytes = static_cast<size_t>(blocksX) * bytesPerBlock;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = item.footprints[mip];
            for (unsigned y = 0; y < blocksY; ++y) {
                std::memcpy(pixels + footprint.Offset + y * footprint.Footprint.RowPitch,
                            source.pixels.data() + sourceOffset + y * rowBytes, rowBytes);
            }
            sourceOffset += rowBytes * blocksY;
        }
        D3D12_RANGE written{0, static_cast<SIZE_T>(uploadSize)};
        item.upload->Unmap(0, &written);
    }
    if (!checked(allocators[0]->Reset(), "Reset cooked upload allocator", error) ||
        !checked(commands->Reset(allocators[0], nullptr), "Reset cooked upload commands", error)) {
        releasePending();
        return false;
    }
    for (size_t index = 0; index < configuredTextures.size(); ++index) {
        PendingUpload& item = pending[index];
        for (unsigned mip = 0; mip < item.mips; ++mip) {
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = cookedTextures[index];
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = item.upload;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = item.footprints[mip];
            commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier = detail::transitionBarrier(
            cookedTextures[index], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &barrier);
    }
    if (!checked(commands->Close(), "Close cooked upload commands", error)) {
        releasePending();
        return false;
    }
    ID3D12CommandList* lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    const bool complete = waitIdle(error);
    releasePending();
    if (!complete) return false;
    for (size_t index = 0; index < configuredTextures.size(); ++index) {
        const TextureUpload& source = configuredTextures[index];
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = dxgiFormat(source.format, source.srgb);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MostDetailedMip = 0;
        view.Texture2D.MipLevels = source.mips;
        device->CreateShaderResourceView(cookedTextures[index], &view,
                                         cpuDescriptor(srvBase + index));
    }
    return true;
}

bool Renderer::Impl::uploadCookedTextures(std::string& error) {
    if (!uploadTextureSet(configuredTextures, detail::CookedTextureSrvBase, cookedTextures, error)) return false;
    // Material records include four independently transformed/sampled texture slots.
    // from relative texture slots to absolute SRV heap indices here.
    std::vector<detail::GpuMaterial> records(configuredMaterials.size());
    std::vector<engine::material::layered::GpuLayer> layers;
    for (size_t index = 0; index < configuredMaterials.size(); ++index) {
        const MaterialUpload& source = configuredMaterials[index];
        detail::GpuMaterial& record = records[index];
        record.albedo = detail::CookedTextureSrvBase + source.albedo;
        record.normal = (source.normal & 0x7FFFFFFFu) + detail::CookedTextureSrvBase;
        record.normal |= source.normal & 0x80000000u;
        record.orm = detail::CookedTextureSrvBase + source.orm;
        record.flags = (source.alphaMasked?1u:0u)|(source.hasEmissive?2u:0u)|(source.hasOcclusion?4u:0u)|(source.unlit?8u:0u)|(source.alphaBlended?16u:0u)|(source.hasCoat?32u:0u)|(source.hasCoatRoughness?64u:0u)|(source.hasCoatNormal?128u:0u);
        record.uvScale = source.uvScale;
        record.roughness = source.roughness;
        record.metalness = source.metalness;
        record.wetness = source.wetness;
        record.wear = source.wear;
        record.clearcoat = source.clearcoat;
        record.clearcoatRoughness = source.clearcoatRoughness;
        record.alphaCutoff = source.alphaCutoff;
        record.emissive = detail::CookedTextureSrvBase + source.emissive;
        std::copy(source.emissiveFactor,source.emissiveFactor+3,record.emissiveFactor);
        record.emissiveStrength = source.emissiveStrength;
        record.opacity=source.opacity;
        record.dielectricF0=engine::materialDielectricF0(source.ior);
        record.occlusion=detail::CookedTextureSrvBase+source.occlusion;record.occlusionStrength=source.occlusionStrength;
        record.coat=detail::CookedTextureSrvBase+source.coat;record.coatRoughness=detail::CookedTextureSrvBase+source.coatRoughness;record.coatNormal=((source.coatNormal&0x7FFFFFFFu)+detail::CookedTextureSrvBase)|(source.coatNormal&0x80000000u);
        std::copy(source.textureMappings,source.textureMappings+8,record.textureMappings);
        record.layerCount=static_cast<unsigned>(source.layers.size());
        record.layerOffset=static_cast<unsigned>(layers.size());record.layerDescriptor=detail::SlotMaterialLayers;
        record.surfaceRelief=source.surfaceRelief;
        if(record.surfaceRelief.texture!=0xffffffffu)record.surfaceRelief.texture+=detail::CookedTextureSrvBase;
        for(const auto& layer:source.layers){
            layers.push_back(layer);
            for(auto& slot:layers.back().slots)if(slot.texture!=0xffffffffu)slot.texture+=detail::CookedTextureSrvBase;
        }
    }
    if(!layers.empty()){
        const size_t layerBytes=layers.size()*sizeof(layers[0]);
        if(!createBuffer(layerBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,&materialLayerRecords,error))return false;
        void* mapped=nullptr;D3D12_RANGE noRead{0,0};
        if(!checked(materialLayerRecords->Map(0,&noRead,&mapped),"Map material layers",error))return false;
        std::memcpy(mapped,layers.data(),layerBytes);D3D12_RANGE written{0,static_cast<SIZE_T>(layerBytes)};materialLayerRecords->Unmap(0,&written);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Buffer.NumElements=static_cast<UINT>(layers.size());view.Buffer.StructureByteStride=sizeof(layers[0]);
        device->CreateShaderResourceView(materialLayerRecords,&view,cpuDescriptor(detail::SlotMaterialLayers));
    }
    const size_t recordBytes = records.size() * sizeof(detail::GpuMaterial);
    if (!createBuffer(recordBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &materialRecords, error)) {
        return false;
    }
    void* mappedRecords = nullptr;
    D3D12_RANGE noReadRecords{0, 0};
    if (!checked(materialRecords->Map(0, &noReadRecords, &mappedRecords),
                 "Map material records", error)) {
        return false;
    }
    std::memcpy(mappedRecords, records.data(), recordBytes);
    D3D12_RANGE writtenRecords{0, static_cast<SIZE_T>(recordBytes)};
    materialRecords->Unmap(0, &writtenRecords);
    D3D12_SHADER_RESOURCE_VIEW_DESC recordView{};
    recordView.Format = DXGI_FORMAT_UNKNOWN;
    recordView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    recordView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    recordView.Buffer.FirstElement = 0;
    recordView.Buffer.NumElements = static_cast<UINT>(records.size());
    recordView.Buffer.StructureByteStride = sizeof(detail::GpuMaterial);
    device->CreateShaderResourceView(materialRecords, &recordView,
                                     cpuDescriptor(detail::SlotMaterialRecords));
    materialTableReady = true;
    return true;
}

bool Renderer::Impl::uploadScene(std::string& error) {
    ID3D12Resource* vertexUpload = nullptr;
    ID3D12Resource* indexUpload = nullptr;
    ID3D12Resource* textureUpload = nullptr;
    auto cleanup = [&] {
        destroy(vertexUpload);
        destroy(indexUpload);
        destroy(textureUpload);
    };
    if (!createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COPY_DEST, &vertexBuffer, error) ||
        !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COPY_DEST, &indexBuffer, error) ||
        !createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &vertexUpload, error) ||
        !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &indexUpload, error)) {
        cleanup();
        return false;
    }
    auto fill = [&](ID3D12Resource* resource, const void* bytes, size_t size) {
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (!checked(resource->Map(0, &noRead, &mapped), "Map upload buffer", error)) return false;
        std::memcpy(mapped, bytes, size);
        D3D12_RANGE written{0, size};
        resource->Unmap(0, &written);
        return true;
    };
    if (!fill(vertexUpload, CubeVertices, sizeof(CubeVertices)) ||
        !fill(indexUpload, CubeIndices, sizeof(CubeIndices))) {
        cleanup();
        return false;
    }

    if (materialPath && materialBindless) {
        // No Texture2DArray at all in the bindless path: upload the vertex/index buffers,
        // then let uploadCookedTextures() create one Texture2D per cooked texture.
        if (!checked(allocators[0]->Reset(), "Reset upload allocator", error) ||
            !checked(commands->Reset(allocators[0], nullptr), "Reset upload commands", error)) {
            cleanup();
            return false;
        }
        commands->CopyBufferRegion(vertexBuffer, 0, vertexUpload, 0, sizeof(CubeVertices));
        commands->CopyBufferRegion(indexBuffer, 0, indexUpload, 0, sizeof(CubeIndices));
        D3D12_RESOURCE_BARRIER barriers[] = {
            detail::transitionBarrier(vertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            detail::transitionBarrier(indexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_INDEX_BUFFER)
        };
        commands->ResourceBarrier(2, barriers);
        if (!checked(commands->Close(), "Close upload commands", error)) {
            cleanup();
            return false;
        }
        ID3D12CommandList* lists[] = {commands};
        queue->ExecuteCommandLists(1, lists);
        const bool complete = waitIdle(error);
        cleanup();
        if (!complete) return false;
        vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexView.SizeInBytes = sizeof(CubeVertices);
        vertexView.StrideInBytes = sizeof(Vertex);
        indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexView.SizeInBytes = sizeof(CubeIndices);
        indexView.Format = DXGI_FORMAT_R16_UINT;
        return uploadCookedTextures(error);
    }

    const UINT textureSlices = materialPath ? materialTextureSlices : detail::MaterialSlotCount;
    const UINT textureMips = materialPath ? materialTextureMips : 1;
    const UINT textureWidth = materialPath ? materialTextureWidth : 64;
    const UINT textureHeight = materialPath ? materialTextureHeight : 64;
    if (materialPath && !materialBindless) {
        // Array fallback: every cooked texture must be RGBA8 with identical dimensions.
        for (const TextureUpload& texture : configuredTextures) {
            if (texture.format != 0 || texture.width != textureWidth ||
                texture.height != textureHeight || texture.mips != textureMips) {
                error = "Array fallback requires uniform RGBA8 textures; bindless support is unavailable";
                cleanup();
                return false;
            }
        }
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = textureWidth;
    description.Height = textureHeight;
    description.DepthOrArraySize = static_cast<UINT16>(textureSlices);
    description.MipLevels = static_cast<UINT16>(textureMips);
    description.Format = detail::ColorFormat;
    description.SampleDesc.Count = 1;
    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&materialTexture)),
                 "Create material texture array", error)) {
        cleanup();
        return false;
    }
    track(materialTexture);
    const UINT subresources = textureSlices * textureMips;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&description, 0, subresources, 0, footprints.data(), nullptr,
                                  nullptr, &uploadSize);
    if (!createBuffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &textureUpload, error)) {
        cleanup();
        return false;
    }
    unsigned char* pixels = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (!checked(textureUpload->Map(0, &noRead, reinterpret_cast<void**>(&pixels)),
                 "Map texture upload", error)) {
        cleanup();
        return false;
    }
    std::memset(pixels, 0, static_cast<size_t>(uploadSize));
    if (materialPath) {
        // Mips are packed in the cooked container in mip order; rows are tightened into
        // the D3D12 row pitch here, which is the only place the two layouts differ.
        for (UINT slice = 0; slice < textureSlices; ++slice) {
            const TextureUpload& source = configuredTextures[slice];
            size_t sourceOffset = 0;
            for (UINT mip = 0; mip < textureMips; ++mip) {
                const UINT width = std::max(1u, textureWidth >> mip);
                const UINT height = std::max(1u, textureHeight >> mip);
                const size_t rowBytes = static_cast<size_t>(width) * 4;
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint =
                    footprints[slice * textureMips + mip];
                for (UINT y = 0; y < height; ++y) {
                    std::memcpy(pixels + footprint.Offset + y * footprint.Footprint.RowPitch,
                                source.pixels.data() + sourceOffset + y * rowBytes, rowBytes);
                }
                sourceOffset += rowBytes * height;
            }
        }
    } else {
        for (UINT slice = 0; slice < textureSlices; ++slice) {
            for (UINT y = 0; y < textureHeight; ++y) {
                for (UINT x = 0; x < textureWidth; ++x) {
                    auto* pixel = pixels + footprints[slice].Offset +
                                  y * footprints[slice].Footprint.RowPitch + x * 4;
                    unsigned char red = 220, green = 220, blue = 220;
                    if (slice == 0) {
                        const unsigned char shade = ((x / 8 + y / 8) & 1) ? 205 : 255;
                        red = green = blue = shade;
                    } else if (slice == 1) {
                        const bool mortar = (y % 16 < 2) || ((x + ((y / 16) % 2) * 8) % 32 < 2);
                        if (mortar) {
                            red = 205; green = 198; blue = 184;
                        } else {
                            const unsigned char variation =
                                static_cast<unsigned char>(18 + ((x / 32 + y / 16) & 3) * 7);
                            red = static_cast<unsigned char>(138 + variation);
                            green = static_cast<unsigned char>(62 + variation / 2);
                            blue = static_cast<unsigned char>(48 + variation / 3);
                        }
                    } else if (slice == 3) {
                        // Dedicated missing-material pattern: magenta checker.
                        const bool light = ((x / 8 + y / 8) & 1) != 0;
                        red = light ? 255 : 40; green = light ? 40 : 0; blue = light ? 220 : 60;
                    } else if (slice == 4) {
                        red = 90; green = 170; blue = 210; // extra blue-grey variant
                    } else if (slice == 5) {
                        red = 200; green = 190; blue = 120; // extra sand variant
                    } else if (slice == 6) {
                        red = 120; green = 200; blue = 140; // extra green variant
                    } else {
                        const unsigned char stripe = ((x / 4) & 1) ? 188 : 132;
                        red = static_cast<unsigned char>(stripe - 12);
                        green = static_cast<unsigned char>(stripe);
                        blue = static_cast<unsigned char>(std::min(255, stripe + 18));
                    }
                    pixel[0] = red; pixel[1] = green; pixel[2] = blue; pixel[3] = 255;
                }
            }
        }
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(uploadSize)};
    textureUpload->Unmap(0, &written);
    if (!checked(allocators[0]->Reset(), "Reset upload allocator", error) ||
        !checked(commands->Reset(allocators[0], nullptr), "Reset upload commands", error)) {
        cleanup();
        return false;
    }
    commands->CopyBufferRegion(vertexBuffer, 0, vertexUpload, 0, sizeof(CubeVertices));
    commands->CopyBufferRegion(indexBuffer, 0, indexUpload, 0, sizeof(CubeIndices));
    for (UINT subresource = 0; subresource < subresources; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = materialTexture;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = textureUpload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[subresource];
        commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barriers[] = {
        detail::transitionBarrier(vertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        detail::transitionBarrier(indexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER),
        detail::transitionBarrier(materialTexture, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    commands->ResourceBarrier(3, barriers);
    if (!checked(commands->Close(), "Close upload commands", error)) {
        cleanup();
        return false;
    }
    ID3D12CommandList* lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    const bool complete = waitIdle(error);
    cleanup();
    if (!complete) return false;
    vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes = sizeof(CubeVertices);
    vertexView.StrideInBytes = sizeof(Vertex);
    indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
    indexView.SizeInBytes = sizeof(CubeIndices);
    indexView.Format = DXGI_FORMAT_R16_UINT;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = detail::ColorFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = textureMips;
    view.Texture2DArray.FirstArraySlice = 0;
    view.Texture2DArray.ArraySize = textureSlices;
    device->CreateShaderResourceView(materialTexture, &view,
                                    cpuDescriptor(detail::SlotMaterialTable));
    materialTableReady = true;
    return true;
}

void Renderer::Impl::releaseConvexMeshes() {
    destroy(convexVertexBuffer);
    destroy(convexIndexBuffer);
    convexRanges.clear();
    convexMeshesReady = false;
    convexMeshBytes = 0;
}

// Concatenates every configured convex mesh into one vertex/index buffer pair. The
// draw path then binds a byte offset instead of switching resources, which keeps the
// per-item cost the same as the cube path.
void Renderer::Impl::releaseTransparency(){
    for(auto* resource:{&transparencyHeads,&transparencyCoverage,&transparencyNodes,&transparencyCounter}){destroy(resource->resource);resource->state=D3D12_RESOURCE_STATE_COMMON;}
    destroy(transparencyReadback);destroy(transparencyCpuHeap);transparencyWidth=transparencyHeight=transparencyCapacity=0;transparencyActive=false;
    for(auto& pending:transparencyPending)pending=false;
}
void Renderer::Impl::transparencyViews(){
    for(unsigned frame=0;frame<detail::FrameCount;frame++){
        const auto base=detail::FrameDescriptorBase+frame*detail::FrameDescriptorStride,oit=detail::TransparencyDescriptorBase+frame*detail::TransparencyDescriptorStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC texture{};texture.Format=DXGI_FORMAT_R32_UINT;texture.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;texture.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;texture.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(transparencyHeads.resource,&texture,cpuDescriptor(base+detail::FdsTransparencyHeads));
        D3D12_SHADER_RESOURCE_VIEW_DESC nodes{};nodes.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;nodes.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;nodes.Buffer.NumElements=std::max(1u,transparencyCapacity);nodes.Buffer.StructureByteStride=32;
        device->CreateShaderResourceView(transparencyNodes.resource,&nodes,cpuDescriptor(base+detail::FdsTransparencyNodes));
        D3D12_UNORDERED_ACCESS_VIEW_DESC head{};head.Format=DXGI_FORMAT_R32_UINT;head.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(transparencyHeads.resource,nullptr,&head,cpuDescriptor(oit));
        device->CreateUnorderedAccessView(transparencyCoverage.resource,nullptr,&head,cpuDescriptor(oit+4));
        D3D12_UNORDERED_ACCESS_VIEW_DESC list{};list.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;list.Buffer.NumElements=std::max(1u,transparencyCapacity);list.Buffer.StructureByteStride=32;
        device->CreateUnorderedAccessView(transparencyNodes.resource,nullptr,&list,cpuDescriptor(oit+1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC count{};count.Format=DXGI_FORMAT_R32_TYPELESS;count.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;count.Buffer.NumElements=4;count.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(transparencyCounter.resource,nullptr,&count,cpuDescriptor(oit+2));
        if(transparencyCpuHeap){auto handle=transparencyCpuHeap->GetCPUDescriptorHandleForHeapStart();device->CreateUnorderedAccessView(transparencyHeads.resource,nullptr,&head,handle);handle.ptr+=srvStride;device->CreateUnorderedAccessView(transparencyCounter.resource,nullptr,&count,handle);handle.ptr+=srvStride;device->CreateUnorderedAccessView(transparencyCoverage.resource,nullptr,&head,handle);handle.ptr+=(1+frame)*srvStride;D3D12_UNORDERED_ACCESS_VIEW_DESC shadow{};shadow.Format=detail::ShadowMaskFormat;shadow.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(shadowMask[frame].resource,nullptr,&shadow,handle);}
        D3D12_SHADER_RESOURCE_VIEW_DESC tlasView{};tlasView.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;tlasView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(raytracingTier>=11)device->CreateShaderResourceView(nullptr,&tlasView,cpuDescriptor(oit+3));
        else device->CreateShaderResourceView(nullptr,&nodes,cpuDescriptor(oit+3));
    }
}
bool Renderer::Impl::createTransparency(std::string& error){
    if(transparencyHeads&&transparencyWidth==width&&transparencyHeight==height)return true;
    if(!waitIdle(error))return false;releaseTransparency();
    const auto count=std::min<std::uint64_t>(std::uint64_t(width)*height*8,8ull*1024*1024);
    if(!count){error="NG-E-MAT-TRANSPARENCY: empty viewport";return false;}transparencyCapacity=static_cast<unsigned>(count);
    if(!createTexture2D(width,height,DXGI_FORMAT_R32_UINT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,nullptr,transparencyHeads,error)||
       !createTexture2D(width,height,DXGI_FORMAT_R32_UINT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,nullptr,transparencyCoverage,error)||
       !createBuffer(count*32,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,&transparencyNodes.resource,error)||
       !createBuffer(16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,&transparencyCounter.resource,error)||
       !createBuffer(16*detail::FrameCount,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,&transparencyReadback,error))return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=3+detail::FrameCount;
    if(!checked(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&transparencyCpuHeap)),"Create transparency clear descriptors",error))return false;track(transparencyCpuHeap);
    transparencyNodes.state=transparencyCounter.state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;transparencyWidth=width;transparencyHeight=height;transparencyViews();return true;
}
bool Renderer::Impl::collectTransparency(std::string& error){
    if(!transparencyReadback||!fence)return true;
    const auto completed=fence->GetCompletedValue();bool ready=false;for(unsigned i=0;i<detail::FrameCount;i++)ready|=transparencyPending[i]&&frameFences[i]<=completed;
    if(!ready)return true;void* mapped=nullptr;D3D12_RANGE read{0,16*detail::FrameCount};
    if(!checked(transparencyReadback->Map(0,&read,&mapped),"Map transparency counters",error))return false;unsigned flags=0;
    for(unsigned i=0;i<detail::FrameCount;i++)if(transparencyPending[i]&&frameFences[i]<=completed){const auto* values=static_cast<const unsigned*>(mapped)+i*4;transparencyFragments=values[0];transparencyPeakFragments=std::max(transparencyPeakFragments,values[0]);if(values[1]){flags|=values[1];++transparencyOverflowFrames;}transparencyPending[i]=false;}
    D3D12_RANGE written{0,0};transparencyReadback->Unmap(0,&written);
    if(flags){error="NG-E-MAT-TRANSPARENCY-BUDGET: "+std::string(flags&1?"fragment pool capacity exceeded":"more than 64 transparent layers at one pixel");addDiagnostic("NG-E-MAT-TRANSPARENCY-BUDGET",error);return false;}return true;
}
bool Renderer::synchronize(std::string& error){return impl_->waitIdle(error);}
bool Renderer::Impl::uploadConvexMeshes(std::string& error) {
    // Scene edits may arrive while previous frames still read this pool. The
    // upload also resets an allocator, so complete those frames before either
    // freeing the old buffers or recording replacement copies.
    if(initialized&&!waitIdle(error))return false;
    releaseConvexMeshes();
    if (configuredConvexMeshes.empty()) return true;
    if(!materialBindless&&std::any_of(configuredConvexMeshes.begin(),configuredConvexMeshes.end(),[](const engine::ConvexMesh& mesh){return mesh.hasTextureUvs;})){
        error="NG-E-MAT-UV: multiple texture UVs require the SM 6.6 material pipeline";return false;
    }

    std::size_t vertexCount = 0, indexCount = 0;
    for (const auto& mesh : configuredConvexMeshes) {
        if (mesh.vertices.size() > 1000000ull || mesh.indices.size() > 3000000ull) {
            error = "NG-E-CONVEX-BUDGET: mesh exceeds the per-shard vertex/index limit";
            return false;
        }
        vertexCount += mesh.vertices.size();
        indexCount += mesh.indices.size();
    }
    if (vertexCount == 0 || indexCount == 0) return true;
    const std::size_t vertexBytes = vertexCount * sizeof(engine::ConvexVertex);
    const std::size_t indexBytes = indexCount * sizeof(std::uint32_t);
    if (vertexBytes + indexBytes > 64ull * 1024ull * 1024ull) {
        error = "NG-E-CONVEX-BUDGET: convex pool exceeds 64 MiB";
        return false;
    }

    std::vector<engine::ConvexVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(vertexCount);
    indices.reserve(indexCount);
    convexRanges.reserve(configuredConvexMeshes.size());
    for (const auto& mesh : configuredConvexMeshes) {
        ConvexRange range;
        range.vertexOffset = static_cast<unsigned>(vertices.size() * sizeof(engine::ConvexVertex));
        range.vertexBytes = static_cast<unsigned>(mesh.vertices.size() * sizeof(engine::ConvexVertex));
        range.indexOffset = static_cast<unsigned>(indices.size() * sizeof(std::uint32_t));
        range.indexCount = static_cast<unsigned>(mesh.indices.size());
        range.hasTextureUvs=mesh.hasTextureUvs;
        range.hasLayerUvs=mesh.hasLayerUvs;
        for (int axis = 0; axis < 3; ++axis) {
            range.boundsMin[axis] = mesh.min[axis];
            range.boundsMax[axis] = mesh.max[axis];
        }
        // Each draw binds its vertex-buffer byte range, so indices remain mesh-local.
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        convexRanges.push_back(range);
    }

    ID3D12Resource* vertexUpload = nullptr;
    ID3D12Resource* indexUpload = nullptr;
    auto cleanup = [&] {
        destroy(vertexUpload);
        destroy(indexUpload);
    };
    if (!createBuffer(vertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                      &convexVertexBuffer, error) ||
        !createBuffer(indexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                      &convexIndexBuffer, error) ||
        !createBuffer(vertexBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &vertexUpload, error) ||
        !createBuffer(indexBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &indexUpload, error)) {
        cleanup();
        releaseConvexMeshes();
        return false;
    }
    auto fill = [&](ID3D12Resource* resource, const void* bytes, std::size_t size) {
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (!checked(resource->Map(0, &noRead, &mapped), "Map convex upload buffer", error)) return false;
        std::memcpy(mapped, bytes, size);
        D3D12_RANGE written{0, size};
        resource->Unmap(0, &written);
        return true;
    };
    if (!fill(vertexUpload, vertices.data(), vertexBytes) ||
        !fill(indexUpload, indices.data(), indexBytes)) {
        cleanup();
        releaseConvexMeshes();
        return false;
    }
    if (!checked(allocators[0]->Reset(), "Reset convex upload allocator", error) ||
        !checked(commands->Reset(allocators[0], nullptr), "Reset convex upload commands", error)) {
        cleanup();
        releaseConvexMeshes();
        return false;
    }
    commands->CopyBufferRegion(convexVertexBuffer, 0, vertexUpload, 0, vertexBytes);
    commands->CopyBufferRegion(convexIndexBuffer, 0, indexUpload, 0, indexBytes);
    D3D12_RESOURCE_BARRIER barriers[] = {
        detail::transitionBarrier(convexVertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        detail::transitionBarrier(convexIndexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER)
    };
    commands->ResourceBarrier(2, barriers);
    if (!checked(commands->Close(), "Close convex upload commands", error)) {
        cleanup();
        releaseConvexMeshes();
        return false;
    }
    ID3D12CommandList* lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    const bool complete = waitIdle(error);
    cleanup();
    if (!complete) {
        releaseConvexMeshes();
        return false;
    }
    convexMeshesReady = true;
    convexMeshBytes = static_cast<unsigned>(vertexBytes + indexBytes);
    return true;
}

void Renderer::Impl::queryCapabilities() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options)))) {
        bindingTier = static_cast<unsigned>(options.ResourceBindingTier);
    }
#if defined(__ID3D12Device1_INTERFACE_DEFINED__)
    // An older runtime can reject a newer maximum. Retry descending versions.
    for (int candidate = 0x68; candidate >= 0x60; --candidate) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(candidate);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                  sizeof(model)))) {
            shaderModel = static_cast<unsigned>(model.HighestShaderModel);
            break;
        }
    }
    if (!shaderModel) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0x51);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                  sizeof(model)))) {
            shaderModel = static_cast<unsigned>(model.HighestShaderModel);
        }
    }
#endif
#if defined(__ID3D12Device5_INTERFACE_DEFINED__)
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                              sizeof(options5)))) {
        raytracingTier = static_cast<int>(options5.RaytracingTier);
    }
#endif
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                              sizeof(options7)))) {
        meshShaderTier = static_cast<int>(options7.MeshShaderTier);
    }
#endif
}

bool Renderer::Impl::buildAccelerationStructures() {
    accelerationStructureError.clear();
    if (raytracingTier < 10) {
        accelerationStructureError = "DXR tier is unsupported";
        return false;
    }
    if (!device5 || !commands4) {
        accelerationStructureError = "DXR device or command-list interface is unavailable";
        return false;
    }
    std::string error;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
    geometry.Triangles.IndexCount = 36;
    geometry.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress();
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = 24;
    geometry.Triangles.VertexBuffer.StartAddress = vertexBuffer->GetGPUVirtualAddress();
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.NumDescs = 1;
    blasInputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasInfo);
    if (blasInfo.ResultDataMaxSizeInBytes == 0 || blasInfo.ScratchDataSizeInBytes == 0) {
        accelerationStructureError = "BLAS prebuild returned an empty size";
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.NumDescs = tlasCapacity;
    tlasInputs.InstanceDescs = 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
    if (tlasInfo.ResultDataMaxSizeInBytes == 0 || tlasInfo.ScratchDataSizeInBytes == 0) {
        accelerationStructureError = "TLAS prebuild returned an empty size";
        return false;
    }

    ID3D12Resource* blasScratch = nullptr;
    auto cleanup = [&] {
        destroy(blasScratch);
    };
    if (!createBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &blas, error) ||
        !createBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      &blasScratch, error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!createBuffer(static_cast<UINT64>(tlasCapacity) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                          &tlasInstances[index], error) ||
            !createBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &tlas[index], error) ||
            !createBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          &tlasScratch[index], error)) {
            accelerationStructureError = error;
            cleanup();
            return false;
        }
    }

    if (!checked(allocators[0]->Reset(), "Reset AS allocator", error) ||
        !checked(commands4->Reset(allocators[0], nullptr), "Reset AS commands", error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc{};
    blasDesc.Inputs = blasInputs;
    blasDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
    blasDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    commands4->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
    D3D12_RESOURCE_BARRIER blasBarrier{};
    blasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    blasBarrier.UAV.pResource = blas;
    commands4->ResourceBarrier(1, &blasBarrier);
    if (!checked(commands4->Close(), "Close AS commands", error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    ID3D12CommandList* lists[] = {commands4};
    queue->ExecuteCommandLists(1, lists);
    if (!waitIdle(error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    cleanup();
    blasBytes = blasInfo.ResultDataMaxSizeInBytes;
    tlasBytes = tlasInfo.ResultDataMaxSizeInBytes;
    tlasScratchBytes = tlasInfo.ScratchDataSizeInBytes;
    blasBuilt = true;

    // TLAS descriptors can only be created once the resources exist.
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.RaytracingAccelerationStructure.Location = tlas[index]->GetGPUVirtualAddress();
        const unsigned base = detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;
        device->CreateShaderResourceView(nullptr, &view,
                                        cpuDescriptor(base + detail::FdsTopLevelAccelerationStructure));
    }
    return true;
}

bool Renderer::Impl::updateTopLevelAccelerationStructure(
    ID3D12GraphicsCommandList4* list, const std::vector<DrawItem>& items, unsigned frameIndex) {
    if (!blasBuilt || !list) return false;
    unsigned count = static_cast<unsigned>(items.size());
    if (count > tlasCapacity) count = tlasCapacity;
    if (count == 0) return false;

    D3D12_RAYTRACING_INSTANCE_DESC* instances = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(tlasInstances[frameIndex]->Map(0, &noRead, reinterpret_cast<void**>(&instances)))) {
        return false;
    }
    std::memset(instances, 0, static_cast<size_t>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    const D3D12_GPU_VIRTUAL_ADDRESS blasAddress = blas->GetGPUVirtualAddress();
    for (unsigned index = 0; index < count; ++index) {
        const Mat4& model = items[index].model;
        // A convex shard is drawn from a body-local mesh, so its model matrix carries no
        // scale and the unit-cube BLAS would be the wrong size. Shadow rays get the
        // fitted OBB instead: same rotation and origin, scaled to the shard's bounds.
        Mat4 instanceModel = model;
        const int mesh = items[index].mesh;
        if (mesh >= 0 && static_cast<std::size_t>(mesh) < convexRanges.size()) {
            const ConvexRange& range = convexRanges[static_cast<std::size_t>(mesh)];
            float center[3], size[3];
            for (int axis = 0; axis < 3; ++axis) {
                center[axis] = 0.5f * (range.boundsMin[axis] + range.boundsMax[axis]);
                size[axis] = std::max(1.0e-3f, range.boundsMax[axis] - range.boundsMin[axis]);
            }
            float offset[3];
            for (int row = 0; row < 3; ++row) {
                offset[row] = model.m[0 * 4 + row] * center[0] + model.m[1 * 4 + row] * center[1] +
                              model.m[2 * 4 + row] * center[2] + model.m[3 * 4 + row];
            }
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    instanceModel.m[column * 4 + row] = model.m[column * 4 + row] * size[column];
                }
                instanceModel.m[3 * 4 + row] = offset[row];
            }
            instanceModel.m[3] = 0.0f;
            instanceModel.m[7] = 0.0f;
            instanceModel.m[11] = 0.0f;
            instanceModel.m[15] = 1.0f;
        }
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                instances[index].Transform[row][column] = instanceModel.m[column * 4 + row];
            }
        }
        instances[index].InstanceID = index;
        instances[index].InstanceMask = items[index].rayVisibilityMask & 0xFFu;
        instances[index].InstanceContributionToHitGroupIndex = 0;
        // Both faces must occlude: the shadow ray starts on a surface.
        instances[index].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instances[index].AccelerationStructure = blasAddress;
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)};
    tlasInstances[frameIndex]->Unmap(0, &written);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = count;
    inputs.InstanceDescs = tlasInstances[frameIndex]->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
    description.Inputs = inputs;
    description.ScratchAccelerationStructureData = tlasScratch[frameIndex]->GetGPUVirtualAddress();
    description.DestAccelerationStructureData = tlas[frameIndex]->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&description, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = tlas[frameIndex];
    list->ResourceBarrier(1, &barrier);
    tlasInstanceCount = count;
    ++tlasRebuilds;
    return true;
}

void Renderer::Impl::createTimestamps() {
    if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)) || !timestampFrequency) return;
    D3D12_QUERY_HEAP_DESC description{};
    description.Count = detail::FrameCount * detail::TimestampsPerFrame;
    description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (FAILED(device->CreateQueryHeap(&description, IID_PPV_ARGS(&timestampHeap)))) return;
    track(timestampHeap);
    std::string ignored;
    if (!createBuffer(detail::FrameCount * detail::TimestampsPerFrame * sizeof(UINT64),
                      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                      &timestampReadback, ignored)) {
        destroy(timestampHeap);
        return;
    }
    timestampsAvailable = true;
}

void Renderer::Impl::readTimestamp(unsigned index) {
    if (!timestampsAvailable || !timestampPending[index]) return;
    const SIZE_T offset =
        static_cast<SIZE_T>(index) * detail::TimestampsPerFrame * sizeof(UINT64);
    D3D12_RANGE readRange{offset, offset + detail::TimestampsPerFrame * sizeof(UINT64)};
    void* mapped = nullptr;
    if (SUCCEEDED(timestampReadback->Map(0, &readRange, &mapped))) {
        auto* ticks = reinterpret_cast<UINT64*>(static_cast<unsigned char*>(mapped) + offset);
        const UINT64 frequency = timestampFrequency ? timestampFrequency : 1;
        if (ticks[detail::TimestampFrameEnd] >= ticks[detail::TimestampFrameBegin]) {
            frameStats.gpuMs =
                static_cast<double>(ticks[detail::TimestampFrameEnd] -
                                    ticks[detail::TimestampFrameBegin]) * 1000.0 /
                static_cast<double>(frequency);
            frameStats.gpuValid = true;
        }
        if(gpuEffectRuntime&&ticks[detail::TimestampGpuEffectsEnd]>=ticks[detail::TimestampGpuEffectsBegin]&&ticks[detail::TimestampGpuEffectDrawEnd]>=ticks[detail::TimestampGpuEffectDrawBegin]){
            gpuEffectSimulationMs=static_cast<double>(ticks[detail::TimestampGpuEffectsEnd]-ticks[detail::TimestampGpuEffectsBegin])*1000.0/static_cast<double>(frequency);
            gpuEffectDrawMs=static_cast<double>(ticks[detail::TimestampGpuEffectDrawEnd]-ticks[detail::TimestampGpuEffectDrawBegin])*1000.0/static_cast<double>(frequency);++gpuEffectTimingSamples;
        }
        if(gpuClothDraw&&ticks[detail::TimestampGpuClothEnd]>=ticks[detail::TimestampGpuClothBegin]&&ticks[detail::TimestampGpuClothDrawEnd]>=ticks[detail::TimestampGpuClothDrawBegin]){
            gpuClothSimulationMs=static_cast<double>(ticks[detail::TimestampGpuClothEnd]-ticks[detail::TimestampGpuClothBegin])*1000.0/static_cast<double>(frequency);
            gpuClothDrawMs=static_cast<double>(ticks[detail::TimestampGpuClothDrawEnd]-ticks[detail::TimestampGpuClothDrawBegin])*1000.0/static_cast<double>(frequency);++gpuClothTimingSamples;
        }
        if (ticks[detail::TimestampRtEnd] > ticks[detail::TimestampRtBegin]) {
            rtLastGpuMs = static_cast<double>(ticks[detail::TimestampRtEnd] -
                                              ticks[detail::TimestampRtBegin]) * 1000.0 /
                          static_cast<double>(frequency);
            frameStats.gpuRtMs = rtLastGpuMs;
            frameStats.gpuRtValid = true;
        } else {
            frameStats.gpuRtMs = 0.0;
            frameStats.gpuRtValid = false;
        }
        if (qualityReady && environment.quality) {
            auto elapsed = [&](unsigned begin, unsigned end) {
                return ticks[end] >= ticks[begin] ? static_cast<double>(ticks[end]-ticks[begin])*1000.0/
                                                     static_cast<double>(frequency) : 0.0;
            };
            qualityCloudShadowMs=elapsed(detail::TimestampQualityBegin,detail::TimestampCloudShadowEnd);
            qualityLocalShadowCacheMs=elapsed(detail::TimestampCloudShadowEnd,detail::TimestampLocalShadowCacheEnd);
            qualityProbeMs=elapsed(detail::TimestampLocalShadowCacheEnd,detail::TimestampProbeEnd);
            qualityFogMs=elapsed(detail::TimestampFogBegin,detail::TimestampFogEnd);
            qualityCloudMs=elapsed(detail::TimestampFogEnd,detail::TimestampCloudEnd);
            if (qualityFrames>30) {
                qualityProbeTimes.push_back(qualityProbeMs);qualityCloudShadowTimes.push_back(qualityCloudShadowMs);
                qualityLocalShadowCacheTimes.push_back(qualityLocalShadowCacheMs);
                qualityFogTimes.push_back(qualityFogMs);qualityCloudTimes.push_back(qualityCloudMs);
                if(qualityProbeTimes.size()>6000) {
                    qualityProbeTimes.erase(qualityProbeTimes.begin(),qualityProbeTimes.begin()+3000);
                    qualityCloudShadowTimes.erase(qualityCloudShadowTimes.begin(),qualityCloudShadowTimes.begin()+3000);
                    qualityFogTimes.erase(qualityFogTimes.begin(),qualityFogTimes.begin()+3000);
                    qualityCloudTimes.erase(qualityCloudTimes.begin(),qualityCloudTimes.begin()+3000);
                    qualityLocalShadowCacheTimes.erase(qualityLocalShadowCacheTimes.begin(),qualityLocalShadowCacheTimes.begin()+3000);
                }
            }
        }
        D3D12_RANGE noWrite{0, 0};
        timestampReadback->Unmap(0, &noWrite);
    }
    timestampPending[index] = false;
}

} // namespace nativegpu

namespace nativegpu {

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() { shutdown(); delete impl_; }

bool Renderer::configureMaterials(std::vector<TextureUpload> textures,
                                  std::vector<MaterialUpload> materials,
                                  std::string& error) {
    error.clear();
    auto& state = *impl_;
    state.configuredTextures.clear();
    state.configuredMaterials.clear();
    state.materialPath = false;
    state.materialBindless = false;
    if (textures.empty() || materials.empty()) {
        if (!textures.empty() || !materials.empty()) {
            error = "Materials require both a texture set and a material table";
            return false;
        }
        return true;
    }
    if (textures.size() > detail::MaxCookedTextures) {
        error = "Cooked texture count exceeds the shader-visible heap budget";
        return false;
    }
    auto mipCount = [](unsigned width, unsigned height) {
        unsigned count = 1;
        while (width > 1 || height > 1) {
            width = std::max(1u, width >> 1);
            height = std::max(1u, height >> 1);
            ++count;
        }
        return count;
    };
    auto mipBytes = [](unsigned width, unsigned height, unsigned format) -> size_t {
        if (format == 0) return static_cast<size_t>(width) * height * 4;
        if (format == 5) return static_cast<size_t>(width) * height * 2;
        const size_t blockBytes = (format == 1) ? 8 : 16;
        const size_t blocksX = (width + 3) / 4;
        const size_t blocksY = (height + 3) / 4;
        return blocksX * blocksY * blockBytes;
    };
    for (const TextureUpload& texture : textures) {
        if (!texture.width || !texture.height || !texture.mips) {
            error = "Texture has zero dimensions or mip count";
            return false;
        }
        if (texture.format > 5) {
            error = "Texture format is outside the supported set";
            return false;
        }
        if (texture.mips != mipCount(texture.width, texture.height)) {
            error = "Texture mip count does not match its dimensions";
            return false;
        }
        size_t expectedBytes = 0;
        for (unsigned mip = 0; mip < texture.mips; ++mip) {
            expectedBytes += mipBytes(std::max(1u, texture.width >> mip),
                                      std::max(1u, texture.height >> mip), texture.format);
        }
        if (texture.pixels.size() != expectedBytes) {
            error = "Texture payload size does not match its format and mip chain";
            return false;
        }
    }
    for (MaterialUpload& material : materials) {
        if(material.layers.size()>4){error="NG-E-MAT-LAYER: at most four layers are supported";return false;}
        if(!material.layers.empty()){
            if(material.alphaMasked||material.alphaBlended||material.unlit){error="NG-E-MAT-LAYER: layered materials currently require an opaque lit surface";return false;}
            const auto& base=material.layers.front();
            if(base.source!=0||base.flags!=0||base.weight!=1||base.gain!=1||base.bias!=0){error="NG-E-MAT-LAYER: base layer must have full constant coverage";return false;}
            for(const auto& layer:material.layers){
                const auto valid=[](float v,float low,float high){return std::isfinite(v)&&v>=low&&v<=high;};
                if(layer.source>2||(layer.flags&~7u)||!valid(layer.weight,0,1)||!valid(layer.gain,0,1000)||!valid(layer.bias,-1000,1000)||!valid(layer.width,.0001f,1000)||!valid(layer.heightBias,-1000,1000)||!valid(layer.roughness,0,1)||!valid(layer.metallic,0,1)||!valid(layer.normalStrength,0,8)||!valid(layer.occlusionStrength,0,1)||!valid(layer.emissiveStrength,0,1000000)||!valid(layer.heightScale,-1000,1000)||!valid(layer.heightOffset,-1000,1000)||std::any_of(layer.baseColor,layer.baseColor+3,[&](float v){return !valid(v,0,1);})||std::any_of(layer.emissiveFactor,layer.emissiveFactor+3,[&](float v){return !valid(v,0,1);})){error="NG-E-MAT-LAYER: invalid layer parameters";return false;}
                if(layer.source==1&&layer.slots[8].texture==0xffffffffu){error="NG-E-MAT-LAYER: texture mask is missing";return false;}
                for(unsigned slotIndex=0;slotIndex<9;++slotIndex){const auto& slot=layer.slots[slotIndex];
                    if(slot.uvSet>7||slot.channel>3||slot.sampler>108||(slot.flags&~1u)||std::any_of(slot.row0,slot.row0+3,[&](float v){return !valid(v,-1000000,1000000);})||std::any_of(slot.row1,slot.row1+3,[&](float v){return !valid(v,-1000000,1000000);})||(slot.texture!=0xffffffffu&&slot.texture>=textures.size())){error="NG-E-MAT-LAYER: invalid texture slot or UV set outside [0, 7]";return false;}
                    if(slot.texture!=0xffffffffu){const auto& texture=textures[slot.texture];const bool color=slotIndex==0||slotIndex==6;if(texture.srgb!=color||(slotIndex==1&&((slot.flags&1u)!=0)!=(texture.format==3))||(slotIndex!=1&&slot.flags)){error="NG-E-MAT-LAYER: texture color space or normal format is invalid";return false;}}
                }
            }
            // Stream requirement bits used by submit(): 1 = extra texture UVs
            // (sets 1-4), 2 = extra layer UVs (sets 5-7).
            for(const auto& layer:material.layers)for(const auto& slot:layer.slots){if(slot.uvSet>=1&&slot.uvSet<=4)material.uvStreamMask|=1u;else if(slot.uvSet>=5)material.uvStreamMask|=2u;}
            if(material.hasSurfaceRelief){
                const auto& relief=material.surfaceRelief;
                const auto valid=[](float v,float low,float high){return std::isfinite(v)&&v>=low&&v<=high;};
                const unsigned mode=relief.flags&3u;
                if(mode==0||mode>2||(relief.flags&~31u)||relief.channel>3||relief.uvSet>7||relief.sampler>108||!valid(relief.amplitudeMetres,0,1)||!valid(relief.referenceHeight,0,1)||!valid(relief.minSteps,1,128)||!valid(relief.maxSteps,1,256)||relief.maxSteps<relief.minSteps||!valid(relief.refineSteps,0,8)||!valid(relief.fadeStartMetres,0,1000)||!valid(relief.fadeEndMetres,0,1000)||relief.fadeEndMetres<relief.fadeStartMetres||!valid(relief.grazingFade,0,1)||!valid(relief.shadowSteps,1,32)){error="NG-E-MAT-RELIEF: invalid surface relief parameters";return false;}
                if((relief.flags&(1u<<4))!=0){
                    if(relief.texture>=textures.size()){error="NG-E-MAT-RELIEF: relief texture is missing";return false;}
                    const auto& texture=textures[relief.texture];
                    if(texture.srgb||(texture.format!=5&&texture.format!=0)){error="NG-E-MAT-RELIEF: relief height texture must be linear R16 or RGBA8";return false;}
                }else{
                    if(relief.heightLayer>=material.layers.size()){error="NG-E-MAT-RELIEF: relief height layer is out of range";return false;}
                    if(material.layers[relief.heightLayer].slots[7].texture==0xffffffffu){error="NG-E-MAT-RELIEF: relief height layer has no height texture";return false;}
                }
                if(relief.uvSet>=1&&relief.uvSet<=4)material.uvStreamMask|=1u;else if(relief.uvSet>=5)material.uvStreamMask|=2u;
            }
        }
        if(!engine::validMaterialIor(material.ior)){error="NG-E-MAT-IOR: ior must be 0 or a finite number at least 1";return false;}
        if((material.hasCoat&&(material.coat>=textures.size()||textures[material.coat].srgb))||(material.hasCoatRoughness&&(material.coatRoughness>=textures.size()||textures[material.coatRoughness].srgb))||(material.hasCoatNormal&&((material.coatNormal&0x7FFFFFFFu)>=textures.size()||textures[material.coatNormal&0x7FFFFFFFu].srgb))){error="NG-E-MAT-COAT: coating requires valid linear textures";return false;}
        const unsigned normalIndex = material.normal & 0x7FFFFFFFu;
        if (material.albedo >= textures.size() || normalIndex >= textures.size() ||
            material.orm >= textures.size() || (material.hasEmissive&&material.emissive>=textures.size()) || (material.hasOcclusion&&material.occlusion>=textures.size())) {
            error = "Material references a texture outside the cooked set";
            return false;
        }
        if (!(material.uvScale > 0.0f) || material.uvScale > 1000.0f) {
            error = "Material uvScale is outside the supported range";
            return false;
        }
        if(!std::isfinite(material.alphaCutoff)||material.alphaCutoff<0||material.alphaCutoff>1) {
            error="NG-E-MAT-ALPHA: cutoff must be in [0,1]";return false;
        }
        if(!std::isfinite(material.emissiveStrength)||material.emissiveStrength<0||material.emissiveStrength>1000000||
            std::any_of(material.emissiveFactor,material.emissiveFactor+3,[](float value){return !std::isfinite(value)||value<0||value>1;})){
            error="NG-E-MAT-EMISSION: invalid factor or strength";return false;
        }
        if(material.hasEmissive&&!textures[material.emissive].srgb){error="NG-E-MAT-EMISSION: emissive texture must use sRGB";return false;}
        if(!std::isfinite(material.occlusionStrength)||material.occlusionStrength<0||material.occlusionStrength>1||(material.hasOcclusion&&textures[material.occlusion].srgb)){error="NG-E-MAT-AO: invalid strength or non-linear data";return false;}
        if(material.alphaMasked&&material.alphaBlended){error="NG-E-MAT-ALPHA: MASK and BLEND are mutually exclusive";return false;}
        if(!std::isfinite(material.opacity)||material.opacity<0||material.opacity>1){error="NG-E-MAT-ALPHA: opacity must be in [0,1]";return false;}
        for(const auto& mapping:material.textureMappings){if(mapping.sampler>108||std::any_of(mapping.row0,mapping.row0+3,[](float value){return !std::isfinite(value)||std::abs(value)>1000000;})||std::any_of(mapping.row1,mapping.row1+3,[](float value){return !std::isfinite(value)||std::abs(value)>1000000;})){error="NG-E-MAT-UV: invalid texture mapping";return false;}}
    }
    state.configuredTextures = std::move(textures);
    state.configuredMaterials = std::move(materials);
    // Keep material texture indices relative. The bindless path converts them to heap
    // indices when it builds the GpuMaterial record buffer; the array fallback uses them
    // directly as slice indices.
    state.materialPath = true;
    state.materialTextureWidth = state.configuredTextures.front().width;
    state.materialTextureHeight = state.configuredTextures.front().height;
    state.materialTextureMips = state.configuredTextures.front().mips;
    state.materialTextureSlices = static_cast<unsigned>(state.configuredTextures.size());
    return true;
}

void Renderer::clearMaterials() {
    auto& state = *impl_;
    for (ID3D12Resource*& texture : state.cookedTextures) state.destroy(texture);
    state.cookedTextures.clear();
    state.configuredTextures.clear();
    state.configuredMaterials.clear();
    state.materialPath = false;
    state.materialBindless = false;
    state.materialTextureWidth = 0;
    state.materialTextureHeight = 0;
    state.materialTextureMips = 0;
    state.materialTextureSlices = 0;
}

bool Renderer::materialsConfigured() const { return impl_->materialPath; }

unsigned Renderer::materialCount() const {
    return static_cast<unsigned>(impl_->configuredMaterials.size());
}

bool Renderer::configureConvexMeshes(std::vector<engine::ConvexMesh> meshes,
                                     std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (meshes.size() > 4096) {
        error = "NG-E-CONVEX-BUDGET: too many convex meshes";
        return false;
    }
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        const auto& mesh = meshes[index];
        // A cut-face mesh is an open shell, so "closed" is not a requirement here: only
        // that the triangle list is well formed and every index resolves.
        if (mesh.vertices.size() < 3 || mesh.indices.size() < 3 || mesh.indices.size() % 3 != 0) {
            error = "NG-E-CONVEX-MESH: shard " + std::to_string(index) +
                    " has no usable triangle list";
            return false;
        }
        for (std::uint32_t vertex : mesh.indices) {
            if (vertex >= mesh.vertices.size()) {
                error = "NG-E-CONVEX-MESH: shard " + std::to_string(index) +
                        " references a vertex out of range";
                return false;
            }
        }
        for (const auto& vertex : mesh.vertices) {
            for (float value : vertex.position) {
                if (!std::isfinite(value)) {
                    error = "NG-E-CONVEX-MESH: non-finite vertex in shard " + std::to_string(index);
                    return false;
                }
            }
        }
    }
    state.configuredConvexMeshes = std::move(meshes);
    // Before initialize() the upload is deferred; afterwards it has to happen now because
    // the draw path reads the buffer pair directly.
    if (!state.initialized) {
        state.convexMeshesReady = false;
        state.convexMeshBytes = 0;
        return true;
    }
    return state.uploadConvexMeshes(error);
}

unsigned Renderer::convexMeshCount() const {
    return static_cast<unsigned>(impl_->convexRanges.size());
}

unsigned Renderer::convexMeshBytes() const { return impl_->convexMeshBytes; }

bool Renderer::configureVegetation(std::vector<VegetationLayer> layers, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (layers.empty()) {
        if (state.initialized && !state.waitIdle(error)) return false;
        for (auto& layer : state.vegetation) if (layer.draw) layer.draw->shutdown(error);
        state.vegetation.clear();
        return true;
    }
    if (!state.initialized) {
        error = "NG-E-VEGETATION-INIT: configure scene vegetation after renderer initialization";
        return false;
    }
    if (!state.waitIdle(error)) return false;
    std::vector<Impl::VegetationRuntimeLayer> next;
    next.reserve(layers.size());
    const auto submit = [&]() -> bool {
        if (!checked(state.commands->Close(), "Close vegetation upload", error)) return false;
        ID3D12CommandList* lists[] = {state.commands};
        state.queue->ExecuteCommandLists(1, lists);
        return state.waitIdle(error);
    };
    for (auto& source : layers) {
        Impl::VegetationRuntimeLayer target;
        target.material = source.material;
        std::copy(source.bounds, source.bounds + 3, target.bounds);
        const unsigned capacity = std::max(256u, static_cast<unsigned>(source.instances.size()) * 2u + 32u);
        target.draw = std::make_unique<VegetationDraw>(state.device, state.fence,
            detail::SceneColorFormat, detail::DepthDsvFormat, capacity);
        if (!checked(state.allocators[0]->Reset(), "Reset vegetation texture upload", error) ||
            !checked(state.commands->Reset(state.allocators[0], nullptr), "Reset vegetation texture commands", error) ||
            !target.draw->setTextures(state.commands, source.textures, error) || !submit()) return false;
        if (!checked(state.allocators[0]->Reset(), "Reset vegetation page upload", error) ||
            !checked(state.commands->Reset(state.allocators[0], nullptr), "Reset vegetation page commands", error) ||
            !target.draw->upload(state.commands, source.mesh, source.instances, source.page, error) || !submit()) return false;
        next.push_back(std::move(target));
    }
    for (auto& layer : state.vegetation) if (layer.draw) {
        std::string ignored;
        layer.draw->shutdown(ignored);
    }
    state.vegetation = std::move(next);
    return true;
}

unsigned Renderer::vegetationLayerCount() const {
    return static_cast<unsigned>(impl_->vegetation.size());
}

bool Renderer::configureHair(HairSetup setup, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (setup.pack.strandCount == 0) {
        if (state.initialized && !state.waitIdle(error)) return false;
        if (state.hair) {
            std::string ignored;
            state.hair->draw->shutdown(ignored);
        }
        state.hair.reset();
        return true;
    }
    if (!state.initialized) {
        error = "NG-E-HAIR-INIT: configure scene hair after renderer initialization";
        return false;
    }
    if (!state.allocators[0] || !state.commands || !state.queue || !state.fence) {
        error = "NG-E-HAIR-INIT: the renderer upload path is unavailable";
        return false;
    }
    if (!state.waitIdle(error)) return false;
    try {
        auto runtime = std::make_unique<Impl::HairRuntime>();
        runtime->material = setup.material;
        runtime->shadowStrength = std::clamp(setup.shadowStrength, 0.0f, 1.0f);
        runtime->shadowSigmaPerMetre = std::max(0.0f, setup.shadowSigmaPerMetre);
        runtime->shadowDensityScale = std::max(0.0f, setup.shadowDensityScale);
        runtime->lightIntensityScale = std::max(0.0f, setup.lightIntensityScale);
        runtime->debugView = std::clamp(setup.debugView, 0, 7);
        runtime->simulation = setup.simulation;
        runtime->draw = std::make_unique<HairDraw>(state.device, state.fence,
            detail::SceneColorFormat, detail::DepthDsvFormat);
        if (!runtime->draw->setSimulation(runtime->simulation, error)) return false;
        if (!checked(state.allocators[0]->Reset(), "Reset hair upload", error) ||
            !checked(state.commands->Reset(state.allocators[0], nullptr), "Reset hair upload commands", error))
            return false;
        // The upload and the frame submit share one fence ticket: HairDraw stores it with
        // its staging buffers, so a ticket that is never signalled would keep them alive.
        const UINT64 fence = state.nextFence;
        if (!runtime->draw->upload(state.commands, fence, setup.pack, error)) return false;
        if (!checked(state.commands->Close(), "Close hair upload", error)) return false;
        ID3D12CommandList* lists[] = {state.commands};
        state.queue->ExecuteCommandLists(1, lists);
        if (!checked(state.queue->Signal(state.fence, fence), "Signal hair upload", error)) return false;
        state.nextFence = fence + 1;
        if (!state.waitFor(fence, error)) return false;
        if (state.hair) {
            std::string ignored;
            state.hair->draw->shutdown(ignored);
        }
        state.hair = std::move(runtime);
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        return false;
    }
}

void Renderer::clearHair() {
    std::string ignored;
    HairSetup empty;
    configureHair(std::move(empty), ignored);
}

bool Renderer::hairConfigured() const { return impl_ && impl_->hair != nullptr; }

HairDrawStats Renderer::hairStats() const {
    return impl_ && impl_->hair ? impl_->hair->draw->stats() : HairDrawStats{};
}

void Renderer::setHairHeadTransform(const Mat4& transform) {
    if (impl_) impl_->hairHeadTransform = transform;
}

void Renderer::setEnvironment(const Environment& environment) {
    impl_->environment = environment;
    if(environment.panorama)impl_->environment.quality=std::max(1u,environment.quality);
    impl_->qualityEnvironmentDirty = true;
    impl_->qualityHistoryValid = false;
    impl_->qualityError.clear();
    impl_->qualityProbeTimes.clear();impl_->qualityCloudShadowTimes.clear();
    impl_->qualityFogTimes.clear();impl_->qualityCloudTimes.clear();
    impl_->qualityLocalShadowCacheTimes.clear();
}

Environment Renderer::environment() const { return impl_->environment; }

bool Renderer::initialize(HWND window, unsigned width, unsigned height, std::string& error) {
    error.clear();
    shutdown();
    if (!window || !IsWindow(window) || width == 0 || height == 0) {
        error = "A valid window and nonzero render dimensions are required";
        return false;
    }
    auto& state = *impl_;
    state.width = width;
    state.height = height;

    if (state.validationRequested) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
    if (!checked(CreateDXGIFactory1(IID_PPV_ARGS(&state.factory)), "Create DXGI factory", error)) {
        return false;
    }
    state.track(state.factory);
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        HRESULT hr = state.factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { detail::release(candidate); continue; }
        DXGI_ADAPTER_DESC1 description{};
        candidate->GetDesc1(&description);
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&state.device)))) {
            state.adapter = candidate;
            state.adapterDescription = description;
            break;
        }
        detail::release(candidate);
    }
    if (!state.device) {
        if (!checked(state.factory->EnumWarpAdapter(IID_PPV_ARGS(&state.adapter)),
                     "Find WARP software adapter", error) ||
            !checked(D3D12CreateDevice(state.adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&state.device)),
                     "Create D3D12 device", error)) {
            return false;
        }
        state.adapter->GetDesc1(&state.adapterDescription);
    }
    state.track(state.adapter);
    state.track(state.device);
    if (state.validationRequested &&
        SUCCEEDED(state.device->QueryInterface(IID_PPV_ARGS(&state.infoQueue)))) {
        state.track(state.infoQueue);
        state.infoQueue->SetMuteDebugOutput(FALSE);
    }
    state.device->QueryInterface(IID_PPV_ARGS(&state.device5));
    state.track(state.device5);

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!checked(state.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&state.queue)),
                 "Create graphics queue", error)) {
        return false;
    }
    state.track(state.queue);
    if (!checked(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)),
                 "Create GPU fence", error)) {
        return false;
    }
    state.track(state.fence);
    state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.fenceEvent) { error = "Create GPU fence event failed"; return false; }

    DXGI_SWAP_CHAIN_DESC1 swapDescription{};
    swapDescription.Width = width;
    swapDescription.Height = height;
    swapDescription.Format = detail::ColorFormat;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = detail::FrameCount;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* baseSwapChain = nullptr;
    HRESULT hr = state.factory->CreateSwapChainForHwnd(state.queue, window, &swapDescription,
                                                      nullptr, nullptr, &baseSwapChain);
    if (!checked(hr, "Create swap chain", error)) return false;
    hr = baseSwapChain->QueryInterface(IID_PPV_ARGS(&state.swapChain));
    detail::release(baseSwapChain);
    if (!checked(hr, "Query swap chain interface", error)) return false;
    state.track(state.swapChain);
    state.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    // One extra slot holds the half-resolution volume target for the temporal path.
    heapDescription.NumDescriptors = detail::RtvTotalSlots;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.rtvHeap)),
                 "Create RTV heap", error)) {
        return false;
    }
    state.track(state.rtvHeap);
    heapDescription.NumDescriptors = detail::FrameCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.dsvHeap)),
                 "Create DSV heap", error)) {
        return false;
    }
    state.track(state.dsvHeap);
    heapDescription.NumDescriptors = detail::DescriptorHeapSlots;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.srvHeap)),
                 "Create shader-visible descriptor heap", error)) {
        return false;
    }
    state.track(state.srvHeap);
    heapDescription.NumDescriptors=109;heapDescription.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if(!checked(state.device->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&state.materialSamplerHeap)),"Create material sampler heap",error))return false;
    state.track(state.materialSamplerHeap);
    const unsigned samplerStride=state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const D3D12_TEXTURE_ADDRESS_MODE address[]={D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    for(unsigned index=0;index<109;index++){
        D3D12_SAMPLER_DESC descriptor{};descriptor.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;descriptor.MaxLOD=D3D12_FLOAT32_MAX;descriptor.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        if(index==0){descriptor.Filter=D3D12_FILTER_ANISOTROPIC;descriptor.MaxAnisotropy=8;descriptor.AddressU=descriptor.AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP;}
        else{const unsigned packed=index-1,wrap=packed/12,min=(packed%12)/2,mag=packed%2;descriptor.Filter=static_cast<D3D12_FILTER>((min%2)*16+mag*4+(min>=4?1:0));descriptor.MaxAnisotropy=1;descriptor.AddressU=address[wrap/3];descriptor.AddressV=address[wrap%3];if(min<2)descriptor.MaxLOD=0;}
        auto handle=state.materialSamplerHeap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=static_cast<SIZE_T>(index)*samplerStride;state.device->CreateSampler(&descriptor,handle);
    }
    state.rtvStride = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    state.srvStride = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!state.createTargets(error)) return false;
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!checked(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                     IID_PPV_ARGS(&state.allocators[index])), "Create command allocator", error)) {
            return false;
        }
        state.track(state.allocators[index]);
    }
    if (!checked(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                 state.allocators[0], nullptr, IID_PPV_ARGS(&state.commands)),
                 "Create command list", error) ||
        !checked(state.commands->Close(), "Close initial command list", error)) {
        return false;
    }
    state.track(state.commands);
    state.commands->QueryInterface(IID_PPV_ARGS(&state.commands4));
    state.track(state.commands4);
    state.commands->QueryInterface(IID_PPV_ARGS(&state.commands6));
    state.track(state.commands6);
    state.device->QueryInterface(IID_PPV_ARGS(&state.device2));
    state.track(state.device2);

    if (!state.createRasterRootSignature(error) ||
        !state.createCompositeRootSignature(error) ||
        !state.createRasterPipeline(error) ||
        !state.createTexturedPipeline(error)) {
        return false;
    }
    // The bindless path is optional: if SM 6.6 or the DXC blob is unavailable, the
    // array-based textured pipeline above remains the fallback.
    if (state.materialPath) {
        std::string bindlessError;
        if (!state.createBindlessPipeline(bindlessError)) {
            state.materialBindless = false;
            state.addDiagnostic("NG-E-MAT-BINDLESS", bindlessError);
        }
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.alphaMasked;})) {
            error="NG-E-MAT-ALPHA: masked materials require the SM 6.6 material pipeline";return false;
        }
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return !material.layers.empty();})){error="NG-E-MAT-LAYER: layered materials require the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.hasEmissive;})){
            error="NG-E-MAT-EMISSION: independent emission requires the SM 6.6 material pipeline";return false;
        }
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.hasTextureMappings;})){error="NG-E-MAT-UV: texture mappings require the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.hasOcclusion;})){error="NG-E-MAT-AO: independent occlusion requires the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.unlit;})){error="NG-E-MAT-UNLIT: unlit requires the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.ior!=1.5f;})){error="NG-E-MAT-IOR: custom ior requires the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.hasCoat||material.hasCoatRoughness||material.hasCoatNormal;})){error="NG-E-MAT-COAT: coating maps require the SM 6.6 material pipeline";return false;}
        if(!state.materialBindless&&std::any_of(state.configuredMaterials.begin(),state.configuredMaterials.end(),[](const MaterialUpload& material){return material.alphaBlended;})){error="NG-E-MAT-ALPHA: BLEND requires the SM 6.6 material pipeline";return false;}
    }
    if (!state.createCompositePipeline(error) ||
        !state.uploadScene(error) ||
        !state.uploadConvexMeshes(error)) {
        return false;
    }
    if (!state.createBuffer(detail::SceneConstantStride*detail::FrameCount, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ,
                            &state.sceneConstantBuffer, error)) {
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    if (!checked(state.sceneConstantBuffer->Map(0, &noRead,
                 reinterpret_cast<void**>(&state.sceneConstantMapped)),
                 "Map scene constants", error)) {
        return false;
    }
    std::memset(state.sceneConstantMapped, 0, detail::SceneConstantStride*detail::FrameCount);
    if (!state.createBuffer(detail::CompositeConstantStride*detail::FrameCount, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ,
                            &state.compositeConstantBuffer, error)) {
        return false;
    }
    if (!checked(state.compositeConstantBuffer->Map(0, &noRead,
                 reinterpret_cast<void**>(&state.compositeConstantMapped)),
                 "Map composite constants", error)) {
        return false;
    }
    std::memset(state.compositeConstantMapped, 0, detail::CompositeConstantStride*detail::FrameCount);

    state.queryCapabilities();
    state.queryVideoMemory();
    {
        std::string materialError;
        if (!state.selfTestMaterialTable(materialError)) {
            state.addDiagnostic("NG-E-MAT-TABLE", materialError);
        }
    }
    state.buildAccelerationStructures();
    state.createTimestamps();
    // A failure to build the DXR state object is not fatal: the raster path keeps
    // rendering and every report records fallbackUsed=true with the error text.
    std::string rtError;
    if (!state.buildRaytracingPipeline(rtError)) {
        state.rtError = rtError.empty() ? "DXRT state object creation failed" : rtError;
    }
    // Mesh shader pipeline: a failure keeps the vertex path and is reported.
    std::string meshBuildError;
    if (!state.buildMeshPipeline(meshBuildError)) {
        state.meshError = meshBuildError.empty() ? "mesh pipeline creation failed"
                                                 : meshBuildError;
    }
    // Compute queue and probe: optional. Failure keeps the renderer usable and is reported
    // through probes.compute.error instead of failing initialization.
    std::string computeBuildError;
    if (!state.createComputePipeline(computeBuildError)) {
        state.computeError = computeBuildError.empty() ? "compute pipeline creation failed"
                                                       : computeBuildError;
    }
    // Particle field: requested before initialize(). A failure keeps the renderer usable
    // and is reported through probes.particles.error instead of failing initialization.
    if (state.particlesRequested) {
        std::string particleBuildError;
        if (!state.createParticles(particleBuildError)) {
            state.particleError = particleBuildError.empty() ? "particle pipeline creation failed"
                                                             : particleBuildError;
        }
    }
    if (!state.configuredEffectTextures.empty() && !state.createEffectSprites(error)) return false;
    if (!state.createGpuFields(error)) return false;
    if (!state.createGpuEffects(error)) return false;
    if (!state.createGpuCloth(error)) return false;
    state.initialized = true;
    return true;
}

bool Renderer::resize(unsigned width, unsigned height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    // Minimized windows have no drawable area. The host resumes after a nonzero resize.
    if (!width || !height || (width == state.width && height == state.height)) return true;
    if (!state.waitIdle(error)) return false;
    state.releaseQuality();
    state.releaseTargets();
    if (!checked(state.swapChain->ResizeBuffers(detail::FrameCount, width, height,
                                                detail::ColorFormat, 0),
                 "Resize swap chain", error)) {
        return false;
    }
    state.width = width;
    state.height = height;
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        state.frameFences[index] = 0;
        state.timestampPending[index] = false;
    }
    state.frameStats.gpuValid = false;
    state.frameStats.gpuRtValid = false;
    return state.createTargets(error);
}

bool Renderer::render(const Mat4& viewProjection, const float cameraPosition[3],
                      const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                      std::string& error) {
    error.clear();
    const auto start = std::chrono::steady_clock::now();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    const UINT index = state.swapChain->GetCurrentBackBufferIndex();
    state.lastFrameIndex = index;
    // Fault injection for the device-removal test. This is an injected fault, not a real
    // hardware reset, and every report says so.
    if (state.faultInjectionFrame != 0 && state.framesRendered + 1 == state.faultInjectionFrame) {
        state.deviceRemovalInjected = true;
        error = "DXGI_ERROR_DEVICE_REMOVED (injected fault at frame " +
                std::to_string(state.framesRendered + 1) + ")";
        state.addDiagnostic("NG-E-DEVICE-REMOVED", error);
        return false;
    }
    if (!state.waitFor(state.frameFences[index], error)) return false;
    if(!state.collectTransparency(error))return false;
    if(!state.prepareEnvironmentTexture(error))return false;
    const auto blended=[&](const DrawItem& item){if(state.environment.panorama&&item.material<0)return false;const auto index=item.material<0||static_cast<std::size_t>(item.material)>=state.configuredMaterials.size()?0u:static_cast<unsigned>(item.material);return state.materialBindless&&!state.configuredMaterials.empty()&&state.configuredMaterials[index].alphaBlended;};
    // The mesh-shader demo path has its own single-texture pixel shader; it cannot
    // evaluate a layered material, so such frames must stay on the raster path.
    const auto layered=[&](const DrawItem& item){return item.material>=0&&static_cast<std::size_t>(item.material)<state.configuredMaterials.size()&&!state.configuredMaterials[static_cast<std::size_t>(item.material)].layers.empty();};
    // Layer slots name compiled vertex streams. A mesh without those streams must be
    // rejected here instead of sampling zeros; there is no silent UV0 fallback.
    for(const auto& item:items){
        if(item.material<0||static_cast<std::size_t>(item.material)>=state.configuredMaterials.size())continue;
        const unsigned mask=state.configuredMaterials[static_cast<std::size_t>(item.material)].uvStreamMask;if(!mask)continue;
        const bool meshInRange=item.mesh>=0&&static_cast<std::size_t>(item.mesh)<state.convexRanges.size();
        const bool textureStreams=item.skin?item.skin->hasTextureUvs:(meshInRange&&state.convexRanges[item.mesh].hasTextureUvs);
        const bool layerStreams=item.skin?item.skin->hasLayerUvs:(meshInRange&&state.convexRanges[item.mesh].hasLayerUvs);
        if(((mask&1u)&&!textureStreams)||((mask&2u)&&!layerStreams)){
            error="NG-E-MAT-LAYER: material '"+state.configuredMaterials[static_cast<std::size_t>(item.material)].name+"' selects mesh UV streams this draw does not carry";
            state.addDiagnostic("NG-E-MAT-LAYER",error);return false;
        }
    }
    const bool hasTransparency=std::any_of(items.begin(),items.end(),blended);
    if(hasTransparency&&!state.createTransparency(error))return false;
    state.transparencyActive=hasTransparency;state.transparencyDraws=0;
    state.readTimestamp(index);
    if(state.qualityReady && state.environment.quality &&
       state.qualityGrid[0] != (state.environment.quality>1?160u:80u)) {
        if(!state.waitIdle(error))return false;
        state.releaseQuality();
    }
    if (state.environment.quality && !state.qualityReady && state.qualityError.empty()) {
        if (!state.createQuality(state.qualityError)) {
            state.releaseQuality();
            state.addDiagnostic("NG-E-LIGHTING-QUALITY", state.qualityError);
        }
    }
    const bool qualityActive = state.environment.quality && state.qualityReady;
    if(state.environment.panorama&&!qualityActive){error="NG-E-ASSET-ENVIRONMENT-GPU: "+state.qualityError;return false;}
    if (!checked(state.allocators[index]->Reset(), "Reset frame allocator", error) ||
        !checked(state.commands->Reset(state.allocators[index], state.rasterPipeline),
                 "Reset frame commands", error)) {
        return false;
    }
    auto* list = state.commands;
    auto* list4 = state.commands4;
    if(state.timestampsAvailable){
        list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampFrameBegin);
        list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuEffectsBegin);
    }
    state.recordEnvironmentTexture(list);
    if (!state.recordGpuFields(list,error)) return false;
    if (!state.recordGpuEffects(list,viewProjection,error)) return false;
    if (!state.prepareSmokeMedia(list,index,cameraPosition,error)) return false;
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuEffectsEnd);
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuClothBegin);
    if(!state.recordGpuCloth(list,error))return false;
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuClothEnd);
    const unsigned descriptorBase =
        detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;

    // 1. Rebuild this frame's TLAS from the rigid instance transforms. Static hard
    //    shadows: one shadow ray per pixel, no skinning, no denoising.
    std::vector<DrawItem> rigidItems;
    std::vector<const DrawItem*> skinBindings;
    std::vector<unsigned> skinSlots(items.size(),detail::MaxSkinInstances);
    std::size_t itemIndex=0;
    for(const auto& item:items) {
        const auto slotIndex=itemIndex++;if(!item.skin){if(!blended(item))rigidItems.push_back(item);continue;}
        auto match=std::find_if(skinBindings.begin(),skinBindings.end(),[&](const DrawItem* binding){return binding->skin==item.skin&&binding->palette==item.palette&&binding->morphWeights==item.morphWeights&&binding->deformation==item.deformation;});
        if(match==skinBindings.end()){if(skinBindings.size()>=detail::MaxSkinInstances){error="NG-E-ANIM-BUDGET: at most 64 independent skin bindings per frame";return false;}skinBindings.push_back(&item);match=skinBindings.end()-1;}
        skinSlots[slotIndex]=static_cast<unsigned>(match-skinBindings.begin());
    }
    state.skinBindingsLastFrame=static_cast<unsigned>(skinBindings.size());state.skinBindingsHighWater=std::max(state.skinBindingsHighWater,state.skinBindingsLastFrame);
    bool tlasReady = false;
    if ((state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated) || qualityActive) {
        tlasReady = state.updateTopLevelAccelerationStructure(list4, rigidItems, index);
    }

    // Particle field (docs/FIELD-PLAN.md): simulate and splat on the compute queue before
    // this frame's graphics list is submitted. The graphics queue waits on the particle
    // fence, so the resolved volume is complete when the volumetric pass reads it.
    if (state.particlesRequested && state.particlesReady) {
        std::string particleDispatchError;
        if (!state.dispatchParticles(index, 1.0f / 60.0f, particleDispatchError)) {
            ++state.particleDispatchFailures;
            state.particleError = particleDispatchError;
            state.addDiagnostic("NG-E-PARTICLE-DISPATCH", particleDispatchError);
        }
    }

    // Compute probe (B1/B3): dispatched on the compute queue before this frame's list is
    // submitted; the graphics queue waits on the compute fence. The one-shot readback runs
    // after a fixed number of dispatches so the pipeline has actually been exercised.
    if (state.computeRequested && state.computePipelineReady) {
        if (!state.dispatchComputeProbe(lights, error)) return false;
        if (!state.computeProbeRan && state.computeDispatchedFrames >= 30) {
            if (!state.readbackComputeProbe(error)) return false;
        }
    }

    // 2. Frame-wide transitions.
    state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
    state.transitionTo(state.sceneBaseColor[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    state.transitionTo(state.sceneKeyLight[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    state.transitionTo(state.shadowMask[index], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, list);
    if (qualityActive && !state.prepareQuality(index, viewProjection, cameraPosition,
                                               rigidItems, lights, tlasReady, error)) return false;
    if (!qualityActive && state.timestampsAvailable) {
        for (unsigned timestamp=detail::TimestampQualityBegin; timestamp<=detail::TimestampLocalShadowCacheEnd; ++timestamp)
            list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+timestamp);
    }

    // 3. Raster pass into two offscreen targets (surface, key-light only).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
    auto rtvHandle = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(detail::RtvSlotBase +
                                         index * detail::RtvSlotsPerFrame) * state.rtvStride;
    rtvHandles[0] = rtvHandle;
    rtvHandle.ptr += state.rtvStride;
    rtvHandles[1] = rtvHandle;
    auto dsv = state.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(index) *
               state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    list->OMSetRenderTargets(2, rtvHandles, FALSE, &dsv);
    const float skyLegacy[4] = {0.055f, 0.075f, 0.11f, 1.0f};
    const float skyMaterials[4] = {state.environment.skyColor[0],
                                   state.environment.skyColor[1],
                                   state.environment.skyColor[2], 1.0f};
    const float* sky = state.materialPath ? skyMaterials : skyLegacy;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtvHandles[0], sky, 0, nullptr);
    list->ClearRenderTargetView(rtvHandles[1], zero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(state.width),
                            static_cast<float>(state.height), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {state.srvHeap,state.materialSamplerHeap};
    list->SetDescriptorHeaps(2, heaps);
    if(hasTransparency){
        const auto oit=detail::TransparencyDescriptorBase+index*detail::TransparencyDescriptorStride;
        state.transitionTo(state.transparencyHeads,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);state.transitionTo(state.transparencyNodes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);state.transitionTo(state.transparencyCounter,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
        const UINT empty[4]={0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu},clear[4]{};
        auto clearCpu=state.transparencyCpuHeap->GetCPUDescriptorHandleForHeapStart();
        list->ClearUnorderedAccessViewUint(state.gpuDescriptor(oit),clearCpu,state.transparencyHeads.resource,empty,0,nullptr);clearCpu.ptr+=state.srvStride;
        list->ClearUnorderedAccessViewUint(state.gpuDescriptor(oit+2),clearCpu,state.transparencyCounter.resource,clear,0,nullptr);clearCpu.ptr+=state.srvStride;
        list->ClearUnorderedAccessViewUint(state.gpuDescriptor(oit+4),clearCpu,state.transparencyCoverage.resource,clear,0,nullptr);
        for(auto* resource:{state.transparencyHeads.resource,state.transparencyCoverage.resource,state.transparencyCounter.resource}){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=resource;list->ResourceBarrier(1,&barrier);}
        if(tlasReady){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.RaytracingAccelerationStructure.Location=state.tlas[index]->GetGPUVirtualAddress();state.device->CreateShaderResourceView(nullptr,&view,state.cpuDescriptor(oit+3));}
    }

    detail::SceneConstants sceneConstants{};
    sceneConstants.qualitySlot = qualityActive ? detail::QualityDescriptorBase +
                                               detail::QConstants + index : 0;
    sceneConstants.transparencyBase=detail::TransparencyDescriptorBase+index*detail::TransparencyDescriptorStride;
    sceneConstants.transparencyCapacity=state.transparencyCapacity|((hasTransparency&&tlasReady&&state.rtRequested&&state.raytracingTier>=11)?0x80000000u:0u);
    for (int channel = 0; channel < 3; ++channel) {
        sceneConstants.ambient[channel] = state.environment.ambient[channel];
    }
    sceneConstants.ambient[3] = 1.0f;
    // See SceneConstants::keyTint: the legacy shader multiplies its baked 0.95 key term by
    // this, so the default environment reproduces the old image exactly.
    for (int channel = 0; channel < 3; ++channel) {
        sceneConstants.keyTint[channel] =
            state.environment.keyColor[channel] * (state.environment.keyIntensity / 1.30f);
    }
    const unsigned gpuLightCount =
        std::min<unsigned>(64, static_cast<unsigned>(lights.size()));
    sceneConstants.gpuLightCount = gpuLightCount;
    // The legacy shader declares fixed [4] arrays and loops lightCount; keep its count
    // capped so a 64-light scene cannot read past its cbuffer.
    sceneConstants.lightCount = std::min<unsigned>(4, gpuLightCount);
    for (unsigned light = 0; light < gpuLightCount; ++light) {
        const nativegpu::LightDesc& source = lights[light];
        detail::GpuLight& target = sceneConstants.lights[light];
        std::memcpy(target.positionRange, source.position, sizeof(source.position));
        target.positionRange[3] = std::max(source.range, 0.001f);
        std::memcpy(target.colorIntensity, source.color, sizeof(source.color));
        target.colorIntensity[3] = std::max(source.intensity, 0.0f);
        std::memcpy(target.directionCos, source.direction, sizeof(source.direction));
        target.directionCos[3] = source.cosOuter;
        target.params[0] = static_cast<float>(source.type);
        target.params[1] = source.cosInner;
        target.params[2] = 0.0f;
        target.params[3] = 0.0f;
        if (light < 4) {
            std::memcpy(sceneConstants.lightPositionRange[light], source.position,
                        sizeof(source.position));
            sceneConstants.lightPositionRange[light][3] = std::max(source.range, 0.001f);
            std::memcpy(sceneConstants.lightColorIntensity[light], source.color,
                        sizeof(source.color));
            sceneConstants.lightColorIntensity[light][3] = std::max(source.intensity, 0.0f);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        sceneConstants.keyDirectionIntensity[axis] = state.environment.keyDirection[axis];
        sceneConstants.keyColor[axis] = state.environment.keyColor[axis];
        sceneConstants.fogColor[axis] = state.environment.fogColor[axis];
        sceneConstants.cameraPosition[axis] = cameraPosition[axis];
    }
    sceneConstants.keyDirectionIntensity[3] = state.environment.keyIntensity;
    sceneConstants.keyColor[3] = 1.0f;
    sceneConstants.fogParams[0] = state.environment.fogDensity;
    sceneConstants.fogParams[1] = state.environment.fogHeightFalloff;
    sceneConstants.fogParams[2] = state.environment.fogBaseHeight;
    sceneConstants.fogParams[3] = 0.0f;
    sceneConstants.fogColor[3] = 1.0f;
    sceneConstants.cameraPosition[3] = 1.0f;
    sceneConstants.layerDebugMode = state.layerDebugMode;
    sceneConstants.layerDebugLayer = state.layerDebugLayer;
    std::memcpy(reinterpret_cast<unsigned char*>(state.sceneConstantMapped)+index*detail::SceneConstantStride,
                &sceneConstants, sizeof(sceneConstants));

    // Mesh shader path or the traditional vertex path for this frame. The mesh path is
    // validated against the legacy bounded table; cooked materials stay on the raster path
    // until the meshlet winding/culling issue is fixed.
    bool usedMeshPath = false;
    if (!state.environment.panorama&&!hasTransparency&&state.meshRequested && state.meshPipelineCreated &&
        std::none_of(rigidItems.begin(),rigidItems.end(),[&](const DrawItem& item){return item.mesh>=0||layered(item);})) {
        usedMeshPath = state.renderMeshPath(state.commands4, rigidItems, viewProjection, cameraPosition);
    }
    state.meshPathUsedLastFrame = usedMeshPath;
    if (usedMeshPath) {
        ++state.meshFrames;
        state.drawCallsLastFrame = static_cast<unsigned>(rigidItems.size());
        state.trianglesLastFrame = static_cast<unsigned long long>(state.meshletDrawnLastFrame) * 2ull;
    }
    if (!usedMeshPath && state.meshRequested) ++state.meshFallbackFrames;
    {
    list->SetPipelineState(state.rasterPipeline);
    list->SetGraphicsRootSignature(state.rasterRootSignature);
    list->SetGraphicsRootDescriptorTable(2, state.gpuDescriptor(detail::SlotMaterialTable));
    list->SetGraphicsRootConstantBufferView(1, state.sceneConstantBuffer->GetGPUVirtualAddress()+
                                             index*detail::SceneConstantStride);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &state.vertexView);
    list->IASetIndexBuffer(&state.indexView);
    unsigned draws = usedMeshPath?state.drawCallsLastFrame:0;
    unsigned long long triangles = usedMeshPath?state.trianglesLastFrame:0;
    // View-frustum culling for the raster path. The DXR TLAS above was built from every
    // static item on purpose: an occluder outside the camera frustum can still cast a
    // shadow into it, so culling casters would create popping shadows.
    const Frustum frameFrustum = frustumFromViewProjection(viewProjection);
    state.submittedLastFrame = static_cast<unsigned>(items.size());
    state.culledLastFrame = 0;
    // Batching plan for the same list. Kept next to the culling counters so the report
    // answers both questions from one frame: how much is off screen, and how many draws
    // grouping by material would remove.
    {
        std::vector<BatchInput> batchInputs;
        batchInputs.reserve(items.size());
        for (const auto& item : items) {
            BatchInput input;
            input.model = item.model.m;
            input.color = item.color;
            input.material = item.material;
            input.mesh = item.mesh;
            input.firstIndex = item.firstIndex;
            input.indexCount = item.indexCount;
            input.roughness = item.roughness;
            input.metalness = item.metalness;
            input.transparent = blended(item);
            input.skinned = item.skin != nullptr;
            batchInputs.push_back(input);
        }
        planBatches(batchInputs, state.batchPlan);
        state.plannedBatchesLastFrame = static_cast<unsigned>(state.batchPlan.batches.size());
        state.plannedDrawsLastFrame = state.plannedBatchesLastFrame + state.batchPlan.skinnedItems +
                                      state.batchPlan.transparentItems;
    }
    std::vector<bool> preparedSkins(skinBindings.size(),false);std::size_t drawItemIndex=0;
    for(unsigned pass=0;pass<(hasTransparency?2u:1u);pass++){
    const bool transparent=pass==1;drawItemIndex=0;
    if(transparent)list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    for (const auto& item : items) {
        const auto originalOrder=drawItemIndex;const auto binding=skinSlots[drawItemIndex++];
        if(blended(item)!=transparent)continue;
        if(usedMeshPath&&!item.skin)continue;
        // Skinned items are never culled: bind-pose bounds do not bound the deformed
        // mesh, and a limb leaving its bind box is exactly the case that would pop.
        if(state.frustumCull&&!item.skin){
            float localMinimum[3]={-0.5f,-0.5f,-0.5f};
            float localMaximum[3]={0.5f,0.5f,0.5f};
            bool bounded=true;
            if(item.mesh>=0){
                const auto meshIndex=static_cast<std::size_t>(item.mesh);
                if(meshIndex<state.convexRanges.size()){
                    const auto& range=state.convexRanges[meshIndex];
                    std::memcpy(localMinimum,range.boundsMin,sizeof(localMinimum));
                    std::memcpy(localMaximum,range.boundsMax,sizeof(localMaximum));
                }else bounded=false;
            }
            if(bounded){
                float worldMinimum[3],worldMaximum[3];
                transformAabb(item.model,localMinimum,localMaximum,worldMinimum,worldMaximum);
                if(aabbOutsideFrustum(frameFrustum,worldMinimum,worldMaximum)){++state.culledLastFrame;continue;}
            }
        }
        const bool inlineLegacy=state.environment.panorama&&(!state.materialPath||item.material<0);
        unsigned indexCount=36;
        if(item.skin){
            const auto skinIndex=index*detail::MaxSkinInstances+binding;
            if(!preparedSkins[binding]){if(!state.prepareSkin(skinIndex,item,error))return false;preparedSkins[binding]=true;}
            const auto& geometry=state.skinGeometry[state.skinGeometrySlot[skinIndex]];
            D3D12_VERTEX_BUFFER_VIEW vertexView{geometry.vertices->GetGPUVirtualAddress(),static_cast<UINT>(item.skin->vertices.size()*sizeof(engine::animation::SkinVertex)),sizeof(engine::animation::SkinVertex)};
            D3D12_INDEX_BUFFER_VIEW indexView{geometry.indices->GetGPUVirtualAddress(),static_cast<UINT>(item.skin->indices.size()*4),DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0,1,&vertexView);list->IASetIndexBuffer(&indexView);
            list->SetGraphicsRootConstantBufferView(3,state.skinPalette[skinIndex]->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(4,state.gpuDescriptor(detail::SkinDeformSrvBase+skinIndex*2));
            list->SetPipelineState(inlineLegacy?state.inlineEnvironmentSkinPipeline:transparent?state.transparentSkinPipeline:state.materialBindless ? state.bindlessSkinPipeline : state.skinPipeline);
            indexCount=static_cast<unsigned>(item.skin->indices.size());++state.skinDraws;
            if(!item.skin->morphs.empty()&&item.morphWeights)++state.morphDraws;if(item.deformation)++state.softDraws;
        }else{
            list->SetPipelineState(inlineLegacy?state.inlineEnvironmentPipeline:transparent?state.transparentPipeline:state.materialPath
                ? (state.materialBindless ? state.bindlessPipeline : state.texturedPipeline)
                : state.rasterPipeline);
            const int mesh = item.mesh;
            if (mesh >= 0 && state.convexMeshesReady &&
                static_cast<std::size_t>(mesh) < state.convexRanges.size()) {
                // Convex shard: one buffer pair holds every mesh, so only the byte
                // offsets change between draws.
                const auto& range = state.convexRanges[static_cast<std::size_t>(mesh)];
                D3D12_VERTEX_BUFFER_VIEW vertexView{
                    state.convexVertexBuffer->GetGPUVirtualAddress() + range.vertexOffset,
                    range.vertexBytes, sizeof(engine::ConvexVertex)};
                D3D12_INDEX_BUFFER_VIEW indexView{
                    state.convexIndexBuffer->GetGPUVirtualAddress() + range.indexOffset,
                    static_cast<UINT>(range.indexCount * sizeof(std::uint32_t)),
                    DXGI_FORMAT_R32_UINT};
                list->IASetVertexBuffers(0, 1, &vertexView);
                list->IASetIndexBuffer(&indexView);
                indexCount = range.indexCount;
            } else {
                list->IASetVertexBuffers(0,1,&state.vertexView);list->IASetIndexBuffer(&state.indexView);
            }
        }
        detail::DrawConstants constants{};
        constants.model = item.model;
        constants.viewProjection = viewProjection;
        constants.normalMatrix = normalTransform(item.model);
        std::memcpy(constants.color, item.color, sizeof(constants.color));
        constants.roughness = std::clamp(item.roughness, 0.0f, 1.0f);
        constants.metalness = std::clamp(item.metalness, 0.0f, 1.0f);
        if (state.materialPath&&!inlineLegacy) {
            // Cooked path: slice indices come from the scene material table. An invalid
            // index falls back to slice 0 (the scene's default material) and is reported.
            unsigned material = item.material < 0 ? 0u : static_cast<unsigned>(item.material);
            if (material >= state.configuredMaterials.size()) {
                state.addDiagnostic("NG-E-MAT-INDEX",
                                    "material index " + std::to_string(material) +
                                        " resolved to the default material");
                material = 0;
            }
            const MaterialUpload& entry = state.configuredMaterials[material];
            if (state.materialBindless) {
                // Bindless path: albedoIndex is a GpuMaterial record index. The record
                // carries heap texture indices, uvScale, roughness/metalness and surface
                // history, replacing the old bit-packed albedoIndex.
                constants.albedoIndex = material;
            } else {
                constants.albedoIndex = entry.albedo;
            }
            constants.normalIndex = (item.skin && !state.materialBindless) ? 0u : entry.normal;
            constants.ormIndex = (item.skin && !state.materialBindless) ? 0u : entry.orm;
            constants.uvScale = entry.uvScale;
            if(item.mesh >= 0 && state.materialBindless) constants.normalIndex |= 0x40000000u;
            if(state.materialBindless&&((item.skin&&item.skin->hasTextureUvs)||(!item.skin&&item.mesh>=0&&static_cast<std::size_t>(item.mesh)<state.convexRanges.size()&&state.convexRanges[item.mesh].hasTextureUvs)))constants.normalIndex|=0x20000000u;
            if(state.materialBindless&&((item.skin&&item.skin->hasLayerUvs)||(!item.skin&&item.mesh>=0&&static_cast<std::size_t>(item.mesh)<state.convexRanges.size()&&state.convexRanges[item.mesh].hasLayerUvs)))constants.normalIndex|=0x10000000u;
            if(transparent)constants.ormIndex=static_cast<unsigned>(originalOrder);
            else if(state.materialBindless){
                // Bindless opaque draws read ORM from the material record, so the root
                // constant is free. 0xFFFFFFFF keeps the authored wetness; otherwise the
                // bits carry the runtime wetness of this entity.
                constants.ormIndex=0xFFFFFFFFu;
                if(item.wetness>=0.0f){
                    const float value=std::clamp(item.wetness,0.0f,1.0f);
                    std::memcpy(&constants.ormIndex,&value,sizeof(value));
                }
            }
        } else {
            // Material slot policy: resolve through the generation-checked table. A missing
            // sentinel or a stale generation falls back to the default slot with a stable code.
            unsigned textureIndex = detail::MaterialDefaultSlot;
            unsigned generation = 0;
            if (item.texture == 0xFFFFFFFFu) {
                state.materials.missingFallbacks++;
                state.addDiagnostic("NG-E-MAT-MISSING",
                                    "missing material texture resolved to the default slot");
            } else if (!state.materials.resolve(item.texture, item.textureGeneration, textureIndex,
                                                generation)) {
                state.addDiagnostic("NG-E-MAT-STALE",
                                    "material slot " + std::to_string(item.texture) +
                                        " rejected: released, out of range or stale generation");
            }
            constants.albedoIndex = textureIndex;
            constants.normalIndex = 0;
            constants.ormIndex = 0;
            constants.uvScale = 1.0f;
        }
        list->SetGraphicsRoot32BitConstants(0, 58, &constants, 0);
        if(item.indexCount) {
            if(item.firstIndex>indexCount||item.indexCount>indexCount-item.firstIndex||item.indexCount%3!=0){error="NG-E-ANIM-SKIN: invalid draw index range";return false;}
            indexCount=item.indexCount;
        } else if(item.firstIndex){error="NG-E-ANIM-SKIN: an index offset requires a count";return false;}
        list->DrawIndexedInstanced(indexCount, 1, item.firstIndex, 0, 0);
        triangles+=indexCount/3;
        ++draws;
        if(transparent)++state.transparencyDraws;
    }
    }
    if(hasTransparency){list->OMSetRenderTargets(2,rtvHandles,FALSE,&dsv);state.transitionTo(state.transparencyHeads,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,list);state.transitionTo(state.transparencyNodes,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,list);}
    state.drawCallsLastFrame = draws;
    state.trianglesLastFrame = triangles;
    }

    // Opaque cloth writes the same MRT/depth before volumes and transparency.
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuClothDrawBegin);
    if(!state.drawGpuCloth(list,viewProjection,cameraPosition,error))return false;
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuClothDrawEnd);

    // Resident vegetation shares the scene's linear HDR target and depth. It is therefore
    // composed, fogged and captured by exactly the same final pass as the rest of the map;
    // no second swap chain, overlay window or reference image is involved.
    if(!state.vegetation.empty()) {
        state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
        list->OMSetRenderTargets(1, &rtvHandles[0], FALSE, &dsv);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        for(auto& layer:state.vegetation) {
            VegetationFrameInput vegetationFrame{};
            vegetationFrame.viewProjection=viewProjection;
            std::copy(cameraPosition,cameraPosition+3,vegetationFrame.camera);
            vegetationFrame.seconds=static_cast<float>(state.framesRendered)/60.0f;
            vegetationFrame.viewportHeight=static_cast<float>(state.height);
            vegetationFrame.tanHalfFov=std::tan(1.05f*0.5f);
            vegetationFrame.maximumDistance=1000.0f;
            vegetationFrame.alphaCutoff=layer.material.alphaCutoff;
            vegetationFrame.lightIntensity=state.environment.keyIntensity;
            vegetationFrame.ambient=(state.environment.ambient[0]+state.environment.ambient[1]+state.environment.ambient[2])/3.0f;
            std::copy(state.environment.keyDirection,state.environment.keyDirection+3,vegetationFrame.lightDirection);
            std::copy(state.environment.keyColor,state.environment.keyColor+3,vegetationFrame.lightColor);
            vegetationFrame.shadowStrength=0.0f; // Scene shadows remain owned by the scene passes.
            if(!layer.draw->draw(list,state.nextFence,vegetationFrame,layer.material,layer.bounds,error)) {
                error="NG-E-VEGETATION-DRAW: "+error;
                return false;
            }
        }
    }

    // The resident hairstyle shares the scene's linear HDR target and depth, so it is
    // occluded by the character and lit by the same environment as everything else.
    if (state.hair) {
        state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
        list->OMSetRenderTargets(1, &rtvHandles[0], FALSE, &dsv);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        HairFrameInput hairFrame{};
        hairFrame.viewProjection = viewProjection;
        std::copy(cameraPosition, cameraPosition + 3, hairFrame.camera);
        hairFrame.viewportWidth = static_cast<float>(state.width);
        hairFrame.viewportHeight = static_cast<float>(state.height);
        // Perspective projection is column-major: m[5] is 1 / tan(fovY/2) and
        // m[14] / m[10] is -near. Reading them keeps the coverage law tied to the
        // actual camera instead of assuming the scene's default field of view.
        const float projectionScale = viewProjection.m[5];
        hairFrame.tanHalfFovY = projectionScale > 1e-6f ? 1.0f / projectionScale : std::tan(1.05f * 0.5f);
        const float nearFromProjection = viewProjection.m[10] != 0.0f
            ? -viewProjection.m[14] / viewProjection.m[10] : 0.1f;
        hairFrame.nearMetres = std::max(0.001f, nearFromProjection);
        hairFrame.debugView = state.hair->debugView;
        hairFrame.headTransform = state.hairHeadTransform;
        // The scene does not carry a frame delta through render(); the visual tick is
        // fixed at 60 Hz and the solver splits it into its own substeps. That is what the
        // plan's "fixed timestep" requirement means for this pass, and it keeps two runs
        // of the same frames identical.
        hairFrame.deltaSeconds = 1.0f / 60.0f;
        HairLighting hairLighting{};
        std::copy(state.environment.keyDirection, state.environment.keyDirection + 3, hairLighting.direction);
        std::copy(state.environment.keyColor, state.environment.keyColor + 3, hairLighting.colour);
        std::copy(state.environment.ambient, state.environment.ambient + 3, hairLighting.ambient);
        hairLighting.intensity = state.environment.keyIntensity * state.hair->lightIntensityScale;
        hairLighting.shadowStrength = state.hair->shadowStrength;
        hairLighting.shadowSigmaPerMetre = state.hair->shadowSigmaPerMetre;
        hairLighting.shadowDensityScale = state.hair->shadowDensityScale;
        if (!state.hair->draw->draw(list, state.nextFence, hairFrame, state.hair->material, hairLighting, error)) {
            error = "NG-E-HAIR-DRAW: " + error;
            return false;
        }
    }

    // 3b. Volumetric field composite and transparent effect sprites are recorded after
    // the water pass (see below): the surface pass owns an opaque snapshot of the base
    // colour and paints over whatever was already there, so drawing particles first made
    // every droplet above a pond disappear behind the surface that is *behind* it.
    // 4. DXR static shadow pass, or the raster fallback with a full visibility mask.
    state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, list);
    bool dispatched = false;
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampRtBegin);
    if (tlasReady && state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated) {
        std::string rtDispatchError;
        dispatched = state.dispatchShadowRays(list4, index, viewProjection, cameraPosition,
                                              rtDispatchError);
        if (!dispatched) {
            state.rtError = rtDispatchError;
            ++state.rtDispatchFailures;
        }
    }
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampRtEnd);
    if (!dispatched) {
        const float fullVisibility[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        auto clearCpu=state.shadowClearHeap->GetCPUDescriptorHandleForHeapStart();clearCpu.ptr+=index*state.srvStride;
        list->ClearUnorderedAccessViewFloat(
            state.gpuDescriptor(descriptorBase + detail::FdsShadowMaskUav),
            clearCpu,
            state.shadowMask[index].resource, fullVisibility, 0, nullptr);
        ++state.rtFallbackFrames;
    }
    state.rtDispatchedLastFrame = dispatched;
    if (dispatched) ++state.rtDispatchedFrames;

    if (qualityActive) {
        state.transitionTo(state.depth[index],static_cast<D3D12_RESOURCE_STATES>(
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),list);
        state.dispatchQualityVolumes(index);
    }
    // Water consumes an immutable opaque HDR/depth snapshot, then writes a
    // fully lit dielectric surface before atmospheric fog and display mapping.
    if(!state.waterSurface.vertices.empty()){
        try{
            if(!state.waterRipple)state.waterRipple=std::make_unique<WaterRipple>(state.device);
            if(!state.waterDraw)state.waterDraw=std::make_unique<WaterDraw>(state.device);
        }
        catch(const std::exception& e){error=e.what();return false;}
        // The residual solver runs before the water draw so the draw pass samples a
        // field that already contains this frame's interactions.
        unsigned injected=0;
        if(!state.waterRipple->update(list,index,state.waterSurface,injected,error))return false;
        state.waterRippleEvents+=injected;
        state.transitionTo(state.sceneBaseColor[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
        state.transitionTo(state.sceneKeyLight[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
        state.transitionTo(state.depth[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
        state.transitionTo(state.shadowMask[index],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,list);
        auto waterRtv=state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        waterRtv.ptr+=static_cast<SIZE_T>(detail::RtvSlotBase+index*detail::RtvSlotsPerFrame)*state.rtvStride;
        auto waterDsv=state.dsvHeap->GetCPUDescriptorHandleForHeapStart();waterDsv.ptr+=index*state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        list->OMSetRenderTargets(2,&waterRtv,TRUE,&waterDsv);
        WaterLighting light;
        for(unsigned i=0;i<3;++i){light.camera[i]=cameraPosition[i];light.keyDirection[i]=state.environment.keyDirection[i];light.keyColor[i]=state.environment.keyColor[i];light.sky[i]=state.environment.skyColor[i];}
        light.keyIntensity=state.environment.keyIntensity;
        if(!state.waterDraw->record(list,index,state.width,state.height,state.sceneBaseColor[index].resource,state.sceneKeyLight[index].resource,state.depth[index].resource,state.shadowMask[index].resource,state.waterRipple->result(),state.waterSurface,viewProjection,light,error))return false;
        state.sceneBaseColor[index].state=state.sceneKeyLight[index].state=D3D12_RESOURCE_STATE_RENDER_TARGET;
        state.depth[index].state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // The upstream body is a full second surface. It shares the same camera and light
        // but owns its residual field and draw instance, so neither body can reuse the
        // other's depth, constants or event cursor.
        if(!state.upperWaterSurface.vertices.empty()){
            if(!state.upperWaterRipple)state.upperWaterRipple=std::make_unique<WaterRipple>(state.device);
            if(!state.upperWaterDraw)state.upperWaterDraw=std::make_unique<WaterDraw>(state.device);
            unsigned upperInjected=0;
            if(!state.upperWaterRipple->update(list,index,state.upperWaterSurface,upperInjected,error))return false;
            state.upperWaterRippleEvents+=upperInjected;
            state.transitionTo(state.sceneBaseColor[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
            state.transitionTo(state.sceneKeyLight[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
            state.transitionTo(state.depth[index],D3D12_RESOURCE_STATE_COPY_SOURCE,list);
            state.transitionTo(state.shadowMask[index],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,list);
            list->OMSetRenderTargets(2,&waterRtv,TRUE,&waterDsv);
            if(!state.upperWaterDraw->record(list,index,state.width,state.height,state.sceneBaseColor[index].resource,state.sceneKeyLight[index].resource,state.depth[index].resource,state.shadowMask[index].resource,state.upperWaterRipple->result(),state.upperWaterSurface,viewProjection,light,error))return false;
            state.sceneBaseColor[index].state=state.sceneKeyLight[index].state=D3D12_RESOURCE_STATE_RENDER_TARGET;
            state.depth[index].state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        ID3D12DescriptorHeap* sceneHeap[]{state.srvHeap};list->SetDescriptorHeaps(1,sceneHeap);
    }
    // 4a-2. Falling sheets. They are translucent and sit between the two bodies, so they
    // are recorded after both water surfaces but before particles.
    if(!state.waterfalls.empty()){
        if(!state.waterfallDraw)state.waterfallDraw=std::make_unique<WaterfallDraw>(state.device);
        auto sheetRtv=state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        sheetRtv.ptr+=static_cast<SIZE_T>(detail::RtvSlotBase+index*detail::RtvSlotsPerFrame)*state.rtvStride;
        auto sheetDsv=state.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        sheetDsv.ptr+=index*state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        state.transitionTo(state.depth[index],D3D12_RESOURCE_STATE_DEPTH_READ,list);
        if(!state.waterfallDraw->record(list,index,state.waterfalls,viewProjection,sheetRtv,sheetDsv,error))return false;
        state.transitionTo(state.depth[index],D3D12_RESOURCE_STATE_DEPTH_WRITE,list);
        ID3D12DescriptorHeap* sheetHeap[]{state.srvHeap};list->SetDescriptorHeaps(1,sheetHeap);
    }
    // 4b. Transparent geometry. Order matters: the water pass above paints a lit surface
    // over the base colour where the water surface is visible, so anything translucent
    // drawn before it is erased even when the droplet sits in front of the water. These
    // passes are recorded after the surface, which is also the physically correct order
    // for particles travelling above it.
    if (state.particlesRequested && state.particlesReady) {
        state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, list);
        state.drawParticles(list, viewProjection, cameraPosition, index);
    }
    if (!state.effectSprites.empty()) {
        state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
        if (!state.drawEffectSprites(list, index, viewProjection, cameraPosition, error)) return false;
    }
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuEffectDrawBegin);
    if (state.gpuEffectSprites) {
        state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
        if (!state.drawGpuEffects(list,index,error)) return false;
    }
    if(state.timestampsAvailable)list->EndQuery(state.timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,index*detail::TimestampsPerFrame+detail::TimestampGpuEffectDrawEnd);
    // 5. Composite: base + key light * visibility, then copy to the swap chain.
    state.transitionTo(state.depth[index],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,list);
    state.transitionTo(state.shadowMask[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, list);
    state.transitionTo(state.sceneBaseColor[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       list);
    state.transitionTo(state.sceneKeyLight[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       list);
    state.transitionTo(state.composite[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    D3D12_CPU_DESCRIPTOR_HANDLE compositeRtv = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    compositeRtv.ptr += static_cast<SIZE_T>(detail::RtvSlotBase +
                                            index * detail::RtvSlotsPerFrame + 2) *
                        state.rtvStride;
    list->OMSetRenderTargets(1, &compositeRtv, FALSE, nullptr);
    detail::CompositeSceneConstants compositeScene{};compositeScene.smoke=state.smokeScene;
    Mat4 inverseViewProjection{};
    if (!invertMatrix(viewProjection, inverseViewProjection)) {
        inverseViewProjection = Mat4{};
        inverseViewProjection.m[0] = inverseViewProjection.m[5] =
            inverseViewProjection.m[10] = inverseViewProjection.m[15] = 1.0f;
    }
    compositeScene.invViewProjection = inverseViewProjection;
    for (int axis = 0; axis < 3; ++axis) {
        compositeScene.cameraPosition[axis] = cameraPosition[axis];
        compositeScene.fogColor[axis] = state.environment.fogColor[axis];
        compositeScene.moonDirection[axis] = state.environment.keyDirection[axis];
        compositeScene.moonColor[axis] = state.environment.keyColor[axis];
    }
    compositeScene.cameraPosition[3] = 1.0f;
    compositeScene.fogParams[0] = state.environment.fogDensity;
    compositeScene.fogParams[1] = state.environment.fogHeightFalloff;
    compositeScene.fogParams[2] = state.environment.fogBaseHeight;
    compositeScene.fogParams[3] = 200.0f;
    compositeScene.fogColor[3] = 1.0f;
    compositeScene.moonDirection[3] = state.environment.keyIntensity;
    compositeScene.moonColor[3] = 1.0f;
    compositeScene.cloudParams[0] = static_cast<float>(state.framesRendered) * state.environment.cloudSpeed;
    compositeScene.cloudParams[1] = state.environment.cloudCoverage;
    compositeScene.cloudParams[2] = state.environment.cloudHeight;
    compositeScene.cloudParams[3] = state.environment.cloudThickness;
    unsigned fogLightCount = 0;
    for (const LightDesc& source : lights) {
        if (source.type != 1 || fogLightCount >= detail::CompositeMaxLights) continue;
        detail::CompositeLight& target = compositeScene.lights[fogLightCount++];
        std::memcpy(target.positionRange, source.position, sizeof(source.position));
        target.positionRange[3] = std::max(source.range, 0.001f);
        std::memcpy(target.colorIntensity, source.color, sizeof(source.color));
        target.colorIntensity[3] = std::max(source.intensity, 0.0f);
        std::memcpy(target.directionCos, source.direction, sizeof(source.direction));
        target.directionCos[3] = source.cosOuter;
        target.params[0] = static_cast<float>(source.type);
        target.params[1] = source.cosInner;
        target.params[2] = 0.0f;
        target.params[3] = 0.0f;
    }
    compositeScene.lightCount = fogLightCount;
    std::memcpy(reinterpret_cast<unsigned char*>(state.compositeConstantMapped)+index*detail::CompositeConstantStride,
                &compositeScene, sizeof(compositeScene));
    const float compositeClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    list->ClearRenderTargetView(compositeRtv, compositeClear, 0, nullptr);
    // The command list reset bound the raster pipeline; the composite needs its own.
    list->SetPipelineState(state.compositePipeline);
    list->SetGraphicsRootSignature(state.compositeRootSignature);
    list->SetGraphicsRootDescriptorTable(0, state.gpuDescriptor(descriptorBase +
                                                               detail::FdsBaseColor));
    list->SetGraphicsRootConstantBufferView(
        2, state.compositeConstantBuffer->GetGPUVirtualAddress()+index*detail::CompositeConstantStride);
    list->SetGraphicsRootDescriptorTable(
        3, state.gpuDescriptor(descriptorBase + detail::FdsDepthComposite));
    list->SetGraphicsRootDescriptorTable(4,state.gpuDescriptor(detail::SmokeDescriptorBase+index*detail::MaxSmokeMedia));
    list->SetGraphicsRoot32BitConstant(1, state.compositeDebugView, 0);
    const UINT compositeToneMap = state.environment.toneMap ? 1u : 0u;
    list->SetGraphicsRoot32BitConstant(1, compositeToneMap, 1);
    const FLOAT compositeExposure = state.environment.exposure;
    list->SetGraphicsRoot32BitConstant(1, *reinterpret_cast<const UINT*>(&compositeExposure), 2);
    const UINT compositeVolumetric = (state.environment.volumetric ? 1u : 0u)|(hasTransparency?2u:0u)|(state.smokeScene.info[0]?4u:0u);
    list->SetGraphicsRoot32BitConstant(1, compositeVolumetric, 3);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (qualityActive) state.compositeQuality(index);
    else list->DrawInstanced(3, 1, 0, 0);
    state.finishSmokeMedia(list);

    for(const auto& layer:state.overlay)if(!layer.rectangles.empty())
        list->ClearRenderTargetView(compositeRtv,layer.color,static_cast<UINT>(layer.rectangles.size()),layer.rectangles.data());
    if(hasTransparency){state.transitionTo(state.transparencyCounter,D3D12_RESOURCE_STATE_COPY_SOURCE,list);list->CopyBufferRegion(state.transparencyReadback,index*16,state.transparencyCounter.resource,0,16);state.transparencyPending[index]=true;}

    state.transitionTo(state.composite[index], D3D12_RESOURCE_STATE_COPY_SOURCE, list);
    auto toCopyDest = detail::transitionBarrier(state.renderTargets[index],
                                                D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopyDest);
    list->CopyResource(state.renderTargets[index], state.composite[index].resource);
    auto toPresent = detail::transitionBarrier(state.renderTargets[index],
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &toPresent);

    if (state.captureRequested) {
        state.smokeCaptureSources=state.smokeFrameSources;
        if (!state.recordCaptureCopy(list, index, error)) return false;
    }

    if (state.timestampsAvailable) {
        list->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                       index * detail::TimestampsPerFrame + detail::TimestampFrameEnd);
        list->ResolveQueryData(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                               index * detail::TimestampsPerFrame, detail::TimestampsPerFrame,
                               state.timestampReadback,
                               index * detail::TimestampsPerFrame * sizeof(UINT64));
    }
    if (!checked(list->Close(), "Close frame commands", error)) return false;
    state.frameStats.cpuSubmitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ID3D12CommandList* lists[] = {list};
    state.queue->ExecuteCommandLists(1, lists);
    const HRESULT presentResult = state.swapChain->Present(1, 0);
    const UINT64 fenceValue = state.nextFence++;
    if (!checked(state.queue->Signal(state.fence, fenceValue), "Signal frame fence", error)) {
        return false;
    }
    state.frameFences[index] = fenceValue;
    state.timestampPending[index] = state.timestampsAvailable;
    if (!checked(presentResult, "Present frame", error)) {
        error += "; device status " + detail::hresultText(state.device->GetDeviceRemovedReason());
        return false;
    }
    ++state.framesRendered;
    state.frameStats.cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if(state.validationRequested)state.collectValidationMessages("frame");
    return true;
}

// Prepares a screenshot: allocates the readback target and asks the next render() to
// record the copy inside the frame submission.
bool Renderer::requestScreenshot(const std::string& path, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    if (path.empty()) { error = "Screenshot path is empty"; return false; }
    if (state.width == 0 || state.height == 0) { error = "Renderer has no drawable size"; return false; }
    D3D12_RESOURCE_DESC description = detail::textureDescription2D(
        state.width, state.height, detail::ColorFormat, D3D12_RESOURCE_FLAG_NONE);
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    state.device->GetCopyableFootprints(&description, 0, 1, 0, &state.captureFootprint, &rows,
                                        &rowBytes, &totalBytes);
    if (!state.captureReadback || state.captureReadbackBytes < totalBytes ||
        state.captureWidth != state.width || state.captureHeight != state.height) {
        state.destroy(state.captureReadback);
        if (!state.createBuffer(totalBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST, &state.captureReadback, error)) {
            state.captureReadbackBytes = 0;
            state.lastScreenshotError = error;
            return false;
        }
        state.captureReadbackBytes = totalBytes;
        state.captureWidth = state.width;
        state.captureHeight = state.height;
    }
    state.capturePath = path;
    state.captureRequested = true;
    state.captureRecorded = false;
    return true;
}

bool Renderer::Impl::recordCaptureCopy(ID3D12GraphicsCommandList* list, unsigned frameIndex,
                                       std::string& error) {
    if (!captureReadback) { error = "Screenshot readback target is missing"; return false; }
    // The composite target is already in COPY_SOURCE from the frame graph above.
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = captureReadback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = captureFootprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = composite[frameIndex].resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    captureRequested = false;
    captureRecorded = true;
    return true;
}

// Reads back the copy recorded by the frame and writes the PNG.
bool Renderer::captureScreenshot(const std::string& path, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    if (!state.captureRecorded) {
        error = "No screenshot was recorded by the frame; call requestScreenshot() first";
        state.lastScreenshotError = error;
        return false;
    }
    if (!state.waitIdle(error)) {
        state.lastScreenshotError = error;
        return false;
    }
    const std::string target = path.empty() ? state.capturePath : path;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(state.captureReadbackBytes)};
    void* mapped = nullptr;
    if (FAILED(state.captureReadback->Map(0, &readRange, &mapped))) {
        error = "Map screenshot readback failed";
        state.lastScreenshotError = error;
        return false;
    }
    const auto* pixels = static_cast<const unsigned char*>(mapped) + state.captureFootprint.Offset;
    const std::size_t rowPitch = static_cast<std::size_t>(state.captureFootprint.Footprint.RowPitch);
    unsigned long long total = 0;
    unsigned long long count = 0;
    unsigned char lowest = 255;
    unsigned char highest = 0;
    for (unsigned row = 0; row < state.captureHeight; ++row) {
        const unsigned char* line = pixels + static_cast<std::size_t>(row) * rowPitch;
        for (unsigned column = 0; column < state.captureWidth; ++column) {
            const unsigned char value = line[static_cast<std::size_t>(column) * 4];
            total += value;
            ++count;
            if (value < lowest) lowest = value;
            if (value > highest) highest = value;
        }
    }
    state.lastScreenshotBytes = state.captureReadbackBytes;
    state.lastScreenshotMean =
        count ? static_cast<double>(total) / static_cast<double>(count) : 0.0;
    state.lastScreenshotMin = lowest;
    state.lastScreenshotMax = highest;
    const bool ok = writePng(target, state.captureWidth, state.captureHeight, pixels, rowPitch,
                             error);
    D3D12_RANGE noWrite{0, 0};
    state.captureReadback->Unmap(0, &noWrite);
    state.collectValidationMessages("screenshot");
    state.captureRecorded = false;
    if (ok) {
        ++state.screenshotsCaptured;
        state.lastScreenshotPath = target;
    } else {
        state.lastScreenshotError = error;
    }
    return ok;
}
void Renderer::setMeshShaders(bool enabled) {
    auto& state = *impl_;
    state.meshRequested = enabled;
    if (enabled && !state.meshPipelineCreated && state.initialized && state.device2) {
        std::string error;
        if (!state.buildMeshPipeline(error)) {
            state.meshError = error.empty() ? "mesh pipeline creation failed" : error;
        }
    }
}

bool Renderer::meshShadersEnabled() const {
    const auto& state = *impl_;
    return state.meshRequested && state.meshPipelineCreated;
}

void Renderer::setFrustumCulling(bool enabled) {
    impl_->frustumCull = enabled;
}

bool Renderer::frustumCullingEnabled() const {
    return impl_->frustumCull;
}

unsigned Renderer::culledDrawsLastFrame() const {
    return impl_->culledLastFrame;
}

unsigned Renderer::submittedDrawsLastFrame() const {
    return impl_->submittedLastFrame;
}

bool Renderer::meshShadersImplemented() const {
    const auto& state = *impl_;
    return state.meshPipelineCreated && state.meshFrames > 0;
}

bool Renderer::meshPathActive() const { return impl_->meshPathUsedLastFrame; }

void Renderer::injectDeviceRemovalAtFrame(unsigned frame) { impl_->faultInjectionFrame = frame; }

unsigned Renderer::injectedRemovalFrame() const { return impl_->faultInjectionFrame; }

long Renderer::liveObjectCount() { return detail::g_liveObjects; }

unsigned long long Renderer::objectsCreatedCount() { return detail::g_objectsCreated; }

unsigned long long Renderer::videoMemoryBytes() {
    impl_->queryVideoMemory();
    return impl_->videoMemoryBytes;
}

bool Renderer::selfTestInvalidPipeline(std::string& code, std::string& text) {
    return impl_->selfTestInvalidPipeline(code, text);
}

void Renderer::setRtShadows(bool enabled) {
    auto& state = *impl_;
    state.rtRequested = enabled;
    if (enabled && !state.rtPipelineCreated && state.initialized && state.device5) {
        std::string error;
        if (!state.buildRaytracingPipeline(error)) {
            state.rtError = error.empty() ? "DXRT state object creation failed" : error;
        }
    }
}

void Renderer::setCompositeDebugView(unsigned view) { impl_->compositeDebugView = view; }
void Renderer::setLayerDebugView(unsigned mode, unsigned layer) {
    impl_->layerDebugMode = mode > 3u ? 0u : mode;
    impl_->layerDebugLayer = layer > 3u ? 0u : layer;
}
void Renderer::setOverlay(std::vector<OverlayLayer> layers) { impl_->overlay=std::move(layers); }
bool Renderer::setWaterSurface(WaterSurfaceFrame frame,std::string& error){
    if(!validateWaterFrame(frame,error))return false;
    // Removing the pond must drop the residual state; otherwise a later pond would
    // inherit waves from a body that no longer exists.
    if(frame.vertices.empty()&&impl_->waterRipple)impl_->waterRipple->reset();
    impl_->waterSurface=std::move(frame);return true;
}
bool Renderer::setUpperWaterSurface(WaterSurfaceFrame frame,std::string& error){
    if(!validateWaterFrame(frame,error))return false;
    if(frame.vertices.empty()&&impl_->upperWaterRipple)impl_->upperWaterRipple->reset();
    impl_->upperWaterSurface=std::move(frame);return true;
}
void Renderer::setWaterfalls(std::vector<WaterfallFrame> frames){ impl_->waterfalls=std::move(frames); }


bool Renderer::rtShadowsEnabled() const {
    const auto& state = *impl_;
    return state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated;
}

bool Renderer::rtShadowsImplemented() const {
    const auto& state = *impl_;
    return state.rtPipelineCreated && state.rtSbtCreated && state.rtDispatchedFrames > 0;
}

FrameStats Renderer::stats() const { return impl_->frameStats; }

std::string Renderer::probesJson() const {
    const auto& state = *impl_;
    std::ostringstream out;
    out << "{"
        << "\"materialTable\":{\"available\":true,\"implemented\":"
        << (state.materialTableReady ? "true" : "false")
        << ",\"slots\":" << state.materialSlots
        << ",\"mode\":\"bounded-texture-array\""
        << ",\"notes\":\"Bounded material table; not SM6.6 descriptor heap indexing\"},"
        << "\"sceneLights\":{\"implemented\":true,\"maxLights\":64}," << state.computeProbesFragment()
        << state.particlesJson()
        << state.effectSpritesJson()
        << state.gpuEffectsJson()
        << state.gpuClothJson()
        << "\"water\":{\"enabled\":" << (!state.waterSurface.vertices.empty()?"true":"false")
        << ",\"frames\":" << (state.waterDraw?state.waterDraw->stats().frames:0)
        << ",\"triangles\":" << (state.waterDraw?state.waterDraw->stats().triangles:0)
        << ",\"residual\":{\"resolution\":" << WaterRippleResolution
        << ",\"dispatches\":" << (state.waterRipple?state.waterRipple->dispatches():0)
        << ",\"eventsInjected\":" << state.waterRippleEvents << "}"
        << ",\"path\":\"continuous-cpu-height-surface-hdr-refraction-ssr-environment-fallback-plus-gpu-residual\"},"
        << "\"skinning\":{\"implemented\":" << (state.skinDraws>0?"true":"false")
        << ",\"draws\":" << state.skinDraws << ",\"maxBones\":256,\"path\":\"vertex-shader\",\"dynamicBlas\":false,\"morphDraws\":" << state.morphDraws << ",\"softDraws\":" << state.softDraws
        << ",\"bindingsLastFrame\":" << state.skinBindingsLastFrame << ",\"bindingsHighWater\":" << state.skinBindingsHighWater << ",\"maxInstances\":" << detail::MaxSkinInstances
        << ",\"geometryEntries\":" << state.skinGeometry.size() << ",\"geometryUploads\":" << state.skinGeometryUploads << ",\"geometryBytes\":" << state.skinGeometryBytes << ",\"uploadBytes\":" << state.skinMemoryBytes << "},"
        << "\"renderTargets\":{\"mode\":\"offscreen-mrt-plus-composite\",\"frames\":"
        << detail::FrameCount
        << ",\"formats\":{\"baseColor\":\"R16G16B16A16_FLOAT\",\"keyLight\":\"R16G16B16A16_FLOAT\""
           ",\"shadowMask\":\"R16_FLOAT\",\"composite\":\"R8G8B8A8_UNORM\",\"depth\":\"R32_FLOAT\"}"
        << ",\"screenshots\":" << state.screenshotsCaptured
        << ",\"debugView\":" << state.compositeDebugView
        << ",\"layerDebug\":{\"mode\":" << state.layerDebugMode << ",\"layer\":" << state.layerDebugLayer << "}"

        << ",\"validationLayer\":" << (state.infoQueue ? "true" : "false")
        << ",\"validationErrors\":" << state.validationErrors
        << ",\"validationWarnings\":" << state.validationWarnings
        << ",\"validationMessages\":["
        << [&] {
               std::ostringstream list;
               for (size_t index = 0; index < state.validationMessages.size(); ++index) {
                   if (index) list << ',';
                   list << detail::jsonString(state.validationMessages[index]);
               }
               return list.str();
           }()
        << "]"
        << ",\"screenshotBytes\":" << state.lastScreenshotBytes
        << ",\"lastScreenshot\":" << jsonString(state.lastScreenshotPath)
        << ",\"lastScreenshotMean\":" << std::fixed << std::setprecision(2) << state.lastScreenshotMean
        << ",\"lastScreenshotMin\":" << static_cast<unsigned>(state.lastScreenshotMin)
        << ",\"lastScreenshotMax\":" << static_cast<unsigned>(state.lastScreenshotMax)
        << ",\"lastScreenshotError\":" << jsonString(state.lastScreenshotError) << "},"
        << "\"raytracingAccelerationStructures\":{\"available\":"
        << (state.raytracingTier >= 10 ? "true" : "false")
        << ",\"built\":" << (state.blasBuilt ? "true" : "false")
        << ",\"effectImplemented\":"
        << ((state.rtPipelineCreated && state.rtDispatchedFrames > 0) ? "true" : "false")
        << ",\"blasBytes\":" << state.blasBytes
        << ",\"tlasBytes\":" << state.tlasBytes
        << ",\"tlasScratchBytes\":" << state.tlasScratchBytes
        << ",\"instances\":" << state.tlasInstanceCount
        << ",\"tlasCapacity\":" << state.tlasCapacity
        << ",\"tlasRebuilds\":" << state.tlasRebuilds
        << ",\"error\":" << jsonString(state.accelerationStructureError)
        << ",\"notes\":\"One unit-cube BLAS; TLAS rebuilt per frame from rigid instance transforms; no skinning\"},"
        << "\"raytracingShadows\":" << state.rtShadowsJson()
        << ",\"meshShader\":" << state.meshShaderJson()
        << ",\"bindless\":" << state.bindlessJson()
        << ",\"lightingQuality\":" << state.qualityJson()
        << ",\"transparency\":{\"active\":" << (state.transparencyActive?"true":"false")
        << ",\"orderIndependent\":true,\"drawsLastFrame\":" << state.transparencyDraws
        << ",\"fragmentsLastCompletedFrame\":" << state.transparencyFragments
        << ",\"peakFragments\":" << state.transparencyPeakFragments
        << ",\"capacity\":" << state.transparencyCapacity << ",\"maxLayersPerPixel\":64,\"overflowFrames\":" << state.transparencyOverflowFrames << "}"
        << ",\"diagnostics\":{\"events\":" << state.diagnosticEvents << ",\"items\":[";
    for (size_t index = 0; index < state.diagnostics.size(); ++index) {
        if (index) out << ',';
        out << "{\"code\":" << jsonString(state.diagnostics[index].code)
            << ",\"occurrences\":" << state.diagnostics[index].occurrences
            << ",\"text\":" << jsonString(state.diagnostics[index].text) << "}";
    }
    out << "]}"
        << ",\"reliability\":{\"liveObjects\":" << detail::g_liveObjects
        << ",\"objectsCreated\":" << detail::g_objectsCreated
        << ",\"videoMemoryBytes\":" << state.videoMemoryBytes
        << ",\"videoMemoryBudgetBytes\":" << state.videoMemoryBudgetBytes
        << ",\"videoMemoryQuerySupported\":" << (state.videoMemoryQuerySupported ? "true" : "false")
        << ",\"injectedDeviceRemoval\":" << (state.deviceRemovalInjected ? "true" : "false")
        << ",\"faultInjectionFrame\":" << state.faultInjectionFrame
        << ",\"framesRendered\":" << state.framesRendered
        << ",\"drawCalls\":" << state.drawCallsLastFrame
        << ",\"triangles\":" << state.trianglesLastFrame
        << ",\"frustumCulling\":" << (state.frustumCull ? "true" : "false")
        << ",\"submittedDraws\":" << state.submittedLastFrame
        << ",\"culledDraws\":" << state.culledLastFrame
        << ",\"plannedDraws\":" << state.plannedDrawsLastFrame
        << ",\"plannedBatches\":" << state.plannedBatchesLastFrame
        << ",\"plannedInstances\":" << static_cast<unsigned>(state.batchPlan.instances.size()) << "}"
        << "}";
    return out.str();
}

std::string Renderer::capabilitiesJson() const {
    const auto& state = *impl_;
    const bool rtEffect = state.rtPipelineCreated && state.rtSbtCreated &&
                          state.rtDispatchedFrames > 0;
    std::ostringstream out;
    out << "{"
        << "\"backend\":\"Direct3D 12\","
        << "\"initialized\":" << (state.initialized ? "true" : "false") << ","
        << "\"adapter\":" << jsonString(utf8(state.adapterDescription.Description)) << ","
        << "\"softwareAdapter\":"
        << ((state.adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false") << ","
        << "\"dedicatedVideoMemoryBytes\":" << state.adapterDescription.DedicatedVideoMemory << ","
        << "\"resourceBindingTier\":" << state.bindingTier << ","
        << "\"shaderModel\":";
    if (state.shaderModel) {
        out << '"' << ((state.shaderModel >> 4) & 15) << '.' << (state.shaderModel & 15) << '"';
    } else {
        out << "null";
    }
    out << ",\"raytracingTier\":";
    if (state.raytracingTier < 0) out << "null";
    else if (state.raytracingTier == 0) out << "\"unsupported\"";
    else out << '"' << (state.raytracingTier / 10) << '.' << (state.raytracingTier % 10) << '"';
    out << ",\"meshShaderTier\":";
    if (state.meshShaderTier < 0) out << "null"; else out << state.meshShaderTier;
    out << ",\"gpuTimestamps\":" << (state.timestampsAvailable ? "true" : "false")
        << ",\"separateShadowTimestamps\":"
        << ((state.timestampsAvailable && state.framesRendered > 0) ? "true" : "false")
        << ",\"bufferedFrames\":" << detail::FrameCount
        << ",\"materialTableSlots\":" << state.materialSlots
        << ",\"materialPath\":" << (state.materialPath ? "true" : "false")
        << ",\"materialBindless\":" << (state.materialBindless ? "true" : "false")
        << ",\"materialRecords\":" << state.configuredMaterials.size()
        << ",\"materialTextureSlices\":" << state.materialTextureSlices
        << ",\"materialTextureMips\":" << state.materialTextureMips
        << ",\"layeredMaterials\":" << [&] {
               unsigned layeredCount = 0, authoredLayers = 0, activeLayers = 0, samples = 0;
               unsigned long long textureBytes = 0;
               std::vector<unsigned> used;
               for (const auto& material : state.configuredMaterials) {
                   if (material.layers.empty()) continue;
                   ++layeredCount;
                   authoredLayers += material.authoredLayers ? material.authoredLayers : static_cast<unsigned>(material.layers.size());
                   activeLayers += static_cast<unsigned>(material.layers.size());
                   for (const auto& layer : material.layers) for (const auto& slot : layer.slots)
                       if (slot.texture != 0xffffffffu) { ++samples; used.push_back(slot.texture); }
               }
               std::sort(used.begin(), used.end()); used.erase(std::unique(used.begin(), used.end()), used.end());
               for (unsigned index : used) if (index < state.configuredTextures.size()) {
                   const auto& texture = state.configuredTextures[index];
                   size_t bytes = 0; unsigned mipWidth = texture.width, mipHeight = texture.height;
                   for (unsigned mip = 0; mip < texture.mips; ++mip) {
                       bytes += texture.format == 0 ? static_cast<size_t>(mipWidth) * mipHeight * 4
                             : texture.format == 5 ? static_cast<size_t>(mipWidth) * mipHeight * 2
                             : static_cast<size_t>((mipWidth + 3) / 4) * ((mipHeight + 3) / 4) * (texture.format == 1 ? 8 : 16);
                       mipWidth = std::max(1u, mipWidth / 2); mipHeight = std::max(1u, mipHeight / 2);
                   }
                   textureBytes += bytes;
               }
               std::ostringstream layered;
               layered << "{\"materials\":" << layeredCount << ",\"authoredLayers\":" << authoredLayers
                       << ",\"activeLayers\":" << activeLayers << ",\"textureSamples\":" << samples
                       << ",\"uniqueTextures\":" << used.size() << ",\"textureBytes\":" << textureBytes;
               return layered.str();
           }()
        << ",\"surfaceRelief\":" << [&] {
               unsigned materialsOn = 0, pom = 0, baked = 0, selfShadow = 0, r16Textures = 0, rgba8Fallback = 0;
               unsigned long long r16Bytes = 0;
               for (const auto& material : state.configuredMaterials) {
                   if (!material.hasSurfaceRelief) continue;
                   ++materialsOn;
                   const unsigned mode = material.surfaceRelief.flags & 3u;
                   if (mode == 1) ++pom; else if (mode == 2) ++baked;
                   if ((material.surfaceRelief.flags & (1u << 3)) != 0) ++selfShadow;
                   if ((material.surfaceRelief.flags & (1u << 4)) != 0 && material.surfaceRelief.texture < state.configuredTextures.size()) {
                       const auto& texture = state.configuredTextures[material.surfaceRelief.texture];
                       if (texture.format == 0) ++rgba8Fallback;
                       else if (texture.format == 5) {
                           ++r16Textures;
                           unsigned width = texture.width, height = texture.height;
                           for (unsigned mip = 0; mip < texture.mips; ++mip) {
                               r16Bytes += static_cast<unsigned long long>(width) * height * 2;
                               width = std::max(1u, width / 2); height = std::max(1u, height / 2);
                           }
                       }
                   }
               }
               std::ostringstream relief;
               relief << "{\"materials\":" << materialsOn << ",\"pom\":" << pom
                      << ",\"bakedDisplacement\":" << baked << ",\"selfShadow\":" << selfShadow
                      << ",\"r16Textures\":" << r16Textures << ",\"r16Bytes\":" << r16Bytes
                      << ",\"rgba8Fallback\":" << rgba8Fallback << ",\"pixelDepth\":false}";
               return relief.str();
           }()
        << ",\"maxLayers\":4,\"sampleBudget\":36"
        << ",\"layerDebug\":{\"mode\":" << state.layerDebugMode << ",\"layer\":" << state.layerDebugLayer << "}"
        << ",\"shaderVariants\":[\"ps_6_6 raster\",\"ps_6_6 transparency capture\"]}"
        << ",\"offScreenComposition\":true,\"screenshots\":true"
        << ",\"dxc\":{\"available\":" << (shaders::kDxcAvailable ? "true" : "false")
        << ",\"version\":" << jsonString(shaders::kDxcVersion)
        << ",\"error\":" << jsonString(shaders::kDxcError) << "}"
        << ",\"implemented\":{\"rasterization\":true,\"depthTest\":true,\"textureSampling\":true,"
           "\"directionalLighting\":true,\"sceneLights\":true,\"materialTable\":true,"
        << "\"offScreenComposition\":true,"
        << "\"accelerationStructures\":" << (state.blasBuilt ? "true" : "false") << ","
        << "\"raytracing\":" << (rtEffect ? "true" : "false") << ","
        << "\"bindless\":false,\"meshShaders\":false,"
           "\"pathTracing\":false,\"virtualGeometry\":false,\"streaming\":false},"
           "\"timingNotes\":\"CPU time includes fence wait and Present; GPU time covers the whole "
           "command list and gpuRtMs covers only the DispatchRays window. Adapter capabilities "
           "do not imply implemented effects.\"}";
    return out.str();
}

void Renderer::Impl::clearValidationMessages() {
    if (!infoQueue) return;
    infoQueue->ClearStoredMessages();
}

void Renderer::Impl::collectValidationMessages(const char* stage) {
    if (!infoQueue) return;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &length)) || length == 0) continue;
        std::vector<char> buffer(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(index, message, &length)) || !message->pDescription) {
            continue;
        }
        if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)++validationErrors;
        if(message->Severity==D3D12_MESSAGE_SEVERITY_WARNING)++validationWarnings;
        const bool severe=message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR;
        const auto text=std::string("[")+stage+"] "+std::to_string(static_cast<int>(message->Severity))+": "+message->pDescription;
        // Repeated sampler warnings must not conceal a later resource-state error.
        if(std::find(validationMessages.begin(),validationMessages.end(),text)==validationMessages.end()){
            if(validationMessages.size()<(severe?64u:32u))validationMessages.push_back(text);
            else if(severe){const auto warning=std::find_if(validationMessages.begin(),validationMessages.end(),[](const std::string& value){return value.find("] 2:")!=std::string::npos;});if(warning!=validationMessages.end())*warning=text;}
        }
    }
    infoQueue->ClearStoredMessages();
}


void Renderer::setValidationEnabled(bool enabled) {
    if (impl_->initialized) return; // must be set before initialize()
    impl_->validationRequested = enabled;
}

void Renderer::Impl::addDiagnostic(const char* code, const std::string& text) {
    ++diagnosticEvents;
    for (auto& entry : diagnostics) {
        if (entry.code == code) {
            ++entry.occurrences;
            return;
        }
    }
    if (diagnostics.size() >= 32) return;
    Diagnostic entry;
    entry.code = code;
    entry.text = text;
    entry.occurrences = 1;
    diagnostics.push_back(entry);
}

void Renderer::Impl::queryVideoMemory() {
    videoMemoryBytes = 0;
    videoMemoryBudgetBytes = 0;
    videoMemoryQuerySupported = false;
    if (!adapter) return;
    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) && adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        const HRESULT localResult =
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        const HRESULT nonLocalResult =
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        if (SUCCEEDED(localResult)) {
            videoMemoryBytes += local.CurrentUsage;
            videoMemoryBudgetBytes += local.Budget;
            videoMemoryQuerySupported = true;
        }
        if (SUCCEEDED(nonLocalResult)) {
            videoMemoryBytes += nonLocal.CurrentUsage;
            videoMemoryBudgetBytes += nonLocal.Budget;
            videoMemoryQuerySupported = true;
        }
        adapter3->Release();
    }
}

bool Renderer::Impl::selfTestInvalidPipeline(std::string& code, std::string& text) {
    // A missing symbol is guaranteed to fail compilation. The point is that the failure is
    // captured as a stable diagnostic and the renderer keeps running.
    const char* broken = "float4 BrokenPS() : SV_Target { return missing_symbol; }";
    ID3DBlob* blob = nullptr;
    std::string compileError;
    const bool compiled = compileEmbedded(broken, "BrokenPS", "ps_5_1", &blob, compileError);
    detail::release(blob);
    code = "NG-E-PIPE-COMPILE";
    if (compiled) {
        text = "broken shader compiled unexpectedly";
        addDiagnostic("NG-E-PIPE-COMPILE", text);
        return false;
    }
    text = compileError;
    addDiagnostic("NG-E-PIPE-COMPILE", text);
    return true;
}

void Renderer::shutdown() {
    if (!impl_) return;
    auto& state = *impl_;
    std::string ignored;
    state.waitIdle(ignored);
    state.waterDraw.reset();
    state.waterRipple.reset();
    for(auto& layer:state.vegetation)if(layer.draw)layer.draw->shutdown(ignored);
    state.vegetation.clear();
    if(state.hair){state.hair->draw->shutdown(ignored);state.hair.reset();}
    state.releaseRaytracingPipeline();
    state.releaseMeshPipeline();
    state.releaseParticles();
    state.releaseGpuEffects();
    state.releaseGpuCloth();
    state.releaseEffectSprites();
    state.releaseComputePipeline();
    state.releaseQuality();
    state.releaseEnvironmentTexture();state.destroy(state.shadowClearHeap);
    state.releaseTransparency();

    state.destroy(state.captureReadback);
    state.destroy(state.commands6);
    state.destroy(state.device2);
    state.destroy(state.commands4);
    state.destroy(state.commands);
    for (auto& allocator : state.allocators) state.destroy(allocator);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        state.destroy(state.tlasInstances[index]);
        state.destroy(state.tlasScratch[index]);
        state.destroy(state.tlas[index]);
    }
    state.destroy(state.blas);
    state.destroy(state.timestampReadback);
    state.destroy(state.timestampHeap);
    state.releaseTargets();
    state.destroy(state.vertexBuffer);
    state.destroy(state.indexBuffer);
    state.releaseConvexMeshes();
    for(auto& geometry:state.skinGeometry){state.destroy(geometry.vertices);state.destroy(geometry.indices);state.destroy(geometry.morphs);state.destroy(geometry.zeroDeformation);}state.skinGeometry.clear();
    for(unsigned index=0;index<detail::SkinBindingCount;++index){state.destroy(state.skinPalette[index]);state.destroy(state.skinDeformation[index]);state.skinDeformationBytes[index]=0;}
    state.skinMemoryBytes=state.skinGeometryBytes=0;
    state.destroy(state.materialTexture);
    state.destroy(state.materialRecords);
    state.destroy(state.materialLayerRecords);
    for (ID3D12Resource*& texture : state.cookedTextures) state.destroy(texture);
    state.cookedTextures.clear();
    if (state.sceneConstantBuffer && state.sceneConstantMapped) {
        D3D12_RANGE noWrite{0, 0};
        state.sceneConstantBuffer->Unmap(0, &noWrite);
        state.sceneConstantMapped = nullptr;
    }
    state.destroy(state.sceneConstantBuffer);
    if (state.compositeConstantBuffer && state.compositeConstantMapped) {
        D3D12_RANGE noWrite{0, 0};
        state.compositeConstantBuffer->Unmap(0, &noWrite);
        state.compositeConstantMapped = nullptr;
    }
    state.destroy(state.compositeConstantBuffer);
    state.destroy(state.rasterPipeline);
    state.destroy(state.texturedPipeline);
    state.destroy(state.bindlessPipeline);
    state.destroy(state.bindlessSkinPipeline);
    state.destroy(state.inlineEnvironmentPipeline);state.destroy(state.inlineEnvironmentSkinPipeline);
    state.destroy(state.transparentPipeline);state.destroy(state.transparentSkinPipeline);
    state.destroy(state.skinPipeline);
    state.destroy(state.compositePipeline);
    state.destroy(state.rasterRootSignature);
    state.destroy(state.compositeRootSignature);
    state.destroy(state.rtRootSignature);
    state.destroy(state.infoQueue);
    state.destroy(state.srvHeap);
    state.destroy(state.materialSamplerHeap);
    state.destroy(state.dsvHeap);
    state.destroy(state.rtvHeap);
    state.destroy(state.swapChain);
    state.destroy(state.queue);
    state.destroy(state.fence);
    if (state.fenceEvent) { CloseHandle(state.fenceEvent); state.fenceEvent = nullptr; }
    state.destroy(state.device5);
    state.destroy(state.device);
    state.destroy(state.adapter);
    state.destroy(state.factory);
    // Re-initialize() calls shutdown() first. The cooked material payload belongs to the
    // host configuration, not to the device, so it survives a device rebuild.
    auto configuredTextures = std::move(state.configuredTextures);
    auto configuredMaterials = std::move(state.configuredMaterials);
    auto configuredEffectTextures = std::move(state.configuredEffectTextures);
    const bool materialPath = state.materialPath;
    const unsigned materialWidth = state.materialTextureWidth;
    const unsigned materialHeight = state.materialTextureHeight;
    const unsigned materialMips = state.materialTextureMips;
    const unsigned materialSlices = state.materialTextureSlices;
    const Environment environment = state.environment;
    // The particle field is built during initialize(), which runs shutdown() first. Its
    // request and emitter therefore have to survive this reset the same way the cooked
    // material payload does, otherwise the toggle set before initialize() is lost.
    const bool particlesRequested = state.particlesRequested;
    const bool firePreset = state.particleFirePreset;
    const unsigned explodeAt = state.particleExplodeAt;
    const float extinction = state.particleExtinction;
    const float ambient = state.particleAmbient;
    const float buoyancy = state.particleBuoyancy;
    const float sink = state.particleSink;
    const float emitterX = state.particleEmitter[0];
    const float emitterY = state.particleEmitter[1];
    const float emitterZ = state.particleEmitter[2];
    const float volumeMinX = state.particleVolumeMin[0];
    const float volumeMinY = state.particleVolumeMin[1];
    const float volumeMinZ = state.particleVolumeMin[2];
    const float volumeExtent = state.particleVolumeExtent;
    const bool validationRequested=state.validationRequested;
    auto configuredGpuEffects=std::move(state.configuredGpuEffects);
    auto configuredConvexMeshes=std::move(state.configuredConvexMeshes);
    auto configuredGpuEffectTextures=std::move(state.configuredGpuEffectTextures);
    auto gpuEffectDriver=std::move(state.gpuEffectDriver);
    auto configuredGpuCloth=std::move(state.configuredGpuCloth);
    auto gpuClothDriver=std::move(state.gpuClothDriver);
    auto configuredGpuFieldWorld=std::move(state.configuredGpuFieldWorld);
    auto gpuFieldDriver=std::move(state.gpuFieldDriver);
    const auto configuredGpuFieldLimits=state.configuredGpuFieldLimits;
    const auto configuredGpuVectorLimits=state.configuredGpuVectorLimits;
    state = Impl{};
    state.validationRequested=validationRequested;
    state.configuredGpuEffects=std::move(configuredGpuEffects);
    state.configuredConvexMeshes=std::move(configuredConvexMeshes);
    state.configuredGpuEffectTextures=std::move(configuredGpuEffectTextures);
    state.gpuEffectDriver=std::move(gpuEffectDriver);
    state.configuredGpuCloth=std::move(configuredGpuCloth);
    state.gpuClothDriver=std::move(gpuClothDriver);
    state.configuredGpuFieldWorld=std::move(configuredGpuFieldWorld);
    state.gpuFieldDriver=std::move(gpuFieldDriver);
    state.configuredGpuFieldLimits=configuredGpuFieldLimits;
    state.configuredGpuVectorLimits=configuredGpuVectorLimits;
    state.configuredTextures = std::move(configuredTextures);
    state.configuredMaterials = std::move(configuredMaterials);
    state.configuredEffectTextures = std::move(configuredEffectTextures);
    state.materialPath = materialPath;
    state.materialTextureWidth = materialWidth;
    state.materialTextureHeight = materialHeight;
    state.materialTextureMips = materialMips;
    state.materialTextureSlices = materialSlices;
    state.environment = environment;
    state.particlesRequested = particlesRequested;
    state.particleFirePreset = firePreset;
    state.particleExplodeAt = explodeAt;
    state.particleExtinction = extinction;
    state.particleAmbient = ambient;
    state.particleBuoyancy = buoyancy;
    state.particleSink = sink;
    state.particleEmitter[0] = emitterX;
    state.particleEmitter[1] = emitterY;
    state.particleEmitter[2] = emitterZ;
    state.particleVolumeMin[0] = volumeMinX;
    state.particleVolumeMin[1] = volumeMinY;
    state.particleVolumeMin[2] = volumeMinZ;
    state.particleVolumeExtent = volumeExtent;
}

} // namespace nativegpu




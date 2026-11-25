#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

// Matches shaders/material_layer_sampling.hlsli StructuredBuffer layout.
// Not yet attached to the renderer material table.
namespace engine::material::layered {
struct GpuTexture {
    float row0[3]{1,0,0}; std::uint32_t texture=std::numeric_limits<std::uint32_t>::max();
    float row1[3]{0,1,0}; std::uint32_t sampler=0;
    std::uint32_t uvSet=0, channel=0, flags=0, padding=0;
};
struct GpuLayer {
    GpuTexture slots[9];
    float baseColor[3]{1,1,1},roughness=1;
    float emissiveFactor[3]{1,1,1},metallic=0;
    float normalStrength=1,occlusionStrength=1,emissiveStrength=1,heightScale=1;
    float heightOffset=0,weight=1,gain=1,bias=0;
    float width=.2f,heightBias=0; std::uint32_t source=0,flags=0;
};
// Material-level relief parameters, appended to GpuMaterial. mode: 0 off, 1 POM,
// 2 baked displacement. source: 0 layer height, 1 baked texture height.
struct GpuSurfaceRelief {
    float row0[3]{1,0,0}; std::uint32_t texture=std::numeric_limits<std::uint32_t>::max();
    float row1[3]{0,1,0}; std::uint32_t sampler=0;
    std::uint32_t uvSet=0,channel=0,flags=0,heightLayer=std::numeric_limits<std::uint32_t>::max();
    float amplitudeMetres=0,referenceHeight=1,minSteps=8,maxSteps=24;
    float refineSteps=2,fadeStartMetres=6,fadeEndMetres=14,grazingFade=.1f;
    float shadowSteps=4,heightOffsetMetres=0,padding0=0,padding1=0;
};
static_assert(sizeof(GpuTexture)==48 && offsetof(GpuTexture,uvSet)==32,"LayerTexture HLSL layout changed");
static_assert(sizeof(GpuLayer)==512 && offsetof(GpuLayer,baseColor)==432 && offsetof(GpuLayer,width)==496,"LayerMaterial HLSL layout changed");
static_assert(sizeof(GpuSurfaceRelief)==96 && offsetof(GpuSurfaceRelief,uvSet)==32 && offsetof(GpuSurfaceRelief,amplitudeMetres)==48,"SurfaceRelief HLSL layout changed");
}

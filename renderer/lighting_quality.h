#pragma once

#include "d3d12_renderer.h"
#include "smoke_medium.hpp"
#include <array>

namespace nativegpu::detail {

// CPU/HLSL ABI, mirrored by shaders/lighting_quality_common.hlsli. All fields are
// 16-byte rows; descriptors belong to the global heap, never to cooked textures.
struct QualityLight {
    float positionRange[4];
    float colorIntensity[4];
    float directionCos[4];
    float params[4];
    float shadow[4]; // surface history weight, volume visibility, volume intensity, id
};
struct QualityInstance {
    Mat4 inverseModel;
    float color[4];
    unsigned textures[4]; // albedo, ORM, normal, valid
    float material[4]; // roughness, metalness, UV scale, emissive scale
};
struct QualityConstants {
    Mat4 inverseViewProjection, viewProjection, previousViewProjection;
    float eyeFrame[4], forwardHistory[4], previousEye[4], previousForward[4];
    float keyDirectionIntensity[4], keyColor[4], skyExposure[4], ambient[4];
    float fog[4], cloud[4], wind[4], probeOriginSpacing[4], probeCountsHistory[4];
    unsigned grid[4], screen[4], slots[15][4];
    float sourceEnvironment[4]; // rotation cosine/sine, radiance intensity, reserved
    float cloudShadowOrigin[4], diffuseSH[9][4];
    unsigned volumeInfo[4];
    float volumeCenters[8][4], volumeExtents[8][4];
    QualityLight lights[64];
    SmokeScene smoke;
};
static_assert(sizeof(QualityLight) == 80, "Quality light ABI");
static_assert(sizeof(QualityInstance) == 112, "Quality instance ABI");
static_assert(sizeof(QualityConstants) % 16 == 0, "Quality constant ABI");
static_assert(sizeof(QualityConstants) == 6896, "Quality CPU/HLSL constant layout changed");
constexpr unsigned QualityCbStride = (sizeof(QualityConstants) + 255u) & ~255u;
constexpr unsigned QualityDescriptorBase = 384;
enum QualitySlot : unsigned {
    QConstants = 0, QInstances = 3, QIbl = 6, QIblMipUav = 7,
    QBrdf = 14, QBrdfUav = 15,
    QProbe = 16, QProbeStride = 6,
    QFogHistory = 28, QFogScratch = 32, QFogIntegrated = 34,
    QNoise = 36, QWeather = 38, QCloudShadow = 40,
    QCloud = 42, QCloudStride = 4, QLocalShadowCache = 50, QEnvironment = 52, QInlineMaterials = 53, QSlotCount = 54
};

} // namespace nativegpu::detail

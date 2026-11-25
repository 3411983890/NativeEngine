#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "../engine/native/vegetation/asset.hpp"
#include "matrix_math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Vegetation draw path: one GPU-side instance table, GPU culling into an indirect draw.
//
// What this module does *not* do, deliberately: it never reads the visible count back to
// the CPU, and it never loops over instances on the CPU. The CPU cost of a frame is one
// constant-buffer write plus three dispatches, independent of the instance count.

namespace nativegpu {

// Debug view codes. One list serves the shader switch (`debugMode` in
// shaders/vegetation.hlsl), the host's pipe parser and the editor's dropdown, so a view cannot
// exist in the UI without existing in the pixel shader. Adding a view means adding a branch to
// both places; the numbers here are the contract.
enum class VegetationDebugView : int {
    Shaded = 0,
    BaseColor = 1,
    UvChecker = 2,
    GeometricNormal = 3,
    MappedNormal = 4,
    AmbientOcclusion = 5,
    Roughness = 6,
    Metallic = 7,
    Opacity = 8,
    Thickness = 9,
    Height = 10,
    WoodOnly = 11,
    LeavesOnly = 12,
    CameraDepth = 13,
    Shadow = 14,
};

struct VegetationDebugViewName {
    const char* name;
    VegetationDebugView view;
};

// The wire names. They are the same strings the editor sends, so a typo is a refusal with a
// message instead of a silently unchanged frame.
inline constexpr VegetationDebugViewName kVegetationDebugViews[] = {
    {"shaded", VegetationDebugView::Shaded},
    {"basecolor", VegetationDebugView::BaseColor},
    {"uv", VegetationDebugView::UvChecker},
    {"normal", VegetationDebugView::GeometricNormal},
    {"mappednormal", VegetationDebugView::MappedNormal},
    {"ao", VegetationDebugView::AmbientOcclusion},
    {"roughness", VegetationDebugView::Roughness},
    {"metallic", VegetationDebugView::Metallic},
    {"opacity", VegetationDebugView::Opacity},
    {"thickness", VegetationDebugView::Thickness},
    {"height", VegetationDebugView::Height},
    {"wood", VegetationDebugView::WoodOnly},
    {"leaves", VegetationDebugView::LeavesOnly},
    {"depth", VegetationDebugView::CameraDepth},
    {"shadow", VegetationDebugView::Shadow},
};

inline bool parseVegetationDebugView(const std::string& name, VegetationDebugView& out) {
    for (const auto& entry : kVegetationDebugViews) {
        if (name == entry.name) { out = entry.view; return true; }
    }
    return false;
}

inline std::string vegetationDebugViewName(VegetationDebugView view) {
    for (const auto& entry : kVegetationDebugViews) {
        if (entry.view == view) return entry.name;
    }
    return "unknown";
}

struct VegetationDrawStats {
    std::uint64_t frames = 0;
    std::uint64_t shadowFrames = 0;
    std::uint64_t drawnInstances = 0;
    std::uint64_t culledInstances = 0;
    std::uint64_t triangles = 0;
    std::uint64_t lastFence = 0;
    std::uint32_t capacity = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t overflow = 0;
    std::uint32_t shadowMapSize = 0;
    // Mip levels actually uploaded per material slot (9 slots). 1 means the texture was
    // uploaded without a chain, which is the configuration that erodes leaf coverage.
    std::uint32_t textureMips[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
};

struct VegetationFrameInput {
    Mat4 viewProjection;
    // World -> key-light clip, same 0..1 depth convention as viewProjection. drawShadow
    // renders with it and draw samples with it; leaving it at identity with shadows on
    // would project every fragment onto one texel, so shadows default to off.
    Mat4 lightViewProjection;
    float camera[3] = {0, 0, 0};
    float seconds = 0;
    float windDirection[3] = {1, 0, 0};
    float windSpeed = 1.0f;
    float windAmplitude = 0.1f;
    float windFrequency = 0.6f;
    float lightDirection[3] = {0.4f, 0.8f, 0.45f};
    float lightColor[3] = {1, 1, 1};
    float lightIntensity = 1.0f;
    float ambient = 0.25f;
    float viewportHeight = 1080.0f;
    float tanHalfFov = 1.0f;
    float alphaCutoff = 0.35f;
    float maximumDistance = 400.0f;
    // 0 disables the key-light shadow for this frame (and skips the shadow pass).
    float shadowStrength = 0.0f;
    float shadowDepthBias = 0.0012f;
    float shadowNormalBiasMetres = 0.05f;
    // Debug view. 0 = shaded. The meaning of every other value is listed in
    // `VegetationDebugView` below and mirrored by the `debugMode` switch in
    // shaders/vegetation.hlsl; the host parses the same names.
    int debugView = 0;
    float debugCells = 8.0f;
};

// One drawable LOD of one species, already decoded from the package.
struct VegetationPrimitive {
    std::uint16_t material = 0;
    std::uint32_t firstIndex = 0, indexCount = 0;
};
struct VegetationMeshData {
    std::vector<float> positions, normals, uvs, windWeights;
    std::vector<std::uint8_t> colors;
    std::vector<std::uint32_t> indices;
    // Material ranges decide which texture set a vertex is shaded with. Bark and leaf
    // share one draw, so this has to travel per vertex rather than per draw call.
    std::vector<VegetationPrimitive> primitives;
    std::uint32_t firstIndex = 0, indexCount = 0;
};

// One cooked NGT3 mip level, decoded by the caller (the loader rejects a bad header before
// the pixels reach this struct).
struct VegetationTextureLevel {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
};

// A cooked texture with its whole mip chain. Uploading level 0 alone is what made leaves
// drop out of a distant canopy, so the level count is part of the contract, not an option.
struct VegetationTexture {
    std::uint32_t width = 0, height = 0;
    bool srgb = false;
    std::vector<VegetationTextureLevel> levels;
    std::uint32_t mipCount() const { return static_cast<std::uint32_t>(levels.size()); }
    bool valid() const {
        if (levels.empty() || width == 0 || height == 0) return false;
        for (const auto& level : levels) {
            if (level.width == 0 || level.height == 0) return false;
            if (level.pixels.size() != static_cast<std::size_t>(level.width) * level.height * 4) return false;
        }
        return levels[0].width == width && levels[0].height == height;
    }
};
// Nine slots, one per quantity the shader reads. The slot order is the descriptor order in
// the shader (t3..t11) and must not be rearranged without changing both.
struct VegetationTextureSet {
    VegetationTexture barkBaseColor, barkNormal, barkOrm, barkHeight;
    VegetationTexture leafBaseColor, leafNormal, leafOrm, leafThickness, leafOpacity;
};

// Shading inputs that come from the species material contract rather than from a texture.
// The caller fills this from the cooked package's `materials` array; nothing here is a
// hard-coded taste value except the defaults, which are only used when a slot is absent.
struct VegetationMaterial {
    float barkRoughness = 0.85f;
    float leafRoughness = 0.55f;
    float barkThicknessMetres = 0.02f;
    float leafThicknessMetres = 0.0004f;
    float leafAbsorptionPerMetre = 900.0f;
    float normalScale = 1.0f;
    float heightScaleMetres = 0.0f;
    float uvTileMetres = 1.6f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.35f;
    bool doubleSided = true;
    // Normal-map convention per material group. `openGL+` sheets store green pointing up, so the
    // shader flips it once when sampling. Two flags because bark and foliage are separate sheets
    // and a species may legitimately mix conventions.
    bool barkNormalFlipGreen = false;
    bool leafNormalFlipGreen = false;
    // Which optional maps actually arrived. The shader gates on these bits instead of
    // sampling a 1x1 placeholder and hoping it looks like the real quantity.
    bool hasBarkOrm = false, hasLeafOrm = false, hasLeafThickness = false, hasBarkHeight = false;
};

// Reads the two materials a vegetation package always has (one bark, one foliage) into the
// shader-facing struct. Returns false when the package has no foliage material.
bool materialFromSpecies(const engine::vegetation::Species& species, VegetationMaterial& out);
class VegetationDraw {
public:
    VegetationDraw(ID3D12Device* device, ID3D12Fence* timeline, DXGI_FORMAT color, DXGI_FORMAT depth, unsigned capacity);
    ~VegetationDraw();
    VegetationDraw(const VegetationDraw&) = delete;
    VegetationDraw& operator=(const VegetationDraw&) = delete;

    // Uploads the material textures once. Must be called before the first draw and only
    // once: the descriptors are referenced by every later command list.
    bool setTextures(ID3D12GraphicsCommandList* list, const VegetationTextureSet& textures, std::string& error);
    // Records the mesh and the packed instance page into `list`. The instance records are
    // exactly the 32-byte records from the region package; the shader decodes them with
    // the same field order, and the page frame travels in the constant buffer.
    bool upload(ID3D12GraphicsCommandList* list, const VegetationMeshData& mesh,
                const std::vector<engine::vegetation::PackedInstance>& instances,
                const engine::vegetation::PageRecord& page, std::string& error);
    // Records the cull + indirect draw. `capacity` bounds the visible list.
    bool draw(ID3D12GraphicsCommandList* list, std::uint64_t fence, const VegetationFrameInput& frame,
              const VegetationMaterial& material, const float bounds[3], std::string& error);
    // Records the key-light depth pass for the same page into this module's own shadow map.
    // It must be recorded into the same command list *before* the caller binds its colour
    // target, because it binds a depth-only output and does not restore the caller's
    // render targets. Returns true and records nothing when frame.shadowStrength <= 0.
    bool drawShadow(ID3D12GraphicsCommandList* list, std::uint64_t fence, const VegetationFrameInput& frame,
                    const VegetationMaterial& material, const float bounds[3], std::string& error);

    VegetationDrawStats stats() const;
    bool shutdown(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativegpu

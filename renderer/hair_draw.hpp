#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "../engine/native/hair/gpu_pack.hpp"
#include "matrix_math.h"

#include <cstdint>
#include <memory>
#include <string>

// D3D12 strand pass for a cooked hairstyle.
//
// One non-indexed draw: the vertex shader expands six vertices per segment from the
// uploaded point/strand records, the pixel shader implements the fibre response from
// engine/hair/shading.mjs, samples a light-space deep-opacity grid for self-shadowing,
// and applies coverage-corrected alpha.
//
// What this module does not claim:
//   * transparency is alpha blending in upload order, not a sorted or order-independent
//     cache. `HairDrawStats::transparency` names that mode.
//   * guide simulation runs on the CPU (HairSimulation): guides are solved at a fixed
//     tick, rendered strands are interpolated and uploaded from three rotating buffers.
//     There is no GPU guide solve, and roots are not attached to an animated head bone.
//   * the deep-opacity grid is built on the CPU from those strands. It is not a shadow
//     pass that casts onto the body or the scene.

namespace nativegpu {

enum class HairDebugView : int {
    Shaded = 0,
    Coverage = 1,
    Tangent = 2,
    RootToTip = 3,
    StrandId = 4,
    MaterialGroup = 5,
    CameraDepth = 6,
    ShadowThickness = 7,
};

struct HairDebugViewName {
    const char* name;
    HairDebugView view;
};
inline constexpr HairDebugViewName kHairDebugViews[] = {
    {"shaded", HairDebugView::Shaded},
    {"coverage", HairDebugView::Coverage},
    {"tangent", HairDebugView::Tangent},
    {"roottotip", HairDebugView::RootToTip},
    {"strandid", HairDebugView::StrandId},
    {"material", HairDebugView::MaterialGroup},
    {"depth", HairDebugView::CameraDepth},
    {"shadow", HairDebugView::ShadowThickness},
};
inline bool parseHairDebugView(const std::string& name, HairDebugView& out) {
    for (const auto& entry : kHairDebugViews)
        if (name == entry.name) { out = entry.view; return true; }
    return false;
}

struct HairMaterial {
    float albedo[3] = {0.055f, 0.036f, 0.024f};
    float roughness = 0.34f;
    float radialRoughness = 0.25f;
    float shiftRadians = 0.12f;
    float rootToTipTint[3] = {0.74f, 0.60f, 0.48f};
};

// Guide-level simulation. Only the guides move; rendered strands are interpolated from
// them every tick, which is the plan's "simulate guides, interpolate strands" rule.
// The solver is the CPU authority: fixed 1/60 s tick split into `substeps`, Verlet
// integration, distance and rest-shape constraints, pinned roots and a head sphere.
// A GPU solve can replace it later without changing the draw contract.
struct HairSimulation {
    bool enabled = false;
    float gravity[3] = {0.0f, -9.81f, 0.0f};
    // Target air velocity in metres per second, not an acceleration. Treating it as a
    // velocity lets drag cap the strand speed instead of accelerating hair forever.
    float wind[3] = {0.0f, 0.0f, 0.0f};
    float damping = 0.985f;
    float windDragPerSecond = 6.0f;
    // Per-iteration pull back toward the authored rest shape. Without it the guides
    // behave like loose ropes and a bob style lies flat.
    float shapeStiffness = 0.06f;
    int iterations = 8;
    int substeps = 4;
};

struct HairLighting {
    float direction[3] = {0.35f, 0.55f, 0.75f};
    float intensity = 3.6f;
    float colour[3] = {1.0f, 0.97f, 0.92f};
    float ambient[3] = {0.05f, 0.052f, 0.06f};
    // 0 disables the deep-opacity lookup; the grid is not built in that case.
    float shadowStrength = 0.85f;
    float shadowSigmaPerMetre = 2400.0f;
    float shadowDensityScale = 1.0f;
};

// The coverage law converts metres to pixels through tanHalfFovY and the clip-space w,
// so it assumes a perspective projection. An orthographic caller has to supply the
// equivalent angular scale explicitly; deriving it from m[5] would halve the strand width.
struct HairFrameInput {
    Mat4 viewProjection{};
    float camera[3] = {0, 0, 0};
    float viewportWidth = 1920;
    float viewportHeight = 1080;
    float tanHalfFovY = 0.29f;
    float nearMetres = 0.01f;
    float minWidthPixels = 1.0f;
    float maxWidthPixels = 6.0f;
    float alphaMinimum = 0.02f;
    int debugView = 0;
    // Draw strands back-to-front by root view depth. Turning it off restores the upload
    // order, which is kept as the evidence control for the ordering claim.
    bool sortStrands = true;
    // Fixed visual tick. The simulation advances in whole 1/60 s steps and interpolation
    // happens after the last step, so two runs with the same input are identical.
    float deltaSeconds = 1.0f / 60.0f;
    // Model-to-world transform of a hairstyle rigidly attached to the head bone. The
    // simulation stays in rest space; gravity and wind are rotated by the inverse, the
    // vertex shader applies the transform, and the deep-opacity lookup is done in rest
    // space so the grid does not have to be rebuilt for every head rotation.
    Mat4 headTransform = identityMatrix();
};

struct HairDrawStats {
    std::uint64_t frames = 0;
    std::uint64_t drawnStrands = 0;
    std::uint64_t drawnVertices = 0;
    std::uint64_t triangles = 0;
    std::uint64_t lastFence = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t shadowBuilds = 0;
    std::uint64_t shadowCoveredTexels = 0;
    std::uint64_t shadowCrossings = 0;
    std::uint64_t shadowDroppedLayers = 0;
    float shadowMaxOpticalDepthMetres = 0;
    std::uint32_t strandCount = 0;
    std::uint32_t pointCount = 0;
    std::uint32_t shadowGridSize = 0;
    std::uint32_t shadowLayers = 0;
    bool simulating = false;
    std::uint64_t simulationTicks = 0;
    std::uint64_t simulationStaleUploads = 0;
    float simulationTimeSeconds = 0;
    float simulationMaxStretch = 1;
    float simulationMaxPenetrationMetres = 0;
    float simulationMaxSpeedMetresPerSecond = 0;
    float simulationMaxRootDriftMetres = 0;
    bool depthSorted = false;
    std::uint64_t sortedStrands = 0;
    std::uint64_t orderStaleUploads = 0;
    // Sorted alpha blending, not an order-independent cache: strands are drawn from far
    // to near, but a strand crossing another can still composite in the wrong order.
    const char* transparency = "depth-sorted-alpha";
    const char* backend = "d3d12";
};

class HairDraw {
public:
    HairDraw(ID3D12Device* device, ID3D12Fence* timeline, DXGI_FORMAT colour, DXGI_FORMAT depth);
    ~HairDraw();
    HairDraw(const HairDraw&) = delete;
    HairDraw& operator=(const HairDraw&) = delete;

    // Records the point and strand buffers. Must be called before the first draw; a later
    // call replaces the previous hairstyle after the previous draw has completed.
    bool upload(ID3D12GraphicsCommandList* list, std::uint64_t fence,
                const engine::hair::GpuPack& pack, std::string& error);
    // Simulation options must be set before upload so the dynamic point buffers are
    // allocated with the pack. Calling it later re-initializes the solver from rest.
    bool setSimulation(const HairSimulation& simulation, std::string& error);
    // Restores every guide to its authored rest pose and drops the pending accumulator.
    void resetSimulation();

    // Records the strand draw. The caller owns the render target, viewport and scissors.
    bool draw(ID3D12GraphicsCommandList* list, std::uint64_t fence, const HairFrameInput& frame,
              const HairMaterial& material, const HairLighting& lighting, std::string& error);

    HairDrawStats stats() const;
    bool shutdown(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativegpu

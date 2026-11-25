#include "hair_draw.hpp"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace nativegpu {
namespace {
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) { if (p) p->Release(); p = other.p; other.p = nullptr; }
        return *this;
    }
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
};

void check(HRESULT code, const char* why) {
    if (FAILED(code)) {
        char message[192];
        std::snprintf(message, sizeof(message), "%s (HRESULT=0x%08lx)", why, static_cast<unsigned long>(code));
        throw engine::hair::Error("NG-E-HAIR-DRAW", message);
    }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC bufferDescription(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

// Mirrors HairConstants in shaders/hair.hlsl. 288 bytes: the 4x4 view-projection plus
// fourteen float4 lanes. The static_assert is the only thing keeping HLSL and C++ from
// drifting silently.
struct Constants {
    Mat4 viewProjection{};
    float cameraPosition[4]{};
    float lightDirection[4]{};
    float lightColour[4]{};
    float albedo[4]{};
    float material[4]{};
    float material2[4]{};
    float shadowU[4]{};
    float shadowV[4]{};
    float shadowOrigin[4]{};
    float viewport[4]{};
    float options[4]{};
    unsigned counts[4]{};
    float ambientColour[4]{};
    float rootTint[4]{};
    // Head attachment. The vertex shader applies `headTransform`; the pixel shader maps
    // the shaded world position back with `inverseHead` and samples the deep-opacity grid
    // with the light rotated into rest space.
    Mat4 headTransform{};
    Mat4 inverseHead{};
    float shadowLight[4]{};
};
static_assert(sizeof(Constants) == 432, "hair constants must stay 108 dwords");

constexpr unsigned kConstantBytes = 512;
constexpr unsigned kShadowSize = 192;
constexpr unsigned kShadowLayers = 4;
constexpr std::uint64_t kShadowRecords = static_cast<std::uint64_t>(kShadowSize) * kShadowSize * kShadowLayers;
constexpr D3D12_RESOURCE_STATES kBufferReadState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

struct Staging {
    Com<ID3D12Resource> resource;
    std::uint64_t fence = 0;
};

bool finite(float value) { return std::isfinite(value); }
bool range(float value, float low, float high) { return finite(value) && value >= low && value <= high; }
bool finite3(const float* value, float bound) {
    for (int axis = 0; axis < 3; ++axis)
        if (!finite(value[axis]) || std::fabs(value[axis]) > bound) return false;
    return true;
}
float clamp01(float value) { return value < 0 ? 0 : value > 1 ? 1 : value; }

struct ShadowGrid {
    bool valid = false;
    float direction[3] = {0, 0, 0};
    float sigmaPerMetre = 0;
    float densityScale = 0;
    float u[3] = {0, 0, 0}, v[3] = {0, 0, 0};
    float uMin = 0, vMin = 0, texel = 0;
    std::vector<float> records;  // (depth, optical thickness) per layer per texel
    std::uint64_t coveredTexels = 0, crossings = 0, dropped = 0;
    float maximumOpticalDepth = 0;
};

// CPU mirror of the reference renderer's deep-opacity grid. Fibre accumulates one
// diameter per texel a segment crosses, in up to four depth-sorted layers, so the lookup
// asks "how much hair lies between the light and this point" instead of "is this texel
// occupied". It is not a shadow map and does not include the body or the scene.
ShadowGrid buildShadowGrid(const engine::hair::GpuPack& pack, const float direction[3],
                           float sigmaPerMetre, float densityScale) {
    ShadowGrid grid;
    grid.sigmaPerMetre = sigmaPerMetre;
    grid.densityScale = densityScale;
    float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]
        + direction[2] * direction[2]);
    if (!(length > 1e-6f) || !finite(length)) throw engine::hair::Error("NG-E-HAIR-DRAW-LIGHT",
        "the shadow grid needs a non-zero light direction");
    const float light[3] = {direction[0] / length, direction[1] / length, direction[2] / length};
    std::copy(light, light + 3, grid.direction);
    const bool useY = std::fabs(light[1]) < 0.9f;
    const float helper[3] = {useY ? 0.0f : 1.0f, useY ? 1.0f : 0.0f, 0.0f};
    auto cross = [](const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    };
    auto normalize = [](float value[3]) {
        const float size = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (size > 1e-12f) { value[0] /= size; value[1] /= size; value[2] /= size; }
    };
    cross(helper, light, grid.u);
    normalize(grid.u);
    cross(light, grid.u, grid.v);
    normalize(grid.v);

    float uMin = std::numeric_limits<float>::infinity(), uMax = -uMin;
    float vMin = uMin, vMax = -uMin;
    auto project = [&](const engine::hair::GpuPackPoint& point, float& u, float& v, float& depth) {
        u = point.position[0] * grid.u[0] + point.position[1] * grid.u[1] + point.position[2] * grid.u[2];
        v = point.position[0] * grid.v[0] + point.position[1] * grid.v[1] + point.position[2] * grid.v[2];
        depth = point.position[0] * light[0] + point.position[1] * light[1] + point.position[2] * light[2];
    };
    for (const auto& point : pack.points) {
        float u = 0, v = 0, depth = 0;
        project(point, u, v, depth);
        uMin = std::min(uMin, u); uMax = std::max(uMax, u);
        vMin = std::min(vMin, v); vMax = std::max(vMax, v);
    }
    constexpr float margin = 0.005f;
    uMin -= margin; uMax += margin; vMin -= margin; vMax += margin;
    const float extent = std::max(uMax - uMin, vMax - vMin);
    if (!(extent > 1e-4f) || !finite(extent)) throw engine::hair::Error("NG-E-HAIR-DRAW-SHADOW",
        "the strand volume has no extent along the light");
    grid.uMin = uMin;
    grid.vMin = vMin;
    grid.texel = extent / kShadowSize;

    std::vector<float> depth(static_cast<std::size_t>(kShadowRecords), std::numeric_limits<float>::infinity());
    std::vector<float> thickness(static_cast<std::size_t>(kShadowRecords), 0.0f);
    std::vector<unsigned char> count(static_cast<std::size_t>(kShadowSize) * kShadowSize, 0);
    auto cellIndex = [&](float u, float v) -> int {
        const int x = static_cast<int>(std::floor((u - grid.uMin) / grid.texel));
        const int y = static_cast<int>(std::floor((v - grid.vMin) / grid.texel));
        if (x < 0 || y < 0 || x >= static_cast<int>(kShadowSize) || y >= static_cast<int>(kShadowSize)) return -1;
        return y * static_cast<int>(kShadowSize) + x;
    };
    auto insert = [&](int cell, float layerDepth, float optical) {
        const std::size_t base = static_cast<std::size_t>(cell) * kShadowLayers;
        const unsigned existing = count[static_cast<std::size_t>(cell)];
        unsigned slot = existing;
        while (slot > 0 && depth[base + slot - 1] > layerDepth) {
            if (slot < kShadowLayers) {
                depth[base + slot] = depth[base + slot - 1];
                thickness[base + slot] = thickness[base + slot - 1];
            }
            --slot;
        }
        // A fragment deeper than the last stored layer cannot be seen from the light along
        // this ray; that is the overflow the stats report rather than a silent drop.
        if (slot >= kShadowLayers) return false;
        depth[base + slot] = layerDepth;
        thickness[base + slot] = optical;
        if (existing < kShadowLayers)
            count[static_cast<std::size_t>(cell)] = static_cast<unsigned char>(existing + 1);
        return true;
    };

    std::vector<float> totalThickness(static_cast<std::size_t>(kShadowSize) * kShadowSize, 0.0f);
    for (const auto& strand : pack.strands) {
        for (std::uint32_t local = 0; local < pack.segmentsPerStrand; ++local) {
            const auto& first = pack.points[strand.firstPoint + local];
            const auto& second = pack.points[strand.firstPoint + local + 1];
            float ua = 0, va = 0, da = 0, ub = 0, vb = 0, db = 0;
            project(first, ua, va, da);
            project(second, ub, vb, db);
            const float distance = std::sqrt((ub - ua) * (ub - ua) + (vb - va) * (vb - va));
            const int steps = std::max(1, static_cast<int>(std::ceil(distance / (grid.texel * 0.5f))));
            const float optical = (first.widthMetres + second.widthMetres) * 0.5f * densityScale;
            int previous = -2;
            for (int step = 0; step <= steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                const int cell = cellIndex(ua + (ub - ua) * t, va + (vb - va) * t);
                if (cell < 0 || cell == previous) continue;
                previous = cell;
                totalThickness[static_cast<std::size_t>(cell)] += optical;
                ++grid.crossings;
                if (!insert(cell, da + (db - da) * t, optical)) ++grid.dropped;
            }
        }
    }
    grid.records.resize(static_cast<std::size_t>(kShadowRecords) * 2);
    for (std::size_t texel = 0; texel < totalThickness.size(); ++texel) {
        if (totalThickness[texel] > 0) ++grid.coveredTexels;
        grid.maximumOpticalDepth = std::max(grid.maximumOpticalDepth, totalThickness[texel]);
        for (unsigned layer = 0; layer < kShadowLayers; ++layer) {
            const std::size_t record = (texel * kShadowLayers + layer) * 2;
            grid.records[record] = depth[texel * kShadowLayers + layer];
            grid.records[record + 1] = thickness[texel * kShadowLayers + layer];
        }
    }
    grid.valid = true;
    return grid;
}
} // namespace

struct HairDraw::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12Fence> timeline;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    Com<ID3D12Resource> pointBuffer, strandBuffer, shadowBuffer, constants;
    Constants* mapped = nullptr;
    std::vector<Staging> staging;
    ShadowGrid grid;
    HairDrawStats stats;
    engine::hair::GpuPack uploaded;
    bool hasPack = false;

    HairSimulation simulation;
    bool simulationReady = false;
    std::uint32_t guideCount = 0, guidePointsPerGuide = 0;
    // Flattened guide state: three floats per point, guides stored point-major.
    std::vector<float> guidePositions, guidePrevious, guideRest;
    std::vector<float> guideRestLengths;
    float colliderCentre[3] = {0, 0, 0};
    float colliderRadius = 0;
    std::vector<engine::hair::GpuPackPoint> simulatedPoints;
    float accumulator = 0;
    // Gravity and wind rotated into the hairstyle's rest space. A head turn must not make
    // the solver think gravity changed direction relative to the strands.
    float simGravityRest[3] = {0, -9.81f, 0};
    float simWindRest[3] = {0, 0, 0};
    float frameMaxStretch = 1, frameMaxPenetration = 0, frameMaxSpeed = 0, frameMaxRootDrift = 0;
    struct DynamicPoints {
        Com<ID3D12Resource> resource;
        engine::hair::GpuPackPoint* mapped = nullptr;
        Com<ID3D12Resource> order;
        std::uint32_t* orderMapped = nullptr;
        std::uint64_t fence = 0;
    };
    std::vector<DynamicPoints> dynamicPoints;
    unsigned dynamicCursor = 0;
    std::vector<std::uint32_t> strandOrder;
    std::vector<float> strandDepth;

    Impl(ID3D12Device* owner, ID3D12Fence* fence, DXGI_FORMAT colour, DXGI_FORMAT depth) {
        device.p = owner; owner->AddRef();
        timeline.p = fence; fence->AddRef();
        if (!shaders::kDxcAvailable)
            throw engine::hair::Error("NG-E-HAIR-DRAW", "the pinned DXC shader blobs are unavailable");

        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (unsigned index = 0; index < 4; ++index) {
            parameters[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[index + 1].Descriptor.ShaderRegister = index;
            parameters[index + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 5;
        signature.pParameters = parameters;
        Com<ID3DBlob> bytes, diagnostics;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, bytes.out(), diagnostics.out()),
            "Serialize hair draw root");
        check(owner->CreateRootSignature(0, bytes.p->GetBufferPointer(), bytes.p->GetBufferSize(),
            IID_PPV_ARGS(root.out())), "Create hair draw root");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.p;
        state.VS = {shaders::kHairVS.data, shaders::kHairVS.size};
        state.PS = {shaders::kHairPS.data, shaders::kHairPS.size};
        state.SampleMask = UINT_MAX;
        state.SampleDesc.Count = 1;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthEnable = TRUE;
        state.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        state.DSVFormat = depth;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = colour;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        auto& blend = state.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        check(owner->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(pipeline.out())),
            "Create hair draw pipeline");

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        const auto description = bufferDescription(kConstantBytes);
        check(owner->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(constants.out())),
            "Create hair constants");
        D3D12_RANGE read{0, 0};
        check(constants.p->Map(0, &read, reinterpret_cast<void**>(&mapped)), "Map hair constants");
    }

    ~Impl() {
        releaseDynamicPoints();
        if (mapped) constants.p->Unmap(0, nullptr);
    }

    void purgeStaging() {
        const auto completed = timeline.p->GetCompletedValue();
        staging.erase(std::remove_if(staging.begin(), staging.end(),
            [completed](const Staging& item) { return item.fence <= completed; }), staging.end());
    }

    void uploadBuffer(Com<ID3D12Resource>& target, const void* data, std::uint64_t bytes,
                      ID3D12GraphicsCommandList* list, std::uint64_t fence) {
        const auto description = bufferDescription(bytes);
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(target.out())), "Create hair buffer");
        Staging item;
        item.fence = fence;
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(item.resource.out())),
            "Create hair staging buffer");
        void* mappedStaging = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        check(item.resource.p->Map(0, &range, &mappedStaging), "Map hair staging buffer");
        std::memcpy(mappedStaging, data, static_cast<std::size_t>(bytes));
        item.resource.p->Unmap(0, nullptr);
        list->CopyBufferRegion(target.p, 0, item.resource.p, 0, bytes);
        transition(list, target.p, D3D12_RESOURCE_STATE_COPY_DEST, kBufferReadState);
        staging.push_back(std::move(item));
    }

    void uploadShadow(ID3D12GraphicsCommandList* list, std::uint64_t fence, const ShadowGrid& built) {
        const std::uint64_t bytes = built.records.size() * sizeof(float);
        Staging item;
        item.fence = fence;
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        const auto description = bufferDescription(bytes);
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(item.resource.out())),
            "Create hair shadow staging");
        void* mappedShadow = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        check(item.resource.p->Map(0, &range, &mappedShadow), "Map hair shadow staging");
        std::memcpy(mappedShadow, built.records.data(), static_cast<std::size_t>(bytes));
        item.resource.p->Unmap(0, nullptr);
        transition(list, shadowBuffer.p, kBufferReadState, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(shadowBuffer.p, 0, item.resource.p, 0, bytes);
        transition(list, shadowBuffer.p, D3D12_RESOURCE_STATE_COPY_DEST, kBufferReadState);
        staging.push_back(std::move(item));
        grid = built;
        stats.shadowBuilds++;
        stats.shadowCoveredTexels = built.coveredTexels;
        stats.shadowCrossings = built.crossings;
        stats.shadowDroppedLayers = built.dropped;
        stats.shadowMaxOpticalDepthMetres = built.maximumOpticalDepth;
        stats.shadowGridSize = kShadowSize;
        stats.shadowLayers = kShadowLayers;
    }

    // --- guide simulation -------------------------------------------------
    // The flat guide table is uniform by contract, so guide boundaries are arithmetic.
    // Guide resolution is shared across LODs; a coarse strand level resamples it below.
    void buildSimulation() {
        simulationReady = false;
        if (!simulation.enabled) return;
        if (uploaded.guideCount == 0 || uploaded.guidePointCount == 0)
            throw engine::hair::Error("NG-E-HAIR-SIM", "the pack has no guides to simulate");
        if (uploaded.guidePointCount % uploaded.guideCount != 0)
            throw engine::hair::Error("NG-E-HAIR-SIM", "the guide point table is not uniform");
        guideCount = uploaded.guideCount;
        guidePointsPerGuide = uploaded.guidePointCount / guideCount;
        if (guidePointsPerGuide < 2)
            throw engine::hair::Error("NG-E-HAIR-SIM", "the guides have fewer than two points");
        const std::size_t floats = static_cast<std::size_t>(guideCount) * guidePointsPerGuide * 3;
        guidePositions.assign(floats, 0.0f);
        guidePrevious.assign(floats, 0.0f);
        guideRest.assign(floats, 0.0f);
        for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
            for (std::uint32_t point = 0; point < guidePointsPerGuide; ++point) {
                const std::size_t source = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point) * 4;
                const std::size_t destination = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point) * 3;
                for (int axis = 0; axis < 3; ++axis) {
                    guideRest[destination + axis] = uploaded.guidePoints[source + axis];
                    guidePositions[destination + axis] = uploaded.guidePoints[source + axis];
                    guidePrevious[destination + axis] = uploaded.guidePoints[source + axis];
                }
            }
        }
        guideRestLengths.assign(static_cast<std::size_t>(guideCount) * (guidePointsPerGuide - 1), 0.0f);
        for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
            for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                const std::size_t a = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point - 1) * 3;
                const std::size_t b = a + 3;
                const float dx = guideRest[b] - guideRest[a];
                const float dy = guideRest[b + 1] - guideRest[a + 1];
                const float dz = guideRest[b + 2] - guideRest[a + 2];
                guideRestLengths[static_cast<std::size_t>(guide) * (guidePointsPerGuide - 1) + point - 1]
                    = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
        // The collider is the head as seen by the roots: centroid and slightly inside the
        // nearest root, so a pinned root can never be reported as penetrating.
        float centre[3] = {0, 0, 0};
        for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
            const std::size_t root = static_cast<std::size_t>(guide) * guidePointsPerGuide * 3;
            for (int axis = 0; axis < 3; ++axis) centre[axis] += guideRest[root + axis];
        }
        for (int axis = 0; axis < 3; ++axis) centre[axis] /= static_cast<float>(guideCount);
        float nearest = std::numeric_limits<float>::infinity();
        for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
            const std::size_t root = static_cast<std::size_t>(guide) * guidePointsPerGuide * 3;
            const float dx = guideRest[root] - centre[0];
            const float dy = guideRest[root + 1] - centre[1];
            const float dz = guideRest[root + 2] - centre[2];
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        std::copy(centre, centre + 3, colliderCentre);
        colliderRadius = std::isfinite(nearest) ? nearest * 0.9f : 0.0f;
        simulatedPoints = uploaded.points;
        accumulator = 0;
        for (int axis = 0; axis < 3; ++axis) {
            simGravityRest[axis] = simulation.gravity[axis];
            simWindRest[axis] = simulation.wind[axis];
        }
        simulationReady = true;
    }

    // Rotate a world-space acceleration into rest space. Translation does not apply.
    void setRestAcceleration(const Mat4& inverse, const float gravity[3], const float wind[3]) {
        for (int axis = 0; axis < 3; ++axis) {
            simGravityRest[axis] = inverse.m[axis] * gravity[0]
                + inverse.m[4 + axis] * gravity[1] + inverse.m[8 + axis] * gravity[2];
            simWindRest[axis] = inverse.m[axis] * wind[0]
                + inverse.m[4 + axis] * wind[1] + inverse.m[8 + axis] * wind[2];
        }
    }

    void releaseDynamicPoints() {
        for (auto& buffer : dynamicPoints) {
            if (buffer.mapped) buffer.resource.p->Unmap(0, nullptr);
            if (buffer.orderMapped) buffer.order.p->Unmap(0, nullptr);
        }
        dynamicPoints.clear();
    }

    void ensureDynamicPoints() {
        if (!dynamicPoints.empty() || !hasPack || uploaded.pointCount == 0) return;
        const std::uint64_t pointBytes = static_cast<std::uint64_t>(uploaded.pointCount)
            * sizeof(engine::hair::GpuPackPoint);
        const std::uint64_t orderBytes = static_cast<std::uint64_t>(uploaded.strandCount) * sizeof(std::uint32_t);
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        const auto pointsDescription = bufferDescription(pointBytes);
        const auto orderDescription = bufferDescription(orderBytes);
        strandOrder.resize(uploaded.strandCount);
        strandDepth.resize(uploaded.strandCount);
        for (unsigned index = 0; index < uploaded.strandCount; ++index) strandOrder[index] = index;
        for (unsigned index = 0; index < 3; ++index) {
            DynamicPoints buffer;
            check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &pointsDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.resource.out())),
                "Create hair simulated point buffer");
            D3D12_RANGE pointsRange{0, static_cast<SIZE_T>(pointBytes)};
            check(buffer.resource.p->Map(0, &pointsRange, reinterpret_cast<void**>(&buffer.mapped)),
                "Map hair simulated point buffer");
            std::memcpy(buffer.mapped, uploaded.points.data(), static_cast<std::size_t>(pointBytes));

            check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &orderDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.order.out())),
                "Create hair strand order buffer");
            D3D12_RANGE orderRange{0, static_cast<SIZE_T>(orderBytes)};
            check(buffer.order.p->Map(0, &orderRange, reinterpret_cast<void**>(&buffer.orderMapped)),
                "Map hair strand order buffer");
            std::memcpy(buffer.orderMapped, strandOrder.data(), static_cast<std::size_t>(orderBytes));
            buffer.fence = 0;
            dynamicPoints.push_back(std::move(buffer));
        }
    }

    void simulateTick(float dt) {
        const int substeps = std::clamp(simulation.substeps, 1, 8);
        const float h = dt / static_cast<float>(substeps);
        float penetration = 0;
        for (int sub = 0; sub < substeps; ++sub) {
            for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                    const std::size_t at = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point) * 3;
                    for (int axis = 0; axis < 3; ++axis) {
                        float velocity = (guidePositions[at + axis] - guidePrevious[at + axis]) / h * simulation.damping;
                        const float drag = simulation.windDragPerSecond;
                        velocity += (simGravityRest[axis] + (simWindRest[axis] - velocity) * drag) * h;
                        guidePrevious[at + axis] = guidePositions[at + axis];
                        guidePositions[at + axis] += velocity * h;
                    }
                }
            }
            for (int iteration = 0; iteration < std::clamp(simulation.iterations, 1, 16); ++iteration) {
                for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                    for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                        const std::size_t a = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point - 1) * 3;
                        const std::size_t b = a + 3;
                        const float dx = guidePositions[b] - guidePositions[a];
                        const float dy = guidePositions[b + 1] - guidePositions[a + 1];
                        const float dz = guidePositions[b + 2] - guidePositions[a + 2];
                        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (length < 1e-6f) continue;
                        const float rest = guideRestLengths[static_cast<std::size_t>(guide) * (guidePointsPerGuide - 1) + point - 1];
                        const float correction = (length - rest) / length * 0.5f;
                        guidePositions[a] += dx * correction;
                        guidePositions[a + 1] += dy * correction;
                        guidePositions[a + 2] += dz * correction;
                        guidePositions[b] -= dx * correction;
                        guidePositions[b + 1] -= dy * correction;
                        guidePositions[b + 2] -= dz * correction;
                    }
                }
                if (simulation.shapeStiffness > 0) {
                    const float stiffness = std::min(1.0f, simulation.shapeStiffness);
                    for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                        for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                            const std::size_t previous = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point - 1) * 3;
                            const std::size_t at = previous + 3;
                            for (int axis = 0; axis < 3; ++axis) {
                                const float offset = guideRest[at + axis] - guideRest[previous + axis];
                                const float target = guidePositions[previous + axis] + offset;
                                guidePositions[at + axis] += (target - guidePositions[at + axis]) * stiffness;
                            }
                        }
                    }
                }
                for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                    const std::size_t root = static_cast<std::size_t>(guide) * guidePointsPerGuide * 3;
                    if (colliderRadius > 0) {
                        for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                            const std::size_t at = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point) * 3;
                            float dx = guidePositions[at] - colliderCentre[0];
                            float dy = guidePositions[at + 1] - colliderCentre[1];
                            float dz = guidePositions[at + 2] - colliderCentre[2];
                            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                            if (distance >= colliderRadius || distance < 1e-6f) continue;
                            const float scale = colliderRadius / distance;
                            guidePositions[at] = colliderCentre[0] + dx * scale;
                            guidePositions[at + 1] = colliderCentre[1] + dy * scale;
                            guidePositions[at + 2] = colliderCentre[2] + dz * scale;
                        }
                    }
                    // Pin the root and rebuild the chain outward. A final forward pass is
                    // what keeps the reported stretch at 1 under wind: pinning the root
                    // after a symmetric distance solve would otherwise re-stretch the first
                    // segment every iteration.
                    for (int axis = 0; axis < 3; ++axis) guidePositions[root + axis] = guideRest[root + axis];
                    for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                        const std::size_t previous = root + (point - 1) * 3;
                        const std::size_t at = previous + 3;
                        const float dx = guidePositions[at] - guidePositions[previous];
                        const float dy = guidePositions[at + 1] - guidePositions[previous + 1];
                        const float dz = guidePositions[at + 2] - guidePositions[previous + 2];
                        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const float rest = guideRestLengths[static_cast<std::size_t>(guide) * (guidePointsPerGuide - 1) + point - 1];
                        if (length > 1e-6f) {
                            const float scale = rest / length;
                            guidePositions[at] = guidePositions[previous] + dx * scale;
                            guidePositions[at + 1] = guidePositions[previous + 1] + dy * scale;
                            guidePositions[at + 2] = guidePositions[previous + 2] + dz * scale;
                        } else {
                            for (int axis = 0; axis < 3; ++axis)
                                guidePositions[at + axis] = guidePositions[previous + axis]
                                    + (guideRest[at + axis] - guideRest[previous + axis]);
                        }
                    }
                }
            }
        }
        // Penetration is measured after the solve, not the correction the projection made:
        // the plan asks whether hair is left inside the head, not how far it intruded.
        if (colliderRadius > 0) {
            for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                    const std::size_t at = (static_cast<std::size_t>(guide) * guidePointsPerGuide + point) * 3;
                    const float dx = guidePositions[at] - colliderCentre[0];
                    const float dy = guidePositions[at + 1] - colliderCentre[1];
                    const float dz = guidePositions[at + 2] - colliderCentre[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < colliderRadius) penetration = std::max(penetration, colliderRadius - distance);
                }
            }
        }
        float stretch = 1, speed = 0, rootDrift = 0;
        for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
            const std::size_t root = static_cast<std::size_t>(guide) * guidePointsPerGuide * 3;
            const float rdx = guidePositions[root] - guideRest[root];
            const float rdy = guidePositions[root + 1] - guideRest[root + 1];
            const float rdz = guidePositions[root + 2] - guideRest[root + 2];
            rootDrift = std::max(rootDrift, std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz));
            for (std::uint32_t point = 1; point < guidePointsPerGuide; ++point) {
                const std::size_t a = root + (point - 1) * 3;
                const std::size_t b = a + 3;
                const float dx = guidePositions[b] - guidePositions[a];
                const float dy = guidePositions[b + 1] - guidePositions[a + 1];
                const float dz = guidePositions[b + 2] - guidePositions[a + 2];
                const float rest = guideRestLengths[static_cast<std::size_t>(guide) * (guidePointsPerGuide - 1) + point - 1];
                if (rest > 1e-6f) stretch = std::max(stretch, std::sqrt(dx * dx + dy * dy + dz * dz) / rest);
                const float vx = guidePositions[b] - guidePrevious[b];
                const float vy = guidePositions[b + 1] - guidePrevious[b + 1];
                const float vz = guidePositions[b + 2] - guidePrevious[b + 2];
                speed = std::max(speed, std::sqrt(vx * vx + vy * vy + vz * vz) / h);
            }
        }
        stats.simulationMaxStretch = std::max(stats.simulationMaxStretch, stretch);
        stats.simulationMaxPenetrationMetres = std::max(stats.simulationMaxPenetrationMetres, penetration);
        stats.simulationMaxSpeedMetresPerSecond = std::max(stats.simulationMaxSpeedMetresPerSecond, speed);
        stats.simulationMaxRootDriftMetres = std::max(stats.simulationMaxRootDriftMetres, rootDrift);
    }

    void interpolateStrands() {
        const std::uint32_t guidePoints = guidePointsPerGuide;
        const auto lerpAxis = [&](std::uint32_t guide, float parameter, int axis) {
            const float scaled = std::clamp(parameter, 0.0f, static_cast<float>(guidePoints - 1));
            const std::uint32_t lower = std::min(static_cast<std::uint32_t>(scaled), guidePoints - 2);
            const float fraction = scaled - static_cast<float>(lower);
            const std::size_t a = (static_cast<std::size_t>(guide) * guidePoints + lower) * 3 + axis;
            return guidePositions[a] + (guidePositions[a + 3] - guidePositions[a]) * fraction;
        };
        for (std::uint32_t index = 0; index < uploaded.strandCount; ++index) {
            const auto& strand = uploaded.strands[index];
            const std::size_t weightBase = static_cast<std::size_t>(index) * 32;
            std::uint32_t guide[3];
            float weight[3];
            std::memcpy(guide, uploaded.guideWeights.data() + weightBase, sizeof(guide));
            std::memcpy(weight, uploaded.guideWeights.data() + weightBase + 16, sizeof(weight));
            const std::uint32_t count = strand.pointCount;
            for (std::uint32_t point = 0; point < count; ++point) {
                const float parameter = count > 1
                    ? static_cast<float>(point) / static_cast<float>(count - 1) * static_cast<float>(guidePoints - 1)
                    : 0.0f;
                auto& target = simulatedPoints[strand.firstPoint + point];
                for (int axis = 0; axis < 3; ++axis) {
                    target.position[axis] = weight[0] * lerpAxis(guide[0], parameter, axis)
                        + weight[1] * lerpAxis(guide[1], parameter, axis)
                        + weight[2] * lerpAxis(guide[2], parameter, axis);
                }
            }
            for (std::uint32_t point = 0; point < count; ++point) {
                auto& target = simulatedPoints[strand.firstPoint + point];
                const auto& before = simulatedPoints[strand.firstPoint + (point > 0 ? point - 1 : 0)];
                const auto& after = simulatedPoints[strand.firstPoint + (point + 1 < count ? point + 1 : point)];
                float tangent[3] = {after.position[0] - before.position[0],
                                    after.position[1] - before.position[1],
                                    after.position[2] - before.position[2]};
                const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
                if (length > 1e-9f) for (int axis = 0; axis < 3; ++axis) tangent[axis] /= length;
                else { tangent[0] = 0; tangent[1] = 1; tangent[2] = 0; }
                std::copy(tangent, tangent + 3, target.tangent);
            }
        }
    }

    void advanceSimulation(float dt) {
        if (!simulationReady) return;
        accumulator += std::clamp(dt, 0.0f, 0.1f);
        constexpr float tick = 1.0f / 60.0f;
        unsigned steps = 0;
        while (accumulator >= tick && steps < 4) {
            simulateTick(tick);
            accumulator -= tick;
            ++steps;
            ++stats.simulationTicks;
            stats.simulationTimeSeconds += tick;
        }
        if (steps == 4 && accumulator >= tick) accumulator = 0;  // drop debt, never spiral
        if (steps) interpolateStrands();
    }
};

HairDraw::HairDraw(ID3D12Device* device, ID3D12Fence* timeline, DXGI_FORMAT colour, DXGI_FORMAT depth) {
    if (!device || !timeline) throw engine::hair::Error("NG-E-HAIR-DRAW", "a device and timeline are required");
    Com<ID3D12Device> owner;
    check(timeline->GetDevice(IID_PPV_ARGS(owner.out())), "Query hair draw fence");
    if (owner.p != device) throw engine::hair::Error("NG-E-HAIR-DRAW-FENCE", "the fence belongs to another device");
    impl_ = std::make_unique<Impl>(device, timeline, colour, depth);
}

HairDraw::~HairDraw() = default;

bool HairDraw::upload(ID3D12GraphicsCommandList* list, std::uint64_t fence,
                      const engine::hair::GpuPack& pack, std::string& error) {
    error.clear();
    if (!impl_) { error = "NG-E-HAIR-DRAW-STATE: hair draw is not initialized"; return false; }
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "NG-E-HAIR-DRAW-STATE: a direct command list is required"; return false;
    }
    auto& self = *impl_;
    Com<ID3D12Device> owner;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(owner.out()))) || owner.p != self.device.p) {
        error = "NG-E-HAIR-DRAW-FENCE: the command list belongs to another device"; return false;
    }
    if (fence <= self.timeline.p->GetCompletedValue() || fence < self.stats.lastFence) {
        error = "NG-E-HAIR-DRAW-FENCE: the upload fence has already completed"; return false;
    }
    if (self.stats.lastFence && self.timeline.p->GetCompletedValue() < self.stats.lastFence) {
        error = "NG-E-HAIR-DRAW-FENCE: the previous hairstyle is still in flight"; return false;
    }
    if (pack.pointCount == 0 || pack.strandCount == 0 || pack.drawVertices == 0
        || pack.points.size() != pack.pointCount || pack.strands.size() != pack.strandCount
        || pack.pointStride != sizeof(engine::hair::GpuPackPoint)
        || pack.strandStride != sizeof(engine::hair::GpuPackStrand)) {
        error = "NG-E-HAIR-DRAW-STATE: the pack has nothing consistent to upload"; return false;
    }
    try {
        self.purgeStaging();
        self.pointBuffer = Com<ID3D12Resource>{};
        self.strandBuffer = Com<ID3D12Resource>{};
        self.shadowBuffer = Com<ID3D12Resource>{};
        self.grid = ShadowGrid{};
        self.hasPack = false;
        const std::uint64_t pointBytes = static_cast<std::uint64_t>(pack.pointCount) * pack.pointStride;
        const std::uint64_t strandBytes = static_cast<std::uint64_t>(pack.strandCount) * pack.strandStride;
        self.uploadBuffer(self.pointBuffer, pack.points.data(), pointBytes, list, fence);
        self.uploadBuffer(self.strandBuffer, pack.strands.data(), strandBytes, list, fence);
        const std::vector<float> zeros(static_cast<std::size_t>(kShadowRecords) * 2, 0.0f);
        self.uploadBuffer(self.shadowBuffer, zeros.data(), zeros.size() * sizeof(float), list, fence);
        self.uploaded = pack;
        self.hasPack = true;
        self.releaseDynamicPoints();
        self.simulationReady = false;
        self.stats.simulating = false;
        self.stats.simulationTicks = 0;
        self.stats.simulationStaleUploads = 0;
        self.stats.simulationTimeSeconds = 0;
        self.stats.simulationMaxStretch = 1;
        self.stats.simulationMaxPenetrationMetres = 0;
        self.stats.simulationMaxSpeedMetresPerSecond = 0;
        self.stats.simulationMaxRootDriftMetres = 0;
        self.stats.depthSorted = false;
        self.stats.sortedStrands = 0;
        self.stats.orderStaleUploads = 0;
        if (self.simulation.enabled) self.buildSimulation();
        self.ensureDynamicPoints();
        self.stats.strandCount = pack.strandCount;
        self.stats.pointCount = pack.pointCount;
        self.stats.uploadBytes = pointBytes + strandBytes;
        self.stats.shadowBuilds = 0;
        self.stats.shadowCoveredTexels = 0;
        self.stats.shadowCrossings = 0;
        self.stats.shadowDroppedLayers = 0;
        self.stats.shadowMaxOpticalDepthMetres = 0;
        self.stats.shadowGridSize = kShadowSize;
        self.stats.shadowLayers = kShadowLayers;
        // The upload is GPU work too: shutdown and a later light rebuild must wait for the
        // copies to finish rather than releasing a buffer a pending list still writes.
        self.stats.lastFence = fence;
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        return false;
    }
}

bool HairDraw::setSimulation(const HairSimulation& simulation, std::string& error) {
    error.clear();
    if (!impl_) { error = "NG-E-HAIR-SIM: hair draw is not initialized"; return false; }
    auto& self = *impl_;
    self.simulation = simulation;
    self.simulation.damping = std::clamp(simulation.damping, 0.0f, 1.0f);
    self.simulation.shapeStiffness = std::clamp(simulation.shapeStiffness, 0.0f, 1.0f);
    self.simulation.iterations = std::clamp(simulation.iterations, 1, 16);
    self.simulation.substeps = std::clamp(simulation.substeps, 1, 8);
    if (!self.hasPack) return true;  // upload() builds the state with the current options
    try {
        self.releaseDynamicPoints();
        self.simulationReady = false;
        self.stats.simulationTicks = 0;
        self.stats.simulationTimeSeconds = 0;
        self.stats.simulationMaxStretch = 1;
        self.stats.simulationMaxPenetrationMetres = 0;
        self.stats.simulationMaxSpeedMetresPerSecond = 0;
        self.stats.simulationMaxRootDriftMetres = 0;
        if (self.simulation.enabled) self.buildSimulation();
        self.ensureDynamicPoints();
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        self.simulation.enabled = false;
        return false;
    }
}

void HairDraw::resetSimulation() {
    if (!impl_ || !impl_->simulationReady) return;
    auto& self = *impl_;
    self.guidePositions = self.guideRest;
    self.guidePrevious = self.guideRest;
    self.accumulator = 0;
    self.simulatedPoints = self.uploaded.points;
    self.stats.simulationTimeSeconds = 0;
    self.stats.simulationMaxStretch = 1;
    self.stats.simulationMaxPenetrationMetres = 0;
    self.stats.simulationMaxSpeedMetresPerSecond = 0;
    self.stats.simulationMaxRootDriftMetres = 0;
    // Buffer fences are left intact: the next draw writes a buffer only after the frame
    // that read it has completed, even though the CPU state is back at rest.
}

bool HairDraw::draw(ID3D12GraphicsCommandList* list, std::uint64_t fence, const HairFrameInput& frame,
                    const HairMaterial& material, const HairLighting& lighting, std::string& error) {
    error.clear();
    if (!impl_) { error = "NG-E-HAIR-DRAW-STATE: hair draw is not initialized"; return false; }
    auto& self = *impl_;
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "NG-E-HAIR-DRAW-STATE: a direct command list is required"; return false;
    }
    if (!self.hasPack) { error = "NG-E-HAIR-DRAW-STATE: no hairstyle has been uploaded"; return false; }
    Com<ID3D12Device> owner;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(owner.out()))) || owner.p != self.device.p) {
        error = "NG-E-HAIR-DRAW-FENCE: the command list belongs to another device"; return false;
    }
    if (fence <= self.timeline.p->GetCompletedValue() || fence < self.stats.lastFence) {
        error = "NG-E-HAIR-DRAW-FENCE: the draw fence has already completed"; return false;
    }
    for (float value : frame.viewProjection.m)
        if (!finite(value)) { error = "NG-E-HAIR-DRAW-STATE: the view projection is not finite"; return false; }
    if (!finite3(frame.camera, 1e6f)) { error = "NG-E-HAIR-DRAW-STATE: the camera position is not finite"; return false; }
    if (!range(frame.viewportWidth, 16, 16384) || !range(frame.viewportHeight, 16, 16384)
        || !range(frame.tanHalfFovY, 0.01f, 4.0f) || !range(frame.nearMetres, 0.0f, 1000.0f)) {
        error = "NG-E-HAIR-DRAW-STATE: the viewport or projection is outside the supported range"; return false;
    }
    if (!range(frame.minWidthPixels, 0.1f, 32.0f) || !range(frame.maxWidthPixels, frame.minWidthPixels, 32.0f)
        || !range(frame.alphaMinimum, 0.0f, 0.5f) || frame.debugView < 0 || frame.debugView > 7) {
        error = "NG-E-HAIR-DRAW-STATE: the coverage or debug options are invalid"; return false;
    }
    if (!range(frame.deltaSeconds, 0.0f, 0.1f)) {
        error = "NG-E-HAIR-DRAW-STATE: the simulation step is outside [0, 0.1] s"; return false;
    }
    if (!finite3(material.albedo, 1.0f) || material.albedo[0] < 0 || material.albedo[1] < 0 || material.albedo[2] < 0
        || !range(material.roughness, 0.03f, 1.0f) || !range(material.radialRoughness, 0.03f, 1.0f)
        || !range(material.shiftRadians, 0.0f, 0.5f) || !finite3(material.rootToTipTint, 1.0f)) {
        error = "NG-E-HAIR-DRAW-STATE: the hair material is outside the shading contract"; return false;
    }
    if (!finite3(lighting.direction, 1e6f) || !range(lighting.intensity, 0.0f, 100.0f)
        || !finite3(lighting.colour, 1.0f) || !finite3(lighting.ambient, 1.0f)
        || !range(lighting.shadowStrength, 0.0f, 1.0f) || !range(lighting.shadowSigmaPerMetre, 0.0f, 1e5f)
        || !range(lighting.shadowDensityScale, 0.0f, 100.0f)) {
        error = "NG-E-HAIR-DRAW-STATE: the lighting is outside the scene contract"; return false;
    }
    const float directionLength = std::sqrt(lighting.direction[0] * lighting.direction[0]
        + lighting.direction[1] * lighting.direction[1] + lighting.direction[2] * lighting.direction[2]);
    if (!(directionLength > 1e-6f) || !finite(directionLength)) {
        error = "NG-E-HAIR-DRAW-STATE: the light direction is zero"; return false;
    }
    const float light[3] = {lighting.direction[0] / directionLength, lighting.direction[1] / directionLength,
                            lighting.direction[2] / directionLength};
    for (float value : frame.headTransform.m)
        if (!finite(value)) { error = "NG-E-HAIR-DRAW-STATE: the head transform is not finite"; return false; }
    Mat4 inverseHead{};
    if (!invertMatrix(frame.headTransform, inverseHead)) {
        error = "NG-E-HAIR-DRAW-STATE: the head transform is singular";
        return false;
    }
    float restLight[3] = {
        inverseHead.m[0] * light[0] + inverseHead.m[4] * light[1] + inverseHead.m[8] * light[2],
        inverseHead.m[1] * light[0] + inverseHead.m[5] * light[1] + inverseHead.m[9] * light[2],
        inverseHead.m[2] * light[0] + inverseHead.m[6] * light[1] + inverseHead.m[10] * light[2],
    };
    {
        const float length = std::sqrt(restLight[0] * restLight[0] + restLight[1] * restLight[1]
            + restLight[2] * restLight[2]);
        if (!(length > 1e-6f)) { error = "NG-E-HAIR-DRAW-STATE: the head transform collapses the light"; return false; }
        for (int axis = 0; axis < 3; ++axis) restLight[axis] /= length;
    }
    self.setRestAcceleration(inverseHead, self.simulation.gravity, self.simulation.wind);

    try {
        self.purgeStaging();
        if (lighting.shadowStrength > 0.0f) {
            const bool rebuild = !self.grid.valid
                || std::fabs(self.grid.direction[0] - restLight[0]) > 1e-4f
                || std::fabs(self.grid.direction[1] - restLight[1]) > 1e-4f
                || std::fabs(self.grid.direction[2] - restLight[2]) > 1e-4f
                || std::fabs(self.grid.sigmaPerMetre - lighting.shadowSigmaPerMetre) > 1e-3f
                || std::fabs(self.grid.densityScale - lighting.shadowDensityScale) > 1e-4f;
            if (rebuild) {
                const bool inFlight = self.stats.lastFence
                    && self.timeline.p->GetCompletedValue() < self.stats.lastFence;
                if (inFlight && self.grid.valid) {
                    // A light change mid-flight keeps last frame's grid for one frame
                    // instead of failing the whole frame. The grid is stale, not wrong
                    // for the previous light, and the next frame rebuilds it.
                } else if (inFlight) {
                    error = "NG-E-HAIR-DRAW-FENCE: the deep-opacity grid cannot rebuild while the previous draw is in flight";
                    return false;
                } else {
                    self.uploadShadow(list, fence, buildShadowGrid(self.uploaded, restLight,
                        lighting.shadowSigmaPerMetre, lighting.shadowDensityScale));
                }
            }
        }

        // Advance the guides and upload the interpolated strands. Three rotating upload
        // buffers keep a write from landing in memory the previous frame still reads; if
        // none is free the frame draws the last uploaded pose and counts the stale frame
        // instead of tearing the strands.
        if (self.dynamicPoints.empty() || self.strandOrder.size() != self.uploaded.strandCount) {
            error = "NG-E-HAIR-DRAW-STATE: the strand order buffer is unavailable";
            return false;
        }
        ID3D12Resource* pointSource = self.pointBuffer.p;
        ID3D12Resource* orderSource = nullptr;
        self.stats.simulating = false;
        self.stats.depthSorted = frame.sortStrands;
        if (self.simulation.enabled && self.simulationReady) {
            self.advanceSimulation(frame.deltaSeconds);
            self.stats.simulating = true;
        }
        const auto& positions = (self.simulation.enabled && self.simulationReady)
            ? self.simulatedPoints : self.uploaded.points;
        // Sort strands by root view depth, far to near. Indices travel in a small order
        // buffer and the vertex shader dereferences it, so the geometry buffers are not
        // rewritten just to change draw order.
        for (std::uint32_t index = 0; index < self.uploaded.strandCount; ++index) {
            self.strandOrder[index] = index;
            if (!frame.sortStrands) continue;
            const auto& root = positions[self.uploaded.strands[index].firstPoint];
            const float depth = frame.viewProjection.m[3] * root.position[0]
                + frame.viewProjection.m[7] * root.position[1]
                + frame.viewProjection.m[11] * root.position[2]
                + frame.viewProjection.m[15];
            // A strand behind the eye has no meaningful order; draw it with the far group.
            self.strandDepth[index] = depth > 0 ? depth : 1e9f;
        }
        if (frame.sortStrands) {
            std::stable_sort(self.strandOrder.begin(), self.strandOrder.end(),
                [&](std::uint32_t left, std::uint32_t right) {
                    return self.strandDepth[left] > self.strandDepth[right];
                });
            self.stats.sortedStrands += self.uploaded.strandCount;
            self.stats.transparency = "depth-sorted-alpha";
        } else {
            self.stats.transparency = "upload-order-alpha";
        }
        {
            const std::uint64_t pointBytes = static_cast<std::uint64_t>(self.uploaded.pointCount)
                * sizeof(engine::hair::GpuPackPoint);
            const std::uint64_t orderBytes = static_cast<std::uint64_t>(self.uploaded.strandCount)
                * sizeof(std::uint32_t);
            const unsigned pick = self.dynamicCursor++ % static_cast<unsigned>(self.dynamicPoints.size());
            auto& buffer = self.dynamicPoints[pick];
            const bool safe = buffer.fence == 0 || self.timeline.p->GetCompletedValue() >= buffer.fence;
            if (safe) {
                std::memcpy(buffer.mapped, positions.data(), static_cast<std::size_t>(pointBytes));
                std::memcpy(buffer.orderMapped, self.strandOrder.data(), static_cast<std::size_t>(orderBytes));
                buffer.fence = fence;
            } else {
                ++self.stats.orderStaleUploads;
                if (self.stats.simulating) ++self.stats.simulationStaleUploads;
            }
            pointSource = buffer.resource.p;
            orderSource = buffer.order.p;
        }

        const float lightness = clamp01(0.2126f * material.albedo[0] + 0.7152f * material.albedo[1]
            + 0.0722f * material.albedo[2]);
        // Calibration from engine/hair/shading.mjs: light hair scatters more and shows a
        // broad highlight; dark hair has a narrow, stronger highlight and transmits less.
        const float diffuseScale = 0.55f + 0.45f * lightness;
        const float specularScale = 1.45f - 0.85f * lightness;
        const float transmissionScale = 0.12f + 0.88f * lightness * lightness;
        const float longitudinalScale = 1.25f - 0.5f * lightness;
        const float radialScale = 1.35f - 0.45f * lightness;
        const float rootToTipScale = 0.55f + 0.45f * lightness;

        Constants constants{};
        constants.viewProjection = frame.viewProjection;
        constants.headTransform = frame.headTransform;
        constants.inverseHead = inverseHead;
        for (int axis = 0; axis < 3; ++axis) {
            constants.shadowLight[axis] = restLight[axis];
            constants.cameraPosition[axis] = frame.camera[axis];
            constants.lightDirection[axis] = light[axis];
            constants.lightColour[axis] = lighting.colour[axis];
            constants.albedo[axis] = material.albedo[axis];
            constants.ambientColour[axis] = lighting.ambient[axis];
            constants.rootTint[axis] = material.rootToTipTint[axis];
            constants.shadowU[axis] = self.grid.u[axis];
            constants.shadowV[axis] = self.grid.v[axis];
        }
        constants.lightDirection[3] = lighting.intensity;
        constants.albedo[3] = material.roughness * longitudinalScale;
        constants.material[0] = material.radialRoughness * radialScale;
        constants.material[1] = material.shiftRadians;
        constants.material[2] = rootToTipScale;
        constants.material[3] = specularScale;
        constants.material2[0] = diffuseScale;
        constants.material2[1] = transmissionScale;
        constants.material2[2] = 1.0f;
        constants.material2[3] = lighting.shadowStrength;
        constants.shadowU[3] = static_cast<float>(kShadowSize);
        constants.shadowV[3] = static_cast<float>(kShadowLayers);
        constants.shadowOrigin[0] = self.grid.uMin;
        constants.shadowOrigin[1] = self.grid.vMin;
        constants.shadowOrigin[2] = self.grid.texel > 0 ? self.grid.texel : 1.0f;
        constants.shadowOrigin[3] = lighting.shadowSigmaPerMetre;
        constants.viewport[0] = frame.viewportWidth;
        constants.viewport[1] = frame.viewportHeight;
        constants.viewport[2] = frame.tanHalfFovY;
        constants.viewport[3] = frame.nearMetres;
        constants.options[0] = frame.minWidthPixels;
        constants.options[1] = frame.maxWidthPixels;
        constants.options[2] = frame.alphaMinimum;
        constants.options[3] = static_cast<float>(frame.debugView);
        constants.counts[0] = self.uploaded.strandCount;
        constants.counts[1] = self.uploaded.pointCount;
        constants.counts[2] = self.uploaded.segmentsPerStrand;
        constants.counts[3] = 0;
        std::memcpy(self.mapped, &constants, sizeof(constants));

        list->SetGraphicsRootSignature(self.root.p);
        list->SetPipelineState(self.pipeline.p);
        list->SetGraphicsRootConstantBufferView(0, self.constants.p->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(1, pointSource->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(2, self.strandBuffer.p->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(3, self.shadowBuffer.p->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(4, orderSource->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(self.uploaded.drawVertices, 1, 0, 0);
        ++self.stats.frames;
        self.stats.drawnStrands += self.uploaded.strandCount;
        self.stats.drawnVertices += self.uploaded.drawVertices;
        self.stats.triangles += self.uploaded.drawVertices / 3;
        self.stats.lastFence = fence;
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        return false;
    }
}

HairDrawStats HairDraw::stats() const {
    return impl_ ? impl_->stats : HairDrawStats{};
}

bool HairDraw::shutdown(std::string& error) {
    error.clear();
    if (!impl_) return true;
    if (impl_->stats.lastFence && impl_->timeline.p->GetCompletedValue() < impl_->stats.lastFence) {
        error = "NG-E-HAIR-DRAW-FENCE: the draw pipeline is in use";
        return false;
    }
    impl_.reset();
    return true;
}

} // namespace nativegpu

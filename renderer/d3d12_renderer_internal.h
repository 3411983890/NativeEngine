#pragma once

// Internal renderer declarations shared by d3d12_renderer.cpp (device, targets, frame
// graph, composite, screenshots) and rt_shadows.cpp (DXR state object, SBT, dispatch).
// Nothing in this header is part of the public API.

#include "d3d12_renderer.h"
#include "batching.hpp"
#include "lighting_quality.h"
#include "gpu_effect_sprites.hpp"
#include "../engine/native/cloth/batch_gpu.hpp"
#include "../engine/native/effects/field_deposits.hpp"
#include "../engine/native/smoke/service.hpp"
#include "../engine/native/smoke/sharing.hpp"
#include "../engine/native/fields/effect_services.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nativegpu {
namespace detail {

constexpr unsigned FrameCount = 3;
constexpr unsigned MaxSkinInstances = 64;
constexpr unsigned SkinBindingCount = FrameCount * MaxSkinInstances;
constexpr DWORD FenceTimeoutMs = 5000;

constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT ShadowMaskFormat = DXGI_FORMAT_R16_FLOAT;
constexpr DXGI_FORMAT DepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT DepthDsvFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT DepthSrvFormat = DXGI_FORMAT_R32_FLOAT;

// Bounded, generation-checked material table. Slot 0 is the default "missing texture"
// slot and is never released; the rest can be acquired, released and reused.
constexpr unsigned MaterialSlotCount = 8;
constexpr unsigned MaterialDefaultSlot = 0;

struct MaterialSlot {
    unsigned generation = 1;
    bool inUse = false;
    unsigned textureId = 0;
};

struct MaterialTable {
    MaterialSlot slots[MaterialSlotCount];
    unsigned acquired = 0;
    unsigned released = 0;
    unsigned reused = 0;
    unsigned missingFallbacks = 0;
    unsigned staleRejections = 0;
    unsigned generationHighWater = 1;

    MaterialTable();
    unsigned acquire(unsigned textureId, unsigned& outGeneration);
    bool release(unsigned slot);
    bool resolve(unsigned slot, unsigned generation, unsigned& outTexture,
                 unsigned& outGeneration);
};

// Maximum rigid instances in a top-level acceleration structure. Clamped and reported
// instead of silently truncating. The town scene uses ~600 boxes, so the ceiling has to
// be comfortably above the old courtyard-sized 256.
constexpr unsigned MaxInstances = 1024;

// Shader-visible CBV/SRV/UAV heap. Slot 0 is the material texture table; every frame
// index owns a contiguous block of eight descriptors so a pass can bind one table.
enum DescriptorSlot : unsigned {
    SlotMaterialTable = 0,
    SlotMeshVertices = 1,
    SlotMeshTriangles = 2,
    SlotMeshlets = 3,
    SlotMaterialRecords = 4,
    SlotMaterialLayers = 5,
    FrameDescriptorBase = 8,
    FrameDescriptorStride = 10
};

enum FrameDescriptorSlot : unsigned {
    FdsDepth = 0,
    FdsTopLevelAccelerationStructure = 1,
    FdsShadowMaskUav = 2,
    FdsBaseColor = 3,
    FdsKeyLight = 4,
    FdsShadowMaskSrv = 5,
    FdsComputeResult = 6,
    FdsDepthComposite = 7,
    FdsTransparencyHeads = 8,
    FdsTransparencyNodes = 9,
    FdsCount = 10
};

constexpr unsigned DescriptorSlotCount =
    FrameDescriptorBase + FrameCount * FrameDescriptorStride;

// Four SRVs live above the frame blocks so the compute probe can prove that
// ResourceDescriptorHeap resolves indices a bounded 8-slot table could never address.
constexpr unsigned VerificationSrvBase = DescriptorSlotCount;
constexpr unsigned VerificationSrvCount = 4;
constexpr unsigned ComputeResultUavSlot = VerificationSrvBase + VerificationSrvCount;

// Particle field descriptors live above the compute probe's, so neither feature can
// silently overwrite the other's views.
constexpr unsigned ParticleFrameBase = ComputeResultUavSlot + 1;
constexpr unsigned ParticleFrameStride = 16;
constexpr unsigned ParticleColliderSrv = ParticleFrameBase + FrameCount * ParticleFrameStride;

// Cooked textures get one SRV each above every other block. The shader reads them through
// ResourceDescriptorHeap, so the heap must be large enough for the whole budget.
constexpr unsigned CookedTextureSrvBase = 128;
constexpr unsigned MaxCookedTextures = 256;
constexpr unsigned SkinDeformSrvBase = QualityDescriptorBase + QSlotCount;
constexpr unsigned EffectTextureSrvBase = SkinDeformSrvBase + SkinBindingCount * 2;
constexpr unsigned MaxEffectTextures = 128;
constexpr unsigned TransparencyDescriptorBase = EffectTextureSrvBase + MaxEffectTextures;
constexpr unsigned TransparencyDescriptorStride = 5;
constexpr unsigned SmokeDescriptorBase = TransparencyDescriptorBase + FrameCount * TransparencyDescriptorStride;
constexpr unsigned SmokeIncidentSrv = SmokeDescriptorBase + FrameCount * MaxSmokeMedia;
constexpr unsigned DescriptorHeapSlots = SmokeIncidentSrv + 2;
constexpr unsigned MaxEffectSprites = 262144;
static_assert(QualityDescriptorBase>=CookedTextureSrvBase+MaxCookedTextures,
              "Lighting descriptors overlap the cooked texture budget");

// Screen-sized particle volume targets (half-res march result and temporal history) are
// shared across frames, so their descriptors sit once above the per-frame blocks.
constexpr unsigned ParticleGlobalBase = ParticleColliderSrv + 1;
enum ParticleGlobalSlot : unsigned {
    PgsHalfSrv = 0,
    PgsHistoryUav = 1,
    PgsHistorySrv = 2,
    PgsDispatchArgsUav = 3,
    PgsCount = 4
};
static_assert(ParticleGlobalBase + PgsCount <= CookedTextureSrvBase,
              "Particle global descriptors overlap the cooked texture budget");

// Obstacle capacity for the particle collision pass. The courtyard has fewer than ten
// colliders; the clamp is reported instead of silently dropping obstacles.
constexpr unsigned ParticleColliderCapacity = 32;

// The solver keeps every field texture in UNORDERED_ACCESS and reads it through
// ResourceDescriptorHeap with manual trilinear filtering, so no pass needs an SRV
// transition. Only the density/heat integer volumes need SRVs (for the resolve), plus one
// state SRV for the volumetric draw, and the depth SRV sits right after it so the volume
// pass can bind both through one contiguous table.
enum ParticleFrameSlot : unsigned {
    PfsParticleUav = 0,
    PfsDensityIntUav = 1,
    PfsHeatIntUav = 2,
    PfsDensityIntSrv = 3,
    PfsHeatIntSrv = 4,
    PfsVelocityAUav = 5,
    PfsVelocityBUav = 6,
    PfsStateAUav = 7,
    PfsStateBUav = 8,
    PfsPressureAUav = 9,
    PfsPressureBUav = 10,
    PfsDivergenceUav = 11,
    PfsStateASrv = 12,
    PfsDepthSrv = 13,
    PfsCurlUav = 14,
    // Third state buffer for the MacCormack correction pass (FIELD-REALISM-GOAL batch 2).
    PfsStateCUav = 15
};

// Field capacity. 64^3 over a 16 m box gives 25 cm voxels; enough to read as a plume
// without the memory or the resolve cost of a finer grid.
constexpr unsigned ParticleCapacity = 65536;
constexpr unsigned ParticleVolumeDim = 96;

// Field channels a consumer can lease. Every solver pass reads or writes one of these, so a
// frame with an unleased required channel is skipped instead of running on stale data.
enum class FieldChannel : unsigned {
    Density = 0,
    Heat,
    Velocity,
    Pressure,
    Divergence,
    Curl,
    Count
};
constexpr unsigned FieldChannelCount = static_cast<unsigned>(FieldChannel::Count);

// Strong = must be produced this frame; Eventual = a few frames of staleness are allowed;
// Stale = rendering-only data that may lag. The lease keeps the strongest level requested.
enum class FieldConsistency : unsigned { Stale = 0, Eventual = 1, Strong = 2 };

struct ChannelLease {
    unsigned references = 0;
    FieldConsistency consistency = FieldConsistency::Stale;
    unsigned acquisitions = 0;
    unsigned lastFrame = 0;
};

constexpr const char* FieldChannelName(FieldChannel channel) {
    switch (channel) {
    case FieldChannel::Density: return "density";
    case FieldChannel::Heat: return "heat";
    case FieldChannel::Velocity: return "velocity";
    case FieldChannel::Pressure: return "pressure";
    case FieldChannel::Divergence: return "divergence";
    case FieldChannel::Curl: return "curl";
    default: return "unknown";
    }
}

// RTV heap: one swap chain target per frame, then three offscreen targets per frame
// (base colour, key light, composite), plus one half-resolution target for the temporal
// volume path (docs/FIELD-REALISM-GOAL.md).
constexpr unsigned RtvSlotBase = FrameCount;
constexpr unsigned RtvSlotsPerFrame = 3;
constexpr unsigned RtvSlotCount = FrameCount * (1 + RtvSlotsPerFrame);
constexpr unsigned RtvVolumeHalf = RtvSlotCount;
constexpr unsigned RtvTotalSlots = RtvSlotCount + 1;

// Four timestamps per buffered frame: whole frame, and the DXR dispatch inside it.
enum TimestampSlot : unsigned {
    TimestampFrameBegin = 0,
    TimestampRtBegin = 1,
    TimestampRtEnd = 2,
    TimestampFrameEnd = 3,
    TimestampQualityBegin = 4,
    TimestampProbeEnd = 5,
    TimestampCloudShadowEnd = 6,
    TimestampFogBegin = 7,
    TimestampFogEnd = 8,
    TimestampCloudEnd = 9,
    TimestampLocalShadowCacheEnd = 10,
    TimestampGpuEffectsBegin = 11,
    TimestampGpuEffectsEnd = 12,
    TimestampGpuEffectDrawBegin = 13,
    TimestampGpuEffectDrawEnd = 14,
    TimestampGpuClothBegin = 15,
    TimestampGpuClothEnd = 16,
    TimestampGpuClothDrawBegin = 17,
    TimestampGpuClothDrawEnd = 18,
    TimestampsPerFrame = 19
};

extern long g_liveObjects;
extern unsigned long long g_objectsCreated;

template<class T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

std::string hresultText(HRESULT hr);
bool checked(HRESULT hr, const char* action, std::string& error);
std::string utf8(const wchar_t* input);
std::wstring utf16(const char* input);
std::string jsonString(const std::string& input);
D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type);
D3D12_RESOURCE_DESC bufferDescription(UINT64 size);
D3D12_RESOURCE_DESC textureDescription2D(unsigned width, unsigned height,
                                        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags);
D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after);

// A resource plus the state the renderer last transitioned it to. Barriers are only
// emitted when the state actually changes, which keeps the frame graph readable.
struct TrackedResource {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

    explicit operator bool() const { return resource != nullptr; }
};

// Root-constant layouts. All must stay 16-byte aligned to match the HLSL cbuffers.
// 58 DWORDs + two root CBVs (2+2) + one descriptor table (1) = 63, under the D3D12 limit
// of 64. The next material field has to move into a GPU buffer instead of this struct.
struct DrawConstants {
    Mat4 model;
    Mat4 viewProjection;
    Mat4 normalMatrix;
    float color[4];
    float roughness;
    float metalness;
    unsigned albedoIndex;
    unsigned normalIndex;
    unsigned ormIndex;
    float uvScale;
};
static_assert(sizeof(DrawConstants) == 58 * sizeof(float), "Root constant packing changed");

// One light as the cooked-material shader sees it. 64 B, four float4 lanes so the HLSL
// cbuffer packing is unambiguous. type 0 = point, 1 = spot; direction points away from
// the light. cosOuter/cosInner are only meaningful for spots.
struct GpuLight {
    float positionRange[4];
    float colorIntensity[4];
    float directionCos[4];
    float params[4]; // x=type, y=cosInner, z=flags, w=padding
};
static_assert(sizeof(GpuLight) == 16 * sizeof(float), "GpuLight packing changed");

// One cooked material as the bindless shader sees it. Texture fields are absolute SRV
// indices; normalIndex bit 31 is still the BC5 flag.
struct GpuMaterial {
    unsigned albedo;
    unsigned normal;
    unsigned orm;
    unsigned flags;
    float uvScale;
    float roughness;
    float metalness;
    float wetness;
    float wear;
    float clearcoat;
    float clearcoatRoughness;
    float alphaCutoff;
    unsigned emissive;
    float emissiveFactor[3];
    float emissiveStrength;
    float opacity;
    float dielectricF0;
    unsigned coat;
    unsigned occlusion;
    float occlusionStrength;
    unsigned coatRoughness;
    unsigned coatNormal;
    engine::TextureMapping textureMappings[8];
    unsigned layerCount;
    unsigned layerOffset;
    unsigned layerDescriptor;
    engine::material::layered::GpuSurfaceRelief surfaceRelief;
};
static_assert(sizeof(GpuMaterial) == (88 + 3) * sizeof(float) + sizeof(engine::material::layered::GpuSurfaceRelief), "GpuMaterial packing changed");

struct SceneConstants {
    // Legacy prefix: the SM 5.1 fallback shader reads exactly these 44 floats.
    float ambient[4];
    float lightPositionRange[4][4];
    float lightColorIntensity[4][4];
    unsigned lightCount;
    // Key-light tint for the legacy raster shader. It baked a fixed 0.95 white key term, so
    // a night scene stayed daylight-bright. Carrying keyColor * (keyIntensity / 1.30) here
    // keeps the courtyard frame byte-identical (white, 1.30 -> 1.0) while letting a night
    // scene go dark. The slot is the old padding, so the 44-float legacy prefix is unchanged.
    float keyTint[3];
    // Appended after the legacy fields so the old shader layout stays byte-compatible.
    float cameraPosition[4];
    // Cooked path: up to 64 lights, key/fog parameters. The legacy path ignores these.
    GpuLight lights[64];
    float keyDirectionIntensity[4]; // xyz = direction to key, w = intensity
    float keyColor[4];
    float fogParams[4];             // x=density, y=heightFalloff, z=baseHeight, w=0
    float fogColor[4];
    unsigned gpuLightCount;
    unsigned qualitySlot;
    unsigned transparencyBase;
    unsigned transparencyCapacity;
    // Preview-only layered debug view (0 off, 1 coverage, 2 height, 3 layer albedo).
    unsigned layerDebugMode;
    unsigned layerDebugLayer;
    unsigned layerDebugPadding[2];
};
static_assert(sizeof(SceneConstants) == 1092 * sizeof(float), "Scene constant packing changed");
constexpr unsigned SceneConstantStride=(sizeof(SceneConstants)+255u)&~255u;

struct ShadowConstants {
    Mat4 invViewProjection;
    float cameraPosition[4];
    float keyLightDirection[4];
    float traceParams[4];
    float depthParams[4];
};
static_assert(sizeof(ShadowConstants) == 32 * sizeof(float), "Shadow constant packing changed");

// A small spotlight payload for fog scattering. Eight is enough for the four cars'
// headlights; the full 64-light buffer stays on the surface-shading path.
constexpr unsigned CompositeMaxLights = 8;
struct CompositeLight {
    float positionRange[4];
    float colorIntensity[4];
    float directionCos[4];
    float params[4]; // x = type, y = cosInner
};
static_assert(sizeof(CompositeLight) == 16 * sizeof(float), "CompositeLight packing changed");

// Composite needs the inverse view-projection to reconstruct world positions, plus the
// fog/moon/cloud parameters for the screen-space volumetric pass.
struct CompositeSceneConstants {
    Mat4 invViewProjection;
    float cameraPosition[4];
    float fogParams[4];      // x=density, y=heightFalloff, z=baseHeight, w=maxDistance
    float fogColor[4];       // rgb = color, w = ambient scale
    float moonDirection[4];  // xyz = direction to moon, w = intensity
    float moonColor[4];
    float cloudParams[4];    // x=time, y=coverage, z=height, w=thickness
    CompositeLight lights[CompositeMaxLights];
    unsigned lightCount;
    float lightPadding[3];
    SmokeScene smoke;
};
static_assert(sizeof(CompositeSceneConstants) == 172 * sizeof(float) + sizeof(SmokeScene),
              "Composite scene constant packing changed");
constexpr unsigned CompositeConstantStride=(sizeof(CompositeSceneConstants)+255u)&~255u;

} // namespace detail

struct Renderer::Impl {
    using TrackedResource = detail::TrackedResource;
    using FieldChannel = detail::FieldChannel;
    using FieldConsistency = detail::FieldConsistency;
    using ChannelLease = detail::ChannelLease;

    // World-space lighting. Persistent histories are ordered on the graphics queue;
    // constants and instance uploads have a separate allocation for each in-flight frame.
    bool qualityReady = false, qualityHistoryValid = false, qualityInitialized = false;
    bool qualityEnvironmentDirty = true;
    std::string qualityError;
    ID3D12RootSignature* qualityRoot = nullptr;
    ID3D12PipelineState* qualityPipelines[10]{};
    ID3D12PipelineState* qualityCompositePipeline = nullptr;
    ID3D12Resource* qualityConstantsBuffer = nullptr;
    void* qualityConstantsMapped = nullptr;
    ID3D12Resource* qualityInstances[detail::FrameCount]{};
    void* qualityInstancesMapped[detail::FrameCount]{};
    TrackedResource qualityIbl, qualityBrdf, qualityNoise, qualityWeather, qualityCloudShadow;
    TrackedResource environmentTexture,environmentInlineTexture;
    ID3D12Resource* environmentUpload=nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT environmentFootprint{},environmentInlineFootprints[8]{};
    std::string environmentTextureHash;
    bool environmentCopyPending=false;
    ID3D12PipelineState* inlineEnvironmentPipeline=nullptr;
    ID3D12PipelineState* inlineEnvironmentSkinPipeline=nullptr;
    TrackedResource qualityLocalShadowCache;
    UINT64 qualityStaticGeometryHash = 0;
    unsigned qualityCachedLightIds[4]{}, qualityCachedLightCount = 0, qualityCacheBuilds = 0;
    float qualityCachedLightPositions[4][4]{};
    TrackedResource qualityProbeIrradiance[2], qualityProbeDistance[2], qualityProbeData[2];
    TrackedResource qualityFogHistory[2], qualityFogScratch, qualityFogIntegrated, qualityFogIncident;
    TrackedResource qualityCloud[2], qualityCloudDepth[2];
    detail::QualityConstants qualityParams{};
    Mat4 qualityPreviousViewProjection = identityMatrix();
    float qualityPreviousEye[3]{}, qualityPreviousForward[3]{};
    float qualityProbeOrigin[3]{};
    unsigned qualityGrid[3] = {80, 45, 48};
    unsigned qualityShadowIds[4]{};
    float qualityShadowWeights[4]{};
    unsigned qualityShadowCount = 0, qualityFrames = 0, qualityHistoryResets = 0;
    UINT64 qualityTextureBytes = 0;
    double qualityProbeMs = 0, qualityCloudShadowMs = 0, qualityFogMs = 0, qualityCloudMs = 0;
    double qualityLocalShadowCacheMs = 0;
    std::vector<double> qualityLocalShadowCacheTimes;
    std::vector<double> qualityProbeTimes, qualityCloudShadowTimes, qualityFogTimes, qualityCloudTimes;
    bool createQuality(std::string& error);
    bool prepareEnvironmentTexture(std::string& error);
    void releaseEnvironmentTexture();
    void recordEnvironmentTexture(ID3D12GraphicsCommandList* list);
    void releaseQuality();
    bool prepareQuality(unsigned frame, const Mat4& vp, const float eye[3],
                        const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                        bool tlasValid, std::string& error);
    void dispatchQualityVolumes(unsigned frame);
    void compositeQuality(unsigned frame);
    std::string qualityJson() const;

    // --- device and swap chain ---
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 nextFence = 1;
    UINT64 frameFences[detail::FrameCount]{};

    // --- descriptors ---
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12DescriptorHeap* materialSamplerHeap = nullptr;
    UINT rtvStride = 0;
    UINT srvStride = 0;

    // --- command recording ---
    ID3D12CommandAllocator* allocators[detail::FrameCount]{};
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12GraphicsCommandList4* commands4 = nullptr;
    // Dedicated pair for out-of-band readbacks so screenshots never disturb frame
    // recording state.
    ID3D12Device2* device2 = nullptr;
    ID3D12GraphicsCommandList6* commands6 = nullptr;

    // --- pipeline objects ---
    ID3D12RootSignature* rasterRootSignature = nullptr;
    ID3D12RootSignature* compositeRootSignature = nullptr;
    bool compositeMediaActive=false;
    WaterSurfaceFrame waterSurface;
    std::unique_ptr<WaterDraw> waterDraw;
    std::unique_ptr<WaterRipple> waterRipple;
    unsigned waterRippleEvents=0;
    // Optional second (upstream) body. It is a full water pass, not a visual-only sheet;
    // the falling ribbon between the two surfaces is drawn separately.
    WaterSurfaceFrame upperWaterSurface;
    std::unique_ptr<WaterDraw> upperWaterDraw;
    std::unique_ptr<WaterRipple> upperWaterRipple;
    unsigned upperWaterRippleEvents=0;
    std::vector<WaterfallFrame> waterfalls;
    std::unique_ptr<WaterfallDraw> waterfallDraw;
    ID3D12RootSignature* rtRootSignature = nullptr;
    ID3D12PipelineState* rasterPipeline = nullptr;
    ID3D12PipelineState* texturedPipeline = nullptr;
    ID3D12PipelineState* bindlessPipeline = nullptr;
    ID3D12PipelineState* bindlessSkinPipeline = nullptr;
    ID3D12PipelineState* transparentPipeline = nullptr;
    ID3D12PipelineState* transparentSkinPipeline = nullptr;
    TrackedResource transparencyHeads,transparencyCoverage,transparencyNodes,transparencyCounter;
    ID3D12Resource* transparencyReadback=nullptr;
    ID3D12DescriptorHeap* transparencyCpuHeap=nullptr;
    ID3D12DescriptorHeap* shadowClearHeap=nullptr;
    unsigned transparencyWidth=0,transparencyHeight=0,transparencyCapacity=0;
    bool transparencyActive=false,transparencyPending[detail::FrameCount]{};
    unsigned transparencyDraws=0,transparencyFragments=0,transparencyPeakFragments=0,transparencyOverflowFrames=0;
    ID3D12PipelineState* skinPipeline = nullptr;
    ID3D12PipelineState* compositePipeline = nullptr;

    // --- geometry and material resources ---
    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    ID3D12Resource* materialTexture = nullptr;
    ID3D12Resource* materialRecords = nullptr;
    ID3D12Resource* materialLayerRecords = nullptr;
    unsigned layerDebugMode = 0, layerDebugLayer = 0;
    ID3D12Resource* sceneConstantBuffer = nullptr;
    detail::SceneConstants* sceneConstantMapped = nullptr;
    ID3D12Resource* compositeConstantBuffer = nullptr;
    detail::CompositeSceneConstants* compositeConstantMapped = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    // Convex mesh pool. Every configured ConvexMesh is concatenated into one vertex and
    // one index buffer, so binding a mesh is a buffer offset rather than a resource
    // switch. Ranges are recorded in bytes to feed D3D12 buffer views directly.
    struct ConvexRange { unsigned vertexOffset=0; unsigned vertexBytes=0; unsigned indexOffset=0; unsigned indexCount=0; bool hasTextureUvs=false; bool hasLayerUvs=false;
                         float boundsMin[3]{}; float boundsMax[3]{}; };
    std::vector<engine::ConvexMesh> configuredConvexMeshes;
    struct VegetationRuntimeLayer {
        std::unique_ptr<VegetationDraw> draw;
        VegetationMaterial material;
        float bounds[3]{1.0f, 1.0f, 0.0f};
    };
    std::vector<VegetationRuntimeLayer> vegetation;
    // One resident hairstyle. The pack is uploaded once; the pass draws the rest-pose
    // strands with the scene's depth buffer and environment light.
    struct HairRuntime {
        std::unique_ptr<HairDraw> draw;
        HairMaterial material;
        float shadowStrength = 0.85f;
        float shadowSigmaPerMetre = 2400.0f;
        float shadowDensityScale = 1.0f;
        float lightIntensityScale = 1.0f;
        int debugView = 0;
        HairSimulation simulation;
    };
    std::unique_ptr<HairRuntime> hair;
    Mat4 hairHeadTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<ConvexRange> convexRanges;
    ID3D12Resource* convexVertexBuffer = nullptr;
    ID3D12Resource* convexIndexBuffer = nullptr;
    bool convexMeshesReady = false;
    unsigned convexMeshBytes = 0;
    struct SkinGeometry {
        std::string key;
        ID3D12Resource *vertices=nullptr,*indices=nullptr,*morphs=nullptr,*zeroDeformation=nullptr;
        unsigned vertexCount=0,indexCount=0,morphCount=0;
    };
    std::vector<SkinGeometry> skinGeometry;
    ID3D12Resource* skinPalette[detail::SkinBindingCount]{};
    ID3D12Resource* skinDeformation[detail::SkinBindingCount]{};
    std::size_t skinDeformationBytes[detail::SkinBindingCount]{};
    unsigned skinGeometrySlot[detail::SkinBindingCount]{};
    std::size_t skinMemoryBytes=0,skinGeometryBytes=0;
    unsigned skinBindingsLastFrame=0,skinBindingsHighWater=0,skinGeometryUploads=0;
    unsigned morphDraws=0, softDraws=0;
    unsigned skinDraws=0;
    bool prepareSkin(unsigned index, const DrawItem& item, std::string& error);
    bool materialTableReady = false;
    unsigned materialSlots = detail::MaterialSlotCount;
    // Cooked material path. When configured, materialTexture is a true mipmapped array
    // and every draw resolves through configuredMaterials.
    std::vector<TextureUpload> configuredTextures;
    std::vector<MaterialUpload> configuredMaterials;
    bool materialPath = false;
    bool materialBindless = false;
    // One Texture2D per cooked texture when the bindless path is active.
    std::vector<ID3D12Resource*> cookedTextures;
    unsigned materialTextureWidth = 0;
    unsigned materialTextureHeight = 0;
    unsigned materialTextureMips = 0;
    unsigned materialTextureSlices = 0;
    bool uploadCookedTextures(std::string& error);
    bool uploadTextureSet(const std::vector<TextureUpload>& sources, unsigned srvBase,
                          std::vector<ID3D12Resource*>& textures, std::string& error);
    std::vector<TextureUpload> configuredEffectTextures;
    std::vector<ID3D12Resource*> effectTextures;
    std::vector<EffectSprite> effectSprites;
    ID3D12RootSignature* effectSpriteRoot = nullptr;
    ID3D12PipelineState* effectSpritePipelines[3]{};
    ID3D12Resource* effectSpriteBuffers[detail::FrameCount]{};
    unsigned effectSpriteCapacity[detail::FrameCount]{};
    unsigned effectSpriteDraws = 0, effectSpriteFrames = 0, effectSpritesRendered = 0;
    bool createEffectSprites(std::string& error);
    void releaseEffectSprites();
    bool drawEffectSprites(ID3D12GraphicsCommandList* list, unsigned index,
                           const Mat4& viewProjection, const float camera[3], std::string& error);
    std::string effectSpritesJson() const;
    std::vector<std::shared_ptr<const engine::effects::Asset>> configuredGpuEffects;
    std::map<std::string,unsigned> configuredGpuEffectTextures;
    GpuEffectDriver gpuEffectDriver;
    std::shared_ptr<engine::fields::World> configuredGpuFieldWorld;
    engine::fields::GpuLimits configuredGpuFieldLimits;
    engine::fields::GpuVectorLimits configuredGpuVectorLimits;
    std::shared_ptr<engine::fields::GpuFieldStorage> gpuFieldStorage;
    std::unique_ptr<engine::fields::GpuVectorFields> gpuVectorFields;
    std::uint64_t gpuFieldBindings=0;
    GpuFieldDriver gpuFieldDriver;
    std::shared_ptr<engine::fields::GpuVectorView> gpuSimulationWind;
    std::uint64_t gpuFieldEpoch=0,gpuFieldFrames=0;
    bool createGpuFields(std::string& error);
    GpuFieldFrame fieldFrame(ID3D12GraphicsCommandList* list);
    bool recordGpuFields(ID3D12GraphicsCommandList* list,std::string& error);
    void releaseGpuFields();
    std::unique_ptr<engine::effects::GpuRuntime> gpuEffectRuntime;
    std::unique_ptr<engine::fields::GpuEffectFields> gpuEffectFields;
    engine::effects::GpuFieldDeposits* gpuEffectDeposits=nullptr;
    std::unique_ptr<GpuEffectSprites> gpuEffectSprites;
    engine::smoke::GpuSmokeService* gpuSmokeService=nullptr;
    engine::smoke::GpuSmokeSharing* gpuSmokeSharing=nullptr;
    detail::SmokeScene smokeScene;
    std::vector<detail::SmokeObservation> smokeFrameSources,smokeCaptureSources;
    std::vector<ID3D12Resource*> smokePixelReads;
    std::uint64_t smokeMediaFrames=0,smokeMediaInstances=0;
    std::uint64_t smokeSharedFrames=0,smokeSharedInstances=0;
    bool prepareSmokeMedia(ID3D12GraphicsCommandList*,unsigned frame,const float camera[3],std::string& error);
    void finishSmokeMedia(ID3D12GraphicsCommandList*);
    std::uint64_t gpuEffectEpoch=0;
    double gpuEffectSimulationMs=0,gpuEffectDrawMs=0;
    unsigned gpuEffectTimingSamples=0;
    bool createGpuEffects(std::string& error);
    bool recordGpuEffects(ID3D12GraphicsCommandList* list,const Mat4& vp,std::string& error);
    bool drawGpuEffects(ID3D12GraphicsCommandList* list,unsigned index,std::string& error);
    void releaseGpuEffects();
    std::string gpuEffectsJson() const;
    std::vector<GpuClothConfig> configuredGpuCloth;
    GpuClothDriver gpuClothDriver;
    std::vector<std::unique_ptr<engine::cloth::GpuSolver>> gpuCloth;
    std::unique_ptr<GpuClothDraw> gpuClothDraw;
    std::unique_ptr<engine::cloth::GpuBatch> gpuClothBatch;
    std::uint64_t gpuClothEpoch=0,gpuClothFrames=0;
    double gpuClothSimulationMs=0,gpuClothDrawMs=0;
    unsigned gpuClothTimingSamples=0;
    bool createGpuCloth(std::string& error);
    bool recordGpuCloth(ID3D12GraphicsCommandList* list,std::string& error);
    bool drawGpuCloth(ID3D12GraphicsCommandList* list,const Mat4& vp,const float camera[3],std::string& error);
    void releaseGpuCloth();
    std::string gpuClothJson() const;

    // --- render targets (one set per buffered frame) ---
    ID3D12Resource* renderTargets[detail::FrameCount]{};
    TrackedResource depth[detail::FrameCount];
    TrackedResource sceneBaseColor[detail::FrameCount];
    TrackedResource sceneKeyLight[detail::FrameCount];
    TrackedResource shadowMask[detail::FrameCount];
    TrackedResource composite[detail::FrameCount];

    // --- acceleration structures ---
    ID3D12Resource* blas = nullptr;
    ID3D12Resource* tlas[detail::FrameCount]{};
    ID3D12Resource* tlasInstances[detail::FrameCount]{};
    ID3D12Resource* tlasScratch[detail::FrameCount]{};
    bool blasBuilt = false;
    std::string accelerationStructureError;
    UINT64 blasBytes = 0;
    UINT64 tlasBytes = 0;
    UINT64 tlasScratchBytes = 0;
    unsigned tlasInstanceCount = 0;
    unsigned tlasRebuilds = 0;
    unsigned tlasCapacity = detail::MaxInstances;

    // --- DXR static shadows ---
    ID3D12StateObject* rtStateObject = nullptr;
    ID3D12StateObjectProperties* rtProperties = nullptr;
    ID3D12Resource* rtShaderBindingTable = nullptr;
    void* rtShaderBindingTableMapped = nullptr;
    UINT64 rtShaderBindingTableSize = 0;
    bool rtRequested = true;
    bool rtPipelineCreated = false;
    bool rtSbtCreated = false;
    bool rtDispatchedLastFrame = false;
    unsigned rtDispatchedFrames = 0;
    unsigned rtFallbackFrames = 0;
    unsigned rtDispatchFailures = 0;
    std::string rtError;
    double rtLastGpuMs = 0.0;
    UINT64 rtShadowTextureBytes = 0;

    // --- timestamps and statistics ---
    ID3D12QueryHeap* timestampHeap = nullptr;
    ID3D12Resource* timestampReadback = nullptr;
    UINT64 timestampFrequency = 0;
    bool timestampPending[detail::FrameCount]{};
    bool timestampsAvailable = false;
    unsigned compositeDebugView = 0;
    unsigned framesRendered = 0;
    unsigned drawCallsLastFrame = 0;
    unsigned long long trianglesLastFrame = 0;
    // Draw items rejected by the view frustum this frame. `submittedLastFrame` is what
    // the host handed over, so culled + submitted - culled == submitted is checkable in
    // the report instead of being an assertion nobody reads.
    bool frustumCull = true;
    unsigned submittedLastFrame = 0;
    unsigned culledLastFrame = 0;
    // Draw-call batching plan for the frame just submitted. Diagnostic until the instanced
    // path lands: `plannedDraws` is what the same item list would cost grouped by material.
    BatchPlan batchPlan;
    unsigned plannedBatchesLastFrame = 0;
    unsigned plannedDrawsLastFrame = 0;

    // --- screenshot readback ---
    unsigned screenshotsCaptured = 0;
    std::string lastScreenshotPath;
    std::string lastScreenshotError;
    unsigned screenshotSource = 0;
    // Screenshots are recorded inside the frame submission (copy composite -> readback)
    // and read back after the frame fence, so no extra submission or state juggling is
    // needed and the file always matches the presented image.
    ID3D12Resource* captureReadback = nullptr;
    UINT64 captureReadbackBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint{};
    bool captureRequested = false;
    bool captureRecorded = false;
    unsigned captureWidth = 0;
    unsigned captureHeight = 0;
    std::string capturePath;
    unsigned long long lastScreenshotBytes = 0;
    double lastScreenshotMean = 0.0;
    unsigned char lastScreenshotMin = 0;
    unsigned char lastScreenshotMax = 0;
    unsigned lastFrameIndex = 0;

    // --- mesh shader path ---
    ID3D12RootSignature* meshRootSignature = nullptr;
    ID3D12PipelineState* meshPipeline = nullptr;
    ID3D12PipelineState* meshMaterialPipeline = nullptr;
    ID3D12Resource* meshletVertices = nullptr;
    ID3D12Resource* meshletTriangles = nullptr;
    ID3D12Resource* meshletDescs = nullptr;
    ID3D12Resource* meshCameraBuffer = nullptr;
    float* meshCameraMapped = nullptr;
    bool meshRequested = true;
    bool meshPipelineCreated = false;
    bool meshPathUsedLastFrame = false;
    unsigned meshFrames = 0;
    unsigned meshFallbackFrames = 0;
    unsigned meshletCount = 0;
    unsigned meshletCulledLastFrame = 0;
    unsigned meshletDrawnLastFrame = 0;
    std::string meshError;
    detail::MaterialTable materials;

    // --- compute queue and probe (B1 / B3) ---
    // A dedicated compute queue proves the renderer can own more than one queue, and the
    // probe dispatch verifies SM 6.6 heap indexing against the descriptor-table path.
    ID3D12CommandQueue* computeQueue = nullptr;
    ID3D12CommandAllocator* computeAllocator = nullptr;
    ID3D12GraphicsCommandList* computeCommands = nullptr;
    ID3D12Fence* computeFence = nullptr;
    UINT64 computeFenceValue = 0;
    ID3D12RootSignature* computeRootSignature = nullptr;
    ID3D12PipelineState* computePipeline = nullptr;
    ID3D12Resource* computeConstants = nullptr;
    float* computeConstantsMapped = nullptr;
    ID3D12Resource* computeResult = nullptr;
    ID3D12Resource* computeReadback = nullptr;
    bool computeRequested = false;
    bool computePipelineReady = false;
    bool computeCorrupt = false;
    bool computeQueueCreated = false;
    unsigned computeDispatchedFrames = 0;
    unsigned computeCrossQueueWaits = 0;
    bool computeProbeRan = false;
    bool computeHeapVerified = false;
    unsigned computeHeapPairsMatched = 0;
    unsigned computeHeapBaseIndex = 0;
    unsigned computeMaxIndexTested = 0;
    float computeHeapMaxDelta = 0.0f;
    float computeLightScaleGpu = 0.0f;
    float computeLightScaleCpu = 0.0f;
    unsigned computeLightCountGpu = 0;
    std::vector<LightDesc> computeLastLights;
    std::string computeError;

    // --- particles and the field they splat into (docs/FIELD-PLAN.md F1-F3) ---
    // One structured buffer holds the whole particle pool. The simulation kernel is the
    // only writer, so the pool needs no cross-queue synchronisation of its own.
    ID3D12Resource* particlePool = nullptr;
    TrackedResource particleDispatchArgs;
    ID3D12CommandSignature* particleDispatchSignature = nullptr;
    ID3D12PipelineState* particleDispatchArgsPipeline = nullptr;
    ID3D12QueryHeap* particleTimestampHeap = nullptr;
    ID3D12Resource* particleTelemetryReadback = nullptr;
    UINT64 particleTimestampFrequency = 0;
    UINT64 particleTelemetryFence[detail::FrameCount]{};
    unsigned particleExpectedGroups[detail::FrameCount]{};
    unsigned particleIndirectDispatches = 0, particleReadbackSamples = 0, particleArgsMismatches = 0;
    unsigned particleTimingSamples = 0;
    double particleComputeMs = 0.0, particleComputeTotalMs = 0.0;
    // Integer splat targets, the float solver state, and the ping-pong pairs the pressure
    // solve and advection need. Per-frame copies remove the hazard between this frame's
    // compute writes and the previous frame's graphics reads.
    TrackedResource volumeDensity[detail::FrameCount];
    TrackedResource volumeHeat[detail::FrameCount];
    TrackedResource volumeState[detail::FrameCount][2];
    // MacCormack scratch buffer for the second-order state advection correction.
    TrackedResource volumeStateC[detail::FrameCount];
    TrackedResource volumeVelocity[detail::FrameCount][2];
    TrackedResource volumePressure[detail::FrameCount][2];
    TrackedResource volumeDivergence[detail::FrameCount];
    TrackedResource volumeCurl[detail::FrameCount];
    ID3D12RootSignature* particleSimRootSignature = nullptr;
    ID3D12RootSignature* particleResolveRootSignature = nullptr;
    ID3D12RootSignature* particleVolumeRootSignature = nullptr;
    ID3D12PipelineState* particleSimPipeline = nullptr;
    ID3D12PipelineState* particleResolvePipeline = nullptr;
    ID3D12PipelineState* particleVolumePipeline = nullptr;
    // Temporal path (FIELD-REALISM-GOAL batch 3): half-res march -> accumulation ->
    // depth-aware upsampled composite. The march reuses particleVolumeRootSignature.
    ID3D12RootSignature* particleAccumulateRootSignature = nullptr;
    ID3D12RootSignature* particleCompositeRootSignature = nullptr;
    ID3D12PipelineState* particleMarchPipeline = nullptr;
    ID3D12PipelineState* particleAccumulatePipeline = nullptr;
    ID3D12PipelineState* particleCompositePipeline = nullptr;
    TrackedResource volumeHalf;     // half-res march target, RTV + SRV
    TrackedResource volumeHistory;  // temporal history, UAV + SRV
    unsigned volumeHalfWidth = 0;
    unsigned volumeHalfHeight = 0;
    bool volumeTargetsReady = false;
    bool particleTemporal = true;
    bool temporalHistoryValid = false;
    float temporalBlend = 1.0f;
    unsigned temporalAccumFrames = 0;
    float prevInvViewProjection[16] = {};
    ID3D12Resource* particleAccumulateConstants = nullptr;
    void* particleAccumulateConstantsMapped = nullptr;
    ID3D12Resource* particleCompositeConstants = nullptr;
    void* particleCompositeConstantsMapped = nullptr;
    // Field solver (docs/FIELD-PLAN.md F3): buoyancy, advection, projection.
    ID3D12RootSignature* fluidRootSignature = nullptr;
    ID3D12PipelineState* fluidBuoyancyPipeline = nullptr;
    ID3D12PipelineState* fluidAdvectVelocityPipeline = nullptr;
    ID3D12PipelineState* fluidAdvectStatePipeline = nullptr;
    ID3D12PipelineState* fluidDivergencePipeline = nullptr;
    ID3D12PipelineState* fluidJacobiPipeline = nullptr;
    ID3D12PipelineState* fluidGradientPipeline = nullptr;
    ID3D12PipelineState* fluidCopyStatePipeline = nullptr;
    ID3D12PipelineState* fluidCurlPipeline = nullptr;
    ID3D12PipelineState* fluidVorticityPipeline = nullptr;
    ID3D12PipelineState* fluidMacCormackPipeline = nullptr;
    ID3D12Resource* fluidConstants = nullptr;
    void* fluidConstantsMapped = nullptr;
    unsigned fluidIterations = 20;
    unsigned fluidPasses = 0;
    // Channel leases (FIELD-PLAN F4, minimal slice): consumers declare which field channels
    // they need and at what consistency. The solver refuses to run a frame whose required
    // channels have no lease, and the report lists every lease with its reference count.
    ChannelLease channelLeases[detail::FieldChannelCount];
    unsigned leaseAcquisitions = 0;
    unsigned leaseReleases = 0;
    unsigned leaseStalls = 0;
    // Fire preset and the explosion burst that widens the source for a short window.
    bool particleFirePreset = false;
    unsigned particleExplodeAt = 0;
    float particleBurst = 0.0f;
    float particleBurstPeak = 0.0f;
    float particleExtinction = 3.2f;
    float particleAmbient = 0.35f;
    float particleBuoyancy = 1.4f;
    float particleSink = 0.8f;
    // Particle obstacles, uploaded once per frame into a mapped structured buffer.
    ID3D12Resource* particleColliders = nullptr;
    void* particleCollidersMapped = nullptr;
    unsigned particleColliderCount = 0;
    unsigned particleColliderRejected = 0;
    std::vector<FieldCollider> fieldColliders;
    ID3D12Resource* particleSimConstants = nullptr;
    void* particleSimConstantsMapped = nullptr;
    ID3D12Resource* particleResolveConstants = nullptr;
    void* particleResolveConstantsMapped = nullptr;
    ID3D12Resource* particleVolumeConstants = nullptr;
    void* particleVolumeConstantsMapped = nullptr;
    bool particlesRequested = false;
    bool particlesReady = false;
    // Dedicated compute list and fence so the particle pass never resets the probe's
    // allocator mid-flight when both features are enabled.
    ID3D12CommandAllocator* particleAllocator = nullptr;
    ID3D12GraphicsCommandList* particleCommands = nullptr;
    ID3D12Fence* particleFence = nullptr;
    UINT64 particleFenceValue = 0;
    unsigned particleFrames = 0;
    unsigned particleDispatchFailures = 0;
    unsigned particleDraws = 0;
    unsigned particleCount = detail::ParticleCapacity;
    float particleEmitter[3] = {0.0f, 0.2f, 0.0f};
    float particleVolumeMin[3] = {-8.0f, 0.0f, -8.0f};
    float particleVolumeExtent = 16.0f;
    std::string particleError;

    bool createParticles(std::string& error);
    bool createVolumeTargets(std::string& error);
    // Channel leases (FIELD-PLAN F4). The simulation owns the solver channels for its whole
    // lifetime; the volumetric pass takes a short render lease every frame.
    void acquireFieldLease(FieldChannel channel, FieldConsistency consistency);
    void releaseFieldLease(FieldChannel channel);
    bool fieldLeasesComplete() const;
    std::string fieldLeasesJson() const;
    // Last computed explosion burst in [0,1]; the host polls it to make the explosion leave
    // a mark on the scene.
    float particleBurstState() const { return particleBurst; }
    void setParticleTemporal(bool enabled) { particleTemporal = enabled; }
    bool particleTemporalEnabled() const { return particleTemporal; }
    void releaseParticles();
    bool dispatchParticles(unsigned frameIndex, float deltaTime, std::string& error);
    void drawParticles(ID3D12GraphicsCommandList* list, const Mat4& viewProjection,
                       const float cameraPosition[3], unsigned frameIndex);
    std::string particlesJson() const;
    bool createTexture3D(unsigned dimension, DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                         D3D12_RESOURCE_FLAGS flags, TrackedResource& output,
                         std::string& error);

    // --- reliability diagnostics ---
    // Stable error codes (NG-E-*) plus bounded text so a failure is never silent and
    // never floods a report. Entries are capped; the counter is exact.
    struct Diagnostic {
        std::string code;
        std::string text;
        unsigned occurrences = 0;
    };
    std::vector<Diagnostic> diagnostics;
    unsigned diagnosticEvents = 0;
    bool deviceRemovalInjected = false;
    unsigned faultInjectionFrame = 0;
    unsigned long long videoMemoryBytes = 0;
    unsigned long long videoMemoryBudgetBytes = 0;
    bool videoMemoryQuerySupported = false;

    // --- state ---
    unsigned width = 0;
    unsigned height = 0;
    bool initialized = false;
    Environment environment;
    std::vector<OverlayLayer> overlay;
    FrameStats frameStats{};

    // --- capabilities ---
    DXGI_ADAPTER_DESC1 adapterDescription{};
    // Validation layer (optional, used by G1 illegal-state evidence).
    bool validationRequested = false;
    ID3D12InfoQueue* infoQueue = nullptr;
    unsigned validationErrors=0,validationWarnings=0;
    std::vector<std::string> validationMessages;
    unsigned bindingTier = 0;
    unsigned shaderModel = 0;
    int raytracingTier = -1;
    int meshShaderTier = -1;

    // D3D12 objects created through the helpers are counted so the lifecycle test can
    // prove that nothing leaks across shutdown/initialize cycles.
    template<class T> T* track(T* object) {
        if (object) { ++detail::g_liveObjects; ++detail::g_objectsCreated; }
        return object;
    }
    template<class T> void destroy(T*& object) {
        if (object) { object->Release(); object = nullptr; --detail::g_liveObjects; }
    }

    // --- helpers ---
    bool waitFor(UINT64 value, std::string& error);
    bool waitIdle(std::string& error);
    bool createBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                      ID3D12Resource** output, std::string& error);
    bool createBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                      D3D12_RESOURCE_FLAGS flags, ID3D12Resource** output, std::string& error);
    bool createTexture2D(unsigned textureWidth, unsigned textureHeight, DXGI_FORMAT format,
                         D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                         const D3D12_CLEAR_VALUE* clear, TrackedResource& output,
                         std::string& error);
    void transitionTo(TrackedResource& tracked, D3D12_RESOURCE_STATES state,
                      ID3D12GraphicsCommandList* list);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor(unsigned slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor(unsigned slot) const;

    // --- setup ---
    bool createTargets(std::string& error);
    void releaseTargets();
    bool createRasterRootSignature(std::string& error);
    bool createCompositeRootSignature(std::string& error);
    bool createRasterPipeline(std::string& error);
    bool createTexturedPipeline(std::string& error);
    bool createBindlessPipeline(std::string& error,bool inlineOnly=false);
    bool createTransparency(std::string& error);
    void releaseTransparency();
    void transparencyViews();
    bool collectTransparency(std::string& error);
    bool createCompositePipeline(std::string& error);
    bool uploadScene(std::string& error);
    bool uploadConvexMeshes(std::string& error);
    void releaseConvexMeshes();
    void queryCapabilities();
    void collectValidationMessages(const char* stage);
    void addDiagnostic(const char* code, const std::string& text);
    void queryVideoMemory();
    bool selfTestInvalidPipeline(std::string& code, std::string& text);
    bool buildMeshPipeline(std::string& error);
    void releaseMeshPipeline();
    bool createComputePipeline(std::string& error);
    void releaseComputePipeline();
    bool dispatchComputeProbe(const std::vector<LightDesc>& lights, std::string& error);
    bool readbackComputeProbe(std::string& error);
    std::string computeProbesFragment() const;
    bool createMeshletData(std::string& error);
    bool renderMeshPath(ID3D12GraphicsCommandList4* list, const std::vector<DrawItem>& items,
                        const Mat4& viewProjection, const float eye[3]);
    std::string meshShaderJson() const;
    std::string bindlessJson() const;
    bool selfTestMaterialTable(std::string& error);
    void clearValidationMessages();
    bool createDescriptorViews(std::string& error);
    bool recordCaptureCopy(ID3D12GraphicsCommandList* list, unsigned frameIndex,
                           std::string& error);
    bool buildAccelerationStructures();
    bool updateTopLevelAccelerationStructure(ID3D12GraphicsCommandList4* list,
                                             const std::vector<DrawItem>& items,
                                             unsigned frameIndex);
    void createTimestamps();
    void readTimestamp(unsigned index);

    // --- ray tracing (rt_shadows.cpp) ---
    bool buildRaytracingPipeline(std::string& error);
    void releaseRaytracingPipeline();
    bool dispatchShadowRays(ID3D12GraphicsCommandList4* list, unsigned frameIndex,
                            const Mat4& viewProjection, const float cameraPosition[3],
                            std::string& error);
    std::string rtShadowsJson() const;
};

} // namespace nativegpu


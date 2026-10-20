#pragma once

// Internal renderer declarations shared by d3d12_renderer.cpp (device, targets, frame
// graph, composite, screenshots) and rt_shadows.cpp (DXR state object, SBT, dispatch).
// Nothing in this header is part of the public API.

#include "d3d12_renderer.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nativegpu {
namespace detail {

constexpr unsigned FrameCount = 3;
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
// instead of silently truncating.
constexpr unsigned MaxInstances = 256;

// Shader-visible CBV/SRV/UAV heap. Slot 0 is the material texture table; every frame
// index owns a contiguous block of eight descriptors so a pass can bind one table.
enum DescriptorSlot : unsigned {
    SlotMaterialTable = 0,
    SlotMeshVertices = 1,
    SlotMeshTriangles = 2,
    SlotMeshlets = 3,
    FrameDescriptorBase = 8,
    FrameDescriptorStride = 8
};

enum FrameDescriptorSlot : unsigned {
    FdsDepth = 0,
    FdsTopLevelAccelerationStructure = 1,
    FdsShadowMaskUav = 2,
    FdsBaseColor = 3,
    FdsKeyLight = 4,
    FdsShadowMaskSrv = 5,
    FdsCount = 8
};

constexpr unsigned DescriptorSlotCount =
    FrameDescriptorBase + FrameCount * FrameDescriptorStride;

// RTV heap: one swap chain target per frame, then three offscreen targets per frame
// (base colour, key light, composite).
constexpr unsigned RtvSlotBase = FrameCount;
constexpr unsigned RtvSlotsPerFrame = 3;
constexpr unsigned RtvSlotCount = FrameCount * (1 + RtvSlotsPerFrame);

// Four timestamps per buffered frame: whole frame, and the DXR dispatch inside it.
enum TimestampSlot : unsigned {
    TimestampFrameBegin = 0,
    TimestampRtBegin = 1,
    TimestampRtEnd = 2,
    TimestampFrameEnd = 3,
    TimestampsPerFrame = 4
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
struct DrawConstants {
    Mat4 model;
    Mat4 viewProjection;
    Mat4 normalMatrix;
    float color[4];
    float roughness;
    float metalness;
    unsigned textureIndex;
    unsigned padding;
};
static_assert(sizeof(DrawConstants) == 56 * sizeof(float), "Root constant packing changed");

struct SceneConstants {
    float ambient[4];
    float lightPositionRange[4][4];
    float lightColorIntensity[4][4];
    unsigned lightCount;
    float padding[3];
};
static_assert(sizeof(SceneConstants) == 40 * sizeof(float), "Scene constant packing changed");

struct ShadowConstants {
    Mat4 invViewProjection;
    float cameraPosition[4];
    float keyLightDirection[4];
    float traceParams[4];
    float depthParams[4];
};
static_assert(sizeof(ShadowConstants) == 32 * sizeof(float), "Shadow constant packing changed");

} // namespace detail

struct Renderer::Impl {
    using TrackedResource = detail::TrackedResource;

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
    ID3D12RootSignature* rtRootSignature = nullptr;
    ID3D12PipelineState* rasterPipeline = nullptr;
    ID3D12PipelineState* compositePipeline = nullptr;

    // --- geometry and material resources ---
    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    ID3D12Resource* materialTexture = nullptr;
    ID3D12Resource* sceneConstantBuffer = nullptr;
    detail::SceneConstants* sceneConstantMapped = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    bool materialTableReady = false;
    unsigned materialSlots = detail::MaterialSlotCount;

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
    FrameStats frameStats{};

    // --- capabilities ---
    DXGI_ADAPTER_DESC1 adapterDescription{};
    // Validation layer (optional, used by G1 illegal-state evidence).
    bool validationRequested = false;
    ID3D12InfoQueue* infoQueue = nullptr;
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
    bool createCompositePipeline(std::string& error);
    bool uploadScene(std::string& error);
    void queryCapabilities();
    void collectValidationMessages(const char* stage);
    void addDiagnostic(const char* code, const std::string& text);
    void queryVideoMemory();
    bool selfTestInvalidPipeline(std::string& code, std::string& text);
    bool buildMeshPipeline(std::string& error);
    void releaseMeshPipeline();
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


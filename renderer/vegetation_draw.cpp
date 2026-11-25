#include "vegetation_draw.hpp"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nativegpu {
namespace {
// Owning pointer with move-only semantics. A shallow copy would release the same COM
// object twice, which is what a vector<Staging> relocation produces.
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept { if (this != &other) { if (p) p->Release(); p = other.p; other.p = nullptr; } return *this; }
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
};

void check(HRESULT code, const char* why) {
    if (FAILED(code)) {
        char message[192];
        std::snprintf(message, sizeof(message), "%s (HRESULT=0x%08lx)", why, static_cast<unsigned long>(code));
        throw engine::vegetation::Error("NG-E-VEG-FORMAT", message);
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

void uavBarrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
}

// Mirrors VegetationConstants in shaders/vegetation.hlsl: 76 dwords. The static_assert is
// the only thing preventing a silent ABI drift between C++ and HLSL.
struct Constants {
    Mat4 viewProjection;
    Mat4 lightViewProjection;
    float cameraPosition[4]{};
    float windDirection[4]{};
    float pageOrigin[4]{};
    float pageScale[4]{};
    float lightDirection[4]{};
    float lightColor[4]{};
    float shading[4]{};
    // Two materials, because bark and leaf share one draw. `materialCommon` carries the
    // quantities that are not per-material (normal scale, parallax scale, transmission gain).
    float materialBark[4]{};
    float materialLeaf[4]{};
    float materialCommon[4]{};
    float instanceBounds[4]{};
    float shadow[4]{};
    unsigned counts[4]{};
    // Debug channel: x = mode, y = UV checker cells. Kept last so the layout is a pure append.
    float debug[4]{};
    // Surface flags: x = bark normal flip, y = leaf normal flip, z/w reserved.
    float materialSurface[4]{};
};
static_assert(sizeof(Constants) == 368, "vegetation constants must stay 92 dwords");

// Vertex record: three float4 per vertex (position/wind, normal/material, uv/packed colour).
struct VertexRecord { float data[12]; };
static_assert(sizeof(VertexRecord) == 48, "vegetation vertex record must be 48 bytes");

// DrawIndexedInstanced arguments: 5 x uint32.
constexpr std::uint32_t kArgumentBytes = 20;
// Descriptor slots: 0-2 cull UAVs, 3-11 material textures, 12 key-light shadow depth.
// The order is the shader's t3..t11 order: bark baseColor/normal/orm/height, then leaf
// baseColor/normal/orm/thickness/opacity.
constexpr unsigned kMaterialBase = 3;
constexpr unsigned kShadowSlot = 12;
constexpr unsigned kHeapSize = 13;
// 2048 over a ~30 m key-light frustum is ~1.5 cm per texel: enough to resolve leaf-on-leaf
// contact without the map itself becoming a memory line item worth reporting separately.
constexpr unsigned kShadowMapSize = 2048;

struct Staging {
    Com<ID3D12Resource> resource;
    unsigned char* mapped = nullptr;
    std::size_t bytes = 0;
};
}

struct VegetationDraw::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12Fence> timeline;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> cullPipeline, argsPipeline, resetPipeline, graphicsPipeline, shadowPipeline;
    Com<ID3D12CommandSignature> signature;
    Com<ID3D12Resource> vertexBuffer, indexBuffer, instanceBuffer, visibleBuffer, argsBuffer, countBuffer;
    Com<ID3D12Resource> constantBuffer;
    Com<ID3D12Resource> shadowMap;
    Com<ID3D12DescriptorHeap> shadowDsv;
    Com<ID3D12DescriptorHeap> heap;
    std::vector<Staging> staging;          // kept alive until the copy has executed
    std::vector<Com<ID3D12Resource>> textures;
    D3D12_INDEX_BUFFER_VIEW indexView{};
    void* constantMapped = nullptr;
    engine::vegetation::PageRecord page;
    unsigned capacity = 0;
    unsigned indexCount = 0;
    unsigned instanceCount = 0;
    std::uint64_t lastFence = 0;
    bool meshReady = false;
    bool uavReady = false;
    bool texturesReady = false;
    // Presence of every slot the shader gates on. The four "required" ones used to be assumed
    // present whenever any texture had been uploaded, so a species with no foliace baseColor
    // sampled the 1x1 white placeholder and its leaves rendered as white cards.
    struct {
        bool barkBaseColor = false, leafBaseColor = false, leafOpacity = false, normal = false;
        bool barkOrm = false, leafOrm = false, leafThickness = false, barkHeight = false;
    } present;
    // The shadow map lives in DEPTH_WRITE until the first shadow pass has finished, then
    // alternates DEPTH_WRITE (caster) / PIXEL_SHADER_RESOURCE (receiver) per frame.
    bool shadowEverSampled = false;
    VegetationDrawStats stats;

    Impl(ID3D12Device* owner, ID3D12Fence* fence, DXGI_FORMAT color, DXGI_FORMAT depth, unsigned visibleCapacity)
        : capacity(visibleCapacity) {
        device.p = owner; owner->AddRef();
        timeline.p = fence; fence->AddRef();
        if (!shaders::kDxcAvailable) throw engine::vegetation::Error("NG-E-VEG-FORMAT", "vegetation shaders require DXC");

        // Root signature. Budget is 64 DWORDs: one root CBV (1) + two root SRVs (4) + a UAV
        // table (1) + a material SRV table (1) = 7. The constants moved to a CBV because
        // 56 root-constant DWORDs plus the material table no longer fit.
        static D3D12_DESCRIPTOR_RANGE uavRanges[3]{};
        for (unsigned index = 0; index < 3; ++index) {
            uavRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRanges[index].NumDescriptors = 1;
            uavRanges[index].BaseShaderRegister = index;
            uavRanges[index].OffsetInDescriptorsFromTableStart = index;
        }
        static D3D12_DESCRIPTOR_RANGE materialRange{};
        materialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        // t3..t12: bark baseColor/normal/ORM/height, leaf baseColor/normal/ORM/thickness/opacity,
        // then the key-light shadow depth. That is ten descriptors, not nine: the shader samples
        // t12, and a descriptor table that stops at t11 makes CreateGraphicsPipelineState fail
        // with E_INVALIDARG (the root signature does not cover a resource the shader declares).
        // The shadow map sits in this table rather than in its own root parameter because a root
        // SRV costs two DWORDs and the signature has no budget left.
        materialRange.NumDescriptors = 10;
        materialRange.BaseShaderRegister = 3;
        materialRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (unsigned index = 0; index < 2; ++index) {
            parameters[1 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[1 + index].Descriptor.ShaderRegister = index;
            parameters[1 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 3;
        parameters[3].DescriptorTable.pDescriptorRanges = uavRanges;
        parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[4].DescriptorTable.NumDescriptorRanges = 1;
        parameters[4].DescriptorTable.pDescriptorRanges = &materialRange;
        parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].MipLODBias = 0;
        samplers[0].MaxAnisotropy = 8;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplers[0].MinLOD = 0;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[0].ShaderRegister = 0;
        // Shadow comparison sampler. Border white means "outside the map is not occluded",
        // which is the only safe default for a map that covers part of the world.
        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].MipLODBias = 0;
        samplers[1].MaxAnisotropy = 1;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].MinLOD = 0;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].ShaderRegister = 1;

        D3D12_ROOT_SIGNATURE_DESC signatureDescription{};
        signatureDescription.NumParameters = 5;
        signatureDescription.pParameters = parameters;
        signatureDescription.NumStaticSamplers = 2;
        signatureDescription.pStaticSamplers = samplers;
        Com<ID3DBlob> blob, diagnostics;
        check(D3D12SerializeRootSignature(&signatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, blob.out(), diagnostics.out()),
              "serialize vegetation root signature");
        check(owner->CreateRootSignature(0, blob.p->GetBufferPointer(), blob.p->GetBufferSize(), IID_PPV_ARGS(root.out())),
              "create vegetation root signature");

        const auto compute = [&](const shaders::Blob& source, Com<ID3D12PipelineState>& target, const char* why) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = root.p;
            description.CS = {source.data, source.size};
            check(owner->CreateComputePipelineState(&description, IID_PPV_ARGS(target.out())), why);
        };
        compute(shaders::kVegetationCull, cullPipeline, "create vegetation cull pipeline");
        compute(shaders::kVegetationArgs, argsPipeline, "create vegetation args pipeline");
        compute(shaders::kVegetationReset, resetPipeline, "create vegetation reset pipeline");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
        graphics.pRootSignature = root.p;
        graphics.VS = {shaders::kVegetationVS.data, shaders::kVegetationVS.size};
        graphics.PS = {shaders::kVegetationPS.data, shaders::kVegetationPS.size};
        graphics.SampleMask = UINT_MAX;
        graphics.SampleDesc.Count = 1;
        // Foliage is double sided: a leaf card seen from behind is the same leaf, and the
        // pixel shader flips the normal for it. Culling back faces here punches holes.
        graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        graphics.RasterizerState.DepthClipEnable = TRUE;
        graphics.DepthStencilState.DepthEnable = TRUE;
        graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        graphics.DSVFormat = depth;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = color;
        graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        check(owner->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(graphicsPipeline.out())), "create vegetation pipeline");

        // Shadow-caster pipeline: no colour target, same geometry, alpha-tested in the pixel
        // shader so a card casts the shape of a leaf. Depth bias is left at zero and handled
        // by the normal offset plus a comparison bias in the shader, because a raster bias
        // tuned for one light angle silently fails at another.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow{};
        shadow.pRootSignature = root.p;
        shadow.VS = {shaders::kVegetationVSShadow.data, shaders::kVegetationVSShadow.size};
        shadow.PS = {shaders::kVegetationPSShadow.data, shaders::kVegetationPSShadow.size};
        shadow.SampleMask = UINT_MAX;
        shadow.SampleDesc.Count = 1;
        shadow.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        shadow.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        shadow.RasterizerState.DepthClipEnable = TRUE;
        shadow.DepthStencilState.DepthEnable = TRUE;
        shadow.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        shadow.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        shadow.NumRenderTargets = 0;
        shadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        check(owner->CreateGraphicsPipelineState(&shadow, IID_PPV_ARGS(shadowPipeline.out())), "create vegetation shadow pipeline");

        // Shadow map + its depth view and the sampled view the receiver reads. The map is
        // owned here rather than by the caller because the caster and the receiver must share
        // one matrix and one lifetime, and reserving the descriptor at construction keeps the
        // heap layout immutable after the material textures are uploaded.
        D3D12_RESOURCE_DESC shadowDescription{};
        shadowDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDescription.Width = kShadowMapSize;
        shadowDescription.Height = kShadowMapSize;
        shadowDescription.DepthOrArraySize = 1;
        shadowDescription.MipLevels = 1;
        shadowDescription.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDescription.SampleDesc.Count = 1;
        shadowDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        shadowDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES shadowHeap{}; shadowHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        shadowHeap.CreationNodeMask = shadowHeap.VisibleNodeMask = 1;
        D3D12_CLEAR_VALUE shadowClear{};
        shadowClear.Format = DXGI_FORMAT_D32_FLOAT;
        shadowClear.DepthStencil.Depth = 1.0f;
        check(owner->CreateCommittedResource(&shadowHeap, D3D12_HEAP_FLAG_NONE, &shadowDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &shadowClear, IID_PPV_ARGS(shadowMap.out())), "create vegetation shadow map");

        D3D12_DESCRIPTOR_HEAP_DESC shadowDsvHeap{};
        shadowDsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        shadowDsvHeap.NumDescriptors = 1;
        check(owner->CreateDescriptorHeap(&shadowDsvHeap, IID_PPV_ARGS(shadowDsv.out())), "create vegetation shadow DSV heap");
        // An uninitialised descriptor clears fine as far as the call returns, but the debug
        // layer then reports a mismatched resource and the caster draws into nothing.
        owner->CreateDepthStencilView(shadowMap.p, nullptr, shadowDsv.p->GetCPUDescriptorHandleForHeapStart());

        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC command{};
        command.ByteStride = kArgumentBytes;
        command.NumArgumentDescs = 1;
        command.pArgumentDescs = &argument;
        check(owner->CreateCommandSignature(&command, nullptr, IID_PPV_ARGS(this->signature.out())), "create vegetation command signature");

        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.NumDescriptors = kHeapSize;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(owner->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(heap.out())), "create vegetation descriptor heap");

        // Depth is sampled as a single channel; the SRV format for D32 is R32_FLOAT.
        D3D12_SHADER_RESOURCE_VIEW_DESC shadowView{};
        shadowView.Format = DXGI_FORMAT_R32_FLOAT;
        shadowView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadowView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowView.Texture2D.MipLevels = 1;
        owner->CreateShaderResourceView(shadowMap.p, &shadowView, cpuSlot(kShadowSlot));

        // Frame constants: one 256-byte upload allocation, written every draw.
        D3D12_HEAP_PROPERTIES constantHeap{}; constantHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        constantHeap.CreationNodeMask = constantHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC constantDescription{};
        constantDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        constantDescription.Width = 512; constantDescription.Height = 1;
        constantDescription.DepthOrArraySize = 1; constantDescription.MipLevels = 1;
        constantDescription.SampleDesc.Count = 1;
        constantDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(owner->CreateCommittedResource(&constantHeap, D3D12_HEAP_FLAG_NONE, &constantDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(constantBuffer.out())), "create vegetation constants");
        D3D12_RANGE constantRange{0, 0};
        check(constantBuffer.p->Map(0, &constantRange, reinterpret_cast<void**>(&constantMapped)), "map vegetation constants");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(unsigned index) const {
        auto handle = heap.p->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * device.p->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(unsigned index) const {
        auto handle = heap.p->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * device.p->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return handle;
    }

    void createUav(std::size_t bytes, unsigned elements, Com<ID3D12Resource>& target, unsigned slot) {
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes; description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
        description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(target.out())), "create vegetation UAV buffer");
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = elements;
        view.Buffer.StructureByteStride = 4;
        device.p->CreateUnorderedAccessView(target.p, nullptr, &view, cpuSlot(slot));
    }

    // Uploads `bytes` into a new DEFAULT buffer through a committed upload heap. The
    // staging resource stays alive until the next upload, because the copy is in flight.
    Com<ID3D12Resource> createUploaded(ID3D12CommandList* list, std::size_t bytes, const void* data,
                                       D3D12_RESOURCE_STATES finalState, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes; description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
        description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = flags;
        Com<ID3D12Resource> target;
        // The debug layer tracks the declared state at first use even though buffers are
        // created in COMMON, so the copy must be declared and transitioned as COPY_DEST.
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(target.out())), "create vegetation buffer");

        Staging upload;
        D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC uploadDescription = description;
        uploadDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        check(device.p->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.resource.out())), "create vegetation staging");
        D3D12_RANGE range{0, 0};
        check(upload.resource.p->Map(0, &range, reinterpret_cast<void**>(&upload.mapped)), "map vegetation staging");
        std::memcpy(upload.mapped, data, bytes);
        upload.bytes = bytes;
        staging.push_back(std::move(upload));

        auto* commands = static_cast<ID3D12GraphicsCommandList*>(list);
        commands->CopyBufferRegion(target.p, 0, staging.back().resource.p, 0, bytes);
        transition(commands, target.p, D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        return target;
    }

    // Uploads every level of a cooked chain. A single-level texture is still accepted — the
    // fallback is a flat colour — but a texture that carries levels is uploaded whole,
    // because a minified mask without its chain is what turns a leaf into a hole.
    void createTexture(ID3D12CommandList* list, const VegetationTexture& source, unsigned slot,
                       const unsigned char fallback[4], const char* label) {
        const bool valid = source.valid();
        const std::uint32_t width = valid ? source.width : 1;
        const std::uint32_t height = valid ? source.height : 1;
        const std::uint32_t mipCount = valid ? source.mipCount() : 1;
        const DXGI_FORMAT format = (valid && source.srgb) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width; description.Height = height;
        description.DepthOrArraySize = 1; description.MipLevels = static_cast<UINT16>(mipCount);
        description.Format = format; description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Com<ID3D12Resource> texture;
        check(device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(texture.out())), label);

        auto* commands = static_cast<ID3D12GraphicsCommandList*>(list);
        // One staging buffer for the whole chain. Each level needs its own 256-byte-aligned
        // row pitch, so the copies cannot share a single footprint.
        std::vector<std::uint32_t> pitches(mipCount, 0);
        std::size_t totalBytes = 0;
        for (std::uint32_t level = 0; level < mipCount; ++level) {
            const std::uint32_t levelWidth = valid ? source.levels[level].width : 1;
            const std::uint32_t rowBytes = levelWidth * 4;
            pitches[level] = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            totalBytes += static_cast<std::size_t>(pitches[level]) * (valid ? source.levels[level].height : 1);
        }

        Staging upload;
        D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC uploadDescription{};
        uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDescription.Width = totalBytes; uploadDescription.Height = 1;
        uploadDescription.DepthOrArraySize = 1; uploadDescription.MipLevels = 1;
        uploadDescription.SampleDesc.Count = 1;
        uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.p->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.resource.out())), "create texture staging");
        D3D12_RANGE range{0, 0};
        check(upload.resource.p->Map(0, &range, reinterpret_cast<void**>(&upload.mapped)), "map texture staging");

        std::size_t cursor = 0;
        for (std::uint32_t level = 0; level < mipCount; ++level) {
            const VegetationTextureLevel placeholder{1, 1, {}};
            const auto& source_level = valid ? source.levels[level] : placeholder;
            const std::uint32_t rowBytes = source_level.width * 4;
            const std::uint32_t rows = source_level.height;
            for (std::uint32_t row = 0; row < rows; ++row) {
                const unsigned char* sourceRow = valid
                    ? source_level.pixels.data() + static_cast<std::size_t>(row) * rowBytes
                    : fallback;
                std::memcpy(upload.mapped + cursor + static_cast<std::size_t>(row) * pitches[level], sourceRow, rowBytes);
            }
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = upload.resource.p;
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Offset = cursor;
            from.PlacedFootprint.Footprint = {format, source_level.width, rows, 1, pitches[level]};
            to.pResource = texture.p;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = level;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            cursor += static_cast<std::size_t>(pitches[level]) * rows;
        }
        upload.bytes = totalBytes;
        staging.push_back(std::move(upload));
        transition(commands, texture.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = mipCount;
        device.p->CreateShaderResourceView(texture.p, &view, cpuSlot(slot));
        textures.push_back(std::move(texture));
    }

    // One constant block per frame, written identically by drawShadow and draw. Both passes
    // are recorded into the same command list and both reference this allocation, so a
    // second, different write would retroactively change the first pass.
    void writeConstants(const VegetationFrameInput& frame, const VegetationMaterial& material, const float bounds[3]) {
        Constants constants{};
        constants.viewProjection = frame.viewProjection;
        constants.lightViewProjection = frame.lightViewProjection;
        constants.cameraPosition[0] = frame.camera[0];
        constants.cameraPosition[1] = frame.camera[1];
        constants.cameraPosition[2] = frame.camera[2];
        constants.cameraPosition[3] = frame.seconds;
        constants.windDirection[0] = frame.windDirection[0];
        constants.windDirection[1] = frame.windDirection[1];
        constants.windDirection[2] = frame.windDirection[2];
        constants.windDirection[3] = frame.windSpeed;
        constants.pageOrigin[0] = page.minX;
        constants.pageOrigin[1] = page.baseY;
        constants.pageOrigin[2] = page.minZ;
        constants.pageOrigin[3] = frame.windAmplitude;
        constants.pageScale[0] = page.maxX - page.minX;
        constants.pageScale[1] = page.heightRange;
        constants.pageScale[2] = page.maxZ - page.minZ;
        constants.pageScale[3] = frame.windFrequency;
        for (int index = 0; index < 3; ++index) {
            constants.lightDirection[index] = frame.lightDirection[index];
            constants.lightColor[index] = frame.lightColor[index];
        }
        constants.lightDirection[3] = frame.lightIntensity;
        constants.lightColor[3] = frame.ambient;
        constants.shading[0] = frame.viewportHeight;
        constants.shading[1] = frame.tanHalfFov;
        constants.shading[2] = frame.alphaCutoff;
        // Bit 1 bark baseColor, 2 leaf baseColor, 4 leaf opacity, 8 normal maps,
        // 16 bark ORM, 32 leaf ORM, 64 leaf thickness, 128 bark height. Every bit is derived from
        // the slot that actually uploaded; a fixed 15 was how a grass clump got a white
        // placeholder for its albedo and no cutout for its foliage.
        unsigned textureBits = 0;
        if (texturesReady) {
            if (present.barkBaseColor) textureBits |= 1u;
            if (present.leafBaseColor) textureBits |= 2u;
            if (present.leafOpacity) textureBits |= 4u;
            if (present.normal) textureBits |= 8u;
            if (present.barkOrm) textureBits |= 16u;
            if (present.leafOrm) textureBits |= 32u;
            if (present.leafThickness) textureBits |= 64u;
            if (present.barkHeight) textureBits |= 128u;
        }
        constants.shading[3] = static_cast<float>(textureBits);
        constants.shading[2] = material.alphaCutoff;
        constants.materialBark[0] = material.barkRoughness;
        constants.materialBark[1] = material.barkThicknessMetres;
        constants.materialBark[2] = material.leafAbsorptionPerMetre;
        constants.materialBark[3] = material.occlusionStrength;
        constants.materialLeaf[0] = material.leafRoughness;
        constants.materialLeaf[1] = material.leafThicknessMetres;
        constants.materialLeaf[2] = material.leafAbsorptionPerMetre;
        constants.materialLeaf[3] = material.occlusionStrength;
        constants.materialCommon[0] = material.normalScale;
        // Height is authored in metres and the parallax offset is in UV units, so the tile
        // size has to divide it here rather than being guessed in the shader.
        constants.materialCommon[1] = material.heightScaleMetres / std::max(0.01f, material.uvTileMetres);
        constants.materialCommon[2] = 1.0f;
        constants.materialCommon[3] = material.doubleSided ? 1.0f : 0.0f;
        constants.instanceBounds[0] = bounds[0];
        constants.instanceBounds[1] = bounds[1];
        constants.instanceBounds[2] = frame.maximumDistance;
        constants.instanceBounds[3] = 0;
        constants.shadow[0] = static_cast<float>(kShadowMapSize);
        constants.shadow[1] = frame.shadowDepthBias;
        constants.shadow[2] = frame.shadowStrength > 0.0f ? frame.shadowStrength : 0.0f;
        constants.shadow[3] = frame.shadowNormalBiasMetres;
        constants.counts[0] = instanceCount;
        constants.counts[1] = capacity;
        constants.counts[2] = 1;
        constants.counts[3] = kMaterialBase;
        constants.debug[0] = static_cast<float>(frame.debugView);
        constants.debug[1] = std::max(1.0f, frame.debugCells);
        constants.debug[2] = 0;
        constants.debug[3] = 0;
        constants.materialSurface[0] = material.barkNormalFlipGreen ? 1.0f : 0.0f;
        constants.materialSurface[1] = material.leafNormalFlipGreen ? 1.0f : 0.0f;
        constants.materialSurface[2] = 0;
        constants.materialSurface[3] = 0;
        std::memcpy(constantMapped, &constants, sizeof(Constants));
    }

    // The three compute dispatches that turn the packed page into indirect arguments. The
    // CPU writes no per-instance data here: this is the whole reason 30M instances do not
    // become a 30M CPU loop.
    void cullPass(ID3D12GraphicsCommandList* list) {
        ID3D12DescriptorHeap* heaps[] = {heap.p};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.p);
        list->SetComputeRootConstantBufferView(0, constantBuffer.p->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, instanceBuffer.p->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, vertexBuffer.p->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(3, heap.p->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(resetPipeline.p);
        list->Dispatch(1, 1, 1);
        uavBarrier(list);
        list->SetPipelineState(cullPipeline.p);
        list->Dispatch((instanceCount + 63) / 64, 1, 1);
        uavBarrier(list);
        list->SetPipelineState(argsPipeline.p);
        list->Dispatch(1, 1, 1);
        uavBarrier(list);
        transition(list, argsBuffer.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }

    bool validate(std::uint64_t fence, const VegetationFrameInput& frame, std::string& error) const {
        if (!meshReady) { error = "NG-E-VEG-FORMAT: vegetation draw is not ready"; return false; }
        if (!fence || fence <= timeline.p->GetCompletedValue() || fence < lastFence) {
            error = "NG-E-VEG-FORMAT: invalid fence"; return false;
        }
        for (float value : frame.viewProjection.m) if (!std::isfinite(value)) { error = "NG-E-VEG-FORMAT: non-finite view projection"; return false; }
        if (!(frame.maximumDistance > 0) || !(frame.tanHalfFov > 0) || !(frame.viewportHeight > 0)) {
            error = "NG-E-VEG-FORMAT: invalid camera parameters"; return false;
        }
        return true;
    }
};

VegetationDraw::VegetationDraw(ID3D12Device* device, ID3D12Fence* timeline, DXGI_FORMAT color, DXGI_FORMAT depth, unsigned capacity) {
    if (!device || !timeline || capacity == 0 || capacity > (1u << 20)) {
        throw engine::vegetation::Error("NG-E-VEG-FORMAT", "vegetation draw needs a device, a fence and a bounded capacity");
    }
    impl_ = std::make_unique<Impl>(device, timeline, color, depth, capacity);
    impl_->stats.capacity = capacity;
    impl_->stats.shadowMapSize = kShadowMapSize;
    impl_->createUav(static_cast<std::size_t>(capacity) * 4, capacity, impl_->visibleBuffer, 0);
    impl_->createUav(kArgumentBytes, kArgumentBytes / 4, impl_->argsBuffer, 1);
    impl_->createUav(4, 1, impl_->countBuffer, 2);
}

VegetationDraw::~VegetationDraw() = default;

bool VegetationDraw::setTextures(ID3D12GraphicsCommandList* list, const VegetationTextureSet& textures, std::string& error) {
    error.clear();
    if (!impl_ || !list) { error = "NG-E-VEG-FORMAT: vegetation draw is not initialised"; return false; }
    auto& self = *impl_;
    if (self.texturesReady) { error = "NG-E-VEG-FORMAT: textures are already uploaded"; return false; }
    // Slots 3..11 mirror the shader's t3..t11 order. The textures live for the lifetime of
    // the draw object: recreating them per page would invalidate descriptors the previous
    // frame's command list still references.
    static const unsigned char kWhite[4] = {255, 255, 255, 255};
    static const unsigned char kFlatNormal[4] = {128, 128, 255, 255};
    // ORM placeholder: AO = 1, roughness = 0.78, metallic = 0. Sampling an unbound ORM would
    // give 0/0/0, which reads as "perfectly smooth metal".
    static const unsigned char kOrm[4] = {255, 199, 0, 255};
    static const unsigned char kBlack[4] = {0, 0, 0, 255};
    self.createTexture(list, textures.barkBaseColor, 3, kWhite, "create bark baseColor");
    self.createTexture(list, textures.barkNormal, 4, kFlatNormal, "create bark normal");
    self.createTexture(list, textures.barkOrm, 5, kOrm, "create bark orm");
    self.createTexture(list, textures.barkHeight, 6, kBlack, "create bark height");
    self.createTexture(list, textures.leafBaseColor, 7, kWhite, "create leaf baseColor");
    self.createTexture(list, textures.leafNormal, 8, kFlatNormal, "create leaf normal");
    self.createTexture(list, textures.leafOrm, 9, kOrm, "create leaf orm");
    self.createTexture(list, textures.leafThickness, 10, kWhite, "create leaf thickness");
    self.createTexture(list, textures.leafOpacity, 11, kWhite, "create leaf opacity");
    const VegetationTexture* slots[9] = {
        &textures.barkBaseColor, &textures.barkNormal, &textures.barkOrm, &textures.barkHeight,
        &textures.leafBaseColor, &textures.leafNormal, &textures.leafOrm,
        &textures.leafThickness, &textures.leafOpacity};
    for (unsigned index = 0; index < 9; ++index) {
        self.stats.textureMips[index] = slots[index]->valid() ? slots[index]->mipCount() : 1;
    }
    // Presence flags travel to the shader through the constant bitfield, so a missing map is
    // reported as absent instead of being sampled from a placeholder that looks like data.
    self.present.barkBaseColor = textures.barkBaseColor.valid();
    self.present.leafBaseColor = textures.leafBaseColor.valid();
    self.present.leafOpacity = textures.leafOpacity.valid();
    // Bit 8 covers both normal maps because they share one shader branch. A missing slot samples
    // the flat normal placeholder, which is neutral, so ORing them is safe where a missing
    // *colour* map would not be (it would sample white and brighten the surface).
    self.present.normal = textures.barkNormal.valid() || textures.leafNormal.valid();
    self.present.barkOrm = textures.barkOrm.valid();
    self.present.leafOrm = textures.leafOrm.valid();
    self.present.leafThickness = textures.leafThickness.valid();
    self.present.barkHeight = textures.barkHeight.valid();
    self.texturesReady = true;
    return true;
}

bool VegetationDraw::upload(ID3D12GraphicsCommandList* list, const VegetationMeshData& mesh,
                            const std::vector<engine::vegetation::PackedInstance>& instances,
                            const engine::vegetation::PageRecord& page, std::string& error) {
    error.clear();
    if (!impl_ || !list) { error = "NG-E-VEG-FORMAT: vegetation draw is not initialised"; return false; }
    const auto vertices = mesh.positions.size() / 3;
    if (vertices == 0 || mesh.indices.empty()) { error = "NG-E-VEG-FORMAT: mesh has no geometry"; return false; }
    if (mesh.normals.size() != vertices * 3 || mesh.uvs.size() != vertices * 2
        || mesh.colors.size() != vertices * 4 || mesh.windWeights.size() != vertices) {
        error = "NG-E-VEG-FORMAT: mesh attribute arrays disagree with its vertex count";
        return false;
    }
    if (instances.size() > impl_->capacity) { error = "NG-E-VEG-CAPACITY: instance page exceeds the visible capacity"; return false; }

    // Material id per vertex. A vertex belongs to exactly one primitive because the cook
    // appends vertices per primitive; anything unreferenced defaults to bark.
    std::vector<std::uint8_t> materialOfVertex(vertices, 0);
    for (const auto& primitive : mesh.primitives) {
        if (primitive.firstIndex + primitive.indexCount > mesh.indices.size()) {
            error = "NG-E-VEG-FORMAT: primitive range outside the index buffer";
            return false;
        }
        for (std::uint32_t offset = 0; offset < primitive.indexCount; ++offset) {
            const auto vertex = mesh.indices[primitive.firstIndex + offset];
            if (vertex < vertices) materialOfVertex[vertex] = static_cast<std::uint8_t>(primitive.material);
        }
    }

    std::vector<VertexRecord> records(vertices);
    for (std::size_t index = 0; index < vertices; ++index) {
        auto& record = records[index].data;
        record[0] = mesh.positions[index * 3 + 0];
        record[1] = mesh.positions[index * 3 + 1];
        record[2] = mesh.positions[index * 3 + 2];
        record[3] = mesh.windWeights[index];
        record[4] = mesh.normals[index * 3 + 0];
        record[5] = mesh.normals[index * 3 + 1];
        record[6] = mesh.normals[index * 3 + 2];
        // v1.w carries the material id as a plain float. Storing integer bits instead
        // would put a denormal in the interpolator, and a GPU that flushes denormals to
        // zero turns every leaf back into bark.
        record[7] = static_cast<float>(materialOfVertex[index]);
        record[8] = mesh.uvs[index * 2 + 0];
        record[9] = mesh.uvs[index * 2 + 1];
        const auto packedColor = static_cast<unsigned>(mesh.colors[index * 4 + 0])
            | (static_cast<unsigned>(mesh.colors[index * 4 + 1]) << 8)
            | (static_cast<unsigned>(mesh.colors[index * 4 + 2]) << 16)
            | (static_cast<unsigned>(mesh.colors[index * 4 + 3]) << 24);
        std::memcpy(&record[10], &packedColor, 4);
        record[11] = 0;
    }

    auto& self = *impl_;
    // First use: the UAV buffers were created in COMMON and must be promoted once.
    if (!self.uavReady) {
        transition(list, self.visibleBuffer.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(list, self.countBuffer.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        self.uavReady = true;
    }
    self.staging.clear();
    self.vertexBuffer = self.createUploaded(list, records.size() * sizeof(VertexRecord), records.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self.indexBuffer = self.createUploaded(list, mesh.indices.size() * 4, mesh.indices.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
    self.indexCount = static_cast<unsigned>(mesh.indices.size());
    self.indexView.BufferLocation = self.indexBuffer.p->GetGPUVirtualAddress();
    self.indexView.Format = DXGI_FORMAT_R32_UINT;
    self.indexView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * 4);

    std::vector<engine::vegetation::PackedInstance> packed = instances;
    if (packed.empty()) packed.push_back({});
    self.instanceBuffer = self.createUploaded(list, packed.size() * sizeof(engine::vegetation::PackedInstance), packed.data(),
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self.instanceCount = static_cast<unsigned>(instances.size());
    self.page = page;

    // Argument 0 (IndexCountPerInstance) is written once here; the compute pass owns only
    // the instance count, so the CPU never rewrites the arguments.
    std::uint32_t arguments[kArgumentBytes / 4] = {self.indexCount, 0, 0, 0, 0};
    Staging argumentUpload;
    {
        D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = kArgumentBytes; description.Height = 1;
        description.DepthOrArraySize = 1; description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(self.device.p->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(argumentUpload.resource.out())), "create argument staging");
        D3D12_RANGE range{0, 0};
        check(argumentUpload.resource.p->Map(0, &range, reinterpret_cast<void**>(&argumentUpload.mapped)), "map argument staging");
        std::memcpy(argumentUpload.mapped, arguments, kArgumentBytes);
        argumentUpload.bytes = kArgumentBytes;
        self.staging.push_back(std::move(argumentUpload));
    }
    transition(list, self.argsBuffer.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(self.argsBuffer.p, 0, self.staging.back().resource.p, 0, kArgumentBytes);
    transition(list, self.argsBuffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    self.meshReady = true;
    self.stats.instanceCount = self.instanceCount;
    self.stats.capacity = self.capacity;
    return true;
}

bool VegetationDraw::drawShadow(ID3D12GraphicsCommandList* list, std::uint64_t fence, const VegetationFrameInput& frame,
                                const VegetationMaterial& material, const float bounds[3], std::string& error) {
    error.clear();
    if (!impl_ || !list) { error = "NG-E-VEG-FORMAT: vegetation draw is not initialised"; return false; }
    auto& self = *impl_;
    if (!self.validate(fence, frame, error)) return false;
    // A page whose instances were all culled away has nothing to cast, and a frame with no
    // shadow strength asked for no shadow pass: both skip without touching the map, so the
    // receiver keeps reading "lit".
    if (!(frame.shadowStrength > 0.0f) || self.instanceCount == 0) return true;

    self.writeConstants(frame, material, bounds);
    self.cullPass(list);

    if (self.shadowEverSampled) {
        transition(list, self.shadowMap.p, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    auto dsv = self.shadowDsv.p->GetCPUDescriptorHandleForHeapStart();
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // Depth-only output: no render target is bound, and the caller rebinds its own targets
    // after this call. That ordering is part of the contract in the header.
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<int>(kShadowMapSize), static_cast<int>(kShadowMapSize)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    list->SetGraphicsRootSignature(self.root.p);
    list->SetGraphicsRootConstantBufferView(0, self.constantBuffer.p->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(1, self.instanceBuffer.p->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(2, self.vertexBuffer.p->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(3, self.heap.p->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootDescriptorTable(4, self.gpuSlot(kMaterialBase));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&self.indexView);
    list->SetPipelineState(self.shadowPipeline.p);
    list->ExecuteIndirect(self.signature.p, 1, self.argsBuffer.p, 0, nullptr, 0);
    transition(list, self.shadowMap.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, self.argsBuffer.p, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    self.shadowEverSampled = true;
    self.stats.shadowFrames += 1;
    if (fence > self.stats.lastFence) self.stats.lastFence = fence;
    return true;
}

bool VegetationDraw::draw(ID3D12GraphicsCommandList* list, std::uint64_t fence, const VegetationFrameInput& frame,
                          const VegetationMaterial& material, const float bounds[3], std::string& error) {
    error.clear();
    if (!impl_ || !list) { error = "NG-E-VEG-FORMAT: vegetation draw is not initialised"; return false; }
    auto& self = *impl_;
    if (!self.validate(fence, frame, error)) return false;
    if (self.instanceCount == 0) return true;

    self.writeConstants(frame, material, bounds);
    self.cullPass(list);

    list->SetGraphicsRootSignature(self.root.p);
    list->SetGraphicsRootConstantBufferView(0, self.constantBuffer.p->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(1, self.instanceBuffer.p->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(2, self.vertexBuffer.p->GetGPUVirtualAddress());
    // The vertex shader reads the compacted visible list, which is a UAV: the graphics
    // root signature must bind the same table or the draw faults the device.
    list->SetGraphicsRootDescriptorTable(3, self.heap.p->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootDescriptorTable(4, self.gpuSlot(kMaterialBase));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&self.indexView);
    list->SetPipelineState(self.graphicsPipeline.p);
    list->ExecuteIndirect(self.signature.p, 1, self.argsBuffer.p, 0, nullptr, 0);
    transition(list, self.argsBuffer.p, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    self.stats.frames += 1;
    self.stats.drawnInstances += self.instanceCount;
    self.stats.triangles += static_cast<std::uint64_t>(self.indexCount / 3) * self.instanceCount;
    self.stats.lastFence = fence;
    return true;
}

bool materialFromSpecies(const engine::vegetation::Species& species, VegetationMaterial& out) {
    bool bark = false, leaf = false;
    for (const auto& material : species.materials) {
        if (material.kind == engine::vegetation::MaterialKind::Bark) {
            out.barkRoughness = material.roughness;
            out.barkThicknessMetres = material.thicknessMetres;
            out.normalScale = material.normalScale;
            out.heightScaleMetres = material.heightScaleMetres;
            out.uvTileMetres = 1.6f;
            out.occlusionStrength = material.occlusionStrength;
            out.barkNormalFlipGreen = material.flipNormalGreen;
            bark = true;
        } else {
            out.leafRoughness = material.roughness;
            out.leafThicknessMetres = material.thicknessMetres;
            out.leafAbsorptionPerMetre = material.absorptionPerMetre;
            out.alphaCutoff = material.alphaCutoff;
            out.doubleSided = material.doubleSided;
            out.occlusionStrength = material.occlusionStrength;
            out.leafNormalFlipGreen = material.flipNormalGreen;
            leaf = true;
        }
    }
    return bark && leaf;
}

VegetationDrawStats VegetationDraw::stats() const { return impl_ ? impl_->stats : VegetationDrawStats{}; }

bool VegetationDraw::shutdown(std::string& error) {
    error.clear();
    if (!impl_) return true;
    if (impl_->timeline.p->GetCompletedValue() < impl_->stats.lastFence) {
        error = "NG-E-VEG-FORMAT: vegetation draw is still in flight";
        return false;
    }
    impl_.reset();
    return true;
}

} // namespace nativegpu

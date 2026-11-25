#include "water_draw.hpp"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nativegpu {
namespace {

constexpr unsigned kFrameSlots = 3;
constexpr unsigned kMaxSheets = 4;
constexpr unsigned kRows = 10;
constexpr unsigned kVerticesPerSheet = kRows * 6;
constexpr unsigned kConstantStride = 256; // D3D12 CBV alignment

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept { if (this != &other) { reset(); p = other.p; other.p = nullptr; } return *this; }
    ~Com() { reset(); }
    T** out() { return &p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

void check(HRESULT result, const char* label) {
    if (FAILED(result)) throw std::runtime_error(std::string("NG-E-WATERFALL: ") + label);
}

struct Vertex { float position[3]; float uv[2]; };
static_assert(sizeof(Vertex) == 20, "Waterfall vertex ABI");

struct Constants {
    Mat4 viewProjection{};
    float sourceTime[4]{};
    float landingWidth[4]{};
    float params[4]{};
};
static_assert(sizeof(Constants) == 112, "Waterfall constant ABI");

} // namespace

struct WaterfallDraw::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    struct Slot {
        Com<ID3D12Resource> vertices, constants;
        void* mappedVertices = nullptr;
        void* mappedConstants = nullptr;
        std::size_t capacity = 0;
    } slots[kFrameSlots];
    WaterfallDrawStats stats;

    explicit Impl(ID3D12Device* d) {
        device.p = d; d->AddRef();
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 2;
        signature.pParameters = parameters;
        Com<ID3DBlob> bytes, diagnostics;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, bytes.out(), diagnostics.out()),
              "serialize root");
        check(d->CreateRootSignature(0, bytes.p->GetBufferPointer(), bytes.p->GetBufferSize(),
                                     IID_PPV_ARGS(root.out())), "create root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.p;
        state.VS = {shaders::kWaterfallVS.data, shaders::kWaterfallVS.size};
        state.PS = {shaders::kWaterfallPS.data, shaders::kWaterfallPS.size};
        state.SampleMask = UINT_MAX;
        state.SampleDesc.Count = 1;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthEnable = TRUE;
        state.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        state.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        state.NumRenderTargets = 2;
        state.RTVFormats[0] = state.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        check(d->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(pipeline.out())), "create pipeline");
    }

    void upload(Com<ID3D12Resource>& resource, std::size_t size, void** mapped) {
        resource.reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.p->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(resource.out())), "create upload");
        D3D12_RANGE read{0, 0};
        check(resource.p->Map(0, &read, mapped), "map upload");
    }
};

WaterfallDraw::WaterfallDraw(ID3D12Device* device) : impl_(std::make_unique<Impl>(device)) {}
WaterfallDraw::~WaterfallDraw() = default;
WaterfallDrawStats WaterfallDraw::stats() const { return impl_->stats; }

bool WaterfallDraw::record(ID3D12GraphicsCommandList* list, unsigned slot,
                           const std::vector<WaterfallFrame>& frames, const Mat4& viewProjection,
                           D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                           std::string& error) {
    if (frames.empty()) return true;
    if (slot >= kFrameSlots || !list) { error = "NG-E-WATERFALL: invalid frame slot"; return false; }
    try {
        auto& state = *impl_;
        auto& target = state.slots[slot];
        std::vector<Vertex> vertices;
        vertices.reserve(std::min<std::size_t>(frames.size(), kMaxSheets) * kVerticesPerSheet);
        unsigned sheets = 0;
        for (const auto& frame : frames) {
            if (sheets == kMaxSheets) break;
            const float dx = frame.landing[0] - frame.source[0];
            const float dz = frame.landing[2] - frame.source[2];
            const float length = std::sqrt(dx * dx + dz * dz);
            float dirX = 1, dirZ = 0;
            if (length > 1e-4f) { dirX = dx / length; dirZ = dz / length; }
            const float sideX = -dirZ, sideZ = dirX;
            const float width = std::max(0.05f, frame.widthMetres);
            for (unsigned row = 0; row < kRows; ++row) {
                const float v0 = float(row) / kRows, v1 = float(row + 1) / kRows;
                const float half0 = width * .5f * (.85f + .3f * v0);
                const float half1 = width * .5f * (.85f + .3f * v1);
                const float c0x = frame.source[0] + (frame.landing[0] - frame.source[0]) * v0;
                const float c0y = frame.source[1] + (frame.landing[1] - frame.source[1]) * v0;
                const float c0z = frame.source[2] + (frame.landing[2] - frame.source[2]) * v0;
                const float c1x = frame.source[0] + (frame.landing[0] - frame.source[0]) * v1;
                const float c1y = frame.source[1] + (frame.landing[1] - frame.source[1]) * v1;
                const float c1z = frame.source[2] + (frame.landing[2] - frame.source[2]) * v1;
                const Vertex p00{{c0x - sideX * half0, c0y, c0z - sideZ * half0}, {0, v0}};
                const Vertex p01{{c0x + sideX * half0, c0y, c0z + sideZ * half0}, {1, v0}};
                const Vertex p10{{c1x - sideX * half1, c1y, c1z - sideZ * half1}, {0, v1}};
                const Vertex p11{{c1x + sideX * half1, c1y, c1z + sideZ * half1}, {1, v1}};
                vertices.push_back(p00); vertices.push_back(p10); vertices.push_back(p01);
                vertices.push_back(p01); vertices.push_back(p10); vertices.push_back(p11);
            }
            ++sheets;
        }
        if (!sheets) return true;
        const std::size_t bytes = vertices.size() * sizeof(Vertex);
        if (target.capacity < bytes) {
            state.upload(target.vertices, bytes, &target.mappedVertices);
            target.capacity = bytes;
        }
        if (!target.constants.p) state.upload(target.constants, kConstantStride * kMaxSheets, &target.mappedConstants);
        std::memcpy(target.mappedVertices, vertices.data(), bytes);
        for (unsigned i = 0; i < sheets; ++i) {
            const auto& frame = frames[i];
            Constants constants{};
            constants.viewProjection = viewProjection;
            constants.sourceTime[0] = frame.source[0];
            constants.sourceTime[1] = frame.source[1];
            constants.sourceTime[2] = frame.source[2];
            constants.sourceTime[3] = frame.time;
            constants.landingWidth[0] = frame.landing[0];
            constants.landingWidth[1] = frame.landing[1];
            constants.landingWidth[2] = frame.landing[2];
            constants.landingWidth[3] = frame.widthMetres;
            constants.params[0] = std::max(0.0f, frame.flowCubicMetresPerSecond);
            const float height = std::fabs(frame.landing[1] - frame.source[1]);
            constants.params[1] = std::clamp(0.38f + height * 0.18f, 0.38f, 0.95f);
            constants.params[2] = height;
            constants.params[3] = frame.time * 120.0f;
            std::memcpy(static_cast<char*>(target.mappedConstants) + i * kConstantStride, &constants, sizeof(constants));
        }
        list->OMSetRenderTargets(2, &rtv, TRUE, &dsv);
        list->SetGraphicsRootSignature(state.root.p);
        list->SetPipelineState(state.pipeline.p);
        list->SetGraphicsRootShaderResourceView(1, target.vertices.p->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (unsigned i = 0; i < sheets; ++i) {
            list->SetGraphicsRootConstantBufferView(0, target.constants.p->GetGPUVirtualAddress() + i * kConstantStride);
            list->DrawInstanced(kVerticesPerSheet, 1, i * kVerticesPerSheet, 0);
        }
        ++state.stats.frames;
        state.stats.vertices += vertices.size();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

} // namespace nativegpu

#include "d3d12_renderer.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nativegpu {
namespace {

constexpr unsigned FrameCount = 3;
constexpr DWORD FenceTimeoutMs = 5000;
constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

template<class T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

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
            if (ch < 32) out << "\\u" << std::hex << std::setw(4)
                             << std::setfill('0') << static_cast<unsigned>(ch);
            else out << static_cast<char>(ch);
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

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
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

// Per-face vertices provide flat normals and an independent UV square on every face.
// Triangle winding is clockwise when seen from outside the cube.
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

const char* ShaderSource = R"hlsl(
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
    float3 scenePad;
};
Texture2DArray textures : register(t0);
SamplerState materialSampler : register(s0);
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PixelInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
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
float4 PSMain(PixelInput input) : SV_TARGET {
    float3 normal = normalize(input.normal);
    float3 textureColor = textures.Sample(materialSampler, float3(input.uv, textureIndex)).rgb;
    // Entity color tints the procedural material instead of multiplying it away.
    float3 albedo = textureColor * (0.45 + baseColor.rgb * 0.55);
    float3 toKeyLight = normalize(float3(-0.45, 0.85, -0.55));
    float keyDiffuse = saturate(dot(normal, toKeyLight));
    float3 lit = albedo * (ambient.rgb + keyDiffuse * 0.45);
    for (uint i = 0; i < lightCount; ++i) {
        float3 delta = lightPositionRange[i].xyz - input.worldPosition;
        float distanceToLight = length(delta);
        float attenuation = saturate(1.0 - distanceToLight / max(lightPositionRange[i].w, 0.001));
        attenuation *= attenuation;
        float3 lightDirection = delta / max(distanceToLight, 0.001);
        float ndotl = saturate(dot(normal, lightDirection));
        float shininess = lerp(96.0, 8.0, roughness);
        float specular = pow(saturate(dot(normal, normalize(lightDirection + float3(0, 1, 0)))), shininess);
        lit += albedo * lightColorIntensity[i].rgb * lightColorIntensity[i].w * ndotl * attenuation;
        lit += lightColorIntensity[i].rgb * specular * metalness * attenuation * lightColorIntensity[i].w * 0.15;
    }
    return float4(lit, baseColor.a);
}
)hlsl";

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

Mat4 normalTransform(const Mat4& model) {
    // Inverse-transpose of the upper-left 3x3, stored column-major.
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

} // namespace

struct Renderer::Impl {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12Resource* renderTargets[FrameCount]{};
    ID3D12Resource* depth = nullptr;
    ID3D12CommandAllocator* allocators[FrameCount]{};
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* sceneConstantBuffer = nullptr;
    SceneConstants* sceneConstantMapped = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12GraphicsCommandList4* commands4 = nullptr;
    ID3D12Resource* blas = nullptr;
    ID3D12Resource* tlas = nullptr;
    ID3D12Resource* tlasInstances = nullptr;
    bool accelerationStructuresBuilt = false;
    std::string accelerationStructureError;
    UINT64 blasBytes = 0;
    UINT64 tlasBytes = 0;
    unsigned tlasInstanceCount = 0;
    unsigned materialSlots = 3;
    bool materialTableReady = false;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 nextFence = 1;
    UINT64 frameFences[FrameCount]{};
    ID3D12QueryHeap* timestampHeap = nullptr;
    ID3D12Resource* timestampReadback = nullptr;
    UINT64 timestampFrequency = 0;
    bool timestampPending[FrameCount]{};
    bool timestampsAvailable = false;
    unsigned width = 0, height = 0;
    UINT rtvStride = 0;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    FrameStats frameStats{};
    DXGI_ADAPTER_DESC1 adapterDescription{};
    unsigned bindingTier = 0;
    unsigned shaderModel = 0;
    int raytracingTier = -1;
    int meshShaderTier = -1;
    bool initialized = false;

    bool waitFor(UINT64 value, std::string& error) {
        if (!fence || !value) return true;
        UINT64 completed = fence->GetCompletedValue();
        if (completed == std::numeric_limits<UINT64>::max()) {
            error = "D3D12 device removed while waiting for GPU (" +
                    hresultText(device->GetDeviceRemovedReason()) + ")";
            return false;
        }
        if (completed >= value) return true;
        if (!checked(fence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion", error))
            return false;
        DWORD result = WaitForSingleObject(fenceEvent, FenceTimeoutMs);
        if (result != WAIT_OBJECT_0) {
            error = result == WAIT_TIMEOUT ? "GPU fence wait timed out after 5 seconds"
                                          : "GPU fence wait failed";
            return false;
        }
        completed = fence->GetCompletedValue();
        if (completed == std::numeric_limits<UINT64>::max() || completed < value) {
            error = "GPU fence did not complete; device status " +
                    hresultText(device->GetDeviceRemovedReason());
            return false;
        }
        return true;
    }

    bool waitIdle(std::string& error) {
        if (!queue || !fence || !fenceEvent) return true;
        const UINT64 value = nextFence++;
        if (!checked(queue->Signal(fence, value), "Signal idle fence", error)) return false;
        return waitFor(value, error);
    }

    bool createBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                      ID3D12Resource** output, std::string& error) {
        return createBuffer(size, type, state, D3D12_RESOURCE_FLAG_NONE, output, error);
    }

    bool createBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                      D3D12_RESOURCE_FLAGS flags, ID3D12Resource** output, std::string& error) {
        auto heap = heapProperties(type);
        auto description = bufferDescription(size);
        description.Flags = flags;
        return checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                       &description, state, nullptr, IID_PPV_ARGS(output)),
                       "Create buffer", error);
    }

    bool createTargets(std::string& error) {
        auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (unsigned index = 0; index < FrameCount; ++index) {
            if (!checked(swapChain->GetBuffer(index, IID_PPV_ARGS(&renderTargets[index])),
                         "Get swap chain buffer", error)) return false;
            device->CreateRenderTargetView(renderTargets[index], nullptr, handle);
            handle.ptr += rtvStride;
        }
        auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DepthFormat;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                     &description, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                     IID_PPV_ARGS(&depth)), "Create depth buffer", error)) return false;
        device->CreateDepthStencilView(depth, nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool createPipeline(std::string& error) {
        D3D12_DESCRIPTOR_RANGE textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;
        textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = 56;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[1].Descriptor.RegisterSpace = 0;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &textureRange;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MinLOD = 0;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy = 1;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = 3;
        rootDescription.pParameters = parameters;
        rootDescription.NumStaticSamplers = 1;
        rootDescription.pStaticSamplers = &sampler;
        rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* signature = nullptr;
        ID3DBlob* diagnostics = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                &signature, &diagnostics);
        if (FAILED(hr)) {
            error = "Serialize root signature failed (" + hresultText(hr) + ")";
            if (diagnostics) error += ": " + std::string(static_cast<char*>(diagnostics->GetBufferPointer()),
                                                         diagnostics->GetBufferSize());
            release(diagnostics);
            release(signature);
            return false;
        }
        release(diagnostics);
        hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                        IID_PPV_ARGS(&rootSignature));
        release(signature);
        if (!checked(hr, "Create root signature", error)) return false;

        ID3DBlob* vertexShader = nullptr;
        ID3DBlob* pixelShader = nullptr;
        auto compile = [&](const char* entry, const char* target, ID3DBlob** output) {
            ID3DBlob* log = nullptr;
            const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            HRESULT result = D3DCompile(ShaderSource, std::strlen(ShaderSource),
                                       "embedded_material.hlsl", nullptr, nullptr,
                                       entry, target, flags, 0, output, &log);
            if (FAILED(result)) {
                error = std::string("Compile ") + entry + " failed (" + hresultText(result) + ")";
                if (log) error += ": " + std::string(static_cast<char*>(log->GetBufferPointer()),
                                                      log->GetBufferSize());
            }
            release(log);
            return SUCCEEDED(result);
        };
        if (!compile("VSMain", "vs_5_1", &vertexShader) ||
            !compile("PSMain", "ps_5_1", &pixelShader)) {
            release(vertexShader);
            release(pixelShader);
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rootSignature;
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
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = ColorFormat;
        description.DSVFormat = DepthFormat;
        description.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline));
        release(vertexShader);
        release(pixelShader);
        return checked(hr, "Create graphics pipeline", error);
    }

    bool uploadScene(std::string& error) {
        ID3D12Resource* vertexUpload = nullptr;
        ID3D12Resource* indexUpload = nullptr;
        ID3D12Resource* textureUpload = nullptr;
        // Upload allocations survive until the initialization fence has completed.
        auto cleanup = [&] { release(vertexUpload); release(indexUpload); release(textureUpload); };
        if (!createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COPY_DEST, &vertexBuffer, error) ||
            !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COPY_DEST, &indexBuffer, error) ||
            !createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &vertexUpload, error) ||
            !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &indexUpload, error)) {
            cleanup(); return false;
        }
        auto fill = [&](ID3D12Resource* resource, const void* bytes, size_t size) {
            void* mapped = nullptr;
            D3D12_RANGE noRead{0,0};
            if (!checked(resource->Map(0, &noRead, &mapped), "Map upload buffer", error)) return false;
            std::memcpy(mapped, bytes, size);
            D3D12_RANGE written{0,size};
            resource->Unmap(0, &written);
            return true;
        };
        if (!fill(vertexUpload, CubeVertices, sizeof(CubeVertices)) ||
            !fill(indexUpload, CubeIndices, sizeof(CubeIndices))) { cleanup(); return false; }

        constexpr UINT side = 64;
        constexpr UINT materialCount = 3;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = side;
        description.Height = side;
        description.DepthOrArraySize = static_cast<UINT16>(materialCount);
        description.MipLevels = 1;
        description.Format = ColorFormat;
        description.SampleDesc.Count = 1;
        auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                     &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&texture)), "Create material texture array", error)) {
            cleanup(); return false;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[materialCount]{};
        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(&description, 0, materialCount, 0, footprints,
                                      nullptr, nullptr, &uploadSize);
        if (!createBuffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                          &textureUpload, error)) { cleanup(); return false; }
        unsigned char* pixels = nullptr;
        D3D12_RANGE noRead{0,0};
        if (!checked(textureUpload->Map(0, &noRead, reinterpret_cast<void**>(&pixels)),
                     "Map texture upload", error)) { cleanup(); return false; }
        for (UINT slice = 0; slice < materialCount; ++slice) {
            for (UINT y = 0; y < side; ++y) {
                for (UINT x = 0; x < side; ++x) {
                    auto* pixel = pixels + footprints[slice].Offset +
                                  y*footprints[slice].Footprint.RowPitch + x*4;
                    unsigned char red = 220, green = 220, blue = 220;
                    if (slice == 0) {
                        const unsigned char shade = ((x/8 + y/8) & 1) ? 205 : 255;
                        red = green = blue = shade;
                    } else if (slice == 1) {
                        const bool mortar = (y % 16 < 2) ||
                            ((x + ((y/16) % 2) * 8) % 32 < 2);
                        if (mortar) {
                            red = 205; green = 198; blue = 184;
                        } else {
                            const unsigned char variation =
                                static_cast<unsigned char>(18 + ((x/32 + y/16) & 3) * 7);
                            red = static_cast<unsigned char>(138 + variation);
                            green = static_cast<unsigned char>(62 + variation/2);
                            blue = static_cast<unsigned char>(48 + variation/3);
                        }
                    } else {
                        const unsigned char stripe = ((x/4) & 1) ? 188 : 132;
                        red = static_cast<unsigned char>(stripe - 12);
                        green = static_cast<unsigned char>(stripe);
                        blue = static_cast<unsigned char>(std::min(255, stripe + 18));
                    }
                    pixel[0] = red; pixel[1] = green; pixel[2] = blue; pixel[3] = 255;
                }
            }
        }
        D3D12_RANGE written{0, static_cast<SIZE_T>(uploadSize)};
        textureUpload->Unmap(0, &written);
        if (!checked(allocators[0]->Reset(), "Reset upload allocator", error) ||
            !checked(commands->Reset(allocators[0], nullptr), "Reset upload commands", error)) {
            cleanup(); return false;
        }
        commands->CopyBufferRegion(vertexBuffer, 0, vertexUpload, 0, sizeof(CubeVertices));
        commands->CopyBufferRegion(indexBuffer, 0, indexUpload, 0, sizeof(CubeIndices));
        for (UINT slice = 0; slice < materialCount; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = texture;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = textureUpload;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[slice];
            commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        D3D12_RESOURCE_BARRIER barriers[] = {
            transition(vertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            transition(indexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
            transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        commands->ResourceBarrier(3, barriers);
        if (!checked(commands->Close(), "Close upload commands", error)) { cleanup(); return false; }
        ID3D12CommandList* lists[] = {commands};
        queue->ExecuteCommandLists(1, lists);
        bool complete = waitIdle(error);
        cleanup();
        if (!complete) return false;
        vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexView.SizeInBytes = sizeof(CubeVertices);
        vertexView.StrideInBytes = sizeof(Vertex);
        indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexView.SizeInBytes = sizeof(CubeIndices);
        indexView.Format = DXGI_FORMAT_R16_UINT;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ColorFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2DArray.MostDetailedMip = 0;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.FirstArraySlice = 0;
        view.Texture2DArray.ArraySize = materialCount;
        device->CreateShaderResourceView(texture, &view, srvHeap->GetCPUDescriptorHandleForHeapStart());
        materialTableReady = true;
        return true;
    }

    void queryCapabilities() {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                                  &options, sizeof(options))))
            bindingTier = static_cast<unsigned>(options.ResourceBindingTier);
#if defined(__ID3D12Device1_INTERFACE_DEFINED__)
        // An older runtime can reject a newer maximum. Retry descending versions.
        for (int candidate = 0x68; candidate >= 0x60; --candidate) {
            D3D12_FEATURE_DATA_SHADER_MODEL model{};
            model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(candidate);
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                      &model, sizeof(model)))) {
                shaderModel = static_cast<unsigned>(model.HighestShaderModel);
                break;
            }
        }
        if (!shaderModel) {
            D3D12_FEATURE_DATA_SHADER_MODEL model{};
            model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0x51);
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                      &model, sizeof(model))))
                shaderModel = static_cast<unsigned>(model.HighestShaderModel);
        }
#endif
#if defined(__ID3D12Device5_INTERFACE_DEFINED__)
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                  &options5, sizeof(options5))))
            raytracingTier = static_cast<int>(options5.RaytracingTier);
#endif
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                  &options7, sizeof(options7))))
            meshShaderTier = static_cast<int>(options7.MeshShaderTier);
#endif
    }

    void buildAccelerationStructures() {
        accelerationStructuresBuilt = false;
        accelerationStructureError.clear();
        if (raytracingTier < 10) {
            accelerationStructureError = "DXR tier is unsupported";
            return;
        }
        if (!device5 || !commands4) {
            accelerationStructureError = "DXR device or command-list interface is unavailable";
            return;
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
            return;
        }

        D3D12_RAYTRACING_INSTANCE_DESC instances[4]{};
        const float positions[4][3] = {
            {0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f},
            {-3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 3.0f},
        };
        for (unsigned i = 0; i < 4; ++i) {
            instances[i].Transform[0][0] = 1.0f;
            instances[i].Transform[1][1] = 1.0f;
            instances[i].Transform[2][2] = 1.0f;
            instances[i].Transform[0][3] = positions[i][0];
            instances[i].Transform[1][3] = positions[i][1];
            instances[i].Transform[2][3] = positions[i][2];
            instances[i].InstanceID = i;
            instances[i].InstanceMask = 0xFF;
            instances[i].InstanceContributionToHitGroupIndex = 0;
            instances[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        }
        tlasInstanceCount = 4;
        if (!createBuffer(sizeof(instances), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &tlasInstances, error)) {
            accelerationStructureError = error;
            return;
        }
        void* mappedInstances = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (!checked(tlasInstances->Map(0, &noRead, &mappedInstances),
                     "Map TLAS instances", error)) {
            accelerationStructureError = error;
            return;
        }
        std::memcpy(mappedInstances, instances, sizeof(instances));
        D3D12_RANGE written{0, sizeof(instances)};
        tlasInstances->Unmap(0, &written);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.NumDescs = tlasInstanceCount;
        tlasInputs.InstanceDescs = tlasInstances->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
        if (tlasInfo.ResultDataMaxSizeInBytes == 0 || tlasInfo.ScratchDataSizeInBytes == 0) {
            accelerationStructureError = "TLAS prebuild returned an empty size";
            return;
        }

        ID3D12Resource* blasScratch = nullptr;
        ID3D12Resource* tlasScratch = nullptr;
        auto cleanup = [&] {
            release(blasScratch);
            release(tlasScratch);
        };
        if (!createBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &blas, error) ||
            !createBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          &blasScratch, error) ||
            !createBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &tlas, error) ||
            !createBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          &tlasScratch, error)) {
            accelerationStructureError = error;
            cleanup();
            return;
        }
        for (unsigned i = 0; i < 4; ++i) {
            instances[i].AccelerationStructure = blas->GetGPUVirtualAddress();
        }
        if (!checked(tlasInstances->Map(0, &noRead, &mappedInstances),
                     "Map TLAS instances for BLAS address", error)) {
            accelerationStructureError = error;
            cleanup();
            return;
        }
        std::memcpy(mappedInstances, instances, sizeof(instances));
        tlasInstances->Unmap(0, &written);

        if (!checked(allocators[0]->Reset(), "Reset AS allocator", error) ||
            !checked(commands4->Reset(allocators[0], nullptr), "Reset AS commands", error)) {
            accelerationStructureError = error;
            cleanup();
            return;
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

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc{};
        tlasDesc.Inputs = tlasInputs;
        tlasDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        tlasDesc.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
        commands4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
        D3D12_RESOURCE_BARRIER tlasBarrier{};
        tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        tlasBarrier.UAV.pResource = tlas;
        commands4->ResourceBarrier(1, &tlasBarrier);
        if (!checked(commands4->Close(), "Close AS commands", error)) {
            accelerationStructureError = error;
            cleanup();
            return;
        }
        ID3D12CommandList* lists[] = {commands4};
        queue->ExecuteCommandLists(1, lists);
        if (!waitIdle(error)) {
            accelerationStructureError = error;
            cleanup();
            return;
        }
        blasBytes = blasInfo.ResultDataMaxSizeInBytes;
        tlasBytes = tlasInfo.ResultDataMaxSizeInBytes;
        accelerationStructuresBuilt = true;
        cleanup();
    }

    void createTimestamps() {
        if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)) || !timestampFrequency) return;
        D3D12_QUERY_HEAP_DESC description{};
        description.Count = FrameCount * 2;
        description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        if (FAILED(device->CreateQueryHeap(&description, IID_PPV_ARGS(&timestampHeap)))) return;
        std::string ignored;
        if (!createBuffer(FrameCount * 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                          D3D12_RESOURCE_STATE_COPY_DEST, &timestampReadback, ignored)) {
            release(timestampHeap);
            return;
        }
        timestampsAvailable = true;
    }

    void readTimestamp(unsigned index) {
        if (!timestampsAvailable || !timestampPending[index]) return;
        const SIZE_T offset = index * 2 * sizeof(UINT64);
        D3D12_RANGE readRange{offset, offset + 2*sizeof(UINT64)};
        void* mapped = nullptr;
        if (SUCCEEDED(timestampReadback->Map(0, &readRange, &mapped))) {
            auto* ticks = reinterpret_cast<UINT64*>(static_cast<unsigned char*>(mapped) + offset);
            if (ticks[1] >= ticks[0]) {
                frameStats.gpuMs = static_cast<double>(ticks[1]-ticks[0])*1000.0 /
                                   static_cast<double>(timestampFrequency);
                frameStats.gpuValid = true;
            }
            D3D12_RANGE noWrite{0,0};
            timestampReadback->Unmap(0, &noWrite);
        }
        timestampPending[index] = false;
    }
};

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() { shutdown(); delete impl_; }

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
    if (!checked(CreateDXGIFactory1(IID_PPV_ARGS(&state.factory)), "Create DXGI factory", error))
        return false;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        HRESULT hr = state.factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { release(candidate); continue; }
        DXGI_ADAPTER_DESC1 description{};
        candidate->GetDesc1(&description);
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&state.device)))) {
            state.adapter = candidate;
            state.adapterDescription = description;
            break;
        }
        release(candidate);
    }
    if (!state.device) {
        if (!checked(state.factory->EnumWarpAdapter(IID_PPV_ARGS(&state.adapter)),
                     "Find WARP software adapter", error) ||
            !checked(D3D12CreateDevice(state.adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&state.device)),
                     "Create D3D12 device", error)) return false;
        state.adapter->GetDesc1(&state.adapterDescription);
    }
    state.device->QueryInterface(IID_PPV_ARGS(&state.device5));
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!checked(state.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&state.queue)),
                 "Create graphics queue", error)) return false;
    if (!checked(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)),
                 "Create GPU fence", error)) return false;
    state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.fenceEvent) { error = "Create GPU fence event failed"; return false; }
    DXGI_SWAP_CHAIN_DESC1 swapDescription{};
    swapDescription.Width = width;
    swapDescription.Height = height;
    swapDescription.Format = ColorFormat;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = FrameCount;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* baseSwapChain = nullptr;
    HRESULT hr = state.factory->CreateSwapChainForHwnd(state.queue, window, &swapDescription,
                                                      nullptr, nullptr, &baseSwapChain);
    if (!checked(hr, "Create swap chain", error)) return false;
    hr = baseSwapChain->QueryInterface(IID_PPV_ARGS(&state.swapChain));
    release(baseSwapChain);
    if (!checked(hr, "Query swap chain interface", error)) return false;
    state.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors = FrameCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.rtvHeap)),
                 "Create RTV heap", error)) return false;
    heapDescription.NumDescriptors = 1;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.dsvHeap)),
                 "Create DSV heap", error)) return false;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.srvHeap)),
                 "Create shader-visible texture heap", error)) return false;
    state.rtvStride = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!state.createTargets(error)) return false;
    for (unsigned index = 0; index < FrameCount; ++index) {
        if (!checked(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                     IID_PPV_ARGS(&state.allocators[index])), "Create command allocator", error))
            return false;
    }
    if (!checked(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                 state.allocators[0], nullptr, IID_PPV_ARGS(&state.commands)),
                 "Create command list", error) ||
        !checked(state.commands->Close(), "Close initial command list", error)) return false;
    state.commands->QueryInterface(IID_PPV_ARGS(&state.commands4));
    if (!state.createPipeline(error) || !state.uploadScene(error)) return false;
    if (!state.createBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                            &state.sceneConstantBuffer, error)) return false;
    D3D12_RANGE noRead{0, 0};
    if (!checked(state.sceneConstantBuffer->Map(0, &noRead,
                 reinterpret_cast<void**>(&state.sceneConstantMapped)),
                 "Map scene constants", error)) return false;
    std::memset(state.sceneConstantMapped, 0, 256);
    state.queryCapabilities();
    state.buildAccelerationStructures();
    state.createTimestamps();
    state.initialized = true;
    return true;
}

bool Renderer::resize(unsigned width, unsigned height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    // Minimized windows have no drawable area. The host resumes rendering after a nonzero resize.
    if (!width || !height || (width == state.width && height == state.height)) return true;
    if (!state.waitIdle(error)) return false;
    for (auto& resource : state.renderTargets) release(resource);
    release(state.depth);
    if (!checked(state.swapChain->ResizeBuffers(FrameCount, width, height, ColorFormat, 0),
                 "Resize swap chain", error)) return false;
    state.width = width;
    state.height = height;
    for (unsigned index = 0; index < FrameCount; ++index) {
        state.frameFences[index] = 0;
        state.timestampPending[index] = false;
    }
    state.frameStats.gpuValid = false;
    return state.createTargets(error);
}

bool Renderer::render(const Mat4& viewProjection, const std::vector<DrawItem>& items,
                      const std::vector<LightDesc>& lights, std::string& error) {
    error.clear();
    auto start = std::chrono::steady_clock::now();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    const UINT index = state.swapChain->GetCurrentBackBufferIndex();
    // Each allocator and timestamp slot is reused only after its own submission completes.
    if (!state.waitFor(state.frameFences[index], error)) return false;
    state.readTimestamp(index);
    if (!checked(state.allocators[index]->Reset(), "Reset frame allocator", error) ||
        !checked(state.commands->Reset(state.allocators[index], state.pipeline),
                 "Reset frame commands", error)) return false;
    auto* commands = state.commands;
    auto toTarget = transition(state.renderTargets[index], D3D12_RESOURCE_STATE_PRESENT,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ResourceBarrier(1, &toTarget);
    if (state.timestampsAvailable)
        commands->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, index*2);
    auto rtv = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * state.rtvStride;
    auto dsv = state.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float sky[4] = {0.055f, 0.075f, 0.11f, 1.0f};
    commands->ClearRenderTargetView(rtv, sky, 0, nullptr);
    commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(state.width),
                           static_cast<float>(state.height), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->SetGraphicsRootSignature(state.rootSignature);
    ID3D12DescriptorHeap* heaps[] = {state.srvHeap};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(2, state.srvHeap->GetGPUDescriptorHandleForHeapStart());
    SceneConstants sceneConstants{};
    sceneConstants.ambient[0] = 0.34f;
    sceneConstants.ambient[1] = 0.36f;
    sceneConstants.ambient[2] = 0.40f;
    sceneConstants.ambient[3] = 1.0f;
    const unsigned lightCount = std::min<unsigned>(4, static_cast<unsigned>(lights.size()));
    sceneConstants.lightCount = lightCount;
    for (unsigned i = 0; i < lightCount; ++i) {
        std::memcpy(sceneConstants.lightPositionRange[i], lights[i].position,
                    sizeof(lights[i].position));
        sceneConstants.lightPositionRange[i][3] = std::max(lights[i].range, 0.001f);
        std::memcpy(sceneConstants.lightColorIntensity[i], lights[i].color,
                    sizeof(lights[i].color));
        sceneConstants.lightColorIntensity[i][3] = std::max(lights[i].intensity, 0.0f);
    }
    std::memcpy(state.sceneConstantMapped, &sceneConstants, sizeof(sceneConstants));
    commands->SetGraphicsRootConstantBufferView(1, state.sceneConstantBuffer->GetGPUVirtualAddress());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->IASetVertexBuffers(0, 1, &state.vertexView);
    commands->IASetIndexBuffer(&state.indexView);
    for (const auto& item : items) {
        DrawConstants constants{};
        constants.model = item.model;
        constants.viewProjection = viewProjection;
        constants.normalMatrix = normalTransform(item.model);
        std::memcpy(constants.color, item.color, sizeof(constants.color));
        constants.roughness = std::clamp(item.roughness, 0.0f, 1.0f);
        constants.metalness = std::clamp(item.metalness, 0.0f, 1.0f);
        constants.textureIndex = std::min(item.texture, state.materialSlots - 1);
        commands->SetGraphicsRoot32BitConstants(0, 56, &constants, 0);
        commands->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }
    if (state.timestampsAvailable) {
        commands->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, index*2+1);
        commands->ResolveQueryData(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, index*2, 2,
                                  state.timestampReadback, index*2*sizeof(UINT64));
    }
    auto toPresent = transition(state.renderTargets[index], D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PRESENT);
    commands->ResourceBarrier(1, &toPresent);
    if (!checked(commands->Close(), "Close frame commands", error)) return false;
    ID3D12CommandList* lists[] = {commands};
    state.queue->ExecuteCommandLists(1, lists);
    const HRESULT presentResult = state.swapChain->Present(1, 0);
    const UINT64 fenceValue = state.nextFence++;
    if (!checked(state.queue->Signal(state.fence, fenceValue), "Signal frame fence", error)) return false;
    state.frameFences[index] = fenceValue;
    state.timestampPending[index] = state.timestampsAvailable;
    if (!checked(presentResult, "Present frame", error)) {
        error += "; device status " + hresultText(state.device->GetDeviceRemovedReason());
        return false;
    }
    state.frameStats.cpuMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now()-start).count();
    return true;
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
        << "\"sceneLights\":{\"implemented\":true,\"maxLights\":4},"
        << "\"raytracingAccelerationStructures\":{\"available\":"
        << (state.raytracingTier >= 10 ? "true" : "false")
        << ",\"built\":" << (state.accelerationStructuresBuilt ? "true" : "false")
        << ",\"effectImplemented\":false"
        << ",\"blasBytes\":" << state.blasBytes
        << ",\"tlasBytes\":" << state.tlasBytes
        << ",\"instances\":" << state.tlasInstanceCount
        << ",\"error\":" << jsonString(state.accelerationStructureError)
        << ",\"notes\":\"BLAS/TLAS build probe; no trace shader yet\"},"
        << "\"meshShader\":{\"available\":" << (state.meshShaderTier >= 10 ? "true" : "false")
        << ",\"implemented\":false}"
        << "}";
    return out.str();
}

std::string Renderer::capabilitiesJson() const {
    const auto& state = *impl_;
    std::ostringstream out;
    out << "{"
        << "\"backend\":\"Direct3D 12\","
        << "\"initialized\":" << (state.initialized ? "true" : "false") << ","
        << "\"adapter\":" << jsonString(utf8(state.adapterDescription.Description)) << ","
        << "\"softwareAdapter\":" << ((state.adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false") << ","
        << "\"dedicatedVideoMemoryBytes\":" << state.adapterDescription.DedicatedVideoMemory << ","
        << "\"resourceBindingTier\":" << state.bindingTier << ","
        << "\"shaderModel\":";
    if (state.shaderModel) out << '"' << ((state.shaderModel >> 4) & 15) << '.' << (state.shaderModel & 15) << '"';
    else out << "null";
    out << ",\"raytracingTier\":";
    if (state.raytracingTier < 0) out << "null";
    else if (state.raytracingTier == 0) out << "\"unsupported\"";
    else out << '"' << (state.raytracingTier/10) << '.' << (state.raytracingTier%10) << '"';
    out << ",\"meshShaderTier\":";
    if (state.meshShaderTier < 0) out << "null"; else out << state.meshShaderTier;
    out << ",\"gpuTimestamps\":" << (state.timestampsAvailable ? "true" : "false")
        << ",\"bufferedFrames\":" << FrameCount
        << ",\"materialTableSlots\":" << state.materialSlots
        << ",\"implemented\":{\"rasterization\":true,\"depthTest\":true,\"textureSampling\":true,"
           "\"directionalLighting\":true,\"sceneLights\":true,\"materialTable\":true,"
           "\"accelerationStructures\":" << (state.accelerationStructuresBuilt ? "true" : "false") << ","
           "\"bindless\":false,\"raytracing\":false,\"meshShaders\":false,"
           "\"pathTracing\":false,\"virtualGeometry\":false,\"streaming\":false},"
           "\"timingNotes\":\"CPU time includes fence wait and Present; GPU time measures render commands "
           "and is read after the associated frame fence. Adapter capabilities do not imply implemented effects.\"}";
    return out.str();
}

void Renderer::shutdown() {
    if (!impl_) return;
    auto& state = *impl_;
    std::string ignored;
    state.waitIdle(ignored);
    release(state.commands);
    release(state.commands4);
    for (auto& allocator : state.allocators) release(allocator);
    release(state.tlasInstances);
    release(state.tlas);
    release(state.blas);
    release(state.timestampReadback);
    release(state.timestampHeap);
    release(state.vertexBuffer);
    release(state.indexBuffer);
    release(state.texture);
    if (state.sceneConstantBuffer && state.sceneConstantMapped) {
        D3D12_RANGE noWrite{0, 0};
        state.sceneConstantBuffer->Unmap(0, &noWrite);
        state.sceneConstantMapped = nullptr;
    }
    release(state.sceneConstantBuffer);
    release(state.pipeline);
    release(state.rootSignature);
    release(state.depth);
    for (auto& resource : state.renderTargets) release(resource);
    release(state.srvHeap);
    release(state.dsvHeap);
    release(state.rtvHeap);
    release(state.swapChain);
    release(state.queue);
    release(state.fence);
    if (state.fenceEvent) { CloseHandle(state.fenceEvent); state.fenceEvent = nullptr; }
    release(state.device5);
    release(state.device);
    release(state.adapter);
    release(state.factory);
    state = Impl{};
}

} // namespace nativegpu

#include "d3d12_renderer_internal.h"

#include "matrix_math.h"
#include "nativegpu_shaders.h"
#include "png_writer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nativegpu {

namespace {
const char* RasterShaderSource = R"hlsl(
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
struct PixelOutput {
    float4 surface : SV_TARGET0;   // ambient + scene point lights
    float4 keyLight : SV_TARGET1;  // key light only, multiplied by the DXR shadow mask later
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
PixelOutput PSMain(PixelInput input) {
    float3 normal = normalize(input.normal);
    float3 textureColor = textures.Sample(materialSampler, float3(input.uv, textureIndex)).rgb;
    // Entity color tints the procedural material instead of multiplying it away.
    float3 albedo = textureColor * (0.45 + baseColor.rgb * 0.55);
    float3 toKeyLight = normalize(float3(-0.45, 0.85, -0.55));
    float keyDiffuse = saturate(dot(normal, toKeyLight));
    float3 surface = albedo * ambient.rgb;
    for (uint i = 0; i < lightCount; ++i) {
        float3 delta = lightPositionRange[i].xyz - input.worldPosition;
        float distanceToLight = length(delta);
        float attenuation = saturate(1.0 - distanceToLight / max(lightPositionRange[i].w, 0.001));
        attenuation *= attenuation;
        float3 lightDirection = delta / max(distanceToLight, 0.001);
        float ndotl = saturate(dot(normal, lightDirection));
        float shininess = lerp(96.0, 8.0, roughness);
        float specular = pow(saturate(dot(normal, normalize(lightDirection + float3(0, 1, 0)))), shininess);
        surface += albedo * lightColorIntensity[i].rgb * lightColorIntensity[i].w * ndotl * attenuation;
        surface += lightColorIntensity[i].rgb * specular * metalness * attenuation * lightColorIntensity[i].w * 0.15;
    }
    PixelOutput output;
    output.surface = float4(surface, baseColor.a);
    // The key light carries most of the direct lighting so DXR shadows are clearly
    // visible; the raster fallback uses the identical expression with mask = 1.
    output.keyLight = float4(albedo * keyDiffuse * 0.95, 0.0);
    return output;
}
)hlsl";

// The composite pass is deliberately compiled with D3DCompile (SM 5.1) from inline
// source: the base renderer must stay runnable when DXC is missing. DXC is only required
// for the ray tracing and mesh shader libraries, which D3DCompile cannot produce.
const char* CompositeShaderSource = R"hlsl(
struct CompositeVertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Texture2D<float4> baseColor : register(t0);
Texture2D<float4> keyLight : register(t1);
Texture2D<float> shadowVisibility : register(t2);
cbuffer CompositeConstants : register(b0) {
    uint debugView;
    uint3 compositePadding;
};
CompositeVertex VSMain(uint vertexId : SV_VertexID) {
    CompositeVertex output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
float4 PSMain(CompositeVertex input) : SV_Target {
    uint2 pixel = uint2(input.position.xy);
    float4 base = baseColor.Load(int3(pixel, 0));
    float4 key = keyLight.Load(int3(pixel, 0));
    float visibility = shadowVisibility.Load(int3(pixel, 0));
    // Diagnostic views used by the DXR shadow evidence: the mask itself, the isolated
    // key-light term, and the ambient surface term.
    if (debugView == 1) { return float4(visibility.xxx, 1.0); }
    if (debugView == 2) { return float4(key.rgb, 1.0); }
    if (debugView == 3) { return float4(base.rgb, 1.0); }
    if (debugView == 4) { return float4(1.0, 0.0, 1.0, 1.0); }
    // Forcing visibility to 1.0 reproduces the 0.3.0 raster image exactly.
    return float4(saturate(base.rgb + key.rgb * saturate(visibility)), base.a);
}
)hlsl";

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


Mat4 normalTransform(const Mat4& model) {
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

bool compileEmbedded(const char* source, const char* entry, const char* target,
                     ID3DBlob** output, std::string& error) {
    ID3DBlob* log = nullptr;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT result = D3DCompile(source, std::strlen(source), "embedded_raster.hlsl", nullptr,
                                nullptr, entry, target, flags, 0, output, &log);
    if (FAILED(result)) {
        error = std::string("Compile ") + entry + " failed (" + detail::hresultText(result) + ")";
        if (log) {
            error += ": " + std::string(static_cast<char*>(log->GetBufferPointer()),
                                        log->GetBufferSize());
        }
    }
    detail::release(log);
    return SUCCEEDED(result);
}
} // namespace

namespace detail {

long g_liveObjects = 0;
unsigned long long g_objectsCreated = 0;

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
    if (!input) return {};
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
            if (ch < 32) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch);
            } else {
                out << static_cast<char>(ch);
            }
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

D3D12_RESOURCE_DESC textureDescription2D(unsigned width, unsigned height,
                                        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* resource,
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

std::wstring utf16(const char* input) {
    if (!input || !*input) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input, -1, &output[0], length);
    output.pop_back();
    return output;
}

} // namespace detail

using detail::checked;
using detail::jsonString;
using detail::release;
using detail::utf8;


D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Impl::cpuDescriptor(unsigned slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * srvStride;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::Impl::gpuDescriptor(unsigned slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * srvStride;
    return handle;
}

void Renderer::Impl::transitionTo(TrackedResource& tracked, D3D12_RESOURCE_STATES state,
                                  ID3D12GraphicsCommandList* list) {
    if (!tracked.resource || tracked.state == state) return;
    const D3D12_RESOURCE_BARRIER barrier =
        detail::transitionBarrier(tracked.resource, tracked.state, state);
    list->ResourceBarrier(1, &barrier);
    tracked.state = state;
}

bool Renderer::Impl::waitFor(UINT64 value, std::string& error) {
    if (!fence || !value) return true;
    UINT64 completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<UINT64>::max()) {
        error = "D3D12 device removed while waiting for the GPU (" +
                detail::hresultText(device->GetDeviceRemovedReason()) + ")";
        return false;
    }
    if (completed >= value) return true;
    if (!checked(fence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion", error)) {
        return false;
    }
    const DWORD result = WaitForSingleObject(fenceEvent, detail::FenceTimeoutMs);
    if (result != WAIT_OBJECT_0) {
        error = result == WAIT_TIMEOUT ? "GPU fence wait timed out after 5 seconds"
                                       : "GPU fence wait failed";
        return false;
    }
    completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<UINT64>::max() || completed < value) {
        error = "GPU fence did not complete; device status " +
                detail::hresultText(device->GetDeviceRemovedReason());
        return false;
    }
    return true;
}

bool Renderer::Impl::waitIdle(std::string& error) {
    if (!queue || !fence || !fenceEvent) return true;
    const UINT64 value = nextFence++;
    if (!checked(queue->Signal(fence, value), "Signal idle fence", error)) return false;
    return waitFor(value, error);
}

bool Renderer::Impl::createBuffer(UINT64 size, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, ID3D12Resource** output,
                                  std::string& error) {
    return createBuffer(size, type, state, D3D12_RESOURCE_FLAG_NONE, output, error);
}

bool Renderer::Impl::createBuffer(UINT64 size, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                  ID3D12Resource** output, std::string& error) {
    auto heap = detail::heapProperties(type);
    auto description = detail::bufferDescription(size);
    description.Flags = flags;
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                 state, nullptr, IID_PPV_ARGS(output)),
                 "Create buffer", error)) {
        return false;
    }
    track(*output);
    return true;
}

bool Renderer::Impl::createTexture2D(unsigned textureWidth, unsigned textureHeight,
                                     DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags, const D3D12_CLEAR_VALUE* clear,
                                     TrackedResource& output, std::string& error) {
    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto description = detail::textureDescription2D(textureWidth, textureHeight, format, flags);
    ID3D12Resource* resource = nullptr;
    const std::string action = "Create render texture " + std::to_string(textureWidth) + "x" +
                               std::to_string(textureHeight) + " format " +
                               std::to_string(static_cast<int>(format)) + " flags " +
                               std::to_string(static_cast<unsigned>(flags)) +
                               " dim " + std::to_string(static_cast<int>(description.Dimension)) +
                               " mips " + std::to_string(description.MipLevels) +
                               " array " + std::to_string(description.DepthOrArraySize) +
                               " samples " + std::to_string(description.SampleDesc.Count) +
                               " quality " + std::to_string(description.SampleDesc.Quality) +
                               " layout " + std::to_string(static_cast<int>(description.Layout)) +
                               " state " + std::to_string(static_cast<unsigned>(state)) +
                               " clear " + (clear ? std::to_string(static_cast<int>(clear->Format))
                                                  : std::string("null"));
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                                 clear, IID_PPV_ARGS(&resource)),
                 action.c_str(), error)) {
        return false;
    }
    track(resource);
    output.resource = resource;
    output.state = state;
    return true;
}

bool Renderer::Impl::createDescriptorViews(std::string& error) {
    (void)error;
    srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        const unsigned base = detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
        depthView.Format = detail::DepthSrvFormat;
        depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthView.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(depth[index].resource, &depthView,
                                        cpuDescriptor(base + detail::FdsDepth));

        D3D12_SHADER_RESOURCE_VIEW_DESC baseView{};
        baseView.Format = detail::SceneColorFormat;
        baseView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        baseView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        baseView.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(sceneBaseColor[index].resource, &baseView,
                                        cpuDescriptor(base + detail::FdsBaseColor));

        D3D12_SHADER_RESOURCE_VIEW_DESC keyView = baseView;
        device->CreateShaderResourceView(sceneKeyLight[index].resource, &keyView,
                                        cpuDescriptor(base + detail::FdsKeyLight));

        D3D12_UNORDERED_ACCESS_VIEW_DESC maskUav{};
        maskUav.Format = detail::ShadowMaskFormat;
        maskUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(shadowMask[index].resource, nullptr, &maskUav,
                                         cpuDescriptor(base + detail::FdsShadowMaskUav));

        D3D12_SHADER_RESOURCE_VIEW_DESC maskSrv{};
        maskSrv.Format = detail::ShadowMaskFormat;
        maskSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maskSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maskSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(shadowMask[index].resource, &maskSrv,
                                        cpuDescriptor(base + detail::FdsShadowMaskSrv));
    }
    // Verification SRVs live above the frame blocks so the compute probe can sample heap
    // indices that the bounded 8-slot material table could never address.
    for (unsigned index = 0; index < detail::VerificationSrvCount; ++index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        const bool colorSource = index < 3;
        view.Format = colorSource ? detail::SceneColorFormat : detail::ShadowMaskFormat;
        ID3D12Resource* source = colorSource ? sceneBaseColor[index].resource
                                            : shadowMask[0].resource;
        device->CreateShaderResourceView(source, &view,
                                        cpuDescriptor(detail::VerificationSrvBase + index));
    }
    return true;
}

bool Renderer::Impl::createTargets(std::string& error) {
    auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!checked(swapChain->GetBuffer(index, IID_PPV_ARGS(&renderTargets[index])),
                     "Get swap chain buffer", error)) {
            return false;
        }
        track(renderTargets[index]);
        device->CreateRenderTargetView(renderTargets[index], nullptr, rtvHandle);
        rtvHandle.ptr += rtvStride;
    }

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = detail::DepthDsvFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = detail::DepthDsvFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    auto dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!createTexture2D(width, height, detail::DepthResourceFormat,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &depthClear,
                             depth[index], error) ||
            !createTexture2D(width, height, detail::SceneColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, sceneBaseColor[index], error) ||
            !createTexture2D(width, height, detail::SceneColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, sceneKeyLight[index], error) ||
            !createTexture2D(width, height, detail::ShadowMaskFormat,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nullptr,
                             shadowMask[index], error) ||
            !createTexture2D(width, height, detail::ColorFormat,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             nullptr, composite[index], error)) {
            return false;
        }
        device->CreateDepthStencilView(depth[index].resource, &dsvDesc, dsvHandle);
        dsvHandle.ptr += dsvStride;

        D3D12_CPU_DESCRIPTOR_HANDLE targetHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        targetHandle.ptr +=
            static_cast<SIZE_T>(detail::RtvSlotBase + index * detail::RtvSlotsPerFrame) * rtvStride;
        device->CreateRenderTargetView(sceneBaseColor[index].resource, nullptr, targetHandle);
        targetHandle.ptr += rtvStride;
        device->CreateRenderTargetView(sceneKeyLight[index].resource, nullptr, targetHandle);
        targetHandle.ptr += rtvStride;
        device->CreateRenderTargetView(composite[index].resource, nullptr, targetHandle);
    }
    rtShadowTextureBytes = static_cast<UINT64>(width) * height * 2u * detail::FrameCount;
    return createDescriptorViews(error);
}

void Renderer::Impl::releaseTargets() {
    for (auto& resource : renderTargets) destroy(resource);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        destroy(depth[index].resource);
        depth[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(sceneBaseColor[index].resource);
        sceneBaseColor[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(sceneKeyLight[index].resource);
        sceneKeyLight[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(shadowMask[index].resource);
        shadowMask[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(composite[index].resource);
        composite[index].state = D3D12_RESOURCE_STATE_COMMON;
    }
}

namespace {

bool serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& description,
                            ID3D12Device* device, ID3D12RootSignature** output,
                            std::string& error) {
    ID3DBlob* signature = nullptr;
    ID3DBlob* diagnostics = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &signature, &diagnostics);
    if (FAILED(hr)) {
        error = "Serialize root signature failed (" + detail::hresultText(hr) + ")";
        if (diagnostics) {
            error += ": " + std::string(static_cast<char*>(diagnostics->GetBufferPointer()),
                                        diagnostics->GetBufferSize());
        }
        detail::release(diagnostics);
        detail::release(signature);
        return false;
    }
    detail::release(diagnostics);
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                     IID_PPV_ARGS(output));
    detail::release(signature);
    return detail::checked(hr, "Create root signature", error);
}

} // namespace

bool Renderer::Impl::createRasterRootSignature(std::string& error) {
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
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!serializeRootSignature(description, device, &rasterRootSignature, error)) return false;
    track(rasterRootSignature);
    return true;
}

bool Renderer::Impl::createCompositeRootSignature(std::string& error) {
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (unsigned index = 0; index < 3; ++index) {
        ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[index].NumDescriptors = 1;
        ranges[index].BaseShaderRegister = index; // t0 base, t1 key light, t2 shadow mask
        ranges[index].OffsetInDescriptorsFromTableStart = index;
    }
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 3;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(description, device, &compositeRootSignature, error)) return false;
    track(compositeRootSignature);
    return true;
}

bool Renderer::Impl::createRasterPipeline(std::string& error) {
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    if (!compileEmbedded(RasterShaderSource, "VSMain", "vs_5_1", &vertexShader, error) ||
        !compileEmbedded(RasterShaderSource, "PSMain", "ps_5_1", &pixelShader, error)) {
        detail::release(vertexShader);
        detail::release(pixelShader);
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rasterRootSignature;
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
    description.NumRenderTargets = 2;
    description.RTVFormats[0] = detail::SceneColorFormat;
    description.RTVFormats[1] = detail::SceneColorFormat;
    description.DSVFormat = detail::DepthDsvFormat;
    description.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&rasterPipeline));
    detail::release(vertexShader);
    detail::release(pixelShader);
    if (FAILED(hr)) return detail::checked(hr, "Create raster pipeline", error);
    track(rasterPipeline);
    return true;
}

bool Renderer::Impl::createCompositePipeline(std::string& error) {
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    if (!compileEmbedded(CompositeShaderSource, "VSMain", "vs_5_1", &vertexShader, error) ||
        !compileEmbedded(CompositeShaderSource, "PSMain", "ps_5_1", &pixelShader, error)) {
        detail::release(vertexShader);
        detail::release(pixelShader);
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = compositeRootSignature;
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
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = detail::ColorFormat;
    description.DSVFormat = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&compositePipeline));
    detail::release(vertexShader);
    detail::release(pixelShader);
    if (FAILED(hr)) return detail::checked(hr, "Create composite pipeline", error);
    track(compositePipeline);
    return true;
}

bool Renderer::Impl::uploadScene(std::string& error) {
    ID3D12Resource* vertexUpload = nullptr;
    ID3D12Resource* indexUpload = nullptr;
    ID3D12Resource* textureUpload = nullptr;
    auto cleanup = [&] {
        destroy(vertexUpload);
        destroy(indexUpload);
        destroy(textureUpload);
    };
    if (!createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COPY_DEST, &vertexBuffer, error) ||
        !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COPY_DEST, &indexBuffer, error) ||
        !createBuffer(sizeof(CubeVertices), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &vertexUpload, error) ||
        !createBuffer(sizeof(CubeIndices), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &indexUpload, error)) {
        cleanup();
        return false;
    }
    auto fill = [&](ID3D12Resource* resource, const void* bytes, size_t size) {
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (!checked(resource->Map(0, &noRead, &mapped), "Map upload buffer", error)) return false;
        std::memcpy(mapped, bytes, size);
        D3D12_RANGE written{0, size};
        resource->Unmap(0, &written);
        return true;
    };
    if (!fill(vertexUpload, CubeVertices, sizeof(CubeVertices)) ||
        !fill(indexUpload, CubeIndices, sizeof(CubeIndices))) {
        cleanup();
        return false;
    }

    constexpr UINT side = 64;
    constexpr UINT materialCount = detail::MaterialSlotCount;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = side;
    description.Height = side;
    description.DepthOrArraySize = static_cast<UINT16>(materialCount);
    description.MipLevels = 1;
    description.Format = detail::ColorFormat;
    description.SampleDesc.Count = 1;
    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&materialTexture)),
                 "Create material texture array", error)) {
        cleanup();
        return false;
    }
    track(materialTexture);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[materialCount]{};
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&description, 0, materialCount, 0, footprints, nullptr, nullptr,
                                  &uploadSize);
    if (!createBuffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &textureUpload, error)) {
        cleanup();
        return false;
    }
    unsigned char* pixels = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (!checked(textureUpload->Map(0, &noRead, reinterpret_cast<void**>(&pixels)),
                 "Map texture upload", error)) {
        cleanup();
        return false;
    }
    for (UINT slice = 0; slice < materialCount; ++slice) {
        for (UINT y = 0; y < side; ++y) {
            for (UINT x = 0; x < side; ++x) {
                auto* pixel = pixels + footprints[slice].Offset +
                              y * footprints[slice].Footprint.RowPitch + x * 4;
                unsigned char red = 220, green = 220, blue = 220;
                if (slice == 0) {
                    const unsigned char shade = ((x / 8 + y / 8) & 1) ? 205 : 255;
                    red = green = blue = shade;
                } else if (slice == 1) {
                    const bool mortar = (y % 16 < 2) || ((x + ((y / 16) % 2) * 8) % 32 < 2);
                    if (mortar) {
                        red = 205; green = 198; blue = 184;
                    } else {
                        const unsigned char variation =
                            static_cast<unsigned char>(18 + ((x / 32 + y / 16) & 3) * 7);
                        red = static_cast<unsigned char>(138 + variation);
                        green = static_cast<unsigned char>(62 + variation / 2);
                        blue = static_cast<unsigned char>(48 + variation / 3);
                    }
                } else if (slice == 3) {
                    // Dedicated missing-material pattern: magenta checker.
                    const bool light = ((x / 8 + y / 8) & 1) != 0;
                    red = light ? 255 : 40; green = light ? 40 : 0; blue = light ? 220 : 60;
                } else if (slice == 4) {
                    red = 90; green = 170; blue = 210; // extra blue-grey variant
                } else if (slice == 5) {
                    red = 200; green = 190; blue = 120; // extra sand variant
                } else if (slice == 6) {
                    red = 120; green = 200; blue = 140; // extra green variant
                } else {
                    const unsigned char stripe = ((x / 4) & 1) ? 188 : 132;
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
        cleanup();
        return false;
    }
    commands->CopyBufferRegion(vertexBuffer, 0, vertexUpload, 0, sizeof(CubeVertices));
    commands->CopyBufferRegion(indexBuffer, 0, indexUpload, 0, sizeof(CubeIndices));
    for (UINT slice = 0; slice < materialCount; ++slice) {
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = materialTexture;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = slice;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = textureUpload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[slice];
        commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barriers[] = {
        detail::transitionBarrier(vertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        detail::transitionBarrier(indexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER),
        detail::transitionBarrier(materialTexture, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    commands->ResourceBarrier(3, barriers);
    if (!checked(commands->Close(), "Close upload commands", error)) {
        cleanup();
        return false;
    }
    ID3D12CommandList* lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    const bool complete = waitIdle(error);
    cleanup();
    if (!complete) return false;
    vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes = sizeof(CubeVertices);
    vertexView.StrideInBytes = sizeof(Vertex);
    indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
    indexView.SizeInBytes = sizeof(CubeIndices);
    indexView.Format = DXGI_FORMAT_R16_UINT;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = detail::ColorFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.FirstArraySlice = 0;
    view.Texture2DArray.ArraySize = materialCount;
    device->CreateShaderResourceView(materialTexture, &view,
                                    cpuDescriptor(detail::SlotMaterialTable));
    materialTableReady = true;
    return true;
}

void Renderer::Impl::queryCapabilities() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options)))) {
        bindingTier = static_cast<unsigned>(options.ResourceBindingTier);
    }
#if defined(__ID3D12Device1_INTERFACE_DEFINED__)
    // An older runtime can reject a newer maximum. Retry descending versions.
    for (int candidate = 0x68; candidate >= 0x60; --candidate) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(candidate);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                  sizeof(model)))) {
            shaderModel = static_cast<unsigned>(model.HighestShaderModel);
            break;
        }
    }
    if (!shaderModel) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0x51);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                  sizeof(model)))) {
            shaderModel = static_cast<unsigned>(model.HighestShaderModel);
        }
    }
#endif
#if defined(__ID3D12Device5_INTERFACE_DEFINED__)
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                              sizeof(options5)))) {
        raytracingTier = static_cast<int>(options5.RaytracingTier);
    }
#endif
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                              sizeof(options7)))) {
        meshShaderTier = static_cast<int>(options7.MeshShaderTier);
    }
#endif
}

bool Renderer::Impl::buildAccelerationStructures() {
    accelerationStructureError.clear();
    if (raytracingTier < 10) {
        accelerationStructureError = "DXR tier is unsupported";
        return false;
    }
    if (!device5 || !commands4) {
        accelerationStructureError = "DXR device or command-list interface is unavailable";
        return false;
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
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.NumDescs = tlasCapacity;
    tlasInputs.InstanceDescs = 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
    if (tlasInfo.ResultDataMaxSizeInBytes == 0 || tlasInfo.ScratchDataSizeInBytes == 0) {
        accelerationStructureError = "TLAS prebuild returned an empty size";
        return false;
    }

    ID3D12Resource* blasScratch = nullptr;
    auto cleanup = [&] {
        destroy(blasScratch);
    };
    if (!createBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &blas, error) ||
        !createBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      &blasScratch, error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!createBuffer(static_cast<UINT64>(tlasCapacity) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                          &tlasInstances[index], error) ||
            !createBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &tlas[index], error) ||
            !createBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          &tlasScratch[index], error)) {
            accelerationStructureError = error;
            cleanup();
            return false;
        }
    }

    if (!checked(allocators[0]->Reset(), "Reset AS allocator", error) ||
        !checked(commands4->Reset(allocators[0], nullptr), "Reset AS commands", error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
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
    if (!checked(commands4->Close(), "Close AS commands", error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    ID3D12CommandList* lists[] = {commands4};
    queue->ExecuteCommandLists(1, lists);
    if (!waitIdle(error)) {
        accelerationStructureError = error;
        cleanup();
        return false;
    }
    cleanup();
    blasBytes = blasInfo.ResultDataMaxSizeInBytes;
    tlasBytes = tlasInfo.ResultDataMaxSizeInBytes;
    tlasScratchBytes = tlasInfo.ScratchDataSizeInBytes;
    blasBuilt = true;

    // TLAS descriptors can only be created once the resources exist.
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.RaytracingAccelerationStructure.Location = tlas[index]->GetGPUVirtualAddress();
        const unsigned base = detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;
        device->CreateShaderResourceView(nullptr, &view,
                                        cpuDescriptor(base + detail::FdsTopLevelAccelerationStructure));
    }
    return true;
}

bool Renderer::Impl::updateTopLevelAccelerationStructure(
    ID3D12GraphicsCommandList4* list, const std::vector<DrawItem>& items, unsigned frameIndex) {
    if (!blasBuilt || !list) return false;
    unsigned count = static_cast<unsigned>(items.size());
    if (count > tlasCapacity) count = tlasCapacity;
    if (count == 0) return false;

    D3D12_RAYTRACING_INSTANCE_DESC* instances = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(tlasInstances[frameIndex]->Map(0, &noRead, reinterpret_cast<void**>(&instances)))) {
        return false;
    }
    std::memset(instances, 0, static_cast<size_t>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    const D3D12_GPU_VIRTUAL_ADDRESS blasAddress = blas->GetGPUVirtualAddress();
    for (unsigned index = 0; index < count; ++index) {
        const Mat4& model = items[index].model;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                instances[index].Transform[row][column] = model.m[column * 4 + row];
            }
        }
        instances[index].InstanceID = index;
        instances[index].InstanceMask = 0xFF;
        instances[index].InstanceContributionToHitGroupIndex = 0;
        // Both faces must occlude: the shadow ray starts on a surface.
        instances[index].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instances[index].AccelerationStructure = blasAddress;
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)};
    tlasInstances[frameIndex]->Unmap(0, &written);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = count;
    inputs.InstanceDescs = tlasInstances[frameIndex]->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
    description.Inputs = inputs;
    description.ScratchAccelerationStructureData = tlasScratch[frameIndex]->GetGPUVirtualAddress();
    description.DestAccelerationStructureData = tlas[frameIndex]->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&description, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = tlas[frameIndex];
    list->ResourceBarrier(1, &barrier);
    tlasInstanceCount = count;
    ++tlasRebuilds;
    return true;
}

void Renderer::Impl::createTimestamps() {
    if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)) || !timestampFrequency) return;
    D3D12_QUERY_HEAP_DESC description{};
    description.Count = detail::FrameCount * detail::TimestampsPerFrame;
    description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (FAILED(device->CreateQueryHeap(&description, IID_PPV_ARGS(&timestampHeap)))) return;
    track(timestampHeap);
    std::string ignored;
    if (!createBuffer(detail::FrameCount * detail::TimestampsPerFrame * sizeof(UINT64),
                      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                      &timestampReadback, ignored)) {
        destroy(timestampHeap);
        return;
    }
    timestampsAvailable = true;
}

void Renderer::Impl::readTimestamp(unsigned index) {
    if (!timestampsAvailable || !timestampPending[index]) return;
    const SIZE_T offset =
        static_cast<SIZE_T>(index) * detail::TimestampsPerFrame * sizeof(UINT64);
    D3D12_RANGE readRange{offset, offset + detail::TimestampsPerFrame * sizeof(UINT64)};
    void* mapped = nullptr;
    if (SUCCEEDED(timestampReadback->Map(0, &readRange, &mapped))) {
        auto* ticks = reinterpret_cast<UINT64*>(static_cast<unsigned char*>(mapped) + offset);
        const UINT64 frequency = timestampFrequency ? timestampFrequency : 1;
        if (ticks[detail::TimestampFrameEnd] >= ticks[detail::TimestampFrameBegin]) {
            frameStats.gpuMs =
                static_cast<double>(ticks[detail::TimestampFrameEnd] -
                                    ticks[detail::TimestampFrameBegin]) * 1000.0 /
                static_cast<double>(frequency);
            frameStats.gpuValid = true;
        }
        if (ticks[detail::TimestampRtEnd] > ticks[detail::TimestampRtBegin]) {
            rtLastGpuMs = static_cast<double>(ticks[detail::TimestampRtEnd] -
                                              ticks[detail::TimestampRtBegin]) * 1000.0 /
                          static_cast<double>(frequency);
            frameStats.gpuRtMs = rtLastGpuMs;
            frameStats.gpuRtValid = true;
        } else {
            frameStats.gpuRtMs = 0.0;
            frameStats.gpuRtValid = false;
        }
        D3D12_RANGE noWrite{0, 0};
        timestampReadback->Unmap(0, &noWrite);
    }
    timestampPending[index] = false;
}

} // namespace nativegpu

namespace nativegpu {

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

    if (state.validationRequested) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
    if (!checked(CreateDXGIFactory1(IID_PPV_ARGS(&state.factory)), "Create DXGI factory", error)) {
        return false;
    }
    state.track(state.factory);
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        HRESULT hr = state.factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { detail::release(candidate); continue; }
        DXGI_ADAPTER_DESC1 description{};
        candidate->GetDesc1(&description);
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&state.device)))) {
            state.adapter = candidate;
            state.adapterDescription = description;
            break;
        }
        detail::release(candidate);
    }
    if (!state.device) {
        if (!checked(state.factory->EnumWarpAdapter(IID_PPV_ARGS(&state.adapter)),
                     "Find WARP software adapter", error) ||
            !checked(D3D12CreateDevice(state.adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&state.device)),
                     "Create D3D12 device", error)) {
            return false;
        }
        state.adapter->GetDesc1(&state.adapterDescription);
    }
    state.track(state.adapter);
    state.track(state.device);
    if (state.validationRequested &&
        SUCCEEDED(state.device->QueryInterface(IID_PPV_ARGS(&state.infoQueue)))) {
        state.infoQueue->SetMuteDebugOutput(FALSE);
    }
    state.device->QueryInterface(IID_PPV_ARGS(&state.device5));
    state.track(state.device5);

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!checked(state.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&state.queue)),
                 "Create graphics queue", error)) {
        return false;
    }
    state.track(state.queue);
    if (!checked(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)),
                 "Create GPU fence", error)) {
        return false;
    }
    state.track(state.fence);
    state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.fenceEvent) { error = "Create GPU fence event failed"; return false; }

    DXGI_SWAP_CHAIN_DESC1 swapDescription{};
    swapDescription.Width = width;
    swapDescription.Height = height;
    swapDescription.Format = detail::ColorFormat;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = detail::FrameCount;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* baseSwapChain = nullptr;
    HRESULT hr = state.factory->CreateSwapChainForHwnd(state.queue, window, &swapDescription,
                                                      nullptr, nullptr, &baseSwapChain);
    if (!checked(hr, "Create swap chain", error)) return false;
    hr = baseSwapChain->QueryInterface(IID_PPV_ARGS(&state.swapChain));
    detail::release(baseSwapChain);
    if (!checked(hr, "Query swap chain interface", error)) return false;
    state.track(state.swapChain);
    state.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors = detail::RtvSlotCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.rtvHeap)),
                 "Create RTV heap", error)) {
        return false;
    }
    state.track(state.rtvHeap);
    heapDescription.NumDescriptors = detail::FrameCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.dsvHeap)),
                 "Create DSV heap", error)) {
        return false;
    }
    state.track(state.dsvHeap);
    heapDescription.NumDescriptors = detail::DescriptorHeapSlots;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!checked(state.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.srvHeap)),
                 "Create shader-visible descriptor heap", error)) {
        return false;
    }
    state.track(state.srvHeap);
    state.rtvStride = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    state.srvStride = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!state.createTargets(error)) return false;
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        if (!checked(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                     IID_PPV_ARGS(&state.allocators[index])), "Create command allocator", error)) {
            return false;
        }
        state.track(state.allocators[index]);
    }
    if (!checked(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                 state.allocators[0], nullptr, IID_PPV_ARGS(&state.commands)),
                 "Create command list", error) ||
        !checked(state.commands->Close(), "Close initial command list", error)) {
        return false;
    }
    state.track(state.commands);
    state.commands->QueryInterface(IID_PPV_ARGS(&state.commands4));
    state.track(state.commands4);
    state.commands->QueryInterface(IID_PPV_ARGS(&state.commands6));
    state.track(state.commands6);
    state.device->QueryInterface(IID_PPV_ARGS(&state.device2));
    state.track(state.device2);

    if (!state.createRasterRootSignature(error) ||
        !state.createCompositeRootSignature(error) ||
        !state.createRasterPipeline(error) ||
        !state.createCompositePipeline(error) ||
        !state.uploadScene(error)) {
        return false;
    }
    if (!state.createBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                            &state.sceneConstantBuffer, error)) {
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    if (!checked(state.sceneConstantBuffer->Map(0, &noRead,
                 reinterpret_cast<void**>(&state.sceneConstantMapped)),
                 "Map scene constants", error)) {
        return false;
    }
    std::memset(state.sceneConstantMapped, 0, 256);

    state.queryCapabilities();
    state.queryVideoMemory();
    {
        std::string materialError;
        if (!state.selfTestMaterialTable(materialError)) {
            state.addDiagnostic("NG-E-MAT-TABLE", materialError);
        }
    }
    state.buildAccelerationStructures();
    state.createTimestamps();
    // A failure to build the DXR state object is not fatal: the raster path keeps
    // rendering and every report records fallbackUsed=true with the error text.
    std::string rtError;
    if (!state.buildRaytracingPipeline(rtError)) {
        state.rtError = rtError.empty() ? "DXRT state object creation failed" : rtError;
    }
    // Mesh shader pipeline: a failure keeps the vertex path and is reported.
    std::string meshBuildError;
    if (!state.buildMeshPipeline(meshBuildError)) {
        state.meshError = meshBuildError.empty() ? "mesh pipeline creation failed"
                                                 : meshBuildError;
    }
    // Compute queue and probe: optional. Failure keeps the renderer usable and is reported
    // through probes.compute.error instead of failing initialization.
    std::string computeBuildError;
    if (!state.createComputePipeline(computeBuildError)) {
        state.computeError = computeBuildError.empty() ? "compute pipeline creation failed"
                                                       : computeBuildError;
    }
    state.initialized = true;
    return true;
}

bool Renderer::resize(unsigned width, unsigned height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    // Minimized windows have no drawable area. The host resumes after a nonzero resize.
    if (!width || !height || (width == state.width && height == state.height)) return true;
    if (!state.waitIdle(error)) return false;
    state.releaseTargets();
    if (!checked(state.swapChain->ResizeBuffers(detail::FrameCount, width, height,
                                                detail::ColorFormat, 0),
                 "Resize swap chain", error)) {
        return false;
    }
    state.width = width;
    state.height = height;
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        state.frameFences[index] = 0;
        state.timestampPending[index] = false;
    }
    state.frameStats.gpuValid = false;
    state.frameStats.gpuRtValid = false;
    return state.createTargets(error);
}

bool Renderer::render(const Mat4& viewProjection, const float cameraPosition[3],
                      const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                      std::string& error) {
    error.clear();
    const auto start = std::chrono::steady_clock::now();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    const UINT index = state.swapChain->GetCurrentBackBufferIndex();
    state.lastFrameIndex = index;
    // Fault injection for the device-removal test. This is an injected fault, not a real
    // hardware reset, and every report says so.
    if (state.faultInjectionFrame != 0 && state.framesRendered + 1 == state.faultInjectionFrame) {
        state.deviceRemovalInjected = true;
        error = "DXGI_ERROR_DEVICE_REMOVED (injected fault at frame " +
                std::to_string(state.framesRendered + 1) + ")";
        state.addDiagnostic("NG-E-DEVICE-REMOVED", error);
        return false;
    }
    if (!state.waitFor(state.frameFences[index], error)) return false;
    state.readTimestamp(index);
    if (!checked(state.allocators[index]->Reset(), "Reset frame allocator", error) ||
        !checked(state.commands->Reset(state.allocators[index], state.rasterPipeline),
                 "Reset frame commands", error)) {
        return false;
    }
    auto* list = state.commands;
    auto* list4 = state.commands4;
    const unsigned descriptorBase =
        detail::FrameDescriptorBase + index * detail::FrameDescriptorStride;

    // 1. Rebuild this frame's TLAS from the rigid instance transforms. Static hard
    //    shadows: one shadow ray per pixel, no skinning, no denoising.
    bool tlasReady = false;
    if (state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated) {
        tlasReady = state.updateTopLevelAccelerationStructure(list4, items, index);
    }

    // Compute probe (B1/B3): dispatched on the compute queue before this frame's list is
    // submitted; the graphics queue waits on the compute fence. The one-shot readback runs
    // after a fixed number of dispatches so the pipeline has actually been exercised.
    if (state.computeRequested && state.computePipelineReady) {
        if (!state.dispatchComputeProbe(lights, error)) return false;
        if (!state.computeProbeRan && state.computeDispatchedFrames >= 30) {
            if (!state.readbackComputeProbe(error)) return false;
        }
    }

    // 2. Frame-wide transitions.
    state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_DEPTH_WRITE, list);
    state.transitionTo(state.sceneBaseColor[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    state.transitionTo(state.sceneKeyLight[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    state.transitionTo(state.shadowMask[index], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, list);
    if (state.timestampsAvailable) {
        list->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                       index * detail::TimestampsPerFrame + detail::TimestampFrameBegin);
    }

    // 3. Raster pass into two offscreen targets (surface, key-light only).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
    auto rtvHandle = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(detail::RtvSlotBase +
                                         index * detail::RtvSlotsPerFrame) * state.rtvStride;
    rtvHandles[0] = rtvHandle;
    rtvHandle.ptr += state.rtvStride;
    rtvHandles[1] = rtvHandle;
    auto dsv = state.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(index) *
               state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    list->OMSetRenderTargets(2, rtvHandles, FALSE, &dsv);
    const float sky[4] = {0.055f, 0.075f, 0.11f, 1.0f};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtvHandles[0], sky, 0, nullptr);
    list->ClearRenderTargetView(rtvHandles[1], zero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(state.width),
                            static_cast<float>(state.height), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {state.srvHeap};
    list->SetDescriptorHeaps(1, heaps);

    detail::SceneConstants sceneConstants{};
    sceneConstants.ambient[0] = 0.34f;
    sceneConstants.ambient[1] = 0.36f;
    sceneConstants.ambient[2] = 0.40f;
    sceneConstants.ambient[3] = 1.0f;
    const unsigned lightCount =
        std::min<unsigned>(4, static_cast<unsigned>(lights.size()));
    sceneConstants.lightCount = lightCount;
    for (unsigned light = 0; light < lightCount; ++light) {
        std::memcpy(sceneConstants.lightPositionRange[light], lights[light].position,
                    sizeof(lights[light].position));
        sceneConstants.lightPositionRange[light][3] = std::max(lights[light].range, 0.001f);
        std::memcpy(sceneConstants.lightColorIntensity[light], lights[light].color,
                    sizeof(lights[light].color));
        sceneConstants.lightColorIntensity[light][3] = std::max(lights[light].intensity, 0.0f);
    }
    std::memcpy(state.sceneConstantMapped, &sceneConstants, sizeof(sceneConstants));

    // Mesh shader path or the traditional vertex path for this frame.
    bool usedMeshPath = false;
    if (state.meshRequested && state.meshPipelineCreated) {
        usedMeshPath = state.renderMeshPath(state.commands4, items, viewProjection, cameraPosition);
    }
    state.meshPathUsedLastFrame = usedMeshPath;
    if (usedMeshPath) {
        ++state.meshFrames;
        state.drawCallsLastFrame = static_cast<unsigned>(items.size());
        state.trianglesLastFrame = static_cast<unsigned long long>(state.meshletDrawnLastFrame) * 2ull;
    } else {
    if (state.meshRequested) ++state.meshFallbackFrames;
    list->SetPipelineState(state.rasterPipeline);
    list->SetGraphicsRootSignature(state.rasterRootSignature);
    list->SetGraphicsRootDescriptorTable(2, state.gpuDescriptor(detail::SlotMaterialTable));
    list->SetGraphicsRootConstantBufferView(1, state.sceneConstantBuffer->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &state.vertexView);
    list->IASetIndexBuffer(&state.indexView);
    unsigned draws = 0;
    for (const auto& item : items) {
        detail::DrawConstants constants{};
        constants.model = item.model;
        constants.viewProjection = viewProjection;
        constants.normalMatrix = normalTransform(item.model);
        std::memcpy(constants.color, item.color, sizeof(constants.color));
        constants.roughness = std::clamp(item.roughness, 0.0f, 1.0f);
        constants.metalness = std::clamp(item.metalness, 0.0f, 1.0f);
        // Material slot policy: resolve through the generation-checked table. A missing
        // sentinel or a stale generation falls back to the default slot with a stable code.
        unsigned textureIndex = detail::MaterialDefaultSlot;
        unsigned generation = 0;
        if (item.texture == 0xFFFFFFFFu) {
            state.materials.missingFallbacks++;
            state.addDiagnostic("NG-E-MAT-MISSING",
                                "missing material texture resolved to the default slot");
        } else if (!state.materials.resolve(item.texture, item.textureGeneration, textureIndex,
                                            generation)) {
            state.addDiagnostic("NG-E-MAT-STALE",
                                "material slot " + std::to_string(item.texture) +
                                    " rejected: released, out of range or stale generation");
        }
        constants.textureIndex = textureIndex;
        list->SetGraphicsRoot32BitConstants(0, 56, &constants, 0);
        list->DrawIndexedInstanced(36, 1, 0, 0, 0);
        ++draws;
    }
    state.drawCallsLastFrame = draws;
    state.trianglesLastFrame = static_cast<unsigned long long>(draws) * 12ull;
    }

    // 4. DXR static shadow pass, or the raster fallback with a full visibility mask.
    state.transitionTo(state.depth[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, list);
    bool dispatched = false;
    if (tlasReady && state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated) {
        if (state.timestampsAvailable) {
            list->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                           index * detail::TimestampsPerFrame + detail::TimestampRtBegin);
        }
        std::string rtDispatchError;
        dispatched = state.dispatchShadowRays(list4, index, viewProjection, cameraPosition,
                                              rtDispatchError);
        if (!dispatched) {
            state.rtError = rtDispatchError;
            ++state.rtDispatchFailures;
        }
        if (state.timestampsAvailable) {
            list->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                           index * detail::TimestampsPerFrame + detail::TimestampRtEnd);
        }
    }
    if (!dispatched) {
        const float fullVisibility[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        list->ClearUnorderedAccessViewFloat(
            state.gpuDescriptor(descriptorBase + detail::FdsShadowMaskUav),
            state.cpuDescriptor(descriptorBase + detail::FdsShadowMaskUav),
            state.shadowMask[index].resource, fullVisibility, 0, nullptr);
        ++state.rtFallbackFrames;
    }
    state.rtDispatchedLastFrame = dispatched;
    if (dispatched) ++state.rtDispatchedFrames;

    // 5. Composite: base + key light * visibility, then copy to the swap chain.
    state.transitionTo(state.shadowMask[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, list);
    state.transitionTo(state.sceneBaseColor[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       list);
    state.transitionTo(state.sceneKeyLight[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       list);
    state.transitionTo(state.composite[index], D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    D3D12_CPU_DESCRIPTOR_HANDLE compositeRtv = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    compositeRtv.ptr += static_cast<SIZE_T>(detail::RtvSlotBase +
                                            index * detail::RtvSlotsPerFrame + 2) *
                        state.rtvStride;
    list->OMSetRenderTargets(1, &compositeRtv, FALSE, nullptr);
    const float compositeClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    list->ClearRenderTargetView(compositeRtv, compositeClear, 0, nullptr);
    // The command list reset bound the raster pipeline; the composite needs its own.
    list->SetPipelineState(state.compositePipeline);
    list->SetGraphicsRootSignature(state.compositeRootSignature);
    list->SetGraphicsRootDescriptorTable(0, state.gpuDescriptor(descriptorBase +
                                                               detail::FdsBaseColor));
    list->SetGraphicsRoot32BitConstant(1, state.compositeDebugView, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    state.transitionTo(state.composite[index], D3D12_RESOURCE_STATE_COPY_SOURCE, list);
    auto toCopyDest = detail::transitionBarrier(state.renderTargets[index],
                                                D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopyDest);
    list->CopyResource(state.renderTargets[index], state.composite[index].resource);
    auto toPresent = detail::transitionBarrier(state.renderTargets[index],
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &toPresent);

    if (state.captureRequested) {
        if (!state.recordCaptureCopy(list, index, error)) return false;
    }

    if (state.timestampsAvailable) {
        list->EndQuery(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                       index * detail::TimestampsPerFrame + detail::TimestampFrameEnd);
        list->ResolveQueryData(state.timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                               index * detail::TimestampsPerFrame, detail::TimestampsPerFrame,
                               state.timestampReadback,
                               index * detail::TimestampsPerFrame * sizeof(UINT64));
    }
    if (!checked(list->Close(), "Close frame commands", error)) return false;
    state.frameStats.cpuSubmitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ID3D12CommandList* lists[] = {list};
    state.queue->ExecuteCommandLists(1, lists);
    const HRESULT presentResult = state.swapChain->Present(1, 0);
    const UINT64 fenceValue = state.nextFence++;
    if (!checked(state.queue->Signal(state.fence, fenceValue), "Signal frame fence", error)) {
        return false;
    }
    state.frameFences[index] = fenceValue;
    state.timestampPending[index] = state.timestampsAvailable;
    if (!checked(presentResult, "Present frame", error)) {
        error += "; device status " + detail::hresultText(state.device->GetDeviceRemovedReason());
        return false;
    }
    ++state.framesRendered;
    state.frameStats.cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

// Prepares a screenshot: allocates the readback target and asks the next render() to
// record the copy inside the frame submission.
bool Renderer::requestScreenshot(const std::string& path, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    if (path.empty()) { error = "Screenshot path is empty"; return false; }
    if (state.width == 0 || state.height == 0) { error = "Renderer has no drawable size"; return false; }
    D3D12_RESOURCE_DESC description = detail::textureDescription2D(
        state.width, state.height, detail::ColorFormat, D3D12_RESOURCE_FLAG_NONE);
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    state.device->GetCopyableFootprints(&description, 0, 1, 0, &state.captureFootprint, &rows,
                                        &rowBytes, &totalBytes);
    if (!state.captureReadback || state.captureReadbackBytes < totalBytes ||
        state.captureWidth != state.width || state.captureHeight != state.height) {
        state.destroy(state.captureReadback);
        if (!state.createBuffer(totalBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST, &state.captureReadback, error)) {
            state.captureReadbackBytes = 0;
            state.lastScreenshotError = error;
            return false;
        }
        state.captureReadbackBytes = totalBytes;
        state.captureWidth = state.width;
        state.captureHeight = state.height;
    }
    state.capturePath = path;
    state.captureRequested = true;
    state.captureRecorded = false;
    return true;
}

bool Renderer::Impl::recordCaptureCopy(ID3D12GraphicsCommandList* list, unsigned frameIndex,
                                       std::string& error) {
    if (!captureReadback) { error = "Screenshot readback target is missing"; return false; }
    // The composite target is already in COPY_SOURCE from the frame graph above.
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = captureReadback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = captureFootprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = composite[frameIndex].resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    captureRequested = false;
    captureRecorded = true;
    return true;
}

// Reads back the copy recorded by the frame and writes the PNG.
bool Renderer::captureScreenshot(const std::string& path, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.initialized) { error = "Renderer is not initialized"; return false; }
    if (!state.captureRecorded) {
        error = "No screenshot was recorded by the frame; call requestScreenshot() first";
        state.lastScreenshotError = error;
        return false;
    }
    if (!state.waitIdle(error)) {
        state.lastScreenshotError = error;
        return false;
    }
    const std::string target = path.empty() ? state.capturePath : path;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(state.captureReadbackBytes)};
    void* mapped = nullptr;
    if (FAILED(state.captureReadback->Map(0, &readRange, &mapped))) {
        error = "Map screenshot readback failed";
        state.lastScreenshotError = error;
        return false;
    }
    const auto* pixels = static_cast<const unsigned char*>(mapped) + state.captureFootprint.Offset;
    const std::size_t rowPitch = static_cast<std::size_t>(state.captureFootprint.Footprint.RowPitch);
    unsigned long long total = 0;
    unsigned long long count = 0;
    unsigned char lowest = 255;
    unsigned char highest = 0;
    for (unsigned row = 0; row < state.captureHeight; ++row) {
        const unsigned char* line = pixels + static_cast<std::size_t>(row) * rowPitch;
        for (unsigned column = 0; column < state.captureWidth; ++column) {
            const unsigned char value = line[static_cast<std::size_t>(column) * 4];
            total += value;
            ++count;
            if (value < lowest) lowest = value;
            if (value > highest) highest = value;
        }
    }
    state.lastScreenshotBytes = state.captureReadbackBytes;
    state.lastScreenshotMean =
        count ? static_cast<double>(total) / static_cast<double>(count) : 0.0;
    state.lastScreenshotMin = lowest;
    state.lastScreenshotMax = highest;
    const bool ok = writePng(target, state.captureWidth, state.captureHeight, pixels, rowPitch,
                             error);
    D3D12_RANGE noWrite{0, 0};
    state.captureReadback->Unmap(0, &noWrite);
    state.collectValidationMessages("screenshot");
    state.captureRecorded = false;
    if (ok) {
        ++state.screenshotsCaptured;
        state.lastScreenshotPath = target;
    } else {
        state.lastScreenshotError = error;
    }
    return ok;
}
void Renderer::setMeshShaders(bool enabled) {
    auto& state = *impl_;
    state.meshRequested = enabled;
    if (enabled && !state.meshPipelineCreated && state.initialized && state.device2) {
        std::string error;
        if (!state.buildMeshPipeline(error)) {
            state.meshError = error.empty() ? "mesh pipeline creation failed" : error;
        }
    }
}

bool Renderer::meshShadersEnabled() const {
    const auto& state = *impl_;
    return state.meshRequested && state.meshPipelineCreated;
}

bool Renderer::meshShadersImplemented() const {
    const auto& state = *impl_;
    return state.meshPipelineCreated && state.meshFrames > 0;
}

bool Renderer::meshPathActive() const { return impl_->meshPathUsedLastFrame; }

void Renderer::injectDeviceRemovalAtFrame(unsigned frame) { impl_->faultInjectionFrame = frame; }

unsigned Renderer::injectedRemovalFrame() const { return impl_->faultInjectionFrame; }

long Renderer::liveObjectCount() { return detail::g_liveObjects; }

unsigned long long Renderer::objectsCreatedCount() { return detail::g_objectsCreated; }

unsigned long long Renderer::videoMemoryBytes() {
    impl_->queryVideoMemory();
    return impl_->videoMemoryBytes;
}

bool Renderer::selfTestInvalidPipeline(std::string& code, std::string& text) {
    return impl_->selfTestInvalidPipeline(code, text);
}

void Renderer::setRtShadows(bool enabled) {
    auto& state = *impl_;
    state.rtRequested = enabled;
    if (enabled && !state.rtPipelineCreated && state.initialized && state.device5) {
        std::string error;
        if (!state.buildRaytracingPipeline(error)) {
            state.rtError = error.empty() ? "DXRT state object creation failed" : error;
        }
    }
}

void Renderer::setCompositeDebugView(unsigned view) { impl_->compositeDebugView = view; }


bool Renderer::rtShadowsEnabled() const {
    const auto& state = *impl_;
    return state.rtRequested && state.rtPipelineCreated && state.rtSbtCreated;
}

bool Renderer::rtShadowsImplemented() const {
    const auto& state = *impl_;
    return state.rtPipelineCreated && state.rtSbtCreated && state.rtDispatchedFrames > 0;
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
        << "\"sceneLights\":{\"implemented\":true,\"maxLights\":4}," << state.computeProbesFragment()
        << "\"renderTargets\":{\"mode\":\"offscreen-mrt-plus-composite\",\"frames\":"
        << detail::FrameCount
        << ",\"formats\":{\"baseColor\":\"R16G16B16A16_FLOAT\",\"keyLight\":\"R16G16B16A16_FLOAT\""
           ",\"shadowMask\":\"R16_FLOAT\",\"composite\":\"R8G8B8A8_UNORM\",\"depth\":\"R32_FLOAT\"}"
        << ",\"screenshots\":" << state.screenshotsCaptured
        << ",\"debugView\":" << state.compositeDebugView

        << ",\"validationLayer\":" << (state.infoQueue ? "true" : "false")
        << ",\"validationMessages\":["
        << [&] {
               std::ostringstream list;
               for (size_t index = 0; index < state.validationMessages.size(); ++index) {
                   if (index) list << ',';
                   list << detail::jsonString(state.validationMessages[index]);
               }
               return list.str();
           }()
        << "]"
        << ",\"screenshotBytes\":" << state.lastScreenshotBytes
        << ",\"lastScreenshot\":" << jsonString(state.lastScreenshotPath)
        << ",\"lastScreenshotMean\":" << std::fixed << std::setprecision(2) << state.lastScreenshotMean
        << ",\"lastScreenshotMin\":" << static_cast<unsigned>(state.lastScreenshotMin)
        << ",\"lastScreenshotMax\":" << static_cast<unsigned>(state.lastScreenshotMax)
        << ",\"lastScreenshotError\":" << jsonString(state.lastScreenshotError) << "},"
        << "\"raytracingAccelerationStructures\":{\"available\":"
        << (state.raytracingTier >= 10 ? "true" : "false")
        << ",\"built\":" << (state.blasBuilt ? "true" : "false")
        << ",\"effectImplemented\":"
        << ((state.rtPipelineCreated && state.rtDispatchedFrames > 0) ? "true" : "false")
        << ",\"blasBytes\":" << state.blasBytes
        << ",\"tlasBytes\":" << state.tlasBytes
        << ",\"tlasScratchBytes\":" << state.tlasScratchBytes
        << ",\"instances\":" << state.tlasInstanceCount
        << ",\"tlasCapacity\":" << state.tlasCapacity
        << ",\"tlasRebuilds\":" << state.tlasRebuilds
        << ",\"error\":" << jsonString(state.accelerationStructureError)
        << ",\"notes\":\"One unit-cube BLAS; TLAS rebuilt per frame from rigid instance transforms; no skinning\"},"
        << "\"raytracingShadows\":" << state.rtShadowsJson()
        << ",\"meshShader\":" << state.meshShaderJson()
        << ",\"bindless\":" << state.bindlessJson()
        << ",\"diagnostics\":{\"events\":" << state.diagnosticEvents << ",\"items\":[";
    for (size_t index = 0; index < state.diagnostics.size(); ++index) {
        if (index) out << ',';
        out << "{\"code\":" << jsonString(state.diagnostics[index].code)
            << ",\"occurrences\":" << state.diagnostics[index].occurrences
            << ",\"text\":" << jsonString(state.diagnostics[index].text) << "}";
    }
    out << "]}"
        << ",\"reliability\":{\"liveObjects\":" << detail::g_liveObjects
        << ",\"objectsCreated\":" << detail::g_objectsCreated
        << ",\"videoMemoryBytes\":" << state.videoMemoryBytes
        << ",\"videoMemoryBudgetBytes\":" << state.videoMemoryBudgetBytes
        << ",\"videoMemoryQuerySupported\":" << (state.videoMemoryQuerySupported ? "true" : "false")
        << ",\"injectedDeviceRemoval\":" << (state.deviceRemovalInjected ? "true" : "false")
        << ",\"faultInjectionFrame\":" << state.faultInjectionFrame
        << ",\"framesRendered\":" << state.framesRendered
        << ",\"drawCalls\":" << state.drawCallsLastFrame
        << ",\"triangles\":" << state.trianglesLastFrame << "}"
        << "}";
    return out.str();
}

std::string Renderer::capabilitiesJson() const {
    const auto& state = *impl_;
    const bool rtEffect = state.rtPipelineCreated && state.rtSbtCreated &&
                          state.rtDispatchedFrames > 0;
    std::ostringstream out;
    out << "{"
        << "\"backend\":\"Direct3D 12\","
        << "\"initialized\":" << (state.initialized ? "true" : "false") << ","
        << "\"adapter\":" << jsonString(utf8(state.adapterDescription.Description)) << ","
        << "\"softwareAdapter\":"
        << ((state.adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false") << ","
        << "\"dedicatedVideoMemoryBytes\":" << state.adapterDescription.DedicatedVideoMemory << ","
        << "\"resourceBindingTier\":" << state.bindingTier << ","
        << "\"shaderModel\":";
    if (state.shaderModel) {
        out << '"' << ((state.shaderModel >> 4) & 15) << '.' << (state.shaderModel & 15) << '"';
    } else {
        out << "null";
    }
    out << ",\"raytracingTier\":";
    if (state.raytracingTier < 0) out << "null";
    else if (state.raytracingTier == 0) out << "\"unsupported\"";
    else out << '"' << (state.raytracingTier / 10) << '.' << (state.raytracingTier % 10) << '"';
    out << ",\"meshShaderTier\":";
    if (state.meshShaderTier < 0) out << "null"; else out << state.meshShaderTier;
    out << ",\"gpuTimestamps\":" << (state.timestampsAvailable ? "true" : "false")
        << ",\"separateShadowTimestamps\":"
        << ((state.timestampsAvailable && state.framesRendered > 0) ? "true" : "false")
        << ",\"bufferedFrames\":" << detail::FrameCount
        << ",\"materialTableSlots\":" << state.materialSlots
        << ",\"offScreenComposition\":true,\"screenshots\":true"
        << ",\"dxc\":{\"available\":" << (shaders::kDxcAvailable ? "true" : "false")
        << ",\"version\":" << jsonString(shaders::kDxcVersion)
        << ",\"error\":" << jsonString(shaders::kDxcError) << "}"
        << ",\"implemented\":{\"rasterization\":true,\"depthTest\":true,\"textureSampling\":true,"
           "\"directionalLighting\":true,\"sceneLights\":true,\"materialTable\":true,"
        << "\"offScreenComposition\":true,"
        << "\"accelerationStructures\":" << (state.blasBuilt ? "true" : "false") << ","
        << "\"raytracing\":" << (rtEffect ? "true" : "false") << ","
        << "\"bindless\":false,\"meshShaders\":false,"
           "\"pathTracing\":false,\"virtualGeometry\":false,\"streaming\":false},"
           "\"timingNotes\":\"CPU time includes fence wait and Present; GPU time covers the whole "
           "command list and gpuRtMs covers only the DispatchRays window. Adapter capabilities "
           "do not imply implemented effects.\"}";
    return out.str();
}

void Renderer::Impl::clearValidationMessages() {
    if (!infoQueue) return;
    infoQueue->ClearStoredMessages();
}

void Renderer::Impl::collectValidationMessages(const char* stage) {
    if (!infoQueue) return;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &length)) || length == 0) continue;
        std::vector<char> buffer(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(index, message, &length)) || !message->pDescription) {
            continue;
        }
        if (validationMessages.size() >= 64) break;
        validationMessages.push_back(std::string("[") + stage + "] " +
                                     std::to_string(static_cast<int>(message->Severity)) + ": " +
                                     std::string(message->pDescription));
    }
    infoQueue->ClearStoredMessages();
}


void Renderer::setValidationEnabled(bool enabled) {
    if (impl_->initialized) return; // must be set before initialize()
    impl_->validationRequested = enabled;
}

void Renderer::Impl::addDiagnostic(const char* code, const std::string& text) {
    ++diagnosticEvents;
    for (auto& entry : diagnostics) {
        if (entry.code == code) {
            ++entry.occurrences;
            return;
        }
    }
    if (diagnostics.size() >= 32) return;
    Diagnostic entry;
    entry.code = code;
    entry.text = text;
    entry.occurrences = 1;
    diagnostics.push_back(entry);
}

void Renderer::Impl::queryVideoMemory() {
    videoMemoryBytes = 0;
    videoMemoryBudgetBytes = 0;
    videoMemoryQuerySupported = false;
    if (!adapter) return;
    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) && adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        const HRESULT localResult =
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        const HRESULT nonLocalResult =
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        if (SUCCEEDED(localResult)) {
            videoMemoryBytes += local.CurrentUsage;
            videoMemoryBudgetBytes += local.Budget;
            videoMemoryQuerySupported = true;
        }
        if (SUCCEEDED(nonLocalResult)) {
            videoMemoryBytes += nonLocal.CurrentUsage;
            videoMemoryBudgetBytes += nonLocal.Budget;
            videoMemoryQuerySupported = true;
        }
        adapter3->Release();
    }
}

bool Renderer::Impl::selfTestInvalidPipeline(std::string& code, std::string& text) {
    // A missing symbol is guaranteed to fail compilation. The point is that the failure is
    // captured as a stable diagnostic and the renderer keeps running.
    const char* broken = "float4 BrokenPS() : SV_Target { return missing_symbol; }";
    ID3DBlob* blob = nullptr;
    std::string compileError;
    const bool compiled = compileEmbedded(broken, "BrokenPS", "ps_5_1", &blob, compileError);
    detail::release(blob);
    code = "NG-E-PIPE-COMPILE";
    if (compiled) {
        text = "broken shader compiled unexpectedly";
        addDiagnostic("NG-E-PIPE-COMPILE", text);
        return false;
    }
    text = compileError;
    addDiagnostic("NG-E-PIPE-COMPILE", text);
    return true;
}

void Renderer::shutdown() {
    if (!impl_) return;
    auto& state = *impl_;
    std::string ignored;
    state.waitIdle(ignored);
    state.releaseRaytracingPipeline();
    state.releaseMeshPipeline();
    state.releaseComputePipeline();

    state.destroy(state.captureReadback);
    state.destroy(state.commands6);
    state.destroy(state.device2);
    state.destroy(state.commands4);
    state.destroy(state.commands);
    for (auto& allocator : state.allocators) state.destroy(allocator);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        state.destroy(state.tlasInstances[index]);
        state.destroy(state.tlasScratch[index]);
        state.destroy(state.tlas[index]);
    }
    state.destroy(state.blas);
    state.destroy(state.timestampReadback);
    state.destroy(state.timestampHeap);
    state.releaseTargets();
    state.destroy(state.vertexBuffer);
    state.destroy(state.indexBuffer);
    state.destroy(state.materialTexture);
    if (state.sceneConstantBuffer && state.sceneConstantMapped) {
        D3D12_RANGE noWrite{0, 0};
        state.sceneConstantBuffer->Unmap(0, &noWrite);
        state.sceneConstantMapped = nullptr;
    }
    state.destroy(state.sceneConstantBuffer);
    state.destroy(state.rasterPipeline);
    state.destroy(state.compositePipeline);
    state.destroy(state.rasterRootSignature);
    state.destroy(state.compositeRootSignature);
    state.destroy(state.rtRootSignature);
    state.destroy(state.infoQueue);
    state.destroy(state.srvHeap);
    state.destroy(state.dsvHeap);
    state.destroy(state.rtvHeap);
    state.destroy(state.swapChain);
    state.destroy(state.queue);
    state.destroy(state.fence);
    if (state.fenceEvent) { CloseHandle(state.fenceEvent); state.fenceEvent = nullptr; }
    state.destroy(state.device5);
    state.destroy(state.device);
    state.destroy(state.adapter);
    state.destroy(state.factory);
    state = Impl{};
}

} // namespace nativegpu




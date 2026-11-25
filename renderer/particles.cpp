// Particle system, the field it splats into, and the Eulerian solver that carries it.
//
// Per frame, all on the dedicated particle command list:
//   clear integer volumes
//   -> simulate + integer-atomic splat
//   -> UAV barrier -> integer volumes to SRV
//   -> resolve: integer density/heat injected into the float state
//   -> buoyancy -> velocity advection -> state advection -> divergence
//   -> N Jacobi pressure iterations -> gradient subtraction -> state copy back
//   -> hand the state volume to the graphics queue for the ray-marched composite
//
// The splat uses integer atomics on purpose. Float atomic adds are order dependent, so a
// float splat rebuilds a different volume depending on thread scheduling; integer adds are
// associative and commutative, so the accumulated source term is identical on every run.
// The solver downstream is single-precision and is not claimed to be deterministic.
#include "d3d12_renderer_internal.h"
#include "nativegpu_shaders.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace nativegpu {
namespace {

constexpr unsigned ParticleThreads = 64;
constexpr unsigned ResolveThreads = 8;
constexpr unsigned FluidThreads = 8;
constexpr float DensityQuantum = 4096.0f;
constexpr float HeatQuantum = 4096.0f;
constexpr float QuantumInverse = 1.0f / 65536.0f;
constexpr unsigned VolumeSteps = 48;
constexpr float Extinction = 3.2f;
constexpr float AmbientScale = 0.35f;
constexpr UINT64 ParticleTelemetryStride = 32; // two timestamps, dispatch xyz, padding
static_assert(sizeof(D3D12_DISPATCH_ARGUMENTS) == 12, "dispatch argument layout changed");

// Root constants. Every field is a float4 so the HLSL cbuffer packing is unambiguous.
struct SimConstants {
    float params[4];   // dt, elapsed, count, frame index
    float emitter[4];  // xyz centre, w radius
    float wind[4];     // xyz wind, w gravity
    float volume[4];   // xyz min, w extent
    float scales[4];   // x density quantum, y heat quantum, z dim, w lifetime
    float misc[4];     // x burst, y buoyancy, z sink, w turbulence (FIELD-REALISM-GOAL)
    float colliders[4];  // x collider count
};
static_assert(sizeof(SimConstants) == 28 * sizeof(float), "sim constant layout changed");

struct ResolveConstants {
    float resolve[4];  // x dim, y density inverse, z heat inverse, w exposure
};
static_assert(sizeof(ResolveConstants) == 4 * sizeof(float), "resolve constant layout changed");

struct VolumeConstants {
    Mat4 invViewProjection;
    float camera[4];  // xyz eye, w frame index (march jitter seed)
    float box[4];
    float params[4];  // x density gain, y extinction, z ambient, w steps
    float light[4];   // xyz direction to light, w intensity
    float fire[4];    // xyz emitter world position, w emission gain (FIELD-REALISM-GOAL)
};
static_assert(sizeof(VolumeConstants) == 36 * sizeof(float), "volume constant layout changed");

// Temporal path constants (FIELD-REALISM-GOAL batch 3).
struct AccumulateConstants {
    float accum[4];  // x blend, y half width, z half height, w unused
};
static_assert(sizeof(AccumulateConstants) == 4 * sizeof(float), "accumulate constant layout changed");

struct CompositeConstants {
    float composite[4];  // x near, y far, z full width, w full height
};
static_assert(sizeof(CompositeConstants) == 4 * sizeof(float), "composite constant layout changed");

struct FluidConstants {
    unsigned indices[4];  // read A, read B, write, dim
    float params[4];      // dt, dx, buoyancy, dissipation
    float burst[4];       // strength, blast centre xyz in normalised volume space
};
static_assert(sizeof(FluidConstants) == 12 * sizeof(unsigned), "fluid constant layout changed");

// The renderer keeps its root-signature helper private to d3d12_renderer.cpp, so this
// translation unit serializes its own signatures with the same version and error style.
bool serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& description,
                            ID3D12Device* device, ID3D12RootSignature** output,
                            std::string& error) {
    ID3DBlob* signature = nullptr;
    ID3DBlob* diagnostics = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &diagnostics);
    if (FAILED(hr)) {
        error = "Serialize particle root signature failed (" + detail::hresultText(hr) + ")";
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
    return detail::checked(hr, "Create particle root signature", error);
}

bool createComputePso(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                           const shaders::Blob& blob, ID3D12PipelineState** output,
                           const char* action, std::string& error) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.CS.pShaderBytecode = blob.data;
    description.CS.BytecodeLength = blob.size;
    if (!detail::checked(device->CreateComputePipelineState(&description, IID_PPV_ARGS(output)),
                         action, error)) {
        return false;
    }
    return true;
}

} // namespace

bool Renderer::Impl::createTexture3D(unsigned dimension, DXGI_FORMAT format,
                                     D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                     TrackedResource& output, std::string& error) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = dimension;
    description.Height = dimension;
    description.DepthOrArraySize = static_cast<UINT16>(dimension);
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;

    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ID3D12Resource* resource = nullptr;
    const std::string action = "Create " + std::to_string(dimension) + "^3 volume, format " +
                               std::to_string(static_cast<int>(format));
    if (!detail::checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                                 nullptr, IID_PPV_ARGS(&resource)),
                 action.c_str(), error)) {
        return false;
    }
    track(resource);
    output.resource = resource;
    output.state = state;
    return true;
}

bool Renderer::Impl::createParticles(std::string& error) {
    if (particlesReady) return true;
    if (!shaders::kDxcAvailable) {
        particleError = shaders::kDxcError[0] ? shaders::kDxcError : "dxcUnavailable";
        return false;
    }
    if (shaders::kParticleSimulate.size < 64 || shaders::kParticleDispatchArguments.size < 64 || shaders::kParticleResolve.size < 64 ||
        shaders::kParticleVolumeVS.size < 64 || shaders::kParticleVolumePS.size < 64 ||
        shaders::kFluidBuoyancy.size < 64 || shaders::kFluidAdvectVelocity.size < 64 ||
        shaders::kFluidAdvectState.size < 64 || shaders::kFluidDivergence.size < 64 ||
        shaders::kFluidJacobi.size < 64 || shaders::kFluidGradient.size < 64 ||
        shaders::kFluidCopyState.size < 64 || shaders::kFluidMacCormack.size < 64 ||
        shaders::kParticleAccumulate.size < 64 || shaders::kParticleCompositeVS.size < 64 ||
        shaders::kParticleCompositePS.size < 64) {
        particleError = "particleShaderBlobMissing";
        return false;
    }
    if (!particleAllocator) {
        if (!detail::checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                    IID_PPV_ARGS(&particleAllocator)),
                     "Create particle allocator", error)) {
            return false;
        }
        track(particleAllocator);
        if (!detail::checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, particleAllocator,
                                               nullptr, IID_PPV_ARGS(&particleCommands)),
                     "Create particle list", error)) {
            return false;
        }
        track(particleCommands);
        particleCommands->Close();
        if (!detail::checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&particleFence)),
                     "Create particle fence", error)) {
            return false;
        }
        track(particleFence);
    }

    // --- root signatures -------------------------------------------------------------
    D3D12_DESCRIPTOR_RANGE simUavRange{};
    simUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    simUavRange.NumDescriptors = 3;  // u0 particles, u1 density, u2 heat
    simUavRange.BaseShaderRegister = 0;
    simUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE simColliderRange{};
    simColliderRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    simColliderRange.NumDescriptors = 1;  // t0 obstacle boxes
    simColliderRange.BaseShaderRegister = 0;
    simColliderRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER simParameters[3]{};
    simParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    simParameters[0].Constants.ShaderRegister = 0;
    simParameters[0].Constants.Num32BitValues = 28;
    simParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    simParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    simParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    simParameters[1].DescriptorTable.pDescriptorRanges = &simUavRange;
    simParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    simParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    simParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    simParameters[2].DescriptorTable.pDescriptorRanges = &simColliderRange;
    simParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC simDescription{};
    simDescription.NumParameters = 3;
    simDescription.pParameters = simParameters;
    simDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(simDescription, device, &particleSimRootSignature, error)) {
        return false;
    }
    track(particleSimRootSignature);

    D3D12_DESCRIPTOR_RANGE resolveSrvRange{};
    resolveSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    resolveSrvRange.NumDescriptors = 2;  // t0 density, t1 heat
    resolveSrvRange.BaseShaderRegister = 0;
    resolveSrvRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE resolveUavRange{};
    resolveUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolveUavRange.NumDescriptors = 1;  // u0 float state
    resolveUavRange.BaseShaderRegister = 0;
    resolveUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER resolveParameters[3]{};
    resolveParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolveParameters[0].Constants.ShaderRegister = 0;
    resolveParameters[0].Constants.Num32BitValues = 4;
    resolveParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    resolveParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolveParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    resolveParameters[1].DescriptorTable.pDescriptorRanges = &resolveSrvRange;
    resolveParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    resolveParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolveParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    resolveParameters[2].DescriptorTable.pDescriptorRanges = &resolveUavRange;
    resolveParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC resolveDescription{};
    resolveDescription.NumParameters = 3;
    resolveDescription.pParameters = resolveParameters;
    resolveDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(resolveDescription, device, &particleResolveRootSignature, error)) {
        return false;
    }
    track(particleResolveRootSignature);

    D3D12_DESCRIPTOR_RANGE volumeSrvRange{};
    volumeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    volumeSrvRange.NumDescriptors = 2;  // t0 state, t1 scene depth
    volumeSrvRange.BaseShaderRegister = 0;
    volumeSrvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER volumeParameters[2]{};
    volumeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    volumeParameters[0].Constants.ShaderRegister = 0;
    volumeParameters[0].Constants.Num32BitValues = 36;
    volumeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    volumeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    volumeParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    volumeParameters[1].DescriptorTable.pDescriptorRanges = &volumeSrvRange;
    volumeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MinLOD = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC volumeDescription{};
    volumeDescription.NumParameters = 2;
    volumeDescription.pParameters = volumeParameters;
    volumeDescription.NumStaticSamplers = 1;
    volumeDescription.pStaticSamplers = &sampler;
    volumeDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(volumeDescription, device, &particleVolumeRootSignature, error)) {
        return false;
    }
    track(particleVolumeRootSignature);

    // The solver reads every field texture through the descriptor heap, so one signature
    // with four heap indices and four scalars serves all seven passes.
    D3D12_ROOT_PARAMETER fluidParameter{};
    fluidParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fluidParameter.Constants.ShaderRegister = 0;
    fluidParameter.Constants.Num32BitValues = 12;
    fluidParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC fluidDescription{};
    fluidDescription.NumParameters = 1;
    fluidDescription.pParameters = &fluidParameter;
    fluidDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    if (!serializeRootSignature(fluidDescription, device, &fluidRootSignature, error)) {
        return false;
    }
    track(fluidRootSignature);

    // --- pipelines -------------------------------------------------------------------
    if (!createComputePso(device, particleSimRootSignature, shaders::kParticleSimulate,
                               &particleSimPipeline, "Create particle simulate pipeline", error) ||
        !createComputePso(device, particleResolveRootSignature, shaders::kParticleResolve,
                               &particleResolvePipeline, "Create particle resolve pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidBuoyancy,
                               &fluidBuoyancyPipeline, "Create fluid buoyancy pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidAdvectVelocity,
                               &fluidAdvectVelocityPipeline, "Create fluid velocity pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidAdvectState,
                               &fluidAdvectStatePipeline, "Create fluid state pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidDivergence,
                               &fluidDivergencePipeline, "Create fluid divergence pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidJacobi,
                               &fluidJacobiPipeline, "Create fluid jacobi pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidGradient,
                               &fluidGradientPipeline, "Create fluid gradient pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidCopyState,
                               &fluidCopyStatePipeline, "Create fluid copy pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidCurl,
                               &fluidCurlPipeline, "Create fluid curl pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidVorticity,
                               &fluidVorticityPipeline, "Create fluid vorticity pipeline", error) ||
        !createComputePso(device, fluidRootSignature, shaders::kFluidMacCormack,
                               &fluidMacCormackPipeline, "Create fluid MacCormack pipeline", error)) {
        return false;
    }
    track(particleSimPipeline);
    track(particleResolvePipeline);
    track(fluidBuoyancyPipeline);
    track(fluidAdvectVelocityPipeline);
    track(fluidAdvectStatePipeline);
    track(fluidDivergencePipeline);
    track(fluidJacobiPipeline);
    track(fluidGradientPipeline);
    track(fluidCopyStatePipeline);
    track(fluidCurlPipeline);
    track(fluidVorticityPipeline);
    track(fluidMacCormackPipeline);

    if (!createComputePso(device, fluidRootSignature, shaders::kParticleDispatchArguments,
                          &particleDispatchArgsPipeline, "Create indirect dispatch argument pipeline", error)) return false;
    track(particleDispatchArgsPipeline);
    D3D12_INDIRECT_ARGUMENT_DESC dispatchArgument{};
    dispatchArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &dispatchArgument;
    if (!detail::checked(device->CreateCommandSignature(&signature, nullptr,
        IID_PPV_ARGS(&particleDispatchSignature)), "Create particle dispatch signature", error)) return false;
    track(particleDispatchSignature);
    if (!createBuffer(sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      &particleDispatchArgs.resource, error) ||
        !createBuffer(detail::FrameCount * ParticleTelemetryStride, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST, &particleTelemetryReadback, error)) return false;
    particleDispatchArgs.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_UNORDERED_ACCESS_VIEW_DESC argumentUav{};
    argumentUav.Format = DXGI_FORMAT_R32_TYPELESS;
    argumentUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    argumentUav.Buffer.NumElements = 3;
    argumentUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(particleDispatchArgs.resource, nullptr, &argumentUav,
        cpuDescriptor(detail::ParticleGlobalBase + detail::PgsDispatchArgsUav));
    if (SUCCEEDED(computeQueue->GetTimestampFrequency(&particleTimestampFrequency)) && particleTimestampFrequency) {
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queries.Count = detail::FrameCount * 2;
        if (SUCCEEDED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&particleTimestampHeap)))) track(particleTimestampHeap);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC volumePipeline{};
    volumePipeline.pRootSignature = particleVolumeRootSignature;
    volumePipeline.VS = {shaders::kParticleVolumeVS.data, shaders::kParticleVolumeVS.size};
    volumePipeline.PS = {shaders::kParticleVolumePS.data, shaders::kParticleVolumePS.size};
    volumePipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    volumePipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    volumePipeline.RasterizerState.DepthClipEnable = TRUE;
    volumePipeline.RasterizerState.FrontCounterClockwise = FALSE;
    volumePipeline.DepthStencilState.DepthEnable = FALSE;
    volumePipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    volumePipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    volumePipeline.DepthStencilState.StencilEnable = FALSE;
    auto& blend = volumePipeline.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    volumePipeline.SampleMask = UINT_MAX;
    volumePipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    volumePipeline.NumRenderTargets = 1;
    volumePipeline.RTVFormats[0] = detail::SceneColorFormat;
    volumePipeline.DSVFormat = DXGI_FORMAT_UNKNOWN;
    volumePipeline.SampleDesc.Count = 1;
    if (!detail::checked(device->CreateGraphicsPipelineState(&volumePipeline,
                                                     IID_PPV_ARGS(&particleVolumePipeline)),
                 "Create particle volume pipeline", error)) {
        return false;
    }
    track(particleVolumePipeline);

    // --- temporal path (FIELD-REALISM-GOAL batch 3) --------------------------------
    // The march pipeline renders the same PSVolume shading into a half-res RGBA16F target
    // with blending off; the accumulate compute pass folds it into the history; the
    // composite pipeline upsamples the history over the scene colour with depth-aware
    // bilateral weights.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC marchPipeline = volumePipeline;
    auto& marchBlend = marchPipeline.BlendState.RenderTarget[0];
    marchBlend.BlendEnable = FALSE;
    marchPipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!detail::checked(device->CreateGraphicsPipelineState(&marchPipeline,
                                                     IID_PPV_ARGS(&particleMarchPipeline)),
                 "Create particle march pipeline", error)) {
        return false;
    }
    track(particleMarchPipeline);

    D3D12_DESCRIPTOR_RANGE accumSrvRange{};
    accumSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    accumSrvRange.NumDescriptors = 1;  // t0 half-res march result
    accumSrvRange.BaseShaderRegister = 0;
    accumSrvRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE accumUavRange{};
    accumUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    accumUavRange.NumDescriptors = 1;  // u0 history
    accumUavRange.BaseShaderRegister = 0;
    accumUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER accumParameters[3]{};
    accumParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    accumParameters[0].Constants.ShaderRegister = 0;
    accumParameters[0].Constants.Num32BitValues = 4;
    accumParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    accumParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    accumParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    accumParameters[1].DescriptorTable.pDescriptorRanges = &accumSrvRange;
    accumParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    accumParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    accumParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    accumParameters[2].DescriptorTable.pDescriptorRanges = &accumUavRange;
    accumParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC accumDescription{};
    accumDescription.NumParameters = 3;
    accumDescription.pParameters = accumParameters;
    accumDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(accumDescription, device, &particleAccumulateRootSignature,
                                error)) {
        return false;
    }
    track(particleAccumulateRootSignature);
    if (!createComputePso(device, particleAccumulateRootSignature, shaders::kParticleAccumulate,
                          &particleAccumulatePipeline, "Create particle accumulate pipeline",
                          error)) {
        return false;
    }
    track(particleAccumulatePipeline);

    D3D12_DESCRIPTOR_RANGE compositeHistoryRange{};
    compositeHistoryRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compositeHistoryRange.NumDescriptors = 1;  // t0 history
    compositeHistoryRange.BaseShaderRegister = 0;
    compositeHistoryRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE compositeDepthRange{};
    compositeDepthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compositeDepthRange.NumDescriptors = 1;  // t1 scene depth
    compositeDepthRange.BaseShaderRegister = 1;  // t0 is the history table above
    compositeDepthRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER compositeParameters[3]{};
    compositeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compositeParameters[0].Constants.ShaderRegister = 0;
    compositeParameters[0].Constants.Num32BitValues = 4;
    compositeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    compositeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compositeParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    compositeParameters[1].DescriptorTable.pDescriptorRanges = &compositeHistoryRange;
    compositeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    compositeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compositeParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    compositeParameters[2].DescriptorTable.pDescriptorRanges = &compositeDepthRange;
    compositeParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC compositeDescription{};
    compositeDescription.NumParameters = 3;
    compositeDescription.pParameters = compositeParameters;
    compositeDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!serializeRootSignature(compositeDescription, device, &particleCompositeRootSignature,
                                error)) {
        return false;
    }
    track(particleCompositeRootSignature);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC compositePipeline = volumePipeline;
    compositePipeline.pRootSignature = particleCompositeRootSignature;
    compositePipeline.VS = {shaders::kParticleCompositeVS.data, shaders::kParticleCompositeVS.size};
    compositePipeline.PS = {shaders::kParticleCompositePS.data, shaders::kParticleCompositePS.size};
    // Blending stays premultiplied-over (inherited from volumePipeline); the RTV format
    // returns to the scene colour format.
    if (!detail::checked(device->CreateGraphicsPipelineState(&compositePipeline,
                                                     IID_PPV_ARGS(&particleCompositePipeline)),
                 "Create particle composite pipeline", error)) {
        return false;
    }
    track(particleCompositePipeline);

    // --- constant buffers ------------------------------------------------------------
    if (!createBuffer(sizeof(SimConstants), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &particleSimConstants, error) ||
        !createBuffer(sizeof(ResolveConstants), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &particleResolveConstants, error) ||
        !createBuffer(sizeof(VolumeConstants), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &particleVolumeConstants, error) ||
        !createBuffer(sizeof(AccumulateConstants), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &particleAccumulateConstants, error) ||
        !createBuffer(sizeof(CompositeConstants), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &particleCompositeConstants, error)) {
        return false;
    }
    if (!detail::checked(particleSimConstants->Map(0, nullptr, &particleSimConstantsMapped),
                 "Map particle sim constants", error) ||
        !detail::checked(particleResolveConstants->Map(0, nullptr, &particleResolveConstantsMapped),
                 "Map particle resolve constants", error) ||
        !detail::checked(particleVolumeConstants->Map(0, nullptr, &particleVolumeConstantsMapped),
                 "Map particle volume constants", error) ||
        !detail::checked(particleAccumulateConstants->Map(0, nullptr,
                 &particleAccumulateConstantsMapped),
                 "Map particle accumulate constants", error) ||
        !detail::checked(particleCompositeConstants->Map(0, nullptr,
                 &particleCompositeConstantsMapped),
                 "Map particle composite constants", error)) {
        return false;
    }

    // --- particle pool ---------------------------------------------------------------
    if (!createBuffer(static_cast<UINT64>(particleCount) * 32, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &particlePool, error)) {
        return false;
    }
    // Obstacles are small and change rarely, so an upload buffer rewritten each frame is
    // cheaper than tracking a dirty flag.
    if (!createBuffer(static_cast<UINT64>(detail::ParticleColliderCapacity) * 32,
                      D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &particleColliders, error)) {
        return false;
    }
    if (!detail::checked(particleColliders->Map(0, nullptr, &particleCollidersMapped),
                         "Map particle colliders", error)) {
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC colliderSrv{};
    colliderSrv.Format = DXGI_FORMAT_UNKNOWN;
    colliderSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    colliderSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colliderSrv.Buffer.FirstElement = 0;
    colliderSrv.Buffer.NumElements = detail::ParticleColliderCapacity;
    colliderSrv.Buffer.StructureByteStride = 32;
    device->CreateShaderResourceView(particleColliders, &colliderSrv,
                                     cpuDescriptor(detail::ParticleColliderSrv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC poolUav{};
    poolUav.Format = DXGI_FORMAT_UNKNOWN;
    poolUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    poolUav.Buffer.FirstElement = 0;
    poolUav.Buffer.NumElements = particleCount;
    poolUav.Buffer.StructureByteStride = 32;
    poolUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    // --- volumes and their views -----------------------------------------------------
    const unsigned dim = detail::ParticleVolumeDim;
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        const unsigned base = detail::ParticleFrameBase + index * detail::ParticleFrameStride;
        // The pool descriptor is duplicated per frame so the simulation's three UAVs are
        // always contiguous, whichever swap-chain index this frame uses.
        device->CreateUnorderedAccessView(particlePool, nullptr, &poolUav,
                                          cpuDescriptor(base + detail::PfsParticleUav));

        if (!createTexture3D(dim, DXGI_FORMAT_R32_UINT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeDensity[index],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R32_UINT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeHeat[index],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeState[index][0],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeState[index][1],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeStateC[index],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeVelocity[index][0],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeVelocity[index][1],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R32_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumePressure[index][0],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R32_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumePressure[index][1],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R32_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeDivergence[index],
                             error) ||
            !createTexture3D(dim, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, volumeCurl[index],
                             error)) {
            return false;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC textureUav{};
        textureUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        textureUav.Texture3D.MipSlice = 0;
        textureUav.Texture3D.FirstWSlice = 0;
        textureUav.Texture3D.WSize = dim;

        textureUav.Format = DXGI_FORMAT_R32_UINT;
        device->CreateUnorderedAccessView(volumeDensity[index].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsDensityIntUav));
        device->CreateUnorderedAccessView(volumeHeat[index].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsHeatIntUav));

        textureUav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(volumeState[index][0].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsStateAUav));
        device->CreateUnorderedAccessView(volumeState[index][1].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsStateBUav));
        device->CreateUnorderedAccessView(volumeStateC[index].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsStateCUav));
        device->CreateUnorderedAccessView(volumeVelocity[index][0].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsVelocityAUav));
        device->CreateUnorderedAccessView(volumeVelocity[index][1].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsVelocityBUav));

        textureUav.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(volumePressure[index][0].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsPressureAUav));
        device->CreateUnorderedAccessView(volumePressure[index][1].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsPressureBUav));
        device->CreateUnorderedAccessView(volumeDivergence[index].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsDivergenceUav));
        textureUav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(volumeCurl[index].resource, nullptr, &textureUav,
                                          cpuDescriptor(base + detail::PfsCurlUav));

        D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
        textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        textureSrv.Texture3D.MipLevels = 1;
        textureSrv.Texture3D.MostDetailedMip = 0;
        textureSrv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(volumeDensity[index].resource, &textureSrv,
                                         cpuDescriptor(base + detail::PfsDensityIntSrv));
        device->CreateShaderResourceView(volumeHeat[index].resource, &textureSrv,
                                         cpuDescriptor(base + detail::PfsHeatIntSrv));

        // The volumetric draw binds state + depth through one contiguous table, so the
        // state SRV sits immediately before the depth SRV.
        textureSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(volumeState[index][0].resource, &textureSrv,
                                         cpuDescriptor(base + detail::PfsStateASrv));
    }

    // The simulation owns the solver channels for as long as the system exists. Density and
    // heat are only needed eventually (they feed a visual volume); pressure, divergence and
    // curl are per-frame intermediates that must be Strong.
    acquireFieldLease(detail::FieldChannel::Density, detail::FieldConsistency::Eventual);
    acquireFieldLease(detail::FieldChannel::Heat, detail::FieldConsistency::Eventual);
    acquireFieldLease(detail::FieldChannel::Velocity, detail::FieldConsistency::Strong);
    acquireFieldLease(detail::FieldChannel::Pressure, detail::FieldConsistency::Strong);
    acquireFieldLease(detail::FieldChannel::Divergence, detail::FieldConsistency::Strong);
    acquireFieldLease(detail::FieldChannel::Curl, detail::FieldConsistency::Strong);

    particlesReady = true;
    particleError.clear();
    return true;
}

// Screen-sized targets for the temporal path: a half-res march target and the accumulation
// history. Both are single copies (they are only touched on the graphics queue inside
// drawParticles) and are recreated lazily when the window size changes.
bool Renderer::Impl::createVolumeTargets(std::string& error) {
    const unsigned halfWidth = width / 2;
    const unsigned halfHeight = height / 2;
    if (halfWidth == 0 || halfHeight == 0) {
        error = "zero-size volume target";
        return false;
    }
    if (volumeTargetsReady && volumeHalfWidth == halfWidth && volumeHalfHeight == halfHeight) {
        return true;
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = halfWidth;
    description.Height = halfHeight;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto heap = detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);

    // Half-res march target: a render target with an SRV for the accumulate pass.
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = description.Format;
    ID3D12Resource* halfResource = nullptr;
    if (!detail::checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                 &description, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 &clear, IID_PPV_ARGS(&halfResource)),
                 "Create half-res volume target", error)) {
        return false;
    }
    track(halfResource);
    volumeHalf.resource = halfResource;
    volumeHalf.state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // History: written by the accumulate compute pass, sampled by the composite.
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* historyResource = nullptr;
    if (!detail::checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                 &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 nullptr, IID_PPV_ARGS(&historyResource)),
                 "Create volume history", error)) {
        return false;
    }
    track(historyResource);
    volumeHistory.resource = historyResource;
    volumeHistory.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_CPU_DESCRIPTOR_HANDLE halfRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    halfRtv.ptr += static_cast<SIZE_T>(rtvStride) * detail::RtvVolumeHalf;
    device->CreateRenderTargetView(halfResource, nullptr, halfRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(halfResource, &srv,
                                     cpuDescriptor(detail::ParticleGlobalBase + detail::PgsHalfSrv));
    device->CreateShaderResourceView(historyResource, &srv,
                                     cpuDescriptor(detail::ParticleGlobalBase + detail::PgsHistorySrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(historyResource, nullptr, &uav,
                                      cpuDescriptor(detail::ParticleGlobalBase + detail::PgsHistoryUav));

    volumeHalfWidth = halfWidth;
    volumeHalfHeight = halfHeight;
    volumeTargetsReady = true;
    temporalHistoryValid = false;
    temporalAccumFrames = 0;
    return true;
}

void Renderer::Impl::releaseParticles() {
    if (particleSimConstantsMapped && particleSimConstants) {
        particleSimConstants->Unmap(0, nullptr);
        particleSimConstantsMapped = nullptr;
    }
    if (particleResolveConstantsMapped && particleResolveConstants) {
        particleResolveConstants->Unmap(0, nullptr);
        particleResolveConstantsMapped = nullptr;
    }
    if (particleVolumeConstantsMapped && particleVolumeConstants) {
        particleVolumeConstants->Unmap(0, nullptr);
        particleVolumeConstantsMapped = nullptr;
    }
    if (particleAccumulateConstantsMapped && particleAccumulateConstants) {
        particleAccumulateConstants->Unmap(0, nullptr);
        particleAccumulateConstantsMapped = nullptr;
    }
    if (particleCompositeConstantsMapped && particleCompositeConstants) {
        particleCompositeConstants->Unmap(0, nullptr);
        particleCompositeConstantsMapped = nullptr;
    }
    destroy(particleTimestampHeap);
    destroy(particleTelemetryReadback);
    destroy(particleDispatchArgs.resource);
    particleDispatchArgs.state = D3D12_RESOURCE_STATE_COMMON;
    destroy(particleDispatchSignature);
    destroy(particleDispatchArgsPipeline);
    particleTimestampFrequency = 0;
    for (unsigned i = 0; i < detail::FrameCount; ++i) {
        particleTelemetryFence[i] = 0;
        particleExpectedGroups[i] = 0;
    }
    particleIndirectDispatches = particleReadbackSamples = particleArgsMismatches = particleTimingSamples = 0;
    particleComputeMs = particleComputeTotalMs = 0.0;
    destroy(particleCompositePipeline);
    destroy(particleAccumulatePipeline);
    destroy(particleMarchPipeline);
    destroy(particleCompositeRootSignature);
    destroy(particleAccumulateRootSignature);
    destroy(particleAccumulateConstants);
    destroy(particleCompositeConstants);
    destroy(volumeHistory.resource);
    volumeHistory.state = D3D12_RESOURCE_STATE_COMMON;
    destroy(volumeHalf.resource);
    volumeHalf.state = D3D12_RESOURCE_STATE_COMMON;
    volumeTargetsReady = false;
    volumeHalfWidth = 0;
    volumeHalfHeight = 0;
    temporalHistoryValid = false;
    temporalAccumFrames = 0;
    destroy(fluidCopyStatePipeline);
    destroy(fluidVorticityPipeline);
    destroy(fluidMacCormackPipeline);
    destroy(fluidCurlPipeline);
    destroy(fluidGradientPipeline);
    destroy(fluidJacobiPipeline);
    destroy(fluidDivergencePipeline);
    destroy(fluidAdvectStatePipeline);
    destroy(fluidAdvectVelocityPipeline);
    destroy(fluidBuoyancyPipeline);
    destroy(fluidRootSignature);
    destroy(particleVolumePipeline);
    destroy(particleResolvePipeline);
    destroy(particleSimPipeline);
    destroy(particleVolumeRootSignature);
    destroy(particleResolveRootSignature);
    destroy(particleSimRootSignature);
    destroy(particleSimConstants);
    destroy(particleResolveConstants);
    destroy(particleVolumeConstants);
    destroy(fluidConstants);
    if (particleCollidersMapped && particleColliders) {
        particleColliders->Unmap(0, nullptr);
        particleCollidersMapped = nullptr;
    }
    destroy(particleColliders);
    destroy(particlePool);
    for (unsigned index = 0; index < detail::FrameCount; ++index) {
        for (unsigned buffer = 0; buffer < 2; ++buffer) {
            destroy(volumeState[index][buffer].resource);
            volumeState[index][buffer].state = D3D12_RESOURCE_STATE_COMMON;
            destroy(volumeVelocity[index][buffer].resource);
            volumeVelocity[index][buffer].state = D3D12_RESOURCE_STATE_COMMON;
            destroy(volumePressure[index][buffer].resource);
            volumePressure[index][buffer].state = D3D12_RESOURCE_STATE_COMMON;
        }
        destroy(volumeStateC[index].resource);
        volumeStateC[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(volumeDivergence[index].resource);
        volumeDivergence[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(volumeCurl[index].resource);
        volumeCurl[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(volumeDensity[index].resource);
        volumeDensity[index].state = D3D12_RESOURCE_STATE_COMMON;
        destroy(volumeHeat[index].resource);
        volumeHeat[index].state = D3D12_RESOURCE_STATE_COMMON;
    }
    destroy(particleFence);
    destroy(particleCommands);
    destroy(particleAllocator);
    particleFenceValue = 0;
    for (unsigned channel = 0; channel < detail::FieldChannelCount; ++channel) {
        while (channelLeases[channel].references > 0) {
            releaseFieldLease(static_cast<detail::FieldChannel>(channel));
        }
    }
    particlesReady = false;
}

void Renderer::Impl::acquireFieldLease(detail::FieldChannel channel,
                                       detail::FieldConsistency consistency) {
    const unsigned index = static_cast<unsigned>(channel);
    if (index >= detail::FieldChannelCount) return;
    ChannelLease& lease = channelLeases[index];
    ++lease.references;
    if (consistency > lease.consistency) lease.consistency = consistency;
    ++lease.acquisitions;
    lease.lastFrame = particleFrames;
    ++leaseAcquisitions;
}

void Renderer::Impl::releaseFieldLease(detail::FieldChannel channel) {
    const unsigned index = static_cast<unsigned>(channel);
    if (index >= detail::FieldChannelCount) return;
    ChannelLease& lease = channelLeases[index];
    if (lease.references == 0) return;
    --lease.references;
    if (lease.references == 0) lease.consistency = detail::FieldConsistency::Stale;
    ++leaseReleases;
}

bool Renderer::Impl::fieldLeasesComplete() const {
    // Every channel the solver touches must be leased by someone. This is the gate that
    // makes the lease a contract rather than a bookkeeping artefact.
    for (unsigned channel = 0; channel < detail::FieldChannelCount; ++channel) {
        if (channelLeases[channel].references == 0) return false;
    }
    return true;
}

std::string Renderer::Impl::fieldLeasesJson() const {
    std::ostringstream out;
    out << "\"leases\":{";
    for (unsigned channel = 0; channel < detail::FieldChannelCount; ++channel) {
        const ChannelLease& lease = channelLeases[channel];
        const char* level = lease.consistency == detail::FieldConsistency::Strong ? "strong"
                            : lease.consistency == detail::FieldConsistency::Eventual ? "eventual"
                                                                                      : "stale";
        if (channel) out << ",";
        out << "\"" << detail::FieldChannelName(static_cast<detail::FieldChannel>(channel))
            << "\":{\"refs\":" << lease.references << ",\"level\":\"" << level
            << "\",\"acquisitions\":" << lease.acquisitions << "}";
    }
    out << "},\"leaseAcquisitions\":" << leaseAcquisitions
        << ",\"leaseReleases\":" << leaseReleases
        << ",\"leaseStalls\":" << leaseStalls
        << ",\"leasesComplete\":" << (fieldLeasesComplete() ? "true" : "false");
    return out.str();
}


bool Renderer::Impl::dispatchParticles(unsigned frameIndex, float deltaTime, std::string& error) {
    if (!particlesRequested || !particlesReady) return true;
    if (!computeQueue) return true;
    // A frame whose required channels are not leased is skipped rather than solved on stale
    // data. The stall is counted and reported so it can never be mistaken for a clean frame.
    if (!fieldLeasesComplete()) {
        ++leaseStalls;
        return true;
    }

    // Read only a completed old ring slot. This path never waits for the GPU and
    // never supplies dispatch control; the GPU consumes its own argument buffer.
    if (particleTelemetryFence[frameIndex] && particleFence->GetCompletedValue() >= particleTelemetryFence[frameIndex]) {
        const SIZE_T offset = static_cast<SIZE_T>(frameIndex * ParticleTelemetryStride);
        D3D12_RANGE readRange{offset, offset + ParticleTelemetryStride};
        void* mapped = nullptr;
        if (SUCCEEDED(particleTelemetryReadback->Map(0, &readRange, &mapped))) {
            auto* bytes = static_cast<unsigned char*>(mapped) + offset;
            UINT64 ticks[2]{};
            unsigned args[3]{};
            std::memcpy(ticks, bytes, sizeof(ticks));
            std::memcpy(args, bytes + 16, sizeof(args));
            if (args[0] != particleExpectedGroups[frameIndex] || args[1] != 1 || args[2] != 1) ++particleArgsMismatches;
            ++particleReadbackSamples;
            if (particleTimestampHeap && ticks[1] >= ticks[0] && particleTimestampFrequency) {
                particleComputeMs = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / particleTimestampFrequency;
                particleComputeTotalMs += particleComputeMs;
                ++particleTimingSamples;
            }
            D3D12_RANGE noWrite{0,0};
            particleTelemetryReadback->Unmap(0, &noWrite);
            particleTelemetryFence[frameIndex] = 0;
        }
    }

    const unsigned dim = detail::ParticleVolumeDim;
    const unsigned base = detail::ParticleFrameBase + frameIndex * detail::ParticleFrameStride;
    const float dx = particleVolumeExtent / static_cast<float>(dim);

    // Explosion burst: one hard pulse at the requested frame, decaying over ~1 s.
    float burst = 0.0f;
    if (particleExplodeAt > 0 && particleFrames >= particleExplodeAt) {
        const float age = static_cast<float>(particleFrames - particleExplodeAt);
        burst = age < 60.0f ? std::exp(-age / 14.0f) : 0.0f;
    }
    particleBurst = burst;
    if (burst > particleBurstPeak) particleBurstPeak = burst;

    SimConstants sim{};
    sim.params[0] = deltaTime;
    sim.params[1] = static_cast<float>(particleFrames) * deltaTime;
    sim.params[2] = static_cast<float>(particleCount);
    sim.params[3] = static_cast<float>(particleFrames);
    sim.emitter[0] = particleEmitter[0];
    sim.emitter[1] = particleEmitter[1];
    sim.emitter[2] = particleEmitter[2];
    // Emitter radius: a campfire burns across the whole wood bed, not out of a pinhole. 0.16
    // widens the base without diluting the splat so much that the plume goes optically thin.
    sim.emitter[3] = particleFirePreset ? 0.16f : 0.25f;
    sim.wind[0] = particleFirePreset ? 0.05f : 0.12f;
    sim.wind[1] = 0.0f;
    sim.wind[2] = particleFirePreset ? -0.03f : -0.08f;
    sim.wind[3] = 9.81f;
    sim.volume[0] = particleVolumeMin[0];
    sim.volume[1] = particleVolumeMin[1];
    sim.volume[2] = particleVolumeMin[2];
    sim.volume[3] = particleVolumeExtent;
    sim.scales[0] = DensityQuantum;
    sim.scales[1] = HeatQuantum;
    sim.scales[2] = static_cast<float>(dim);
    sim.scales[3] = particleFirePreset ? 2.6f : 12.0f;
    sim.misc[0] = burst;
    sim.misc[1] = particleBuoyancy;
    sim.misc[2] = particleSink;
    // Curl-noise turbulence (FIELD-REALISM-GOAL batch 2): fire tongues tear and wander,
    // smoke drifts in slow eddies. Deterministic per particle position, so the splat
    // contract is untouched.
    sim.misc[3] = particleFirePreset ? 1.6f : 0.55f;
    // Obstacles are uploaded before the constants so the kernel never sees a count that
    // points past the buffer it just read.
    const unsigned uploadCount =
        particleColliderCount < detail::ParticleColliderCapacity ? particleColliderCount
                                                                 : detail::ParticleColliderCapacity;
    if (uploadCount > 0 && particleCollidersMapped && !fieldColliders.empty()) {
        std::memcpy(particleCollidersMapped, fieldColliders.data(),
                    static_cast<size_t>(uploadCount) * sizeof(FieldCollider));
    }
    sim.colliders[0] = static_cast<float>(uploadCount);
    std::memcpy(particleSimConstantsMapped, &sim, sizeof(sim));

    ResolveConstants resolve{};
    resolve.resolve[0] = static_cast<float>(dim);
    resolve.resolve[1] = QuantumInverse;
    resolve.resolve[2] = QuantumInverse;
    resolve.resolve[3] = particleFirePreset ? 0.05f : 0.07f;
    std::memcpy(particleResolveConstantsMapped, &resolve, sizeof(resolve));

    if (!detail::checked(particleAllocator->Reset(), "Reset particle allocator", error)) return false;
    if (!detail::checked(particleCommands->Reset(particleAllocator, nullptr), "Reset particle list", error)) {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    particleCommands->SetDescriptorHeaps(1, heaps);
    if (particleTimestampHeap) particleCommands->EndQuery(particleTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2);

    auto barrierUav = [&](ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        particleCommands->ResourceBarrier(1, &barrier);
    };

    // --- 1. clear the integer splat targets ------------------------------------------
    transitionTo(volumeDensity[frameIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);
    transitionTo(volumeHeat[frameIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);
    const UINT clearZero[4] = {0, 0, 0, 0};
    particleCommands->ClearUnorderedAccessViewUint(
        gpuDescriptor(base + detail::PfsDensityIntUav),
        cpuDescriptor(base + detail::PfsDensityIntUav), volumeDensity[frameIndex].resource, clearZero,
        0, nullptr);
    particleCommands->ClearUnorderedAccessViewUint(
        gpuDescriptor(base + detail::PfsHeatIntUav), cpuDescriptor(base + detail::PfsHeatIntUav),
        volumeHeat[frameIndex].resource, clearZero, 0, nullptr);
    barrierUav(volumeDensity[frameIndex].resource);
    barrierUav(volumeHeat[frameIndex].resource);

    // --- 2. generate arguments on GPU, then simulate + integer-atomic splat ----------
    transitionTo(particleDispatchArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);
    FluidConstants argumentConstants{};
    argumentConstants.indices[0] = detail::ParticleGlobalBase + detail::PgsDispatchArgsUav;
    argumentConstants.indices[3] = particleCount;
    particleCommands->SetComputeRootSignature(fluidRootSignature);
    particleCommands->SetComputeRoot32BitConstants(0, 12, &argumentConstants, 0);
    particleCommands->SetPipelineState(particleDispatchArgsPipeline);
    particleCommands->Dispatch(1,1,1);
    barrierUav(particleDispatchArgs.resource);
    transitionTo(particleDispatchArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, particleCommands);
    particleCommands->SetComputeRootSignature(particleSimRootSignature);
    particleCommands->SetComputeRoot32BitConstants(0, 28, &sim, 0);
    particleCommands->SetComputeRootDescriptorTable(1,
                                                    gpuDescriptor(base + detail::PfsParticleUav));
    particleCommands->SetComputeRootDescriptorTable(
        2, gpuDescriptor(detail::ParticleColliderSrv));
    particleCommands->SetPipelineState(particleSimPipeline);
    particleCommands->ExecuteIndirect(particleDispatchSignature, 1, particleDispatchArgs.resource, 0, nullptr, 0);
    ++particleIndirectDispatches;
    barrierUav(volumeDensity[frameIndex].resource);
    barrierUav(volumeHeat[frameIndex].resource);

    // --- 3. resolve: integer source into the float state -----------------------------
    transitionTo(volumeDensity[frameIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 particleCommands);
    transitionTo(volumeHeat[frameIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 particleCommands);
    transitionTo(volumeState[frameIndex][0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);
    particleCommands->SetComputeRootSignature(particleResolveRootSignature);
    particleCommands->SetComputeRoot32BitConstants(0, 4, &resolve, 0);
    particleCommands->SetComputeRootDescriptorTable(1, gpuDescriptor(base + detail::PfsDensityIntSrv));
    particleCommands->SetComputeRootDescriptorTable(2, gpuDescriptor(base + detail::PfsStateAUav));
    particleCommands->SetPipelineState(particleResolvePipeline);
    particleCommands->Dispatch((dim + ResolveThreads - 1) / ResolveThreads,
                               (dim + ResolveThreads - 1) / ResolveThreads,
                               (dim + ResolveThreads - 1) / ResolveThreads);
    barrierUav(volumeState[frameIndex][0].resource);
    transitionTo(volumeDensity[frameIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);
    transitionTo(volumeHeat[frameIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, particleCommands);

    // --- 4. solver -------------------------------------------------------------------
    const unsigned groups = (dim + FluidThreads - 1) / FluidThreads;
    // Blast centre in normalised volume space; strength is the burst scaled to m/s.
    float burstCentre[3] = {
        (particleEmitter[0] - particleVolumeMin[0]) / particleVolumeExtent,
        (particleEmitter[1] - particleVolumeMin[1]) / particleVolumeExtent,
        (particleEmitter[2] - particleVolumeMin[2]) / particleVolumeExtent};
    const float burstStrength = particleBurst * 3.5f;
    auto fluidPass = [&](ID3D12PipelineState* pipeline, unsigned readA, unsigned readB,
                         unsigned write, float buoyancy, float dissipation,
                         ID3D12Resource* written, float dtScale = 1.0f) {
        FluidConstants constants{};
        constants.indices[0] = readA;
        constants.indices[1] = readB;
        constants.indices[2] = write;
        constants.indices[3] = dim;
        constants.params[0] = deltaTime * dtScale;
        constants.params[1] = dx;
        constants.params[2] = buoyancy;
        constants.params[3] = dissipation;
        constants.burst[0] = burstStrength;
        constants.burst[1] = burstCentre[0];
        constants.burst[2] = burstCentre[1];
        constants.burst[3] = burstCentre[2];
        particleCommands->SetPipelineState(pipeline);
        particleCommands->SetComputeRootSignature(fluidRootSignature);
        particleCommands->SetComputeRoot32BitConstants(0, 12, &constants, 0);
        particleCommands->Dispatch(groups, groups, groups);
        if (written) barrierUav(written);
    };

    auto slot = [&](unsigned index) {
        return detail::ParticleFrameBase + frameIndex * detail::ParticleFrameStride + index;
    };
    const unsigned stateA = slot(detail::PfsStateAUav);
    const unsigned stateB = slot(detail::PfsStateBUav);
    const unsigned velocityA = slot(detail::PfsVelocityAUav);
    const unsigned velocityB = slot(detail::PfsVelocityBUav);
    const unsigned pressureA = slot(detail::PfsPressureAUav);
    const unsigned pressureB = slot(detail::PfsPressureBUav);
    const unsigned divergence = slot(detail::PfsDivergenceUav);
    const unsigned curl = slot(detail::PfsCurlUav);

    fluidPass(fluidBuoyancyPipeline, stateA, stateA, velocityA, particleBuoyancy,
              1.2f, volumeVelocity[frameIndex][0].resource);
    fluidPass(fluidAdvectVelocityPipeline, velocityA, velocityA, velocityB, 0.0f, 0.6f,
              volumeVelocity[frameIndex][1].resource);
    // MacCormack second-order state advection (FIELD-REALISM-GOAL batch 2): forward
    // semi-Lagrangian to stateB, backward to the scratch buffer, then the correction pass
    // restores the detail that first-order backtracing dissipates.
    fluidPass(fluidAdvectStatePipeline, stateA, velocityB, stateB, 0.0f,
              particleFirePreset ? 2.5f : 1.6f, volumeState[frameIndex][1].resource);
    fluidPass(fluidAdvectStatePipeline, stateB, velocityB, slot(detail::PfsStateCUav), 0.0f,
              0.0f, volumeStateC[frameIndex].resource, -1.0f);
    fluidPass(fluidMacCormackPipeline, stateA, slot(detail::PfsStateCUav), stateB, 0.0f, 0.0f,
              volumeState[frameIndex][1].resource);
    // Vorticity confinement runs on the advected field, before the projection removes the
    // divergence it introduces.
    fluidPass(fluidCurlPipeline, velocityB, velocityB, curl, 0.0f, 0.0f,
              volumeCurl[frameIndex].resource);
    fluidPass(fluidVorticityPipeline, curl, velocityB, velocityA, 1.1f, 0.0f,
              volumeVelocity[frameIndex][0].resource);
    fluidPass(fluidDivergencePipeline, velocityA, velocityA, divergence, 0.0f, 0.0f,
              volumeDivergence[frameIndex].resource);

    // Pressure starts from zero and Jacobi alternates A/B. An even iteration count ends in
    // A, which is the buffer the gradient pass reads.
    const float clearPressure[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    transitionTo(volumePressure[frameIndex][0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 particleCommands);
    particleCommands->ClearUnorderedAccessViewFloat(
        gpuDescriptor(base + detail::PfsPressureAUav),
        cpuDescriptor(base + detail::PfsPressureAUav), volumePressure[frameIndex][0].resource,
        clearPressure, 0, nullptr);
    barrierUav(volumePressure[frameIndex][0].resource);

    unsigned readSlot = pressureA;
    unsigned writeSlot = pressureB;
    for (unsigned iteration = 0; iteration < fluidIterations; ++iteration) {
        fluidPass(fluidJacobiPipeline, readSlot, divergence, writeSlot, 0.0f, 0.0f,
                  (writeSlot == pressureA ? volumePressure[frameIndex][0].resource
                                          : volumePressure[frameIndex][1].resource));
        const unsigned previous = readSlot;
        readSlot = writeSlot;
        writeSlot = previous;
    }

    // With an even iteration count the final write landed in pressureA. The gradient pass
    // reads and writes velocityA in place: each thread only touches its own cell and the
    // pressure neighbours, so there is no cross-thread velocity race.
    fluidPass(fluidGradientPipeline, velocityA, pressureA, velocityA, 0.0f, 0.0f,
              volumeVelocity[frameIndex][0].resource);
    fluidPass(fluidCopyStatePipeline, stateB, stateB, stateA, 0.0f, 0.0f,
              volumeState[frameIndex][0].resource);

    fluidPasses += 10 + fluidIterations;

    if (particleTimestampHeap) {
        particleCommands->EndQuery(particleTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2 + 1);
        particleCommands->ResolveQueryData(particleTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
            frameIndex * 2, 2, particleTelemetryReadback, frameIndex * ParticleTelemetryStride);
    }
    transitionTo(particleDispatchArgs, D3D12_RESOURCE_STATE_COPY_SOURCE, particleCommands);
    particleCommands->CopyBufferRegion(particleTelemetryReadback, frameIndex * ParticleTelemetryStride + 16,
                                      particleDispatchArgs.resource, 0, sizeof(D3D12_DISPATCH_ARGUMENTS));
    transitionTo(particleDispatchArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, particleCommands);

    if (!detail::checked(particleCommands->Close(), "Close particle list", error)) return false;
    computeQueue->ExecuteCommandLists(
        1, reinterpret_cast<ID3D12CommandList* const*>(&particleCommands));
    ++particleFenceValue;
    if (!detail::checked(computeQueue->Signal(particleFence, particleFenceValue), "Signal particle fence",
                 error)) {
        return false;
    }
    particleTelemetryFence[frameIndex] = particleFenceValue;
    particleExpectedGroups[frameIndex] = (particleCount + ParticleThreads - 1) / ParticleThreads;
    if (!detail::checked(queue->Wait(particleFence, particleFenceValue), "Graphics waits for particles",
                 error)) {
        return false;
    }
    ++particleFrames;
    return true;
}

void Renderer::Impl::drawParticles(ID3D12GraphicsCommandList* list, const Mat4& viewProjection,
                                   const float cameraPosition[3], unsigned frameIndex) {
    if (!particlesRequested || !particlesReady) return;
    Mat4 inverse{};
    if (!invertMatrix(viewProjection, inverse)) return;
    // The volumetric pass is a second consumer: it takes a short render lease on the two
    // channels it samples, and gives them back before the next simulation step.
    acquireFieldLease(detail::FieldChannel::Density, detail::FieldConsistency::Eventual);
    acquireFieldLease(detail::FieldChannel::Heat, detail::FieldConsistency::Eventual);

    VolumeConstants constants{};
    constants.invViewProjection = inverse;
    constants.camera[0] = cameraPosition[0];
    constants.camera[1] = cameraPosition[1];
    constants.camera[2] = cameraPosition[2];
    // Frame index drives the per-pixel march jitter that removes banding.
    constants.camera[3] = static_cast<float>(particleFrames);
    constants.box[0] = particleVolumeMin[0];
    constants.box[1] = particleVolumeMin[1];
    constants.box[2] = particleVolumeMin[2];
    constants.box[3] = particleVolumeExtent;
    // Fire reads as a dense, optically thick body: the wider emitter disc spreads the same
    // particle count over a bigger area, so the density gain rises with it.
    constants.params[0] = particleFirePreset ? 2.2f : 1.0f;
    constants.params[1] = particleExtinction;
    // Ambient follows the scene's ambient luminance instead of a fixed daylight value, so
    // the same plume is bright at noon and almost black at night (FIELD-REALISM-GOAL).
    const float ambientLuminance = 0.2126f * environment.ambient[0] +
                                   0.7152f * environment.ambient[1] +
                                   0.0722f * environment.ambient[2];
    constants.params[2] = particleAmbient * (0.15f + 0.85f * ambientLuminance / 0.36f);
    constants.params[3] = static_cast<float>(VolumeSteps);
    // Key light comes from the scene environment: the moon at night, the sun by day.
    constants.light[0] = environment.keyDirection[0];
    constants.light[1] = environment.keyDirection[1];
    constants.light[2] = environment.keyDirection[2];
    float length = std::sqrt(constants.light[0] * constants.light[0] +
                             constants.light[1] * constants.light[1] +
                             constants.light[2] * constants.light[2]);
    if (length < 1e-4f) {
        constants.light[0] = 0.35f;
        constants.light[1] = 0.85f;
        constants.light[2] = 0.40f;
        length = std::sqrt(constants.light[0] * constants.light[0] +
                           constants.light[1] * constants.light[1] +
                           constants.light[2] * constants.light[2]);
    }
    constants.light[0] /= length;
    constants.light[1] /= length;
    constants.light[2] /= length;
    constants.light[3] = environment.keyIntensity;
    // Fire as an internal volume light: the emitter position plus the emission gain. The
    // gain climbs with the explosion burst so the blast reads as a light event, not just a
    // density event.
    constants.fire[0] = particleEmitter[0];
    constants.fire[1] = particleEmitter[1];
    constants.fire[2] = particleEmitter[2];
    // Fire emission gain. The Planckian term is already scaled by (T/1600)^2.5, so a large
    // multiplier clips the whole plume to flat white and destroys the colour gradient, while
    // too small a value leaves only a smudge. 3.2 puts the core just past the clip point and
    // keeps the body in the orange band.
    constants.fire[3] = (particleFirePreset ? 4.2f : 0.6f) * (1.0f + particleBurst * 2.5f);
    std::memcpy(particleVolumeConstantsMapped, &constants, sizeof(constants));

    const unsigned base = detail::ParticleFrameBase + frameIndex * detail::ParticleFrameStride;
    transitionTo(volumeState[frameIndex][0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, list);

    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    sceneRtv.ptr += static_cast<SIZE_T>(detail::RtvSlotBase +
                                        frameIndex * detail::RtvSlotsPerFrame) *
                    rtvStride;

    // The temporal path needs screen-sized targets. If they cannot be created the draw
    // falls back to the direct full-resolution composite instead of dropping the effect.
    const bool temporal = particleTemporal && createVolumeTargets(particleError) &&
                          volumeTargetsReady;

    if (!temporal) {
        list->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
        list->SetPipelineState(particleVolumePipeline);
        list->SetGraphicsRootSignature(particleVolumeRootSignature);
        list->SetGraphicsRootDescriptorTable(1, gpuDescriptor(base + detail::PfsStateASrv));
        list->SetGraphicsRoot32BitConstants(0, 36, &constants, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        ++particleDraws;
        transitionTo(volumeState[frameIndex][0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, list);
        releaseFieldLease(detail::FieldChannel::Density);
        releaseFieldLease(detail::FieldChannel::Heat);
        return;
    }

    // --- 3b-1. half-resolution march -------------------------------------------------
    D3D12_CPU_DESCRIPTOR_HANDLE halfRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    halfRtv.ptr += static_cast<SIZE_T>(rtvStride) * detail::RtvVolumeHalf;
    list->OMSetRenderTargets(1, &halfRtv, FALSE, nullptr);
    D3D12_VIEWPORT halfViewport{};
    halfViewport.Width = static_cast<float>(volumeHalfWidth);
    halfViewport.Height = static_cast<float>(volumeHalfHeight);
    halfViewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &halfViewport);
    list->SetPipelineState(particleMarchPipeline);
    list->SetGraphicsRootSignature(particleVolumeRootSignature);
    list->SetGraphicsRootDescriptorTable(1, gpuDescriptor(base + detail::PfsStateASrv));
    list->SetGraphicsRoot32BitConstants(0, 36, &constants, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    ++particleDraws;

    // --- 3b-2. temporal accumulation -------------------------------------------------
    // The blend hard-resets when the camera moves (no motion vectors exist yet), when the
    // history is invalid, and softens during a burst so the explosion does not smear.
    float cameraDelta = 0.0f;
    for (int element = 0; element < 16; ++element) {
        const float difference = std::fabs(inverse.m[element] - prevInvViewProjection[element]);
        if (difference > cameraDelta) cameraDelta = difference;
    }
    const bool cameraMoved = !temporalHistoryValid || cameraDelta > 2e-4f;
    const float blend = cameraMoved ? 1.0f : (particleBurst > 0.02f ? 0.4f : 0.18f);
    temporalBlend = blend;

    transitionTo(volumeHalf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, list);
    AccumulateConstants accumulate{};
    accumulate.accum[0] = blend;
    accumulate.accum[1] = static_cast<float>(volumeHalfWidth);
    accumulate.accum[2] = static_cast<float>(volumeHalfHeight);
    std::memcpy(particleAccumulateConstantsMapped, &accumulate, sizeof(accumulate));
    list->SetPipelineState(particleAccumulatePipeline);
    list->SetComputeRootSignature(particleAccumulateRootSignature);
    list->SetComputeRoot32BitConstants(0, 4, &accumulate, 0);
    list->SetComputeRootDescriptorTable(
        1, gpuDescriptor(detail::ParticleGlobalBase + detail::PgsHalfSrv));
    list->SetComputeRootDescriptorTable(
        2, gpuDescriptor(detail::ParticleGlobalBase + detail::PgsHistoryUav));
    list->Dispatch((volumeHalfWidth + 7) / 8, (volumeHalfHeight + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = volumeHistory.resource;
        list->ResourceBarrier(1, &barrier);
    }
    transitionTo(volumeHistory, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, list);

    // --- 3b-3. depth-aware composite over the scene colour ---------------------------
    list->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);
    CompositeConstants composite{};
    composite.composite[0] = 0.1f;    // near plane, matches the host projection
    composite.composite[1] = 120.0f;  // far plane
    composite.composite[2] = static_cast<float>(width);
    composite.composite[3] = static_cast<float>(height);
    std::memcpy(particleCompositeConstantsMapped, &composite, sizeof(composite));
    list->SetPipelineState(particleCompositePipeline);
    list->SetGraphicsRootSignature(particleCompositeRootSignature);
    list->SetGraphicsRoot32BitConstants(0, 4, &composite, 0);
    list->SetGraphicsRootDescriptorTable(
        1, gpuDescriptor(detail::ParticleGlobalBase + detail::PgsHistorySrv));
    list->SetGraphicsRootDescriptorTable(2, gpuDescriptor(base + detail::PfsDepthSrv));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    // Restore the states the next frame expects: history as a UAV, the half target as an
    // RTV, and the canonical state volume as a UAV for the solver.
    transitionTo(volumeHistory, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, list);
    transitionTo(volumeHalf, D3D12_RESOURCE_STATE_RENDER_TARGET, list);
    transitionTo(volumeState[frameIndex][0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, list);

    std::memcpy(prevInvViewProjection, inverse.m, sizeof(prevInvViewProjection));
    temporalHistoryValid = true;
    ++temporalAccumFrames;

    releaseFieldLease(detail::FieldChannel::Density);
    releaseFieldLease(detail::FieldChannel::Heat);
}

std::string Renderer::Impl::particlesJson() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(0);
    const double voxel = particleVolumeExtent / static_cast<double>(detail::ParticleVolumeDim);
    out << "\"particles\":{\"requested\":" << (particlesRequested ? "true" : "false")
        << ",\"available\":" << (particlesReady ? "true" : "false")
        << ",\"implemented\":" << (particleFrames > 0 ? "true" : "false")
        << ",\"frames\":" << particleFrames << ",\"draws\":" << particleDraws
        << ",\"failures\":" << particleDispatchFailures
        << ",\"indirect\":{\"active\":" << (particleIndirectDispatches > 0 ? "true" : "false")
        << ",\"dispatches\":" << particleIndirectDispatches
        << ",\"countSource\":\"configured-capacity\",\"gpuWrittenArguments\":true"
        << ",\"validatedSamples\":" << particleReadbackSamples << ",\"argumentMismatches\":" << particleArgsMismatches << "}"
        << ",\"computeTiming\":{\"available\":" << (particleTimestampHeap ? "true" : "false")
        << ",\"samples\":" << particleTimingSamples << ",\"lastMs\":" << std::setprecision(4) << particleComputeMs
        << ",\"averageMs\":" << (particleTimingSamples ? particleComputeTotalMs / particleTimingSamples : 0.0)
        << ",\"scope\":\"simulate+splat+resolve+solver\",\"blockingReadback\":false}"
        << ",\"count\":" << particleCount << ",\"volumeDim\":" << detail::ParticleVolumeDim
        << ",\"voxelMetres\":" << std::setprecision(3) << voxel
        << ",\"volumeExtent\":" << std::setprecision(1) << particleVolumeExtent
        << ",\"splat\":\"integer-atomic\""
        << ",\"splatKernel\":\"trilinear2x2x2\""
        << ",\"shading\":\"blackbody+hg+multiscatter+fireglow\""
        << ",\"emissionModel\":\"planck-chroma+heat-radiance\""
        << ",\"macCormack\":" << (fluidPasses > 0 ? "true" : "false")
        << ",\"turbulence\":" << std::setprecision(2)
        << (particleFirePreset ? 1.6 : 0.55)
        << ",\"solver\":" << (fluidPasses > 0 ? "\"buoyancy+advection+vorticity+jacobi\"" : "\"none\"")
        << ",\"solverIterations\":" << fluidIterations
        << ",\"solverPasses\":" << fluidPasses
        << ",\"vorticity\":" << (fluidPasses > 0 ? "true" : "false")
        << ",\"firePreset\":" << (particleFirePreset ? "true" : "false")
        << ",\"explodeAt\":" << particleExplodeAt
        << ",\"burstPeak\":" << std::setprecision(3) << particleBurstPeak
        << ",\"colliders\":" << particleColliderCount
        << ",\"collidersRejected\":" << particleColliderRejected
        << ",\"temporal\":{\"requested\":" << (particleTemporal ? "true" : "false")
        << ",\"active\":" << (volumeTargetsReady ? "true" : "false")
        << ",\"accumulatedFrames\":" << temporalAccumFrames
        << ",\"blend\":" << std::setprecision(2) << temporalBlend
        << ",\"halfWidth\":" << volumeHalfWidth << ",\"halfHeight\":" << volumeHalfHeight << "}"
        << "," << fieldLeasesJson()
        << ",\"error\":" << detail::jsonString(particleError) << "},";
    return out.str();
}

void Renderer::setParticlesEnabled(bool enabled) { impl_->particlesRequested = enabled; }
bool Renderer::particlesEnabled() const { return impl_->particlesRequested; }
bool Renderer::particlesImplemented() const { return impl_->particleFrames > 0; }

void Renderer::setParticleEmitter(const float position[3]) {
    if (!position) return;
    impl_->particleEmitter[0] = position[0];
    impl_->particleEmitter[1] = position[1];
    impl_->particleEmitter[2] = position[2];
}

void Renderer::setFirePreset(bool enabled) {
    auto& state = *impl_;
    state.particleFirePreset = enabled;
    // The campfire runs hotter, tighter and more opaque than the loose smoke plume.
    state.particleExtinction = enabled ? 5.5f : 3.2f;
    state.particleAmbient = enabled ? 0.60f : 0.35f;
    state.particleBuoyancy = enabled ? 3.4f : 1.4f;
    state.particleSink = enabled ? 0.55f : 0.80f;
    // Resolution follows the effect: a fire is a ~6 m phenomenon, so the field shrinks with
    // it and the voxel drops from 16.7 cm to 6.25 cm at the same 96^3 grid.
    const float fireMinimum[3] = {-3.0f, 0.0f, -3.0f};
    const float smokeMinimum[3] = {-8.0f, 0.0f, -8.0f};
    setFieldBounds(enabled ? fireMinimum : smokeMinimum, enabled ? 6.0f : 16.0f);
}

void Renderer::setFieldBounds(const float minimum[3], float extent) {
    if (!minimum || !(extent > 0.5f)) return;
    auto& state = *impl_;
    for (unsigned axis = 0; axis < 3; ++axis) state.particleVolumeMin[axis] = minimum[axis];
    state.particleVolumeExtent = extent;
    // The accumulated history belongs to the previous field layout; reusing it would
    // ghost the old plume through the new bounds.
    state.temporalHistoryValid = false;
}

void Renderer::setParticleTemporal(bool enabled) { impl_->setParticleTemporal(enabled); }
bool Renderer::particleTemporalEnabled() const { return impl_->particleTemporalEnabled(); }

void Renderer::setExplosionFrame(unsigned frame) { impl_->particleExplodeAt = frame; }

float Renderer::particleBurst() const { return impl_->particleBurst; }

void Renderer::setFieldColliders(const std::vector<FieldCollider>& colliders) {
    auto& state = *impl_;
    static_assert(sizeof(FieldCollider) == 32, "collider stride must match the shader struct");
    state.fieldColliders = colliders;
    const size_t kept =
        colliders.size() < detail::ParticleColliderCapacity ? colliders.size()
                                                           : detail::ParticleColliderCapacity;
    state.particleColliderCount = static_cast<unsigned>(kept);
    state.particleColliderRejected = static_cast<unsigned>(colliders.size() - kept);
}

} // namespace nativegpu

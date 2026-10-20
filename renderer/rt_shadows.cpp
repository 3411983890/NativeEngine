// DXR static shadow pass: state object, shader binding table and per-frame dispatch.
//
// Design notes:
//  * One ray per screen pixel, traced from the depth-reconstructed surface toward the
//    fixed key light. Hard visibility only: no denoising, no multi-sample shadows.
//  * The acceleration structure is a single unit-cube BLAS instanced by the rigid
//    DrawItems of the current frame, so shadows follow the actual box transforms. The
//    TLAS is rebuilt per frame from rigid transforms; there is no skinning or vertex
//    animation in the acceleration structure.
//  * The pass is optional at every level: missing DXC, unsupported adapter, a failed
//    state object or a failed dispatch all leave the raster compositing path intact and
//    are reported with fallbackUsed=true.

#include "d3d12_renderer_internal.h"

#include "nativegpu_shaders.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace nativegpu {
namespace {

constexpr UINT kRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32 bytes
// Records are placed on 64-byte boundaries so every record and the table itself satisfy
// D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT.
constexpr UINT kSlotStride = 64;
constexpr UINT kRayGenOffset = 0;
constexpr UINT kMissOffset = kSlotStride;
constexpr UINT kHitGroupOffset = kSlotStride * 2;
constexpr UINT kSbtSize = kSlotStride * 3;

// The three numbers must match the raster pixel shader's key light direction.
void normalizedKeyLightDirection(float out[3]) {
    constexpr float source[3] = {-0.45f, 0.85f, -0.55f};
    const float length = std::sqrt(source[0] * source[0] + source[1] * source[1] +
                                   source[2] * source[2]);
    const float inverse = length > 1.0e-6f ? 1.0f / length : 1.0f;
    out[0] = source[0] * inverse;
    out[1] = source[1] * inverse;
    out[2] = source[2] * inverse;
}

} // namespace

void Renderer::Impl::releaseRaytracingPipeline() {
    if (rtShaderBindingTable && rtShaderBindingTableMapped) {
        D3D12_RANGE noWrite{0, 0};
        rtShaderBindingTable->Unmap(0, &noWrite);
        rtShaderBindingTableMapped = nullptr;
    }
    destroy(rtShaderBindingTable);
    destroy(rtProperties);
    destroy(rtStateObject);
    rtPipelineCreated = false;
    rtSbtCreated = false;
    rtShaderBindingTableSize = 0;
}

bool Renderer::Impl::buildRaytracingPipeline(std::string& error) {
    error.clear();
    if (rtPipelineCreated) return true;
    if (raytracingTier < 10) {
        error = "DXR tier is unsupported on this adapter";
        addDiagnostic("NG-E-RT-UNSUPPORTED", error);
        return false;
    }
    if (!device5) {
        error = "ID3D12Device5 is unavailable";
        return false;
    }
    if (!shaders::kDxcAvailable) {
        error = shaders::kDxcError[0] ? shaders::kDxcError
                                     : "dxcUnavailable: pinned DXC is not installed";
        return false;
    }
    if (!shaders::kRaytracingShadows.data || shaders::kRaytracingShadows.size < 64) {
        error = "DXR shader library blob is empty; the ray tracing library did not compile";
        return false;
    }

    // --- global root signature ------------------------------------------------
    if (!rtRootSignature) {
        D3D12_DESCRIPTOR_RANGE ranges[3]{};
        for (unsigned index = 0; index < 3; ++index) {
            ranges[index].RangeType = index == 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                                 : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors = 1;
            ranges[index].BaseShaderRegister = index == 2 ? 0u : index; // t0, t1, u0
            ranges[index].OffsetInDescriptorsFromTableStart = index;
        }
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = sizeof(detail::ShadowConstants) / sizeof(float);
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 3;
        parameters[1].DescriptorTable.pDescriptorRanges = ranges;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 2;
        description.pParameters = parameters;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* signature = nullptr;
        ID3DBlob* diagnostics = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &signature, &diagnostics);
        if (FAILED(hr)) {
            error = "Serialize DXR root signature failed (" + detail::hresultText(hr) + ")";
            if (diagnostics) {
                error += ": " + std::string(
                    static_cast<char*>(diagnostics->GetBufferPointer()),
                    diagnostics->GetBufferSize());
            }
            detail::release(diagnostics);
            detail::release(signature);
            return false;
        }
        detail::release(diagnostics);
        hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
                                         signature->GetBufferSize(),
                                         IID_PPV_ARGS(&rtRootSignature));
        detail::release(signature);
        if (FAILED(hr)) {
            error = "Create DXR root signature failed (" + detail::hresultText(hr) + ")";
            return false;
        }
        track(rtRootSignature);
    }

    // --- state object ---------------------------------------------------------
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary.pShaderBytecode = shaders::kRaytracingShadows.data;
    library.DXILLibrary.BytecodeLength = shaders::kRaytracingShadows.size;
    library.NumExports = 0;
    library.pExports = nullptr;

    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport = L"ShadowHitGroup";
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ShadowClosestHit";
    hitGroup.AnyHitShaderImport = nullptr;
    hitGroup.IntersectionShaderImport = nullptr;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = sizeof(float); // ShadowPayload { float visible; }
    shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{};
    globalRoot.pGlobalRootSignature = rtRootSignature;

    D3D12_STATE_SUBOBJECT subobjects[5]{};
    UINT count = 0;
    subobjects[count].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[count++].pDesc = &library;
    subobjects[count].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[count++].pDesc = &hitGroup;
    subobjects[count].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[count++].pDesc = &shaderConfig;
    subobjects[count].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[count++].pDesc = &pipelineConfig;
    subobjects[count].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[count++].pDesc = &globalRoot;

    D3D12_STATE_OBJECT_DESC stateObjectDescription{};
    stateObjectDescription.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDescription.NumSubobjects = count;
    stateObjectDescription.pSubobjects = subobjects;
    HRESULT hr = device5->CreateStateObject(&stateObjectDescription,
                                            IID_PPV_ARGS(&rtStateObject));
    if (FAILED(hr)) {
        error = "CreateStateObject (raytracing pipeline) failed (" + detail::hresultText(hr) + ")";
        addDiagnostic("NG-E-RT-PSO", error);
        detail::release(rtStateObject);
        return false;
    }
    track(rtStateObject);
    if (FAILED(rtStateObject->QueryInterface(IID_PPV_ARGS(&rtProperties))) || !rtProperties) {
        error = "ID3D12StateObjectProperties is unavailable";
        releaseRaytracingPipeline();
        return false;
    }
    track(rtProperties);
    rtPipelineCreated = true;

    // --- shader binding table -------------------------------------------------
    const void* rayGenIdentifier = rtProperties->GetShaderIdentifier(L"ShadowRayGen");
    const void* missIdentifier = rtProperties->GetShaderIdentifier(L"ShadowMiss");
    const void* hitGroupIdentifier = rtProperties->GetShaderIdentifier(L"ShadowHitGroup");
    if (!rayGenIdentifier || !missIdentifier || !hitGroupIdentifier) {
        error = "GetShaderIdentifier failed for the shadow shader exports";
        addDiagnostic("NG-E-RT-SBT", error);
        releaseRaytracingPipeline();
        return false;
    }
    if (!createBuffer(kSbtSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &rtShaderBindingTable, error)) {
        releaseRaytracingPipeline();
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    if (FAILED(rtShaderBindingTable->Map(0, &noRead, &rtShaderBindingTableMapped)) ||
        !rtShaderBindingTableMapped) {
        error = "Map shader binding table failed";
        releaseRaytracingPipeline();
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(rtShaderBindingTableMapped);
    std::memset(bytes, 0, kSbtSize);
    std::memcpy(bytes + kRayGenOffset, rayGenIdentifier, kRecordSize);
    std::memcpy(bytes + kMissOffset, missIdentifier, kRecordSize);
    std::memcpy(bytes + kHitGroupOffset, hitGroupIdentifier, kRecordSize);
    D3D12_RANGE written{0, kSbtSize};
    rtShaderBindingTable->Unmap(0, &written);
    rtShaderBindingTableMapped = nullptr;
    rtShaderBindingTableSize = kSbtSize;
    rtSbtCreated = true;
    rtError.clear();
    return true;
}

bool Renderer::Impl::dispatchShadowRays(ID3D12GraphicsCommandList4* list, unsigned frameIndex,
                                        const Mat4& viewProjection,
                                        const float cameraPosition[3], std::string& error) {
    error.clear();
    if (!rtPipelineCreated || !rtSbtCreated || !rtShaderBindingTable || !rtStateObject) {
        error = "DXR shadow pipeline is not ready";
        addDiagnostic("NG-E-RT-DISPATCH", error);
        return false;
    }
    if (!list) {
        error = "DXR command list interface is unavailable";
        return false;
    }
    detail::ShadowConstants constants{};
    if (!invertMatrix(viewProjection, constants.invViewProjection)) {
        error = "View-projection matrix is singular; shadow rays cannot be reconstructed";
        return false;
    }
    constants.cameraPosition[0] = cameraPosition[0];
    constants.cameraPosition[1] = cameraPosition[1];
    constants.cameraPosition[2] = cameraPosition[2];
    constants.cameraPosition[3] = 1.0f;
    float direction[3];
    normalizedKeyLightDirection(direction);
    constants.keyLightDirection[0] = direction[0];
    constants.keyLightDirection[1] = direction[1];
    constants.keyLightDirection[2] = direction[2];
    // Origin bias in world units: large enough to avoid self-intersection on a unit box,
    // small enough that contact shadows are not visibly detached.
    constants.keyLightDirection[3] = 0.02f;
    constants.traceParams[0] = static_cast<float>(width);
    constants.traceParams[1] = static_cast<float>(height);
    constants.traceParams[2] = 200.0f;
    constants.traceParams[3] = 1.0f;
    constants.depthParams[0] = 0.1f;
    constants.depthParams[1] = 120.0f;
    constants.depthParams[2] = width ? 1.0f / static_cast<float>(width) : 0.0f;
    constants.depthParams[3] = height ? 1.0f / static_cast<float>(height) : 0.0f;

    const unsigned descriptorBase =
        detail::FrameDescriptorBase + frameIndex * detail::FrameDescriptorStride;
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rtRootSignature);
    list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(float), &constants, 0);
    list->SetComputeRootDescriptorTable(1, gpuDescriptor(descriptorBase + detail::FdsDepth));
    list->SetPipelineState1(rtStateObject);

    const D3D12_GPU_VIRTUAL_ADDRESS tableAddress = rtShaderBindingTable->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord.StartAddress = tableAddress + kRayGenOffset;
    dispatch.RayGenerationShaderRecord.SizeInBytes = kRecordSize;
    dispatch.MissShaderTable.StartAddress = tableAddress + kMissOffset;
    dispatch.MissShaderTable.SizeInBytes = kRecordSize;
    dispatch.MissShaderTable.StrideInBytes = kRecordSize;
    dispatch.HitGroupTable.StartAddress = tableAddress + kHitGroupOffset;
    dispatch.HitGroupTable.SizeInBytes = kRecordSize;
    dispatch.HitGroupTable.StrideInBytes = kRecordSize;
    dispatch.CallableShaderTable.StartAddress = 0;
    dispatch.CallableShaderTable.SizeInBytes = 0;
    dispatch.CallableShaderTable.StrideInBytes = 0;
    dispatch.Width = width;
    dispatch.Height = height;
    dispatch.Depth = 1;
    list->DispatchRays(&dispatch);

    // The composite pass reads the mask as an SRV; the UAV barrier plus the state
    // transition below make the write visible to it.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = shadowMask[frameIndex].resource;
    list->ResourceBarrier(1, &barrier);
    return true;
}

std::string Renderer::Impl::rtShadowsJson() const {
    const bool effectImplemented = rtPipelineCreated && rtSbtCreated && rtDispatchedFrames > 0;
    std::ostringstream out;
    out << "{"
        << "\"probeAvailable\":" << (raytracingTier >= 10 ? "true" : "false")
        << ",\"dxcAvailable\":" << (shaders::kDxcAvailable ? "true" : "false")
        << ",\"dxcVersion\":" << detail::jsonString(shaders::kDxcVersion)
        << ",\"requested\":" << (rtRequested ? "true" : "false")
        << ",\"enabled\":" << ((rtRequested && rtPipelineCreated && rtSbtCreated) ? "true" : "false")
        << ",\"rtpsoCreated\":" << (rtPipelineCreated ? "true" : "false")
        << ",\"sbtCreated\":" << (rtSbtCreated ? "true" : "false")
        << ",\"effectImplemented\":" << (effectImplemented ? "true" : "false")
        << ",\"fallbackUsed\":" << (rtDispatchedLastFrame ? "false" : "true")
        << ",\"shadowTextureBytes\":" << rtShadowTextureBytes
        << ",\"dispatchedFrames\":" << rtDispatchedFrames
        << ",\"fallbackFrames\":" << rtFallbackFrames
        << ",\"dispatchFailures\":" << rtDispatchFailures
        << ",\"gpuRtMs\":" << std::fixed << std::setprecision(4) << rtLastGpuMs
        << ",\"tlasInstances\":" << tlasInstanceCount
        << ",\"tlasRebuilds\":" << tlasRebuilds
        << ",\"sbtBytes\":" << rtShaderBindingTableSize
        << ",\"error\":" << detail::jsonString(rtError)
        << ",\"mode\":\"one-ray-per-pixel-hard-visibility\""
        << ",\"tlasPolicy\":\"per-frame-rigid\""
        << "}";
    return out.str();
}

} // namespace nativegpu



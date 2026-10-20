// Compute queue and the B1/B3 probe dispatch.
//
// B1: the renderer owns a second (COMPUTE) queue with its own allocator, list and fence,
//     and the graphics queue waits on that fence before its next submission.
// B3: the probe shader samples four descriptors twice, once through a descriptor table and
//     once through SM 6.6 ResourceDescriptorHeap at indices above the bounded 8-slot
//     material table. The results are read back and compared: a match proves real heap
//     indexing, and the indices used can never be addressed by the bounded table.
#include "d3d12_renderer_internal.h"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace nativegpu {
namespace {

// Root constants: 4 header words + 4 lights * (colour+intensity, position+range).
constexpr unsigned ComputeConstantWords = 4 + 4 * 8;

struct ComputeConstants {
    unsigned heapBaseIndex = 0;
    unsigned lightCount = 0;
    unsigned corruptMode = 0;
    unsigned padding = 0;
    float lightColorIntensity[4][4]{};
    float lightPositionRange[4][4]{};
};
static_assert(sizeof(ComputeConstants) == ComputeConstantWords * sizeof(unsigned),
              "compute constant layout must match the HLSL cbuffer");

// The renderer keeps its root-signature helper private to d3d12_renderer.cpp, so this
// translation unit serializes its own signature with the same version and error style.
bool serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& description,
                            ID3D12Device* device, ID3D12RootSignature** output,
                            std::string& error) {
    ID3DBlob* signature = nullptr;
    ID3DBlob* diagnostics = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &diagnostics);
    if (FAILED(hr)) {
        error = "Serialize compute root signature failed (" + detail::hresultText(hr) + ")";
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
    return detail::checked(hr, "Create compute root signature", error);
}

} // namespace

bool Renderer::Impl::createComputePipeline(std::string& error) {
    if (computePipelineReady) return true;
    if (!shaders::kDxcAvailable) {
        computeError = shaders::kDxcError[0] ? shaders::kDxcError : "dxcUnavailable";
        return true;  // Optional feature: the renderer stays usable without DXC.
    }
    if (shaders::kComputeProbe.size < 64) {
        computeError = "computeProbeBlobMissing";
        return true;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = detail::VerificationSrvCount;
    ranges[0].BaseShaderRegister = 0;  // t0..t3
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;  // u0
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = ComputeConstantWords;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = parameters;
    // SM 6.6 heap indexing requires the signature to declare that the heap is directly
    // indexed; without this flag the compute PSO is rejected with E_INVALIDARG.
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    if (!serializeRootSignature(description, device, &computeRootSignature, error)) {
        return false;
    }
    track(computeRootSignature);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = computeRootSignature;
    pipeline.CS.pShaderBytecode = shaders::kComputeProbe.data;
    pipeline.CS.BytecodeLength = shaders::kComputeProbe.size;
    pipeline.NodeMask = 0;
    pipeline.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (!detail::checked(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&computePipeline)),
                         "Create compute pipeline", error)) {
        return false;
    }
    track(computePipeline);

    if (!createBuffer(ComputeConstantWords * sizeof(unsigned), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &computeConstants, error)) {
        return false;
    }
    if (!detail::checked(computeConstants->Map(0, nullptr, reinterpret_cast<void**>(&computeConstantsMapped)),
                 "Map compute constants", error)) {
        return false;
    }
    // 16 float4: one lighting row, then four table samples and four heap samples.
    if (!createBuffer(16 * sizeof(float) * 4, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &computeResult, error)) {
        return false;
    }
    if (!createBuffer(16 * sizeof(float) * 4, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST, &computeReadback, error)) {
        return false;
    }

    // The UAV view lives in the existing shader-visible heap, so no extra heap is needed.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = 16;
    uav.Buffer.StructureByteStride = sizeof(float) * 4;
    uav.Buffer.CounterOffsetInBytes = 0;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(computeResult, nullptr, &uav,
                                     cpuDescriptor(detail::ComputeResultUavSlot));

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!detail::checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&computeQueue)),
                 "Create compute queue", error)) {
        return false;
    }
    track(computeQueue);
    if (!detail::checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                IID_PPV_ARGS(&computeAllocator)),
                 "Create compute allocator", error)) {
        return false;
    }
    track(computeAllocator);
    if (!detail::checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAllocator,
                                           nullptr, IID_PPV_ARGS(&computeCommands)),
                 "Create compute list", error)) {
        return false;
    }
    track(computeCommands);
    computeCommands->Close();
    if (!detail::checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&computeFence)),
                 "Create compute fence", error)) {
        return false;
    }
    track(computeFence);

    computeQueueCreated = true;
    computePipelineReady = true;
    computeHeapBaseIndex = detail::VerificationSrvBase;
    computeError.clear();
    return true;
}

void Renderer::Impl::releaseComputePipeline() {
    if (computeConstantsMapped && computeConstants) {
        computeConstants->Unmap(0, nullptr);
        computeConstantsMapped = nullptr;
    }
    destroy(computeFence);
    destroy(computeCommands);
    destroy(computeAllocator);
    destroy(computeQueue);
    destroy(computePipeline);
    destroy(computeRootSignature);
    destroy(computeResult);
    destroy(computeReadback);
    destroy(computeConstants);
    computeQueueCreated = false;
    computePipelineReady = false;
}

// Dispatches the probe once per frame when requested, then makes the graphics queue wait
// on the compute fence so the two submissions cannot overlap unsafely.
bool Renderer::Impl::dispatchComputeProbe(const std::vector<LightDesc>& lights,
                                          std::string& error) {
    if (!computeRequested) return true;
    if (!computePipelineReady) return true;

    computeLastLights = lights;
    ComputeConstants constants{};
    constants.heapBaseIndex = computeHeapBaseIndex;
    constants.lightCount = static_cast<unsigned>(std::min<size_t>(4, lights.size()));
    constants.corruptMode = computeCorrupt ? 1u : 0u;
    for (unsigned index = 0; index < constants.lightCount; ++index) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            constants.lightColorIntensity[index][channel] = lights[index].color[channel];
        }
        constants.lightColorIntensity[index][3] = lights[index].intensity;
        for (unsigned axis = 0; axis < 3; ++axis) {
            constants.lightPositionRange[index][axis] = lights[index].position[axis];
        }
        constants.lightPositionRange[index][3] = lights[index].range;
    }
    std::memcpy(computeConstantsMapped, &constants, sizeof(constants));

    if (!detail::checked(computeAllocator->Reset(), "Reset compute allocator", error)) return false;
    if (!detail::checked(computeCommands->Reset(computeAllocator, nullptr), "Reset compute list", error)) {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    computeCommands->SetDescriptorHeaps(1, heaps);
    computeCommands->SetComputeRootSignature(computeRootSignature);
    computeCommands->SetComputeRoot32BitConstants(0, ComputeConstantWords, &constants, 0);
    computeCommands->SetComputeRootDescriptorTable(
        1, gpuDescriptor(detail::VerificationSrvBase));
    computeCommands->SetComputeRootDescriptorTable(
        2, gpuDescriptor(detail::ComputeResultUavSlot));
    computeCommands->SetPipelineState(computePipeline);
    computeCommands->Dispatch(1, 1, 1);
    if (!detail::checked(computeCommands->Close(), "Close compute list", error)) return false;

    computeQueue->ExecuteCommandLists(1,
        reinterpret_cast<ID3D12CommandList* const*>(&computeCommands));
    ++computeFenceValue;
    if (!detail::checked(computeQueue->Signal(computeFence, computeFenceValue),
                 "Signal compute fence", error)) {
        return false;
    }
    // The graphics queue consumes the buffer next frame; wait here so ordering is explicit
    // rather than accidental.
    if (!detail::checked(queue->Wait(computeFence, computeFenceValue), "Graphics waits for compute",
                 error)) {
        return false;
    }
    ++computeCrossQueueWaits;
    ++computeDispatchedFrames;
    return true;
}

// One-shot verification: run the probe, drain both queues, copy the UAV into a readback
// buffer and compare the two sampling paths on the CPU.
bool Renderer::Impl::readbackComputeProbe(std::string& error) {
    if (!computePipelineReady || computeProbeRan) return true;
    computeProbeRan = true;

    // A queue-side Wait() only orders GPU work; it does not block the CPU. The allocator
    // and command list below are reused, so the CPU must observe the fence completion
    // first. Without this the copy can run while the dispatch is still executing and read
    // the previous contents (observed as a zero light scale in the full verify run).
    if (computeFence->GetCompletedValue() < computeFenceValue) {
        HANDLE completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completion) {
            error = "NG-E-COMPUTE-SYNC: cannot create completion event";
            return false;
        }
        computeFence->SetEventOnCompletion(computeFenceValue, completion);
        const DWORD wait = WaitForSingleObject(completion, 5000);
        CloseHandle(completion);
        if (wait != WAIT_OBJECT_0) {
            error = "NG-E-COMPUTE-SYNC: compute fence did not complete";
            return false;
        }
    }
    if (!detail::checked(computeAllocator->Reset(), "Reset compute allocator", error)) return false;
    if (!detail::checked(computeCommands->Reset(computeAllocator, nullptr), "Reset compute list", error)) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = computeResult;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    computeCommands->ResourceBarrier(1, &barrier);
    // A transition alone does not guarantee that the UAV writes from the dispatch are
    // visible to the copy; a UAV barrier is required for that ordering.
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = computeResult;
    computeCommands->ResourceBarrier(1, &uavBarrier);
    computeCommands->CopyResource(computeReadback, computeResult);
    if (!detail::checked(computeCommands->Close(), "Close readback list", error)) return false;
    computeQueue->ExecuteCommandLists(1,
        reinterpret_cast<ID3D12CommandList* const*>(&computeCommands));
    ++computeFenceValue;
    if (!detail::checked(computeQueue->Signal(computeFence, computeFenceValue), "Signal readback",
                 error)) {
        return false;
    }
    if (!detail::checked(computeQueue->Wait(computeFence, computeFenceValue), "Drain readback", error)) {
        return false;
    }

    void* mapped = nullptr;
    if (!detail::checked(computeReadback->Map(0, nullptr, &mapped), "Map compute readback", error)) {
        return false;
    }
    const float* values = static_cast<const float*>(mapped);
    computeLightScaleGpu = values[0];
    computeLightCountGpu = static_cast<unsigned>(values[15 * 4]);
    // Table samples start at float4 index 1, heap samples at 5.
    float maxDelta = 0.0f;
    unsigned matched = 0;
    for (unsigned sample = 0; sample < detail::VerificationSrvCount; ++sample) {
        float delta = 0.0f;
        for (unsigned channel = 0; channel < 4; ++channel) {
            const float tableValue = values[4 + sample * 4 + channel];
            const float heapValue = values[4 + (4 + sample) * 4 + channel];
            delta = std::max(delta, std::fabs(tableValue - heapValue));
        }
        computeHeapMaxDelta = std::max(computeHeapMaxDelta, delta);
        if (delta == 0.0f) ++matched;
    }
    computeHeapPairsMatched = matched;
    computeHeapVerified = matched == detail::VerificationSrvCount;
    computeMaxIndexTested = computeHeapBaseIndex + detail::VerificationSrvCount - 1;

    // The CPU reference for the lighting reduction, using the same order as the shader.
    float sum[3] = {0.0f, 0.0f, 0.0f};
    const unsigned lightCount = static_cast<unsigned>(std::min<size_t>(4, computeLastLights.size()));
    for (unsigned index = 0; index < lightCount; ++index) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            sum[channel] += computeLastLights[index].color[channel] * computeLastLights[index].intensity;
        }
    }
    const float average = (sum[0] + sum[1] + sum[2]) / 3.0f;
    computeLightScaleCpu = std::min(1.0f, std::max(0.0f, average));

    computeReadback->Unmap(0, nullptr);
    return true;
}

std::string Renderer::Impl::computeProbesFragment() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "\"compute\":{\"available\":"
        << (computeQueueCreated ? "true" : "false")
        << ",\"implemented\":" << (computeDispatchedFrames > 0 ? "true" : "false")
        << ",\"requested\":" << (computeRequested ? "true" : "false")
        << ",\"queue\":\"compute\""
        << ",\"queueCreated\":" << (computeQueueCreated ? "true" : "false")
        << ",\"crossQueueWaits\":" << computeCrossQueueWaits
        << ",\"dispatchedFrames\":" << computeDispatchedFrames
        << ",\"probeRan\":" << (computeProbeRan ? "true" : "false")
        << ",\"heapIndexing\":{\"baseIndex\":" << computeHeapBaseIndex
        << ",\"maxIndexTested\":" << computeMaxIndexTested
        << ",\"pairsCompared\":" << detail::VerificationSrvCount
        << ",\"pairsMatched\":" << computeHeapPairsMatched
        << ",\"maxDelta\":" << computeHeapMaxDelta
        << ",\"verified\":" << (computeHeapVerified ? "true" : "false") << "}"
        << ",\"lightScale\":{\"gpu\":" << computeLightScaleGpu
        << ",\"cpu\":" << computeLightScaleCpu
        << ",\"delta\":" << std::fabs(computeLightScaleGpu - computeLightScaleCpu) << "}"
        << ",\"lightCountGpu\":" << computeLightCountGpu
        << ",\"corrupt\":" << (computeCorrupt ? "true" : "false")
        << ",\"error\":" << detail::jsonString(computeError) << "},";
    return out.str();
}

void Renderer::setComputeEnabled(bool enabled) { impl_->computeRequested = enabled; }
void Renderer::setComputeCorrupt(bool corrupt) { impl_->computeCorrupt = corrupt; }
bool Renderer::computeProbeVerified() const { return impl_->computeHeapVerified; }

} // namespace nativegpu

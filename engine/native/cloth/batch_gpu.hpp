#pragma once
#include "solver_gpu.hpp"

namespace engine::cloth {
struct GpuBatchLimits {std::uint32_t maxInstances=32,maxInFlight=32;std::uint64_t maxAllocationBytes=4*1024*1024;};
struct GpuBatchStep {GpuSolver* solver=nullptr;float dt=1.f/120;std::optional<fields::Tick> fieldTick;};
struct GpuBatchStats {
    std::uint64_t metadataAllocationBytes=0,descriptorEstimateBytes=0,accountedBytes=0;
    std::uint64_t calls=0,dispatches=0,batchedSteps=0,fallbackSteps=0,pausedSteps=0,lastFence=0;
    std::uint32_t descriptorSlots=0,activeSlots=0,heldResourceReferences=0;
};
// One group per bounded cloth. Metadata/descriptors are immutable until the
// actual queue fence completes. No wait or readback; single recording thread.
class GpuBatch {
public:
    GpuBatch(ID3D12Device* device,ID3D12Fence* timeline,GpuBatchLimits limits={});
    ~GpuBatch();GpuBatch(const GpuBatch&)=delete;
    bool step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,const std::vector<GpuBatchStep>& steps,std::string& error);
    void collect();GpuBatchStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}

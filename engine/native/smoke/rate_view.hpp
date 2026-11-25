#pragma once
#include "solver.hpp"
#include <d3d12.h>
#include <memory>

namespace engine::smoke {
struct GpuRateBindings {
    ID3D12Resource* rates=nullptr;ID3D12Resource* diagnostics=nullptr;
    Spec grid;Layout layout;std::uint64_t sourceTick=0;
    std::array<std::uint32_t,3> unitsPerSource{1000000,1000,1000000};
};
class GpuParticleSmokeSource;
// Immutable source-rate field, not integrated smoke state. All readers and
// producers use one actual device/queue/fence. No CPU value readback.
class GpuSmokeRateView {
public:
    ~GpuSmokeRateView();GpuSmokeRateView(const GpuSmokeRateView&)=delete;
    GpuRateBindings bindings() const;
    ID3D12Device* device() const;ID3D12Fence* timeline() const;
    bool markUsed(std::uint64_t fence);std::uint64_t lastFence() const;
private:
    GpuSmokeRateView();struct Impl;std::unique_ptr<Impl> impl_;
    friend class GpuParticleSmokeSource;
};
}

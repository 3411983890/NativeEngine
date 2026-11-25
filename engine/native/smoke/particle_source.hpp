#pragma once
#include "rate_view.hpp"
#include "../effects/gpu_pool.hpp"
#include <string>
namespace engine::smoke {
struct ParticleSourceQuantization {
    // Density/fuel: quanta per kg/s per alive particle. Temperature: quanta
    // per K*m^3/s per particle (artist heat-volume rate, not enthalpy).
    std::array<std::uint32_t,3> unitsPerSource{1000000,1000,1000000};
};
struct ParticleSourceBatch {effects::GpuParticlePool* pool=nullptr;std::array<std::uint32_t,3> quantum{};};
struct ParticleSourceLimits {std::uint64_t maxAllocationBytes=32ull*1024*1024;unsigned maxFrames=8,maxBatches=32,maxParticlesPerBatch=1000000;};
struct ParticleSourceStats {std::uint64_t allocationBytes=0,frames=0,dispatches=0,batches=0,lastTick=0,lastFence=0,rejections=0;unsigned views=0,heldInputReferences=0;bool failed=false;};
std::uint64_t particleSourceFrameBytes(ID3D12Device*,const Spec&);
class GpuParticleSmokeSource {
public:
    GpuParticleSmokeSource(ID3D12Device*,ID3D12Fence*,Spec,ParticleSourceQuantization={},ParticleSourceLimits={});
    ~GpuParticleSmokeSource();GpuParticleSmokeSource(const GpuParticleSmokeSource&)=delete;
    // Full per-tick source set. Empty produces a known zero rate field.
    // Reject duplicate pools/ticks and uint32/rate/budget overcommit before work.
    // Record after all pool spawns, on the solver's same queue and fence timeline.
    // Retain the producer until shutdown succeeds; drain even a failed recorder.
    std::shared_ptr<GpuSmokeRateView> produce(const std::vector<ParticleSourceBatch>&,ID3D12GraphicsCommandList*,std::uint64_t fence,std::uint64_t tick,std::string& error);
    void collect();ParticleSourceStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}

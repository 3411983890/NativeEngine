#pragma once
#include "gpu.hpp"
#include "particle_source.hpp"
#include "../effects/gpu_runtime.hpp"
#include "../fields/gpu_local.hpp"
namespace engine::smoke {
struct ServiceLimits {
    unsigned maxInstances=8,maxDispatchesPerTick=16384;
    std::uint64_t maxAllocationBytes=128ull*1024*1024;
    GpuLimits solver;ParticleSourceLimits sources;
};
struct ServiceStats {
    std::uint64_t frames=0,flushes=0,solverSteps=0,tailSteps=0,skippedSteps=0,admissionRejected=0;
    std::uint64_t allocationBytes=0,reservedBytes=0,lastTick=0,lastFence=0;
    unsigned instances=0,tails=0,retiring=0,reservations=0;bool failed=false;
};
struct ServiceOutput {effects::Handle instance;std::uint64_t serial=0;std::string asset;GpuBuffers buffers;bool paused=false,tail=false;std::uint64_t skippedSteps=0,lastAttemptTick=0;Optics optics;};
// One GPU runtime, one actual queue/fence. Spawn admission reserves the full
// solver + immutable-input peak before any particle pool is constructed.
class GpuSmokeService {
public:
    GpuSmokeService(ID3D12Device*,ID3D12Fence*,ServiceLimits={});
    ~GpuSmokeService();GpuSmokeService(const GpuSmokeService&)=delete;
    effects::GpuSmokeHooks hooks();
    effects::GpuDepositAdmissionResult admit(const effects::GpuDepositSpawnRequest&);
    bool record(const effects::GpuSmokeFrame&,std::string& error);
    std::vector<ServiceOutput> outputs() const;
    fields::GpuScalarInput densityInput(std::uint64_t serial) const;
    bool prepareRead(std::uint64_t serial,ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    void collect();ServiceStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::shared_ptr<Impl> impl_;
};
}

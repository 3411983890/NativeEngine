#pragma once
#include "solver.hpp"
#include "rate_view.hpp"
#include <d3d12.h>
#include <memory>
#include <string>

namespace engine::smoke {
struct GpuLimits {std::uint64_t maxAllocationBytes=64ull*1024*1024;unsigned maxInputSnapshots=8,maxDispatchesPerStep=4096;};
struct GpuStats {std::uint64_t allocationBytes=0,dispatches=0,ticks=0,lastTick=0,lastFence=0;unsigned inputSnapshots=0,particleRateSnapshots=0;bool initialized=false,failed=false;};
struct GpuBuffers {ID3D12Resource* scalars=nullptr;ID3D12Resource* velocity=nullptr;ID3D12Resource* pressure=nullptr;ID3D12Resource* diagnostics=nullptr;Layout layout;Spec spec;std::uint64_t sourceTick=0,version=0;};
struct GpuFootprint {std::uint64_t fieldBytes=0,inputSnapshotBytes=0;};
GpuFootprint gpuFootprint(ID3D12Device*,const Spec&);
// One visual LEF solver on one actual device/queue/fence. Dense inputs are
// immutable upload snapshots. No field/state readback or CPU World mutation.
class GpuSmokeFireSolver {
public:
    GpuSmokeFireSolver(ID3D12Device*,ID3D12Fence*,Spec,GpuLimits={});
    ~GpuSmokeFireSolver();GpuSmokeFireSolver(const GpuSmokeFireSolver&)=delete;
    // New sources/obstacles apply on the next step. Initial state is accepted
    // only before first recording; rebuild/reset policy is explicit to caller.
    bool setInputs(const std::vector<CellInput>&,std::string& error,const State* initial=nullptr);
    // Optional same-tick immutable GPU source rates; additive to CellInput rates.
    bool step(ID3D12GraphicsCommandList*,std::uint64_t fence,std::uint64_t tick,float dt,std::string& error,std::shared_ptr<GpuSmokeRateView> particleRates={});
    GpuBuffers buffers() const;
    ID3D12Fence* timeline() const;
    bool prepareRead(ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    void collect();GpuStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include "runtime.hpp"
#include "../fields/gpu_vector.hpp"
#include <optional>

namespace engine::effects {
// Byte offsets match the current seven-attribute cooked sprite SoA layout.
// Every attribute allocation is aligned to 16 bytes, including small capacities.
struct GpuParticleLayout {
    std::uint32_t position=0,velocity=0,age=0,lifetime=0,spawnId=0,color=0,size=0,bytes=0;
    static GpuParticleLayout make(std::uint32_t capacity);
};
struct GpuPoolLimits {std::uint32_t maxParticles=1000000;std::uint64_t maxAllocationBytes=128*1024*1024;};
struct GpuPoolBuffers {
    ID3D12Resource* attributes=nullptr;
    ID3D12Resource* alive=nullptr;
    ID3D12Resource* dead=nullptr;
    ID3D12Resource* counters=nullptr;
    ID3D12Resource* dispatchArguments=nullptr;
    ID3D12Resource* drawArguments=nullptr;
    GpuParticleLayout layout;
    std::uint32_t capacity=0;
    Vec3 origin{};
    bool localSpace=false;
    D3D12_RESOURCE_STATES dispatchState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
// Counter ABI (uint32): alive-in, alive-out, free, accepted-spawn,
// dead-base, emitted, expired, dropped, faults. Cumulative counters saturate.
constexpr std::uint32_t GpuPoolCounterWords=16;
struct GpuPoolStats {
    std::uint64_t allocationBytes=0,recordedTicks=0,simulationIndirects=0,spawnIndirects=0,lastFence=0;
    bool initialized=false,drawReady=false;
};
// Shared GPU-only ledger. Words: alive, remaining-spawns, emitted, expired,
// dropped, faults, killed, reserved. Counters are visual telemetry, not authority.
// All users must record commands on the same queue and fence timeline.
class GpuParticleBudget {
public:
    GpuParticleBudget(ID3D12Device* device,std::uint32_t capacity,std::uint32_t maxSpawnPerTick);
    ~GpuParticleBudget();
    GpuParticleBudget(const GpuParticleBudget&)=delete;
    GpuParticleBudget& operator=(const GpuParticleBudget&)=delete;
    ID3D12Resource* buffer() const;
    std::uint64_t allocationBytes() const;
    std::uint64_t lastFence() const;
    std::uint32_t capacity() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class GpuParticlePool;
};
// One emitter's visual GPU pool. Caller owns the device/command list and one
// fence timeline. No queue waits or CPU readbacks, including for alive counts.
// Emission requests are CPU integer tick decisions; GPU capacity decides the
// accepted count, builds ExecuteIndirect arguments and recycles all dead slots.
// The caller keeps this object alive through its last recorded fence.
class GpuParticlePool {
public:
    GpuParticlePool(ID3D12Device* device,Emitter emitter,std::uint32_t capacity,GpuPoolLimits limits={},
                    std::shared_ptr<GpuParticleBudget> instanceBudget={},std::shared_ptr<GpuParticleBudget> globalBudget={},
                    std::shared_ptr<fields::GpuVectorView> field={},bool externalFieldDeposits=false);
    ~GpuParticlePool();
    GpuParticlePool(const GpuParticlePool&)=delete;
    GpuParticlePool& operator=(const GpuParticlePool&)=delete;
    bool initialize(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    // Reset each shared ledger once per tick, before any simulation/spawn. An
    // uninitialized ledger is initialized once even when reset is false.
    bool prepareBudgets(ID3D12GraphicsCommandList* commands,std::uint64_t fence,
                        bool resetInstance,bool resetGlobal,std::string& error);
    // Managers simulate every pool first, then spawn in stable authored order.
    // A successful simulate must be followed by spawn before clear/draw/step.
    bool simulate(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t age,Vec3 origin,std::string& error,
                  std::optional<fields::Tick> fieldTick={});
    bool spawn(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint32_t requested,
               std::uint64_t sequence,std::uint64_t emitterSeed,std::string& error);
    // sequence is the first requested spawn ordinal, not the accepted count.
    // CPU derives emitterSeed from asset/instance/emitter identity. A dropped
    // request therefore never shifts future random values.
    bool step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t age,
              std::uint32_t requested,std::uint64_t sequence,std::uint64_t emitterSeed,
              Vec3 origin,std::string& error,std::optional<fields::Tick> fieldTick={});
    bool bindField(std::shared_ptr<fields::GpuVectorView> field,std::string& error);
    bool move(Vec3 origin); // local particles follow; world-space particles stay fixed
    bool clear(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    // Transitions attributes/list to SRV and GPU-generated draw args to INDIRECT.
    // Reading/drawing must be on this command queue; the next step restores UAVs.
    bool prepareDraw(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    GpuPoolBuffers buffers() const;
    GpuPoolStats stats() const;
    bool shutdown(std::uint64_t completedFence,std::string& error);
    // Explicit delegation: the caller must execute FieldDeposit modules after
    // spawn on the same queue. The pool never silently enables writes itself.
    static std::string unsupported(const Emitter& emitter,const fields::GpuVectorView* field=nullptr,bool externalFieldDeposits=false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

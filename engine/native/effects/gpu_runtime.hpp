#pragma once
#include "gpu_pool.hpp"
#include <functional>

namespace engine::effects {
struct GpuRuntimeLimits {
    std::uint32_t maxParticles=100000,maxInstances=128,maxSpawnPerTick=100000;
    std::uint64_t maxAllocationBytes=256ull*1024*1024;
};
struct GpuBudgetCounters {
    std::uint32_t alive=0,remainingSpawns=0,emitted=0,expired=0,dropped=0,faults=0,killed=0,reserved=0;
};
struct GpuRuntimeStats {
    std::uint64_t tick=0,allocationBytes=0,lastFence=0,telemetryFence=0,skippedReadbacks=0;
    std::size_t instances=0,retiredInstances=0,pendingReadbacks=0;
    GpuBudgetCounters visual;
    bool telemetryValid=false;
    std::uint64_t fieldDepositCalls=0,fieldDepositFlushCalls=0;
    bool fieldDepositFailed=false;
    std::uint64_t fieldAdmissionRejected=0,fieldMoveRejected=0;
    std::string fieldAdmissionError;
    std::uint64_t smokeCalls=0,smokeFlushCalls=0,smokeAdmissionRejected=0,smokeMoveRejected=0;
    bool smokeFailed=false;std::string smokeError;
};
class GpuDepositPermit {
public:
    virtual ~GpuDepositPermit()=default;
    virtual bool move(Vec3 origin,std::string& error)=0;
};
struct GpuDepositSpawnRequest {std::uint64_t serial=0;std::shared_ptr<const Asset> asset;Vec3 origin{};};
struct GpuDepositAdmissionResult {SpawnStatus status=SpawnStatus::UnsupportedFeature;std::string reason;std::shared_ptr<GpuDepositPermit> permit;};
using GpuFieldDepositAdmission=std::function<GpuDepositAdmissionResult(const GpuDepositSpawnRequest&)>;
struct GpuDepositInstance {
    Handle instance;std::uint64_t serial=0;
    std::shared_ptr<const Asset> asset;Vec3 origin{};bool paused=false;
    // Borrowed only during this callback. Never retain a pool pointer; LEF
    // splat retains the actual resources through its own real-fence lifetime.
    std::vector<GpuParticlePool*> emitters;
};
struct GpuDepositFrame {
    ID3D12GraphicsCommandList* commands=nullptr;
    std::uint64_t fence=0,tick=0;
    // Complete live set of FieldDeposit instances, including paused sources.
    // Empty means all writing sources retired. Flush synchronizes lifecycle
    // only; it must not re-splat or advance the simulation publication tick.
    bool simulated=false;std::vector<GpuDepositInstance> instances;
};
using GpuFieldDepositDriver=std::function<bool(const GpuDepositFrame&,std::string&)>;
struct GpuSmokeRetirement {std::uint64_t serial=0;bool immediate=false;};
struct GpuSmokeFrame:GpuDepositFrame {std::vector<GpuSmokeRetirement> retired;};
struct GpuSmokeHooks {
    // Complete live set of smoke assets, including paused instances. Retirement
    // distinguishes immediate kill from naturally exhausted particles/tail smoke.
    std::function<bool(const GpuSmokeFrame&,std::string&)> record;
    GpuFieldDepositAdmission admit;
};
// Driver runs after GPU simulation/spawn and before runtime tick advancement.
// It must use the same actual queue/fence, honor paused/flush semantics and
// avoid re-entering this runtime. On failure, abandon or drain recorded work
// and recreate the runtime; further recording is rejected to prevent retries
// from simulating the same tick twice. No driver means deposits are rejected.
struct GpuDrawBatch {
    Handle instance;
    std::uint32_t emitter=0;
    GpuPoolBuffers buffers;
    std::vector<Renderer> renderers;
};
// Visual-only fixed 120 Hz instance scheduler. Commands, readbacks and draws
// share one queue/fence timeline. collect never waits and only maps completed
// readbacks. Draw batches are borrowed through the next lifecycle operation;
// prepareDraw must receive the fence of every submission using those buffers.
class GpuRuntime {
public:
    explicit GpuRuntime(ID3D12Device* device,GpuRuntimeLimits limits={},GpuFieldDepositDriver deposits={},GpuFieldDepositAdmission admission={},GpuSmokeHooks smoke={});
    ~GpuRuntime();
    GpuRuntime(const GpuRuntime&)=delete;
    GpuRuntime& operator=(const GpuRuntime&)=delete;
    void install(std::shared_ptr<const Asset> asset);
    SpawnResult spawn(const std::string& assetId,Vec3 origin={},std::uint64_t seed=0,
                      std::shared_ptr<fields::GpuVectorView> field={},SpawnParams params={});
    bool bindField(Handle handle,std::shared_ptr<fields::GpuVectorView> field,std::string& error);
    bool valid(Handle handle) const;
    bool pause(Handle handle,bool paused);
    bool move(Handle handle,Vec3 origin);
    // Immediate stop invalidates the handle now; flush/step records the kill.
    bool stop(Handle handle,bool immediate=false);
    void clear();
    bool step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t completedFence,std::string& error,
              std::optional<fields::Tick> fieldTick={});
    bool flush(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t completedFence,std::string& error);
    bool prepareDraw(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    void collect(std::uint64_t completedFence);
    std::vector<GpuDrawBatch> drawBatches() const;
    GpuRuntimeStats stats() const;
    bool shutdown(std::uint64_t completedFence,std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

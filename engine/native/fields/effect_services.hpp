#pragma once
#include "../effects/field_deposits.hpp"
#include "../smoke/sharing.hpp"

namespace engine::fields {
struct EffectFieldLimits {
    GpuVisualLimits world;
    GpuLocalGrid grid;
    effects::GpuDepositLimits deposits;
    smoke::SharingLimits sharing;
    smoke::ServiceLimits smoke;
    // Fixed partitions cover all three field stores plus private smoke. GPU
    // particles, optical targets, cloth and water have independent budgets.
    std::uint64_t maxAllocationBytes=288ull*1024*1024;
};
struct EffectFieldHooks {effects::GpuFieldDepositDriver deposits;effects::GpuFieldDepositAdmission admission;effects::GpuSmokeHooks smoke;};
struct EffectFieldStats {
    std::uint64_t steps=0,flushes=0,deferredCommits=0,lastTick=0,lastFence=0,allocationBytes=0,fieldBytes=0,reservedSmokeBytes=0;
    std::size_t readers=0;bool failed=false,recording=false;std::string lastDeferral;
    GpuVisualStats world;
};
// Owns one typed visual world and both production adapters. Runtime hooks use
// one real simulation tick; consumers use this router rather than raw WF views.
// One CPU recording owner. Drain the actual queue before destruction.
class GpuEffectFields {
public:
    static constexpr ChannelId ParticleDensity=0,ParticleMoisture=1,SmokeDensity=2;
    using SmokeObserver=std::function<smoke::SharingObservation(const smoke::ServiceOutput&)>;
    GpuEffectFields(ID3D12Device*,ID3D12Fence*,EffectFieldLimits={});
    ~GpuEffectFields();GpuEffectFields(const GpuEffectFields&)=delete;
    static std::vector<Channel> catalog();
    EffectFieldHooks hooks();
    bool setSmokeObserver(SmokeObserver,std::string& error);
    GpuAcquisition acquire(LeaseRequest);bool release(GpuLease,std::string& error);
    std::shared_ptr<GpuVisualView> view(GpuLease,std::string& error);
    effects::GpuFieldDeposits& deposits();smoke::GpuSmokeService& solver();smoke::GpuSmokeSharing& sharing();
    void collect();EffectFieldStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::shared_ptr<Impl> impl_;
};
}

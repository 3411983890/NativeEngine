#pragma once
#include "gpu_runtime.hpp"
#include "../fields/gpu_sharing.hpp"

namespace engine::effects {
struct GpuDepositLimits {
    std::uint32_t maxInstances=32,maxPagesPerSource=64;
    std::uint64_t maxAllocationBytes=128ull*1024*1024;
    fields::GpuLocalGrid grid;
    fields::GpuLocalLimits local{64,256,512,1000000,1048576,64ull*1024*1024};
    fields::GpuVisualLimits world;
    fields::PromotionPolicy promotion;
    fields::Tick publicationMaxDelayTicks=6;
};
struct GpuDepositObservation {bool worldVisible=false;unsigned subscribers=0,priority=0;std::int32_t concentration=0;};
struct GpuDepositPublicationInfo {
    bool pending=false,overdue=false;
    fields::Tick pendingSince=0,lastAttemptTick=0,lastPublishedTick=0;
    std::uint64_t deferredTicks=0;
    std::string reason;
};
struct GpuDepositSourceInfo {
    bool exists=false,ready=false;
    std::uint64_t source=0;fields::LocalSpec spec;fields::GpuSharingInfo sharing;fields::Tick sourceTick=0;
    GpuDepositPublicationInfo publication;
};
struct GpuDepositPublicationEntry {
    Handle instance;std::uint64_t source=0;unsigned priority=0;
    GpuDepositPublicationInfo publication;
};
struct GpuDepositStats {
    std::uint64_t frames=0,flushes=0,rewrites=0,gridRebuilds=0,deferredPromotions=0,allocationBytes=0,lastFence=0,lastTick=0;
    std::size_t instances=0,pendingRetirements=0,readLeases=0;
    fields::GpuLocalStats local;fields::GpuVisualStats world;fields::GpuSharingStats sharing;
    bool failed=false;
    std::uint64_t reservedMutableBytes=0,reservedSamples=0,unmaterializedBytes=0,deferredWrites=0,deferredUpdates=0,admissionRejected=0;
    std::size_t reservations=0;
    std::uint64_t deferredCommits=0;
    std::string lastCommitDeferral;
    std::uint64_t deferredCandidates=0;
    std::size_t pendingPublications=0,overduePublications=0;
    std::string lastCandidateDeferral;
    bool ownsWorld=true,publicationPrepared=false,publicationStaged=false;
    std::size_t retainedPoolPointers=0;
};
enum class GpuDepositPublicationStage { Ready, Deferred, Invalid };
// Particle snapshot -> private LEF -> optional visual WF projections. The
// catalog is copied. This owns no particles, CPU authority or fluid history.
// One recording owner, one actual queue/fence. Renderer teardown must drain
// that queue before destruction. No synchronous field/particle value readback.
class GpuFieldDeposits {
public:
    GpuFieldDeposits(ID3D12Device*,ID3D12Fence*,std::vector<fields::Channel>,GpuDepositLimits={});
    // Shared world outlives this service. Only explicitly listed channels are
    // writable; global world bytes are accounted by the owning coordinator.
    GpuFieldDeposits(fields::GpuVisualWorld&,std::vector<fields::ChannelId> writableChannels,GpuDepositLimits={});
    ~GpuFieldDeposits();GpuFieldDeposits(const GpuFieldDeposits&)=delete;
    static std::vector<fields::Channel> defaultCatalog();
    bool supports(const Asset&,std::string& error) const;
    GpuDepositAdmissionResult admit(const GpuDepositSpawnRequest&);
    bool record(const GpuDepositFrame&,std::string& error);
    // Shared mode: prepare consumes borrowed pools synchronously; retained
    // metadata has no pool pointers. Begin a joint transaction after all local
    // preparations, stage each service, commit/rollback once, then finish each.
    // Command list and real fence must match the preparation recording scope.
    bool prepare(const GpuDepositFrame&,std::string& error);
    GpuDepositPublicationStage stagePublication(ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    bool finishPublication(fields::GpuCommitResult,std::string& error);
    fields::GpuFieldSharing* publicationController();
    // Lifecycle rejection for new production views. Existing immutable views
    // retain their recorded snapshot; pending movement keeps valid old data.
    bool blocksNewViews(const fields::LeaseRequest&) const;
    // Consumer grants are read-only. Their region/channel overlap also counts
    // as an observation; all authority/Strong requests remain rejected by WF.
    fields::GpuAcquisition acquire(fields::LeaseRequest);
    bool release(fields::GpuLease,std::string& error);
    std::shared_ptr<fields::GpuVisualView> view(fields::GpuLease,std::string& error);
    bool observe(Handle,GpuDepositObservation,std::string& error);
    GpuDepositSourceInfo inspect(Handle) const;
    std::vector<GpuDepositPublicationEntry> publicationBacklog() const;
    fields::GpuLocalBuffers localBuffers(Handle) const;
    bool markLocalUsed(Handle,std::uint64_t fence);
    void collect();GpuDepositStats stats() const;
    // Normal shutdown requires runtime retirement to have been synchronized
    // by a simulation callback, no consumer grants/views and completed work.
    bool shutdown(std::string& error);
private:struct Impl;std::shared_ptr<Impl> impl_;
};
}

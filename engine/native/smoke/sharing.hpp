#pragma once
#include "service.hpp"
#include "../fields/gpu_sharing.hpp"

namespace engine::smoke {
struct SharingLimits {
    unsigned maxInstances=8,maxPublicationsPerTick=4;
    std::uint64_t maxAllocationBytes=64ull*1024*1024;
    fields::GpuLocalGrid grid;
    fields::GpuLocalLimits local{16,64,64,1000000,1048576,32ull*1024*1024};
    fields::GpuVisualLimits world{128,8,64,64,32,65536,32ull*1024*1024};
    fields::PromotionPolicy promotion;
    fields::Tick maxAgeTicks=6;
};
struct SharingObservation {std::uint64_t serial=0;bool worldVisible=false;unsigned subscribers=0,priority=0;std::int32_t concentration=0;};
struct SharingSource {
    bool exists=false,materialized=false,retiring=false,pending=false;
    std::uint64_t serial=0,source=0,inputVersion=0,publishedInputVersion=0;
    fields::Tick sourceTick=0,pendingSince=0;
    fields::GpuSharingInfo sharing;std::string reason;
};
struct SharingStats {
    std::uint64_t syncs=0,imports=0,publications=0,deferredCandidates=0,deferredCommits=0,lastTick=0,lastFence=0,allocationBytes=0;
    unsigned instances=0,pending=0,retiring=0;bool failed=false,queriesBlocked=false;
    fields::GpuLocalStats local;fields::GpuVisualStats world;fields::GpuSharingStats sharing;
    std::string lastDeferral;
    bool ownsWorld=true,publicationPrepared=false,publicationStaged=false;
    std::size_t retainedSolverPointers=0;
};
enum class SharingPublicationStage { Ready, Invalid };
struct SharedDensityMedium {std::uint64_t serial=0,inputVersion=0;fields::Tick sourceTick=0;std::shared_ptr<fields::GpuLocalProjection> projection;};
// Optional visual sharing: service retains the solver. No CPU authority writes.
// Call after the producer's final simulation/flush for a frame. Strictly newer
// simulation ticks may commit WF; same-tick calls only invalidate retired
// sources for new readers. Existing immutable views retain their old version.
class GpuSmokeSharing {
public:
    GpuSmokeSharing(ID3D12Device*,ID3D12Fence*,GpuSmokeService&,SharingLimits={});
    // World and producer outlive the service. The explicit channel must have
    // the canonical smoke density unit/owner/quantization; its name may differ.
    GpuSmokeSharing(fields::GpuVisualWorld&,GpuSmokeService&,fields::ChannelId densityChannel,SharingLimits={});
    ~GpuSmokeSharing();GpuSmokeSharing(const GpuSmokeSharing&)=delete;
    static std::vector<fields::Channel> catalog();
    bool sync(ID3D12GraphicsCommandList*,std::uint64_t fence,fields::Tick,const std::vector<SharingObservation>&,std::string& error);
    // simulated=false refreshes lifecycle only; a flush never invents a tick.
    bool prepare(ID3D12GraphicsCommandList*,std::uint64_t fence,fields::Tick,bool simulated,const std::vector<SharingObservation>&,std::string& error);
    SharingPublicationStage stagePublication(ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    bool finishPublication(fields::GpuCommitResult,std::string& error);
    fields::GpuFieldSharing* publicationController();
    bool blocksNewViews(const fields::LeaseRequest&) const;
    fields::GpuAcquisition acquire(fields::LeaseRequest);
    bool release(fields::GpuLease,std::string& error);
    std::shared_ptr<fields::GpuVisualView> view(fields::GpuLease,std::string& error);
    SharingSource inspect(std::uint64_t serial) const;
    // Only current, acknowledged versions. Caller marks the projection with
    // its real read fence; deferred/stopped/newer-unpublished sources omitted.
    std::vector<SharedDensityMedium> media() const;
    void collect();SharingStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}

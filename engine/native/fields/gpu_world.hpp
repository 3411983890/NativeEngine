#pragma once
#include "gpu_local.hpp"

namespace engine::fields {
class GpuFieldTransaction;
struct GpuVisualLimits {
    std::uint32_t maxPages=128,maxSnapshots=8,maxLeases=256,maxViews=256,maxSources=128,maxRows=65536;
    std::uint64_t maxAllocationBytes=64*1024*1024;
};
struct GpuVisualStats {
    std::uint32_t leases=0,views=0,sources=0,contributingSources=0,currentPages=0,residentPages=0,snapshots=0;
    std::uint64_t defaultBytes=0,uploadBytes=0,descriptorEstimateBytes=0,accountedBytes=0;
    std::uint64_t commits=0,dispatches=0,publications=0,duplicates=0,rejections=0,lastFence=0;
    std::uint64_t deferredCommits=0;
};
struct GpuWorldContext {ID3D12Device* device=nullptr;ID3D12Fence* timeline=nullptr;std::vector<Channel> catalog;GpuLocalGrid grid;};
enum class GpuCommitCode { Committed, Deferred, Invalid, ResourceFailure };
struct GpuCommitResult {
    GpuCommitCode code=GpuCommitCode::Invalid;
    bool accepted() const{return code==GpuCommitCode::Committed;}
    bool deferred() const{return code==GpuCommitCode::Deferred;}
};
enum class GpuBudgetLimit { None, Invalid, Pages, Leases, Sources, Snapshots, Rows, Bytes };
struct GpuVisualProposal {
    std::string owner;std::uint64_t source=0;Region region;
    std::vector<ChannelId> channels;std::size_t maxPages=64;
    // Sources of the same owner withdrawn atomically with this proposal.
    std::vector<std::uint64_t> retiringSources;
};
struct GpuVisualBudget {
    GpuBudgetLimit limit=GpuBudgetLimit::None;
    std::uint64_t pages=0,rows=0,descriptors=0,allocationBytes=0,residentBytes=0;
    bool fits() const{return limit==GpuBudgetLimit::None;}
    const char* reason() const;
};
class GpuVisualView {
public:
    ~GpuVisualView();GpuVisualView(const GpuVisualView&)=delete;
    GpuBindings bindings() const;bool markUsed(std::uint64_t fence);std::uint64_t lastFence() const;
private:
    GpuVisualView();struct Impl;std::unique_ptr<Impl> impl_;friend class GpuVisualWorld;
};
// GPU visual WF: CPU metadata leases/source epochs, GPU-only contribution
// values and sparse aggregate snapshots. No CPU World mutation or readback.
// All producers/readers use the same actual queue/fence timeline.
// One CPU recording owner; this metadata service is not thread-safe.
class GpuVisualWorld {
public:
    GpuVisualWorld(ID3D12Device*,ID3D12Fence*,std::vector<Channel> catalog,GpuVisualLimits limits={},GpuLocalGrid grid={});
    ~GpuVisualWorld();GpuVisualWorld(const GpuVisualWorld&)=delete;
    GpuAcquisition acquire(LeaseRequest);
    bool release(GpuLease,std::string& error);
    // WF epoch is explicit and independent of the producer's local epoch.
    PublishResult publish(GpuLease,std::shared_ptr<GpuLocalProjection>,std::uint64_t epoch,Tick publishTick,std::string& error);
    PublishResult withdraw(GpuLease,std::uint64_t source,std::uint64_t epoch,Tick publishTick,std::string& error);
    bool commit(ID3D12GraphicsCommandList*,std::uint64_t fence,Tick tick,std::string& error);
    // Capacity rejection records no aggregate GPU commands and preserves the
    // committed snapshot. Staged publication metadata remains caller-owned.
    GpuCommitResult tryCommit(ID3D12GraphicsCommandList*,std::uint64_t fence,Tick tick,std::string& error);
    // CPU metadata/resource-allocation-info only. No buffers, grants, epochs or
    // GPU commands are created. A proposal can be tested before LEF projection.
    GpuVisualBudget budget(const GpuVisualProposal* proposal=nullptr) const;
    std::shared_ptr<GpuVisualView> view(GpuLease,std::string& error);
    SourceView sourceView(const std::string& owner,std::uint64_t source) const;
    // Global read demand, including consumers registered by other producers.
    std::uint32_t subscribers(Region,const std::vector<ChannelId>&) const;
    GpuWorldContext context() const;
    void collect();GpuVisualStats stats() const;bool shutdown(std::string& error);
private:
    friend class GpuFieldSharing;
    friend class GpuFieldTransaction;
    bool beginTransaction(std::string& error);
    bool rollbackTransaction() noexcept;
    void finishTransaction() noexcept;
    void retireCoverage(GpuLease);
    bool hasStagedPublications() const;
    struct Impl;std::unique_ptr<Impl> impl_;
    GpuFieldTransaction* transactionGroup_=nullptr;bool groupCommitInProgress_=false,groupMutationAllowed_=false;
};
}

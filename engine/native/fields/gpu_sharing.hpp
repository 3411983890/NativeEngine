#pragma once
#include "gpu_world.hpp"

namespace engine::fields {
struct GpuSharingHandle {std::uint32_t slot=0,generation=0;};
// Registration is metadata only. Coverage/channels are taken from the first
// accepted immutable projection; local simulation remains producer-owned.
// Source IDs increase monotonically within this controller, including retired
// registrations, so slot reuse cannot resurrect an earlier source identity.
struct GpuSharingSpec {std::string owner;std::uint64_t source=0;std::size_t maxWorldPages=64;Tick maxAgeTicks=3;};
struct GpuSharingCreation {LocalResult result;GpuSharingHandle handle;};
struct GpuSharingInfo {
    LocalState state=LocalState::Retired;
    std::uint64_t source=0,acknowledgedEpoch=0,pendingEpoch=0,localVersion=0;
    Tick sourceTick=0,stateSince=0,pendingPublishTick=0,lastActionTick=0;
    bool pending=false,retireAfterCommit=false;
};
struct GpuSharingObservation {
    GpuSharingHandle handle;
    bool worldVisible=false;unsigned subscribers=0,priority=0;
    // An explicitly supplied observation, not a synchronous GPU density read.
    std::int32_t concentration=0;
    std::shared_ptr<GpuLocalProjection> projection;
};
struct GpuSharingObservationResult {GpuSharingHandle handle;LocalResult result;};
struct GpuSharingStats {
    std::size_t instances=0,shared=0,pending=0,promotionsThisTick=0;
    std::uint64_t promotions=0,updates=0,withdrawals=0,rejections=0;
};
// Visual sharing controller. World must outlive this single-recording-owner
// service. Normal teardown retires, commits World, then settles each source.
// Pending means recorded metadata, not completed GPU work. Settle acknowledges
// the exact committed source version; actual resources remain fence-protected.
class GpuFieldSharing {
public:
    explicit GpuFieldSharing(GpuVisualWorld&,std::size_t maxInstances=128);
    ~GpuFieldSharing();GpuFieldSharing(const GpuFieldSharing&)=delete;
    GpuSharingCreation create(GpuSharingSpec,Tick createdAt=0);
    bool valid(GpuSharingHandle) const;GpuSharingInfo inspect(GpuSharingHandle) const;
    LocalResult promote(GpuSharingHandle,std::shared_ptr<GpuLocalProjection>,Tick publishTick);
    LocalResult project(GpuSharingHandle,std::shared_ptr<GpuLocalProjection>,Tick publishTick);
    LocalResult demote(GpuSharingHandle,Tick publishTick);
    LocalResult retire(GpuSharingHandle,Tick publishTick);
    LocalResult settle(GpuSharingHandle);
    // Priority descending, then source ID and owner ascending. Quota persists
    // across repeated calls at one tick. Invalid batches change no decisions.
    std::vector<GpuSharingObservationResult> observe(const std::vector<GpuSharingObservation>&,Tick,const PromotionPolicy& = {});
    GpuSharingStats stats() const;
    // Optional atomic publication batch. Failure restores writer grants,
    // source epochs, sharing states and quota; local GPU simulation is separate.
    // Existing explicit publish/commit/settle callers remain supported.
    bool beginTransaction(std::string& error);
    GpuCommitResult commitTransaction(ID3D12GraphicsCommandList*,std::uint64_t fence,Tick,std::string& error);
    void rollbackTransaction() noexcept;
    bool inGroupTransaction() const{return group_!=nullptr;}
private:
    friend class GpuFieldTransaction;
    struct Slot {
        bool active=false,hasWriter=false;std::uint32_t generation=1;
        GpuSharingSpec spec;GpuSharingInfo info;GpuLease writer;
        Region region;std::vector<ChannelId> channels;
    };
    Slot* find(GpuSharingHandle);const Slot* find(GpuSharingHandle) const;
    LocalResult publish(Slot&,std::shared_ptr<GpuLocalProjection>,Tick);
    LocalResult withdraw(Slot&,Tick,bool retiring);void dispose(Slot&);
    bool canMutate() const{return !world_.transactionGroup_||world_.transactionGroup_==group_;}
    GpuVisualWorld& world_;std::size_t maxInstances_;std::vector<Slot> slots_;
    GpuSharingStats counters_;Tick observationTick_=0;std::uint64_t lastSource_=0;bool hasObservationTick_=false;
    struct Checkpoint {std::vector<Slot> slots;GpuSharingStats counters;Tick observationTick;bool hasObservationTick;};
    std::unique_ptr<Checkpoint> checkpoint_;
    GpuFieldTransaction* group_=nullptr;
};
// One WF commit for multiple solver-owned sharing controllers. Register/local
// preparation must precede begin(); producer states settle only after commit.
// World and participants must outlive this noncopyable recording scope.
class GpuFieldTransaction {
public:
    GpuFieldTransaction(GpuVisualWorld&,std::vector<GpuFieldSharing*>);
    ~GpuFieldTransaction();GpuFieldTransaction(const GpuFieldTransaction&)=delete;
    bool begin(std::string& error);
    GpuCommitResult commit(ID3D12GraphicsCommandList*,std::uint64_t fence,Tick,std::string& error);
    void rollback() noexcept;bool active() const{return active_;}
private:
    GpuVisualWorld& world_;std::vector<GpuFieldSharing*> participants_;bool active_=false;
};
}

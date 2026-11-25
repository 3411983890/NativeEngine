#pragma once
#include "world.hpp"

namespace engine::fields {

enum class ProjectionReduction { VolumeAverage, Maximum };
struct LocalChannel { ChannelId channel = 0; ProjectionReduction reduction = ProjectionReduction::VolumeAverage; };
struct LocalSpec {
    std::string owner;
    // Source IDs are monotonic within this controller and unique among the
    // owner's producers. Retiring a slot never reuses its previous source ID.
    std::uint64_t source = 0;
    Cell originFine{}, dimensions{8,8,8};
    unsigned finePerWorld = 2;
    std::vector<LocalChannel> channels;
    std::size_t maxWorldPages = 64;
    Tick maxAgeTicks = 3;
};
struct LocalHandle { std::uint32_t slot = 0, generation = 0; };
enum class LocalState { LocalOnly, PromotePending, Shared, DemotePending, Retired };
enum class LocalCode { Accepted, NoChange, InvalidHandle, Invalid, Capacity, Busy, NotShared, PublicationRejected };
struct LocalResult {
    LocalCode code = LocalCode::Invalid;
    PublishResult publication = PublishResult::Invalid;
    std::string error;
    bool accepted() const { return code==LocalCode::Accepted||code==LocalCode::NoChange; }
};
struct LocalCreation { LocalResult result; LocalHandle handle; };
struct LocalValues { ChannelId channel = 0; std::vector<std::int32_t> values; };
struct ProjectionMetric {
    ChannelId channel = 0;
    ProjectionReduction reduction = ProjectionReduction::VolumeAverage;
    std::size_t coarseSamples = 0;
    // Only VolumeAverage is an integral projection. The residual is the signed
    // quantization loss in fine-cell integer units, not a spatial error estimate.
    std::int64_t inputIntegral = 0, reconstructedIntegral = 0, quantizationResidual = 0;
};
struct LocalInfo {
    LocalState state = LocalState::Retired;
    std::uint64_t source = 0, localVersion = 0, acknowledgedEpoch = 0, pendingEpoch = 0;
    Tick sourceTick = 0, transformTick = 0, pendingPublishTick = 0, stateSince = 0;
    Cell originFine{}, dimensions{};
    bool pending = false, retireAfterCommit = false;
    std::vector<ProjectionMetric> projection;
};
enum class Representation { Unavailable, LocalDetail, SharedProxy };
struct RoutedSample {
    Representation representation = Representation::Unavailable;
    Sample sample;
    std::uint64_t source = 0;
};
struct LocalLimits { std::size_t maxInstances = 128, maxLocalSamples = 1048576; };
struct PromotionPolicy {
    std::int32_t promoteAt = 600, demoteAt = 300;
    Tick minimumSharedTicks = 30, minimumLocalTicks = 2;
    std::size_t promotionsPerTick = 4;
};
struct Observation {
    LocalHandle handle;
    bool worldVisible = false;
    unsigned subscribers = 0, priority = 0;
    std::int32_t concentration = 0;
};
struct ObservationResult { LocalHandle handle; LocalResult result; };
struct LocalStats {
    std::size_t instances = 0, localSamples = 0, shared = 0, pending = 0;
    std::uint64_t acceptedProjections = 0, rejectedProjections = 0;
};

// B2 CPU visual reference. LEF always owns its local values/solver; WF receives
// replaceable coarse projections. This does not migrate a solver or implement
// GPU sparse storage. World must outlive the controller. Normal teardown calls
// retire(), commits World, then settle(); the destructor only releases leases.
class LocalFields {
public:
    explicit LocalFields(World& world, LocalLimits limits = {});
    ~LocalFields();
    LocalFields(const LocalFields&) = delete;
    LocalFields& operator=(const LocalFields&) = delete;
    LocalCreation create(LocalSpec spec);
    bool valid(LocalHandle handle) const;
    LocalInfo inspect(LocalHandle handle) const;
    LocalResult replace(LocalHandle handle, const std::vector<LocalValues>& values, Tick sourceTick);
    LocalResult promote(LocalHandle handle, Tick publishTick);
    LocalResult project(LocalHandle handle, Tick publishTick);
    LocalResult move(LocalHandle handle, Cell originFine, Tick publishTick);
    LocalResult demote(LocalHandle handle, Tick publishTick);
    LocalResult retire(LocalHandle handle, Tick publishTick);
    LocalResult settle(LocalHandle handle, std::uint64_t lastUseFence = 0);
    RoutedSample sample(LocalHandle handle, ChannelId channel, Cell positionFine,
                        Tick requestedTick, Representation preferred = Representation::LocalDetail) const;
    // A bounded stable scheduler: priority descending, then source ID ascending.
    // Observation affects visual sharing only; CPU authority channels are rejected.
    std::vector<ObservationResult> observe(const std::vector<Observation>& observations,
                                           Tick publishTick, const PromotionPolicy& policy = {});
    LocalStats stats() const;
private:
    struct Slot {
        bool active = false;
        std::uint32_t generation = 1;
        LocalSpec spec;
        LocalInfo info;
        std::vector<LocalValues> values;
        Lease writer{}, pendingWriter{};
        bool hasWriter = false, hasPendingWriter = false;
    };
    Slot* find(LocalHandle handle);
    const Slot* find(LocalHandle handle) const;
    LocalResult publish(Slot& slot, Cell originFine, Tick publishTick, bool moving);
    LocalResult withdraw(Slot& slot, Tick publishTick, bool retiring);
    void dispose(Slot& slot);
    World& world_;
    LocalLimits limits_;
    std::vector<Slot> slots_;
    std::size_t localSamples_ = 0;
    std::uint64_t lastSource_ = 0, projections_ = 0, rejections_ = 0;
    Tick observationTick_ = 0;
    std::size_t promotionsThisTick_ = 0;
    bool hasObservationTick_ = false;
};
} // namespace engine::fields

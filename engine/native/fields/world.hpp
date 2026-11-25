#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace engine::fields {

constexpr std::int32_t BrickDim = 16;
constexpr std::size_t BrickVoxels = BrickDim * BrickDim * BrickDim;
using ChannelId = std::uint32_t;
using Tick = std::uint64_t;

struct Cell {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator<(const Cell& b) const { return std::tie(x,y,z) < std::tie(b.x,b.y,b.z); }
    bool operator==(const Cell& b) const { return x == b.x && y == b.y && z == b.z; }
};
// Integer cell bounds, min inclusive, max exclusive. Units and world mapping
// belong to the channel descriptor, not to an implicit global metre scale.
struct Region {
    Cell min, max;
    bool contains(Cell cell) const;
};
enum class Domain { CpuAuthority, VisualReference };
enum class Merge { Sum, Maximum };
enum class Consistency { Strong, Eventual, Stale };
enum class Status { Ok, InvalidLease, AccessDenied, UnknownChannel, OutsideRegion,
                    Unavailable, TickMismatch, Stale, InvalidPosition };
enum class PublishResult { Accepted, Duplicate, OldEpoch, Conflict, Invalid,
                           AccessDenied, PastTick, ResourceExpired, CapacityExceeded };

struct Channel {
    std::string name, unit, owner;
    Domain domain = Domain::CpuAuthority;
    Merge merge = Merge::Sum;
    std::int32_t minimum = 0, maximum = 65535;
    // Values are fixed integers. value / unitsPerInteger converts to the declared unit.
    std::uint32_t unitsPerInteger = 1;
};
struct LeaseRequest {
    Region region;
    std::vector<ChannelId> channels;
    std::string owner;
    bool writable = false;
    Consistency consistency = Consistency::Eventual;
    Tick maxAgeTicks = 3;
    std::size_t maxPages = 64;
    // Read-only stencil coverage. Query centres remain inside region; halo
    // neighbours consume the same page budget and never grant writer access.
    std::uint32_t haloCells = 0;
    bool storageRegion(Region& out) const;
};
// Coordinates refer to lattice samples, without an implicit half-cell offset.
// Each fraction is [0,255]/256; use floor coordinates for negative positions.
struct SamplePosition {
    Cell cell;
    std::array<std::uint32_t,3> fraction{};
};
struct Lease {
    std::uint32_t slot = 0, generation = 0;
    bool operator==(const Lease& b) const { return slot == b.slot && generation == b.generation; }
};
struct Acquisition {
    bool accepted = false;
    Lease lease;
    std::string error;
    std::size_t coveredPages = 0;
};
struct Write { Cell cell; std::int32_t value = 0; };
struct Publication {
    Lease writer;
    ChannelId channel = 0;
    std::uint64_t source = 0, epoch = 0;
    Tick sourceTick = 0, publishTick = 0;
    Domain origin = Domain::VisualReference;
    std::vector<Write> writes;
};
struct SourceView {
    bool exists = false, resident = false, hasContribution = false;
    std::uint64_t epoch = 0, version = 0;
    Tick sourceTick = 0, publishTick = 0, snapshotTick = 0;
};
struct Sample {
    Status status = Status::Unavailable;
    std::int32_t value = 0;
    Tick snapshotTick = 0, sourceTick = 0, ageTicks = 0;
    std::uint64_t version = 0;
    std::uint64_t sourceEpoch = 0;
    bool clamped = false;
    bool valid() const { return status == Status::Ok; }
};
struct Stats {
    std::size_t activePages = 0, retiredPages = 0, leases = 0;
    std::size_t committedBytes = 0, sourceTiles = 0, sourceRecords = 0;
    std::uint64_t publications = 0, duplicates = 0, rejections = 0;
};

// B0/B1 CPU reference. It has real regional allocation and immutable committed
// views, but is not the GPU field allocator or the full LEF/WF scheduler.
class World {
public:
    explicit World(std::size_t maxPhysicalPages = 1024, std::size_t maxSourceTiles = 4096);
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    ChannelId registerChannel(Channel channel);
    const Channel& channel(ChannelId id) const;
    Acquisition acquire(LeaseRequest request);
    bool release(Lease lease, std::uint64_t lastUseFence = 0);
    void collect(std::uint64_t completedFence);
    PublishResult publish(Lease writer, ChannelId channel, std::uint64_t source,
                          std::uint64_t epoch, Tick sourceTick, Domain origin,
                          const std::vector<Write>& writes);
    // A late visual source has its own source tick; publication never rewrites
    // a committed snapshot. The original publish() uses the same tick for both.
    PublishResult publishAt(const Publication& publication);
    // CPU reference transaction: clone/check staging, then swap all affected
    // state. Failed batches cannot leave a partial multi-channel publication.
    PublishResult publishAtomic(const std::vector<Publication>& publications);
    // Commit is the publication barrier. Readers see the old view until here.
    bool commit(Tick tick);
    Sample sample(Lease reader, ChannelId channel, Cell cell, Tick requestedTick) const;
    Sample sampleSource(Lease reader, ChannelId channel, std::uint64_t source,
                        Cell cell, Tick requestedTick) const;
    Sample sampleLinear(Lease reader, ChannelId channel, SamplePosition position, Tick requestedTick) const;
    Sample sampleSourceLinear(Lease reader, ChannelId channel, std::uint64_t source,
                              SamplePosition position, Tick requestedTick) const;
    SourceView sourceView(ChannelId channel, std::uint64_t source) const;
    bool sourcePresent(ChannelId channel, std::uint64_t source) const;
    Tick committedTick() const { return committedTick_; }
    std::uint64_t version() const { return version_; }
    std::size_t channelCount() const { return channels_.size(); }
    std::uint64_t digest() const;
    Stats stats() const;

private:
    struct PageKey {
        ChannelId channel;
        Cell brick;
        bool operator<(const PageKey& b) const {
            return std::tie(channel,brick.x,brick.y,brick.z) <
                   std::tie(b.channel,b.brick.x,b.brick.y,b.brick.z);
        }
    };
    struct Tile {
        std::array<std::int32_t, BrickVoxels> values{};
        std::bitset<BrickVoxels> known;
        Tick sourceTick = 0;
    };
    struct Page {
        std::size_t references = 0;
        std::uint64_t generation = 0;
        std::uint64_t lastUseFence = 0;
        std::map<std::uint64_t, Tile> staging, committed;
    };
    struct LeaseSlot {
        std::uint32_t generation = 1;
        bool active = false;
        LeaseRequest request;
        std::vector<PageKey> pages;
    };
    struct Source {
        std::string owner;
        Lease writer;
        std::uint64_t epoch = 0, fingerprint = 0;
        Tick tick = 0, publishTick = 0;
        std::vector<std::pair<PageKey, std::uint64_t>> pages;
    };
    struct Retired { std::shared_ptr<Page> page; std::uint64_t fence; };
    const LeaseSlot* findLease(Lease lease) const;
    static PageKey pageKey(ChannelId channel, Cell cell);
    static std::size_t localIndex(Cell cell);
    std::vector<Channel> channels_;
    std::vector<LeaseSlot> leases_;
    std::map<PageKey, std::shared_ptr<Page>> pages_;
    std::vector<Retired> retired_;
    std::map<std::pair<ChannelId, std::uint64_t>, Source> sources_;
    std::map<std::pair<ChannelId, std::uint64_t>, Source> committedSources_;
    std::size_t maxPhysicalPages_, maxSourceTiles_;
    std::uint64_t nextPageGeneration_ = 1, version_ = 0;
    Tick committedTick_ = 0;
    bool hasCommit_ = false;
    Sample sampleImpl(Lease reader, ChannelId channel, Cell cell, Tick requestedTick,
                      const std::uint64_t* source, bool includeHalo = false) const;
    Sample sampleLinearImpl(Lease reader, ChannelId channel, SamplePosition position,
                            Tick requestedTick, const std::uint64_t* source) const;
    std::uint64_t publications_ = 0, duplicates_ = 0, rejections_ = 0;
};

} // namespace engine::fields

#pragma once

#include "asset.hpp"
#include "../../../renderer/hzb.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Vegetation world runtime: residency, streaming and the CPU reference culling path.
//
// The counting contract from the plan is implemented literally here, because the
// numbers are the deliverable:
//   worldInstances     - identities in the region index (never loaded as one array)
//   residentInstances  - instances currently decoded and owned by the CPU
//   candidateInstances - instances that survived coarse (page) culling
//   visibleInstances   - instances that survived fine culling and entered a draw list
//   shadowOnlyInstances- instances kept for shadows but not for the camera
// A page that is not resident is never counted as visible; it is counted as a page
// fault and drawn with its declared lower representation instead.

namespace engine::vegetation {

enum class PageState : std::uint8_t {
    Unloaded = 0, Requested, Reading, Decoding, UploadQueued, Resident, Retiring, Failed, Cancelled,
};

const char* pageStateName(PageState state);

struct StreamBudget {
    std::uint32_t readBytesPerFrame = 4u << 20;
    std::uint32_t decodeBytesPerFrame = 8u << 20;
    std::uint32_t uploadBytesPerFrame = 8u << 20;
    std::uint32_t maxInFlightPages = 32;
    std::uint32_t maxPagesPerFrame = 8;
    std::uint64_t softResidentBytes = 1200ull << 20;
    std::uint64_t hardResidentBytes = 1600ull << 20;
};

struct WorldStats {
    std::uint64_t worldInstances = 0;
    std::uint64_t residentInstances = 0;
    std::uint64_t candidateInstances = 0;
    std::uint64_t visibleInstances = 0;
    std::uint64_t shadowOnlyInstances = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t peakBytesIncludingInflight = 0;
    std::uint64_t uploadBytes = 0;
    std::uint32_t pageFaults = 0;
    std::uint32_t fallbackFrames = 0;
    std::uint32_t overflow = 0;
    std::uint32_t residentPages = 0;
    std::uint32_t inFlightPages = 0;
    std::uint32_t retiredPages = 0;
    std::uint32_t evictions = 0;
    std::uint32_t failedPages = 0;
};

// Sparse authoritative edit. A harvested tree is one override, not a rebuild of the
// region: the world index stays immutable and the override layer decides what is gone.
struct InstanceOverride {
    bool removed = false;
    bool harvested = false;
    float scale = 1.0f;
};

struct VisiblePage {
    std::uint32_t region = 0, page = 0;
    std::uint32_t candidateInstances = 0;
    bool resident = false;
    bool shadowOnly = false;
};

class World {
public:
    // Loads one page's packed records. Returning an empty vector is a hard failure; the
    // world records it as a page fault and keeps the fallback representation.
    using PageLoader = std::function<std::vector<PackedInstance>(std::uint32_t region, std::uint32_t page)>;

    void addRegion(std::shared_ptr<RegionFile> region);
    void setLoader(PageLoader loader) { loader_ = std::move(loader); }
    void setBudget(const StreamBudget& budget) { budget_ = budget; }
    void setFallbackReserve(float metres) { fallbackRangeMetres_ = metres; }

    void beginFrame(std::uint64_t frame);
    // Requests every page whose bounds intersect the camera sphere. `velocity` shifts
    // the request centre forward so a moving camera reads pages before it reaches them.
    void requestAround(const Vec3& camera, const Vec3& velocity, float rangeMetres, float prefetchSeconds = 0.5f);
    // Advances the page state machine under the per-frame budgets. Returns the number of
    // state transitions performed.
    std::uint32_t tick();

    // CPU reference culling. `hzb` may be null (frustum only). Returns pages that pass
    // coarse culling; `visible` counts instances that pass fine culling.
    std::vector<VisiblePage> cull(const nativegpu::Frustum& frustum, const nativegpu::DepthPyramid* hzb,
                                  const Vec3& camera, float maximumDistance,
                                  const float viewProjection[16], std::uint32_t viewportWidth, std::uint32_t viewportHeight);

    void setOverride(std::uint32_t stableId, InstanceOverride override);
    bool hasOverride(std::uint32_t stableId) const;
    const InstanceOverride* findOverride(std::uint32_t stableId) const;

    const std::vector<PackedInstance>* pageInstances(std::uint32_t region, std::uint32_t page) const;
    PageState pageState(std::uint32_t region, std::uint32_t page) const;
    const std::vector<SpeciesRef>& species(std::uint32_t region) const;
    std::uint32_t regionCount() const { return static_cast<std::uint32_t>(regions_.size()); }
    WorldStats stats() const { return stats_; }
    std::uint64_t frame() const { return frame_; }
    // Bytes currently owned plus in-flight, the number that must stay under the hard cap.
    std::uint64_t peakBytes() const { return stats_.peakBytesIncludingInflight; }

private:
    struct Slot {
        std::uint32_t region = 0, page = 0;
        PageState state = PageState::Unloaded;
        std::uint64_t lastUsedFrame = 0;
        std::uint64_t bytes = 0;
        std::vector<PackedInstance> instances;
        bool failed = false;
    };
    struct RegionEntry {
        std::shared_ptr<RegionFile> file;
        std::uint32_t firstSlot = 0, slotCount = 0;
    };

    Slot& slot(std::uint32_t region, std::uint32_t page);
    const Slot& slot(std::uint32_t region, std::uint32_t page) const;
    void recomputeStats();
    void evictIfNeeded(std::uint64_t incomingBytes);

    std::vector<RegionEntry> regions_;
    std::vector<Slot> slots_;
    std::unordered_map<std::uint32_t, InstanceOverride> overrides_;
    PageLoader loader_;
    StreamBudget budget_{};
    WorldStats stats_{};
    std::uint64_t frame_ = 0;
    float fallbackRangeMetres_ = 0.0f;
    // Budgets are consumed per frame, so the counters reset in beginFrame().
    std::uint32_t readBudget_ = 0, decodeBudget_ = 0, uploadBudget_ = 0, pagesThisFrame_ = 0;
};

} // namespace engine::vegetation

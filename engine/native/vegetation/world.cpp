#include "world.hpp"
#include "lod.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::vegetation {
namespace {
constexpr std::uint64_t kPageOverheadBytes = 512; // decoded vector capacity + slot bookkeeping
}

const char* pageStateName(PageState state) {
    switch (state) {
        case PageState::Unloaded: return "unloaded";
        case PageState::Requested: return "requested";
        case PageState::Reading: return "reading";
        case PageState::Decoding: return "decoding";
        case PageState::UploadQueued: return "uploadQueued";
        case PageState::Resident: return "resident";
        case PageState::Retiring: return "retiring";
        case PageState::Failed: return "failed";
        case PageState::Cancelled: return "cancelled";
    }
    return "unknown";
}

void World::addRegion(std::shared_ptr<RegionFile> region) {
    if (!region) throw Error("NG-E-VEG-FORMAT", "world cannot own a null region");
    RegionEntry entry;
    entry.file = std::move(region);
    entry.firstSlot = static_cast<std::uint32_t>(slots_.size());
    entry.slotCount = entry.file->pageCount();
    slots_.resize(slots_.size() + entry.slotCount);
    for (std::uint32_t index = 0; index < entry.slotCount; ++index) {
        auto& slot = slots_[entry.firstSlot + index];
        slot.region = static_cast<std::uint32_t>(regions_.size());
        slot.page = index;
        slot.state = PageState::Unloaded;
    }
    stats_.worldInstances += entry.file->worldInstances();
    regions_.push_back(std::move(entry));
}

World::Slot& World::slot(std::uint32_t region, std::uint32_t page) {
    if (region >= regions_.size()) throw Error("NG-E-VEG-PAGE", "region index outside the world");
    const auto& entry = regions_[region];
    if (page >= entry.slotCount) throw Error("NG-E-VEG-PAGE", "page index outside the region");
    return slots_[entry.firstSlot + page];
}

const World::Slot& World::slot(std::uint32_t region, std::uint32_t page) const {
    if (region >= regions_.size()) throw Error("NG-E-VEG-PAGE", "region index outside the world");
    const auto& entry = regions_[region];
    if (page >= entry.slotCount) throw Error("NG-E-VEG-PAGE", "page index outside the region");
    return slots_[entry.firstSlot + page];
}

void World::beginFrame(std::uint64_t frame) {
    frame_ = frame;
    readBudget_ = budget_.readBytesPerFrame;
    decodeBudget_ = budget_.decodeBytesPerFrame;
    uploadBudget_ = budget_.uploadBytesPerFrame;
    pagesThisFrame_ = 0;
}

void World::requestAround(const Vec3& camera, const Vec3& velocity, float rangeMetres, float prefetchSeconds) {
    const float shiftX = velocity.x * prefetchSeconds, shiftZ = velocity.z * prefetchSeconds;
    const float centreX = camera.x + shiftX, centreZ = camera.z + shiftZ;
    const float rangeSquared = rangeMetres * rangeMetres;
    for (std::uint32_t region = 0; region < regions_.size(); ++region) {
        const auto& entry = regions_[region];
        for (std::uint32_t page = 0; page < entry.slotCount; ++page) {
            auto& target = slots_[entry.firstSlot + page];
            if (target.state == PageState::Resident || target.state == PageState::Failed) {
                if (target.state == PageState::Resident) target.lastUsedFrame = frame_;
                continue;
            }
            const auto& record = entry.file->page(page);
            const float nearestX = std::max(record.minX, std::min(centreX, record.maxX));
            const float nearestZ = std::max(record.minZ, std::min(centreZ, record.maxZ));
            const float dx = nearestX - centreX, dz = nearestZ - centreZ;
            if (dx * dx + dz * dz > rangeSquared) continue;
            if (target.state == PageState::Unloaded || target.state == PageState::Cancelled) {
                target.state = PageState::Requested;
                target.lastUsedFrame = frame_;
            }
        }
    }
}

void World::evictIfNeeded(std::uint64_t incomingBytes) {
    // Eviction is LRU over resident pages with hysteresis: a page is only retired when
    // the total is above the hard cap, and the soft cap is used as the target so the
    // world does not thrash between two pages every frame.
    std::uint64_t resident = 0;
    for (const auto& slot : slots_) if (slot.state == PageState::Resident) resident += slot.bytes;
    if (resident + incomingBytes <= budget_.hardResidentBytes) return;
    std::vector<Slot*> candidates;
    for (auto& slot : slots_) if (slot.state == PageState::Resident) candidates.push_back(&slot);
    std::sort(candidates.begin(), candidates.end(), [](const Slot* a, const Slot* b) { return a->lastUsedFrame < b->lastUsedFrame; });
    for (auto* candidate : candidates) {
        if (resident + incomingBytes <= budget_.softResidentBytes) break;
        resident -= candidate->bytes;
        candidate->state = PageState::Retiring;
        ++stats_.evictions;
    }
}

std::uint32_t World::tick() {
    std::uint32_t transitions = 0;
    for (auto& slot : slots_) {
        if (pagesThisFrame_ >= budget_.maxPagesPerFrame) break;
        switch (slot.state) {
            case PageState::Requested: {
                // Admission control: a page that would push residency past the hard cap
                // is not read at all. Reading first and evicting later would spike the
                // peak, which is exactly what the budget is meant to bound.
                std::uint64_t resident = 0;
                for (const auto& other : slots_) if (other.state == PageState::Resident) resident += other.bytes;
                if (resident > budget_.hardResidentBytes) break;
                slot.state = PageState::Reading;
                ++transitions;
                break;
            }
            case PageState::Reading: {
                const auto& record = regions_[slot.region].file->page(slot.page);
                const std::uint64_t bytes = static_cast<std::uint64_t>(record.instanceCount) * sizeof(PackedInstance) + kPageOverheadBytes;
                if (bytes > readBudget_ || pagesThisFrame_ >= budget_.maxPagesPerFrame) break;
                readBudget_ -= static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, readBudget_));
                ++pagesThisFrame_;
                if (!loader_) {
                    slot.state = PageState::Failed;
                    slot.failed = true;
                    ++stats_.failedPages;
                    ++transitions;
                    break;
                }
                std::vector<PackedInstance> loaded;
                try {
                    loaded = loader_(slot.region, slot.page);
                } catch (const std::exception&) {
                    slot.state = PageState::Failed;
                    slot.failed = true;
                    ++stats_.failedPages;
                    ++stats_.pageFaults;
                    ++transitions;
                    break;
                }
                if (loaded.size() != record.instanceCount) {
                    slot.state = PageState::Failed;
                    slot.failed = true;
                    ++stats_.failedPages;
                    ++stats_.pageFaults;
                    ++transitions;
                    break;
                }
                slot.instances = std::move(loaded);
                slot.bytes = bytes;
                slot.state = PageState::Decoding;
                evictIfNeeded(bytes);
                ++transitions;
                break;
            }
            case PageState::Decoding: {
                const std::uint64_t bytes = static_cast<std::uint64_t>(slot.instances.size()) * sizeof(PackedInstance);
                if (bytes > decodeBudget_) break;
                decodeBudget_ -= static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, decodeBudget_));
                slot.state = PageState::UploadQueued;
                ++transitions;
                break;
            }
            case PageState::UploadQueued: {
                const std::uint64_t bytes = static_cast<std::uint64_t>(slot.instances.size()) * sizeof(PackedInstance);
                if (bytes > uploadBudget_) break;
                uploadBudget_ -= static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, uploadBudget_));
                stats_.uploadBytes += bytes;
                slot.state = PageState::Resident;
                slot.lastUsedFrame = frame_;
                ++transitions;
                break;
            }
            case PageState::Retiring:
                slot.instances.clear();
                slot.instances.shrink_to_fit();
                slot.bytes = 0;
                slot.state = PageState::Unloaded;
                ++transitions;
                break;
            default:
                break;
        }
    }
    recomputeStats();
    return transitions;
}

void World::recomputeStats() {
    std::uint64_t resident = 0, residentBytes = 0, inFlight = 0;
    std::uint32_t residentPages = 0, inFlightPages = 0, retired = 0;
    for (const auto& slot : slots_) {
        switch (slot.state) {
            case PageState::Resident:
                resident += slot.instances.size();
                residentBytes += slot.bytes;
                ++residentPages;
                break;
            case PageState::Reading:
            case PageState::Decoding:
            case PageState::UploadQueued:
                inFlight += slot.instances.size();
                ++inFlightPages;
                break;
            case PageState::Retiring:
                ++retired;
                break;
            default:
                break;
        }
    }
    stats_.residentInstances = resident;
    stats_.residentBytes = residentBytes;
    stats_.residentPages = residentPages;
    stats_.inFlightPages = inFlightPages;
    stats_.retiredPages = retired;
    stats_.peakBytesIncludingInflight = std::max(stats_.peakBytesIncludingInflight, residentBytes + inFlight * sizeof(PackedInstance));
}

std::vector<VisiblePage> World::cull(const nativegpu::Frustum& frustum, const nativegpu::DepthPyramid* hzb,
                                     const Vec3& camera, float maximumDistance,
                                     const float viewProjection[16], std::uint32_t viewportWidth, std::uint32_t viewportHeight) {
    std::vector<VisiblePage> result;
    stats_.candidateInstances = 0;
    stats_.visibleInstances = 0;
    stats_.shadowOnlyInstances = 0;
    const float maxDistanceSquared = maximumDistance * maximumDistance;
    for (std::uint32_t region = 0; region < regions_.size(); ++region) {
        const auto& entry = regions_[region];
        for (std::uint32_t page = 0; page < entry.slotCount; ++page) {
            const auto& record = entry.file->page(page);
            float minimum[3] = {record.minX, record.baseY, record.minZ};
            float maximum[3] = {record.maxX, record.baseY + record.heightRange, record.maxZ};
            // Coarse culling uses the page's world bounds expanded by the tallest
            // species in the region: wind sway must not be able to push a plant out of
            // the box the culler tested.
            float tallest = 0;
            for (const auto& species : entry.file->species()) tallest = std::max(tallest, species.heightMetres);
            maximum[1] += tallest * 0.35f;
            if (!frustum.intersectsAabb(minimum, maximum)) continue;
            const auto& target = slots_[entry.firstSlot + page];
            const bool resident = target.state == PageState::Resident;
            if (resident) {
                // Page centre distance: pages are at most a few tens of metres across, so
                // the centre is a sound distance proxy for the whole page.
                const float centreX = (record.minX + record.maxX) * 0.5f;
                const float centreZ = (record.minZ + record.maxZ) * 0.5f;
                const float dx = centreX - camera.x, dz = centreZ - camera.z;
                if (dx * dx + dz * dz > maxDistanceSquared) continue;
            }
            VisiblePage visible;
            visible.region = region;
            visible.page = page;
            visible.resident = resident;
            if (!resident) {
                // Not resident: counted as a page fault and drawn with the fallback
                // representation. It is never counted as a visible instance.
                ++stats_.pageFaults;
                result.push_back(visible);
                continue;
            }
            std::uint32_t visibleCount = 0;
            for (const auto& packed : target.instances) {
                const auto decoded = decodeInstance(packed, record);
                if (const auto* override = findOverride(packed.stableId)) {
                    if (override->removed) continue;
                }
                const float halfWidth = entry.file->species()[packed.species].radiusMetres * decoded.scale;
                const float height = entry.file->species()[packed.species].heightMetres * decoded.scale;
                float instanceMin[3] = {decoded.position.x - halfWidth, decoded.position.y, decoded.position.z - halfWidth};
                float instanceMax[3] = {decoded.position.x + halfWidth, decoded.position.y + height, decoded.position.z + halfWidth};
                if (!frustum.intersectsAabb(instanceMin, instanceMax)) {
                    // Outside the camera frustum but inside the shadow range: it still
                    // contributes a shadow, which is a separate counter.
                    ++stats_.shadowOnlyInstances;
                    continue;
                }
                if (hzb && hzb->valid()) {
                    float nearest = 0;
                    const auto rect = nativegpu::DepthPyramid::projectBounds(viewProjection, instanceMin, instanceMax,
                                                                             viewportWidth, viewportHeight, &nearest);
                    if (hzb->occluded(rect, nearest)) continue;
                }
                ++visibleCount;
            }
            visible.candidateInstances = static_cast<std::uint32_t>(target.instances.size());
            stats_.candidateInstances += target.instances.size();
            stats_.visibleInstances += visibleCount;
            result.push_back(visible);
        }
    }
    return result;
}

void World::setOverride(std::uint32_t stableId, InstanceOverride override) { overrides_[stableId] = override; }
bool World::hasOverride(std::uint32_t stableId) const { return overrides_.count(stableId) != 0; }
const InstanceOverride* World::findOverride(std::uint32_t stableId) const {
    const auto found = overrides_.find(stableId);
    return found == overrides_.end() ? nullptr : &found->second;
}

const std::vector<PackedInstance>* World::pageInstances(std::uint32_t region, std::uint32_t page) const {
    const auto& target = slot(region, page);
    return target.state == PageState::Resident ? &target.instances : nullptr;
}

PageState World::pageState(std::uint32_t region, std::uint32_t page) const { return slot(region, page).state; }

const std::vector<SpeciesRef>& World::species(std::uint32_t region) const {
    if (region >= regions_.size()) throw Error("NG-E-VEG-SPECIES", "region index outside the world");
    return regions_[region].file->species();
}

} // namespace engine::vegetation

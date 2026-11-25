#include "world.hpp"
#include "../determinism.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace engine::fields {
namespace {
std::int32_t floorBrick(std::int32_t value) {
    return value / BrickDim - (value % BrickDim < 0 ? 1 : 0);
}
bool has(const std::vector<ChannelId>& values, ChannelId channel) {
    return std::binary_search(values.begin(), values.end(), channel);
}
}

bool Region::contains(Cell c) const {
    return c.x >= min.x && c.x < max.x && c.y >= min.y && c.y < max.y &&
           c.z >= min.z && c.z < max.z;
}

bool LeaseRequest::storageRegion(Region& out) const {
    if (haloCells > 1 || (writable && haloCells) || region.min.x>=region.max.x ||
        region.min.y>=region.max.y || region.min.z>=region.max.z) return false;
    const auto h = static_cast<std::int64_t>(haloCells);
    const std::int64_t low[] = {region.min.x-h, region.min.y-h, region.min.z-h};
    const std::int64_t high[] = {region.max.x+h, region.max.y+h, region.max.z+h};
    for (unsigned i=0; i<3; ++i)
        if (low[i] < std::numeric_limits<std::int32_t>::min() ||
            high[i] > std::numeric_limits<std::int32_t>::max() || low[i] >= high[i]) return false;
    out = {{static_cast<std::int32_t>(low[0]),static_cast<std::int32_t>(low[1]),static_cast<std::int32_t>(low[2])},
           {static_cast<std::int32_t>(high[0]),static_cast<std::int32_t>(high[1]),static_cast<std::int32_t>(high[2])}};
    return true;
}

World::World(std::size_t maxPages, std::size_t maxTiles)
    : maxPhysicalPages_(maxPages), maxSourceTiles_(maxTiles) {
    // This bound also guarantees int32 source sums fit signed int64 exactly.
    if (maxPages == 0 || maxTiles == 0 || maxTiles > 1000000 || maxPages > 1000000)
        throw std::invalid_argument("NG-E-FIELD-BUDGET: invalid capacity");
}

ChannelId World::registerChannel(Channel c) {
    if (c.name.empty() || c.unit.empty() || c.owner.empty() || c.minimum > c.maximum ||
        c.unitsPerInteger == 0 || c.name.size() > 128 || c.owner.size() > 128 || c.unit.size() > 64)
        throw std::invalid_argument("NG-E-FIELD-CHANNEL: invalid descriptor");
    if (channels_.size() >= 256) throw std::length_error("NG-E-FIELD-CHANNEL: registry full");
    for (const auto& existing : channels_) if (existing.name == c.name)
        throw std::invalid_argument("NG-E-FIELD-CHANNEL: duplicate name");
    channels_.push_back(std::move(c));
    return static_cast<ChannelId>(channels_.size() - 1);
}

const Channel& World::channel(ChannelId id) const {
    if (id >= channels_.size()) throw std::out_of_range("NG-E-FIELD-CHANNEL: unknown id");
    return channels_[id];
}

World::PageKey World::pageKey(ChannelId channel, Cell c) {
    return {channel, {floorBrick(c.x), floorBrick(c.y), floorBrick(c.z)}};
}

std::size_t World::localIndex(Cell c) {
    const auto local = [](std::int32_t v) { return static_cast<std::size_t>((v % BrickDim + BrickDim) % BrickDim); };
    return local(c.x) + BrickDim * (local(c.y) + BrickDim * local(c.z));
}

Acquisition World::acquire(LeaseRequest request) {
    auto reject = [](const char* error) { return Acquisition{false, {}, error, 0}; };
    Region coverage;
    if (request.owner.empty() || request.owner.size() > 128 || request.channels.empty() ||
        request.region.min.x >= request.region.max.x || request.region.min.y >= request.region.max.y ||
        request.region.min.z >= request.region.max.z || request.maxPages == 0)
        return reject("NG-E-FIELD-LEASE: invalid region or owner");
    if (!request.storageRegion(coverage)) return reject("NG-E-FIELD-LEASE: invalid halo or coordinate overflow");
    std::sort(request.channels.begin(), request.channels.end());
    if (std::adjacent_find(request.channels.begin(), request.channels.end()) != request.channels.end())
        return reject("NG-E-FIELD-LEASE: duplicate channels");
    for (auto id : request.channels) {
        if (id >= channels_.size()) return reject("NG-E-FIELD-LEASE: unknown channel");
        if (request.writable && request.owner != channels_[id].owner)
            return reject("NG-E-FIELD-LEASE: writer is not channel owner");
    }
    const auto a = pageKey(0, coverage.min).brick;
    const auto b = pageKey(0, {coverage.max.x - 1, coverage.max.y - 1, coverage.max.z - 1}).brick;
    std::size_t count = request.channels.size();
    const std::uint64_t dimensions[] = {
        static_cast<std::uint64_t>(static_cast<std::int64_t>(b.x) - a.x + 1),
        static_cast<std::uint64_t>(static_cast<std::int64_t>(b.y) - a.y + 1),
        static_cast<std::uint64_t>(static_cast<std::int64_t>(b.z) - a.z + 1)};
    for (auto dimension : dimensions) {
        if (dimension > request.maxPages / count || dimension > maxPhysicalPages_ / count)
            return reject("NG-E-FIELD-BUDGET: region exceeds page budget");
        count *= static_cast<std::size_t>(dimension);
    }
    std::vector<PageKey> keys;
    keys.reserve(count);
    std::size_t fresh = 0;
    for (auto id : request.channels)
        for (std::int64_t x = a.x; x <= b.x; ++x)
            for (std::int64_t y = a.y; y <= b.y; ++y)
                for (std::int64_t z = a.z; z <= b.z; ++z) {
                    PageKey key{id, {static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),static_cast<std::int32_t>(z)}};
                    keys.push_back(key);
                    if (pages_.find(key) == pages_.end()) ++fresh;
                }
    if (fresh > maxPhysicalPages_ - pages_.size() - retired_.size())
        return reject("NG-E-FIELD-BUDGET: physical pages (including in-flight pages) exhausted");
    if (fresh >= std::numeric_limits<std::uint64_t>::max() - nextPageGeneration_)
        return reject("NG-E-FIELD-GENERATION: capacity exceeded");
    std::size_t slot = 0;
    while (slot < leases_.size() && (leases_[slot].active || leases_[slot].generation == 0)) ++slot;
    if (slot >= 65536) return reject("NG-E-FIELD-LEASE: handle capacity exceeded");
    if (slot == leases_.size()) leases_.emplace_back();
    auto& entry = leases_[slot];
    entry.request = std::move(request);
    entry.pages = std::move(keys);
    for (const auto& key : entry.pages) {
        auto& page = pages_[key];
        if (!page) {
            page = std::make_shared<Page>();
            page->generation = nextPageGeneration_++;
        }
        ++page->references;
    }
    entry.active = true;
    return {true, {static_cast<std::uint32_t>(slot), entry.generation}, {}, count};
}

const World::LeaseSlot* World::findLease(Lease lease) const {
    if (lease.slot >= leases_.size()) return nullptr;
    const auto& slot = leases_[lease.slot];
    return slot.active && lease.generation == slot.generation ? &slot : nullptr;
}

bool World::release(Lease lease, std::uint64_t fence) {
    if (!findLease(lease)) return false;
    auto& entry = leases_[lease.slot];
    retired_.reserve(retired_.size() + entry.pages.size());
    for (const auto& key : entry.pages) {
        auto found = pages_.find(key);
        if (found != pages_.end()) found->second->lastUseFence = std::max(found->second->lastUseFence, fence);
        if (found != pages_.end() && --found->second->references == 0) {
            retired_.push_back({found->second, found->second->lastUseFence});
            pages_.erase(found);
        }
    }
    entry.active = false;
    // Generation zero is permanently retired instead of wrapping to a valid old handle.
    entry.generation = entry.generation == std::numeric_limits<std::uint32_t>::max() ? 0 : entry.generation + 1;
    entry.request = {};
    entry.pages.clear();
    return true;
}

void World::collect(std::uint64_t completedFence) {
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [&](const Retired& page) { return page.fence <= completedFence; }), retired_.end());
    // Metadata is bounded too. Once no active page retains the source, an old
    // lease cannot resurrect it: the lease generation is already invalid.
    for (auto source = sources_.begin(); source != sources_.end();) {
        // A different emitter sharing the same subsystem owner must not pin
        // this source's withdrawal tombstone forever. Its own writer generation
        // is the capability that can still retransmit it.
        bool retained = findLease(source->second.writer)!=nullptr;
        for (const auto& footprint : source->second.pages) {
            auto p = pages_.find(footprint.first);
            if (p != pages_.end() && p->second->generation == footprint.second) { retained = true; break; }
        }
        // Committed metadata remains an immutable snapshot until commit().
        if (!retained) source = sources_.erase(source); else ++source;
    }
}

PublishResult World::publish(Lease writer, ChannelId id, std::uint64_t sourceId,
                            std::uint64_t epoch, Tick tick, Domain origin,
                            const std::vector<Write>& writes) {
    return publishAt({writer,id,sourceId,epoch,tick,tick,origin,writes});
}

PublishResult World::publishAt(const Publication& publication) {
    const auto writer=publication.writer;
    const auto id=publication.channel;
    const auto sourceId=publication.source,epoch=publication.epoch;
    const auto tick=publication.sourceTick,publishTick=publication.publishTick;
    const auto origin=publication.origin;
    const auto& writes=publication.writes;
    const auto reject = [&](PublishResult reason) { ++rejections_; return reason; };
    const auto* lease = findLease(writer);
    if (!lease || id >= channels_.size() || sourceId == 0 || epoch == 0 || tick>publishTick)
        return reject(PublishResult::Invalid);
    const auto& spec = channels_[id];
    if (!lease->request.writable || !has(lease->request.channels, id) ||
        lease->request.owner != spec.owner || origin != spec.domain)
        return reject(PublishResult::AccessDenied);
    // Canonicalization and all semantic/capacity checks precede any mutation.
    if (writes.size() > lease->pages.size() * BrickVoxels)
        return reject(PublishResult::CapacityExceeded);
    auto canonical = writes;
    std::sort(canonical.begin(), canonical.end(), [](const Write& a, const Write& b) { return a.cell < b.cell; });
    determinism::Hasher fingerprint;
    fingerprint.u64(tick);
    fingerprint.u64(publishTick);
    std::map<PageKey, Tile> tiles;
    Cell previous{};
    bool first = true;
    for (const auto& write : canonical) {
        if (!lease->request.region.contains(write.cell) || write.value < spec.minimum || write.value > spec.maximum ||
            (!first && write.cell == previous)) return reject(PublishResult::Invalid);
        first = false;
        previous = write.cell;
        fingerprint.i32(write.cell.x); fingerprint.i32(write.cell.y); fingerprint.i32(write.cell.z);
        fingerprint.i32(write.value);
        auto& tile = tiles[pageKey(id, write.cell)];
        const auto index = localIndex(write.cell);
        tile.values[index] = write.value;
        tile.known.set(index);
        tile.sourceTick = tick;
    }
    const auto sourceKey = std::make_pair(id, sourceId);
    auto old = sources_.find(sourceKey);
    std::size_t replacedTiles = 0;
    if (old != sources_.end()) {
        if (old->second.owner != lease->request.owner) return reject(PublishResult::AccessDenied);
        if (epoch < old->second.epoch) return reject(PublishResult::OldEpoch);
        if (epoch == old->second.epoch) {
            if (fingerprint.value() != old->second.fingerprint) return reject(PublishResult::Conflict);
            for (const auto& footprint : old->second.pages) {
                auto p = pages_.find(footprint.first);
                if (p == pages_.end() || p->second->generation != footprint.second)
                    return reject(PublishResult::ResourceExpired);
            }
            ++duplicates_;
            return PublishResult::Duplicate;
        }
        if (tick < old->second.tick || publishTick < old->second.publishTick) return reject(PublishResult::PastTick);
        for (const auto& footprint : old->second.pages) {
            auto p = pages_.find(footprint.first);
            if (p != pages_.end() && p->second->generation == footprint.second &&
                p->second->staging.count(sourceId)) ++replacedTiles;
        }
    }
    if (hasCommit_ && publishTick <= committedTick_) return reject(PublishResult::PastTick);
    const auto allocated = stats().sourceTiles;
    if (tiles.size() > maxSourceTiles_ - (allocated - replacedTiles) ||
        (old == sources_.end() && sources_.size() >= maxSourceTiles_))
        return reject(PublishResult::CapacityExceeded);
    // A replacement must control the old footprint as well as the new one.
    // This prevents a narrow regional writer silently withdrawing another region.
    if (old != sources_.end()) for (const auto& footprint : old->second.pages) {
        if (std::none_of(lease->pages.begin(), lease->pages.end(), [&](const PageKey& key) {
            return !(key < footprint.first) && !(footprint.first < key);
        })) return reject(PublishResult::AccessDenied);
        auto p = pages_.find(footprint.first);
        if (p != pages_.end() && p->second->generation == footprint.second) {
            auto tile = p->second->staging.find(sourceId);
            if (tile != p->second->staging.end()) for (std::size_t i = 0; i < BrickVoxels; ++i) {
                if (!tile->second.known[i]) continue;
                const auto& b = footprint.first.brick;
                const Cell cell{b.x * BrickDim + static_cast<std::int32_t>(i % BrickDim),
                    b.y * BrickDim + static_cast<std::int32_t>((i / BrickDim) % BrickDim),
                    b.z * BrickDim + static_cast<std::int32_t>(i / (BrickDim * BrickDim))};
                if (!lease->request.region.contains(cell)) return reject(PublishResult::AccessDenied);
            }
        }
    }
    Source next{lease->request.owner, writer, epoch, fingerprint.value(), tick, publishTick, {}};
    for (const auto& tile : tiles) {
        const auto p = pages_.find(tile.first);
        if (p == pages_.end()) return reject(PublishResult::ResourceExpired);
        next.pages.push_back({tile.first, p->second->generation});
    }
    if (old != sources_.end()) for (const auto& footprint : old->second.pages) {
        auto p = pages_.find(footprint.first);
        if (p != pages_.end() && p->second->generation == footprint.second) p->second->staging.erase(sourceId);
    }
    for (auto& tile : tiles) pages_.at(tile.first)->staging[sourceId] = std::move(tile.second);
    sources_[sourceKey] = std::move(next);
    ++publications_;
    return PublishResult::Accepted;
}

PublishResult World::publishAtomic(const std::vector<Publication>& publications) {
    if (publications.empty() || publications.size()>512) { ++rejections_; return PublishResult::Invalid; }
    World draft(maxPhysicalPages_,maxSourceTiles_);
    draft.channels_=channels_;draft.leases_=leases_;draft.retired_=retired_;
    draft.sources_=sources_;draft.committedSources_=committedSources_;
    draft.nextPageGeneration_=nextPageGeneration_;draft.version_=version_;
    draft.committedTick_=committedTick_;draft.hasCommit_=hasCommit_;
    draft.publications_=publications_;draft.duplicates_=duplicates_;draft.rejections_=rejections_;
    for(const auto& page:pages_)draft.pages_.emplace(page.first,std::make_shared<Page>(*page.second));
    bool changed=false;
    for(const auto& publication:publications) {
        const auto result=draft.publishAt(publication);
        if(result!=PublishResult::Accepted&&result!=PublishResult::Duplicate){++rejections_;return result;}
        changed|=result==PublishResult::Accepted;
    }
    pages_.swap(draft.pages_);sources_.swap(draft.sources_);
    publications_=draft.publications_;duplicates_=draft.duplicates_;
    return changed?PublishResult::Accepted:PublishResult::Duplicate;
}

bool World::commit(Tick tick) {
    if ((hasCommit_ && tick <= committedTick_) || version_ == std::numeric_limits<std::uint64_t>::max()) return false;
    for (const auto& source:sources_) if(source.second.publishTick>tick)return false;
    // Copy first, swap second: readers cannot observe half a committed snapshot
    // even if allocating the CPU reference snapshot throws.
    std::vector<std::pair<Page*, std::map<std::uint64_t, Tile>>> snapshots;
    snapshots.reserve(pages_.size());
    auto sourceSnapshot=sources_;
    for (auto& p : pages_) snapshots.push_back({p.second.get(), p.second->staging});
    for (auto& snapshot : snapshots) snapshot.first->committed.swap(snapshot.second);
    committedSources_.swap(sourceSnapshot);
    committedTick_ = tick;
    hasCommit_ = true;
    ++version_;
    return true;
}

Sample World::sample(Lease reader, ChannelId id, Cell cell, Tick requestedTick) const {
    return sampleImpl(reader,id,cell,requestedTick,nullptr);
}
Sample World::sampleSource(Lease reader,ChannelId id,std::uint64_t source,Cell cell,Tick requestedTick) const {
    return sampleImpl(reader,id,cell,requestedTick,&source);
}
Sample World::sampleImpl(Lease reader, ChannelId id, Cell cell, Tick requestedTick,const std::uint64_t* requestedSource,bool includeHalo) const {
    Sample result;
    result.snapshotTick = committedTick_;
    result.version = version_;
    const auto* lease = findLease(reader);
    if (!lease) { result.status = Status::InvalidLease; return result; }
    if (id >= channels_.size()) { result.status = Status::UnknownChannel; return result; }
    if (!has(lease->request.channels, id)) { result.status = Status::AccessDenied; return result; }
    Region coverage=lease->request.region;
    if (includeHalo) lease->request.storageRegion(coverage);
    if (!coverage.contains(cell)) { result.status = Status::OutsideRegion; return result; }
    if (!hasCommit_ || requestedTick < committedTick_ ||
        (lease->request.consistency == Consistency::Strong && requestedTick != committedTick_)) {
        result.status = Status::TickMismatch; return result;
    }
    const auto page = pages_.find(pageKey(id, cell));
    if (page == pages_.end()) return result;
    const auto index = localIndex(cell);
    std::int64_t value = 0;
    bool known = false;
    Tick oldest = std::numeric_limits<Tick>::max();
    for (const auto& source : page->second->committed) {
        if(requestedSource&&source.first!=*requestedSource)continue;
        const auto& tile = source.second;
        if (!tile.known[index]) continue;
        const auto contribution = tile.values[index];
        if (channels_[id].merge == Merge::Sum) value += contribution;
        else if (!known || contribution > value) value = contribution;
        known = true;
        oldest = std::min(oldest, tile.sourceTick);
    }
    if (!known) return result; // Unknown never silently becomes a zero hazard.
    const auto& spec = channels_[id];
    result.clamped = value < spec.minimum || value > spec.maximum;
    result.value = static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(spec.minimum),
                                                      static_cast<std::int64_t>(spec.maximum)));
    result.sourceTick = oldest;
    if(requestedSource) {
        const auto info=sourceView(id,*requestedSource);
        result.sourceEpoch=info.epoch;
    }
    result.ageTicks = requestedTick - oldest;
    const auto maxAge = lease->request.consistency == Consistency::Strong ? 0 : lease->request.maxAgeTicks;
    result.status = lease->request.consistency != Consistency::Stale && result.ageTicks > maxAge ? Status::Stale : Status::Ok;
    return result;
}

Sample World::sampleLinear(Lease reader,ChannelId id,SamplePosition position,Tick tick) const {
    return sampleLinearImpl(reader,id,position,tick,nullptr);
}
Sample World::sampleSourceLinear(Lease reader,ChannelId id,std::uint64_t source,SamplePosition position,Tick tick) const {
    return sampleLinearImpl(reader,id,position,tick,&source);
}
Sample World::sampleLinearImpl(Lease reader,ChannelId id,SamplePosition p,Tick tick,const std::uint64_t* source) const {
    Sample result;result.snapshotTick=committedTick_;result.version=version_;
    const auto* lease=findLease(reader);
    if (!lease) {result.status=Status::InvalidLease;return result;}
    if (id>=channels_.size()) {result.status=Status::UnknownChannel;return result;}
    if (!has(lease->request.channels,id)) {result.status=Status::AccessDenied;return result;}
    if (!lease->request.region.contains(p.cell)) {result.status=Status::OutsideRegion;return result;}
    for (auto f:p.fraction) if (f>255) {result.status=Status::InvalidPosition;return result;}
    if (!hasCommit_ || tick<committedTick_ || (lease->request.consistency==Consistency::Strong&&tick!=committedTick_)) {
        result.status=Status::TickMismatch;return result;
    }
    Region coverage;lease->request.storageRegion(coverage);
    const Cell last{p.cell.x+(p.fraction[0]!=0),p.cell.y+(p.fraction[1]!=0),p.cell.z+(p.fraction[2]!=0)};
    if (!coverage.contains(last)) {result.status=Status::OutsideRegion;return result;}
    std::int64_t sum=0;Tick oldest=std::numeric_limits<Tick>::max();bool clamped=false;
    for (unsigned z=0;z<2;++z) for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x) {
        const auto weight=(x?p.fraction[0]:256-p.fraction[0])*(y?p.fraction[1]:256-p.fraction[1])*(z?p.fraction[2]:256-p.fraction[2]);
        if (!weight) continue; // An exact lattice query never requires absent zero-weight neighbours.
        const auto value=sampleImpl(reader,id,{p.cell.x+static_cast<int>(x),p.cell.y+static_cast<int>(y),p.cell.z+static_cast<int>(z)},tick,source,true);
        if (value.status!=Status::Ok&&value.status!=Status::Stale) {result.status=value.status;return result;}
        sum+=static_cast<std::int64_t>(value.value)*weight;oldest=std::min(oldest,value.sourceTick);clamped|=value.clamped;
    }
    // One rounding after all eight products; signed ties go away from zero.
    constexpr std::int64_t half=1<<23,denominator=1<<24;
    result.value=static_cast<std::int32_t>(sum<0?-((-sum+half)/denominator):(sum+half)/denominator);
    result.sourceTick=oldest;result.ageTicks=tick-oldest;result.clamped=clamped;
    if (source) result.sourceEpoch=sourceView(id,*source).epoch;
    const auto maxAge=lease->request.consistency==Consistency::Strong?0:lease->request.maxAgeTicks;
    result.status=lease->request.consistency!=Consistency::Stale&&result.ageTicks>maxAge?Status::Stale:Status::Ok;
    return result;
}

SourceView World::sourceView(ChannelId id,std::uint64_t source) const {
    SourceView result;
    result.snapshotTick=committedTick_;result.version=version_;
    const auto found=committedSources_.find({id,source});
    if(found==committedSources_.end())return result;
    result.exists=true;result.resident=true;result.epoch=found->second.epoch;
    result.sourceTick=found->second.tick;result.publishTick=found->second.publishTick;
    result.hasContribution=!found->second.pages.empty();
    for(const auto& footprint:found->second.pages) {
        const auto page=pages_.find(footprint.first);
        if(page==pages_.end()||page->second->generation!=footprint.second||!page->second->committed.count(source)) {
            result.resident=false;break;
        }
    }
    return result;
}

bool World::sourcePresent(ChannelId id,std::uint64_t source) const {
    return sources_.find({id,source})!=sources_.end();
}

Stats World::stats() const {
    Stats result;
    result.activePages = pages_.size();
    result.sourceRecords=sources_.size();
    result.retiredPages = retired_.size();
    for (const auto& lease : leases_) if (lease.active) ++result.leases;
    auto count = [&](const Page& page) {
        result.sourceTiles += page.staging.size();
        // CPU payload bytes only; allocator/map bookkeeping is not GPU VRAM.
        result.committedBytes += sizeof(Page) + sizeof(Tile) * (page.staging.size() + page.committed.size());
    };
    for (const auto& page : pages_) count(*page.second);
    for (const auto& page : retired_) count(*page.page);
    result.publications = publications_; result.duplicates = duplicates_; result.rejections = rejections_;
    return result;
}

std::uint64_t World::digest() const {
    determinism::Hasher hash;
    hash.text("nativegpu.field-cpu-reference/2");
    hash.u8(hasCommit_ ? 1 : 0);
    hash.u64(committedTick_);
    for (ChannelId id = 0; id < channels_.size(); ++id) {
        const auto& spec = channels_[id];
        if (spec.domain != Domain::CpuAuthority) continue;
        hash.u32(id); hash.text(spec.name); hash.text(spec.unit); hash.text(spec.owner);
        hash.u32(spec.unitsPerInteger); hash.i32(spec.minimum); hash.i32(spec.maximum);
        hash.u32(static_cast<std::uint32_t>(spec.merge));
        for(const auto& source:committedSources_)if(source.first.first==id) {
            hash.u64(source.first.second);hash.u64(source.second.epoch);
            hash.u64(source.second.tick);hash.u64(source.second.publishTick);
        }
        hash.u64(0);
        for (const auto& p : pages_) {
            if (p.first.channel != id || p.second->committed.empty()) continue;
            hash.i32(p.first.brick.x); hash.i32(p.first.brick.y); hash.i32(p.first.brick.z);
            for (const auto& source : p.second->committed) {
                hash.u64(source.first); hash.u64(source.second.sourceTick);
                for (std::size_t i = 0; i < BrickVoxels; ++i) if (source.second.known[i]) {
                    hash.u32(static_cast<std::uint32_t>(i)); hash.i32(source.second.values[i]);
                }
                hash.u32(static_cast<std::uint32_t>(BrickVoxels));
            }
            hash.u64(0); // source 0 is reserved, so the page terminator is unambiguous.
        }
        hash.u32(std::numeric_limits<std::uint32_t>::max());
    }
    return hash.value();
}

} // namespace engine::fields

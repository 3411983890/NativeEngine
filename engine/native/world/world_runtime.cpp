#include "world_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace engine {
namespace world {

const char* chunkStateName(ChunkState state) {
    switch (state) {
        case ChunkState::Unloaded: return "unloaded";
        case ChunkState::Requested: return "requested";
        case ChunkState::Reading: return "reading";
        case ChunkState::Validated: return "validated";
        case ChunkState::CpuReady: return "cpuReady";
        case ChunkState::Uploading: return "uploading";
        case ChunkState::Resident: return "resident";
        case ChunkState::Retiring: return "retiring";
    }
    return "unknown";
}

DirectoryChunkSource::DirectoryChunkSource(std::string root) : root_(std::move(root)) {}

bool DirectoryChunkSource::read(ChunkCoord coord, std::vector<std::uint8_t>& bytes, std::string& error) {
    char name[64];
    std::snprintf(name, sizeof(name), "/chunks/%d_%d.ngrc", coord.x, coord.z);
    const std::string path = root_ + name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { error = "cannot open " + path; return false; }
    const std::streamoff size = stream.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(Limits::kMaxChunkBytes)) {
        error = "chunk file has an unusable size";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) { error = "chunk file read failed"; return false; }
    return true;
}

WorldRuntime::WorldRuntime() = default;
WorldRuntime::~WorldRuntime() = default;

bool WorldRuntime::configure(const RuntimeConfig& config, std::string& error) {
    if (!(config.chunkSizeMetres >= 16.0f && config.chunkSizeMetres <= 512.0f)) {
        error = "chunk size must be within [16, 512] metres";
        return false;
    }
    if (!(config.sampleStepMetres >= 0.25f && config.sampleStepMetres <= 8.0f)) {
        error = "sample step must be within [0.25, 8] metres";
        return false;
    }
    if (config.maxResidentChunks <= 0 || config.maxResidentChunks > 65536) {
        error = "maxResidentChunks must be in [1, 65536]";
        return false;
    }
    if (!(config.nearDistanceMetres > 0.0f) || !(config.midDistanceMetres >= config.nearDistanceMetres)
        || !(config.farDistanceMetres >= config.midDistanceMetres)) {
        error = "LOD distances must be positive and non-decreasing";
        return false;
    }
    config_ = config;
    chunks_.clear();
    index_.clear();
    retiring_.clear();
    bytesResident_ = 0;
    return true;
}

void WorldRuntime::setManifestBounds(Bounds2 bounds) { worldBounds_ = bounds; }

void WorldRuntime::setSource(ChunkSource* source) { source_ = source; }

void WorldRuntime::reset(std::uint64_t newEpoch) {
    epoch_ = newEpoch == 0 ? epoch_ + 1 : newEpoch;
    chunks_.clear();
    index_.clear();
    retiring_.clear();
    bytesResident_ = 0;
    dataVersion_ += 1;
}

ChunkRecord* WorldRuntime::find(ChunkCoord coord) {
    const auto it = index_.find(chunkKeyOf(coord));
    if (it == index_.end()) return nullptr;
    return &chunks_[it->second];
}

const ChunkRecord* WorldRuntime::find(ChunkCoord coord) const {
    const auto it = index_.find(chunkKeyOf(coord));
    if (it == index_.end()) return nullptr;
    return &chunks_[it->second];
}

ChunkRecord& WorldRuntime::touch(ChunkCoord coord) {
    const std::int64_t key = chunkKeyOf(coord);
    const auto it = index_.find(key);
    if (it != index_.end()) return chunks_[it->second];
    ChunkRecord record;
    record.coord = coord;
    record.state = ChunkState::Unloaded;
    record.epoch = epoch_;
    chunks_.push_back(record);
    index_[key] = chunks_.size() - 1;
    return chunks_.back();
}

std::uint32_t WorldRuntime::lodFor(float distance) const {
    if (distance <= config_.nearDistanceMetres) return 0;
    if (distance <= config_.midDistanceMetres) return 1;
    return 2;
}

void WorldRuntime::loadRecord(ChunkRecord& record) {
    if (!source_) { record.state = ChunkState::Unloaded; return; }
    record.state = ChunkState::Reading;
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!source_->read(record.coord, bytes, error)) {
        record.state = ChunkState::Unloaded;
        stats_.loadErrors += 1;
        lastError_ = error;
        return;
    }
    record.state = ChunkState::Validated;
    auto data = std::make_shared<ChunkData>();
    if (!deserializeChunk(bytes.data(), bytes.size(), *data, error)) {
        record.state = ChunkState::Unloaded;
        stats_.loadErrors += 1;
        lastError_ = error;
        return;
    }
    if (data->coord != record.coord) {
        // A file named for one chunk that declares another is a corrupt cook, not a
        // recoverable condition.
        record.state = ChunkState::Unloaded;
        stats_.loadErrors += 1;
        lastError_ = "chunk file does not match its requested coordinates";
        return;
    }
    if (data->lod > record.lod) record.lod = data->lod;
    record.bytes = bytes.size();
    record.data = std::move(data);
    record.state = ChunkState::CpuReady;
    record.lastUsedFrame = frame_;
    stats_.chunksLoadedTotal += 1;
}

void WorldRuntime::evictFarthest() {
    ChunkRecord* victim = nullptr;
    for (auto& record : chunks_) {
        if (record.state != ChunkState::CpuReady && record.state != ChunkState::Resident) continue;
        if (!victim || record.distanceMetres > victim->distanceMetres) victim = &record;
    }
    if (!victim) return;
    if (victim->state == ChunkState::Resident) {
        // Still possibly in flight on the GPU. Park it in retiring; the renderer releases it
        // after its fence, and only then does the memory come back.
        victim->state = ChunkState::Retiring;
        retiring_.push_back(victim->coord);
    } else {
        if (bytesResident_ >= victim->bytes) bytesResident_ -= victim->bytes;
        victim->state = ChunkState::Unloaded;
        victim->data.reset();
        victim->bytes = 0;
    }
    stats_.chunksEvictedTotal += 1;
}

void WorldRuntime::pump(float cameraX, float cameraZ, int maxLoads) {
    const auto start = std::chrono::steady_clock::now();
    frame_ += 1;
    stats_.frames = frame_;

    if (!worldBounds_.valid()) { stats_.lastPumpMillis = 0.0; return; }

    const float chunkSize = config_.chunkSizeMetres;
    const ChunkRange range = chunkRangeForBounds(worldBounds_, chunkSize);

    // Distance to the camera per chunk, using the nearest point on the chunk rectangle so a
    // chunk the camera stands in is always at distance 0.
    std::vector<std::pair<float, ChunkCoord>> candidates;
    candidates.reserve(static_cast<std::size_t>(range.count()));
    for (std::int32_t cz = range.minCz; cz <= range.maxCz; ++cz) {
        for (std::int32_t cx = range.minCx; cx <= range.maxCx; ++cx) {
            const Bounds2 bounds = chunkBoundsOf({cx, cz}, chunkSize);
            const float dx = std::max({bounds.minX - cameraX, 0.0f, cameraX - bounds.maxX});
            const float dz = std::max({bounds.minZ - cameraZ, 0.0f, cameraZ - bounds.maxZ});
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > config_.farDistanceMetres) continue;
            candidates.emplace_back(distance, ChunkCoord{cx, cz});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  if (a.second.z != b.second.z) return a.second.z < b.second.z;
                  return a.second.x < b.second.x;
              });

    int loads = 0;
    for (const auto& candidate : candidates) {
        ChunkRecord& record = touch(candidate.second);
        record.distanceMetres = candidate.first;
        const std::uint32_t desiredLod = lodFor(candidate.first);
        if (record.state == ChunkState::CpuReady || record.state == ChunkState::Resident
            || record.state == ChunkState::Uploading) {
            record.lastUsedFrame = frame_;
            continue;
        }
        if (record.state == ChunkState::Reading) continue;
        if (loads >= maxLoads) { record.state = ChunkState::Requested; record.epoch = epoch_; continue; }
        record.lod = desiredLod;
        record.epoch = epoch_;
        loadRecord(record);
        if (record.state == ChunkState::CpuReady) {
            bytesResident_ += record.bytes;
            loads += 1;
        }
        while (chunks_.size() > static_cast<std::size_t>(config_.maxResidentChunks)
               || bytesResident_ > config_.memoryBudgetBytes) {
            const std::size_t before = chunks_.size();
            evictFarthest();
            if (chunks_.size() == before && bytesResident_ <= config_.memoryBudgetBytes) break;
            if (bytesResident_ <= config_.memoryBudgetBytes
                && chunks_.size() <= static_cast<std::size_t>(config_.maxResidentChunks)) break;
        }
    }

    // Anything beyond the far band is evicted, including records that were only requested.
    // The distance is recomputed from the record's own bounds: a chunk that dropped out of
    // the candidate set never had its distance refreshed, and trusting the stale value is
    // what keeps a teleported player's old chunks resident forever.
    for (auto& record : chunks_) {
        if (record.state == ChunkState::Unloaded) continue;
        const Bounds2 bounds = chunkBoundsOf(record.coord, chunkSize);
        const float dx = std::max({bounds.minX - cameraX, 0.0f, cameraX - bounds.maxX});
        const float dz = std::max({bounds.minZ - cameraZ, 0.0f, cameraZ - bounds.maxZ});
        record.distanceMetres = std::sqrt(dx * dx + dz * dz);
        if (record.distanceMetres <= config_.farDistanceMetres) continue;
        if (record.state == ChunkState::Resident) {
            record.state = ChunkState::Retiring;
            retiring_.push_back(record.coord);
        } else if (record.state != ChunkState::Retiring) {
            if (bytesResident_ >= record.bytes) bytesResident_ -= record.bytes;
            record.state = ChunkState::Unloaded;
            record.data.reset();
            record.bytes = 0;
        }
        stats_.chunksEvictedTotal += 1;
    }

    const auto end = std::chrono::steady_clock::now();
    stats_.lastPumpMillis = std::chrono::duration<double, std::milli>(end - start).count();
}

SurfaceQuery WorldRuntime::querySurface(float x, float z) const {
    SurfaceQuery out;
    if (!worldBounds_.valid() || !worldBounds_.contains(x, z)) return out;
    const float chunkSize = config_.chunkSizeMetres;
    const std::int32_t cx = chunkIndexForMetres(x, chunkSize);
    const std::int32_t cz = chunkIndexForMetres(z, chunkSize);
    const ChunkRecord* record = find(ChunkCoord{cx, cz});
    if (!record || !record->data) return out;
    if (record->state != ChunkState::CpuReady && record->state != ChunkState::Resident
        && record->state != ChunkState::Uploading) {
        return out;
    }
    const ChunkData& chunk = *record->data;
    if (chunk.vertexCount < 2) return out;

    const float step = chunk.chunkSizeMetres / static_cast<float>(chunk.vertexCount - 1);
    const float localX = x - chunk.bounds.minX;
    const float localZ = z - chunk.bounds.minZ;
    const float fx = std::max(0.0f, std::min(static_cast<float>(chunk.vertexCount - 1), localX / step));
    const float fz = std::max(0.0f, std::min(static_cast<float>(chunk.vertexCount - 1), localZ / step));
    const std::uint32_t x0 = static_cast<std::uint32_t>(fx);
    const std::uint32_t z0 = static_cast<std::uint32_t>(fz);
    const std::uint32_t x1 = std::min(chunk.vertexCount - 1, x0 + 1);
    const std::uint32_t z1 = std::min(chunk.vertexCount - 1, z0 + 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const auto indexOf = [&](std::uint32_t vx, std::uint32_t vz) { return chunk.vertexIndex(vx, vz); };
    const auto bilinear = [&](const std::vector<float>& values) {
        const float a = values[indexOf(x0, z0)] * (1.0f - tx) + values[indexOf(x1, z0)] * tx;
        const float b = values[indexOf(x0, z1)] * (1.0f - tx) + values[indexOf(x1, z1)] * tx;
        return a * (1.0f - tz) + b * tz;
    };

    out.ready = true;
    out.chunkLod = chunk.lod;
    out.dataVersion = dataVersion_;
    out.height = bilinear(chunk.heights);
    const std::uint32_t nearestIndex = indexOf(tx < 0.5f ? x0 : x1, tz < 0.5f ? z0 : z1);
    out.flags = chunk.query[nearestIndex];
    for (int channel = 0; channel < 4; ++channel) {
        out.weights[channel] = static_cast<float>(chunk.materialWeights[nearestIndex * 4 + static_cast<std::size_t>(channel)]) / 255.0f;
    }
    std::int16_t encoded[2] = {chunk.normals[nearestIndex * 2], chunk.normals[nearestIndex * 2 + 1]};
    decodeNormalOctahedral(encoded, out.normal);
    out.normalY = out.normal[1];
    const float clamped = out.normalY < 1.0e-6f ? 1.0e-6f : (out.normalY > 1.0f ? 1.0f : out.normalY);
    out.slopeDegrees = std::acos(clamped) * 180.0f / 3.14159265358979323846f;
    if ((out.flags & kQueryWater) != 0) {
        // The water surface sits at the bed plus the channel depth; the chunk stores the
        // bed, so a query has to look at the river bed mesh to recover the level. Rather
        // than store it twice, the runtime reports the terrain height and lets the water
        // pass use its own surface buffer.
        out.waterHeight = out.height;
    }
    return out;
}

bool WorldRuntime::queryHeight(float x, float z, float& height) const {
    const SurfaceQuery query = querySurface(x, z);
    if (!query.ready) return false;
    height = query.height;
    return true;
}

BuildingQuery WorldRuntime::queryBuilding(float x, float z, float radius) const {
    BuildingQuery out;
    if (!worldBounds_.valid()) return out;
    const float chunkSize = config_.chunkSizeMetres;
    const std::int32_t minCx = chunkIndexForMetres(x - radius, chunkSize);
    const std::int32_t maxCx = chunkIndexForMetres(x + radius, chunkSize);
    const std::int32_t minCz = chunkIndexForMetres(z - radius, chunkSize);
    const std::int32_t maxCz = chunkIndexForMetres(z + radius, chunkSize);
    for (std::int32_t cz = minCz; cz <= maxCz; ++cz) {
        for (std::int32_t cx = minCx; cx <= maxCx; ++cx) {
            const ChunkRecord* record = find(ChunkCoord{cx, cz});
            if (!record || !record->data) continue;
            for (const auto& building : record->data->buildings) {
                const float halfWidth = building.footprint[0] * 0.5f + radius;
                const float halfDepth = building.footprint[1] * 0.5f + radius;
                if (std::fabs(x - building.position[0]) > halfWidth) continue;
                if (std::fabs(z - building.position[2]) > halfDepth) continue;
                out.found = true;
                out.component = building.component;
                for (int i = 0; i < 3; ++i) out.position[i] = building.position[i];
                out.yaw = building.yaw;
                out.footprint[0] = building.footprint[0];
                out.footprint[1] = building.footprint[1];
                out.heightMetres = building.heightMetres;
                for (int i = 0; i < 3; ++i) out.entrance[i] = building.entrance[i];
                out.entranceConnected = building.entranceConnected != 0;
                out.stableId = building.stableId;
                return out;
            }
        }
    }
    return out;
}

bool WorldRuntime::isReady(const Bounds2& bounds) const {
    if (!worldBounds_.valid()) return false;
    const float chunkSize = config_.chunkSizeMetres;
    const ChunkRange range = chunkRangeForBounds(bounds, chunkSize);
    for (std::int32_t cz = range.minCz; cz <= range.maxCz; ++cz) {
        for (std::int32_t cx = range.minCx; cx <= range.maxCx; ++cx) {
            const ChunkRecord* record = find(ChunkCoord{cx, cz});
            if (!record || !record->data) return false;
            if (record->state != ChunkState::CpuReady && record->state != ChunkState::Resident
                && record->state != ChunkState::Uploading) {
                return false;
            }
        }
    }
    return true;
}

int WorldRuntime::prefetch(const Bounds2& bounds, std::uint32_t lod) {
    if (!worldBounds_.valid()) return 0;
    const float chunkSize = config_.chunkSizeMetres;
    const ChunkRange range = chunkRangeForBounds(bounds, chunkSize);
    int requested = 0;
    for (std::int32_t cz = range.minCz; cz <= range.maxCz; ++cz) {
        for (std::int32_t cx = range.minCx; cx <= range.maxCx; ++cx) {
            ChunkRecord& record = touch(ChunkCoord{cx, cz});
            if (record.state == ChunkState::CpuReady || record.state == ChunkState::Resident
                || record.state == ChunkState::Uploading || record.state == ChunkState::Reading) {
                continue;
            }
            record.lod = lod;
            record.epoch = epoch_;
            record.distanceMetres = 0.0f;
            loadRecord(record);
            if (record.state == ChunkState::CpuReady) {
                bytesResident_ += record.bytes;
                requested += 1;
            }
        }
    }
    return requested;
}

std::vector<std::shared_ptr<ChunkData>> WorldRuntime::beginUpload(std::size_t maxChunks) {
    std::vector<std::shared_ptr<ChunkData>> out;
    for (auto& record : chunks_) {
        if (out.size() >= maxChunks) break;
        if (record.state != ChunkState::CpuReady || !record.data) continue;
        record.state = ChunkState::Uploading;
        out.push_back(record.data);
    }
    return out;
}

void WorldRuntime::completeUpload(const std::vector<ChunkCoord>& coords) {
    for (const auto& coord : coords) {
        ChunkRecord* record = find(coord);
        if (!record) continue;
        if (record->state == ChunkState::Uploading) record->state = ChunkState::Resident;
    }
}

std::vector<ChunkCoord> WorldRuntime::takeRetired() {
    std::vector<ChunkCoord> out = retiring_;
    retiring_.clear();
    return out;
}

void WorldRuntime::releaseRetired(const std::vector<ChunkCoord>& coords) {
    for (const auto& coord : coords) {
        ChunkRecord* record = find(coord);
        if (!record) continue;
        if (bytesResident_ >= record->bytes) bytesResident_ -= record->bytes;
        record->state = ChunkState::Unloaded;
        record->data.reset();
        record->bytes = 0;
    }
}

RuntimeStats WorldRuntime::stats() const {
    RuntimeStats out = stats_;
    out.epoch = epoch_;
    out.chunksResident = 0;
    out.chunksCpuReady = 0;
    out.chunksRequested = 0;
    out.bytesResident = bytesResident_;
    out.instancesResident = 0;
    out.buildingsResident = 0;
    for (const auto& record : chunks_) {
        if (record.state == ChunkState::Resident) out.chunksResident += 1;
        if (record.state == ChunkState::CpuReady || record.state == ChunkState::Resident) {
            out.chunksCpuReady += 1;
        }
        if (record.state == ChunkState::Requested || record.state == ChunkState::Reading) out.chunksRequested += 1;
        if (record.data) {
            out.instancesResident += record.data->instances.size();
            out.buildingsResident += record.data->buildings.size();
        }
    }
    out.lastError = lastError_;
    return out;
}

} // namespace world
} // namespace engine

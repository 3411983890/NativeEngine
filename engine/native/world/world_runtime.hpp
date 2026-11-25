#pragma once

// World runtime: the part that ships.
//
// It owns chunk residency, answers spatial queries and never re-runs the generator. A
// published build has no recipe, no layers and no Node: it has a manifest, a set of chunk
// files and this class.
//
// The state machine is explicit because the interesting failures are all transitions:
//
//   unloaded -> requested -> reading -> validated -> cpuReady -> resident -> retiring -> unloaded
//
//   * A chunk is queryable from `cpuReady`; `resident` adds "the renderer has it".
//   * `retiring` exists so the GPU fence can drain before the memory is reused. Releasing a
//     chunk that is still in flight is the classic crash, and a state makes it visible.
//   * Every state carries the epoch it was requested in. A load that finishes after the
//     project switched is dropped instead of overwriting the new world.
//
// A query for a chunk that is not ready returns `ready = false`. It never returns 0 as a
// height: a player standing on "0" in the middle of an unloaded valley is worse than the
// player being held still.

#include "world_chunk.hpp"
#include "world_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace world {

enum class ChunkState : std::uint8_t {
    Unloaded = 0,
    Requested = 1,
    Reading = 2,
    Validated = 3,
    CpuReady = 4,
    Uploading = 5,
    Resident = 6,
    Retiring = 7,
};

const char* chunkStateName(ChunkState state);

struct RuntimeConfig {
    float chunkSizeMetres = 128.0f;
    float sampleStepMetres = 1.0f;
    // Distance bands. A chunk inside `nearDistance` is loaded at LOD 0, inside `midDistance`
    // at LOD 1, inside `farDistance` at LOD 2. Beyond that it is unloaded.
    float nearDistanceMetres = 320.0f;
    float midDistanceMetres = 900.0f;
    float farDistanceMetres = 1600.0f;
    int maxResidentChunks = 256;
    std::uint64_t memoryBudgetBytes = 512ull * 1024ull * 1024ull;
    // Collision prefetch: chunks are made CPU-ready this far beyond the view band so a
    // sprinting character never reaches ground that is still being read.
    float collisionLeadMetres = 96.0f;
    bool validateHashes = true;
};

struct ChunkRecord {
    ChunkCoord coord{};
    ChunkState state = ChunkState::Unloaded;
    std::uint32_t lod = 0;
    std::uint64_t epoch = 0;
    std::uint64_t lastUsedFrame = 0;
    float distanceMetres = 0.0f;
    std::uint64_t bytes = 0;
    std::shared_ptr<ChunkData> data;
};

// Reads one chunk file. The runtime never touches the filesystem itself, so a published
// build can pack chunks into one archive without changing this class.
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual bool read(ChunkCoord coord, std::vector<std::uint8_t>& bytes, std::string& error) = 0;
};

// Directory-backed source. Paths are relative to the cooked world root and contain no
// user-controlled segments, so there is nothing here to traverse.
class DirectoryChunkSource : public ChunkSource {
public:
    explicit DirectoryChunkSource(std::string root);
    bool read(ChunkCoord coord, std::vector<std::uint8_t>& bytes, std::string& error) override;
private:
    std::string root_;
};

struct SurfaceQuery {
    bool ready = false;
    float height = 0.0f;
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float normalY = 1.0f;
    float waterHeight = -1.0e9f;
    float slopeDegrees = 0.0f;
    std::uint8_t flags = 0;
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t chunkLod = 0;
    std::uint64_t dataVersion = 0;
    bool onRoad() const { return (flags & kQueryRoad) != 0; }
    bool onBridge() const { return (flags & kQueryBridge) != 0; }
    bool underwater() const { return (flags & kQueryWater) != 0; }
    bool steep() const { return (flags & kQuerySteep) != 0; }
};

struct BuildingQuery {
    bool found = false;
    std::uint16_t component = 0;
    float position[3] = {0, 0, 0};
    float yaw = 0.0f;
    float footprint[2] = {0, 0};
    float heightMetres = 0.0f;
    float entrance[3] = {0, 0, 0};
    bool entranceConnected = false;
    std::uint64_t stableId = 0;
};

struct RuntimeStats {
    std::uint64_t frames = 0;
    std::uint64_t chunksResident = 0;
    std::uint64_t chunksCpuReady = 0;
    std::uint64_t chunksRequested = 0;
    std::uint64_t chunksLoadedTotal = 0;
    std::uint64_t chunksEvictedTotal = 0;
    std::uint64_t bytesResident = 0;
    std::uint64_t instancesResident = 0;
    std::uint64_t buildingsResident = 0;
    std::uint64_t staleDropped = 0;
    std::uint64_t loadErrors = 0;
    std::uint64_t epoch = 0;
    double lastPumpMillis = 0.0;
    std::string lastError;
};

class WorldRuntime {
public:
    WorldRuntime();
    ~WorldRuntime();

    bool configure(const RuntimeConfig& config, std::string& error);
    // A manifest is not required to stream: the runtime can be driven by an explicit
    // bounds list. It is required for chunk bounds and for the world extent.
    void setManifestBounds(Bounds2 bounds);
    void setSource(ChunkSource* source);
    // Bumps the epoch, drops every in-flight load and unloads everything. Called when the
    // project or the world changes.
    void reset(std::uint64_t newEpoch = 0);

    // Advances streaming for one frame. `maxLoads` bounds the work so a teleport does not
    // block the frame for a second.
    void pump(float cameraX, float cameraZ, int maxLoads = 6);

    // Spatial queries. All of them are safe to call every frame and return `ready = false`
    // when the covering chunk is not CPU-ready.
    SurfaceQuery querySurface(float x, float z) const;
    bool queryHeight(float x, float z, float& height) const;
    BuildingQuery queryBuilding(float x, float z, float radius = 0.0f) const;

    // True when every chunk overlapping `bounds` is at least CPU-ready. The character
    // controller uses this to refuse a teleport into unloaded ground.
    bool isReady(const Bounds2& bounds) const;
    // Forces the chunks overlapping `bounds` to the front of the queue. Returns how many
    // were newly requested.
    int prefetch(const Bounds2& bounds, std::uint32_t lod = 0);

    const std::vector<ChunkRecord>& chunks() const { return chunks_; }
    RuntimeStats stats() const;
    std::uint64_t epoch() const { return epoch_; }

    // Renderer handshake. `beginUpload` moves cpuReady chunks to uploading and returns them;
    // `completeUpload` moves them to resident. A chunk evicted while uploading goes to
    // retiring and is released by `releaseRetired` once the fence has drained.
    std::vector<std::shared_ptr<ChunkData>> beginUpload(std::size_t maxChunks);
    void completeUpload(const std::vector<ChunkCoord>& coords);
    std::vector<ChunkCoord> takeRetired();
    void releaseRetired(const std::vector<ChunkCoord>& coords);

private:
    ChunkRecord* find(ChunkCoord coord);
    const ChunkRecord* find(ChunkCoord coord) const;
    ChunkRecord& touch(ChunkCoord coord);
    void loadRecord(ChunkRecord& record);
    void evictFarthest();
    std::uint32_t lodFor(float distance) const;

    RuntimeConfig config_{};
    Bounds2 worldBounds_{};
    ChunkSource* source_ = nullptr;
    std::uint64_t epoch_ = 1;
    std::uint64_t frame_ = 0;
    std::uint64_t dataVersion_ = 1;
    std::uint64_t bytesResident_ = 0;
    std::unordered_map<std::int64_t, std::size_t> index_;
    std::vector<ChunkRecord> chunks_;
    std::vector<ChunkCoord> retiring_;
    RuntimeStats stats_{};
    std::string lastError_;
};

} // namespace world
} // namespace engine

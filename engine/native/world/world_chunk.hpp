#pragma once

// Chunk container: the immutable, content-addressed unit the runtime streams.
//
// Layout (little endian, all offsets in bytes):
//
//   0   char[4]  'NGRC'
//   4   u16      version
//   6   u16      lod
//   8   i32      chunkX
//   12  i32      chunkZ
//   16  u32      chunkSizeMetres
//   20  u32      sampleStepCentimetres
//   24  u32      vertexCount (per axis)
//   28  u32      instanceCount
//   32  u32      segmentCount
//   36  u32      flags
//   40  u64      contentHash (FNV-1a 64 of the payload)
//   48  u64      inputDigestPrefix
//   56  u32      payloadBytes
//   60  u32      reserved
//   64  segment table: { u16 id, u16 elementBytes, u32 count, u32 offset, u32 length }
//   ..  payload
//
// The reader validates every length against the remaining bytes before it trusts it. A
// truncated or hostile file must produce a diagnostic, never an out-of-bounds read in a
// host process that is holding a GPU device.

#include "world_scatter.hpp"
#include "world_settlement.hpp"
#include "world_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace world {

constexpr std::uint32_t kChunkMagic = 0x4352474eu; // 'NGRC' little endian
constexpr std::uint16_t kChunkVersion = 1;
constexpr std::size_t kChunkHeaderBytes = 64;
constexpr std::size_t kChunkSegmentBytes = 16;

enum SegmentId : std::uint16_t {
    kSegmentHeights = 1,
    kSegmentMaterialWeights = 2,
    kSegmentNormals = 3,
    kSegmentInstances = 4,
    kSegmentRoadMesh = 5,
    kSegmentRiverBed = 6,
    kSegmentCollision = 7,
    kSegmentBuildings = 8,
    kSegmentQuery = 9,
};

// Per-vertex query flags stored in the QUERY segment. The runtime answers "is this a road"
// without touching the path network, which is what keeps a published build free of the
// authoring-side graph.
enum QueryFlags : std::uint8_t {
    kQueryRoad = 1u << 0,
    kQueryBridge = 1u << 1,
    kQueryWater = 1u << 2,
    kQueryBuilding = 1u << 3,
    kQuerySteep = 1u << 4,
};

// 64-byte building record. Fixed size so the segment is a flat array and the runtime can
// index it without a parser. The padding is explicit so the layout cannot drift with the
// compiler's alignment rules.
struct ChunkBuildingRecord {
    std::uint16_t component = 0;
    std::uint8_t entranceConnected = 0;
    std::uint8_t reserved = 0;
    float position[3] = {0, 0, 0};
    float yaw = 0.0f;
    float scale = 1.0f;
    float footprint[2] = {0, 0};
    float heightMetres = 0.0f;
    float entrance[3] = {0, 0, 0};
    float entranceYaw = 0.0f;
    std::uint32_t pad0 = 0;
    std::uint64_t stableId = 0;
};
static_assert(sizeof(ChunkBuildingRecord) == 64, "ChunkBuildingRecord is a file layout contract");
static_assert(offsetof(ChunkBuildingRecord, stableId) == 56, "stableId offset is a file layout contract");

struct ChunkData {
    ChunkCoord coord{};
    std::uint16_t lod = 0;
    float chunkSizeMetres = 128.0f;
    float sampleStepMetres = 1.0f;
    std::uint32_t vertexCount = 0; // per axis
    std::uint32_t flags = 0;

    // vertexCount² floats, row-major with z as the outer axis.
    std::vector<float> heights;
    // vertexCount² × 4 bytes: grass, rock, dirt, sand, normalized to 255.
    std::vector<std::uint8_t> materialWeights;
    // vertexCount² × 2 int16: octahedral normal.
    std::vector<std::int16_t> normals;
    // vertexCount² bytes of QueryFlags.
    std::vector<std::uint8_t> query;
    // Collision heights. Identical to `heights` at LOD 0; decimated at coarser LODs.
    std::vector<float> collision;

    std::vector<WorldInstance> instances;
    std::vector<ChunkBuildingRecord> buildings;

    // Road surface triangles: x, y, z per vertex, three vertices per triangle.
    std::vector<float> roadMesh;
    // River bed triangles, same layout.
    std::vector<float> riverBed;

    Bounds2 bounds{};
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    std::uint64_t contentHash = 0;

    std::uint32_t vertexIndex(std::uint32_t x, std::uint32_t z) const { return z * vertexCount + x; }
    bool valid() const {
        if (vertexCount < 2) return false;
        const std::size_t expected = static_cast<std::size_t>(vertexCount) * vertexCount;
        return heights.size() == expected
            && materialWeights.size() == expected * 4
            && normals.size() == expected * 2
            && query.size() == expected
            && (collision.empty() || collision.size() == expected);
    }
};

// Octahedral normal encoding. Two snorm16 values reproduce a unit normal to within about
// 0.02°, which is far below the precision of the shading normal it feeds.
void encodeNormalOctahedral(const float normal[3], std::int16_t out[2]);
void decodeNormalOctahedral(const std::int16_t in[2], float out[3]);

// Serializes into `out`. The content hash covers the payload only, so the same geometry
// hashes identically regardless of which segment table order produced it.
bool serializeChunk(const ChunkData& chunk, std::vector<std::uint8_t>& out, std::string& error);

// Parses and validates. `out` is only modified on success.
bool deserializeChunk(const std::uint8_t* data, std::size_t size, ChunkData& out, std::string& error);

// Reads one field from a header without decoding the payload. Returns false when the
// buffer is not a chunk this build understands.
struct ChunkHeaderInfo {
    std::uint16_t version = 0;
    std::uint16_t lod = 0;
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    std::uint32_t chunkSizeMetres = 0;
    std::uint32_t sampleStepCentimetres = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t segmentCount = 0;
    std::uint64_t contentHash = 0;
    std::uint64_t inputDigestPrefix = 0;
    std::uint32_t payloadBytes = 0;
};
bool readChunkHeader(const std::uint8_t* data, std::size_t size, ChunkHeaderInfo& out, std::string& error);

} // namespace world
} // namespace engine

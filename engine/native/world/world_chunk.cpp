#include "world_chunk.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine {
namespace world {
namespace {

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(&v), reinterpret_cast<const std::uint8_t*>(&v) + 2); }
    void u32(std::uint32_t v) { bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(&v), reinterpret_cast<const std::uint8_t*>(&v) + 4); }
    void u64(std::uint64_t v) { bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(&v), reinterpret_cast<const std::uint8_t*>(&v) + 8); }
    void raw(const void* data, std::size_t count) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + count);
    }
    std::size_t size() const { return bytes.size(); }
};

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool u8(std::uint8_t& v) { if (offset + 1 > size) return false; v = data[offset]; offset += 1; return true; }
    bool u16(std::uint16_t& v) { if (offset + 2 > size) return false; std::memcpy(&v, data + offset, 2); offset += 2; return true; }
    bool u32(std::uint32_t& v) { if (offset + 4 > size) return false; std::memcpy(&v, data + offset, 4); offset += 4; return true; }
    bool u64(std::uint64_t& v) { if (offset + 8 > size) return false; std::memcpy(&v, data + offset, 8); offset += 8; return true; }
    bool raw(void* out, std::size_t count) {
        if (offset + count > size) return false;
        std::memcpy(out, data + offset, count);
        offset += count;
        return true;
    }
    bool skip(std::size_t count) {
        if (offset + count > size) return false;
        offset += count;
        return true;
    }
};

struct SegmentPlan {
    std::uint16_t id = 0;
    std::uint16_t elementBytes = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> payload;
};

// Signed octahedral mapping, per "A Survey of Efficient Representations for Independent
// Unit Vectors" (Cigolle et al.). snorm16 keeps the round trip inside one texel of an
// 8-bit normal map.
float signNotZero(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

std::int16_t toSnorm16(float v) {
    const float clamped = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const float scaled = clamped * 32767.0f;
    return static_cast<std::int16_t>(std::lround(scaled));
}

float fromSnorm16(std::int16_t v) {
    return std::max(-1.0f, static_cast<float>(v) / 32767.0f);
}

} // namespace

void encodeNormalOctahedral(const float normal[3], std::int16_t out[2]) {
    float x = normal[0], y = normal[1], z = normal[2];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length < 1.0e-6f) { out[0] = 0; out[1] = 32767; return; }
    x /= length; y /= length; z /= length;
    const float l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (l1 < 1.0e-6f) { out[0] = 0; out[1] = 32767; return; }
    x /= l1; y /= l1; z /= l1;
    float ox = x, oy = y;
    if (z < 0.0f) {
        ox = (1.0f - std::fabs(y)) * signNotZero(x);
        oy = (1.0f - std::fabs(x)) * signNotZero(y);
    }
    out[0] = toSnorm16(ox);
    out[1] = toSnorm16(oy);
}

void decodeNormalOctahedral(const std::int16_t in[2], float out[3]) {
    const float ox = fromSnorm16(in[0]);
    const float oy = fromSnorm16(in[1]);
    float x = ox, y = oy;
    const float z = 1.0f - std::fabs(ox) - std::fabs(oy);
    if (z < 0.0f) {
        x = (1.0f - std::fabs(oy)) * signNotZero(ox);
        y = (1.0f - std::fabs(ox)) * signNotZero(oy);
    }
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length < 1.0e-6f) { out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f; return; }
    out[0] = x / length;
    out[1] = z / length;
    out[2] = y / length;
}

bool serializeChunk(const ChunkData& chunk, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (!chunk.valid()) {
        error = "chunk data is inconsistent (vertex count does not match its arrays)";
        return false;
    }

    std::vector<SegmentPlan> segments;
    const auto addSegment = [&segments](std::uint16_t id, std::uint16_t elementBytes, std::uint32_t count,
                                        const void* data, std::size_t bytes) {
        if (count == 0 || bytes == 0) return;
        SegmentPlan plan;
        plan.id = id;
        plan.elementBytes = elementBytes;
        plan.count = count;
        plan.payload.resize(bytes);
        std::memcpy(plan.payload.data(), data, bytes);
        segments.push_back(std::move(plan));
    };

    addSegment(kSegmentHeights, sizeof(float), static_cast<std::uint32_t>(chunk.heights.size()),
               chunk.heights.data(), chunk.heights.size() * sizeof(float));
    addSegment(kSegmentMaterialWeights, 4, static_cast<std::uint32_t>(chunk.materialWeights.size() / 4),
               chunk.materialWeights.data(), chunk.materialWeights.size());
    addSegment(kSegmentNormals, 4, static_cast<std::uint32_t>(chunk.normals.size() / 2),
               chunk.normals.data(), chunk.normals.size() * sizeof(std::int16_t));
    addSegment(kSegmentQuery, 1, static_cast<std::uint32_t>(chunk.query.size()),
               chunk.query.data(), chunk.query.size());
    addSegment(kSegmentCollision, sizeof(float), static_cast<std::uint32_t>(chunk.collision.size()),
               chunk.collision.data(), chunk.collision.size() * sizeof(float));
    addSegment(kSegmentInstances, static_cast<std::uint16_t>(sizeof(WorldInstance)),
               static_cast<std::uint32_t>(chunk.instances.size()),
               chunk.instances.data(), chunk.instances.size() * sizeof(WorldInstance));
    addSegment(kSegmentBuildings, static_cast<std::uint16_t>(sizeof(ChunkBuildingRecord)),
               static_cast<std::uint32_t>(chunk.buildings.size()),
               chunk.buildings.data(), chunk.buildings.size() * sizeof(ChunkBuildingRecord));
    // A road/river vertex is three floats, so the element size is 12 and the element count
    // is a vertex count. Declaring it as 4 would make count*elementBytes disagree with the
    // byte length and the reader would (correctly) refuse the file.
    addSegment(kSegmentRoadMesh, 12, static_cast<std::uint32_t>(chunk.roadMesh.size() / 3),
               chunk.roadMesh.data(), chunk.roadMesh.size() * sizeof(float));
    addSegment(kSegmentRiverBed, 12, static_cast<std::uint32_t>(chunk.riverBed.size() / 3),
               chunk.riverBed.data(), chunk.riverBed.size() * sizeof(float));

    std::size_t payloadBytes = 0;
    for (const auto& segment : segments) payloadBytes += segment.payload.size();
    const std::size_t total = kChunkHeaderBytes + segments.size() * kChunkSegmentBytes + payloadBytes;
    if (total > Limits::kMaxChunkBytes) {
        error = "chunk exceeds the maximum file size";
        return false;
    }

    // Hash the payload before writing the header: the header contains the hash, so it
    // cannot be part of its own input.
    Hasher hasher;
    for (const auto& segment : segments) hasher.bytes(segment.payload.data(), segment.payload.size());
    const std::uint64_t contentHash = hasher.digest();

    Writer writer;
    writer.bytes.resize(total);
    std::memcpy(writer.bytes.data(), "NGRC", 4);
    auto put16 = [&](std::size_t offset, std::uint16_t value) { std::memcpy(writer.bytes.data() + offset, &value, 2); };
    auto put32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(writer.bytes.data() + offset, &value, 4); };
    auto put64 = [&](std::size_t offset, std::uint64_t value) { std::memcpy(writer.bytes.data() + offset, &value, 8); };

    put16(4, kChunkVersion);
    put16(6, chunk.lod);
    put32(8, static_cast<std::uint32_t>(chunk.coord.x));
    put32(12, static_cast<std::uint32_t>(chunk.coord.z));
    put32(16, static_cast<std::uint32_t>(std::lround(chunk.chunkSizeMetres)));
    put32(20, static_cast<std::uint32_t>(std::lround(chunk.sampleStepMetres * 100.0f)));
    put32(24, chunk.vertexCount);
    put32(28, static_cast<std::uint32_t>(chunk.instances.size()));
    put32(32, static_cast<std::uint32_t>(segments.size()));
    put32(36, chunk.flags);
    put64(40, contentHash);
    put64(48, chunk.contentHash);
    put32(56, static_cast<std::uint32_t>(payloadBytes));
    put32(60, 0u);

    std::size_t tableOffset = kChunkHeaderBytes;
    std::size_t payloadOffset = 0;
    for (const auto& segment : segments) {
        put16(tableOffset + 0, segment.id);
        put16(tableOffset + 2, segment.elementBytes);
        put32(tableOffset + 4, segment.count);
        put32(tableOffset + 8, static_cast<std::uint32_t>(payloadOffset));
        put32(tableOffset + 12, static_cast<std::uint32_t>(segment.payload.size()));
        tableOffset += kChunkSegmentBytes;
        std::memcpy(writer.bytes.data() + kChunkHeaderBytes + segments.size() * kChunkSegmentBytes + payloadOffset,
                    segment.payload.data(), segment.payload.size());
        payloadOffset += segment.payload.size();
    }

    out = std::move(writer.bytes);
    return true;
}

bool readChunkHeader(const std::uint8_t* data, std::size_t size, ChunkHeaderInfo& out, std::string& error) {
    if (!data || size < kChunkHeaderBytes) { error = "chunk is shorter than its header"; return false; }
    if (std::memcmp(data, "NGRC", 4) != 0) { error = "chunk magic is not NGRC"; return false; }
    std::uint16_t version = 0, lod = 0;
    std::uint32_t value = 0;
    std::memcpy(&version, data + 4, 2);
    if (version != kChunkVersion) {
        error = "chunk version " + std::to_string(version) + " is not supported";
        return false;
    }
    std::memcpy(&lod, data + 6, 2);
    out.version = version;
    out.lod = lod;
    std::memcpy(&out.chunkX, data + 8, 4);
    std::memcpy(&out.chunkZ, data + 12, 4);
    std::memcpy(&out.chunkSizeMetres, data + 16, 4);
    std::memcpy(&out.sampleStepCentimetres, data + 20, 4);
    std::memcpy(&out.vertexCount, data + 24, 4);
    std::memcpy(&out.instanceCount, data + 28, 4);
    std::memcpy(&out.segmentCount, data + 32, 4);
    std::memcpy(&out.contentHash, data + 40, 8);
    std::memcpy(&out.inputDigestPrefix, data + 48, 8);
    std::memcpy(&out.payloadBytes, data + 56, 4);
    (void)value;

    if (out.lod > 3) { error = "chunk LOD is outside 0..3"; return false; }
    if (out.vertexCount > static_cast<std::uint32_t>(Limits::kMaxVerticesPerAxis * Limits::kMaxVerticesPerAxis)) {
        error = "chunk vertex count exceeds the contract limit";
        return false;
    }
    if (out.instanceCount > static_cast<std::uint32_t>(Limits::kMaxInstancesPerChunk)) {
        error = "chunk instance count exceeds the contract limit";
        return false;
    }
    if (out.segmentCount > 32) { error = "chunk segment count exceeds the contract limit"; return false; }
    const std::size_t tableBytes = static_cast<std::size_t>(out.segmentCount) * kChunkSegmentBytes;
    if (kChunkHeaderBytes + tableBytes + out.payloadBytes != size) {
        error = "chunk byte count does not match its header";
        return false;
    }
    return true;
}

bool deserializeChunk(const std::uint8_t* data, std::size_t size, ChunkData& out, std::string& error) {
    ChunkHeaderInfo header;
    if (!readChunkHeader(data, size, header, error)) return false;

    ChunkData chunk;
    chunk.coord.x = header.chunkX;
    chunk.coord.z = header.chunkZ;
    chunk.lod = header.lod;
    chunk.chunkSizeMetres = static_cast<float>(header.chunkSizeMetres);
    chunk.sampleStepMetres = static_cast<float>(header.sampleStepCentimetres) / 100.0f;
    chunk.vertexCount = header.vertexCount;
    chunk.contentHash = header.inputDigestPrefix;

    const std::size_t tableBytes = static_cast<std::size_t>(header.segmentCount) * kChunkSegmentBytes;
    const std::size_t payloadBase = kChunkHeaderBytes + tableBytes;

    // First pass: validate every segment against the payload before copying anything.
    for (std::uint32_t index = 0; index < header.segmentCount; ++index) {
        const std::uint8_t* entry = data + kChunkHeaderBytes + index * kChunkSegmentBytes;
        std::uint16_t id = 0, elementBytes = 0;
        std::uint32_t count = 0, offset = 0, length = 0;
        std::memcpy(&id, entry + 0, 2);
        std::memcpy(&elementBytes, entry + 2, 2);
        std::memcpy(&count, entry + 4, 4);
        std::memcpy(&offset, entry + 8, 4);
        std::memcpy(&length, entry + 12, 4);
        if (static_cast<std::uint64_t>(offset) + length > header.payloadBytes) {
            error = "chunk segment runs past the payload";
            return false;
        }
        if (elementBytes > 0 && static_cast<std::uint64_t>(count) * elementBytes != length) {
            error = "chunk segment element count does not match its byte length";
            return false;
        }
    }

    Hasher hasher;
    hasher.bytes(data + payloadBase, header.payloadBytes);
    if (hasher.digest() != header.contentHash) {
        error = "chunk payload hash does not match its header";
        return false;
    }

    // Second pass: copy.
    for (std::uint32_t index = 0; index < header.segmentCount; ++index) {
        const std::uint8_t* entry = data + kChunkHeaderBytes + index * kChunkSegmentBytes;
        std::uint16_t id = 0, elementBytes = 0;
        std::uint32_t count = 0, offset = 0, length = 0;
        std::memcpy(&id, entry + 0, 2);
        std::memcpy(&elementBytes, entry + 2, 2);
        std::memcpy(&count, entry + 4, 4);
        std::memcpy(&offset, entry + 8, 4);
        std::memcpy(&length, entry + 12, 4);
        const std::uint8_t* payload = data + payloadBase + offset;
        switch (id) {
            case kSegmentHeights:
                chunk.heights.resize(count);
                std::memcpy(chunk.heights.data(), payload, length);
                break;
            case kSegmentMaterialWeights:
                chunk.materialWeights.resize(length);
                std::memcpy(chunk.materialWeights.data(), payload, length);
                break;
            case kSegmentNormals:
                chunk.normals.resize(static_cast<std::size_t>(count) * 2);
                std::memcpy(chunk.normals.data(), payload, length);
                break;
            case kSegmentQuery:
                chunk.query.resize(length);
                std::memcpy(chunk.query.data(), payload, length);
                break;
            case kSegmentCollision:
                chunk.collision.resize(count);
                std::memcpy(chunk.collision.data(), payload, length);
                break;
            case kSegmentInstances:
                chunk.instances.resize(count);
                std::memcpy(chunk.instances.data(), payload, length);
                break;
            case kSegmentBuildings:
                chunk.buildings.resize(count);
                std::memcpy(chunk.buildings.data(), payload, length);
                break;
            case kSegmentRoadMesh:
                chunk.roadMesh.resize(static_cast<std::size_t>(count) * 3);
                std::memcpy(chunk.roadMesh.data(), payload, length);
                break;
            case kSegmentRiverBed:
                chunk.riverBed.resize(static_cast<std::size_t>(count) * 3);
                std::memcpy(chunk.riverBed.data(), payload, length);
                break;
            default:
                // Unknown segments are skipped on purpose: a newer cooker may add one and
                // an older runtime must still read the rest of the chunk.
                break;
        }
    }

    if (!chunk.valid()) {
        error = "chunk arrays do not match its declared vertex count";
        return false;
    }

    float minimum = 1.0e30f, maximum = -1.0e30f;
    for (float height : chunk.heights) {
        minimum = std::min(minimum, height);
        maximum = std::max(maximum, height);
    }
    chunk.minHeight = minimum;
    chunk.maxHeight = maximum;
    const float size_ = chunk.chunkSizeMetres;
    chunk.bounds = {static_cast<float>(chunk.coord.x) * size_, static_cast<float>(chunk.coord.z) * size_,
                    static_cast<float>(chunk.coord.x) * size_ + size_, static_cast<float>(chunk.coord.z) * size_ + size_};
    out = std::move(chunk);
    return true;
}

} // namespace world
} // namespace engine

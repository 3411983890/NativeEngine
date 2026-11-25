#pragma once

// Shared value types for the world compiler and runtime.
//
// The rules this header exists to enforce:
//   * Chunk coordinates are integers derived with floor(), never truncation. A world
//     centred on the origin has negative chunks, and `static_cast<int>(-0.5f)` is 0,
//     which would put the seam of every brush stroke in the wrong chunk.
//   * Random streams are named and derived from (seed, generator version, layer, cell).
//     Nothing here reads a clock, a thread id or std::random_device, so two runs with the
//     same input produce the same bytes regardless of how the work was scheduled.
//   * Floats that leave the compiler are quantized. Cross-compiler libm differences in the
//     15th decimal must not change a content hash.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace engine {
namespace world {

// ---------------------------------------------------------------- coordinates

struct ChunkCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;
    bool operator==(const ChunkCoord& other) const { return x == other.x && z == other.z; }
    bool operator!=(const ChunkCoord& other) const { return !(*this == other); }
    bool operator<(const ChunkCoord& other) const {
        return z != other.z ? z < other.z : x < other.x;
    }
};

struct Bounds2 {
    float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    float width() const { return maxX - minX; }
    float depth() const { return maxZ - minZ; }
    float centreX() const { return (minX + maxX) * 0.5f; }
    float centreZ() const { return (minZ + maxZ) * 0.5f; }
    bool valid() const { return maxX > minX && maxZ > minZ; }
    bool contains(float x, float z) const { return x >= minX && x <= maxX && z >= minZ && z <= maxZ; }
    bool overlaps(const Bounds2& other) const {
        return minX < other.maxX && other.minX < maxX && minZ < other.maxZ && other.minZ < maxZ;
    }
    void expand(float metres) {
        minX -= metres; minZ -= metres; maxX += metres; maxZ += metres;
    }
    void include(float x, float z) {
        if (x < minX) minX = x;
        if (z < minZ) minZ = z;
        if (x > maxX) maxX = x;
        if (z > maxZ) maxZ = z;
    }
};

inline Bounds2 emptyBounds() {
    const float big = 1.0e30f;
    return {big, big, -big, -big};
}

inline Bounds2 unionBounds(Bounds2 a, const Bounds2& b) {
    if (b.minX < a.minX) a.minX = b.minX;
    if (b.minZ < a.minZ) a.minZ = b.minZ;
    if (b.maxX > a.maxX) a.maxX = b.maxX;
    if (b.maxZ > a.maxZ) a.maxZ = b.maxZ;
    return a;
}

inline Bounds2 chunkBoundsOf(ChunkCoord coord, float chunkSizeMetres) {
    const float x = static_cast<float>(coord.x) * chunkSizeMetres;
    const float z = static_cast<float>(coord.z) * chunkSizeMetres;
    return {x, z, x + chunkSizeMetres, z + chunkSizeMetres};
}

// floor(), not truncation. -0.5 / 128 is -0.0039, whose floor is -1.
inline std::int32_t chunkIndexForMetres(float metres, float chunkSizeMetres) {
    const float quotient = metres / chunkSizeMetres;
    return static_cast<std::int32_t>(std::floor(quotient));
}

struct ChunkRange {
    std::int32_t minCx = 0, minCz = 0, maxCx = -1, maxCz = -1;
    bool valid() const { return maxCx >= minCx && maxCz >= minCz; }
    std::uint64_t count() const {
        if (!valid()) return 0;
        return static_cast<std::uint64_t>(maxCx - minCx + 1) * static_cast<std::uint64_t>(maxCz - minCz + 1);
    }
};

// Dense index key for a chunk coordinate. Negative coordinates are supported; the pair is
// packed into one 64-bit value so an unordered_map does not need a custom hash.
inline std::int64_t chunkKeyOf(ChunkCoord coord) {
    return (static_cast<std::int64_t>(coord.x) << 32) ^ static_cast<std::uint32_t>(coord.z);
}

inline ChunkRange chunkRangeForBounds(const Bounds2& bounds, float chunkSizeMetres) {
    ChunkRange range;
    range.minCx = chunkIndexForMetres(bounds.minX, chunkSizeMetres);
    range.minCz = chunkIndexForMetres(bounds.minZ, chunkSizeMetres);
    // A bounds edge exactly on a chunk border must not dirty the next chunk: step back by
    // a fraction of a millimetre before taking the floor of the max edge.
    range.maxCx = chunkIndexForMetres(bounds.maxX - 1.0e-4f, chunkSizeMetres);
    range.maxCz = chunkIndexForMetres(bounds.maxZ - 1.0e-4f, chunkSizeMetres);
    if (range.maxCx < range.minCx) range.maxCx = range.minCx;
    if (range.maxCz < range.minCz) range.maxCz = range.minCz;
    return range;
}

// ---------------------------------------------------------------- hashing

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct Hasher {
    std::uint64_t value = kFnvOffset;
    void bytes(const void* data, std::size_t count) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            value ^= static_cast<std::uint64_t>(p[i]);
            value *= kFnvPrime;
        }
    }
    void u8(std::uint8_t v) { bytes(&v, 1); }
    void u16(std::uint16_t v) { bytes(&v, 2); }
    void u32(std::uint32_t v) { bytes(&v, 4); }
    void u64(std::uint64_t v) { bytes(&v, 8); }
    void i32(std::int32_t v) { bytes(&v, 4); }
    void text(const std::string& v) { u32(static_cast<std::uint32_t>(v.size())); bytes(v.data(), v.size()); }
    // Floats are quantized before hashing. Hashing raw bits would make the digest depend on
    // whether the compiler kept a value in an 80-bit register.
    void quantized(float v, float scale = 10000.0f) {
        if (!std::isfinite(v)) { u32(0x7fc00000u); return; }
        const double scaled = static_cast<double>(v) * static_cast<double>(scale);
        const std::int32_t fixed = static_cast<std::int32_t>(std::llround(scaled));
        i32(fixed);
    }
    std::string hex() const {
        static const char* digits = "0123456789abcdef";
        std::string out(16, '0');
        std::uint64_t remaining = value;
        for (int i = 15; i >= 0; --i) {
            out[static_cast<std::size_t>(i)] = digits[remaining & 0xfu];
            remaining >>= 4;
        }
        return out;
    }
    std::uint64_t digest() const { return value; }
};

inline std::uint64_t mix64(std::uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}

inline std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) {
    return mix64(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

inline std::uint64_t hashText(std::uint64_t seed, const std::string& text) {
    Hasher hasher;
    hasher.value = seed;
    hasher.text(text);
    return hasher.digest();
}

// Parses a decimal seed string. 64-bit seeds do not survive a double, which is why the
// recipe carries them as text; this is the only place the string becomes an integer.
inline bool parseSeed(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ull) return false;
        value = value * 10ull + digit;
    }
    out = value;
    return true;
}

// ---------------------------------------------------------------- random

// xoshiro256** with splitmix64 seeding. Fully specified by integer arithmetic, so the
// sequence is identical on every supported compiler and every thread order.
class Rng {
public:
    Rng() { seed(0ull, 0ull); }
    Rng(std::uint64_t seedValue, std::uint64_t streamId) { seed(seedValue, streamId); }

    void seed(std::uint64_t seedValue, std::uint64_t streamId) {
        std::uint64_t state = mix64(seedValue ^ mix64(streamId + 0x9e3779b97f4a7c15ull));
        for (auto& word : state_) {
            state = state + 0x9e3779b97f4a7c15ull;
            word = mix64(state);
        }
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(state_[1] * 5ull, 7) * 9ull;
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // [0, 1). The 24-bit mantissa keeps the result exactly representable in float, so a
    // float conversion downstream cannot round two different draws to the same value.
    float unit() {
        return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
    }

    float range(float low, float high) { return low + (high - low) * unit(); }

    std::uint32_t below(std::uint32_t bound) {
        if (bound == 0) return 0;
        return static_cast<std::uint32_t>(next() % bound);
    }

private:
    static std::uint64_t rotl(std::uint64_t value, int count) {
        return (value << count) | (value >> (64 - count));
    }
    std::uint64_t state_[4]{};
};

// Derives the stream for one (layer, cell, purpose) triple. Every consumer calls this with
// its own purpose tag, so adding a new consumer cannot shift a sequence another consumer
// already depends on.
inline std::uint64_t deriveStream(std::uint64_t worldSeed, std::uint64_t generatorTag,
                                  const std::string& layerId, std::int32_t cellX,
                                  std::int32_t cellZ, std::uint32_t purpose) {
    Hasher hasher;
    hasher.value = mix64(worldSeed);
    hasher.u64(generatorTag);
    hasher.text(layerId);
    hasher.i32(cellX);
    hasher.i32(cellZ);
    hasher.u32(purpose);
    return hasher.digest();
}

// ---------------------------------------------------------------- quantization

inline float quantize(float value, float step = 0.001f) {
    if (!std::isfinite(value)) return 0.0f;
    return std::round(value / step) * step;
}

// ---------------------------------------------------------------- limits

// Contract limits. The JS validator rejects the same numbers, so a file that reached this
// far and violates one is corrupt rather than merely ambitious.
struct Limits {
    static constexpr int kMaxChunksPerAxis = 512;
    static constexpr int kMaxVerticesPerAxis = 257;
    static constexpr int kMaxInstancesPerChunk = 262144;
    static constexpr int kMaxLayers = 256;
    static constexpr int kMaxPathPoints = 4096;
    static constexpr int kMaxPathsPerLayer = 512;
    static constexpr int kMaxComponents = 4096;
    static constexpr int kMaxChunkBytes = 256 * 1024 * 1024;
};

} // namespace world
} // namespace engine

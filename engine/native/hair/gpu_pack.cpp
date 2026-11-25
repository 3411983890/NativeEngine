#include "gpu_pack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace engine::hair {
namespace {
constexpr std::uint32_t kFileVersion = 1;
constexpr std::uint32_t kHeaderBytes = 320;
constexpr std::uint32_t kSectionAlignment = 256;
constexpr std::uint32_t kPointStride = 32;
constexpr std::uint32_t kStrandStride = 32;
constexpr std::uint32_t kGuidePointStride = 16;
constexpr std::uint32_t kGuideWeightStride = 32;
constexpr std::uint32_t kAttributeStride = 16;
constexpr std::uint64_t kMaximumFileBytes = 512ull * 1024 * 1024;

[[noreturn]] void reject(const std::string& why) {
    throw Error("NG-E-HAIR-GPU-PACK-FILE", why);
}
void require(bool ok, const std::string& why) { if (!ok) reject(why); }

bool isHex64(const std::string& text) {
    if (text.size() != 64) return false;
    for (char character : text)
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return false;
    return true;
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    void span(std::uint64_t offset, std::uint64_t length, const char* what) const {
        require(offset <= size && length <= size - offset, std::string(what) + " leaves the file");
    }
    std::uint32_t u32(std::uint64_t offset) const {
        span(offset, 4, "a 32-bit header field");
        std::uint32_t value = 0;
        std::memcpy(&value, data + offset, 4);
        return value;
    }
    float f32(std::uint64_t offset) const {
        span(offset, 4, "a 32-bit header field");
        float value = 0;
        std::memcpy(&value, data + offset, 4);
        return value;
    }
    std::uint64_t u64(std::uint64_t offset) const {
        span(offset, 8, "a 64-bit header field");
        std::uint64_t value = 0;
        std::memcpy(&value, data + offset, 8);
        return value;
    }
    std::string text(std::uint64_t offset, std::size_t length) const {
        span(offset, length, "a header string");
        return std::string(reinterpret_cast<const char*>(data + offset), length);
    }
};

constexpr std::uint32_t kSectionOffsets = 88;
constexpr std::uint32_t kSectionSizes = 108;

void validateStrandTable(GpuPack& pack) {
    const auto& bytes = pack.guideWeights;
    const auto& attributes = pack.strandAttributes;
    require(bytes.size() == static_cast<std::size_t>(pack.strandCount) * kGuideWeightStride,
        "guide weight table size disagrees with the strand count");
    require(attributes.size() == static_cast<std::size_t>(pack.strandCount) * kAttributeStride,
        "strand attribute size disagrees with the strand count");
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(pack.strandCount);
    std::uint64_t cursor = 0;
    for (std::uint32_t index = 0; index < pack.strandCount; ++index) {
        const auto& strand = pack.strands[index];
        require(strand.firstPoint == cursor, "strand points are not stored contiguously");
        require(strand.pointCount == pack.segmentsPerStrand + 1,
            "a strand's point count disagrees with the level's segment count");
        require(strand.guide0 < pack.guideCount && strand.guide1 < pack.guideCount && strand.guide2 < pack.guideCount,
            "a strand references a guide that is not in the package");
        require(ids.insert(strand.stableId).second, "two strands share a stable id");
        const std::size_t base = static_cast<std::size_t>(index) * kGuideWeightStride;
        std::uint32_t guide[3];
        std::memcpy(guide, bytes.data() + base, sizeof(guide));
        require(guide[0] == strand.guide0 && guide[1] == strand.guide1 && guide[2] == strand.guide2,
            "the guide weight table and the strand record name different guides");
        float weight[3];
        std::memcpy(weight, bytes.data() + base + 16, sizeof(weight));
        const float total = weight[0] + weight[1] + weight[2];
        require(std::isfinite(total) && std::fabs(total - 1.0f) <= 1e-3f,
            "a strand's guide weights are not normalised");
        std::uint32_t attributeId = 0;
        std::memcpy(&attributeId, attributes.data() + static_cast<std::size_t>(index) * kAttributeStride + 12, 4);
        require(attributeId == strand.stableId, "the attribute table and the strand record disagree on the stable id");
        cursor += strand.pointCount;
    }
    require(cursor == pack.pointCount, "the strand table does not describe every point");

    float minimum[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    float maximum[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
    for (std::uint32_t index = 0; index < pack.pointCount; ++index) {
        const auto& point = pack.points[index];
        for (int axis = 0; axis < 3; ++axis) {
            require(std::isfinite(point.position[axis]) && std::fabs(point.position[axis]) <= 1200.0f,
                "a strand point is not a finite position inside the metre mapping bound");
            minimum[axis] = std::min(minimum[axis], point.position[axis]);
            maximum[axis] = std::max(maximum[axis], point.position[axis]);
        }
        const float length = std::sqrt(point.tangent[0] * point.tangent[0]
            + point.tangent[1] * point.tangent[1] + point.tangent[2] * point.tangent[2]);
        require(std::isfinite(length) && std::fabs(length - 1.0f) <= 1e-3f, "a strand tangent is not unit length");
        require(std::isfinite(point.widthMetres) && point.widthMetres > 0 && point.widthMetres <= 0.05f,
            "a strand width is outside (0, 0.05] m");
        require(std::isfinite(point.rootToTip) && point.rootToTip >= 0 && point.rootToTip <= 1,
            "a root-to-tip parameter is outside [0, 1]");
    }
    for (int axis = 0; axis < 3; ++axis) {
        require(std::isfinite(pack.boundsMin[axis]) && std::isfinite(pack.boundsMax[axis])
            && pack.boundsMin[axis] <= pack.boundsMax[axis], "the declared bounds are not a finite box");
        require(std::fabs(pack.boundsMin[axis] - minimum[axis]) <= 1e-4f
            && std::fabs(pack.boundsMax[axis] - maximum[axis]) <= 1e-4f,
            "the declared bounds do not match the strand points");
    }
}
} // namespace

Error::Error(std::string codeValue, std::string message)
    : std::runtime_error(message), code(std::move(codeValue)) {}

GpuPack decodeGpuPack(const std::vector<std::uint8_t>& bytes) {
    require(bytes.size() >= kHeaderBytes, "the file is shorter than its header");
    Reader reader{bytes.data(), bytes.size()};
    require(reader.text(0, 4) == "HGP1", "the magic is not HGP1");
    require(reader.u32(4) == kFileVersion, "the pack version is not supported");
    require(reader.u32(8) == kHeaderBytes, "the declared header size is not the one this reader implements");
    GpuPack pack;
    pack.level = reader.u32(12);
    pack.strandCount = reader.u32(16);
    pack.pointCount = reader.u32(20);
    pack.segmentCount = reader.u32(24);
    pack.guideCount = reader.u32(28);
    pack.guidePointCount = reader.u32(32);
    pack.materialGroupCount = reader.u32(36);
    pack.pointStride = reader.u32(40);
    pack.strandStride = reader.u32(44);
    pack.guidePointStride = reader.u32(48);
    pack.guideWeightStride = reader.u32(52);
    require(pack.pointStride == kPointStride && pack.strandStride == kStrandStride
        && pack.guidePointStride == kGuidePointStride && pack.guideWeightStride == kGuideWeightStride,
        "the pack declares strides this reader does not implement");
    for (std::uint32_t value : {pack.strandCount, pack.pointCount, pack.segmentCount, pack.guideCount, pack.guidePointCount,
                                pack.materialGroupCount})
        require(value > 0, "a name count in the header is zero");
    pack.drawVertices = reader.u32(128);
    pack.segmentsPerStrand = reader.u32(132);
    require(pack.segmentsPerStrand >= 1, "a level needs at least one segment per strand");
    require(pack.pointCount == static_cast<std::uint64_t>(pack.strandCount) * (pack.segmentsPerStrand + 1),
        "the point count is not strands × (segments + 1)");
    require(pack.segmentCount == static_cast<std::uint64_t>(pack.strandCount) * pack.segmentsPerStrand,
        "the segment count is not strands × segments per strand");
    require(pack.drawVertices == static_cast<std::uint64_t>(pack.segmentCount) * 6,
        "the draw vertex count is not six per segment");
    // The flat guide-point table has no per-guide header, so the only way a runtime can find
    // guide boundaries is a uniform point count per guide. The guide resolution is shared
    // across LODs, so it is not tied to this level's segment count.
    require(pack.guidePointCount % pack.guideCount == 0,
        "the guide point table is not uniform across guides");
    require(pack.guidePointCount / pack.guideCount >= 2, "the guides have fewer than two points");
    for (int axis = 0; axis < 3; ++axis) {
        pack.boundsMin[axis] = reader.f32(56 + axis * 4);
        pack.boundsMax[axis] = reader.f32(72 + axis * 4);
    }
    pack.uploadBytes = reader.u64(144);
    pack.hash = reader.text(152, 64);
    pack.hairHash = reader.text(216, 64);
    require(isHex64(pack.hash), "the pack hash is not a sha256 hex string");
    require(isHex64(pack.hairHash), "the pack does not name the cooked hairstyle it came from");

    const std::uint64_t expectedSizes[5] = {
        static_cast<std::uint64_t>(pack.pointCount) * kPointStride,
        static_cast<std::uint64_t>(pack.strandCount) * kStrandStride,
        static_cast<std::uint64_t>(pack.guidePointCount) * kGuidePointStride,
        static_cast<std::uint64_t>(pack.strandCount) * kGuideWeightStride,
        static_cast<std::uint64_t>(pack.strandCount) * kAttributeStride,
    };
    std::uint64_t offsets[5]{}, sizes[5]{};
    std::string payload;
    payload.reserve(static_cast<std::size_t>(expectedSizes[0] + expectedSizes[1] + expectedSizes[2]));
    for (int index = 0; index < 5; ++index) {
        offsets[index] = reader.u32(kSectionOffsets + index * 4);
        sizes[index] = reader.u32(kSectionSizes + index * 4);
        require(sizes[index] == expectedSizes[index], "a section size disagrees with its counts and stride");
        require(offsets[index] % kSectionAlignment == 0, "a section offset is not 256-byte aligned");
        require(offsets[index] >= kHeaderBytes, "a section starts inside the header");
        reader.span(offsets[index], sizes[index], "a section");
        payload.append(reinterpret_cast<const char*>(bytes.data() + offsets[index]),
            static_cast<std::size_t>(sizes[index]));
    }
    require(engine::assets::sha256(payload) == pack.hash, "the payload does not match the declared hash");
    require(pack.uploadBytes == payload.size(), "the declared upload size disagrees with the payload");

    pack.points.resize(pack.pointCount);
    pack.strands.resize(pack.strandCount);
    pack.guidePoints.resize(static_cast<std::size_t>(pack.guidePointCount) * 4);
    pack.guideWeights.resize(sizes[3]);
    pack.strandAttributes.resize(sizes[4]);
    std::memcpy(pack.points.data(), bytes.data() + offsets[0], sizes[0]);
    std::memcpy(pack.strands.data(), bytes.data() + offsets[1], sizes[1]);
    std::memcpy(pack.guidePoints.data(), bytes.data() + offsets[2], sizes[2]);
    std::memcpy(pack.guideWeights.data(), bytes.data() + offsets[3], sizes[3]);
    std::memcpy(pack.strandAttributes.data(), bytes.data() + offsets[4], sizes[4]);
    for (const auto& point : pack.points) {
        for (float value : point.position)
            require(std::isfinite(value), "a strand point is not finite");
    }
    validateStrandTable(pack);
    return pack;
}

GpuPack loadGpuPack(const std::string& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(std::filesystem::u8path(path), sizeError);
    require(!sizeError && fileSize >= kHeaderBytes, "the hair pack does not exist or is empty");
    require(fileSize <= kMaximumFileBytes, "the hair pack exceeds the supported size bound");
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    require(stream.good(), "the hair pack cannot be opened");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(stream.gcount() == static_cast<std::streamsize>(bytes.size()), "the hair pack is truncated on read");
    return decodeGpuPack(bytes);
}

} // namespace engine::hair

#pragma once

#include "../assets/sha256.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Native reader for the strand upload document written by engine/hair/gpu-pack.mjs.
//
// The file, not the in-memory pack, is the interface between the cook and the renderer.
// That makes the reader the place where a bad offset, a stale stride or a hand-edited
// curve has to be refused: every count, stride, section extent and the payload hash are
// checked before a single point reaches a vertex buffer.
//
// The draw needs points and strands. Guide points and the weight/attribute tables are
// read and validated anyway, because a package whose guide table disagrees with its
// strands is a broken cook even when the draw would not touch it.

namespace engine::hair {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string codeValue, std::string message);
};

struct GpuPackPoint {
    float position[3] = {0, 0, 0};
    float tangent[3] = {0, 0, 1};
    float widthMetres = 0;
    float rootToTip = 0;
};
static_assert(sizeof(GpuPackPoint) == 32, "the point record is the 32-byte file ABI");

struct GpuPackStrand {
    std::uint32_t firstPoint = 0, pointCount = 0, materialGroup = 0, stableId = 0;
    std::uint32_t guide0 = 0, guide1 = 0, guide2 = 0, flags = 0;
};
static_assert(sizeof(GpuPackStrand) == 32, "the strand record is the 32-byte file ABI");

struct GpuPack {
    std::uint32_t level = 0;
    std::uint32_t segmentsPerStrand = 0;
    std::uint32_t drawVertices = 0;
    std::uint32_t strandCount = 0, pointCount = 0, segmentCount = 0;
    std::uint32_t guideCount = 0, guidePointCount = 0, materialGroupCount = 0;
    std::uint32_t pointStride = 0, strandStride = 0, guidePointStride = 0, guideWeightStride = 0;
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    std::uint64_t uploadBytes = 0;
    std::string hash, hairHash;
    std::vector<GpuPackPoint> points;
    std::vector<GpuPackStrand> strands;
    std::vector<float> guidePoints;  // 4 floats per guide point: position + radius
    std::vector<std::uint8_t> guideWeights, strandAttributes;
};

// Throws `Error` (NG-E-HAIR-GPU-PACK-FILE) for anything it cannot prove.
GpuPack decodeGpuPack(const std::vector<std::uint8_t>& bytes);
GpuPack loadGpuPack(const std::string& path);

} // namespace engine::hair

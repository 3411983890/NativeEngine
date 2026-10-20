#pragma once

#include <cstdint>

// 32-bit material handle (docs/GEOMETRY-LAYOUT.md §7):
//   handle = (generation << 16) | (sliceIndex & 0xFFFF)
//
// The GPU side only decodes the slice index and samples ResourceDescriptorHeap; generation
// checking stays on the CPU, where the material table already validates it. Encoding lives
// here so no caller hand-writes the shifts.
namespace engine {

constexpr std::uint32_t kMaterialSliceBits = 16;
constexpr std::uint32_t kMaterialSliceMask = 0xFFFFu;
constexpr std::uint32_t kMaterialGenerationMask = 0xFFFFu;
constexpr std::uint32_t kMaterialHandleDefault = 0;

struct MaterialHandleParts {
    std::uint32_t slice = 0;
    std::uint32_t generation = 0;
};

// Returns false when either part does not fit in 16 bits; the caller must not truncate
// silently, because a truncated handle samples the wrong texture.
bool encodeMaterialHandle(std::uint32_t slice, std::uint32_t generation,
                          std::uint32_t& outHandle);

MaterialHandleParts decodeMaterialHandle(std::uint32_t handle);

} // namespace engine

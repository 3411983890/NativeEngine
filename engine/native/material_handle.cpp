#include "material_handle.hpp"

namespace engine {

bool encodeMaterialHandle(std::uint32_t slice, std::uint32_t generation,
                          std::uint32_t& outHandle) {
    if (slice > kMaterialSliceMask || generation > kMaterialGenerationMask) {
        outHandle = kMaterialHandleDefault;
        return false;
    }
    outHandle = (generation << kMaterialSliceBits) | (slice & kMaterialSliceMask);
    return true;
}

MaterialHandleParts decodeMaterialHandle(std::uint32_t handle) {
    MaterialHandleParts parts;
    parts.slice = handle & kMaterialSliceMask;
    parts.generation = (handle >> kMaterialSliceBits) & kMaterialGenerationMask;
    return parts;
}

} // namespace engine

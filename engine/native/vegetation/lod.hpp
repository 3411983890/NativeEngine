#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// LOD selection shared by the CPU reference and the GPU culling shader.
//
// The rule from the plan: a LOD is chosen by projected error, not by distance. Distance
// is only an input to the projection, so a tall tree and a short grass clump at the same
// distance do not get the same level.

namespace engine::vegetation {

struct LodChoice {
    std::uint32_t level = 0;
    float errorPixels = 0;
};

// errorPixels = worldError * instanceScale * viewportHeight / (2 * tan(fovY/2) * depth)
inline float projectErrorPixels(float worldErrorMetres, float instanceScale, float viewportHeight,
                                float tanHalfFovY, float viewDepth) {
    if (!(viewDepth > 1e-3f) || !(tanHalfFovY > 1e-4f)) return 1e9f;
    return worldErrorMetres * instanceScale * viewportHeight / (2.0f * tanHalfFovY * viewDepth);
}

// Selects the coarsest level whose projected error is still below the level's
// threshold, with hysteresis so a camera hovering at a threshold does not flip every
// frame. `previous` < 0 means "no history".
inline LodChoice selectLod(const float* screenErrorPixels, std::uint32_t count, float errorPixels,
                           int previous, float hysteresisFraction = 0.15f) {
    LodChoice choice{0, errorPixels};
    if (count == 0) return choice;
    for (std::uint32_t level = 0; level < count; ++level) {
        float threshold = screenErrorPixels[level];
        if (static_cast<int>(level) == previous) {
            // Stay on the current level a little longer, in both directions. This is the
            // whole point of hysteresis: it removes the frame-to-frame flicker at a
            // boundary without changing the steady-state selection.
            threshold *= 1.0f + hysteresisFraction;
        } else if (previous >= 0 && static_cast<int>(level) < previous) {
            threshold *= 1.0f - hysteresisFraction;
        }
        if (errorPixels <= threshold) { choice.level = level; return choice; }
    }
    choice.level = count - 1;
    return choice;
}

// Stable per-instance hash in [0,1). Grass density LOD uses it so that removing half
// the blades removes the same half every frame and at every camera position.
inline std::uint32_t stableHash(std::uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

inline float stableUnit(std::uint32_t value) { return stableHash(value) / 4294967296.0f; }

// Nested subset: level k keeps an instance only if level k-1 kept it, which is what
// makes a density reduction monotone instead of a re-shuffle.
inline bool densityKeeps(std::uint32_t stableId, std::uint32_t level, const float* fractions, std::uint32_t count) {
    if (level >= count) level = count - 1;
    const float unit = stableUnit(stableId);
    for (std::uint32_t index = 0; index <= level; ++index) if (unit >= fractions[index]) return false;
    return true;
}

} // namespace engine::vegetation

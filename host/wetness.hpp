#pragma once

// Surface wetness history for rocks, banks and beds that touch a water body.
//
// The renderer already has a per-material `wetness`; that value is static. This layer
// keeps a small per-entity history instead: a surface becomes fully wet as soon as the
// water level overlaps it and dries with a time constant when it no longer does. The
// value is presentation only; it never feeds the water solver or the gameplay state.

#include <algorithm>
#include <array>
#include <cmath>

namespace host {

// Five probes (centre plus four inset corners) of the water surface at an entity's
// footprint. `valid` is false outside the water body.
struct WetnessSample {
    bool valid = false;
    float surfaceHeightMetres = 0;
};

struct WetnessState {
    float value = 0;
};

// A large flat ground only counts as wet when its whole footprint is covered; otherwise
// a pond in the middle of a courtyard would darken the entire courtyard. Props and walls
// count when any probe touches water, because a rock is often only partly submerged.
inline float wetnessTarget(const std::array<WetnessSample, 5>& samples,
                           float bottomMetres, bool largeFlat) {
    unsigned wet = 0, valid = 0;
    for (const auto& sample : samples) {
        if (!sample.valid) continue;
        ++valid;
        // Any surface above the footprint's bottom marks it wet: a bed or a fully
        // submerged rock is wet even though the water is above its top face.
        if (sample.surfaceHeightMetres >= bottomMetres - 0.02f) ++wet;
    }
    if (valid == 0) return 0;
    if (largeFlat) return wet == samples.size() ? 1.0f : 0.0f;
    return wet > 0 ? 1.0f : 0.0f;
}

// Wetting is immediate; drying is exponential so the mark does not disappear with the
// first frame in which the probe leaves the water.
inline void updateWetness(WetnessState& state, float target, float dtSeconds,
                          float dryTimeSeconds = 6.0f) {
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0) return;
    target = std::clamp(target, 0.0f, 1.0f);
    if (target >= state.value) {
        state.value = target;
        return;
    }
    const float dry = std::max(dryTimeSeconds, 1e-3f);
    state.value = target + (state.value - target) * std::exp(-dtSeconds / dry);
    if (state.value < 1e-4f) state.value = 0;
}

} // namespace host

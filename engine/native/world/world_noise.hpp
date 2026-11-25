#pragma once

// Deterministic 2D noise for terrain and ecological fields.
//
// No library dependency and no precomputed permutation table: the lattice gradient comes
// from a hash of the integer cell and the seed, so the field is defined for any coordinate
// including negative ones and does not need to be re-created in the same order on both the
// CPU and the GPU. The compiler is the only producer of terrain, so a GPU twin is not
// required; if one is added later it must hash the same way, which is why the hash is a
// named function rather than an inlined expression.

#include "world_types.hpp"

#include <cmath>

namespace engine {
namespace world {
namespace noise {

inline float fade(float t) {
    // Quintic: C2 continuous, so the second derivative of the height does not jump at a
    // cell border. A cubic fade leaves a visible crease once the terrain is shaded.
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline std::uint64_t latticeHash(std::int32_t x, std::int32_t z, std::uint64_t seed) {
    Hasher hasher;
    hasher.value = mix64(seed);
    hasher.i32(x);
    hasher.i32(z);
    return hasher.digest();
}

// Value noise in [-1, 1].
inline float value2D(float x, float z, std::uint64_t seed) {
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iz = static_cast<std::int32_t>(fz);
    const float tx = fade(x - fx);
    const float tz = fade(z - fz);

    const auto sample = [&](std::int32_t cx, std::int32_t cz) {
        const std::uint64_t bits = latticeHash(cx, cz, seed);
        // 24 bits into [-1, 1): the same mantissa width as Rng::unit, so a lattice value and
        // a jittered sample round identically.
        return static_cast<float>(bits >> 40) * (2.0f / 16777216.0f) - 1.0f;
    };

    const float a = sample(ix, iz);
    const float b = sample(ix + 1, iz);
    const float c = sample(ix, iz + 1);
    const float d = sample(ix + 1, iz + 1);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), tz);
}

// Fractal Brownian motion. `octaves` is clamped: 12 octaves at 1 m sampling is already
// below float precision at kilometre coordinates.
inline float fbm(float x, float z, std::uint64_t seed, int octaves = 5,
                 float lacunarity = 2.02f, float gain = 0.5f) {
    if (octaves < 1) octaves = 1;
    if (octaves > 12) octaves = 12;
    float sum = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;
    float frequency = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += value2D(x * frequency, z * frequency, hashCombine(seed, static_cast<std::uint64_t>(octave))) * amplitude;
        normalization += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return normalization > 0.0f ? sum / normalization : 0.0f;
}

// Ridged multifractal: the valley-floor / mountain-ridge look the ancient-valley preset
// wants. `sharpness` controls how narrow the ridges are.
inline float ridged(float x, float z, std::uint64_t seed, int octaves = 4, float sharpness = 1.6f) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    float sum = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;
    float frequency = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        const float n = 1.0f - std::fabs(value2D(x * frequency, z * frequency,
                                                  hashCombine(seed, 0x51ed270bull + static_cast<std::uint64_t>(octave))));
        sum += std::pow(n, sharpness) * amplitude;
        normalization += amplitude;
        amplitude *= 0.52f;
        frequency *= 2.03f;
    }
    return normalization > 0.0f ? sum / normalization : 0.0f;
}

// Distance to the nearest of three jittered feature points in a cell, normalized to
// roughly [0, 1]. Used for rock clumping and for the "is this spot a clearing" mask; it is
// not a general-purpose Voronoi implementation and does not return cell ids.
inline float cellular(float x, float z, std::uint64_t seed) {
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iz = static_cast<std::int32_t>(fz);
    float best = 1.0e9f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::uint64_t bits = latticeHash(ix + dx, iz + dz, seed);
            const float jx = static_cast<float>(bits & 0xffffull) / 65536.0f;
            const float jz = static_cast<float>((bits >> 16) & 0xffffull) / 65536.0f;
            const float px = static_cast<float>(ix + dx) + jx;
            const float pz = static_cast<float>(iz + dz) + jz;
            const float d = std::sqrt((px - x) * (px - x) + (pz - z) * (pz - z));
            if (d < best) best = d;
        }
    }
    return best > 1.0f ? 1.0f : best;
}

// Smooth 0..1 mask used for ecological zones: 1 at `centre`, 0 at |value - centre| >= width.
// `width` is in noise units, so the caller decides the world scale.
inline float band(float value, float centre, float width) {
    if (width <= 0.0f) return value == centre ? 1.0f : 0.0f;
    const float t = std::fabs(value - centre) / width;
    if (t >= 1.0f) return 0.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

} // namespace noise
} // namespace world
} // namespace engine

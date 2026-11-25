#pragma once

#include "asset.hpp"
#include "../vec3.hpp"

#include <cstdint>
#include <vector>

// CPU reference guide simulation (XPBD) and strand interpolation.
//
// This is the reference the GPU path is compared against, not a fallback: the plan
// requires a CPU solve to exist before the GPU one is trusted. It deliberately does the
// expensive, obvious thing — fixed substeps, Jacobi-free sequential constraints, real
// collision projection — so a GPU result can be diffed against it.

namespace engine::groom {

struct CollisionSphere { Vec3 centre; float radius = 0; };
struct CollisionCapsule { Vec3 a, b; float radius = 0; };

// Defaults target short styles (brows, beard). A scalp hairstyle with 15 cm guides stretches
// 1.64x under gravity at these values; `iterations = 12, substeps = 4, compliance = 5e-7,
// bendCompliance = 1e-4` brings the same solve to 1.04x. Tune per style rather than raising
// the defaults, because the short styles would then pay the iteration cost for nothing.
struct SolverSettings {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    float damping = 0.9f;          // velocity retained per second
    float compliance = 2e-6f;      // metres / newton
    float bendCompliance = 4e-4f;
    std::uint32_t iterations = 4;
    std::uint32_t substeps = 2;
    float windResponse = 0.35f;
    float maxSpeedMetresPerSecond = 25.0f;
};

struct SolverStats {
    float maxStretchRatio = 1.0f;      // longest segment / rest length
    float maxPenetrationMetres = 0.0f; // deepest remaining collision penetration
    float maxSpeedMetresPerSecond = 0.0f;
    std::uint32_t steps = 0;
    std::uint32_t points = 0;
};

class GuideSolver {
public:
    explicit GuideSolver(const Groom& groom, std::uint32_t level = 0);

    void setSettings(const SolverSettings& settings) { settings_ = settings; }
    const SolverSettings& settings() const { return settings_; }
    void setWind(Vec3 wind) { wind_ = wind; }
    void setCollision(std::vector<CollisionSphere> spheres, std::vector<CollisionCapsule> capsules);
    void reset();
    // Advances one fixed tick. `dt` is clamped to a bounded range so a long frame cannot
    // explode the solve; the caller is responsible for substepping a paused/resumed
    // clock, which is why the clamp is reported through the stats instead of hidden.
    void step(float dt);

    // Interpolates one render strand from the current guide state. `weights` must come
    // from the groom's GUIDE_WEIGHT table for the same level.
    std::vector<Vec3> interpolate(const GuideWeight& weights, std::uint32_t points) const;
    // Root position of a guide after the solve, used to verify that the root never
    // detaches from the scalp.
    Vec3 guideRoot(std::uint32_t guide) const;
    Vec3 guidePoint(std::uint32_t guide, std::uint32_t point) const;

    SolverStats stats() const { return stats_; }
    // Guides, not constraints. `rest_` is sized guides × segments, so returning its size here
    // made every caller that iterated guides run past the end of the point buffer.
    std::uint32_t guideCount() const { return guides_; }
    std::uint32_t pointCount() const { return points_; }

private:
    void solveConstraints(float dt);
    void resolveCollisions();
    void measure();

    SolverSettings settings_{};
    std::vector<Vec3> position_, previous_, velocity_, restPosition_, tickStart_;
    std::vector<float> inverseMass_;
    std::vector<float> rest_, bendRest_;
    std::vector<Vec3> roots_;
    std::vector<float> restTotal_;
    std::vector<CollisionSphere> spheres_;
    std::vector<CollisionCapsule> capsules_;
    Vec3 wind_{};
    std::uint32_t points_ = 0;
    std::uint32_t guides_ = 0;
    SolverStats stats_{};
};

// Analytic sub-pixel coverage of one strand segment. A strand thinner than a pixel
// contributes partial coverage instead of being widened to a full pixel, which is the
// difference between a soft hairline and a solid band.
float strandCoveragePixels(float widthMetres, float viewDepth, float viewportHeight, float tanHalfFovY);

// Coverage-weighted opacity for a tile sample of K strands, used by the tile
// transparency path. `count` strands of equal width at `depth`; returns [0,1].
float tileCoverage(float widthMetres, float viewDepth, float viewportHeight, float tanHalfFovY, std::uint32_t count);

} // namespace engine::groom

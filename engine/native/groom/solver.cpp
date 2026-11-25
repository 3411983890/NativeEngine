#include "solver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::groom {
namespace {
constexpr float kMinimumLength = 1e-6f;

float distance(Vec3 a, Vec3 b) {
    const auto dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 closestOnSegment(Vec3 point, Vec3 a, Vec3 b) {
    const auto ab = b - a;
    const auto lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    if (lengthSquared < kMinimumLength) return a;
    const auto ap = point - a;
    float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / lengthSquared;
    t = std::max(0.0f, std::min(1.0f, t));
    return a + ab * t;
}
}

GuideSolver::GuideSolver(const Groom& groom, std::uint32_t level) {
    const auto& lod = levelFor(groom, level);
    if (lod.segments < 1) throw Error("NG-E-GROOM-SIMULATION", "groom level has no segments to simulate");
    points_ = lod.segments + 1;
    const auto guides = static_cast<std::uint32_t>(groom.guides.size());
    guides_ = guides;
    position_.resize(static_cast<std::size_t>(guides) * points_);
    previous_.resize(position_.size());
    velocity_.resize(position_.size());
    restPosition_.resize(position_.size());
    tickStart_.resize(position_.size());
    inverseMass_.resize(position_.size(), 1.0f);
    rest_.resize(static_cast<std::size_t>(guides) * lod.segments);
    bendRest_.resize(static_cast<std::size_t>(guides) * std::max(0u, lod.segments - 1));
    roots_.resize(guides);
    restTotal_.resize(guides);
    for (std::uint32_t guide = 0; guide < guides; ++guide) {
        const auto& curve = groom.guides[guide];
        if (curve.pointCount != points_) throw Error("NG-E-GROOM-SIMULATION", "guide point count disagrees with the level segment count");
        float total = 0;
        for (std::uint32_t point = 0; point < points_; ++point) {
            const auto index = static_cast<std::size_t>(guide) * points_ + point;
            position_[index] = curve.points[point];
            previous_[index] = curve.points[point];
            velocity_[index] = {};
            restPosition_[index] = curve.points[point];
            tickStart_[index] = curve.points[point];
            // The root is pinned. A solver that lets the root move detaches the hair from
            // the scalp and no amount of collision fixes it.
            inverseMass_[index] = point == 0 ? 0.0f : 1.0f;
            if (point + 1 < points_) {
                const auto length = distance(curve.points[point], curve.points[point + 1]);
                rest_[static_cast<std::size_t>(guide) * lod.segments + point] = std::max(kMinimumLength, length);
                total += length;
            }
            if (point + 2 < points_) {
                bendRest_[static_cast<std::size_t>(guide) * (lod.segments - 1) + point] =
                    std::max(kMinimumLength, distance(curve.points[point], curve.points[point + 2]));
            }
        }
        roots_[guide] = curve.points.front();
        restTotal_[guide] = total;
    }
    stats_.points = static_cast<std::uint32_t>(position_.size());
}

void GuideSolver::setCollision(std::vector<CollisionSphere> spheres, std::vector<CollisionCapsule> capsules) {
    spheres_ = std::move(spheres);
    capsules_ = std::move(capsules);
}

void GuideSolver::reset() {
    // Restore the rest pose exactly. A "reset" that re-simulates from the current state
    // is not a reset, and a teleport that keeps velocity is how hair explodes on respawn.
    for (std::size_t index = 0; index < position_.size(); ++index) {
        position_[index] = restPosition_[index];
        previous_[index] = restPosition_[index];
        tickStart_[index] = restPosition_[index];
        velocity_[index] = {};
    }
    stats_ = SolverStats{};
    stats_.points = static_cast<std::uint32_t>(position_.size());
}

void GuideSolver::step(float dt) {
    if (!(dt > 0)) return;
    dt = std::min(dt, 1.0f / 30.0f);
    const auto substeps = std::max(1u, settings_.substeps);
    const auto h = dt / static_cast<float>(substeps);
    // Velocity is measured over the whole tick, not the last substep: using the substep
    // start would report a velocity `substeps` times too large.
    tickStart_ = position_;
    for (std::uint32_t substep = 0; substep < substeps; ++substep) {
        // Predict.
        for (std::size_t index = 0; index < position_.size(); ++index) {
            previous_[index] = position_[index];
            if (inverseMass_[index] == 0.0f) continue;
            auto& velocity = velocity_[index];
            velocity = velocity + settings_.gravity * h;
            // Wind is an acceleration, scaled by how far the point is from the root so
            // the base stays planted.
            const auto guide = index / points_;
            const auto point = index % points_;
            const auto t = static_cast<float>(point) / static_cast<float>(std::max(1u, points_ - 1));
            velocity = velocity + wind_ * (settings_.windResponse * t * t * h);
            velocity = velocity * std::pow(std::max(0.0f, std::min(1.0f, settings_.damping)), h);
            const auto speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
            if (speed > settings_.maxSpeedMetresPerSecond) velocity = velocity * (settings_.maxSpeedMetresPerSecond / speed);
            position_[index] = position_[index] + velocity * h;
            (void)guide;
        }
        solveConstraints(h);
        resolveCollisions();
    }
    // Velocity is derived from the actual movement so a constraint or a collision
    // changes it. Keeping the pre-projection velocity is how a solver gains energy.
    const auto inverse = 1.0f / dt;
    for (std::size_t index = 0; index < position_.size(); ++index) {
        if (inverseMass_[index] == 0.0f) { velocity_[index] = {}; continue; }
        velocity_[index] = (position_[index] - tickStart_[index]) * inverse;
    }
    ++stats_.steps;
    measure();
}

void GuideSolver::solveConstraints(float dt) {
    const auto dtSquared = dt * dt;
    const auto alpha = settings_.compliance / std::max(1e-12f, dtSquared);
    const auto bendAlpha = settings_.bendCompliance / std::max(1e-12f, dtSquared);
    const auto segments = points_ - 1;
    for (std::uint32_t iteration = 0; iteration < settings_.iterations; ++iteration) {
        for (std::uint32_t guide = 0; guide < roots_.size(); ++guide) {
            const auto base = static_cast<std::size_t>(guide) * points_;
            for (std::uint32_t segment = 0; segment < segments; ++segment) {
                auto& a = position_[base + segment];
                auto& b = position_[base + segment + 1];
                const auto wa = inverseMass_[base + segment], wb = inverseMass_[base + segment + 1];
                const auto sum = wa + wb;
                if (sum <= 0) continue;
                const auto delta = b - a;
                const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                if (length < kMinimumLength) continue;
                const auto constraint = length - rest_[static_cast<std::size_t>(guide) * segments + segment];
                const auto correction = -constraint / (sum + alpha);
                const auto direction = delta * (1.0f / length);
                if (wa > 0) a = a - direction * (correction * wa);
                if (wb > 0) b = b + direction * (correction * wb);
            }
            // Bending: keeps the strand from folding into a zigzag when a collision pushes
            // a middle point sideways. It is a distance constraint on i..i+2, which is the
            // cheapest form that still resists a sharp kink.
            if (points_ >= 3) {
                const auto bendSegments = points_ - 2;
                for (std::uint32_t segment = 0; segment < bendSegments; ++segment) {
                    auto& a = position_[base + segment];
                    auto& c = position_[base + segment + 2];
                    const auto wa = inverseMass_[base + segment], wc = inverseMass_[base + segment + 2];
                    const auto sum = wa + wc;
                    if (sum <= 0) continue;
                    const auto delta = c - a;
                    const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                    if (length < kMinimumLength) continue;
                    const auto constraint = length - bendRest_[static_cast<std::size_t>(guide) * bendSegments + segment];
                    const auto correction = -constraint / (sum + bendAlpha);
                    const auto direction = delta * (1.0f / length);
                    if (wa > 0) a = a - direction * (correction * wa);
                    if (wc > 0) c = c + direction * (correction * wc);
                }
            }
            // The root is re-pinned every iteration so a collision cannot drag it.
            position_[base] = roots_[guide];
        }
    }
}

void GuideSolver::resolveCollisions() {
    for (std::uint32_t guide = 0; guide < roots_.size(); ++guide) {
        const auto base = static_cast<std::size_t>(guide) * points_;
        for (std::uint32_t point = 1; point < points_; ++point) {
            auto& position = position_[base + point];
            for (const auto& sphere : spheres_) {
                const auto delta = position - sphere.centre;
                const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                if (length < kMinimumLength) { position = sphere.centre + Vec3{0, sphere.radius, 0}; continue; }
                if (length < sphere.radius) position = sphere.centre + delta * (sphere.radius / length);
            }
            for (const auto& capsule : capsules_) {
                const auto closest = closestOnSegment(position, capsule.a, capsule.b);
                const auto delta = position - closest;
                const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                if (length < kMinimumLength) { position = closest + Vec3{0, capsule.radius, 0}; continue; }
                if (length < capsule.radius) position = closest + delta * (capsule.radius / length);
            }
        }
    }
}

void GuideSolver::measure() {
    float worstStretch = 1.0f, worstPenetration = 0.0f, worstSpeed = 0.0f;
    const auto segments = points_ - 1;
    for (std::uint32_t guide = 0; guide < roots_.size(); ++guide) {
        const auto base = static_cast<std::size_t>(guide) * points_;
        for (std::uint32_t point = 0; point < points_; ++point) {
            const auto speed = std::sqrt(velocity_[base + point].x * velocity_[base + point].x
                + velocity_[base + point].y * velocity_[base + point].y
                + velocity_[base + point].z * velocity_[base + point].z);
            worstSpeed = std::max(worstSpeed, speed);
            for (const auto& sphere : spheres_) {
                const auto d = distance(position_[base + point], sphere.centre);
                worstPenetration = std::max(worstPenetration, sphere.radius - d);
            }
            for (const auto& capsule : capsules_) {
                const auto d = distance(position_[base + point], closestOnSegment(position_[base + point], capsule.a, capsule.b));
                worstPenetration = std::max(worstPenetration, capsule.radius - d);
            }
            if (point + 1 < points_) {
                const auto rest = rest_[static_cast<std::size_t>(guide) * segments + point];
                const auto current = distance(position_[base + point], position_[base + point + 1]);
                worstStretch = std::max(worstStretch, current / rest);
            }
        }
    }
    stats_.maxStretchRatio = worstStretch;
    stats_.maxPenetrationMetres = std::max(0.0f, worstPenetration);
    stats_.maxSpeedMetresPerSecond = worstSpeed;
}

std::vector<Vec3> GuideSolver::interpolate(const GuideWeight& weights, std::uint32_t points) const {
    if (points == 0) points = points_;
    std::vector<Vec3> result(points);
    for (std::uint32_t point = 0; point < points; ++point) {
        Vec3 sum{};
        for (int slot = 0; slot < 3; ++slot) {
            const auto guide = weights.guide[slot];
            const auto weight = weights.weight[slot];
            if (weight == 0.0f || guide >= roots_.size()) continue;
            // A coarser LOD has fewer points per guide; sample by parameter so the
            // interpolated strand keeps its shape instead of its index.
            const auto t = points == 1 ? 0.0f : static_cast<float>(point) / static_cast<float>(points - 1);
            const auto source = t * static_cast<float>(points_ - 1);
            const auto lower = static_cast<std::uint32_t>(source);
            const auto upper = std::min(points_ - 1, lower + 1);
            const auto blend = source - static_cast<float>(lower);
            const auto a = position_[static_cast<std::size_t>(guide) * points_ + lower];
            const auto b = position_[static_cast<std::size_t>(guide) * points_ + upper];
            sum = sum + (a * (1.0f - blend) + b * blend) * weight;
        }
        result[point] = sum;
    }
    return result;
}

Vec3 GuideSolver::guideRoot(std::uint32_t guide) const {
    if (guide >= roots_.size()) throw Error("NG-E-GROOM-SIMULATION", "guide index outside the solver");
    return position_[static_cast<std::size_t>(guide) * points_];
}

Vec3 GuideSolver::guidePoint(std::uint32_t guide, std::uint32_t point) const {
    if (guide >= roots_.size() || point >= points_) throw Error("NG-E-GROOM-SIMULATION", "guide point outside the solver");
    return position_[static_cast<std::size_t>(guide) * points_ + point];
}

float strandCoveragePixels(float widthMetres, float viewDepth, float viewportHeight, float tanHalfFovY) {
    if (!(viewDepth > 1e-3f) || !(tanHalfFovY > 1e-4f)) return 0;
    return widthMetres * viewportHeight / (2.0f * tanHalfFovY * viewDepth);
}

float tileCoverage(float widthMetres, float viewDepth, float viewportHeight, float tanHalfFovY, std::uint32_t count) {
    // Coverage of N independent strands in one pixel: 1 - (1 - c)^N. This is the number
    // the tile transparency path needs, and it is why K=4 and K=8 are not interchangeable
    // for a dense groom: the difference is in the coverage, not just in the sample count.
    const auto single = std::max(0.0f, std::min(1.0f, strandCoveragePixels(widthMetres, viewDepth, viewportHeight, tanHalfFovY)));
    return 1.0f - std::pow(1.0f - single, static_cast<float>(count));
}

} // namespace engine::groom

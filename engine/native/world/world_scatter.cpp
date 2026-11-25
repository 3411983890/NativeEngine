#include "world_scatter.hpp"

#include "world_noise.hpp"

#include <algorithm>
#include <cmath>

namespace engine {
namespace world {
namespace {

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

float smoothstep01(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

float distanceToSegment(float ax, float az, float bx, float bz, float px, float pz) {
    const float dx = bx - ax;
    const float dz = bz - az;
    const float lengthSquared = dx * dx + dz * dz;
    if (lengthSquared <= 1.0e-12f) return std::sqrt((px - ax) * (px - ax) + (pz - az) * (pz - az));
    float t = ((px - ax) * dx + (pz - az) * dz) / lengthSquared;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return std::sqrt((px - (ax + dx * t)) * (px - (ax + dx * t)) + (pz - (az + dz * t)) * (pz - (az + dz * t)));
}

bool pointInPolygon(const std::vector<PathPoint>& polygon, float x, float z) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
        const float xi = polygon[i].x, zi = polygon[i].z;
        const float xj = polygon[previous].x, zj = polygon[previous].z;
        if (((zi > z) != (zj > z))
            && (x < (xj - xi) * (z - zi) / ((zj - zi) == 0.0f ? 1.0e-12f : (zj - zi)) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

constexpr std::uint32_t kPurposeScatter = 0x53434154u; // 'SCAT'

} // namespace

float scatterCellSize(float densityPerHectare, float minSpacingMetres) {
    // 1 hectare is 10,000 m². One candidate per cell means cellSide² = 10000 / density.
    float side = 4.0f;
    if (densityPerHectare > 0.0f) side = std::sqrt(10000.0f / densityPerHectare);
    if (minSpacingMetres > 0.0f) side = std::max(side, minSpacingMetres);
    return std::max(0.05f, std::min(200.0f, side));
}

float DensityRegion::weight(float x, float z) const {
    if (!bounds.contains(x, z)) return 0.0f;
    if (shape == TerrainEditShape::Circle) {
        const float dx = x - centreX;
        const float dz = z - centreZ;
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance >= radiusMetres) return 0.0f;
        if (falloffMetres <= 0.0f) return 1.0f;
        const float inner = std::max(0.0f, radiusMetres - falloffMetres);
        if (distance <= inner) return 1.0f;
        return 1.0f - smoothstep01((distance - inner) / (radiusMetres - inner));
    }
    if (shape == TerrainEditShape::Rectangle) {
        if (falloffMetres <= 0.0f) return 1.0f;
        const float edge = std::min(std::min(x - bounds.minX, bounds.maxX - x),
                                    std::min(z - bounds.minZ, bounds.maxZ - z));
        return smoothstep01(edge / falloffMetres);
    }
    if (pointInPolygon(polygon, x, z)) {
        if (falloffMetres <= 0.0f) return 1.0f;
        float nearest = 1.0e30f;
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            nearest = std::min(nearest, distanceToSegment(a.x, a.z, b.x, b.z, x, z));
        }
        return smoothstep01(nearest / falloffMetres);
    }
    if (falloffMetres <= 0.0f) return 0.0f;
    float nearest = 1.0e30f;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        nearest = std::min(nearest, distanceToSegment(a.x, a.z, b.x, b.z, x, z));
    }
    if (nearest >= falloffMetres) return 0.0f;
    return 1.0f - smoothstep01(nearest / falloffMetres);
}

void ScatterField::configure(const TerrainSurface* surface, const std::vector<ScatterRule>* rules,
                             const std::vector<DensityRegion>* regions,
                             const std::vector<ExclusionZone>* exclusions,
                             std::uint64_t worldSeed) {
    surface_ = surface;
    rules_ = rules;
    regions_ = regions;
    exclusions_ = exclusions;
    worldSeed_ = worldSeed;
}

std::uint64_t scatterCellPriority(const ScatterRule& rule, std::uint64_t worldSeed,
                                  std::int32_t cellX, std::int32_t cellZ) {
    return deriveStream(worldSeed, rule.seedStream, rule.layerId, cellX, cellZ, kPurposeScatter);
}

bool ScatterField::evaluateCell(const ScatterRule& rule, std::int32_t cellX, std::int32_t cellZ,
                                float cellSizeMetres, float& outX, float& outZ,
                                std::uint64_t& outId, std::uint64_t& outPriority) const {
    const std::uint64_t priority = scatterCellPriority(rule, worldSeed_, cellX, cellZ);
    outPriority = priority;
    Rng rng(priority, 0x9e3779b97f4a7c15ull);
    // Jitter inside the cell. 0.98 keeps the point strictly inside its own cell, so the
    // spacing filter never has to reason about a candidate that drifted into a neighbour.
    const float jitterX = rng.range(0.01f, 0.99f);
    const float jitterZ = rng.range(0.01f, 0.99f);
    outX = (static_cast<float>(cellX) + jitterX) * cellSizeMetres;
    outZ = (static_cast<float>(cellZ) + jitterZ) * cellSizeMetres;
    outId = priority;
    return true;
}

float ScatterField::densityAt(float x, float z) const {
    float multiplier = 1.0f;
    if (!regions_) return multiplier;
    for (const auto& region : *regions_) {
        const float w = region.weight(x, z);
        if (w <= 0.0f) continue;
        if (region.erase) multiplier *= 1.0f - clamp01(w * region.strength);
        else multiplier *= 1.0f + w * region.strength;
    }
    return std::max(0.0f, std::min(8.0f, multiplier));
}

bool ScatterField::excluded(float x, float z, std::uint8_t category) const {
    if (!exclusions_) return false;
    for (const auto& zone : *exclusions_) {
        if (!zone.contains(x, z)) continue;
        if (zone.mask & category) return true;
    }
    return false;
}

void ScatterField::generateChunk(ChunkCoord coord, float chunkSizeMetres,
                                 std::vector<WorldInstance>& out, ScatterStats& stats) const {
    out.clear();
    stats = ScatterStats{};
    if (!surface_ || !rules_) return;

    const Bounds2 core = chunkBoundsOf(coord, chunkSizeMetres);

    for (const auto& rule : *rules_) {
        const float cellSize = scatterCellSize(rule.densityPerHectare, rule.minSpacingMetres);
        const float halo = std::max({rule.exclusionRadiusMetres, rule.minSpacingMetres, 4.0f});
        const Bounds2 work{core.minX - halo, core.minZ - halo, core.maxX + halo, core.maxZ + halo};

        const std::int32_t minCx = static_cast<std::int32_t>(std::floor(work.minX / cellSize));
        const std::int32_t maxCx = static_cast<std::int32_t>(std::floor(work.maxX / cellSize));
        const std::int32_t minCz = static_cast<std::int32_t>(std::floor(work.minZ / cellSize));
        const std::int32_t maxCz = static_cast<std::int32_t>(std::floor(work.maxZ / cellSize));

        // Rings of neighbour cells that can contain a point inside the spacing radius.
        const int ringReach = rule.minSpacingMetres > 0.0f
            ? std::max(1, static_cast<int>(std::ceil(rule.minSpacingMetres / cellSize)) + 1)
            : 0;

        const std::uint8_t category = rule.exclusionCategory;

        for (std::int32_t cz = minCz; cz <= maxCz; ++cz) {
            for (std::int32_t cx = minCx; cx <= maxCx; ++cx) {
                float x = 0.0f, z = 0.0f;
                std::uint64_t id = 0, priority = 0;
                evaluateCell(rule, cx, cz, cellSize, x, z, id, priority);
                // Only the chunk that owns the centre emits the instance.
                if (x < core.minX || x >= core.maxX || z < core.minZ || z >= core.maxZ) continue;
                stats.candidates += 1;

                // Density: the grid is sized for the *authored* density, so a paint region
                // can raise it above 1 and a budget cut can push the threshold up.
                Rng densityRng(priority ^ 0x5bf03635ull, 0x2545f4914f6cdd1dull);
                const float roll = densityRng.unit();
                float threshold = 1.0f;
                if (rule.densityPerHectare <= 0.0f) threshold = 0.0f;
                const float multiplier = densityAt(x, z);
                const float effective = clamp01(multiplier);
                if (rule.clumping > 0.0f) {
                    const float clump = noise::cellular(x * 0.06f, z * 0.06f, worldSeed_ ^ rule.seedStream);
                    threshold *= (1.0f - rule.clumping) + rule.clumping * (1.0f - clump);
                }
                if (roll > effective * threshold) { stats.rejectedDensity += 1; continue; }

                if (rule.edgeFalloffMetres > 0.0f) {
                    const float edge = std::min(std::min(x - core.minX, core.maxX - x),
                                                std::min(z - core.minZ, core.maxZ - z));
                    if (edge < rule.edgeFalloffMetres) {
                        const float keep = smoothstep01(edge / rule.edgeFalloffMetres);
                        if (roll > keep) { stats.rejectedDensity += 1; continue; }
                    }
                }

                // Blue-noise filter. A higher-priority neighbour that lands inside the
                // spacing radius wins; ties break on the cell index so the decision is
                // total and does not depend on evaluation order.
                if (ringReach > 0) {
                    bool rejected = false;
                    for (int dz = -ringReach; dz <= ringReach && !rejected; ++dz) {
                        for (int dx = -ringReach; dx <= ringReach && !rejected; ++dx) {
                            if (dx == 0 && dz == 0) continue;
                            float nx = 0.0f, nz = 0.0f;
                            std::uint64_t nid = 0, npriority = 0;
                            evaluateCell(rule, cx + dx, cz + dz, cellSize, nx, nz, nid, npriority);
                            const float distance = std::sqrt((nx - x) * (nx - x) + (nz - z) * (nz - z));
                            if (distance >= rule.minSpacingMetres) continue;
                            const bool neighbourWins = npriority > priority
                                || (npriority == priority
                                    && (cz + dz > cz || (cz + dz == cz && cx + dx > cx)));
                            if (neighbourWins) rejected = true;
                        }
                    }
                    if (rejected) { stats.rejectedSpacing += 1; continue; }
                }

                const SurfaceSample surfaceSample = surface_->sample(x, z);
                if (surfaceSample.slopeDegrees > rule.maxSlopeDegrees) { stats.rejectedSlope += 1; continue; }
                if (rule.avoidWater && surfaceSample.submerged) { stats.rejectedWater += 1; continue; }
                if (rule.avoidRoads && surfaceSample.onRoad) { stats.rejectedRoad += 1; continue; }
                if (excluded(x, z, category)) { stats.rejectedExclusion += 1; continue; }

                Rng placementRng(priority, 0x94d049bb133111ebull);
                WorldInstance instance;
                instance.position[0] = x;
                // Ground contact: the instance pivot sits on the surface, so a tree on a
                // slope does not float at one side and sink at the other.
                instance.position[1] = surfaceSample.height;
                instance.position[2] = z;
                instance.yaw = placementRng.range(0.0f, 6.2831853f);
                instance.scale = placementRng.range(rule.scaleMin, rule.scaleMax);
                instance.component = rule.component;
                instance.lod = 0;
                instance.flags = rule.alignToSlope ? kInstanceAlignedToSlope : 0u;
                instance.stableId = id;
                out.push_back(instance);
                stats.accepted += 1;
            }
        }
    }
}

} // namespace world
} // namespace engine

#include "world_settlement.hpp"

#include "world_noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine {
namespace world {
namespace {

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }


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

constexpr std::uint32_t kPurposeLot = 0x4c4f5453u; // 'LOTS'

// Axis-aligned overlap of two rotated rectangles, evaluated by projecting onto the four
// axes. A full SAT is overkill: buildings are near-axis-aligned by construction because
// they face the road, and the corner cases are covered by the extra spacing margin.
bool rotatedRectsOverlap(float ax, float az, float aw, float ad, float ayaw,
                         float bx, float bz, float bw, float bd, float byaw, float margin) {
    const auto project = [](float cx, float cz, float w, float d, float yaw, float axisX, float axisZ,
                            float& low, float& high) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        const float ex = (w * 0.5f) * c + (d * 0.5f) * s;
        const float ez = (w * 0.5f) * s + (d * 0.5f) * c;
        const float centre = cx * axisX + cz * axisZ;
        const float radius = std::fabs(ex * axisX) + std::fabs(ez * axisZ);
        low = centre - radius;
        high = centre + radius;
    };
    const float axes[4][2] = {
        {1.0f, 0.0f}, {0.0f, 1.0f},
        {std::cos(ayaw), std::sin(ayaw)}, {std::cos(byaw), std::sin(byaw)},
    };
    for (const auto& axis : axes) {
        float alo = 0, ahi = 0, blo = 0, bhi = 0;
        project(ax, az, aw, ad, ayaw, axis[0], axis[1], alo, ahi);
        project(bx, bz, bw, bd, byaw, axis[0], axis[1], blo, bhi);
        if (ahi + margin < blo || bhi + margin < alo) return false;
    }
    return true;
}

} // namespace

bool SettlementRegion::contains(float x, float z) const {
    if (!bounds.contains(x, z)) return false;
    switch (shape) {
        case TerrainEditShape::Circle: {
            const float dx = x - centreX;
            const float dz = z - centreZ;
            return dx * dx + dz * dz <= radiusMetres * radiusMetres;
        }
        case TerrainEditShape::Rectangle:
            return true; // bounds already checked
        case TerrainEditShape::Polygon:
            return pointInPolygon(polygon, x, z);
    }
    return false;
}

void SettlementPlanner::configure(const TerrainSurface* surface, const PathNetwork* paths,
                                  const std::vector<SettlementParams>* settlements,
                                  const std::vector<SettlementRegion>* regions,
                                  const std::vector<ExclusionZone>* exclusions,
                                  std::uint64_t worldSeed) {
    surface_ = surface;
    paths_ = paths;
    settlements_ = settlements;
    regions_ = regions;
    exclusions_ = exclusions;
    worldSeed_ = worldSeed;
    buildings_.clear();
    diagnostics_.clear();
    stats_ = SettlementStats{};
}

void SettlementPlanner::plan(const std::vector<std::array<float, 2>>& componentSizes,
                             const std::vector<float>& componentHeights) {
    buildings_.clear();
    diagnostics_.clear();
    stats_ = SettlementStats{};
    if (!surface_ || !paths_ || !settlements_ || settlements_->empty()) return;
    if (!regions_ || regions_->empty()) {
        diagnostics_.push_back({"SETTLEMENT_NO_REGION",
                                "a settlement layer has no region to build in", settlements_->front().layerId,
                                0.0f, 0.0f,
                                "Paint a settlement area, or the planner has nowhere to put a lot."});
        return;
    }

    for (const auto& params : *settlements_) {
        for (const auto& path : paths_->paths()) {
            if (path.spec.kind != PathKind::Road) continue;
            planAlongRoad(params, path, componentSizes, componentHeights);
        }
    }
}

void SettlementPlanner::planAlongRoad(const SettlementParams& params, const CompiledPath& road,
                                      const std::vector<std::array<float, 2>>& sizes,
                                      const std::vector<float>& heights) {
    if (road.samples.size() < 2) return;
    if (params.components.empty()) return;

    const float lotSpacing = std::max(4.0f, params.lotSizeMetres);
    // Walk in arc-length steps so a curved road keeps the same lot spacing instead of
    // bunching at the inside of every bend.
    float travelled = lotSpacing;
    std::size_t sampleIndex = 0;
    std::uint32_t ordinal = 0;
    while (travelled < road.length && static_cast<int>(buildings_.size()) < params.maxBuildings) {
        while (sampleIndex + 1 < road.samples.size() && road.samples[sampleIndex + 1].distance < travelled) {
            sampleIndex += 1;
        }
        if (sampleIndex + 1 >= road.samples.size()) break;
        const auto& a = road.samples[sampleIndex];
        const auto& b = road.samples[sampleIndex + 1];
        const float span = b.distance - a.distance;
        const float t = span > 1.0e-5f ? (travelled - a.distance) / span : 0.0f;
        const float cx = a.x + (b.x - a.x) * t;
        const float cz = a.z + (b.z - a.z) * t;
        const float tx = a.tx;
        const float tz = a.tz;
        // Left normal in XZ: (-tz, tx). Buildings on the left face right, and vice versa.
        const float nx = -tz;
        const float nz = tx;

        for (int side = 0; side < 2; ++side) {
            if (static_cast<int>(buildings_.size()) >= params.maxBuildings) break;
            const float sign = side == 0 ? 1.0f : -1.0f;

            Rng rng(deriveStream(worldSeed_, params.seedStream, params.layerId, ordinal,
                                 side, kPurposeLot), 0x1234567890abcdefull);
            ordinal += 1;

            // Not every lot gets a building: `density` is the probability a lot is used,
            // which is what leaves gaps and courtyards instead of a solid wall of houses.
            if (rng.unit() > clamp01(params.density)) continue;
            stats_.lotsConsidered += 1;

            const std::uint16_t component = params.components[rng.below(static_cast<std::uint32_t>(params.components.size()))];
            const float width = component < sizes.size() ? sizes[component][0] : 10.0f;
            const float depth = component < sizes.size() ? sizes[component][1] : 10.0f;
            const float height = component < heights.size() ? heights[component] : 6.0f;

            const float offset = params.roadWidthMetres * 0.5f + params.setbackMetres + depth * 0.5f;
            const float x = cx + nx * offset * sign;
            const float z = cz + nz * offset * sign;

            // Inside a settlement region?
            bool inside = false;
            for (const auto& region : *regions_) {
                if (region.contains(x, z)) { inside = true; break; }
            }
            if (!inside) { stats_.rejectedRegion += 1; continue; }

            // Facing the road: yaw is the road tangent rotated to point away from the road.
            const float yaw = std::atan2(nx * sign, nz * sign);

            const SurfaceSample sample = surface_->sample(x, z, 2.0f);
            if (sample.slopeDegrees > params.maxSlopeDegrees) { stats_.rejectedSlope += 1; continue; }
            if (sample.submerged) { stats_.rejectedWater += 1; continue; }
            if (sample.onRoad || sample.onBridge) { stats_.rejectedSpacing += 1; continue; }

            if (exclusions_) {
                bool blocked = false;
                for (const auto& zone : *exclusions_) {
                    if (zone.contains(x, z)) { blocked = true; break; }
                }
                if (blocked) { stats_.rejectedRegion += 1; continue; }
            }

            if (!footprintFree(x, z, width, depth, yaw, params.buildingSpacingMetres)) {
                stats_.rejectedSpacing += 1;
                continue;
            }

            // The entrance sits on the road-facing edge. A door that opens into a 30° slope
            // is recorded as disconnected rather than moved: moving it would silently put
            // the door on the back of the house.
            const float entranceX = x - nx * sign * (depth * 0.5f + 0.6f);
            const float entranceZ = z - nz * sign * (depth * 0.5f + 0.6f);
            const PathHit doorHit = paths_->nearestOfKind(entranceX, entranceZ, 64.0f, PathKind::Road);
            const bool connected = doorHit.valid()
                && doorHit.distance <= doorHit.width * 0.5f + params.entranceClearanceMetres;
            if (!connected) {
                stats_.disconnectedEntrances += 1;
                char message[192];
                std::snprintf(message, sizeof(message),
                              "entrance at (%.1f, %.1f) is %.1f m from the nearest road",
                              entranceX, entranceZ, doorHit.valid() ? doorHit.distance : 999.0f);
                diagnostics_.push_back({"SETTLEMENT_ENTRANCE_DISCONNECTED", message, params.layerId,
                                        entranceX, entranceZ,
                                        "Widen the setback allowance or move the road closer."});
            }

            BuildingRecord record;
            record.layerId = params.layerId;
            record.component = component;
            record.position[0] = x;
            record.position[1] = sample.height;
            record.position[2] = z;
            record.yaw = yaw;
            record.scale = rng.range(params.scaleMin, params.scaleMax);
            record.footprintMetres[0] = width;
            record.footprintMetres[1] = depth;
            record.heightMetres = height;
            record.entrance[0] = entranceX;
            record.entrance[1] = sample.height;
            record.entrance[2] = entranceZ;
            record.entranceYaw = yaw;
            record.entranceConnected = connected;
            record.roadDistanceMetres = doorHit.valid() ? doorHit.distance : 999.0f;
            record.stableId = deriveStream(worldSeed_, params.seedStream, params.layerId, ordinal, side, 0x4255494cu);
            char id[96];
            std::snprintf(id, sizeof(id), "%s/building/%u/%d", params.layerId.c_str(), ordinal, side);
            record.id = id;
            buildings_.push_back(record);
            stats_.buildingsPlaced += 1;
        }
        travelled += lotSpacing;
    }
}

bool SettlementPlanner::footprintFree(float x, float z, float width, float depth, float yaw, float spacing) const {
    for (const auto& existing : buildings_) {
        if (rotatedRectsOverlap(x, z, width, depth, yaw,
                                existing.position[0], existing.position[2],
                                existing.footprintMetres[0], existing.footprintMetres[1],
                                existing.yaw, spacing)) {
            return false;
        }
    }
    return true;
}

} // namespace world
} // namespace engine

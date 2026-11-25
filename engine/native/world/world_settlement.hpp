#pragma once

// Settlement planning: lots and buildings along a road network.
//
// The planner is deliberately single-pass. A layout that iterates "roads react to buildings
// react to roads" can oscillate, and the plan calls that out as a risk. Instead:
//
//   1. Roads are fixed first — either authored by the user or generated from the settlement
//      region's main axis.
//   2. Lots are swept along each road at a fixed interval, on both sides.
//   3. Each lot is tested once against slope, water, setback and spacing. It is either
//      placed or recorded as a failure with a reason.
//
// A failed lot is a diagnostic, never a silently skipped one: "no room for a house here"
// has to be visible on the map or the user cannot fix it.

#include "world_path.hpp"
#include "world_scatter.hpp"
#include "world_terrain.hpp"
#include "world_types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace world {

struct SettlementParams {
    std::string layerId;
    // Which component categories may be placed, in preference order.
    std::vector<std::uint16_t> components;
    float lotSizeMetres = 20.0f;
    float setbackMetres = 2.5f;
    float roadWidthMetres = 5.0f;
    float entranceClearanceMetres = 3.0f;
    float maxSlopeDegrees = 8.0f;
    float buildingSpacingMetres = 4.0f;
    int maxBuildings = 256;
    float density = 0.75f;
    float scaleMin = 0.92f;
    float scaleMax = 1.08f;
    std::uint64_t seedStream = 0;
};

// A region where building is allowed. Outside every region nothing is placed, which is how
// "draw a settlement area and let it fill" works without a global density knob.
struct SettlementRegion {
    TerrainEditShape shape = TerrainEditShape::Circle;
    Bounds2 bounds{};
    float centreX = 0.0f, centreZ = 0.0f;
    float radiusMetres = 0.0f;
    std::vector<PathPoint> polygon;
    bool contains(float x, float z) const;
};

struct BuildingRecord {
    std::string id;
    std::string layerId;
    std::uint16_t component = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float scale = 1.0f;
    // Width along the road, depth away from it.
    float footprintMetres[2] = {0.0f, 0.0f};
    float heightMetres = 0.0f;
    // Door position and facing. The runtime uses this to verify the player can reach the
    // door from the road; a building whose entrance faces a cliff is a failed lot.
    float entrance[3] = {0.0f, 0.0f, 0.0f};
    float entranceYaw = 0.0f;
    bool entranceConnected = false;
    float roadDistanceMetres = 0.0f;
    std::uint64_t stableId = 0;
};

struct SettlementDiagnostic {
    std::string code;
    std::string message;
    std::string layerId;
    float x = 0.0f, z = 0.0f;
    std::string suggestion;
};

struct SettlementStats {
    std::uint64_t lotsConsidered = 0;
    std::uint64_t buildingsPlaced = 0;
    std::uint64_t rejectedSlope = 0;
    std::uint64_t rejectedWater = 0;
    std::uint64_t rejectedSpacing = 0;
    std::uint64_t rejectedRegion = 0;
    std::uint64_t rejectedBudget = 0;
    std::uint64_t disconnectedEntrances = 0;
};

class SettlementPlanner {
public:
    void configure(const TerrainSurface* surface, const PathNetwork* paths,
                   const std::vector<SettlementParams>* settlements,
                   const std::vector<SettlementRegion>* regions,
                   const std::vector<ExclusionZone>* exclusions,
                   std::uint64_t worldSeed);

    // Plans every settlement. `componentSize` gives the footprint of each component index;
    // an index outside the array is treated as a 10 m × 10 m prefab.
    void plan(const std::vector<std::array<float, 2>>& componentSizes,
              const std::vector<float>& componentHeights);

    const std::vector<BuildingRecord>& buildings() const { return buildings_; }
    const std::vector<SettlementDiagnostic>& diagnostics() const { return diagnostics_; }
    const SettlementStats& stats() const { return stats_; }

private:
    void planAlongRoad(const SettlementParams& params, const CompiledPath& road,
                       const std::vector<std::array<float, 2>>& sizes,
                       const std::vector<float>& heights);
    bool footprintFree(float x, float z, float width, float depth, float yaw, float spacing) const;

    const TerrainSurface* surface_ = nullptr;
    const PathNetwork* paths_ = nullptr;
    const std::vector<SettlementParams>* settlements_ = nullptr;
    const std::vector<SettlementRegion>* regions_ = nullptr;
    const std::vector<ExclusionZone>* exclusions_ = nullptr;
    std::uint64_t worldSeed_ = 0;

    std::vector<BuildingRecord> buildings_;
    std::vector<SettlementDiagnostic> diagnostics_;
    SettlementStats stats_{};
};

} // namespace world
} // namespace engine

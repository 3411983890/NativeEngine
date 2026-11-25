#pragma once

// Deterministic scatter: grass, trees, rocks and decoration.
//
// The hard part is not "place random points". It is placing them so that two neighbouring
// chunks, compiled independently and possibly in a different order, agree about every
// point near their shared border. This module gets that by making acceptance a pure
// function of the global cell:
//
//   * Candidates live on a fixed world grid, sized from the rule's density. Cell (i, j)
//     always produces the same candidate position for the same seed.
//   * Minimum spacing is resolved with a deterministic priority filter. A candidate is
//     rejected when a *higher-priority* neighbour cell produced a point inside its spacing
//     radius. Priorities are hashes, so the winner does not depend on iteration order.
//   * A chunk generates into a halo wider than the largest exclusion radius and then keeps
//     only the points whose centre falls inside its own bounds. Points near the border are
//     therefore evaluated against the same neighbours from both sides.
//
// No instance is ever stored as a scene entity. A 2 km world has millions of grass
// candidates and the scene document has a 10,000-entity limit; these records live in the
// chunk file and are drawn as instanced geometry.

#include "world_terrain.hpp"
#include "world_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace world {

// 32-byte record, identical to the chunk file layout and to the GPU instance stride.
struct WorldInstance {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float scale = 1.0f;
    std::uint16_t component = 0;
    std::uint8_t lod = 0;
    std::uint8_t flags = 0;
    // Stable identity. Derived from (layer, cell, ordinal), never from a list index, so a
    // hand edit stored against this id survives a regeneration that changes the order.
    std::uint64_t stableId = 0;
};
static_assert(sizeof(WorldInstance) == 32, "WorldInstance is a file and GPU layout contract");

enum InstanceFlags : std::uint8_t {
    kInstanceAlignedToSlope = 1u << 0,
    kInstanceLocked = 1u << 1,
    kInstanceMoved = 1u << 2,
    kInstanceHandPlaced = 1u << 3,
};

struct ScatterRule {
    std::string layerId;
    std::uint16_t component = 0;
    // Expected instances per hectare. The compiler converts it to a grid cell size; it
    // never loops "density times area" times, which would be unbounded for grass.
    float densityPerHectare = 100.0f;
    float maxSlopeDegrees = 35.0f;
    float minSpacingMetres = 0.0f;
    float exclusionRadiusMetres = 0.0f;
    float edgeFalloffMetres = 0.0f;
    float clumping = 0.0f;
    float scaleMin = 0.9f;
    float scaleMax = 1.1f;
    bool alignToSlope = false;
    bool avoidWater = true;
    bool avoidRoads = true;
    bool keepUpright = true;
    std::uint64_t seedStream = 0;
    // Which categories this rule blocks. Buildings and roads add themselves to the
    // exclusion set; a rule says what it cares about.
    bool blockedByBuildings = true;
    bool blockedByRoads = true;
    // Which exclusion bit this rule respects. A grass rule and a tree rule do not have to
    // be blocked by the same zones.
    std::uint8_t exclusionCategory = 1u << 0;
};

// A region that suppresses or boosts density. Painted areas use the same shape language as
// the terrain brush so the UI has one set of controls.
struct DensityRegion {
    TerrainEditShape shape = TerrainEditShape::Circle;
    Bounds2 bounds{};
    float centreX = 0.0f, centreZ = 0.0f;
    float radiusMetres = 0.0f;
    float falloffMetres = 0.0f;
    float strength = 1.0f;
    bool erase = false;
    std::vector<PathPoint> polygon;
    float weight(float x, float z) const;
};

// An area no scatter may enter: building footprints, bridge approaches, spawn points.
struct ExclusionZone {
    Bounds2 bounds{};
    std::uint8_t mask = 0xff; // which categories are blocked; 0xff = everything
    bool contains(float x, float z) const { return bounds.contains(x, z); }
};

enum ExclusionCategory : std::uint8_t {
    kExcludeGrass = 1u << 0,
    kExcludeTrees = 1u << 1,
    kExcludeRocks = 1u << 2,
    kExcludeDecor = 1u << 3,
    kExcludeAll = 0xff,
};

struct ScatterStats {
    std::uint64_t candidates = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejectedSlope = 0;
    std::uint64_t rejectedWater = 0;
    std::uint64_t rejectedRoad = 0;
    std::uint64_t rejectedExclusion = 0;
    std::uint64_t rejectedSpacing = 0;
    std::uint64_t rejectedDensity = 0;
    std::uint64_t rejectedBudget = 0;
    // Fraction of the authored density that survived the budget. Reported so "my forest is
    // thinner than the number I typed" has an answer instead of a shrug.
    float budgetScale = 1.0f;
};

class ScatterField {
public:
    void configure(const TerrainSurface* surface, const std::vector<ScatterRule>* rules,
                   const std::vector<DensityRegion>* regions,
                   const std::vector<ExclusionZone>* exclusions,
                   std::uint64_t worldSeed);

    // Generates the instances whose *centre* lands inside `core`. Candidates are evaluated
    // in a halo so spacing decisions agree with the neighbouring chunk.
    void generateChunk(ChunkCoord coord, float chunkSizeMetres,
                       std::vector<WorldInstance>& out, ScatterStats& stats) const;

    // One candidate at a world cell. Public so the tests can assert that the same cell
    // yields the same position regardless of which chunk asked for it.
    bool evaluateCell(const ScatterRule& rule, std::int32_t cellX, std::int32_t cellZ,
                      float cellSizeMetres, float& outX, float& outZ, std::uint64_t& outId,
                      std::uint64_t& outPriority) const;

private:
    float densityAt(float x, float z) const;
    bool excluded(float x, float z, std::uint8_t category) const;

    const TerrainSurface* surface_ = nullptr;
    const std::vector<ScatterRule>* rules_ = nullptr;
    const std::vector<DensityRegion>* regions_ = nullptr;
    const std::vector<ExclusionZone>* exclusions_ = nullptr;
    std::uint64_t worldSeed_ = 0;
};

// Cell size for a rule: the side of a square that holds one candidate at the requested
// density. Clamped so a rule cannot ask for a grid finer than 5 cm or coarser than 200 m.
float scatterCellSize(float densityPerHectare, float minSpacingMetres);

// The priority of a cell, used by the spacing filter. Exposed for tests.
std::uint64_t scatterCellPriority(const ScatterRule& rule, std::uint64_t worldSeed,
                                  std::int32_t cellX, std::int32_t cellZ);

} // namespace world
} // namespace engine

#pragma once

// River and road path network.
//
// A path is authored as a handful of semantic control points and compiled into a dense
// centre line. Three things make the geometry usable by the rest of the compiler:
//
//   * The centre line is a Catmull-Rom spline, not the control polygon, so a road bends
//     instead of turning in straight segments with a visible kink at every node.
//   * Every sample carries an arc length, a tangent and a left normal, so a cross-section
//     can be swept without recomputing them per chunk.
//   * The distance field is queried in world space, never per chunk. A chunk asks "how far
//     is (x, z) from the road" and gets the same answer as its neighbour, which is what
//     keeps the flattening seam-free.
//
// Queries go through a uniform grid of sample indices. A 2 km world with 40 km of path at
// 0.5 m spacing is 80k samples; scanning them per terrain vertex would be 10^11 operations
// per cook, so the grid is not an optimisation but a requirement.

#include "world_types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace world {

class TerrainField;

enum class PathKind : std::uint8_t { River = 0, Road = 1 };

struct PathPoint {
    float x = 0.0f;
    float z = 0.0f;
};

struct PathSpec {
    std::string id;
    PathKind kind = PathKind::Road;
    std::vector<PathPoint> control;
    float widthMetres = 3.0f;
    float shoulderMetres = 0.5f;
    float bedDepthMetres = 0.6f;
    float flowMetresPerSecond = 1.0f;
    float maxSlopeDegrees = 20.0f;
    bool closed = false;
    bool bridge = false;
    bool visible = true;
};

struct PathSample {
    float x = 0.0f;
    float z = 0.0f;
    // Unit tangent in XZ.
    float tx = 1.0f;
    float tz = 0.0f;
    // Arc length from the start of the path.
    float distance = 0.0f;
    // Terrain height the path is authored against. For a road this is the smoothed profile
    // the surface is flattened to; for a river it is the water surface.
    float centreHeight = 0.0f;
};

struct CompiledPath {
    PathSpec spec;
    std::vector<PathSample> samples;
    float length = 0.0f;
    // Water surface height, rivers only. Descends monotonically from source to outlet.
    bool waterValid = false;
    // Bridge decks that belong to this path, as index ranges into `samples`. A road inside
    // a span is not flattened into the terrain: the deck carries it.
    struct DeckSpan { std::size_t first = 0, last = 0; float deckHeight = 0.0f; };
    std::vector<DeckSpan> decks;
};

// Result of the nearest-path query. `distance` is measured to the centre line.
struct PathHit {
    int pathIndex = -1;
    float distance = 1.0e9f;
    float alongMetres = 0.0f;
    float centreHeight = 0.0f;
    float waterHeight = 0.0f;
    float width = 0.0f;
    float bedDepth = 0.0f;
    PathKind kind = PathKind::Road;
    bool onDeck = false;
    bool valid() const { return pathIndex >= 0; }
};

struct PathDiagnostic {
    std::string code;
    std::string message;
    std::string pathId;
    std::string suggestion;
};

// A place where a road crosses a river. The road does not paint over the water: the
// crossing becomes a deck with its own height, and the runtime reports it as a walkable
// surface distinct from the terrain.
struct BridgeSpan {
    std::string id;
    std::string roadId;
    std::string riverId;
    float x = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float lengthMetres = 0.0f;
    float widthMetres = 0.0f;
    float deckHeight = 0.0f;
    float waterHeight = 0.0f;
    float riverWidthMetres = 0.0f;
};

class PathNetwork {
public:
    // Builds the spline, validates control points and solves river descent. Returns false
    // and fills `error` only for structural problems (too few points, non-finite width);
    // geometric problems land in `diagnostics()` and do not stop the cook.
    bool build(const std::vector<PathSpec>& specs, const TerrainField* baseTerrain,
               float worldMinX, float worldMinZ, float worldMaxX, float worldMaxZ,
               std::string& error);

    bool empty() const { return paths_.empty(); }
    const std::vector<CompiledPath>& paths() const { return paths_; }
    const std::vector<PathDiagnostic>& diagnostics() const { return diagnostics_; }
    const std::vector<BridgeSpan>& bridges() const { return bridges_; }

    // Nearest centre line to (x, z). `radius` bounds the search: a query far from any path
    // returns an invalid hit instead of the distance to the other side of the world.
    PathHit nearest(float x, float z, float radius = 48.0f) const;
    // Nearest path of one kind. Bridge resolution needs "the nearest river", and the
    // nearest path overall is almost always the road being resolved.
    PathHit nearestOfKind(float x, float z, float radius, PathKind kind) const;

    bool inWater(float x, float z) const;
    float waterHeightAt(float x, float z) const;

    // The bridge deck covering (x, z), or null. Queried directly against the spans instead
    // of through the nearest path: at a crossing the nearest sample is a tie between the
    // road and the river, and which one wins must not decide whether the ground is solid.
    const BridgeSpan* bridgeAt(float x, float z) const;

private:
    struct GridCell {
        std::vector<std::uint32_t> samples; // encoded pathIndex * 2^20 + sampleIndex
    };
    PathHit nearestInternal(float x, float z, float radius, int requiredKind) const;
    void buildGrid();
    void insertGrid(std::uint32_t pathIndex, std::uint32_t sampleIndex, float x, float z, float radius);
    const GridCell* cellAt(std::int32_t cx, std::int32_t cz) const;
    void solveBridges();
    void solveRiverDescent();

    std::vector<CompiledPath> paths_;
    std::vector<PathDiagnostic> diagnostics_;
    std::vector<BridgeSpan> bridges_;
    std::unordered_map<std::uint64_t, GridCell> grid_;
    float gridOriginX_ = 0.0f, gridOriginZ_ = 0.0f;
    float gridCellMetres_ = 16.0f;
    const TerrainField* base_ = nullptr;
};

// Catmull-Rom point at parameter t in [0, 1] between p1 and p2.
PathPoint catmullRom(const PathPoint& p0, const PathPoint& p1, const PathPoint& p2, const PathPoint& p3, float t);

} // namespace world
} // namespace engine

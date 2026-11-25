#pragma once

// Terrain: the single height source that rendering, collision and scatter all read.
//
// The whole design rests on one property: `TerrainSurface::height(x, z)` is a pure function
// of world coordinates. It is not accumulated per chunk and it is not stored in a chunk
// until the chunk is written. Two neighbouring chunks therefore sample the identical
// function at their shared edge, and a seam cannot appear because there is nothing to
// disagree about.
//
// Layering order is fixed and matches the plan's logical stack:
//   base noise -> terrain edits (raise/lower/smooth/flatten) -> river bed -> road flatten
// Roads flatten last because a road over a riverbed is a bridge, and the bridge deck wins.

#include "world_path.hpp"
#include "world_types.hpp"

#include <string>
#include <vector>

namespace engine {
namespace world {

struct TerrainParams {
    std::string preset = "mountain-valley";
    float sampleStepMetres = 1.0f;
    float amplitudeMetres = 90.0f;
    float baseHeightMetres = 0.0f;
    float ridgeStrength = 0.55f;
    int erosionPasses = 2;
    std::uint64_t seed = 0;
};

// Base procedural terrain. No edits, no paths: this is what the settlement planner sees
// before anything has been placed.
class TerrainField {
public:
    bool configure(const TerrainParams& params, std::string& error);

    float height(float x, float z) const;
    // Gradient magnitude in metres per metre.
    float gradientMagnitude(float x, float z) const;
    float slopeDegrees(float x, float z) const;
    // 0..1 ecological fields. `moisture` rises near the low end of the height range and
    // near rivers (the path network adds the river contribution); `rockiness` rises on
    // steep ground.
    float moisture(float x, float z) const;
    float rockiness(float x, float z) const;
    // Height range actually reachable by the configured preset, sampled on a coarse grid.
    // The compiler uses it to normalize ecological fields instead of assuming a range.
    float minHeight() const { return minHeight_; }
    float maxHeight() const { return maxHeight_; }

    const TerrainParams& params() const { return params_; }

private:
    TerrainParams params_{};
    float minHeight_ = 0.0f;
    float maxHeight_ = 1.0f;
};

enum class TerrainEditMode : std::uint8_t {
    Raise = 0,
    Lower = 1,
    Smooth = 2,
    Flatten = 3,
    Water = 4,
    Paint = 5,
};

enum class TerrainEditShape : std::uint8_t { Circle = 0, Rectangle = 1, Polygon = 2 };

// One brush result, already resampled by the editor into world space. The compiler applies
// these analytically so they participate in the pure height function.
struct TerrainEdit {
    TerrainEditMode mode = TerrainEditMode::Raise;
    TerrainEditShape shape = TerrainEditShape::Circle;
    Bounds2 bounds{};
    float centreX = 0.0f, centreZ = 0.0f;
    float radiusMetres = 0.0f;
    float falloffMetres = 0.0f;
    float strength = 1.0f;
    float targetHeight = 0.0f;
    float deltaMetres = 1.0f;
    bool erase = false;
    std::vector<PathPoint> polygon;

    // Weight at a world point, 0 outside the shape.
    float weight(float x, float z) const;
};

struct SurfaceWeights {
    float grass = 0.0f;
    float rock = 0.0f;
    float dirt = 0.0f;
    float sand = 0.0f;
    float water = 0.0f;
    void normalize();
};

// Everything the compiler needs to know about one surface point, computed in a single pass.
//
// The single-pass form is not a convenience: computing height, normal, slope, moisture and
// water separately would run the nearest-path query six times per vertex, and a 2 km world
// has four million vertices. One query, six answers.
struct SurfaceSample {
    float height = 0.0f;
    float normalY = 1.0f;
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float slopeDegrees = 0.0f;
    float moisture = 0.0f;
    float rockiness = 0.0f;
    SurfaceWeights weights{};
    float waterHeight = -1.0e9f;
    bool submerged = false;
    bool onRoad = false;
    bool onBridge = false;
    float pathDistance = 1.0e9f;
    PathKind pathKind = PathKind::Road;
    bool hasPath = false;
};

// Final surface: base field + edits + path flattening. Both the renderer and the physics
// query read this, which is what "rendering and collision share a source" means concretely.
class TerrainSurface {
public:
    // The edit list is copied, not referenced. Callers legitimately pass a temporary
    // (`configure(field, paths, {})` for a world with no brush edits), and a stored pointer
    // would dangle the moment that full expression ended.
    void configure(const TerrainField* base, const PathNetwork* paths,
                   const std::vector<TerrainEdit>& edits);

    // One-pass evaluation. `stepMetres` is the finite-difference step for the normal; the
    // chunk generator passes its own sample step so the normal matches the mesh it builds.
    SurfaceSample sample(float x, float z, float stepMetres = 1.0f) const;

    float height(float x, float z) const;
    float normalY(float x, float z) const;
    void normal(float x, float z, float out[3]) const;
    float slopeDegrees(float x, float z) const;
    SurfaceWeights weights(float x, float z) const;
    // Water surface height at (x, z), or a negative sentinel when dry. Rivers only in v1.
    float waterHeight(float x, float z) const;
    bool submerged(float x, float z) const;
    // True when the point is on a road surface (including a bridge deck).
    bool onRoad(float x, float z) const;
    // True when the point is on a bridge deck rather than the terrain.
    bool onBridge(float x, float z) const;

    const TerrainField* base() const { return base_; }
    const PathNetwork* paths() const { return paths_; }

private:
    float heightWithHit(float x, float z, const PathHit& hit) const;
    float applyEdits(float x, float z, float height) const;
    const TerrainField* base_ = nullptr;
    const PathNetwork* paths_ = nullptr;
    std::vector<TerrainEdit> edits_;
};

// Converts a slope angle to the cosine threshold used by placement rules.
inline float slopeCosineThreshold(float maxSlopeDegrees) {
    const float clamped = maxSlopeDegrees < 0.0f ? 0.0f : (maxSlopeDegrees > 89.5f ? 89.5f : maxSlopeDegrees);
    return std::cos(clamped * 3.14159265358979323846f / 180.0f);
}

} // namespace world
} // namespace engine

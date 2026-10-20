#pragma once

#include <cstdint>

// Geometry routing contract (docs/GEOMETRY-LAYOUT.md §3).
//
// The point of this enum is that "which rasterizer draws this mesh" is a property of the
// mesh, not a global mode switch. The layout can be unified while the rasterizer stays
// selectable, which is what lets the same scene run on a discrete GPU and on an iGPU.
namespace engine {

enum class GeometryClass : std::uint32_t {
    Cluster = 0,      // static opaque, dense enough to amortise the cluster pipeline
    Simple = 1,       // static opaque, too small for the cluster fixed cost
    Deformable = 2,   // skinned or world-position-offset
    Transparent = 3,  // translucent / masked two-sided: ordered forward path
    Effect = 4        // particles, sprites, UI
};

struct GeometryHints {
    std::uint32_t triangles = 0;
    bool animated = false;
    bool translucent = false;
    bool twoSided = false;
    bool alphaTest = false;
};

// Below this the cluster pipeline's fixed cost dominates. The value must be tuned with
// the budget governor and the image contracts, not by guesswork.
constexpr std::uint32_t kClusterTriangleThreshold = 5000;

GeometryClass classifyGeometry(const GeometryHints& hints);
const char* geometryClassName(GeometryClass value);

} // namespace engine

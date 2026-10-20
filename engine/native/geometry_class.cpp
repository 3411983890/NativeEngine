#include "geometry_class.hpp"

namespace engine {

GeometryClass classifyGeometry(const GeometryHints& hints) {
    // Order matters: translucency forces the forward path regardless of density, and a
    // deforming mesh keeps the vertex path until the cluster skinning path is proven.
    if (hints.translucent) return GeometryClass::Transparent;
    if (hints.animated) return GeometryClass::Deformable;
    if (hints.triangles >= kClusterTriangleThreshold) return GeometryClass::Cluster;
    return GeometryClass::Simple;
}

const char* geometryClassName(GeometryClass value) {
    switch (value) {
        case GeometryClass::Cluster: return "cluster";
        case GeometryClass::Simple: return "simple";
        case GeometryClass::Deformable: return "deformable";
        case GeometryClass::Transparent: return "transparent";
        case GeometryClass::Effect: return "effect";
    }
    return "unknown";
}

} // namespace engine

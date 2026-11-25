#pragma once

// Convex mesh geometry shared by the physics/destruction layer and the renderer.
//
// A Voronoi shard is a convex polyhedron: its collision proxy is the fitted OBB in
// `Cell`, but its silhouette is the hull. This header is the hand-off between the
// two: the game bakes one ConvexMesh per cell and the renderer uploads the whole
// set into a single vertex/index buffer pair.
//
// Vertex layout is deliberately identical to the renderer's unit cube
// (position + normal + eight per-slot UVs + linear RGBA color + tangent, 120 bytes). Legacy geometry leaves the
// extra streams unused; imported meshes can select separate source UV sets.

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

struct ConvexVertex {
    float position[3]{0.0f, 0.0f, 0.0f};
    float normal[3]{0.0f, 1.0f, 0.0f};
    float uv[2]{0.0f, 0.0f};
    float textureUvs[4][2]{};
    float color[4]{1.0f,1.0f,1.0f,1.0f};
    float tangent[4]{}; // w=0 selects the generated derivative frame.
    float layerUvs[3][2]{};
};
static_assert(sizeof(ConvexVertex) == 120, "Convex vertex layout must match the renderer");

struct ConvexMesh {
    std::vector<ConvexVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Local-space bounds, used for culling and for the OBB-proxy sanity check.
    float min[3]{0.0f, 0.0f, 0.0f};
    float max[3]{0.0f, 0.0f, 0.0f};
    // True when this mesh came from a baked Voronoi package rather than a box fallback.
    bool baked = false;
    bool hasTextureUvs = false;
    bool hasLayerUvs = false;

    bool empty() const { return vertices.empty() || indices.empty(); }
};

// A baked Voronoi shard set for one destructible prop, as produced by
// tools/offline/voronoi.mjs (schema nativegpu.voronoi/1). `meshes[i]` is shard i's hull
// in body-local space, and `shards[i]` is the physics proxy that stands in for it.
struct VoronoiShardProxy {
    float center[3]{0.0f, 0.0f, 0.0f};
    float half[3]{0.05f, 0.05f, 0.05f};
    float volume = 0.0f;
    // Index of the shard's exterior hull in the same table. Meshes and shards are not
    // one-to-one once cut faces exist, so the index is explicit.
    int mesh = -1;
    // Index of the shard's cut-face mesh, or -1 when every face was on the original
    // surface. Drawn as a second pose so the interior can use its own tint.
    int meshInterior = -1;
};

struct VoronoiLink {
    int a = -1;
    int b = -1;
    float area = 0.0f;
};

struct VoronoiPackage {
    std::string source;  // asset path, echoed in the report
    std::vector<ConvexMesh> meshes;
    std::vector<VoronoiShardProxy> shards;
    std::vector<VoronoiLink> links;
    // Half extents of the box that was split. UVs are projected against these so a shard
    // samples the same part of the texture the intact prop did.
    float half[3]{0.0f, 0.0f, 0.0f};
    float volume = 0.0f;
    float boxVolume = 0.0f;

    bool empty() const { return meshes.empty() || shards.empty(); }
};

} // namespace engine

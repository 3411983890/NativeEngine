#pragma once

// World compiler: turns a resolved compile input into immutable chunk files.
//
// "Resolved" is the important word. This module never reads a library, a layer file or an
// override file: the JS world service has already followed every reference, verified every
// hash and flattened the authoring documents into one self-contained request. That split is
// what lets the compiler be a plain C++ library with no filesystem policy, and it is what
// keeps path-traversal rules in exactly one place.
//
// Order of operations is fixed and each step only reads the previous ones:
//
//   1. terrain field      (pure noise, no placement yet)
//   2. path network       (spline, river descent, bridges)
//   3. terrain surface    (edits + path flattening, still no placement)
//   4. settlement plan    (needs roads, produces buildings)
//   5. exclusion zones    (needs buildings, bridges and locks)
//   6. scatter            (needs exclusions)
//   7. chunk generation   (samples the surface, collects instances and buildings)
//
// Nothing here iterates back to an earlier step, so a layout cannot oscillate.

#include "world_chunk.hpp"
#include "world_path.hpp"
#include "world_scatter.hpp"
#include "world_settlement.hpp"
#include "world_terrain.hpp"
#include "world_types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace world {

struct ComponentSpec {
    std::string id;
    std::string category;
    std::string shape;
    float sizeMetres[3] = {1.0f, 1.0f, 1.0f};
    float albedo[3] = {0.7f, 0.7f, 0.7f};
    float roughness = 0.85f;
    float metalness = 0.0f;
    bool collision = false;
    float collisionHalfExtents[3] = {0.5f, 0.5f, 0.5f};
    std::uint16_t index = 0;
};

struct PreservedInstance {
    WorldInstance instance;
};

struct CompileInput {
    std::string worldId;
    std::string generatorVersion;
    std::uint64_t seed = 0;
    float extentMetres[2] = {256.0f, 256.0f};
    float originMetres[3] = {-128.0f, 0.0f, -128.0f};
    float chunkSizeMetres = 128.0f;
    float sampleStepMetres = 1.0f;
    std::string budgetProfile = "desktop-balanced";

    TerrainParams terrain{};
    std::vector<ComponentSpec> components;
    std::vector<PathSpec> paths;
    std::vector<TerrainEdit> terrainEdits;
    std::vector<ScatterRule> scatter;
    std::vector<DensityRegion> densityRegions;
    std::vector<SettlementParams> settlements;
    std::vector<SettlementRegion> settlementRegions;
    std::vector<ExclusionZone> extraExclusions;
    std::vector<WorldInstance> preserved;
    std::vector<std::uint64_t> deletedIds;
    std::vector<std::pair<std::uint64_t, WorldInstance>> movedInstances;
    std::vector<Bounds2> locks;

    Bounds2 worldBounds() const {
        return {originMetres[0], originMetres[2],
                originMetres[0] + extentMetres[0], originMetres[2] + extentMetres[1]};
    }
};

struct ChunkFileInfo {
    std::int32_t cx = 0, cz = 0;
    std::string file;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::uint32_t lod = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t vertexCount = 0;
    Bounds2 bounds{};
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
};

// The compiler hands each finished chunk to a sink instead of accumulating them: a 8 km
// world is 4,096 chunks and holding them all would defeat the memory bound the runtime is
// being built to respect.
class ChunkSink {
public:
    virtual ~ChunkSink() = default;
    virtual bool write(const ChunkData& chunk, ChunkFileInfo& info, std::string& error) = 0;
};

struct CompileStats {
    std::uint64_t chunks = 0;
    std::uint64_t totalInstances = 0;
    std::uint64_t totalBuildings = 0;
    std::uint64_t vertices = 0;
    std::uint64_t budgetCutInstances = 0;
    float budgetScale = 1.0f;
    double cookMillis = 0.0;
    ScatterStats scatter{};
    SettlementStats settlement{};
    std::vector<PathDiagnostic> pathDiagnostics;
    std::vector<SettlementDiagnostic> settlementDiagnostics;
    std::vector<std::string> warnings;
};

class WorldCompiler {
public:
    // Parses the compile request. Returns false with a message on any structural problem;
    // semantic problems (a path with two points) are reported through `stats` instead.
    static bool parseInput(const std::string& json, CompileInput& out, std::string& error);

    bool compile(const CompileInput& input, ChunkSink& sink, CompileStats& stats, std::string& error);

    // Manifest JSON for the chunks the sink reported. The caller writes it next to them.
    static std::string manifestJson(const CompileInput& input, const CompileStats& stats,
                                    const std::vector<ChunkFileInfo>& chunks,
                                    const std::string& inputDigest,
                                    const std::string& manifestHash);

private:
    void buildChunk(const CompileInput& input, ChunkCoord coord,
                    const TerrainSurface& surface, const PathNetwork& paths,
                    const ScatterField& scatter,
                    const std::vector<BuildingRecord>& buildings,
                    std::vector<WorldInstance>& scratch,
                    ChunkData& chunk, CompileStats& stats) const;
    void buildRoadMesh(const CompileInput& input, ChunkCoord coord, const PathNetwork& paths,
                       const TerrainSurface& surface, ChunkData& chunk) const;
    void buildRiverBed(const CompileInput& input, ChunkCoord coord, const PathNetwork& paths,
                       ChunkData& chunk) const;
};

} // namespace world
} // namespace engine

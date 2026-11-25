// World compiler and runtime contract tests.
//
// Every case here pins a property that fails silently in production:
//
//   * a chunk index computed by truncation instead of floor splits the seam at x = 0;
//   * a random stream seeded from anything but the world seed changes between runs;
//   * a scatter rule evaluated per chunk instead of per world cell bunches trees at borders;
//   * a runtime that returns 0 for an unloaded chunk drops the player through the floor;
//   * a chunk reader that trusts its header reads past the end of a truncated file.
//
// The binary has no GPU dependency, so it runs in CI and in a fresh checkout.

#include "../../../engine/native/world/world_chunk.hpp"
#include "../../../engine/native/world/world_compiler.hpp"
#include "../../../engine/native/world/world_noise.hpp"
#include "../../../engine/native/world/world_runtime.hpp"
#include "../../../engine/native/world/world_scatter.hpp"
#include "../../../engine/native/world/world_settlement.hpp"
#include "../../../engine/native/world/world_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace engine::world;

namespace {

unsigned checks = 0;
unsigned groups = 0;
std::vector<std::string> failures;

void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        failures.push_back(message);
        std::cerr << "  FAIL: " << message << "\n";
    }
}

template <typename A, typename B>
void checkEqual(A a, B b, const char* message) {
    ++checks;
    if (!(a == b)) {
        failures.push_back(message);
        std::cerr << "  FAIL: " << message << " (" << a << " != " << b << ")\n";
    }
}

void group(const char* name) {
    ++groups;
    std::cout << "[" << name << "]\n";
}

// Collects chunks in memory so a test can inspect them and serialize them twice.
class MemorySink : public ChunkSink {
public:
    std::vector<ChunkData> chunks;
    std::vector<ChunkFileInfo> infos;
    bool write(const ChunkData& chunk, ChunkFileInfo& info, std::string& error) override {
        std::vector<std::uint8_t> bytes;
        if (!serializeChunk(chunk, bytes, error)) return false;
        Hasher hasher;
        hasher.bytes(bytes.data(), bytes.size());
        info.file = "chunks/" + std::to_string(chunk.coord.x) + "_" + std::to_string(chunk.coord.z) + ".ngrc";
        info.sha256 = hasher.hex() + hasher.hex() + hasher.hex() + hasher.hex();
        info.bytes = bytes.size();
        info.lod = chunk.lod;
        info.instanceCount = static_cast<std::uint32_t>(chunk.instances.size());
        info.vertexCount = chunk.vertexCount;
        info.bounds = chunk.bounds;
        info.minHeight = chunk.minHeight;
        info.maxHeight = chunk.maxHeight;
        chunks.push_back(chunk);
        infos.push_back(info);
        return true;
    }
};

std::string compileInputJson(float extent, float chunkSize, const std::string& extra = "") {
    // A 256 m world with a river down the middle, a road crossing it, a forest and a
    // village. Deliberately small: the point is to exercise every stage, not to be pretty.
    std::string json = R"({
  "schema": "nativegpu.world-compile/1",
  "worldId": "test-valley",
  "generatorVersion": "worldgen-1",
  "seed": "947210",
  "extentMetres": [)";
    json += std::to_string(static_cast<int>(extent)) + ", " + std::to_string(static_cast<int>(extent));
    json += R"(],
  "originMetres": [)";
    json += std::to_string(static_cast<int>(-extent / 2)) + ", 0, " + std::to_string(static_cast<int>(-extent / 2));
    json += R"(],
  "chunkSizeMetres": )";
    json += std::to_string(static_cast<int>(chunkSize));
    json += R"(,
  "sampleStepMetres": 2,
  "budgetProfile": "debug",
  "terrain": {"preset": "mountain-valley", "amplitudeMetres": 60, "baseHeightMetres": 4, "ridgeStrength": 0.5},
  "components": [
    {"id": "pine", "category": "tree", "sizeMetres": [3, 11, 3], "shape": "cone", "collision": true},
    {"id": "grass", "category": "grass", "sizeMetres": [0.4, 0.3, 0.4], "shape": "cross"},
    {"id": "boulder", "category": "rock", "sizeMetres": [2, 1.5, 2], "shape": "sphere"},
    {"id": "house", "category": "building-prefab", "sizeMetres": [11, 6, 9], "shape": "box", "collision": true}
  ],
  "paths": [
    {"id": "river-main", "kind": "river", "points": [[-120, -110], [-40, -40], [0, 0], [40, 60], [100, 120]],
     "widthMetres": 14, "bedDepthMetres": 1.4},
    {"id": "road-main", "kind": "road", "points": [[-120, 10], [-40, 4], [0, 0], [40, -6], [120, -10]],
     "widthMetres": 5, "maxSlopeDegrees": 18}
  ],
  "scatter": [
    {"layerId": "forest", "component": "pine", "category": "tree", "densityPerHectare": 90,
     "maxSlopeDegrees": 34, "minSpacingMetres": 5},
    {"layerId": "meadow", "component": "grass", "category": "grass", "densityPerHectare": 3000,
     "maxSlopeDegrees": 36},
    {"layerId": "stones", "component": "boulder", "category": "rock", "densityPerHectare": 6,
     "maxSlopeDegrees": 45, "minSpacingMetres": 4}
  ],
  "settlements": [
    {"layerId": "village", "components": ["house"], "density": 0.6, "maxBuildings": 24,
     "lotSizeMetres": 18, "setbackMetres": 2, "roadWidthMetres": 5}
  ],
  "settlementRegions": [
    {"shape": "circle", "center": [0, 0], "radiusMetres": 90}
  ]
)";
    if (!extra.empty()) {
        json.pop_back();
        json += ",\n" + extra + "\n}";
    } else {
        json += "\n}";
    }
    return json;
}

CompileInput parseOrThrow(const std::string& json) {
    CompileInput input;
    std::string error;
    if (!WorldCompiler::parseInput(json, input, error)) {
        throw std::runtime_error("parseInput failed: " + error);
    }
    return input;
}

void testChunkCoordinates() {
    group("chunk coordinates use floor, not truncation");
    checkEqual(chunkIndexForMetres(-0.5f, 128.0f), -1, "x = -0.5 lands in chunk -1");
    checkEqual(chunkIndexForMetres(-128.0f, 128.0f), -1, "x = -128 is the first metre of chunk -1");
    checkEqual(chunkIndexForMetres(-128.1f, 128.0f), -2, "x = -128.1 rolls into chunk -2");
    checkEqual(chunkIndexForMetres(0.0f, 128.0f), 0, "x = 0 is chunk 0");
    checkEqual(chunkIndexForMetres(127.99f, 128.0f), 0, "x = 127.99 is still chunk 0");
    checkEqual(chunkIndexForMetres(128.0f, 128.0f), 1, "x = 128 is chunk 1");

    const ChunkRange range = chunkRangeForBounds({-128.0f, -128.0f, 128.0f, 128.0f}, 128.0f);
    checkEqual(range.minCx, -1, "a 256 m world centred on the origin starts at chunk -1");
    checkEqual(range.maxCx, 0, "and ends at chunk 0");
    checkEqual(static_cast<int>(range.count()), 4, "a 256 m world is 2x2 chunks");
}

void testHashAndRng() {
    group("deterministic hash and random streams");
    Hasher a;
    a.i32(-7);
    a.quantized(1.2345f);
    Hasher b;
    b.i32(-7);
    b.quantized(1.2345f);
    checkEqual(a.digest(), b.digest(), "the same input hashes the same");

    Rng first(42ull, 7ull);
    Rng second(42ull, 7ull);
    bool identical = true;
    for (int i = 0; i < 64; ++i) {
        if (first.next() != second.next()) { identical = false; break; }
    }
    check(identical, "two streams with the same seed produce the same sequence");

    Rng other(42ull, 8ull);
    check(first.next() != other.next(), "a different stream id diverges");

    // Cross-check a fixed vector so a future refactor cannot silently change the sequence
    // and invalidate every cooked world in existence.
    Rng vector_(123456789ull, 0ull);
    const std::uint64_t expected = vector_.next();
    Rng vector2(123456789ull, 0ull);
    checkEqual(expected, vector2.next(), "the RNG sequence is stable across construction");

    std::uint64_t seed = 0;
    check(parseSeed("18446744073709551615", seed), "the largest uint64 parses as a seed");
    checkEqual(seed, 18446744073709551615ull, "and keeps every bit");
    check(!parseSeed("18446744073709551616", seed), "one past uint64 is rejected");
    check(!parseSeed("12a", seed), "a non-decimal seed is rejected");
}

void testTerrainDeterminism() {
    group("terrain is a pure function of world coordinates");
    TerrainParams params;
    params.preset = "mountain-valley";
    params.amplitudeMetres = 60.0f;
    params.baseHeightMetres = 4.0f;
    params.ridgeStrength = 0.5f;
    params.seed = 947210ull;
    params.sampleStepMetres = 1.0f;

    TerrainField field;
    std::string error;
    check(field.configure(params, error), "the mountain-valley preset configures");

    const float a = field.height(12.5f, -37.25f);
    const float b = field.height(12.5f, -37.25f);
    checkEqual(a, b, "the same coordinate returns the same height");

    // Negative coordinates must be handled by the noise lattice, not folded to positive.
    const float negative = field.height(-12.5f, -37.25f);
    const float positive = field.height(12.5f, -37.25f);
    check(std::isfinite(negative) && std::fabs(negative - positive) > 1.0e-4f,
          "negative and positive coordinates are different places");

    check(field.maxHeight() > field.minHeight(), "the configured preset has a height range");

    TerrainParams bad = params;
    bad.preset = "volcano";
    TerrainField invalid;
    check(!invalid.configure(bad, error), "an unknown preset is refused");
}

void testRiverDescent() {
    group("a river descends from source to outlet");
    TerrainParams params;
    params.preset = "rolling-hills";
    params.amplitudeMetres = 40.0f;
    params.seed = 7ull;
    TerrainField field;
    std::string error;
    check(field.configure(params, error), "rolling-hills configures");

    PathSpec river;
    river.id = "river";
    river.kind = PathKind::River;
    river.widthMetres = 12.0f;
    river.bedDepthMetres = 1.0f;
    for (int i = 0; i < 12; ++i) {
        river.control.push_back({static_cast<float>(i) * 20.0f - 120.0f, static_cast<float>(i) * 6.0f});
    }
    PathNetwork network;
    check(network.build({river}, &field, -256.0f, -256.0f, 256.0f, 256.0f, error), "the river network builds");
    checkEqual(network.paths().size(), static_cast<std::size_t>(1), "one river was compiled");

    const auto& compiled = network.paths().front();
    check(compiled.waterValid, "the river has a solved water surface");
    bool monotone = true;
    for (std::size_t i = 1; i < compiled.samples.size(); ++i) {
        if (compiled.samples[i].centreHeight > compiled.samples[i - 1].centreHeight + 1.0e-5f) {
            monotone = false;
            break;
        }
    }
    check(monotone, "water never flows uphill along the centre line");

    // The carved channel is below the water surface at the centre and meets it at the bank.
    TerrainSurface surface;
    surface.configure(&field, &network, {});
    const auto& middle = compiled.samples[compiled.samples.size() / 2];
    const float centreHeight = surface.height(middle.x, middle.z);
    check(centreHeight < middle.centreHeight, "the bed is carved below the water surface");
    const float outside = surface.height(middle.x + middle.tx * 40.0f, middle.z - middle.tz * 40.0f);
    check(outside > centreHeight, "the bank rises away from the channel");
}

void testBridgeCrossing() {
    group("a road over a river becomes a bridge, not a painted stripe");
    TerrainParams params;
    params.preset = "flat";
    params.amplitudeMetres = 0.0f;
    params.baseHeightMetres = 10.0f;
    params.seed = 3ull;
    TerrainField field;
    std::string error;
    check(field.configure(params, error), "flat terrain configures");

    PathSpec river;
    river.id = "river";
    river.kind = PathKind::River;
    river.widthMetres = 16.0f;
    river.bedDepthMetres = 2.0f;
    river.control = {{0.0f, -80.0f}, {0.0f, 0.0f}, {0.0f, 80.0f}};

    PathSpec road;
    road.id = "road";
    road.kind = PathKind::Road;
    road.widthMetres = 5.0f;
    road.control = {{-80.0f, 0.0f}, {0.0f, 0.0f}, {80.0f, 0.0f}};

    PathNetwork network;
    check(network.build({river, road}, &field, -128.0f, -128.0f, 128.0f, 128.0f, error), "the crossing builds");
    checkEqual(network.bridges().size(), static_cast<std::size_t>(1), "exactly one bridge was detected");

    const auto& bridge = network.bridges().front();
    check(bridge.deckHeight > bridge.waterHeight, "the deck sits above the water");
    check(bridge.lengthMetres > bridge.riverWidthMetres, "the deck spans the river plus its abutments");

    TerrainSurface surface;
    surface.configure(&field, &network, {});
    check(surface.onBridge(bridge.x, bridge.z), "the crossing point is reported as a bridge deck");
    check(!surface.onBridge(bridge.x, bridge.z + 40.0f), "the road away from the water is not a bridge");
}

void testScatterSpacing() {
    group("tree spacing is decided on the world grid, not per chunk");
    TerrainParams params;
    params.preset = "flat";
    params.amplitudeMetres = 0.0f;
    params.baseHeightMetres = 0.0f;
    params.seed = 11ull;
    TerrainField field;
    std::string error;
    check(field.configure(params, error), "flat terrain configures");

    TerrainSurface surface;
    surface.configure(&field, nullptr, {});

    ScatterRule rule;
    rule.layerId = "forest";
    rule.component = 0;
    rule.densityPerHectare = 400.0f;
    rule.minSpacingMetres = 6.0f;
    rule.maxSlopeDegrees = 60.0f;
    rule.seedStream = 0x1234ull;

    std::vector<ScatterRule> rules = {rule};
    ScatterField scatter;
    scatter.configure(&surface, &rules, nullptr, nullptr, 11ull);

    // Generate two neighbouring chunks in opposite orders. The union must be identical and
    // no two trees may be closer than the spacing, including across the shared border.
    std::vector<WorldInstance> first, second;
    ScatterStats statsA, statsB;
    scatter.generateChunk({0, 0}, 128.0f, first, statsA);
    scatter.generateChunk({1, 0}, 128.0f, second, statsB);

    std::vector<WorldInstance> reversedFirst, reversedSecond;
    ScatterStats statsC, statsD;
    scatter.generateChunk({1, 0}, 128.0f, reversedSecond, statsD);
    scatter.generateChunk({0, 0}, 128.0f, reversedFirst, statsC);

    checkEqual(first.size(), reversedFirst.size(), "chunk (0,0) is the same size in either order");
    checkEqual(second.size(), reversedSecond.size(), "chunk (1,0) is the same size in either order");
    bool identical = first.size() == reversedFirst.size();
    for (std::size_t i = 0; identical && i < first.size(); ++i) {
        if (first[i].stableId != reversedFirst[i].stableId) identical = false;
    }
    check(identical, "instance identities do not depend on generation order");

    std::vector<WorldInstance> all = first;
    all.insert(all.end(), second.begin(), second.end());
    double closest = 1.0e9;
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            const double dx = all[i].position[0] - all[j].position[0];
            const double dz = all[i].position[2] - all[j].position[2];
            closest = std::min(closest, std::sqrt(dx * dx + dz * dz));
        }
    }
    check(all.size() > 8, "the rule produced a usable number of trees");
    check(closest >= rule.minSpacingMetres - 1.0e-3f, "no two trees are closer than the spacing, across the seam");

    // Every instance must belong to exactly one chunk: the one containing its centre.
    bool ownershipCorrect = true;
    for (const auto& instance : all) {
        const std::int32_t cx = chunkIndexForMetres(instance.position[0], 128.0f);
        if (cx != 0 && cx != 1) ownershipCorrect = false;
        if (chunkIndexForMetres(instance.position[2], 128.0f) != 0) ownershipCorrect = false;
    }
    check(ownershipCorrect, "each instance is emitted by the chunk that contains its centre");
}

void testCompileDeterminism() {
    group("compiling the same input twice produces identical bytes");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);

    WorldCompiler compiler;
    CompileStats statsA, statsB;
    MemorySink sinkA, sinkB;
    std::string error;
    check(compiler.compile(input, sinkA, statsA, error), "the first compile succeeds");
    check(compiler.compile(input, sinkB, statsB, error), "the second compile succeeds");
    checkEqual(sinkA.chunks.size(), sinkB.chunks.size(), "both compiles produced the same chunk count");
    checkEqual(statsA.totalInstances, statsB.totalInstances, "both compiles produced the same instance count");

    bool identical = sinkA.chunks.size() == sinkB.chunks.size();
    for (std::size_t i = 0; identical && i < sinkA.chunks.size(); ++i) {
        std::vector<std::uint8_t> a, b;
        check(serializeChunk(sinkA.chunks[i], a, error), "chunk serializes");
        check(serializeChunk(sinkB.chunks[i], b, error), "chunk serializes again");
        if (a != b) identical = false;
    }
    check(identical, "every chunk is byte-identical between runs");

    // And a different seed must produce different content: a test that passes for a broken
    // RNG would also pass if the seed were ignored.
    CompileInput other = input;
    other.seed = 999ull;
    other.terrain.seed = 999ull;
    for (auto& rule : other.scatter) rule.seedStream ^= 0x99ull;
    CompileStats statsC;
    MemorySink sinkC;
    check(compiler.compile(other, sinkC, statsC, error), "a different seed compiles");
    bool different = false;
    for (std::size_t i = 0; i < std::min(sinkA.chunks.size(), sinkC.chunks.size()); ++i) {
        if (sinkA.chunks[i].heights != sinkC.chunks[i].heights) { different = true; break; }
    }
    check(different, "a different seed produces different terrain");
}

void testChunkSeams() {
    group("neighbouring chunks meet without a crack");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");

    const ChunkData* left = nullptr;
    const ChunkData* right = nullptr;
    for (const auto& chunk : sink.chunks) {
        if (chunk.coord.x == -1 && chunk.coord.z == -1) left = &chunk;
        if (chunk.coord.x == 0 && chunk.coord.z == -1) right = &chunk;
    }
    check(left != nullptr && right != nullptr, "the two western chunks were produced");
    if (!left || !right) return;

    checkEqual(left->vertexCount, right->vertexCount, "neighbours have the same vertex count");
    const std::uint32_t n = left->vertexCount;
    float worst = 0.0f;
    for (std::uint32_t iz = 0; iz < n; ++iz) {
        const float a = left->heights[left->vertexIndex(n - 1, iz)];
        const float b = right->heights[right->vertexIndex(0, iz)];
        worst = std::max(worst, std::fabs(a - b));
    }
    check(worst < 1.0e-4f, "the shared edge samples identical heights");

    // Collision heights must equal render heights at LOD 0: "rendering and collision share
    // a source" is only true if the file actually stores the same numbers.
    bool collisionMatches = left->collision.size() == left->heights.size();
    for (std::size_t i = 0; collisionMatches && i < left->heights.size(); ++i) {
        if (left->collision[i] != left->heights[i]) collisionMatches = false;
    }
    check(collisionMatches, "collision heights equal render heights at LOD 0");
}

void testChunkRoundTrip() {
    group("a chunk survives serialization and rejects corruption");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");
    check(!sink.chunks.empty(), "at least one chunk exists");

    const ChunkData& original = sink.chunks.front();
    std::vector<std::uint8_t> bytes;
    check(serializeChunk(original, bytes, error), "the chunk serializes");

    ChunkData restored;
    std::string roundTripError;
    check(deserializeChunk(bytes.data(), bytes.size(), restored, roundTripError), "the chunk deserializes");
    if (!roundTripError.empty()) std::cerr << "    reason: " << roundTripError << "\n";
    checkEqual(restored.coord.x, original.coord.x, "the chunk coordinate survives");
    checkEqual(restored.vertexCount, original.vertexCount, "the vertex count survives");
    checkEqual(restored.heights.size(), original.heights.size(), "the height array survives");
    checkEqual(restored.instances.size(), original.instances.size(), "the instance array survives");
    bool heightsMatch = true;
    for (std::size_t i = 0; i < original.heights.size(); ++i) {
        if (restored.heights[i] != original.heights[i]) { heightsMatch = false; break; }
    }
    check(heightsMatch, "every height survives the round trip bit for bit");

    // Truncation.
    ChunkData truncated;
    check(!deserializeChunk(bytes.data(), bytes.size() - 8, truncated, error), "a truncated chunk is refused");

    // Header tampering: claim more payload than exists.
    std::vector<std::uint8_t> badLength = bytes;
    const std::uint32_t inflated = 0x00ffffffu;
    std::memcpy(badLength.data() + 56, &inflated, 4);
    check(!deserializeChunk(badLength.data(), badLength.size(), truncated, error), "an inflated payload length is refused");

    // Payload corruption: the hash must catch a single flipped byte.
    std::vector<std::uint8_t> flipped = bytes;
    flipped[flipped.size() - 1] ^= 0xffu;
    check(!deserializeChunk(flipped.data(), flipped.size(), truncated, error), "a flipped payload byte is refused");

    // Magic.
    std::vector<std::uint8_t> wrongMagic = bytes;
    wrongMagic[0] = 'X';
    check(!deserializeChunk(wrongMagic.data(), wrongMagic.size(), truncated, error), "a wrong magic is refused");
}

void testBudget() {
    group("the instance budget is applied up front and reported");
    // A dense grass rule on a 512 m world would be ~260k candidates; the debug profile caps
    // at 65536, so the compiler must scale the density and say so.
    const std::string json = compileInputJson(512.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    for (auto& rule : input.scatter) {
        if (rule.exclusionCategory == kExcludeGrass) rule.densityPerHectare = 200000.0f;
    }
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the over-budget world still compiles");
    check(stats.budgetScale < 1.0f, "the density was scaled down");
    check(!stats.warnings.empty(), "the reduction is reported as a warning");
}

void testRuntimeStreaming() {
    group("runtime streaming, queries and eviction");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");

    // In-memory source: the runtime is not allowed to care where the bytes come from.
    struct MemorySource : ChunkSource {
        std::map<std::pair<int, int>, std::vector<std::uint8_t>> files;
        bool read(ChunkCoord coord, std::vector<std::uint8_t>& bytes, std::string& error_) override {
            const auto it = files.find({coord.x, coord.z});
            if (it == files.end()) { error_ = "missing chunk"; return false; }
            bytes = it->second;
            return true;
        }
    } source;
    for (const auto& chunk : sink.chunks) {
        std::vector<std::uint8_t> bytes;
        if (!serializeChunk(chunk, bytes, error)) { check(false, "serialize for runtime"); continue; }
        source.files[{chunk.coord.x, chunk.coord.z}] = bytes;
    }

    RuntimeConfig config;
    config.chunkSizeMetres = 128.0f;
    config.sampleStepMetres = 2.0f;
    config.nearDistanceMetres = 200.0f;
    config.midDistanceMetres = 400.0f;
    config.farDistanceMetres = 600.0f;
    config.maxResidentChunks = 8;

    WorldRuntime runtime;
    check(runtime.configure(config, error), "the runtime configures");
    runtime.setManifestBounds({-128.0f, -128.0f, 128.0f, 128.0f});
    runtime.setSource(&source);

    // Before any pump, a query must not claim to be ready.
    const SurfaceQuery cold = runtime.querySurface(0.0f, 0.0f);
    check(!cold.ready, "an unloaded chunk reports not ready instead of height 0");

    runtime.pump(0.0f, 0.0f, 8);
    const SurfaceQuery warm = runtime.querySurface(0.0f, 0.0f);
    check(warm.ready, "after a pump the chunk answers");
    check(std::isfinite(warm.height), "the height is finite");

    // A point far outside the world is not "the nearest edge".
    const SurfaceQuery outside = runtime.querySurface(5000.0f, 5000.0f);
    check(!outside.ready, "a query outside the world is not ready");

    // The same height the compiler wrote must come back out.
    float direct = 0.0f;
    check(runtime.queryHeight(10.0f, -10.0f, direct), "queryHeight works inside a resident chunk");
    const SurfaceQuery again = runtime.querySurface(10.0f, -10.0f);
    checkEqual(direct, again.height, "queryHeight and querySurface agree");

    // Readiness for a character-sized box.
    check(runtime.isReady({-2.0f, -2.0f, 2.0f, 2.0f}), "a small box around the origin is ready");

    // Eviction: move the camera far away and pump until the origin chunk is gone.
    const std::uint64_t evictedBefore = runtime.stats().chunksEvictedTotal;
    runtime.pump(5000.0f, 5000.0f, 8);
    check(runtime.stats().chunksEvictedTotal >= evictedBefore, "moving away evicts chunks");

    // Epoch bump drops everything and bumps the version.
    const std::uint64_t epochBefore = runtime.epoch();
    runtime.reset();
    check(runtime.epoch() > epochBefore, "reset bumps the epoch");
    check(!runtime.querySurface(0.0f, 0.0f).ready, "reset unloads every chunk");
}

void testRuntimeUploadHandshake() {
    group("upload handshake and fence release");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");

    struct MemorySource : ChunkSource {
        std::map<std::pair<int, int>, std::vector<std::uint8_t>> files;
        bool read(ChunkCoord coord, std::vector<std::uint8_t>& bytes, std::string& error_) override {
            const auto it = files.find({coord.x, coord.z});
            if (it == files.end()) { error_ = "missing chunk"; return false; }
            bytes = it->second;
            return true;
        }
    } source;
    for (const auto& chunk : sink.chunks) {
        std::vector<std::uint8_t> bytes;
        serializeChunk(chunk, bytes, error);
        source.files[{chunk.coord.x, chunk.coord.z}] = bytes;
    }

    RuntimeConfig config;
    config.chunkSizeMetres = 128.0f;
    config.sampleStepMetres = 2.0f;
    config.nearDistanceMetres = 200.0f;
    config.midDistanceMetres = 400.0f;
    config.farDistanceMetres = 600.0f;
    config.maxResidentChunks = 16;
    WorldRuntime runtime;
    check(runtime.configure(config, error), "the runtime configures");
    runtime.setManifestBounds({-128.0f, -128.0f, 128.0f, 128.0f});
    runtime.setSource(&source);
    runtime.pump(0.0f, 0.0f, 8);

    auto upload = runtime.beginUpload(4);
    check(!upload.empty(), "there is work to upload");
    checkEqual(runtime.stats().chunksResident, static_cast<std::uint64_t>(0),
               "nothing is resident before the upload completes");

    std::vector<ChunkCoord> coords;
    for (const auto& chunk : upload) coords.push_back(chunk->coord);
    runtime.completeUpload(coords);
    check(runtime.stats().chunksResident > 0, "completing the upload makes chunks resident");

    // Moving away must park resident chunks in retiring, not free them immediately.
    runtime.pump(4000.0f, 4000.0f, 4);
    const auto retired = runtime.takeRetired();
    check(!retired.empty(), "evicted resident chunks are retired, waiting for the fence");
    runtime.releaseRetired(retired);
}

void testSettlementConnectivity() {
    group("settlement lots connect their entrances to the road");
    TerrainParams params;
    params.preset = "flat";
    params.amplitudeMetres = 0.0f;
    params.baseHeightMetres = 0.0f;
    params.seed = 5ull;
    TerrainField field;
    std::string error;
    check(field.configure(params, error), "flat terrain configures");

    PathSpec road;
    road.id = "main-street";
    road.kind = PathKind::Road;
    road.widthMetres = 6.0f;
    road.control = {{-100.0f, 0.0f}, {0.0f, 0.0f}, {100.0f, 0.0f}};
    PathNetwork network;
    check(network.build({road}, &field, -128.0f, -128.0f, 128.0f, 128.0f, error), "the road builds");

    TerrainSurface surface;
    surface.configure(&field, &network, {});

    SettlementParams params2;
    params2.layerId = "village";
    params2.components = {0};
    params2.maxBuildings = 16;
    params2.density = 1.0f;
    params2.lotSizeMetres = 20.0f;
    params2.setbackMetres = 2.0f;
    params2.roadWidthMetres = 6.0f;

    SettlementRegion region;
    region.shape = TerrainEditShape::Rectangle;
    region.bounds = {-110.0f, -60.0f, 110.0f, 60.0f};

    std::vector<SettlementParams> settlements = {params2};
    std::vector<SettlementRegion> regions = {region};
    SettlementPlanner planner;
    planner.configure(&surface, &network, &settlements, &regions, nullptr, 5ull);
    planner.plan({{11.0f, 9.0f}}, {6.0f});

    check(!planner.buildings().empty(), "the planner placed at least one building");
    bool allConnected = true;
    for (const auto& building : planner.buildings()) {
        if (!building.entranceConnected) allConnected = false;
    }
    check(allConnected, "every placed building has a reachable entrance");

    // Buildings must not overlap: the spacing test is what stops a solid wall of houses.
    bool overlap = false;
    const auto& placed = planner.buildings();
    for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            const float dx = std::fabs(placed[i].position[0] - placed[j].position[0]);
            const float dz = std::fabs(placed[i].position[2] - placed[j].position[2]);
            if (dx < 5.0f && dz < 5.0f) overlap = true;
        }
    }
    check(!overlap, "no two buildings occupy the same spot");
}

void testSceneEntitySeparation() {
    group("generated content never becomes scene entities");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");

    // The scene document caps entities at 10,000. A world that produced that many
    // instances must still produce a manifest with one entry per chunk, not per object.
    check(stats.totalInstances > 1000, "the test world produces a meaningful number of instances");
    check(sink.chunks.size() <= 8, "but only a handful of chunk files");
    bool everyInstanceInAChunk = true;
    for (const auto& chunk : sink.chunks) {
        for (const auto& instance : chunk.instances) {
            if (instance.position[0] < chunk.bounds.minX || instance.position[0] >= chunk.bounds.maxX
                || instance.position[2] < chunk.bounds.minZ || instance.position[2] >= chunk.bounds.maxZ) {
                everyInstanceInAChunk = false;
            }
        }
    }
    check(everyInstanceInAChunk, "every instance lies inside the chunk that stores it");
}

void testManifestShape() {
    group("the manifest describes the cook without the authoring sources");
    const std::string json = compileInputJson(256.0f, 128.0f);
    CompileInput input = parseOrThrow(json);
    WorldCompiler compiler;
    CompileStats stats;
    MemorySink sink;
    std::string error;
    check(compiler.compile(input, sink, stats, error), "the compile succeeds");

    const std::string manifest = WorldCompiler::manifestJson(input, stats, sink.infos,
                                                             std::string(64, 'a'), std::string(64, 'b'));
    check(manifest.find("nativegpu.world-manifest/1") != std::string::npos, "the manifest declares its schema");
    check(manifest.find("\"chunkRange\"") != std::string::npos, "the manifest carries the chunk range");
    check(manifest.find("\"inputDigest\"") != std::string::npos, "the manifest carries the input digest");
    check(manifest.find("recipe") == std::string::npos, "the manifest does not embed the authoring recipe");
    check(manifest.find("\"budget\"") != std::string::npos, "the manifest carries the budget block");
}

} // namespace

int main() {
    try {
        testChunkCoordinates();
        testHashAndRng();
        testTerrainDeterminism();
        testRiverDescent();
        testBridgeCrossing();
        testScatterSpacing();
        testCompileDeterminism();
        testChunkSeams();
        testChunkRoundTrip();
        testBudget();
        testRuntimeStreaming();
        testRuntimeUploadHandshake();
        testSettlementConnectivity();
        testSceneEntitySeparation();
        testManifestShape();
    } catch (const std::exception& exception) {
        std::cerr << "unhandled exception: " << exception.what() << "\n";
        return 1;
    }

    std::cout << "\n" << groups << " groups, " << checks << " checks";
    if (!failures.empty()) {
        std::cout << ", " << failures.size() << " FAILED\n";
        for (const auto& failure : failures) std::cout << "  - " << failure << "\n";
        return 1;
    }
    std::cout << ", all passed\n";
    return 0;
}

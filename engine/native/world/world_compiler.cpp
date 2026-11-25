#include "world_compiler.hpp"

#include "world_noise.hpp"

#include "../../../third_party/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace engine {
namespace world {
namespace {

using Json = nlohmann::json;

float numberOr(const Json& value, const char* key, float fallback) {
    if (!value.contains(key)) return fallback;
    const auto& field = value.at(key);
    if (!field.is_number()) return fallback;
    const double parsed = field.get<double>();
    return std::isfinite(parsed) ? static_cast<float>(parsed) : fallback;
}

std::string stringOr(const Json& value, const char* key, const char* fallback) {
    if (!value.contains(key) || !value.at(key).is_string()) return fallback;
    return value.at(key).get<std::string>();
}

bool boolOr(const Json& value, const char* key, bool fallback) {
    if (!value.contains(key) || !value.at(key).is_boolean()) return fallback;
    return value.at(key).get<bool>();
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Sutherland-Hodgman clip of a convex polygon against an axis-aligned rectangle. The road
// ribbon is a quad and the river bed strip is convex, so this is exact and gap-free: a
// chunk emits precisely the part of the surface that lies inside it.
void clipPolygonToRect(std::vector<std::array<float, 2>>& polygon, const Bounds2& rect) {
    const auto clipEdge = [](std::vector<std::array<float, 2>>& input, int axis, float value, bool keepGreater) {
        std::vector<std::array<float, 2>> output;
        if (input.empty()) return output;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto& current = input[i];
            const auto& previous = input[(i + input.size() - 1) % input.size()];
            const float currentValue = current[static_cast<std::size_t>(axis)];
            const float previousValue = previous[static_cast<std::size_t>(axis)];
            const bool currentInside = keepGreater ? currentValue >= value : currentValue <= value;
            const bool previousInside = keepGreater ? previousValue >= value : previousValue <= value;
            if (currentInside != previousInside) {
                const float denominator = currentValue - previousValue;
                const float t = std::fabs(denominator) > 1.0e-9f ? (value - previousValue) / denominator : 0.0f;
                output.push_back({previous[0] + (current[0] - previous[0]) * t,
                                  previous[1] + (current[1] - previous[1]) * t});
            }
            if (currentInside) output.push_back(current);
        }
        return output;
    };
    polygon = clipEdge(polygon, 0, rect.minX, true);
    polygon = clipEdge(polygon, 0, rect.maxX, false);
    polygon = clipEdge(polygon, 1, rect.minZ, true);
    polygon = clipEdge(polygon, 1, rect.maxZ, false);
}

struct RibbonVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Height of a road sample, honouring a bridge deck. The deck is the road surface where the
// road crosses water; using the terrain-following profile there would draw the road under
// the river.
float roadSampleHeight(const CompiledPath& path, std::size_t index) {
    for (const auto& deck : path.decks) {
        if (index >= deck.first && index <= deck.last) return deck.deckHeight;
    }
    return path.samples[index].centreHeight;
}

} // namespace

bool WorldCompiler::parseInput(const std::string& jsonText, CompileInput& out, std::string& error) {
    Json document;
    try {
        document = Json::parse(jsonText);
    } catch (const std::exception& exception) {
        error = std::string("compile input is not valid JSON: ") + exception.what();
        return false;
    }
    if (!document.is_object()) { error = "compile input must be a JSON object"; return false; }
    if (stringOr(document, "schema", "") != "nativegpu.world-compile/1") {
        error = "compile input schema must be 'nativegpu.world-compile/1'";
        return false;
    }

    CompileInput input;
    input.worldId = stringOr(document, "worldId", "");
    if (input.worldId.empty() || input.worldId.size() > 64) { error = "worldId must be 1-64 characters"; return false; }
    input.generatorVersion = stringOr(document, "generatorVersion", "worldgen-1");

    const std::string seedText = stringOr(document, "seed", "0");
    if (!parseSeed(seedText, input.seed)) { error = "seed must be a decimal string within uint64"; return false; }

    if (document.contains("extentMetres") && document.at("extentMetres").is_array()
        && document.at("extentMetres").size() == 2) {
        input.extentMetres[0] = document.at("extentMetres")[0].get<float>();
        input.extentMetres[1] = document.at("extentMetres")[1].get<float>();
    }
    if (document.contains("originMetres") && document.at("originMetres").is_array()
        && document.at("originMetres").size() == 3) {
        for (int i = 0; i < 3; ++i) input.originMetres[i] = document.at("originMetres")[i].get<float>();
    }
    if (!(input.extentMetres[0] >= 16.0f && input.extentMetres[0] <= 8192.0f)
        || !(input.extentMetres[1] >= 16.0f && input.extentMetres[1] <= 8192.0f)) {
        error = "extentMetres must be within [16, 8192]";
        return false;
    }
    input.chunkSizeMetres = numberOr(document, "chunkSizeMetres", 128.0f);
    if (!(input.chunkSizeMetres >= 16.0f && input.chunkSizeMetres <= 512.0f)) {
        error = "chunkSizeMetres must be within [16, 512]";
        return false;
    }
    input.sampleStepMetres = numberOr(document, "sampleStepMetres", 1.0f);
    if (!(input.sampleStepMetres >= 0.25f && input.sampleStepMetres <= 8.0f)) {
        error = "sampleStepMetres must be within [0.25, 8]";
        return false;
    }
    input.budgetProfile = stringOr(document, "budgetProfile", "desktop-balanced");

    if (document.contains("terrain") && document.at("terrain").is_object()) {
        const auto& terrain = document.at("terrain");
        input.terrain.preset = stringOr(terrain, "preset", "mountain-valley");
        input.terrain.amplitudeMetres = numberOr(terrain, "amplitudeMetres", 90.0f);
        input.terrain.baseHeightMetres = numberOr(terrain, "baseHeightMetres", 0.0f);
        input.terrain.ridgeStrength = numberOr(terrain, "ridgeStrength", 0.55f);
        input.terrain.erosionPasses = static_cast<int>(numberOr(terrain, "erosionPasses", 2.0f));
    }
    input.terrain.seed = input.seed;
    input.terrain.sampleStepMetres = input.sampleStepMetres;

    if (document.contains("components") && document.at("components").is_array()) {
        const auto& components = document.at("components");
        if (components.size() > static_cast<std::size_t>(Limits::kMaxComponents)) {
            error = "too many components";
            return false;
        }
        for (const auto& entry : components) {
            if (!entry.is_object()) continue;
            ComponentSpec component;
            component.id = stringOr(entry, "id", "");
            component.category = stringOr(entry, "category", "");
            component.shape = stringOr(entry, "shape", "box");
            if (entry.contains("sizeMetres") && entry.at("sizeMetres").is_array() && entry.at("sizeMetres").size() == 3) {
                for (int i = 0; i < 3; ++i) component.sizeMetres[i] = entry.at("sizeMetres")[i].get<float>();
            }
            if (entry.contains("albedo") && entry.at("albedo").is_array() && entry.at("albedo").size() == 3) {
                for (int i = 0; i < 3; ++i) component.albedo[i] = entry.at("albedo")[i].get<float>();
            }
            component.roughness = numberOr(entry, "roughness", 0.85f);
            component.metalness = numberOr(entry, "metalness", 0.0f);
            component.collision = boolOr(entry, "collision", false);
            if (entry.contains("collisionHalfExtents") && entry.at("collisionHalfExtents").is_array()
                && entry.at("collisionHalfExtents").size() == 3) {
                for (int i = 0; i < 3; ++i) component.collisionHalfExtents[i] = entry.at("collisionHalfExtents")[i].get<float>();
            }
            component.index = static_cast<std::uint16_t>(input.components.size());
            input.components.push_back(std::move(component));
        }
    }

    if (document.contains("paths") && document.at("paths").is_array()) {
        for (const auto& entry : document.at("paths")) {
            if (!entry.is_object()) continue;
            PathSpec spec;
            spec.id = stringOr(entry, "id", "");
            const std::string kind = stringOr(entry, "kind", "road");
            spec.kind = kind == "river" ? PathKind::River : PathKind::Road;
            spec.widthMetres = numberOr(entry, "widthMetres", spec.kind == PathKind::River ? 12.0f : 4.0f);
            spec.shoulderMetres = numberOr(entry, "shoulderMetres", 0.6f);
            spec.bedDepthMetres = numberOr(entry, "bedDepthMetres", 1.0f);
            spec.flowMetresPerSecond = numberOr(entry, "flowMetresPerSecond", 1.0f);
            spec.maxSlopeDegrees = numberOr(entry, "maxSlopeDegrees", 20.0f);
            spec.closed = boolOr(entry, "closed", false);
            spec.bridge = boolOr(entry, "bridge", true);
            spec.visible = boolOr(entry, "visible", true);
            if (entry.contains("points") && entry.at("points").is_array()) {
                for (const auto& point : entry.at("points")) {
                    if (!point.is_array() || point.size() != 2) continue;
                    PathPoint pathPoint;
                    pathPoint.x = point[0].get<float>();
                    pathPoint.z = point[1].get<float>();
                    spec.control.push_back(pathPoint);
                }
            }
            input.paths.push_back(std::move(spec));
        }
    }

    if (document.contains("terrainEdits") && document.at("terrainEdits").is_array()) {
        for (const auto& entry : document.at("terrainEdits")) {
            if (!entry.is_object()) continue;
            TerrainEdit edit;
            const std::string mode = stringOr(entry, "mode", "raise");
            if (mode == "lower") edit.mode = TerrainEditMode::Lower;
            else if (mode == "smooth") edit.mode = TerrainEditMode::Smooth;
            else if (mode == "flatten") edit.mode = TerrainEditMode::Flatten;
            else if (mode == "water") edit.mode = TerrainEditMode::Water;
            else if (mode == "paint") edit.mode = TerrainEditMode::Paint;
            const std::string shape = stringOr(entry, "shape", "circle");
            if (shape == "rectangle") edit.shape = TerrainEditShape::Rectangle;
            else if (shape == "polygon") edit.shape = TerrainEditShape::Polygon;
            edit.strength = numberOr(entry, "strength", 1.0f);
            edit.deltaMetres = numberOr(entry, "deltaMetres", 1.0f);
            edit.targetHeight = numberOr(entry, "targetHeight", 0.0f);
            edit.falloffMetres = numberOr(entry, "falloffMetres", 0.0f);
            edit.erase = boolOr(entry, "erase", false);
            if (entry.contains("center") && entry.at("center").is_array() && entry.at("center").size() == 2) {
                edit.centreX = entry.at("center")[0].get<float>();
                edit.centreZ = entry.at("center")[1].get<float>();
            }
            edit.radiusMetres = numberOr(entry, "radiusMetres", 0.0f);
            if (entry.contains("bounds") && entry.at("bounds").is_array() && entry.at("bounds").size() == 4) {
                edit.bounds.minX = entry.at("bounds")[0].get<float>();
                edit.bounds.minZ = entry.at("bounds")[1].get<float>();
                edit.bounds.maxX = entry.at("bounds")[2].get<float>();
                edit.bounds.maxZ = entry.at("bounds")[3].get<float>();
            }
            if (entry.contains("points") && entry.at("points").is_array()) {
                for (const auto& point : entry.at("points")) {
                    if (!point.is_array() || point.size() != 2) continue;
                    edit.polygon.push_back({point[0].get<float>(), point[1].get<float>()});
                }
            }
            if (edit.shape == TerrainEditShape::Circle) {
                edit.bounds = {edit.centreX - edit.radiusMetres, edit.centreZ - edit.radiusMetres,
                               edit.centreX + edit.radiusMetres, edit.centreZ + edit.radiusMetres};
            } else if (edit.shape == TerrainEditShape::Polygon && !edit.polygon.empty()) {
                Bounds2 bounds = emptyBounds();
                for (const auto& point : edit.polygon) bounds.include(point.x, point.z);
                bounds.expand(edit.falloffMetres + 0.5f);
                edit.bounds = bounds;
            }
            if (edit.bounds.valid()) input.terrainEdits.push_back(std::move(edit));
        }
    }

    if (document.contains("scatter") && document.at("scatter").is_array()) {
        for (const auto& entry : document.at("scatter")) {
            if (!entry.is_object()) continue;
            ScatterRule rule;
            rule.layerId = stringOr(entry, "layerId", "");
            const std::string componentId = stringOr(entry, "component", "");
            rule.component = 0;
            for (const auto& component : input.components) {
                if (component.id == componentId) { rule.component = component.index; break; }
            }
            rule.densityPerHectare = numberOr(entry, "densityPerHectare", 100.0f);
            rule.maxSlopeDegrees = numberOr(entry, "maxSlopeDegrees", 35.0f);
            rule.minSpacingMetres = numberOr(entry, "minSpacingMetres", 0.0f);
            rule.exclusionRadiusMetres = numberOr(entry, "exclusionRadiusMetres", 0.0f);
            rule.edgeFalloffMetres = numberOr(entry, "edgeFalloffMetres", 0.0f);
            rule.clumping = clamp01(numberOr(entry, "clumping", 0.0f));
            rule.scaleMin = numberOr(entry, "scaleMin", 0.9f);
            rule.scaleMax = numberOr(entry, "scaleMax", 1.1f);
            rule.alignToSlope = boolOr(entry, "alignToSlope", false);
            rule.avoidWater = boolOr(entry, "avoidWater", true);
            rule.avoidRoads = boolOr(entry, "avoidRoads", true);
            rule.blockedByBuildings = boolOr(entry, "blockedByBuildings", true);
            const std::string category = stringOr(entry, "category", "grass");
            if (category == "tree") rule.exclusionCategory = kExcludeTrees;
            else if (category == "rock") rule.exclusionCategory = kExcludeRocks;
            else if (category == "decor") rule.exclusionCategory = kExcludeDecor;
            else rule.exclusionCategory = kExcludeGrass;
            rule.seedStream = hashText(0x5ca77e2ull, rule.layerId);
            input.scatter.push_back(std::move(rule));
        }
    }

    if (document.contains("densityRegions") && document.at("densityRegions").is_array()) {
        for (const auto& entry : document.at("densityRegions")) {
            if (!entry.is_object()) continue;
            DensityRegion region;
            const std::string shape = stringOr(entry, "shape", "circle");
            if (shape == "rectangle") region.shape = TerrainEditShape::Rectangle;
            else if (shape == "polygon") region.shape = TerrainEditShape::Polygon;
            region.strength = numberOr(entry, "strength", 1.0f);
            region.falloffMetres = numberOr(entry, "falloffMetres", 0.0f);
            region.erase = boolOr(entry, "erase", false);
            if (entry.contains("center") && entry.at("center").is_array() && entry.at("center").size() == 2) {
                region.centreX = entry.at("center")[0].get<float>();
                region.centreZ = entry.at("center")[1].get<float>();
            }
            region.radiusMetres = numberOr(entry, "radiusMetres", 0.0f);
            if (entry.contains("bounds") && entry.at("bounds").is_array() && entry.at("bounds").size() == 4) {
                region.bounds.minX = entry.at("bounds")[0].get<float>();
                region.bounds.minZ = entry.at("bounds")[1].get<float>();
                region.bounds.maxX = entry.at("bounds")[2].get<float>();
                region.bounds.maxZ = entry.at("bounds")[3].get<float>();
            }
            if (entry.contains("points") && entry.at("points").is_array()) {
                for (const auto& point : entry.at("points")) {
                    if (!point.is_array() || point.size() != 2) continue;
                    region.polygon.push_back({point[0].get<float>(), point[1].get<float>()});
                }
            }
            if (region.shape == TerrainEditShape::Circle) {
                region.bounds = {region.centreX - region.radiusMetres, region.centreZ - region.radiusMetres,
                                 region.centreX + region.radiusMetres, region.centreZ + region.radiusMetres};
            } else if (region.shape == TerrainEditShape::Polygon && !region.polygon.empty()) {
                Bounds2 bounds = emptyBounds();
                for (const auto& point : region.polygon) bounds.include(point.x, point.z);
                bounds.expand(region.falloffMetres + 0.5f);
                region.bounds = bounds;
            }
            if (region.bounds.valid()) input.densityRegions.push_back(std::move(region));
        }
    }

    if (document.contains("settlements") && document.at("settlements").is_array()) {
        for (const auto& entry : document.at("settlements")) {
            if (!entry.is_object()) continue;
            SettlementParams params;
            params.layerId = stringOr(entry, "layerId", "");
            params.lotSizeMetres = numberOr(entry, "lotSizeMetres", 20.0f);
            params.setbackMetres = numberOr(entry, "setbackMetres", 2.5f);
            params.roadWidthMetres = numberOr(entry, "roadWidthMetres", 5.0f);
            params.entranceClearanceMetres = numberOr(entry, "entranceClearanceMetres", 3.0f);
            params.maxSlopeDegrees = numberOr(entry, "maxSlopeDegrees", 8.0f);
            params.buildingSpacingMetres = numberOr(entry, "buildingSpacingMetres", 4.0f);
            params.maxBuildings = static_cast<int>(numberOr(entry, "maxBuildings", 256.0f));
            params.density = clamp01(numberOr(entry, "density", 0.75f));
            params.scaleMin = numberOr(entry, "scaleMin", 0.92f);
            params.scaleMax = numberOr(entry, "scaleMax", 1.08f);
            if (entry.contains("components") && entry.at("components").is_array()) {
                for (const auto& id : entry.at("components")) {
                    if (!id.is_string()) continue;
                    const std::string wanted = id.get<std::string>();
                    for (const auto& component : input.components) {
                        if (component.id == wanted) { params.components.push_back(component.index); break; }
                    }
                }
            }
            params.seedStream = hashText(0x5e771eull, params.layerId);
            input.settlements.push_back(std::move(params));
        }
    }

    if (document.contains("settlementRegions") && document.at("settlementRegions").is_array()) {
        for (const auto& entry : document.at("settlementRegions")) {
            if (!entry.is_object()) continue;
            SettlementRegion region;
            const std::string shape = stringOr(entry, "shape", "circle");
            if (shape == "rectangle") region.shape = TerrainEditShape::Rectangle;
            else if (shape == "polygon") region.shape = TerrainEditShape::Polygon;
            if (entry.contains("center") && entry.at("center").is_array() && entry.at("center").size() == 2) {
                region.centreX = entry.at("center")[0].get<float>();
                region.centreZ = entry.at("center")[1].get<float>();
            }
            region.radiusMetres = numberOr(entry, "radiusMetres", 0.0f);
            if (entry.contains("bounds") && entry.at("bounds").is_array() && entry.at("bounds").size() == 4) {
                region.bounds.minX = entry.at("bounds")[0].get<float>();
                region.bounds.minZ = entry.at("bounds")[1].get<float>();
                region.bounds.maxX = entry.at("bounds")[2].get<float>();
                region.bounds.maxZ = entry.at("bounds")[3].get<float>();
            }
            if (entry.contains("points") && entry.at("points").is_array()) {
                for (const auto& point : entry.at("points")) {
                    if (!point.is_array() || point.size() != 2) continue;
                    region.polygon.push_back({point[0].get<float>(), point[1].get<float>()});
                }
            }
            if (region.shape == TerrainEditShape::Circle) {
                region.bounds = {region.centreX - region.radiusMetres, region.centreZ - region.radiusMetres,
                                 region.centreX + region.radiusMetres, region.centreZ + region.radiusMetres};
            } else if (region.shape == TerrainEditShape::Polygon && !region.polygon.empty()) {
                Bounds2 bounds = emptyBounds();
                for (const auto& point : region.polygon) bounds.include(point.x, point.z);
                region.bounds = bounds;
            }
            if (region.bounds.valid()) input.settlementRegions.push_back(std::move(region));
        }
    }

    if (document.contains("deleted") && document.at("deleted").is_array()) {
        for (const auto& value : document.at("deleted")) {
            if (!value.is_string()) continue;
            const std::string text = value.get<std::string>();
            std::uint64_t id = 0;
            if (text.size() == 16) {
                // The service serializes stable ids as 16 hex digits so a JSON document does
                // not have to carry a 64-bit integer through a double.
                id = std::strtoull(text.c_str(), nullptr, 16);
            } else if (!parseSeed(text, id)) {
                continue;
            }
            input.deletedIds.push_back(id);
        }
    }

    if (document.contains("moved") && document.at("moved").is_array()) {
        for (const auto& entry : document.at("moved")) {
            if (!entry.is_object()) continue;
            const std::string text = stringOr(entry, "id", "");
            std::uint64_t id = 0;
            if (text.size() == 16) id = std::strtoull(text.c_str(), nullptr, 16);
            else if (!parseSeed(text, id)) continue;
            WorldInstance instance;
            instance.stableId = id;
            if (entry.contains("position") && entry.at("position").is_array() && entry.at("position").size() == 3) {
                for (int i = 0; i < 3; ++i) instance.position[i] = entry.at("position")[i].get<float>();
            }
            instance.yaw = numberOr(entry, "yaw", 0.0f);
            instance.scale = numberOr(entry, "scale", 1.0f);
            instance.flags = kInstanceMoved;
            input.movedInstances.emplace_back(id, instance);
        }
    }

    if (document.contains("preserved") && document.at("preserved").is_array()) {
        for (const auto& entry : document.at("preserved")) {
            if (!entry.is_object()) continue;
            WorldInstance instance;
            const std::string text = stringOr(entry, "id", "");
            if (text.size() == 16) instance.stableId = std::strtoull(text.c_str(), nullptr, 16);
            if (entry.contains("position") && entry.at("position").is_array() && entry.at("position").size() == 3) {
                for (int i = 0; i < 3; ++i) instance.position[i] = entry.at("position")[i].get<float>();
            }
            instance.yaw = numberOr(entry, "yaw", 0.0f);
            instance.scale = numberOr(entry, "scale", 1.0f);
            instance.component = static_cast<std::uint16_t>(numberOr(entry, "component", 0.0f));
            instance.flags = static_cast<std::uint8_t>(numberOr(entry, "flags", 0.0f)) | kInstanceLocked;
            input.preserved.push_back(instance);
        }
    }

    if (document.contains("locks") && document.at("locks").is_array()) {
        for (const auto& entry : document.at("locks")) {
            if (!entry.is_array() || entry.size() != 4) continue;
            Bounds2 bounds;
            bounds.minX = entry[0].get<float>();
            bounds.minZ = entry[1].get<float>();
            bounds.maxX = entry[2].get<float>();
            bounds.maxZ = entry[3].get<float>();
            if (bounds.valid()) input.locks.push_back(bounds);
        }
    }

    out = std::move(input);
    return true;
}

bool WorldCompiler::compile(const CompileInput& input, ChunkSink& sink, CompileStats& stats, std::string& error) {
    const auto startTime = std::chrono::steady_clock::now();
    stats = CompileStats{};

    TerrainField terrain;
    if (!terrain.configure(input.terrain, error)) return false;

    PathNetwork paths;
    const Bounds2 world = input.worldBounds();
    if (!paths.build(input.paths, &terrain, world.minX, world.minZ, world.maxX, world.maxZ, error)) return false;
    stats.pathDiagnostics = paths.diagnostics();

    TerrainSurface surface;
    surface.configure(&terrain, &paths, input.terrainEdits);

    // Settlement planning runs before scatter so building footprints can become exclusion
    // zones. The planner does not know about trees, which is what keeps the two steps from
    // influencing each other.
    std::vector<std::array<float, 2>> componentSizes;
    std::vector<float> componentHeights;
    for (const auto& component : input.components) {
        componentSizes.push_back({component.sizeMetres[0], component.sizeMetres[2]});
        componentHeights.push_back(component.sizeMetres[1]);
    }

    SettlementPlanner settlement;
    settlement.configure(&surface, &paths, &input.settlements, &input.settlementRegions,
                         &input.extraExclusions, input.seed);
    settlement.plan(componentSizes, componentHeights);
    stats.settlement = settlement.stats();
    stats.settlementDiagnostics = settlement.diagnostics();

    std::vector<ExclusionZone> exclusions = input.extraExclusions;
    for (const auto& bridge : paths.bridges()) {
        ExclusionZone zone;
        const float halfLength = bridge.lengthMetres * 0.5f + 2.0f;
        const float halfWidth = bridge.widthMetres * 0.5f + 2.0f;
        const float radius = std::max(halfLength, halfWidth);
        zone.bounds = {bridge.x - radius, bridge.z - radius, bridge.x + radius, bridge.z + radius};
        zone.mask = kExcludeTrees | kExcludeRocks;
        exclusions.push_back(zone);
    }
    for (const auto& building : settlement.buildings()) {
        ExclusionZone zone;
        const float radius = std::max(building.footprintMetres[0], building.footprintMetres[1]) * 0.5f + 1.0f;
        zone.bounds = {building.position[0] - radius, building.position[2] - radius,
                       building.position[0] + radius, building.position[2] + radius};
        zone.mask = kExcludeAll;
        exclusions.push_back(zone);
    }

    // Budget: scale the authored density up front rather than dropping instances per chunk.
    // A per-chunk cut would make the thinning depend on which chunks were compiled first.
    const float areaHectares = (input.extentMetres[0] * input.extentMetres[1]) / 10000.0f;
    double estimated = 0.0;
    for (const auto& rule : input.scatter) estimated += static_cast<double>(rule.densityPerHectare) * areaHectares;
    for (const auto& params : input.settlements) estimated += params.maxBuildings;

    std::uint64_t instanceBudget = 1200000ull;
    if (input.budgetProfile == "debug") instanceBudget = 65536ull;
    else if (input.budgetProfile == "desktop-quality") instanceBudget = 2400000ull;
    else if (input.budgetProfile == "stress") instanceBudget = 8000000ull;

    std::vector<ScatterRule> rules = input.scatter;
    if (estimated > static_cast<double>(instanceBudget) && estimated > 0.0) {
        const float scale = static_cast<float>(static_cast<double>(instanceBudget) / estimated);
        stats.budgetScale = scale;
        stats.warnings.push_back("scatter density reduced to " + std::to_string(static_cast<int>(scale * 100.0f))
                                 + "% of the authored value to stay inside the instance budget");
        for (auto& rule : rules) rule.densityPerHectare *= scale;
    }

    ScatterField scatter;
    scatter.configure(&surface, &rules, &input.densityRegions, &exclusions, input.seed);

    std::unordered_set<std::uint64_t> deleted(input.deletedIds.begin(), input.deletedIds.end());
    std::unordered_map<std::uint64_t, WorldInstance> moved;
    for (const auto& entry : input.movedInstances) moved[entry.first] = entry.second;

    const ChunkRange range = chunkRangeForBounds(world, input.chunkSizeMetres);
    if (range.count() > static_cast<std::uint64_t>(Limits::kMaxChunksPerAxis) * static_cast<std::uint64_t>(Limits::kMaxChunksPerAxis)) {
        error = "world requests more chunks than the contract allows";
        return false;
    }

    // Preserved instances (from a locked region) are grouped by chunk so the per-chunk pass
    // does not have to scan the whole list.
    std::unordered_map<std::int64_t, std::vector<WorldInstance>> preservedByChunk;
    for (const auto& instance : input.preserved) {
        const std::int32_t cx = chunkIndexForMetres(instance.position[0], input.chunkSizeMetres);
        const std::int32_t cz = chunkIndexForMetres(instance.position[2], input.chunkSizeMetres);
        const std::int64_t key = (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cz);
        preservedByChunk[key].push_back(instance);
    }

    const std::vector<BuildingRecord>& buildings = settlement.buildings();
    std::vector<WorldInstance> scratch;

    for (std::int32_t cz = range.minCz; cz <= range.maxCz; ++cz) {
        for (std::int32_t cx = range.minCx; cx <= range.maxCx; ++cx) {
            ChunkCoord coord{cx, cz};
            ChunkData chunk;
            buildChunk(input, coord, surface, paths, scatter, buildings, scratch, chunk, stats);

            const std::int64_t key = (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cz);
            const auto preserved = preservedByChunk.find(key);
            if (preserved != preservedByChunk.end()) {
                chunk.instances.insert(chunk.instances.end(), preserved->second.begin(), preserved->second.end());
            }

            ChunkFileInfo info;
            if (!sink.write(chunk, info, error)) return false;
            info.cx = cx;
            info.cz = cz;
            stats.chunks += 1;
            stats.totalInstances += info.instanceCount;
            stats.vertices += static_cast<std::uint64_t>(chunk.vertexCount) * chunk.vertexCount;
            (void)deleted;
            (void)moved;
        }
    }

    stats.totalBuildings = buildings.size();
    const auto endTime = std::chrono::steady_clock::now();
    stats.cookMillis = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return true;
}

void WorldCompiler::buildChunk(const CompileInput& input, ChunkCoord coord,
                               const TerrainSurface& surface, const PathNetwork& paths,
                               const ScatterField& scatter,
                               const std::vector<BuildingRecord>& buildings,
                               std::vector<WorldInstance>& scratch,
                               ChunkData& chunk, CompileStats& stats) const {
    chunk = ChunkData{};
    chunk.coord = coord;
    chunk.lod = 0;
    chunk.chunkSizeMetres = input.chunkSizeMetres;
    chunk.sampleStepMetres = input.sampleStepMetres;
    chunk.vertexCount = static_cast<std::uint32_t>(std::lround(input.chunkSizeMetres / input.sampleStepMetres)) + 1u;
    if (chunk.vertexCount < 2) chunk.vertexCount = 2;

    const std::uint32_t count = chunk.vertexCount;
    const std::size_t vertices = static_cast<std::size_t>(count) * count;
    chunk.heights.resize(vertices);
    chunk.materialWeights.resize(vertices * 4);
    chunk.normals.resize(vertices * 2);
    chunk.query.resize(vertices);
    chunk.collision.resize(vertices);

    chunk.bounds = chunkBoundsOf(coord, input.chunkSizeMetres);
    const float step = input.sampleStepMetres;
    const float originX = chunk.bounds.minX;
    const float originZ = chunk.bounds.minZ;

    float minimum = 1.0e30f, maximum = -1.0e30f;
    for (std::uint32_t iz = 0; iz < count; ++iz) {
        for (std::uint32_t ix = 0; ix < count; ++ix) {
            const float x = originX + static_cast<float>(ix) * step;
            const float z = originZ + static_cast<float>(iz) * step;
            const SurfaceSample sample = surface.sample(x, z, step);
            const std::uint32_t index = chunk.vertexIndex(ix, iz);
            chunk.heights[index] = sample.height;
            chunk.collision[index] = sample.height;
            minimum = std::min(minimum, sample.height);
            maximum = std::max(maximum, sample.height);

            chunk.materialWeights[index * 4 + 0] = static_cast<std::uint8_t>(std::lround(clamp01(sample.weights.grass) * 255.0f));
            chunk.materialWeights[index * 4 + 1] = static_cast<std::uint8_t>(std::lround(clamp01(sample.weights.rock) * 255.0f));
            chunk.materialWeights[index * 4 + 2] = static_cast<std::uint8_t>(std::lround(clamp01(sample.weights.dirt) * 255.0f));
            chunk.materialWeights[index * 4 + 3] = static_cast<std::uint8_t>(std::lround(clamp01(sample.weights.sand) * 255.0f));

            std::int16_t encoded[2];
            encodeNormalOctahedral(sample.normal, encoded);
            chunk.normals[index * 2 + 0] = encoded[0];
            chunk.normals[index * 2 + 1] = encoded[1];

            std::uint8_t flags = 0;
            if (sample.onRoad) flags |= kQueryRoad;
            if (sample.onBridge) flags |= kQueryBridge;
            if (sample.submerged) flags |= kQueryWater;
            if (sample.slopeDegrees > 35.0f) flags |= kQuerySteep;
            chunk.query[index] = flags;
        }
    }
    chunk.minHeight = minimum;
    chunk.maxHeight = maximum;

    scatter.generateChunk(coord, input.chunkSizeMetres, scratch, stats.scatter);
    chunk.instances.reserve(scratch.size());
    for (const auto& instance : scratch) {
        // Hand edits win over generated content. A deleted object stays deleted; a moved one
        // keeps the hand position. Neither is silently restored by a regeneration.
        if (std::find(input.deletedIds.begin(), input.deletedIds.end(), instance.stableId) != input.deletedIds.end()) continue;
        WorldInstance final_ = instance;
        for (const auto& moved : input.movedInstances) {
            if (moved.first == instance.stableId) {
                final_.position[0] = moved.second.position[0];
                final_.position[1] = moved.second.position[1];
                final_.position[2] = moved.second.position[2];
                final_.yaw = moved.second.yaw;
                final_.scale = moved.second.scale;
                final_.flags |= kInstanceMoved;
                break;
            }
        }
        chunk.instances.push_back(final_);
    }

    const float chunkCentreX = chunk.bounds.centreX();
    const float chunkCentreZ = chunk.bounds.centreZ();
    for (const auto& building : buildings) {
        if (building.position[0] < chunk.bounds.minX || building.position[0] >= chunk.bounds.maxX) continue;
        if (building.position[2] < chunk.bounds.minZ || building.position[2] >= chunk.bounds.maxZ) continue;
        ChunkBuildingRecord record;
        record.component = building.component;
        record.entranceConnected = building.entranceConnected ? 1u : 0u;
        for (int i = 0; i < 3; ++i) record.position[i] = building.position[i];
        record.yaw = building.yaw;
        record.scale = building.scale;
        record.footprint[0] = building.footprintMetres[0];
        record.footprint[1] = building.footprintMetres[1];
        record.heightMetres = building.heightMetres;
        for (int i = 0; i < 3; ++i) record.entrance[i] = building.entrance[i];
        record.entranceYaw = building.entranceYaw;
        record.stableId = building.stableId;
        chunk.buildings.push_back(record);
    }

    buildRoadMesh(input, coord, paths, surface, chunk);
    buildRiverBed(input, coord, paths, chunk);
    (void)chunkCentreX;
    (void)chunkCentreZ;
}

void WorldCompiler::buildRoadMesh(const CompileInput& input, ChunkCoord coord, const PathNetwork& paths,
                                  const TerrainSurface& surface, ChunkData& chunk) const {
    (void)surface;
    const Bounds2 rect = chunkBoundsOf(coord, input.chunkSizeMetres);
    const float lift = 0.06f;

    for (const auto& path : paths.paths()) {
        if (path.spec.kind != PathKind::Road || !path.spec.visible) continue;
        const float halfWidth = path.spec.widthMetres * 0.5f;
        for (std::size_t i = 1; i < path.samples.size(); ++i) {
            const auto& a = path.samples[i - 1];
            const auto& b = path.samples[i];
            const float segmentMinX = std::min(a.x, b.x) - halfWidth;
            const float segmentMaxX = std::max(a.x, b.x) + halfWidth;
            const float segmentMinZ = std::min(a.z, b.z) - halfWidth;
            const float segmentMaxZ = std::max(a.z, b.z) + halfWidth;
            if (segmentMaxX < rect.minX || segmentMinX > rect.maxX) continue;
            if (segmentMaxZ < rect.minZ || segmentMinZ > rect.maxZ) continue;

            const float ax = -a.tz, az = a.tx;
            const float bx = -b.tz, bz = b.tx;
            const float ay = roadSampleHeight(path, i - 1) + lift;
            const float by = roadSampleHeight(path, i) + lift;

            // Quad in order: a-left, b-left, b-right, a-right.
            std::vector<std::array<float, 2>> polygon = {
                {a.x + ax * halfWidth, a.z + az * halfWidth},
                {b.x + bx * halfWidth, b.z + bz * halfWidth},
                {b.x - bx * halfWidth, b.z - bz * halfWidth},
                {a.x - ax * halfWidth, a.z - az * halfWidth},
            };
            const std::vector<float> ys = {ay, by, by, ay};
            clipPolygonToRect(polygon, rect);
            if (polygon.size() < 3) continue;

            const auto heightAt = [&](const std::array<float, 2>& point) {
                // Bilinear along the segment: the quad's two ends carry different heights,
                // and interpolating by projection keeps the deck flat across its width.
                const float dx = b.x - a.x;
                const float dz = b.z - a.z;
                const float lengthSquared = dx * dx + dz * dz;
                if (lengthSquared < 1.0e-9f) return ay;
                float t = ((point[0] - a.x) * dx + (point[1] - a.z) * dz) / lengthSquared;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                return ay + (by - ay) * t;
            };
            for (std::size_t tri = 1; tri + 1 < polygon.size(); ++tri) {
                const std::array<float, 2> points[3] = {polygon[0], polygon[tri], polygon[tri + 1]};
                for (const auto& point : points) {
                    chunk.roadMesh.push_back(point[0]);
                    chunk.roadMesh.push_back(heightAt(point));
                    chunk.roadMesh.push_back(point[1]);
                }
            }
        }
    }
}

void WorldCompiler::buildRiverBed(const CompileInput& input, ChunkCoord coord, const PathNetwork& paths,
                                  ChunkData& chunk) const {
    const Bounds2 rect = chunkBoundsOf(coord, input.chunkSizeMetres);
    constexpr int kAcross = 5;

    for (const auto& path : paths.paths()) {
        if (path.spec.kind != PathKind::River || !path.spec.visible) continue;
        const float halfWidth = path.spec.widthMetres * 0.5f;
        for (std::size_t i = 1; i < path.samples.size(); ++i) {
            const auto& a = path.samples[i - 1];
            const auto& b = path.samples[i];
            if (std::max(a.x, b.x) + halfWidth < rect.minX || std::min(a.x, b.x) - halfWidth > rect.maxX) continue;
            if (std::max(a.z, b.z) + halfWidth < rect.minZ || std::min(a.z, b.z) - halfWidth > rect.maxZ) continue;

            const float ax = -a.tz, az = a.tx;
            const float bx = -b.tz, bz = b.tx;
            for (int across = 0; across + 1 < kAcross; ++across) {
                const float t0 = -1.0f + 2.0f * static_cast<float>(across) / static_cast<float>(kAcross - 1);
                const float t1 = -1.0f + 2.0f * static_cast<float>(across + 1) / static_cast<float>(kAcross - 1);
                std::vector<std::array<float, 2>> polygon = {
                    {a.x + ax * halfWidth * t0, a.z + az * halfWidth * t0},
                    {b.x + bx * halfWidth * t0, b.z + bz * halfWidth * t0},
                    {b.x + bx * halfWidth * t1, b.z + bz * halfWidth * t1},
                    {a.x + ax * halfWidth * t1, a.z + az * halfWidth * t1},
                };
                clipPolygonToRect(polygon, rect);
                if (polygon.size() < 3) continue;
                const auto bedHeight = [&](float t) {
                    const float depth = path.spec.bedDepthMetres * (1.0f - t * t);
                    return a.centreHeight - depth - 0.02f;
                };
                const auto heightAt = [&](const std::array<float, 2>& point) {
                    const float dx = b.x - a.x;
                    const float dz = b.z - a.z;
                    const float lengthSquared = dx * dx + dz * dz;
                    if (lengthSquared < 1.0e-9f) return bedHeight(t0);
                    float t = ((point[0] - a.x) * dx + (point[1] - a.z) * dz) / lengthSquared;
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                    const float across_ = ((point[0] - (a.x + (b.x - a.x) * t)) * ax
                                         + (point[1] - (a.z + (b.z - a.z) * t)) * az) / std::max(0.001f, halfWidth);
                    return bedHeight(std::max(-1.0f, std::min(1.0f, across_)));
                };
                for (std::size_t tri = 1; tri + 1 < polygon.size(); ++tri) {
                    const std::array<float, 2> points[3] = {polygon[0], polygon[tri], polygon[tri + 1]};
                    for (const auto& point : points) {
                        chunk.riverBed.push_back(point[0]);
                        chunk.riverBed.push_back(heightAt(point));
                        chunk.riverBed.push_back(point[1]);
                    }
                }
            }
        }
    }
}

std::string WorldCompiler::manifestJson(const CompileInput& input, const CompileStats& stats,
                                        const std::vector<ChunkFileInfo>& chunks,
                                        const std::string& inputDigest,
                                        const std::string& manifestHash) {
    Json document;
    document["schema"] = "nativegpu.world-manifest/1";
    document["worldId"] = input.worldId;
    document["generatorVersion"] = input.generatorVersion;
    document["inputDigest"] = inputDigest;
    document["manifestHash"] = manifestHash;
    document["extentMetres"] = {input.extentMetres[0], input.extentMetres[1]};
    document["originMetres"] = {input.originMetres[0], input.originMetres[1], input.originMetres[2]};
    document["chunkSizeMetres"] = input.chunkSizeMetres;
    document["sampleStepMetres"] = input.sampleStepMetres;
    const ChunkRange range = chunkRangeForBounds(input.worldBounds(), input.chunkSizeMetres);
    document["chunkRange"] = {{"minCx", range.minCx}, {"minCz", range.minCz},
                              {"maxCx", range.maxCx}, {"maxCz", range.maxCz}};

    Json chunkArray = Json::array();
    for (const auto& chunk : chunks) {
        chunkArray.push_back({
            {"cx", chunk.cx}, {"cz", chunk.cz}, {"file", chunk.file}, {"sha256", chunk.sha256},
            {"bytes", chunk.bytes}, {"lod", chunk.lod},
            {"instanceCount", chunk.instanceCount}, {"vertexCount", chunk.vertexCount},
            {"bounds", {{"minX", chunk.bounds.minX}, {"minZ", chunk.bounds.minZ},
                        {"maxX", chunk.bounds.maxX}, {"maxZ", chunk.bounds.maxZ}}},
            {"minHeight", chunk.minHeight}, {"maxHeight", chunk.maxHeight},
        });
    }
    document["chunks"] = chunkArray;

    // The component palette travels with the manifest. A published build has no library
    // file and no compile request, but it still has to know what shape a component index
    // refers to and what colour it is.
    Json componentArray = Json::array();
    for (const auto& component : input.components) {
        componentArray.push_back({
            {"id", component.id}, {"category", component.category}, {"shape", component.shape},
            {"sizeMetres", {component.sizeMetres[0], component.sizeMetres[1], component.sizeMetres[2]}},
            {"albedo", {component.albedo[0], component.albedo[1], component.albedo[2]}},
            {"roughness", component.roughness}, {"metalness", component.metalness},
            {"collision", component.collision},
        });
    }
    document["components"] = componentArray;
    document["budget"] = {
        {"profile", input.budgetProfile},
        {"totalInstances", stats.totalInstances},
        {"residentChunks", chunks.size()},
        {"cookMillis", stats.cookMillis},
    };
    document["stats"] = {
        {"vertices", stats.vertices},
        {"buildings", stats.totalBuildings},
        {"budgetScale", stats.budgetScale},
        {"scatterAccepted", stats.scatter.accepted},
        {"scatterCandidates", stats.scatter.candidates},
        {"rejectedSlope", stats.scatter.rejectedSlope},
        {"rejectedWater", stats.scatter.rejectedWater},
        {"rejectedSpacing", stats.scatter.rejectedSpacing},
        {"rejectedDensity", stats.scatter.rejectedDensity},
    };
    return document.dump(2);
}

} // namespace world
} // namespace engine

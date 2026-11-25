#include "body.hpp"
#include "../assets/sha256.hpp"
#include "../../../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace engine::water {
using Json = nlohmann::json;
Error::Error(std::string value, std::string message) : std::runtime_error(std::move(message)), code(std::move(value)) {}

namespace {
void require(bool ok, const std::string& message, const char* code = "NG-E-WATER-ASSET") {
    if (!ok) throw Error(code, message);
}
void keys(const Json& value, std::initializer_list<const char*> required,
          std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), "Expected water object");
    std::set<std::string> allowed;
    for (auto key : required) { allowed.insert(key); require(value.contains(key), std::string("Missing water field: ") + key); }
    for (auto key : optional) allowed.insert(key);
    for (const auto& item : value.items()) require(allowed.count(item.key()) != 0, "Unsupported water field: " + item.key());
}
double scalar(const Json& value, double lo, double hi) {
    require(value.is_number(), "Water scalar must be numeric");
    const auto result = value.get<double>();
    require(std::isfinite(result) && result >= lo && result <= hi, "Water scalar outside range");
    return result;
}
std::uint32_t integer(const Json& value, std::uint32_t lo, std::uint32_t hi) {
    const auto result = scalar(value, lo, hi);
    require(std::floor(result) == result, "Water integer has a fractional part");
    return static_cast<std::uint32_t>(result);
}
std::string text(const Json& value) { require(value.is_string(), "Expected water string"); return value.get<std::string>(); }
std::string id(const Json& value) {
    auto result = text(value);
    require(!result.empty() && result.size() <= 64 && result[0] >= 'a' && result[0] <= 'z', "Invalid water identifier");
    for (char c : result) require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-', "Invalid water identifier");
    return result;
}
void array(const Json& value, std::size_t min, std::size_t max) {
    require(value.is_array() && value.size() >= min && value.size() <= max, "Invalid water array length");
}
std::string read(const std::filesystem::path& path, std::size_t maximum = 16 * 1024 * 1024) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && size <= maximum, "Water input missing or too large: " + path.u8string());
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(size));
    require(stream.gcount() == static_cast<std::streamsize>(size) && stream.peek() == std::char_traits<char>::eof(),
            "Water input changed during read");
    return bytes;
}
bool hashText(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Json authenticated(const std::string& bytes, const char* schema, const char* hashKey = "hash") {
    require(bytes.size() <= 16 * 1024 * 1024, "Water container too large");
    auto outer = Json::parse(bytes);
    require(outer.is_object() && outer.value("schema", std::string()) == schema && outer.contains("payloadJson"),
            "Unsupported water cook schema");
    const auto payload = text(outer.at("payloadJson"));
    require(hashText(text(outer.at(hashKey))) && assets::sha256(payload) == text(outer.at(hashKey)),
            "Water payload SHA-256 mismatch");
    auto parsed = Json::parse(payload);
    require(parsed.is_object() && parsed.value("schema", std::string()) == schema, "Water payload schema mismatch");
    return parsed;
}
}

std::shared_ptr<const Asset> parseCookedWater(const std::string& bytes) {
    try {
        const auto payload = authenticated(bytes, "nativegpu.water-cook/1");
        const auto& source = payload.at("source");
        keys(source, {"schema", "id", "revision", "seed", "backend", "extentMetres", "cellMetres", "stillHeightMetres",
                      "bathymetry", "boundaries", "waves", "material", "budget", "derived"},
             {"sources", "drains", "obstacles"});
        require(text(source.at("schema")) == "nativegpu.water/1", "Unsupported water source schema");
        auto asset = std::make_shared<Asset>();
        asset->id = id(source.at("id"));
        asset->hash = assets::sha256(text(Json::parse(bytes).at("payloadJson")));
        asset->revision = integer(source.at("revision"), 0, 0xFFFFFFFFu);
        asset->seed = integer(source.at("seed"), 0, 0xFFFFFFFFu);
        const auto backend = text(source.at("backend"));
        if (backend == "analytic") asset->backend = Backend::Analytic;
        else if (backend == "shallow") asset->backend = Backend::Shallow;
        else if (backend == "spectrum") asset->backend = Backend::Spectrum;
        else throw Error("NG-E-WATER-UNSUPPORTED", "Water backend has no native CPU reference: " + backend);
        array(source.at("extentMetres"), 2, 2);
        asset->extentX = static_cast<float>(scalar(source.at("extentMetres")[0], 0.5, 500));
        asset->extentZ = static_cast<float>(scalar(source.at("extentMetres")[1], 0.5, 500));
        asset->cellMetres = static_cast<float>(scalar(source.at("cellMetres"), 0.01, 5));
        asset->stillHeightMetres = static_cast<float>(scalar(source.at("stillHeightMetres"), -50, 200));
        const auto& bathymetry = source.at("bathymetry");
        const auto type = text(bathymetry.at("type"));
        if (type == "flat") { keys(bathymetry, {"type", "heightMetres"}); asset->bathymetry.type = BathymetryType::Flat; }
        else if (type == "slope") {
            keys(bathymetry, {"type", "heightMetres", "slopeX", "slopeZ"});
            asset->bathymetry.type = BathymetryType::Slope;
            asset->bathymetry.slopeX = static_cast<float>(scalar(bathymetry.at("slopeX"), -10, 10));
            asset->bathymetry.slopeZ = static_cast<float>(scalar(bathymetry.at("slopeZ"), -10, 10));
        } else if (type == "basin") {
            keys(bathymetry, {"type", "heightMetres", "depthMetres"});
            asset->bathymetry.type = BathymetryType::Basin;
            asset->bathymetry.depthMetres = static_cast<float>(scalar(bathymetry.at("depthMetres"), 0.01, 100));
        } else if (type == "channel") {
            keys(bathymetry, {"type", "heightMetres", "widthMetres", "depthMetres"}, {"bankSlope"});
            asset->bathymetry.type = BathymetryType::Channel;
            asset->bathymetry.channelWidthMetres = static_cast<float>(scalar(bathymetry.at("widthMetres"), 0.1, 500));
            asset->bathymetry.depthMetres = static_cast<float>(scalar(bathymetry.at("depthMetres"), 0.01, 100));
            if (bathymetry.contains("bankSlope"))
                asset->bathymetry.channelBankSlope = static_cast<float>(scalar(bathymetry.at("bankSlope"), 0, 100));
        } else if (type == "mounds") {
            keys(bathymetry, {"type", "heightMetres", "mounds"});
            asset->bathymetry.type = BathymetryType::Mounds;
            const auto& mounds = bathymetry.at("mounds");
            array(mounds, 1, 64);
            for (const auto& item : mounds) {
                keys(item, {"x", "z", "radiusMetres", "heightMetres"});
                Mound mound;
                mound.x = static_cast<float>(scalar(item.at("x"), -1000, 1000));
                mound.z = static_cast<float>(scalar(item.at("z"), -1000, 1000));
                mound.radiusMetres = static_cast<float>(scalar(item.at("radiusMetres"), 0.05, 100));
                mound.heightMetres = static_cast<float>(scalar(item.at("heightMetres"), 0, 100));
                asset->bathymetry.mounds.push_back(mound);
            }
        } else require(false, "Unsupported water bathymetry");
        asset->bathymetry.heightMetres = static_cast<float>(scalar(bathymetry.at("heightMetres"), -100, 100));
        const auto& boundaries = source.at("boundaries");
        keys(boundaries, {"type", "inflowCubicMetresPerSecond", "outflowCubicMetresPerSecond"});
        const auto boundary = text(boundaries.at("type"));
        require(boundary == "closed" || boundary == "open", "Invalid water boundary type");
        asset->openBoundaries = boundary == "open";
        asset->inflowCubicMetresPerSecond = static_cast<float>(scalar(boundaries.at("inflowCubicMetresPerSecond"), 0, 10000));
        asset->outflowCubicMetresPerSecond = static_cast<float>(scalar(boundaries.at("outflowCubicMetresPerSecond"), 0, 10000));
        const auto openings = [&](const char* name, std::vector<FlowOpening>& target) {
            if (!source.contains(name)) return;
            const auto& items = source.at(name);
            array(items, 0, 32);
            for (const auto& item : items) {
                keys(item, {"x", "z", "radiusMetres", "flowCubicMetresPerSecond"},
                     {"speedMetresPerSecond", "direction"});
                FlowOpening opening;
                opening.x = static_cast<float>(scalar(item.at("x"), -1000, 1000));
                opening.z = static_cast<float>(scalar(item.at("z"), -1000, 1000));
                opening.radiusMetres = static_cast<float>(scalar(item.at("radiusMetres"), 0.05, 100));
                opening.flowCubicMetresPerSecond =
                    static_cast<float>(scalar(item.at("flowCubicMetresPerSecond"), 0, 10000));
                if (item.contains("speedMetresPerSecond"))
                    opening.speedMetresPerSecond =
                        static_cast<float>(scalar(item.at("speedMetresPerSecond"), 0, 50));
                if (item.contains("direction")) {
                    array(item.at("direction"), 2, 2);
                    opening.directionX = static_cast<float>(scalar(item.at("direction")[0], -1, 1));
                    opening.directionZ = static_cast<float>(scalar(item.at("direction")[1], -1, 1));
                    const float length = std::sqrt(opening.directionX * opening.directionX +
                                                   opening.directionZ * opening.directionZ);
                    require(length > 1e-3f, "Water opening direction must not be zero");
                    opening.directionX /= length;
                    opening.directionZ /= length;
                }
                target.push_back(opening);
            }
        };
        openings("sources", asset->sources);
        openings("drains", asset->drains);
        if (source.contains("obstacles")) {
            const auto& items = source.at("obstacles");
            array(items, 0, 64);
            for (const auto& item : items) {
                keys(item, {"x", "z", "radiusMetres", "topHeightMetres"});
                Obstacle obstacle;
                obstacle.x = static_cast<float>(scalar(item.at("x"), -1000, 1000));
                obstacle.z = static_cast<float>(scalar(item.at("z"), -1000, 1000));
                obstacle.radiusMetres = static_cast<float>(scalar(item.at("radiusMetres"), 0.05, 100));
                obstacle.topHeightMetres =
                    static_cast<float>(scalar(item.at("topHeightMetres"), -100, 200));
                asset->obstacles.push_back(obstacle);
            }
        }
        const auto& waves = source.at("waves");
        keys(waves, {"amplitudeMetres", "wavelengthMetres", "speedMetresPerSecond", "direction", "components"});
        asset->waves.amplitudeMetres = static_cast<float>(scalar(waves.at("amplitudeMetres"), 0, 5));
        asset->waves.wavelengthMetres = static_cast<float>(scalar(waves.at("wavelengthMetres"), 0.1, 200));
        asset->waves.speedMetresPerSecond = static_cast<float>(scalar(waves.at("speedMetresPerSecond"), 0, 50));
        array(waves.at("direction"), 2, 2);
        asset->waves.directionX = static_cast<float>(scalar(waves.at("direction")[0], -1, 1));
        asset->waves.directionZ = static_cast<float>(scalar(waves.at("direction")[1], -1, 1));
        const float directionLength = std::sqrt(asset->waves.directionX * asset->waves.directionX +
                                                asset->waves.directionZ * asset->waves.directionZ);
        require(directionLength > 1e-3f, "Water wave direction must not be zero");
        asset->waves.directionX /= directionLength;
        asset->waves.directionZ /= directionLength;
        asset->waves.components = integer(waves.at("components"), 1, 16);
        const auto& material = source.at("material");
        keys(material, {"refractionIndex", "absorptionPerMetre", "foamThreshold"});
        asset->material.refractionIndex = static_cast<float>(scalar(material.at("refractionIndex"), 1, 3));
        asset->material.absorptionPerMetre = static_cast<float>(scalar(material.at("absorptionPerMetre"), 0, 100));
        asset->material.foamThreshold = static_cast<float>(scalar(material.at("foamThreshold"), 0, 10));
        const auto& budget = source.at("budget");
        keys(budget, {"maxCells", "maxSubsteps"});
        asset->maxCells = integer(budget.at("maxCells"), 16, 1048576);
        asset->maxSubsteps = integer(budget.at("maxSubsteps"), 1, 32);
        const auto& derived = source.at("derived");
        keys(derived, {"columns", "rows", "cellMetres", "extentMetres", "totalCells"});
        asset->columns = integer(derived.at("columns"), 4, 1024);
        asset->rows = integer(derived.at("rows"), 4, 1024);
        require(integer(derived.at("totalCells"), 16, 1048576) == asset->columns * asset->rows,
                "Water cell count does not match the derived grid");
        require(asset->columns * asset->rows <= asset->maxCells, "Water grid exceeds the asset budget");
        array(derived.at("extentMetres"), 2, 2);
        require(std::fabs(scalar(derived.at("extentMetres")[0], 0.5, 500) - asset->columns * asset->cellMetres) < 1e-3 &&
                std::fabs(scalar(derived.at("extentMetres")[1], 0.5, 500) - asset->rows * asset->cellMetres) < 1e-3,
                "Water derived extent does not match the grid");
        asset->extentX = asset->columns * asset->cellMetres;
        asset->extentZ = asset->rows * asset->cellMetres;
        return asset;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-WATER-ASSET", error.what()); }
}

std::shared_ptr<const Asset> loadWater(const std::string& file) { return parseCookedWater(read(std::filesystem::u8path(file))); }

Library loadWaterLibrary(const std::string& manifestFile, const std::string& assetRoot) {
    try {
        const auto path = std::filesystem::canonical(std::filesystem::u8path(manifestFile));
        const auto bytes = read(path);
        const auto payload = authenticated(bytes, "nativegpu.water-library/1", "packageHash");
        keys(payload, {"schema", "libraryHash", "bodies", "runtimeIntegrated"});
        Library result;
        result.hash = assets::sha256(text(Json::parse(bytes).at("payloadJson")));
        const auto contained = [](const std::filesystem::path& parent, const std::filesystem::path& child) {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != std::filesystem::path("..");
        };
        require(payload.at("bodies").is_array() && payload.at("bodies").size() >= 1 && payload.at("bodies").size() <= 256,
                "Invalid water library contents");
        for (const auto& entry : payload.at("bodies")) {
            keys(entry, {"id", "hash", "file"});
            const auto name = id(entry.at("id")), expected = text(entry.at("hash"));
            require(hashText(expected) && text(entry.at("file")) == expected + ".ngwater.json", "Invalid water content address");
            const auto file = std::filesystem::canonical(path.parent_path() / std::filesystem::u8path(text(entry.at("file"))));
            require(contained(path.parent_path(), file), "Cooked water escapes package directory");
            const auto asset = loadWater(file.u8string());
            require(asset->id == name && asset->hash == expected, "Water package identity mismatch");
            require(result.bodies.emplace(name, asset).second, "Duplicate water library id");
        }
        (void)assetRoot;
        return result;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-WATER-ASSET", error.what()); }
}
} // namespace engine::water

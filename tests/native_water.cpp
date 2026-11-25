#include "../engine/native/water/body.hpp"
#include "../engine/native/assets/sha256.hpp"
#include "../third_party/json.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

using namespace engine::water;
using engine::Vec3;
using Json = nlohmann::json;

namespace {
unsigned checks = 0, groups = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template <class F> void throws(F action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}

std::shared_ptr<const Asset> pool(float extent = 10.0f, float cell = 0.25f, float still = 1.0f) {
    auto asset = std::make_shared<Asset>();
    asset->id = "test_pool";
    asset->hash = "test-pool-shallow";
    asset->backend = Backend::Shallow;
    asset->columns = static_cast<std::uint32_t>(extent / cell);
    asset->rows = asset->columns;
    asset->cellMetres = cell;
    asset->extentX = asset->extentZ = asset->columns * cell;
    asset->stillHeightMetres = still;
    asset->bathymetry = {BathymetryType::Flat, 0, 0, 0, 0};
    asset->openBoundaries = false;
    asset->waves = {0, 1, 0, 1, 0, 1};
    asset->maxCells = 262144;
    asset->maxSubsteps = 8;
    return asset;
}

void libraryAndContract(const Library& library) {
    check(library.bodies.size() == 4, "water library asset count");
    for (const char* name : {"pond_still", "well_water", "river_shallow", "coast_spectrum"})
        check(library.bodies.count(name) == 1, "water library missing an authored asset");
    const auto river = library.bodies.at("river_shallow");
    check(river->backend == Backend::Shallow && river->columns == 240 && river->rows == 80, "shallow river grid");
    check(library.bodies.at("coast_spectrum")->backend == Backend::Spectrum, "spectrum backend identity");
    // Payload tampering must fail before any simulation state exists.
    std::ifstream stream("build/water/" + river->hash + ".ngwater.json");
    Json document; stream >> document;
    document["payloadJson"] = document["payloadJson"].get<std::string>() + " ";
    throws([&] { parseCookedWater(document.dump()); }, "water payload tampering not rejected");
    // A declared but unimplemented backend must be refused by code, not silently cooked.
    Json payload = Json::parse(document["payloadJson"].get<std::string>());
    payload["source"]["backend"] = "pbf";
    const auto text = payload.dump();
    Json rewritten = {{"schema", "nativegpu.water-cook/1"}, {"payloadJson", text},
                      {"hash", engine::assets::sha256(text)}};
    bool unsupported = false;
    try { parseCookedWater(rewritten.dump()); }
    catch (const Error& error) { unsupported = error.code == "NG-E-WATER-UNSUPPORTED"; }
    check(unsupported, "unimplemented pbf backend did not report an explicit unsupported error");
    ++groups;
}

void stillWater() {
    Body body(pool(10.0f, 0.25f, 1.0f));
    for (int step = 0; step < 7200; ++step) body.step(1.0f / 120.0f);
    const auto result = body.diagnostics();
    check(result.tick == 7200 && result.substeps >= 1, "still water tick accounting");
    check(result.nonFinite == 0, "still water produced non-finite state");
    check(result.maxSurfaceDeviationMetres <= 0.001f, "still water surface drift exceeded 1 mm over 60 s");
    check(result.maxDepthMetres >= 0, "still water depth went negative");
    check(result.massErrorFraction <= 0.01f, "still water lost mass");
    ++groups;
}

void conservation() {
    Body body(pool(10.0f, 0.25f, 1.0f));
    const auto initial = body.diagnostics();
    check(initial.initialVolumeCubicMetres > 90 && initial.initialVolumeCubicMetres < 110, "pool volume baseline");
    for (int step = 0; step < 7200; ++step) body.step(1.0f / 120.0f);
    const auto result = body.diagnostics();
    check(std::fabs(result.addedCubicMetres) < 1e-6f && std::fabs(result.removedCubicMetres) < 1e-6f,
          "closed pool invented a source or sink");
    check(result.massErrorFraction <= 0.01f, "closed 10 m pool mass error exceeded 1% over 60 s");
    ++groups;
}

void damBreak() {
    Body body(pool(10.0f, 0.25f, 1.0f));
    for (std::uint32_t column = 0; column < body.columns(); ++column)
        for (std::uint32_t row = 0; row < body.rows(); ++row)
            body.setDepth(column, row, column < body.columns() / 2 ? 2.0f : 0.2f);
    const auto before = body.diagnostics();
    for (int step = 0; step < 1200; ++step) body.step(1.0f / 120.0f);
    const auto after = body.diagnostics();
    check(after.nonFinite == 0, "dam break produced non-finite state");
    check(after.massErrorFraction <= 0.01f, "dam break mass error exceeded 1%");
    check(after.maxDepthMetres <= before.maxDepthMetres + 1e-3f, "dam break created water above its initial column");
    const float rightmost = body.depthAt(body.columns() - 1, body.rows() / 2);
    check(rightmost > 0.25f, "dam break front did not reach the far wall");
    check(after.maxSpeedMetresPerSecond > 0.5f, "dam break produced no flow");
    ++groups;
}

void buoyancy() {
    Body body(pool(10.0f, 0.25f, 2.0f));
    const auto floating = body.buoyancy({0, 1.8f, 0}, {1, 1, 1}, 500.0f);
    check(floating.valid, "buoyancy query outside a valid body");
    check(std::fabs(floating.submergedVolumeCubicMetres - 0.7f) <= 0.7f * 0.05f,
          "submerged volume error exceeded 5%");
    check(std::fabs(floating.displacedMassKg - 700.0f) <= 700.0f * 0.05f, "displaced mass error exceeded 5%");
    check(std::fabs(floating.centre.y - 1.65f) < 0.05f, "centre of buoyancy is wrong");
    const auto dry = body.buoyancy({0, 3.5f, 0}, {1, 1, 1}, 500.0f);
    check(!dry.valid || dry.submergedVolumeCubicMetres < 1e-4f, "a box above the surface reported displacement");
    check(!body.buoyancy({100, 1, 0}, {1, 1, 1}, 500.0f).valid, "buoyancy outside the body reported valid water");
    // A fully submerged box must displace its own volume, not more.
    const auto sunk = body.buoyancy({0, 1.0f, 0}, {1, 1, 1}, 500.0f);
    check(sunk.valid && std::fabs(sunk.submergedVolumeCubicMetres - 1.0f) <= 0.05f, "fully submerged volume");
    ++groups;
}

void queryContract(const Library& library) {
    Body pond(library.bodies.at("pond_still"));
    const auto inside = pond.query(0, 0);
    check(inside.valid && inside.depthMetres > 0 && inside.tick == 0, "analytic query inside the body");
    const float length = std::sqrt(inside.normal.x * inside.normal.x + inside.normal.y * inside.normal.y +
                                   inside.normal.z * inside.normal.z);
    check(std::fabs(length - 1) < 1e-3f, "water normal is not unit length");
    check(!pond.query(1000, 0).valid, "query outside the body reported valid water");
    check(!pond.query(std::numeric_limits<float>::quiet_NaN(), 0).valid, "NaN query reported valid water");
    const auto before = pond.digest();
    for (int step = 0; step < 60; ++step) pond.step(1.0f / 120.0f);
    check(pond.digest() != before, "analytic surface phase did not advance");
    ++groups;
}

void analyticAndSpectrum(const Library& library) {
    Body pond(library.bodies.at("pond_still"));
    const auto& waves = pond.asset().waves;
    for (int sample = 0; sample < 32; ++sample) {
        const float x = -5.0f + sample * 0.3f, z = sample * 0.17f;
        const auto query = pond.query(x, z);
        check(query.valid, "analytic query rejected an inside point");
        check(std::fabs(query.surfaceHeightMetres - pond.asset().stillHeightMetres) <= waves.amplitudeMetres * 2.5f,
              "analytic height exceeded the declared amplitude envelope");
    }
    auto flat = std::make_shared<Asset>(*library.bodies.at("pond_still"));
    flat->waves.amplitudeMetres = 0;
    Body still(flat);
    check(still.query(0, 0).surfaceHeightMetres == flat->stillHeightMetres, "zero-amplitude surface is not flat");
    Body coast(library.bodies.at("coast_spectrum"));
    const auto spectral = coast.query(0, 0);
    check(spectral.valid && std::isfinite(spectral.surfaceHeightMetres), "spectrum query is not finite");
    Body twin(library.bodies.at("coast_spectrum"));
    check(coast.query(1.5f, -2.0f).surfaceHeightMetres == twin.query(1.5f, -2.0f).surfaceHeightMetres,
          "spectrum surface is not deterministic");
    ++groups;
}

void levelEdits() {
    Body body(pool(10.0f, 0.25f, 1.0f));
    const float before = body.query(0, 0).depthMetres;
    body.setLevel(1.5f);
    const auto raised = body.diagnostics();
    check(std::fabs(body.query(0, 0).depthMetres - 1.5f) < 1e-3f && raised.massErrorFraction < 1e-6f,
          "setLevel did not rebuild the authority state");
    check(body.query(0, 0).depthMetres > before, "setLevel did not raise the surface");
    throws([&] { body.setLevel(std::numeric_limits<float>::quiet_NaN()); }, "NaN water level accepted");
    throws([&] { body.setDepth(9999, 0, 1.0f); }, "out-of-range cell edit accepted");
    throws([&] { body.step(0.0f); }, "zero water step accepted");
    throws([&] { body.step(1.0f); }, "oversized water step accepted");
    ++groups;
}

void determinism() {
    std::uint64_t reference = 0;
    for (unsigned run = 0; run < 100; ++run) {
        Body body(pool(10.0f, 0.25f, 1.0f));
        for (std::uint32_t column = 0; column < body.columns(); ++column)
            for (std::uint32_t row = 0; row < body.rows(); ++row)
                body.setDepth(column, row, column < body.columns() / 2 ? 2.0f : 0.2f);
        for (int step = 0; step < 60; ++step) body.step(1.0f / 120.0f);
        if (!reference) reference = body.digest();
        check(body.digest() == reference, "100-run CPU shallow water reference diverged");
    }
    ++groups;
}
} // namespace

int main() {
    try {
        const auto library = loadWaterLibrary("build/water/library.json", "assets");
        libraryAndContract(library);
        stillWater();
        conservation();
        damBreak();
        buoyancy();
        queryContract(library);
        analyticAndSpectrum(library);
        levelEdits();
        determinism();
        std::cout << "{\"schema\":\"nativegpu.water-reference-evidence/1\",\"ok\":true,\"groups\":" << groups
                  << ",\"checks\":" << checks << ",\"determinismRuns\":100,\"simulationBackend\":\"cpu-shallow-fv\"}"
                  << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "Water reference failed after " << checks << " checks: " << error.what() << std::endl;
        return 1;
    }
}

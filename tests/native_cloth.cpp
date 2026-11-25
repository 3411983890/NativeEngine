#include "../engine/native/cloth/asset.hpp"
#include "../engine/native/cloth/solver_cpu.hpp"
#include "../third_party/json.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <filesystem>

using namespace engine::cloth;
using engine::Vec3;
using Json = nlohmann::json;

namespace {
unsigned checks = 0, groups = 0;
std::filesystem::path cookedDirectory="build/cloth";
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template <class F> void throws(F action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
Vec3 centroid(const Solver& solver) {
    Vec3 sum{};
    for (std::size_t i = 0; i < solver.vertexCount(); ++i) sum += solver.position(i);
    return sum / static_cast<float>(solver.vertexCount());
}

void libraryAndBaking(const Library& library) {
    check(library.cloths.size() == 5, "cloth library asset count");
    for (const char* name : {"flag_banner", "canopy_tarp", "curtain_drape", "cape_cloak", "skirt_panel"})
        check(library.cloths.count(name) == 1, "cloth library missing an authored asset");
    const auto flag = library.cloths.at("flag_banner");
    check(flag->columns == 20 && flag->rows == 14 && flag->rest.size() == 280, "baked grid dimensions");
    check(flag->triangles.size() == 2 * 19 * 13, "baked triangle count");
    check(flag->stretch.size() == 19 * 14 + 20 * 13, "baked structural edge count");
    check(flag->shear.size() == 2 * 19 * 13, "baked shear edge count");
    check(flag->bend.size() == 18 * 14 + 20 * 12, "baked bend edge count");
    check(flag->pins.size() == 8, "baked pin count");
    check(flag->skinned == false && library.cloths.at("cape_cloak")->skinned, "cloth kind flags");
    float mass = 0;
    for (auto value : flag->masses) mass += value;
    check(std::fabs(mass - flag->material.arealDensityKgM2 * flag->totalAreaMetres2) < 1e-3f,
          "baked mass does not match areal density times area");
    // Content-addressed load must reject a payload edited after cook.
    std::ifstream stream(cookedDirectory/(flag->hash + ".ngcloth.json"));
    Json document; stream >> document;
    document["payloadJson"] = document["payloadJson"].get<std::string>() + " ";
    throws([&] { parseCookedCloth(document.dump()); }, "cloth payload tampering not rejected");
    document["schema"] = "nativegpu.cloth-cook/0";
    throws([&] { parseCookedCloth(document.dump()); }, "obsolete cloth cook silently accepted");
    ++groups;
}

void lodSelection(const Library& library) {
    Solver solver(library.cloths.at("canopy_tarp"));
    check(solver.lodFor(4.0f) == 0, "near LOD selection");
    check(solver.lodFor(20.0f) == 1, "mid LOD selection");
    check(solver.lodFor(1000.0f) == 2, "far LOD falls back to the coarsest level");
    ++groups;
}

void hangStability(const Library& library) {
    Solver solver(library.cloths.at("flag_banner"));
    const auto rest = solver.diagnostics();
    check(rest.tick == 0 && rest.totalMassKg > 0, "initial diagnostics");
    for (int step = 0; step < 600; ++step) solver.step(1.0f / 120.0f);
    const auto result = solver.diagnostics();
    check(result.tick == 600 && result.substeps == 4, "fixed tick and substep count");
    check(result.nonFinite == 0, "hanging cloth produced non-finite state");
    check(result.edgeErrorP95 <= 0.05f, "1 m cloth edge length P95 exceeded 5%");
    check(result.maxSpeed < 25.0f, "hanging cloth gained unbounded speed");
    for (const auto& pin : solver.asset().pins) {
        const Vec3 expected = pin.anchor;
        const Vec3 actual = solver.position(pin.vertex);
        check(std::fabs(actual.x - expected.x) < 1e-6f && std::fabs(actual.y - expected.y) < 1e-6f &&
              std::fabs(actual.z - expected.z) < 1e-6f, "pin drifted from its anchor");
    }
    // A 1 m square pinned on one edge must hang, not collapse or explode.
    const auto centre = centroid(solver);
    check(centre.y < 0.55f && centre.y > 0.05f, "hanging cloth centroid outside the plausible sag range");
    ++groups;
}

void contactPenetration(const Library& library) {
    auto asset = std::make_shared<Asset>(*library.cloths.at("flag_banner"));
    asset->pins = {{0, asset->rest[0]}, {asset->columns - 1, asset->rest[asset->columns - 1]}};
    asset->collision.spheres.push_back({{0.0f, 0.45f, 0.0f}, 0.18f});
    asset->collision.thicknessMetres = 0.004f;
    Solver solver(asset);
    float worst = 0;
    for (int step = 0; step < 360; ++step) {
        solver.step(1.0f / 120.0f);
        worst = std::max(worst, solver.diagnostics().penetrationMax);
    }
    const auto result = solver.diagnostics();
    check(result.nonFinite == 0, "collision produced non-finite state");
    check(result.penetrationP99 <= 0.002f, "cloth contact penetration P99 exceeded 2 mm");
    check(result.penetrationMax <= 0.010f, "cloth contact penetration max exceeded 10 mm");
    check(worst <= 0.010f, "transient penetration exceeded 10 mm");
    ++groups;
}

void windResponse(const Library& library) {
    Solver calm(library.cloths.at("flag_banner")), windy(library.cloths.at("flag_banner"));
    // The sheet rests in the XY plane, so the wind must blow along +Z to load its face.
    // Wind along X would only tension the weave and prove nothing about drag.
    windy.setWind({0.0f, 0.0f, 6.0f});
    for (int step = 0; step < 240; ++step) { calm.step(1.0f / 120.0f); windy.step(1.0f / 120.0f); }
    const float calmZ = centroid(calm).z, windyZ = centroid(windy).z;
    check(windyZ > calmZ + 0.02f, "wind did not deflect the cloth downwind");
    check(windy.diagnostics().nonFinite == 0 && windy.diagnostics().edgeErrorP95 <= 0.05f,
          "wind loading broke the cloth");
    // Wind must be a force, not a teleport: the sheet cannot outrun the air.
    check(windy.diagnostics().maxSpeed < 6.0f * 4.0f, "cloth speed is inconsistent with the wind field");
    throws([&] { windy.setWind({std::numeric_limits<float>::quiet_NaN(), 0, 0}); }, "non-finite wind accepted");
    ++groups;
}

void lifecycleAndTimeContract(const Library& library) {
    Solver solver(library.cloths.at("curtain_drape"));
    for (int step = 0; step < 120; ++step) solver.step(1.0f / 120.0f);
    const auto before = centroid(solver);
    solver.setPaused(true);
    for (int step = 0; step < 60; ++step) solver.step(1.0f / 120.0f);
    const auto during = centroid(solver);
    check(during.x == before.x && during.y == before.y && during.z == before.z, "paused cloth advanced");
    check(solver.diagnostics().tick == 120, "paused cloth consumed simulation ticks");
    solver.setPaused(false);
    const auto centre = centroid(solver);
    solver.setOrigin({3.0f, 1.0f, -2.0f});
    const auto moved = centroid(solver);
    check(std::fabs(moved.x - centre.x - 3.0f) < 1e-5f && std::fabs(moved.z - centre.z + 2.0f) < 1e-5f,
          "teleport did not translate the whole sheet");
    float speed = 0;
    for (std::size_t i = 0; i < solver.vertexCount(); ++i) {
        const auto v = solver.velocity(i);
        speed = std::max(speed, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    check(speed == 0, "teleport carried velocity into the new location");
    solver.reset();
    check(solver.diagnostics().tick == 0, "reset did not clear the tick");
    throws([&] { solver.step(0.0f); }, "zero cloth step accepted");
    throws([&] { solver.step(std::numeric_limits<float>::quiet_NaN()); }, "NaN cloth step accepted");
    throws([&] { solver.step(1.0f); }, "oversized cloth step accepted");
    throws([&] { solver.setSelfCollision(true); }, "unimplemented self-collision silently enabled");
    solver.setSelfCollision(false);
    ++groups;
}

void determinism(const Library& library) {
    std::uint64_t reference = 0;
    for (unsigned run = 0; run < 100; ++run) {
        Solver solver(library.cloths.at("cape_cloak"));
        solver.setWind({2.5f, 0.0f, 1.5f});
        for (int step = 0; step < 120; ++step) solver.step(1.0f / 120.0f);
        if (!reference) reference = solver.digest();
        check(solver.digest() == reference, "100-run CPU cloth reference diverged");
    }
    ++groups;
}
} // namespace

int main(int argc,char** argv) {
    try {
        std::filesystem::path libraryPath="build/cloth/library.json";
        if(argc==3&&std::string(argv[1])=="--library")libraryPath=std::filesystem::u8path(argv[2]);
        else if(argc!=1)throw std::runtime_error("Usage: native_cloth_tests [--library path]");
        cookedDirectory=libraryPath.parent_path();
        const auto library = loadClothLibrary(libraryPath.u8string(), "assets");
        libraryAndBaking(library);
        lodSelection(library);
        hangStability(library);
        contactPenetration(library);
        windResponse(library);
        lifecycleAndTimeContract(library);
        determinism(library);
        std::cout << "{\"schema\":\"nativegpu.cloth-reference-evidence/1\",\"ok\":true,\"groups\":" << groups
                  << ",\"checks\":" << checks << ",\"determinismRuns\":100,\"simulationBackend\":\"cpu-xpbd\"}"
                  << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "Cloth reference failed after " << checks << " checks: " << error.what() << std::endl;
        return 1;
    }
}

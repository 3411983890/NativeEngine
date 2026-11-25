// Native groom evidence: package parsing, scalp binding, XPBD guide solve, strand
// interpolation and coverage.

#include "../engine/native/groom/asset.hpp"
#include "../engine/native/groom/solver.hpp"
#include "../third_party/json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace engine::groom;
using engine::Vec3;

namespace {
unsigned checks = 0, groups = 0;
void require(bool ok, const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }

std::string readText(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + file);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

float lengthOf(Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
float distanceOf(Vec3 a, Vec3 b) { return lengthOf(b - a); }

Groom load(const std::string& id) {
    const auto library = nlohmann::json::parse(readText("build/groom/cooked/library.json"));
    for (const auto& entry : library.at("grooms")) {
        if (entry.at("id").get<std::string>() != id) continue;
        const auto path = "build/groom/cooked/" + entry.at("file").get<std::string>();
        return loadGroom(path, entry.at("hash").get<std::string>());
    }
    throw std::runtime_error("groom " + id + " is not in the library");
}

void packageShape() {
    const auto groom = load("bob_short");
    require(groom.id == "bob_short", "groom id was not preserved");
    require(groom.guides.size() == 320, "guide count disagrees with the fixture");
    require(groom.levels.size() == 3, "expected three LOD levels");
    require(groom.levels[0].strands == 4000, "LOD0 strand count disagrees with the fixture");
    require(groom.levels[1].strands < groom.levels[0].strands, "LOD1 must have fewer strands");
    require(groom.levels[0].curves.size() == groom.levels[0].strands, "curve table and strand count disagree");
    require(groom.bindings.size() == groom.guides.size(), "every guide needs a scalp binding");
    require(groom.scalpIndices.size() % 3 == 0, "scalp indices must describe triangles");
    require(groom.scalpIndices.size() > 300, "scalp mesh is too coarse to bind");
    for (const auto& curve : groom.guides) {
        require(curve.pointCount == groom.levels[0].segments + 1, "guide point count disagrees with the segment count");
        for (const auto radius : curve.radii) require(radius > 0 && radius <= 0.05f, "guide radius out of range");
    }
    // Units: a 0.22 m bob is a bob. A millimetre/metre mix-up here would produce a 22 cm
    // hair or a 220 m one, and every downstream stage would still "work".
    require(groom.lengthMetres > 0.1f && groom.lengthMetres < 0.6f, "groom length is not in metres");
    require(groom.widthMetres > 0.00001f && groom.widthMetres < 0.001f, "strand width is not a hair width in metres");
    ++groups;
}

void bindingFrame() {
    const auto groom = load("bob_short");
    for (std::size_t index = 0; index < groom.bindings.size(); ++index) {
        const auto& binding = groom.bindings[index];
        const auto sum = binding.bary[0] + binding.bary[1] + binding.bary[2];
        require(std::fabs(sum - 1.0f) < 1e-3f, "binding weights do not sum to 1");
        const auto i0 = groom.scalpIndices[binding.triangle * 3];
        const auto i1 = groom.scalpIndices[binding.triangle * 3 + 1];
        const auto i2 = groom.scalpIndices[binding.triangle * 3 + 2];
        const auto reconstruct = [&](int axis) {
            const auto& p = groom.scalpPositions;
            return binding.bary[0] * p[i0 * 3 + axis] + binding.bary[1] * p[i1 * 3 + axis] + binding.bary[2] * p[i2 * 3 + axis];
        };
        const Vec3 point{reconstruct(0), reconstruct(1), reconstruct(2)};
        const auto root = groom.guides[index].points.front();
        const auto distance = distanceOf(point, root);
        // The recorded offset is the distance from the root to the bind point. They must
        // agree, otherwise the stored frame belongs to a different root.
        require(std::fabs(distance - binding.offsetMetres) < 2e-5f, "binding offset disagrees with the reconstructed point");
        require(binding.rootU >= 0 && binding.rootU <= 1 && binding.rootV >= 0 && binding.rootV <= 1, "root UV outside [0,1]");
    }
    ++groups;
}

void guideSolve() {
    const auto groom = load("bob_short");
    GuideSolver solver(groom, 0);
    SolverSettings settings;
    settings.substeps = 2;
    settings.iterations = 4;
    solver.setSettings(settings);
    std::vector<CollisionSphere> spheres;
    // A head-sized sphere just inside the scalp (0.095 m radius): the guides start on the
    // surface and must never end up inside the skull.
    spheres.push_back({{0.0f, 0.0f, 0.0f}, 0.090f});
    solver.setCollision(spheres, {});
    const auto restRoot = solver.guideRoot(0);
    for (int tick = 0; tick < 240; ++tick) solver.step(1.0f / 120.0f);
    const auto stats = solver.stats();
    require(stats.steps == 240, "solver did not advance one step per tick");
    require(std::isfinite(stats.maxStretchRatio) && stats.maxStretchRatio < 1.35f, "strand segments stretched past 35%");
    require(stats.maxPenetrationMetres < 2e-3f, "guides are still inside the collision sphere");
    require(stats.maxSpeedMetresPerSecond < 12.0f, "guide speed is unbounded");
    const auto movedRoot = solver.guideRoot(0);
    require(distanceOf(restRoot, movedRoot) < 1e-6f, "the root moved during the solve");

    // Gravity must actually pull the tips down; a solver that converges to the rest pose
    // is not solving anything.
    const auto tip = solver.guidePoint(0, solver.pointCount() - 1);
    require(tip.y < groom.guides[0].points.back().y - 0.005f, "gravity did not lower the guide tip");

    // Reset restores the rest pose exactly.
    solver.reset();
    const auto resetTip = solver.guidePoint(0, solver.pointCount() - 1);
    const auto restTip = groom.guides[0].points.back();
    require(distanceOf(resetTip, restTip) < 1e-6f, "reset did not restore the rest pose");
    require(solver.stats().steps == 0, "reset did not clear the step counter");
    ++groups;
}

void windAndTeleport() {
    const auto groom = load("long_straight");
    // A settled pose is the meaningful comparison: a single second of a light pendulum is
    // mid-swing and would pass or fail depending on the phase, not on the wind model.
    const auto settle = [&](Vec3 wind) {
        GuideSolver solver(groom, 0);
        SolverSettings settings;
        settings.damping = 0.05f;
        settings.iterations = 8;
        solver.setSettings(settings);
        solver.setWind(wind);
        for (int tick = 0; tick < 600; ++tick) solver.step(1.0f / 120.0f);
        return solver;
    };
    const auto calm = settle({0, 0, 0});
    const auto positive = settle({12.0f, 0, 0});
    const auto negative = settle({-12.0f, 0, 0});
    const auto tip = [](const GuideSolver& solver) { return solver.guidePoint(0, solver.pointCount() - 1); };
    const auto calmTip = tip(calm), positiveTip = tip(positive), negativeTip = tip(negative);
    const auto forward = positiveTip.x - calmTip.x, backward = negativeTip.x - calmTip.x;
    // Wind must move the hair along the wind axis, and reversing the wind must reverse the
    // displacement. A solver that only scales the pose passes the first check and fails this.
    require(forward > 0.02f, "wind did not displace the guide tip along its axis");
    require(backward < -0.02f, "reversing the wind did not reverse the displacement");
    require(std::fabs(forward + backward) < forward * 0.2f, "wind response is not symmetric");
    // Bounded: 12 m/s must not turn a 0.5 m strand into a horizontal projectile.
    const auto displacement = distanceOf(positiveTip, calmTip);
    require(displacement < groom.lengthMetres * 1.5f, "wind displacement is unbounded");
    require(positive.stats().maxSpeedMetresPerSecond < 25.0f, "wind drove the solver past its speed cap");
    require(positive.stats().maxStretchRatio < 1.35f, "wind stretched a strand segment past 35%");

    // A teleport that keeps velocity is how hair explodes. Reset must restore the rest pose
    // exactly, including velocity.
    GuideSolver solver(groom, 0);
    solver.setWind({12.0f, 0, 0});
    for (int tick = 0; tick < 60; ++tick) solver.step(1.0f / 120.0f);
    require(distanceOf(solver.guidePoint(0, solver.pointCount() - 1), groom.guides[0].points.back()) > 1e-3f,
        "the test did not move the guide before the teleport");
    solver.reset();
    require(distanceOf(solver.guidePoint(0, solver.pointCount() - 1), groom.guides[0].points.back()) < 1e-6f,
        "teleport reset did not restore the rest pose");
    require(solver.stats().maxSpeedMetresPerSecond == 0.0f, "reset left residual velocity");
    ++groups;
}

void interpolation() {
    const auto groom = load("bob_short");
    GuideSolver solver(groom, 0);
    const auto& level = groom.levels[0];
    for (std::size_t index = 0; index < level.weights.size(); index += 97) {
        const auto& weights = level.weights[index];
        const auto points = solver.interpolate(weights, groom.levels[0].segments + 1);
        require(points.size() == groom.levels[0].segments + 1, "interpolation returned the wrong point count");
        // The interpolated root is the weighted blend of the pinned guide roots. The cook
        // adds a per-strand root jitter (<= 1.8 mm) that is not carried in the package, so
        // the runtime reproduces the strand root to within that jitter, not exactly. This
        // is a known approximation, recorded here rather than hidden behind a loose
        // tolerance on the whole curve.
        const auto root = points.front();
        require(distanceOf(root, level.curves[index].points.front()) < 0.004f,
            "interpolated root drifted further than the baked root jitter");
        for (const auto& point : points) require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z), "interpolated point is not finite");
    }
    // A coarser LOD samples the same guides by parameter, so its shape must stay inside
    // the groom bounds rather than collapsing to the root.
    const auto coarse = solver.interpolate(groom.levels[0].weights[0], groom.levels[2].segments + 1);
    require(coarse.size() == groom.levels[2].segments + 1, "coarse interpolation returned the wrong point count");
    const auto length = distanceOf(coarse.front(), coarse.back());
    require(length > groom.lengthMetres * 0.4f, "coarse interpolation collapsed the strand");
    ++groups;
}

void coverage() {
    // A 70 micrometre strand at 2 m, 1080p, 90-degree vertical FOV covers well under a
    // pixel. It must contribute partial coverage, not a solid line.
    const auto thin = strandCoveragePixels(0.00007f, 2.0f, 1080.0f, 1.0f);
    require(thin > 0.0f && thin < 0.1f, "a thin strand at 2 m covers too many pixels");
    const auto close = strandCoveragePixels(0.00007f, 0.1f, 1080.0f, 1.0f);
    require(close > thin * 15.0f, "coverage does not scale with 1/depth");
    // Tile coverage saturates with strand count and never exceeds 1.
    const auto four = tileCoverage(0.00007f, 0.2f, 1080.0f, 1.0f, 4);
    const auto eight = tileCoverage(0.00007f, 0.2f, 1080.0f, 1.0f, 8);
    require(eight > four && four > 0, "tile coverage is not monotone in sample count");
    require(eight <= 1.0f && four <= 1.0f, "tile coverage exceeded 1");
    ++groups;
}

void tamper() {
    const auto library = nlohmann::json::parse(readText("build/groom/cooked/library.json"));
    for (const auto& entry : library.at("grooms")) {
        if (entry.at("id").get<std::string>() != "brow_patch") continue;
        auto bytes = readText("build/groom/cooked/" + entry.at("file").get<std::string>());
        bytes[bytes.size() / 3] ^= 0x33;
        bool rejected = false;
        try { (void)parseGroom(bytes, entry.at("hash").get<std::string>()); } catch (const Error&) { rejected = true; }
        require(rejected, "a tampered groom package was accepted");
        ++groups;
        return;
    }
    throw std::runtime_error("brow_patch is not in the groom library");
}
}

int main() {
    try {
        packageShape();
        bindingFrame();
        guideSolve();
        windAndTeleport();
        interpolation();
        coverage();
        tamper();
        std::cout << nlohmann::json{{"schema", "nativegpu.groom-evidence/1"}, {"ok", true}, {"groups", groups}, {"checks", checks}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "groom tests failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}

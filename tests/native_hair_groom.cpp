#include "../engine/native/groom/asset.hpp"
#include "../engine/native/groom/solver.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// The authored hairstyle, inside the native groom runtime.
//
// `tests/native_groom.cpp` covers the procedural fixtures (`bob_short`, `long_straight`) from
// `nativegpu.groom/1`, where guides are grown from a spherical cap. This harness covers the
// other authoring form: `nativegpu.groom/2`, produced by `groomSourceFromHairstyle`, which
// transcribes the hairstyle's own scalp mesh, its 1806 scalp-bound guides, its 14448 strands
// and their guide-weight table.
//
// What it proves: the package written by the explicit cook parses under the same strict reader,
// the guide count and scalp bindings are the hairstyle's (not a cap's), the CPU XPBD guide solve
// runs on those guides with pinned roots and bounded stretch, collision projection lands, strand
// interpolation is finite, and two identical runs agree bit for bit.
//
// What it does not prove: that the hairstyle looks right, that the GPU guide solve matches this
// reference, or that the draw pass exists.

using namespace engine::groom;
using engine::Vec3;

namespace {
unsigned checks = 0, groups = 0;
// Whatever was measured before a failure, printed in the catch block: a threshold failure
// without the number it tripped on is not actionable.
nlohmann::json gReport = nlohmann::json::object();
void check(bool value, const std::string& why) { ++checks; if (!value) throw std::runtime_error(why); }

constexpr float TickSeconds = 1.0f / 120.0f;
float lengthOf(Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }

std::string readText(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + file);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

struct Fixture { Groom groom; nlohmann::json entry; };

Fixture loadFixture(const std::filesystem::path& libraryPath) {
    const auto library = nlohmann::json::parse(readText(libraryPath.u8string()));
    check(library.value("schema", std::string()) == "nativegpu.groom-library/1", "groom library schema");
    const auto& grooms = library.at("grooms");
    check(grooms.is_array() && grooms.size() >= 1, "groom library is empty");
    const auto& entry = grooms.at(0);
    const auto file = libraryPath.parent_path() / entry.at("file").get<std::string>();
    return { loadGroom(file.u8string(), entry.at("hash").get<std::string>()), entry };
}

std::uint64_t digest(const GuideSolver& solver) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t guide = 0; guide < solver.guideCount(); ++guide) {
        for (std::uint32_t point = 0; point < solver.pointCount(); ++point) {
            const Vec3 position = solver.guidePoint(guide, point);
            for (float value : {position.x, position.y, position.z}) {
                hash ^= static_cast<std::uint64_t>(static_cast<long long>(std::llround(static_cast<double>(value) * 1e5)));
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

void hairstyleEvidence(const Groom& groom, const nlohmann::json& entry) {
    // The whole point of this path: the counts are the hairstyle's, not a cap's.
    check(groom.guides.size() == entry.at("guides").get<std::size_t>(),
          "native guide count disagrees with the manifest");
    check(groom.levels.size() == entry.at("levels").size(), "native level count disagrees with the manifest");
    check(groom.guides.size() >= 1000, "a hairstyle groom with a handful of guides is the cap fallback, not this path");
    check(groom.scalpIndices.size() / 3 >= 2000, "the scalp must be the character's mesh, not a cap");
    check(groom.bindings.size() == groom.guides.size(), "every hairstyle guide needs a scalp binding");
    check(groom.groups.size() >= 2, "regions must survive as groom groups, otherwise region_id has one value");
    check(groom.levels[0].segments == 8, "hair fixture declares eight segments per strand");
    for (const auto& curve : groom.guides) {
        check(curve.pointCount == groom.levels[0].segments + 1, "guide point count disagrees with the level segments");
        for (const auto radius : curve.radii) check(radius > 0 && radius <= 0.05f, "guide radius out of range");
    }
    for (const auto& level : groom.levels) {
        check(level.curves.size() == level.strands, "curve table and strand count disagree");
        for (const auto& weight : level.weights) {
            const float total = weight.weight[0] + weight.weight[1] + weight.weight[2];
            check(std::fabs(total - 1.0f) <= 1e-3f, "guide weights reaching the runtime are not normalised");
            check(weight.guide[0] < groom.guides.size() && weight.guide[1] < groom.guides.size()
                  && weight.guide[2] < groom.guides.size(), "guide weight references a missing guide");
        }
    }
    // Bindings must actually reproduce the roots on this scalp, which is the property the cap
    // path could never satisfy.
    float worst = 0;
    for (std::size_t index = 0; index < groom.bindings.size(); ++index) {
        const auto& binding = groom.bindings[index];
        const auto& curve = groom.guides[index];
        const auto i0 = groom.scalpIndices[binding.triangle * 3];
        const auto i1 = groom.scalpIndices[binding.triangle * 3 + 1];
        const auto i2 = groom.scalpIndices[binding.triangle * 3 + 2];
        const auto at = [&](std::uint32_t index) {
            return Vec3{groom.scalpPositions[index * 3], groom.scalpPositions[index * 3 + 1], groom.scalpPositions[index * 3 + 2]};
        };
        const Vec3 p0 = at(i0), p1 = at(i1), p2 = at(i2);
        const Vec3 reconstructed{
            p0.x * binding.bary[0] + p1.x * binding.bary[1] + p2.x * binding.bary[2],
            p0.y * binding.bary[0] + p1.y * binding.bary[1] + p2.y * binding.bary[2],
            p0.z * binding.bary[0] + p1.z * binding.bary[1] + p2.z * binding.bary[2],
        };
        worst = std::max(worst, lengthOf(curve.points.front() - reconstructed));
    }
    check(worst <= 1e-3f, "a scalp binding does not reproduce its guide root");
    ++groups;
}

// Stiffness that holds a 15 cm strand. The solver defaults are tuned for short styles
// (brows, beard): on a scalp hairstyle they let a guide stretch 37–80% under gravity alone,
// which is a settings problem rather than a solver defect, so both numbers are reported.
constexpr float TunedIterations = 12;
constexpr float TunedSubsteps = 4;
constexpr float TunedCompliance = 5e-7f;
constexpr float TunedBendCompliance = 1e-4f;
constexpr float TunedDamping = 0.82f;

struct SolveOutcome {
    float stretch = 1, penetration = 0, speed = 0, rootDrift = 0;
};

// The collider is the head: centroid and radius of the scalp mesh the guides are bound to.
// Using the *roots'* centroid instead puts the sphere inside the hair curtain — the back and
// nape strands descend past it — and the projection then inflates them (measured 4.8× stretch).
struct HeadSphere { Vec3 centre; float radius; };

// The collider is the head, sized strictly inside the root shell.
//
// Roots are pinned and the resolver only projects points 1..N, so a sphere that swallows a root
// reports penetration the solver is not allowed to fix (measured 3.4 mm that way). Sizing the
// sphere at 0.9 × the *nearest* root keeps every root outside it by construction, which makes
// the remaining penetration purely a question about the dynamic points.
HeadSphere headSphereOf(const Groom& groom, const std::vector<Vec3>& roots) {
    const auto vertices = groom.scalpPositions.size() / 3;
    Vec3 centre{};
    for (std::size_t index = 0; index < vertices; ++index)
        centre += Vec3{groom.scalpPositions[index * 3], groom.scalpPositions[index * 3 + 1], groom.scalpPositions[index * 3 + 2]};
    centre = centre / static_cast<float>(vertices);
    float nearest = std::numeric_limits<float>::infinity();
    for (const auto root : roots) nearest = std::min(nearest, lengthOf(root - centre));
    return {centre, nearest * 0.9f};
}

SolveOutcome solveWith(const Groom& groom, const std::vector<Vec3>& roots, bool tuned, const HeadSphere& head) {
    GuideSolver solver(groom, 0);
    if (tuned) {
        SolverSettings settings = solver.settings();
        settings.iterations = static_cast<std::uint32_t>(TunedIterations);
        settings.substeps = static_cast<std::uint32_t>(TunedSubsteps);
        settings.compliance = TunedCompliance;
        settings.bendCompliance = TunedBendCompliance;
        settings.damping = TunedDamping;
        solver.setSettings(settings);
    }
    if (head.radius > 0) solver.setCollision({{head.centre, head.radius}}, {});
    for (int step = 0; step < 240; ++step) solver.step(TickSeconds);
    SolveOutcome outcome;
    for (std::uint32_t guide = 0; guide < solver.guideCount(); ++guide)
        outcome.rootDrift = std::max(outcome.rootDrift, lengthOf(solver.guideRoot(guide) - roots[guide]));
    const auto stats = solver.stats();
    outcome.stretch = stats.maxStretchRatio;
    outcome.penetration = stats.maxPenetrationMetres;
    outcome.speed = stats.maxSpeedMetresPerSecond;
    return outcome;
}

void solvesOnHairstyleGuides(const Groom& groom, nlohmann::json& report) {
    std::vector<Vec3> roots;
    {
        GuideSolver probe(groom, 0);
        for (std::uint32_t guide = 0; guide < probe.guideCount(); ++guide) roots.push_back(probe.guideRoot(guide));
        check(roots.size() == groom.guides.size(), "solver guide count disagrees with the package");
    }
    const auto defaults = solveWith(groom, roots, false, HeadSphere{});
    const auto tuned = solveWith(groom, roots, true, HeadSphere{});
    const auto head = headSphereOf(groom, roots);
    const auto collided = solveWith(groom, roots, true, head);
    report["defaultSettings"] = {{"stretchRatio", defaults.stretch}, {"penetrationMetres", defaults.penetration}};
    report["tunedWithHeadCollision"] = {
        {"headRadiusMetres", head.radius}, {"stretchRatio", collided.stretch},
        {"penetrationMetres", collided.penetration}, {"maxSpeed", collided.speed},
    };
    report["tunedSettings"] = {
        {"iterations", TunedIterations}, {"substeps", TunedSubsteps}, {"compliance", TunedCompliance},
        {"bendCompliance", TunedBendCompliance}, {"damping", TunedDamping},
        {"stretchRatio", tuned.stretch}, {"penetrationMetres", tuned.penetration}, {"maxSpeed", tuned.speed},
    };
    // The assertions live in `assertSolve` so a failure can print the numbers it failed on.
    report["rootDrift"] = {{"defaults", defaults.rootDrift}, {"tuned", tuned.rootDrift}, {"collided", collided.rootDrift}};

    // Interpolation on the hairstyle's own weight table.
    GuideSolver solver(groom, 0);
    const auto& level = groom.levels[0];
    for (std::size_t index = 0; index < std::min<std::size_t>(level.weights.size(), 256); index += 7) {
        const auto points = solver.interpolate(level.weights[index], solver.pointCount());
        check(points.size() == solver.pointCount(), "interpolation returned the wrong point count");
        float length = 0;
        for (std::size_t point = 1; point < points.size(); ++point) {
            for (const auto coordinate : {points[point].x, points[point].y, points[point].z})
                check(std::isfinite(coordinate), "interpolation produced a non-finite point");
            length += lengthOf(points[point] - points[point - 1]);
        }
        check(length > 0.02f, "an interpolated strand collapsed to nothing");
    }
    check(solver.stats().points == solver.guideCount() * solver.pointCount(), "solver point count");
    ++groups;
}

void assertSolve(const nlohmann::json& report) {
    const float defaults = report.at("defaultSettings").at("stretchRatio").get<float>();
    const float tuned = report.at("tunedSettings").at("stretchRatio").get<float>();
    const auto& collision = report.at("tunedWithHeadCollision");
    // The default stiffness is reported, not asserted away, but it must stay bounded: above 2
    // the solve would be diverging rather than merely soft.
    check(defaults < 2.0f, "the default settings diverged on a hairstyle groom");
    check(tuned < 1.15f, "tuned settings must hold a 15 cm strand under gravity");
    check(report.at("rootDrift").at("tuned").get<float>() < 1e-5f, "a guide root left its scalp binding during the solve");
    check(collision.at("penetrationMetres").get<float>() <= 2e-3f, "guides are still inside the head sphere");
    check(collision.at("stretchRatio").get<float>() < 1.3f, "head collision stretched the strands");
    ++groups;
}

void deterministic(const Groom& groom) {
    std::uint64_t reference = 0;
    for (unsigned run = 0; run < 6; ++run) {
        GuideSolver solver(groom, 0);
        solver.setWind({2.0f, 0.0f, 1.2f});
        for (int step = 0; step < 150; ++step) solver.step(TickSeconds);
        if (!reference) reference = digest(solver);
        check(digest(solver) == reference, "the hairstyle guide solve diverged between identical runs");
    }
    // Reset must restore the authored rest pose exactly, not re-simulate from the current one.
    GuideSolver solver(groom, 0);
    solver.setWind({2.0f, 0.0f, 1.2f});
    for (int step = 0; step < 60; ++step) solver.step(TickSeconds);
    solver.setWind({});
    solver.reset();
    GuideSolver fresh(groom, 0);
    check(digest(solver) == digest(fresh), "reset did not restore the hairstyle's rest pose");
    ++groups;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path libraryPath = "build/hair-groom/cooked/library.json";
        if (argc == 3 && std::string(argv[1]) == "--library") libraryPath = std::filesystem::u8path(argv[2]);
        else if (argc != 1) throw std::runtime_error("Usage: native_hair_groom_tests [--library path]");
        const auto fixture = loadFixture(libraryPath);
        hairstyleEvidence(fixture.groom, fixture.entry);
        nlohmann::json solveReport;
        solvesOnHairstyleGuides(fixture.groom, solveReport);
        gReport = solveReport;
        assertSolve(solveReport);
        deterministic(fixture.groom);
        std::cout << nlohmann::json({
            {"schema", "nativegpu.hair-groom-evidence/1"}, {"ok", true},
            {"groups", groups}, {"checks", checks},
            {"groom", {
                {"id", fixture.groom.id}, {"guides", fixture.groom.guides.size()},
                {"strands", fixture.groom.levels[0].strands},
                {"scalpTriangles", fixture.groom.scalpIndices.size() / 3},
                {"groups", fixture.groom.groups.size()},
                {"levels", fixture.groom.levels.size()},
                {"sourceSchema", "nativegpu.groom/2"},
            }},
            {"solve", solveReport},
            {"capabilities", {
                {"explicitGuideCook", true}, {"scalpBoundGuides", true}, {"cpuGuideSolve", true},
                {"strandInterpolation", true}, {"gpuGuideSolve", false}, {"strandDraw", false},
            }},
        }).dump() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hair groom evidence failed after " << checks << " checks: " << error.what() << std::endl;
        if (!gReport.empty()) std::cerr << "measured: " << gReport.dump() << std::endl;
        return 1;
    }
}

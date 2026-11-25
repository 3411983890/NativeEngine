#include "../engine/native/animation.hpp"
#include "../engine/native/game.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include "animation_features_tests.hpp"

using namespace engine::animation;
namespace {
unsigned assertions = 0;
void check(bool ok, const char *message) {
    ++assertions;
    if (!ok)
        throw std::runtime_error(message);
}
bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
template <class F> void rejects(const char *code, F action) {
    try {
        action();
    } catch (const Error &e) {
        check(e.code == code, "Unexpected error code");
        return;
    }
    throw std::runtime_error("Expected rejection");
}
std::uint64_t digest(const Controller &c) {
    engine::determinism::Hasher h;
    c.digest(h);
    return h.value();
}
void loaderFailures() {
    const std::filesystem::path original = "build/animation/mannequin";
    const std::filesystem::path scratch = "build/animation/native-loader-tests";
    std::filesystem::create_directories(scratch);
    std::ifstream input(original / "character.json");
    nlohmann::json header;
    input >> header;
    auto writeHeader = [&] {
        std::ofstream out(scratch / "character.json");
        out << header.dump();
    };
    std::filesystem::copy_file(original / "animation.bin", scratch / "animation.bin",
                               std::filesystem::copy_options::overwrite_existing);
    writeHeader();
    {
        std::fstream corrupt(scratch / "animation.bin",
                             std::ios::in | std::ios::out | std::ios::binary);
        char value = 42;
        corrupt.write(&value, 1);
    }
    rejects("NG-E-ANIM-HASH", [&] { loadAsset(scratch / "character.json"); });
    std::filesystem::copy_file(original / "animation.bin", scratch / "animation.bin",
                               std::filesystem::copy_options::overwrite_existing);
    header["clips"][0]["offset"] = 0;
    writeHeader();
    rejects("NG-E-ANIM-CLIP", [&] { loadAsset(scratch / "character.json"); });
    header["binary"] = "../animation.bin";
    writeHeader();
    rejects("NG-E-ANIM-FORMAT", [&] { loadAsset(scratch / "character.json"); });
    std::filesystem::remove(scratch / "animation.bin");
    std::filesystem::remove(scratch / "character.json");
    std::filesystem::remove(scratch);
}
void skeletonAndSampling(const std::shared_ptr<const Asset> &a) {
    auto rest = a->skeleton.restPose();
    auto palette = a->skeleton.palette(rest);
    for (const auto &m : palette)
        for (unsigned k = 0; k < 16; ++k)
            check(near(m[k], k % 5 == 0 ? 1.f : 0.f), "Bind palette must be identity");
    auto c = a->clips[a->clipIndex("walk")];
    auto first = c.sample(0), loop = c.sample(c.duration());
    check(first.rotation == loop.rotation, "Loop seam");
    auto moving = c.sample(0.25);
    check(moving.rotation[10] != first.rotation[10], "Walk must actually animate the thigh");
    check(c.sample(0.25 + 0.5 / 60).rotation[10] != moving.rotation[10],
          "Half-frame interpolation");
    c.stepChannels[10] = 2;
    check(c.sample(0.25 + 0.5 / 60).rotation[10] == moving.rotation[10],
          "STEP channel must hold between samples");
    c.stepChannels[10] = 0;
    auto nonloop = c;
    nonloop.loop = false;
    check(nonloop.sample(100).rotation == nonloop.sample(nonloop.duration()).rotation,
          "Clamp nonloop sample");
    auto anti = slerp({0, 0, 0, 1}, {0, 0, 0, -1}, 0.5f);
    check(near(anti.w, 1), "Shortest quaternion arc");
    rejects("NG-E-ANIM-CLIP", [&] { c.sample(std::numeric_limits<double>::infinity()); });
    auto invalid = a->skeleton;
    invalid.bones[1].parent = 2;
    rejects("NG-E-ANIM-SKELETON", [&] { invalid.validate(); });
    invalid = a->skeleton;
    invalid.bones[1].name = invalid.bones[0].name;
    rejects("NG-E-ANIM-SKELETON", [&] { invalid.validate(); });
    // Root delta must unwrap the full displacement across multiple loops.
    for (unsigned f = 0; f < c.frameCount; ++f)
        c.samples[f * c.boneCount].translation.z = float(f) / 60;
    check(near(c.rootDelta(0.9, 2.1).z, 1.2f), "Multi-loop root motion");
    c.loop = false;
    check(near(c.rootDelta(0.9, 2.1).z, 0.1f), "Nonloop root motion endpoint");
    auto target = rest;
    auto t = target.get(5);
    t.translation.x = 1;
    target.set(5, t);
    BoneMask mask;
    mask.set(5);
    auto mixed = blend(rest, target, 16, 0.25f, mask);
    check(near(mixed.translation[5][0], rest.translation[5][0] + 0.25f), "Per-bone layering");
    check(mixed.translation[4] == rest.translation[4], "Mask preserves other bones");
}
void spacesAndSpring() {
    auto weights = blendSpace1D({{6, 0, 2}, {0, 0, 0}, {3, 0, 1}}, 4.5f);
    check(weights.size() == 2 && weights[0].clip == 1 && near(weights[0].weight, 0.5f),
          "1D interpolation");
    check(blendSpace1D({{0, 0, 0}, {3, 0, 1}}, -1)[0].clip == 0, "1D outside clamp");
    rejects("NG-E-ANIM-BLEND", [] { blendSpace1D({{0, 0, 0}, {0, 0, 1}}, 0); });
    std::vector<BlendPoint> points{{0, 0, 0}, {1, 0, 1}, {0, 1, 2}};
    auto tri = blendSpace2D(points, {{{0, 1, 2}}}, 0.25f, 0.25f);
    check(near(tri[0].weight, 0.5f) && near(tri[1].weight, 0.25f), "2D barycentric interpolation");
    auto edge = blendSpace2D(points, {{{0, 1, 2}}}, 2, 0);
    check(near(edge[1].weight, 1), "2D projection to boundary");
    rejects("NG-E-ANIM-BLEND", [&] { blendSpace2D(points, {{{0, 0, 0}}}, 0, 0); });
    PoseBuffer from, to, prev;
    from.translation[1][0] = 1;
    prev = from;
    Inertializer spring;
    spring.begin(from, prev, to, to, 2, 1.f / 120);
    auto zero = spring.apply(to, 2, 0);
    check(near(zero.translation[1][0], 1), "Transition preserves initial position");
    check(spring.apply(to, 2, 0.25).translation[1][0] < 0.02f, "Analytic spring decays");
    // Query order and the number of previous evaluations cannot change the solution.
    auto half = spring.apply(to, 2, 0.5);
    for (unsigned i = 0; i < 120; ++i)
        spring.apply(to, 2, i / 240.0);
    check(half.translation == spring.apply(to, 2, 0.5).translation,
          "Spring is analytic and stateless");
}
void graphContracts() {
    Graph graph;
    PoseBuffer pose;
    Selection selection;
    std::vector<unsigned> order;
    Trait a;
    a.id = 4;
    a.priority = 2;
    a.stage = Stage::Blend;
    a.writeCurves = 1;
    a.evaluate = [&](const Context &, PoseBuffer &p) {
        order.push_back(4);
        p.curves[0] += 1;
    };
    Trait b = a;
    b.id = 2;
    b.priority = 1;
    b.evaluate = [&](const Context &, PoseBuffer &p) {
        order.push_back(2);
        p.curves[0] += 2;
    };
    graph.compile({a, b}, 2);
    graph.evaluate({}, selection, pose);
    check(order == std::vector<unsigned>({2, 4}), "Stable priority/id order");
    auto hashes = graph.stages();
    order.clear();
    pose = PoseBuffer{};
    graph.evaluate({}, selection, pose);
    for (unsigned i = 0; i < 5; ++i)
        check(hashes[i].hash == graph.stages()[i].hash, "Stage hash reproduction");
    check(hashes[4].hash == hashes[3].hash, "Emit excluded from digest");
    a.after = {4};
    rejects("NG-E-ANIM-GRAPH", [&] { graph.compile({a}, 2); });
    a.after.clear();
    a.deterministic = false;
    rejects("NG-E-ANIM-GRAPH", [&] { graph.compile({a}, 2); });
    a.deterministic = true;
    a.writes.set(0);
    a.stage = Stage::Deform;
    rejects("NG-E-ANIM-MASK", [&] { graph.compile({a}, 2); });
    a.stage = Stage::Emit;
    rejects("NG-E-ANIM-MASK", [&] { graph.compile({a}, 2); });
    a.writes = {};
    a.stage = Stage::Blend;
    a.evaluate = [](const Context &, PoseBuffer &p) { p.translation[1][0] = 1; };
    graph.compile({a}, 2);
    rejects("NG-E-ANIM-MASK", [&] { graph.evaluate({}, selection, pose); });
    a.evaluate = [](const Context &, PoseBuffer &p) { p.curves[1] = 1; };
    graph.compile({a}, 2);
    pose = PoseBuffer{};
    rejects("NG-E-ANIM-MASK", [&] { graph.evaluate({}, selection, pose); });
    a.writeCurves = 0;
    a.writes.set(1);
    a.stage = Stage::Deform;
    a.evaluate = [](const Context &, PoseBuffer &p) { p.translation[1][0] += 1; };
    graph.compile({a}, 2);
    pose = PoseBuffer{};
    graph.evaluate({}, selection, pose, 0.01f);
    check(near(pose.translation[1][0], 0.01f) && near(graph.lastDeformScale(), 0.01f),
          "Deform budget clamps increment");
    a.evaluate = [](const Context &, PoseBuffer &p) {
        p.translation[1][0] = std::numeric_limits<float>::quiet_NaN();
    };
    graph.compile({a}, 2);
    pose = PoseBuffer{};
    rejects("NG-E-ANIM-POSE", [&] { graph.evaluate({}, selection, pose); });
}
nlohmann::json controllers(const std::shared_ptr<const Asset> &a) {
    Controller first, second;
    first.initialize(a);
    second.initialize(a);
    std::uint64_t stageComparisons = 0, notifyCount = 0;
    std::vector<Event> contacts;
    for (std::uint64_t tick = 1; tick <= 720; ++tick) {
        float speed = tick < 60 ? 0 : tick < 180 ? 3 : tick < 300 ? 6 : tick < 400 ? 0 : 3;
        bool grounded = !(tick >= 200 && tick < 260);
        float vertical = tick < 230 ? 2 : -2;
        if (tick == 401) {
            BoneMask mask;
            mask.set(7);
            mask.set(8);
            first.play("interact", mask);
            second.play("interact", mask);
        }
        Intent in{tick, speed, vertical, 0, grounded};
        first.tick(in);
        second.tick(in);
        check(digest(first) == digest(second), "Controller hidden state must reproduce");
        for (unsigned stage = 0; stage < 5; ++stage) {
            check(first.stages()[stage].hash == second.stages()[stage].hash, "Stage divergence");
            ++stageComparisons;
        }
        if (tick == 100)
            check(first.state() == State::Walk, "Walk state");
        if (tick == 190)
            check(first.state() == State::Run, "Run state");
        if (tick == 210)
            check(first.state() == State::Jump, "Jump state");
        if (tick == 240)
            check(first.state() == State::Fall, "Fall state");
        if (tick == 360)
            check(first.state() == State::Idle, "Idle state");
        for (const auto &event : first.events()) {
            ++notifyCount;
            if (event.name == "interaction_contact")
                contacts.push_back(event);
        }
        check(first.pose().translation[0] == a->skeleton.restPose().translation[0],
              "Gameplay owns root translation");
    }
    check(contacts.size() == 1 && contacts[0].tick == 460, "Notify fires once at the fixed tick");
    auto before = digest(first);
    first.reset();
    second.reset();
    check(digest(first) == digest(second) && digest(first) != before,
          "Reset clears all animation state");
    rejects("NG-E-ANIM-TICK", [&] { first.tick({2, 0, 0, 0, true}); });
    BoneMask root;
    root.set(0);
    rejects("NG-E-ANIM-MASK", [&] { first.play("interact", root); });
    rejects("NG-E-ANIM-CLIP", [&] { first.play("unknown", {}); });
    ControllerConfig bad;
    bad.clips[2] = "missing_run";
    rejects("NG-E-ANIM-CLIP", [&] {
        Controller c;
        c.initialize(a, bad);
    });
    // Exercise the integration at two render frequencies. The fixed tick timeline
    // and all pending transition state must be equal, not just the final position.
    engine::Game sixty, oneTwenty;
    sixty.initialize({});
    oneTwenty.initialize({});
    sixty.setCharacter(a);
    oneTwenty.setCharacter(a);
    engine::Input input;
    input.forward = 1;
    for (unsigned frame = 0; frame < 100; ++frame) {
        sixty.update(input, 1.f / 60);
        oneTwenty.update(input, 1.f / 120);
        oneTwenty.update(input, 1.f / 120);
    }
    engine::determinism::Hasher h1, h2;
    sixty.digest(h1);
    oneTwenty.digest(h2);
    check(h1.value() == h2.value(), "Render frequency independence");
    engine::Game notifications;
    notifications.initialize({});
    notifications.setCharacter(a);
    notifications.update({}, 1.f / 120);
    engine::Input interact;
    interact.interact = true;
    notifications.update(interact, 1.f / 120);
    for (unsigned frame = 0; frame < 61; ++frame)
        notifications.update({}, 1.f / 60);
    auto queued = notifications.takeAnimationEvents();
    check(queued.size() == 1 && queued[0].name == "interaction_contact" && queued[0].tick == 61,
          "Game queue retains Notify from the first of two ticks in a render frame");
    check(notifications.takeAnimationEvents().empty(), "Notify queue is consumed exactly once");
    return {{"ticksCompared", 720},       {"stageComparisons", stageComparisons},
            {"notifyCount", notifyCount}, {"contactTick", contacts[0].tick},
            {"finalDigest", h1.hex()},    {"runtimeFingerprint", a->contentHash}};
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto asset = loadAsset("build/animation/mannequin/character.json");
        loaderFailures();
        skeletonAndSampling(asset);
        spacesAndSpring();
        graphContracts();
        auto evidence = controllers(asset);
        evidence["milestones"]=featureTests(asset);
        evidence["assertions"] = assertions;
        evidence["success"] = true;
        evidence["bones"] = asset->skeleton.bones.size();
        evidence["clips"] = asset->clips.size();
        if (argc == 3 && std::string(argv[1]) == "--evidence") {
            std::ofstream f(std::filesystem::u8path(argv[2]));
            f << evidence.dump(2) << '\n';
            if (!f)
                throw std::runtime_error("Cannot write evidence");
        }
        std::cout << "Animation: " << assertions << " base assertions and " << evidence["milestones"]["checks"] << " M1-M4 checks passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

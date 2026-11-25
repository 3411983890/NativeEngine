#include "../engine/native/game.hpp"
#include "../engine/native/geometry_class.hpp"
#include "../engine/native/material_handle.hpp"
#include "../engine/native/math.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using engine::Entity;
using engine::Game;
using engine::Input;
using engine::Scene;
using engine::Vec3;

namespace {
bool near(float a, float b, float tolerance = 0.002f) {
    return std::abs(a - b) < tolerance;
}

Entity object(const char* id, const char* kind, Vec3 position, Vec3 scale,
              bool collider = false) {
    Entity result;
    result.id = id;
    result.name = id;
    result.kind = kind;
    result.position = position;
    result.scale = scale;
    result.collider = collider;
    return result;
}

Scene courtyard() {
    Scene scene;
    scene.entities = {
        object("player", "player", {0, 0, 5}, {.6f, 1.8f, .6f}),
        object("ground", "ground", {0, -0.2f, 0}, {20, 0.4f, 20}, true),
        object("key", "key", {4, 0.8f, 1}, {0.25f, 0.5f, 0.25f}),
        object("gate", "door", {0, 1.5f, -8}, {2.4f, 3, 0.25f}, true),
        object("exit", "goal", {0, 0, -9}, {1.5f, 0.1f, 1.5f}),
        object("left-gate-wall", "wall", {-6, 1.5f, -8}, {9.6f, 3, 0.3f}, true),
        object("right-gate-wall", "wall", {6, 1.5f, -8}, {9.6f, 3, 0.3f}, true),
    };
    return scene;
}

void advance(Game& game, Input input, int frames, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < frames; ++i) game.update(input, dt);
}

void pressE(Game& game) {
    Input input;
    input.interact = true;
    game.update(input, 1.0f / 60.0f);
    game.update({}, 1.0f / 60.0f);
}

void testRenderVisibilityMasks() {
    Scene scene=courtyard();
    scene.entities.push_back(object("car","vehicle",{2,1,0},{2,1,4}));
    scene.entities.push_back(object("lamp","light",{-3,3,2},{0.4f,0.4f,0.4f}));
    Game game;game.initialize(scene);
    unsigned emitters=0;bool car=false,ground=false,door=false;
    for(const auto& pose:game.renderPoses()) {
        emitters+=pose.rayVisibilityMask==4;
        if(near(pose.position.x,2)&&near(pose.position.y,1)&&near(pose.scale.z,4))car=pose.rayVisibilityMask==2;
        if(near(pose.scale.x,20)&&near(pose.scale.z,20))ground=pose.rayVisibilityMask==1;
        if(near(pose.position.z,-8)&&near(pose.scale.x,2.4f))door=pose.rayVisibilityMask==2;
    }
    assert(car&&ground&&door&&emitters==5);
}
void testFixedStepAndSpeed() {
    Game sixty;
    Game oneTwenty;
    sixty.initialize(courtyard());
    oneTwenty.initialize(courtyard());
    Input forward;
    forward.forward = 1;
    advance(sixty, forward, 60);
    advance(oneTwenty, forward, 120, 1.0f / 120.0f);
    assert(near(sixty.playerPosition().z, 2.0f));
    assert(near(sixty.playerPosition().z, oneTwenty.playerPosition().z));
    Game diagonal;
    diagonal.initialize(courtyard());
    forward.right = 1;
    advance(diagonal, forward, 60);
    Vec3 position = diagonal.playerPosition();
    assert(near(std::hypot(position.x, position.z - 5), 3.0f));
    Game running;
    running.initialize(courtyard());
    forward.right = 0;
    forward.run = true;
    advance(running, forward, 60);
    assert(near(running.playerPosition().z, -1.0f));
}

void testJumpAndInputEdges() {
    Game game;
    game.initialize(courtyard());
    Input heldJump;
    heldJump.jump = true;
    game.update(heldJump, 1.0f / 60.0f);
    assert(!game.grounded());
    assert(game.playerPosition().y > 0.0f);
    float peak = game.playerPosition().y;
    for (int i = 0; i < 120; ++i) {
        game.update(heldJump, 1.0f / 60.0f);
        peak = std::max(peak, game.playerPosition().y);
        assert(game.playerPosition().y >= 0.0f);
    }
    assert(peak > 1.15f && peak < 1.30f);
    assert(game.grounded());
    assert(near(game.playerPosition().y, 0.0f));
    game.update({}, 1.0f / 60.0f);
    // Edge events survive a frame shorter than one physics tick.
    game.update(heldJump, 0.001f);
    game.update({}, 1.0f / 60.0f);
    assert(game.playerPosition().y > 0.0f);
}

void testSweptCollisionsAndVerticalContacts() {
    Scene scene = courtyard();
    scene.walkSpeed = 20;
    scene.runSpeed = 30;
    scene.entities.push_back(
        object("thin-wall", "wall", {0, 1, 4.11f}, {4, 2, 0.01f}, true));
    Game game;
    game.initialize(scene);
    Input forward;
    forward.forward = 1;
    forward.run = true;
    advance(game, forward, 10, 0.1f);
    assert(near(game.playerPosition().z, 4.415f));
    // A blocked axis still permits sliding along a wall.
    forward.right = 1;
    advance(game, forward, 2, 0.1f);
    assert(game.playerPosition().x > 1);

    Scene platform;
    platform.entities = {
        object("player", "player", {0, 4, 0}, {.6f, 1.8f, .6f}),
        object("platform", "prop", {0, 0.5f, 0}, {3, 1, 3}, true)
    };
    game.initialize(platform);
    advance(game, {}, 120);
    assert(game.grounded());
    assert(near(game.playerPosition().y, 1));

    Scene ceiling;
    ceiling.entities = {
        object("player", "player", {0, 0, 0}, {.6f, 1.8f, .6f}),
        object("ceiling", "prop", {0, 2.3f, 0}, {3, 0.2f, 3}, true)
    };
    game.initialize(ceiling);
    Input jump;
    jump.jump = true;
    float maxY = 0;
    for (int i = 0; i < 120; ++i) {
        game.update(jump, 1.0f / 120.0f);
        maxY = std::max(maxY, game.playerPosition().y);
    }
    assert(maxY <= 0.4001f);
    assert(game.grounded());
}

void testKeyDoorCompletionAndReset() {
    Scene scene = courtyard();
    const Vec3 sourceDoor = scene.entities[3].position;
    Game game;
    game.initialize(scene);
    const std::size_t initialPoseCount = game.renderPoses().size();
    Input forward;
    forward.forward = 1;
    forward.run = true;
    advance(game, forward, 150);
    assert(game.playerPosition().z > -7.58f);
    pressE(game);
    assert(!game.doorOpen());
    assert(!game.complete());
    assert(!game.hasKey());
    game.reset();

    Input right;
    right.right = 1;
    advance(game, right, 80); // (4, 0, 5)
    forward.run = false;
    advance(game, forward, 80); // (4, 0, 1)
    pressE(game);
    assert(game.hasKey());
    assert(!game.doorOpen());
    // The scene key disappears and a small carried key appears.
    assert(game.renderPoses().size() == initialPoseCount);
    right.right = -1;
    advance(game, right, 80); // (0, 0, 1)
    advance(game, forward, 145); // (0, 0, -6.25), interaction range
    pressE(game);
    assert(game.doorOpen());
    advance(game, forward, 55);
    assert(game.complete());
    assert(game.playerPosition().z < -8.5f);
    assert(near(scene.entities[3].position.x, sourceDoor.x));
    assert(near(scene.entities[3].position.z, sourceDoor.z));
    game.reset();
    assert(!game.hasKey() && !game.doorOpen() && !game.complete());
    assert(near(game.playerPosition().x, 0));
    assert(near(game.playerPosition().z, 5));
    assert(game.renderPoses().size() == initialPoseCount);
}

void testBoundariesAndFrameClamp() {
    Scene empty;
    Game game;
    game.initialize(empty);
    Input right;
    right.right = 1;
    game.update(right, 10.0f);
    assert(near(game.playerPosition().x, 0.3f));
    advance(game, right, 500);
    assert(near(game.playerPosition().x, 9.7f));
    game.update(right, std::numeric_limits<float>::quiet_NaN());
    assert(std::isfinite(game.playerPosition().x));
    game.reset();
    right = {};
    right.yawDelta = 3.14159265358979323846f * 0.5f;
    game.update(right, 0);
    right = {};
    right.forward = 1;
    advance(game, right, 60);
    assert(near(game.playerPosition().x, 3));
    assert(near(game.playerPosition().z, 5));
}

Vec3 transformPoint(const engine::Matrix& matrix, Vec3 point) {
    const float w = matrix.m[3] * point.x + matrix.m[7] * point.y +
                    matrix.m[11] * point.z + matrix.m[15];
    return {(matrix.m[0] * point.x + matrix.m[4] * point.y +
             matrix.m[8] * point.z + matrix.m[12]) / w,
            (matrix.m[1] * point.x + matrix.m[5] * point.y +
             matrix.m[9] * point.z + matrix.m[13]) / w,
            (matrix.m[2] * point.x + matrix.m[6] * point.y +
             matrix.m[10] * point.z + matrix.m[14]) / w};
}

void testMatrices() {
    const engine::Matrix projection = engine::perspectiveLH(1, 1.5f, 0.1f, 100);
    assert(near(transformPoint(projection, {0, 0, 0.1f}).z, 0));
    assert(near(transformPoint(projection, {0, 0, 100}).z, 1));
    const engine::Matrix view = engine::lookAtLH({0, 2, 5}, {0, 2, 0});
    const Vec3 eye = transformPoint(view, {0, 2, 5});
    assert(near(eye.x, 0) && near(eye.y, 0) && near(eye.z, 0));
    assert(near(transformPoint(view, {0, 2, 0}).z, 5));
    const engine::Matrix model =
        engine::compose({2, 3, 4}, {2, 2, 2}, 3.14159265358979323846f * 0.5f);
    const Vec3 facing = transformPoint(model, {0, 0, -1});
    assert(near(facing.x, 4) && near(facing.y, 3) && near(facing.z, 4));
    const engine::Matrix combined = engine::multiply(view, model);
    const Vec3 direct = transformPoint(combined, {0, 0, -1});
    const Vec3 sequential = transformPoint(view, facing);
    assert(near(direct.x, sequential.x));
    assert(near(direct.y, sequential.y));
    assert(near(direct.z, sequential.z));
    const engine::Matrix verticalView = engine::lookAtLH({0, 5, 0}, {0, 0, 0});
    assert(near(transformPoint(verticalView, {0, 0, 0}).z, 5));
}

// ------------------------------------------------------------------ determinism

void testHasherGrid() {
    using engine::determinism::Hasher;
    Hasher a, b;
    for (int index = 0; index < 16; ++index) {
        a.quantized(0.1f * static_cast<float>(index));
        b.quantized(0.1f * static_cast<float>(index));
    }
    assert(a.value() == b.value());
    Hasher different;
    different.quantized(0.1f);
    assert(different.value() != a.value());

    // Quantization collapses float noise to the same integer, which is what makes the
    // digest survive a different libm or optimization level.
    Hasher q1, q2, q3;
    q1.quantized(1.0f);
    q2.quantized(1.0f + 0.4e-4f);
    q3.quantized(1.0f + 3.0e-4f);
    assert(q1.value() == q2.value());
    assert(q3.value() != q1.value());

    // Non-finite input hashes to a sentinel instead of a conversion result that differs
    // between compilers.
    Hasher nan, inf;
    nan.quantized(std::numeric_limits<float>::quiet_NaN());
    inf.quantized(std::numeric_limits<float>::infinity());
    assert(nan.value() == inf.value());
    assert(nan.value() != q1.value());
}

void testStreams() {
    using namespace engine::determinism;
    Stream a("gameplay", 42), b("gameplay", 42), c("gameplay", 43);
    bool reproduced = true, diverged = false;
    for (int index = 0; index < 256; ++index) {
        const auto va = a.next(), vb = b.next(), vc = c.next();
        if (va != vb) reproduced = false;
        if (va != vc) diverged = true;
    }
    assert(reproduced);
    assert(diverged);
    assert(a.counter() == 256);

    // Drawing from one stream must never shift another stream's sequence.
    Registry& registry = engine::determinism::registry();
    registry.reset(7);
    Stream& gameplay = registry.stream("gameplay");
    Stream& cosmetic = registry.stream("cosmetic");
    const auto cosmeticFirst = cosmetic.next();
    for (int index = 0; index < 1000; ++index) gameplay.next();
    const auto cosmeticSecond = cosmetic.next();
    Stream fresh("cosmetic", deriveSeed(7, "cosmetic"));
    assert(fresh.next() == cosmeticFirst);
    assert(fresh.next() == cosmeticSecond);

    // The fingerprint covers the seed and every counter, so a consumed number is visible.
    const auto before = registry.fingerprint();
    gameplay.next();
    assert(registry.fingerprint() != before);

    Stream unit("unit", 1);
    for (int index = 0; index < 1000; ++index) {
        const float value = unit.unit();
        assert(value >= 0.0f && value < 1.0f);
        assert(unit.range(7) < 7);
    }
    assert(unit.range(0) == 0);
}

void testGameDigest() {
    Scene scene;
    scene.entities.push_back(object("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(object("player", "player", {0, 0, 5}, {0.8f, 1.8f, 0.5f}));
    Game first, second;
    first.initialize(scene);
    second.initialize(scene);
    Input walk;
    walk.forward = 1.0f;
    walk.right = 0.25f;
    for (int index = 0; index < 120; ++index) {
        first.update(walk, 1.0f / 60.0f);
        second.update(walk, 1.0f / 60.0f);
    }
    engine::determinism::Hasher h1, h2;
    first.digest(h1);
    second.digest(h2);
    assert(first.ticks() == second.ticks());
    assert(first.ticks() > 0);
    assert(h1.value() == h2.value());

    // A different input sequence must produce a different digest, otherwise the digest
    // is not actually covering the simulated state.
    Game idleGame;
    idleGame.initialize(scene);
    Input idle;
    for (int index = 0; index < 120; ++index) idleGame.update(idle, 1.0f / 60.0f);
    engine::determinism::Hasher h3;
    idleGame.digest(h3);
    assert(h3.value() != h1.value());

    // Reset returns the simulation to tick 0 so a replay can start from its own origin.
    first.reset();
    assert(first.ticks() == 0);
}

// ------------------------------------------------------------------ layout contracts

void testGeometryRouting() {
    using engine::GeometryClass;
    using engine::GeometryHints;
    using engine::classifyGeometry;

    GeometryHints cube;
    cube.triangles = 12;
    assert(classifyGeometry(cube) == GeometryClass::Simple);

    GeometryHints dense;
    dense.triangles = engine::kClusterTriangleThreshold;
    assert(classifyGeometry(dense) == GeometryClass::Cluster);

    GeometryHints justUnder;
    justUnder.triangles = engine::kClusterTriangleThreshold - 1;
    assert(classifyGeometry(justUnder) == GeometryClass::Simple);

    // Translucency wins over density: the cluster pipeline has no ordering.
    GeometryHints glass;
    glass.triangles = 200000;
    glass.translucent = true;
    assert(classifyGeometry(glass) == GeometryClass::Transparent);

    // Deforming meshes stay on the vertex path until cluster skinning is proven.
    GeometryHints skinned;
    skinned.triangles = 200000;
    skinned.animated = true;
    assert(classifyGeometry(skinned) == GeometryClass::Deformable);

    GeometryHints both;
    both.triangles = 200000;
    both.animated = true;
    both.translucent = true;
    assert(classifyGeometry(both) == GeometryClass::Transparent);

    assert(std::string(engine::geometryClassName(GeometryClass::Cluster)) == "cluster");
    assert(std::string(engine::geometryClassName(GeometryClass::Transparent)) == "transparent");
}

void testMaterialHandles() {
    using engine::decodeMaterialHandle;
    using engine::encodeMaterialHandle;

    std::uint32_t handle = 0xFFFFFFFFu;
    assert(encodeMaterialHandle(3, 7, handle));
    assert(handle == ((7u << 16) | 3u));
    const auto parts = decodeMaterialHandle(handle);
    assert(parts.slice == 3 && parts.generation == 7);

    // The default handle must decode to slice 0 / generation 0, not to a sentinel.
    const auto defaultParts = decodeMaterialHandle(engine::kMaterialHandleDefault);
    assert(defaultParts.slice == 0 && defaultParts.generation == 0);

    // A handle that does not fit must be rejected instead of silently truncated.
    std::uint32_t rejected = 1234u;
    assert(!encodeMaterialHandle(0x10000u, 1, rejected));
    assert(rejected == engine::kMaterialHandleDefault);
    rejected = 1234u;
    assert(!encodeMaterialHandle(1, 0x10000u, rejected));
    assert(rejected == engine::kMaterialHandleDefault);

    // Extremes round-trip exactly.
    assert(encodeMaterialHandle(0xFFFFu, 0xFFFFu, handle));
    assert(handle == 0xFFFFFFFFu);
    const auto extremes = decodeMaterialHandle(handle);
    assert(extremes.slice == 0xFFFFu && extremes.generation == 0xFFFFu);
}
} // namespace

int main() {
    testRenderVisibilityMasks();
    testFixedStepAndSpeed();
    testJumpAndInputEdges();
    testSweptCollisionsAndVerticalContacts();
    testKeyDoorCompletionAndReset();
    testBoundariesAndFrameClamp();
    testMatrices();
    testHasherGrid();
    testStreams();
    testGameDigest();
    testGeometryRouting();
    testMaterialHandles();
    std::cout << "Native simulation: fixed-step movement, jumping, swept contacts, "
                 "key/gate/exit, reset, bounds, matrices, deterministic hashing and "
                 "RNG streams, geometry routing and material handles passed.\n";
}

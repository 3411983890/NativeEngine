#include "../engine/native/game.hpp"
#include "../engine/native/physics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using engine::Entity;
using engine::Game;
using engine::Input;
using engine::Scene;
using engine::Vec3;

namespace {
Entity box(const char* id, const char* kind, Vec3 position, Vec3 scale,
           bool collider = true) {
    Entity entity;
    entity.id = id;
    entity.name = id;
    entity.kind = kind;
    entity.position = position;
    entity.scale = scale;
    entity.collider = collider;
    entity.color[0] = 0.55f;
    entity.color[1] = 0.52f;
    entity.color[2] = 0.47f;
    return entity;
}

Entity destructible(const char* id, Vec3 position, Vec3 scale, float strength,
                    int fragments, unsigned seed) {
    Entity entity = box(id, "prop", position, scale);
    entity.destructible = true;
    entity.strength = strength;
    entity.fragments = fragments;
    entity.structural = true;
    entity.fractureSeed = seed;
    entity.hasPhysics = true;
    entity.dynamicBody = false;
    entity.physicsMass = 10.0f;
    return entity;
}

Scene towerScene() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 0, 8}, {0.6f, 1.8f, 0.6f}));
    scene.entities.push_back(destructible("base", {0, 0.5f, 0}, {1.6f, 1.0f, 1.6f},
                                         32.0f, 8, 11));
    scene.entities.push_back(destructible("middle", {0, 1.5f, 0}, {1.6f, 1.0f, 1.6f},
                                         30.0f, 8, 12));
    scene.entities.push_back(destructible("top", {0, 2.5f, 0}, {1.6f, 1.0f, 1.6f},
                                         28.0f, 8, 13));
    Entity crate = box("crate", "prop", {4, 0.55f, 2}, {1.1f, 1.1f, 1.1f});
    crate.hasPhysics = true;
    crate.dynamicBody = true;
    crate.physicsMass = 4.0f;
    scene.entities.push_back(crate);
    return scene;
}

void advance(Game& game, int frames) {
    for (int i = 0; i < frames; ++i) game.update({}, 1.0f / 60.0f);
}

void testProjectileFractureAndCollapse() {
    Game game;
    game.initialize(towerScene());
    assert(game.destructibleCount() == 3);
    assert(game.intactDestructibleCount() == 3);
    assert(game.physicsStats().activeBodies >= 4);
    assert(game.physicsStats().dynamicBodies >= 1);

    game.fireProjectileAt({0.0f, 0.5f, 0.0f}, 34.0f);
    assert(game.projectileCount() == 1);
    advance(game, 300);

    assert(game.fractureCount() >= 1);
    assert(game.fragmentsSpawned() >= 8);
    // Fracturing the base removes support from the two courses above it.
    assert(game.structuralCollapses() >= 2);
    assert(game.intactDestructibleCount() == 0);
    assert(game.physicsStats().debris > 0);
    assert(game.physicsStats().activeBodies >= 20);
    // The destroyed tower still occupies the same scene document; rendering comes
    // entirely from the replacement debris bodies.
    assert(game.renderPoses().size() >= 20);
}

void testExplosionAndReset() {
    Game game;
    game.initialize(towerScene());
    game.detonate({0.0f, 0.6f, 0.0f}, 6.0f, 10.0f);
    assert(game.explosionCount() == 1);
    advance(game, 240);
    assert(game.fractureCount() == 3);
    assert(game.intactDestructibleCount() == 0);
    assert(game.fragmentsSpawned() >= 24);

    game.reset();
    assert(game.fractureCount() == 0);
    assert(game.fragmentsSpawned() == 0);
    assert(game.explosionCount() == 0);
    assert(game.intactDestructibleCount() == 3);
    assert(game.physicsStats().dynamicBodies >= 1);
}

void testDeterministicDestruction() {
    Game first;
    Game second;
    first.initialize(towerScene());
    second.initialize(towerScene());
    for (Game* game : {&first, &second}) {
        game->fireProjectileAt({0.0f, 0.5f, 0.0f}, 34.0f);
        advance(*game, 240);
        game->detonate({0.0f, 1.0f, 0.0f}, 5.0f, 8.0f);
        advance(*game, 120);
    }
    engine::determinism::Hasher a;
    engine::determinism::Hasher b;
    first.digest(a);
    second.digest(b);
    assert(a.value() == b.value());
    assert(first.fractureCount() == second.fractureCount());
    assert(first.fragmentsSpawned() == second.fragmentsSpawned());
    assert(first.physicsStats().activeBodies == second.physicsStats().activeBodies);
}

void testCharacterOnDynamicBody() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 3.0f, 0}, {0.6f, 1.8f, 0.6f}));
    Entity crate = box("crate", "prop", {0, 0.55f, 0}, {1.1f, 1.1f, 1.1f});
    crate.hasPhysics = true;
    crate.dynamicBody = true;
    crate.physicsMass = 4.0f;
    scene.entities.push_back(crate);
    Game game;
    game.initialize(scene);
    advance(game, 240);
    // The crate is a dynamic body at y=0.55 with top 1.10. The character must use the
    // body's current top, not the authored scene box or a stale AABB.
    assert(game.grounded());
    assert(std::abs(game.playerPosition().y - 1.1f) < 0.16f);

    // Blast the crate sideways. Once the support leaves, the character must fall to
    // the ground instead of standing on an invisible authored collider.
    game.detonate({1.5f, 0.55f, 0.0f}, 3.5f, 20.0f);
    advance(game, 240);
    assert(game.playerPosition().y < 0.08f);
    assert(game.grounded());
}

void testCharacterStepUp() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 0, 3}, {0.6f, 1.8f, 0.6f}));
    scene.entities.push_back(box("step", "prop", {0, 0.15f, 0}, {2.0f, 0.3f, 2.0f}));
    Game game;
    game.initialize(scene);
    Input forward;
    forward.forward = 1.0f;
    for (int i = 0; i < 40; ++i) game.update(forward, 1.0f / 60.0f);
    assert(game.playerPosition().y > 0.28f);
    assert(game.grounded());
    assert(game.playerPosition().z < 1.4f);
}

void testFracturedColliderRemoved() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 0, 3}, {0.6f, 1.8f, 0.6f}));
    Entity wall = destructible("wall", {0, 0.5f, -2}, {1.6f, 1.0f, 1.6f},
                               20.0f, 4, 77);
    wall.debrisLifetime = 1.0f;
    scene.entities.push_back(wall);
    Game game;
    game.initialize(scene);
    game.detonate({0.0f, 0.5f, -2.0f}, 3.0f, 8.0f);
    assert(game.fractureCount() == 1);
    // Let the short-lived debris disappear, then walk through the old wall volume.
    advance(game, 240);
    Input forward;
    forward.forward = 1.0f;
    for (int i = 0; i < 120; ++i) game.update(forward, 1.0f / 60.0f);
    assert(game.playerPosition().z < -2.5f);
}

void testDynamicSettling() {
    Game game;
    game.initialize(towerScene());
    advance(game, 600);
    const engine::physics::Stats stats = game.physicsStats();
    assert(stats.dynamicBodies >= 1);
    assert(stats.sleepingBodies >= 1);
    // No destructible may lose structural support without an impact.
    assert(game.fractureCount() == 0);
}

Scene bondedWallScene() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 0, 8}, {0.6f, 1.8f, 0.6f}));
    Entity wall = box("bonded-wall", "wall", {0, 1.5f, -4}, {4.0f, 3.0f, 0.3f});
    wall.destructible = true;
    wall.bonded = true;
    wall.bondedCells = 48;
    wall.strength = 40.0f;
    wall.fragments = 12;
    wall.hasPhysics = true;
    wall.dynamicBody = false;
    wall.physicsMass = 60.0f;
    scene.entities.push_back(wall);
    return scene;
}

void testBondedHoleKeepsWallStanding() {
    Game game;
    game.initialize(bondedWallScene());
    const std::size_t posesBefore = game.renderPoses().size();
    assert(game.destructibleCount() == 1);
    assert(game.bondedBrokenBonds() == 0);

    game.fireProjectileAt({0.0f, 1.5f, -4.0f}, 36.0f);
    advance(game, 180);

    // A local hole, not a whole-object fracture.
    assert(game.bondedBrokenBonds() > 0);
    assert(game.bondedHoles() >= 1);
    assert(game.fractureCount() == 0);
    assert(game.intactDestructibleCount() == 1);

    // The single wall body has been replaced by per-cell bodies and per-cell poses.
    const std::size_t posesAfter = game.renderPoses().size();
    assert(posesAfter > posesBefore + 10);
    assert(game.physicsStats().activeBodies > 20);

    // A second hit next to the hole widens it without fracturing the object. Firing
    // at the hole itself must do nothing: the projectile flies through the opening.
    const unsigned brokenAfterFirst = game.bondedBrokenBonds();
    game.fireProjectileAt({0.0f, 1.5f, -4.0f}, 36.0f);
    advance(game, 180);
    assert(game.bondedBrokenBonds() == brokenAfterFirst);
    game.fireProjectileAt({0.9f, 1.5f, -4.0f}, 36.0f);
    advance(game, 180);
    assert(game.bondedBrokenBonds() > brokenAfterFirst);
    assert(game.fractureCount() == 0);
    assert(game.intactDestructibleCount() == 1);
}

// A bonded structure is part of the simulation state, so a save/load must restore the
// exact damage, bond graph and per-cell bodies. Anything less turns a hole back into
// a solid wall on load, which is the failure mode this test exists to catch.
void testBondedSnapshotRoundTrip() {
    Game first;
    first.initialize(bondedWallScene());
    first.fireProjectileAt({0.0f, 1.5f, -4.0f}, 36.0f);
    advance(first, 180);
    assert(first.bondedHoles() >= 1);

    const auto state = first.snapshot();
    Game resumed;
    resumed.initialize(bondedWallScene());
    resumed.restore(state);
    assert(resumed.bondedHoles() == first.bondedHoles());
    assert(resumed.bondedBrokenBonds() == first.bondedBrokenBonds());
    assert(resumed.snapshot() == first.snapshot());

    // The restored hole must keep behaving like a hole: driving the same inputs on
    // both worlds keeps them locked together, and the standing wall stays standing.
    for (int i = 0; i < 240; ++i) {
        first.update({}, 1.0f / 120.0f);
        resumed.update({}, 1.0f / 120.0f);
        assert(first.snapshot() == resumed.snapshot());
    }
    assert(first.fractureCount() == 0);
    assert(resumed.fractureCount() == 0);
    assert(resumed.intactDestructibleCount() == 1);
}

// A baked Voronoi package must reach the renderer: the shards draw their hull (Pose::mesh
// set, scale 1) while the physics proxy stays the AABB. This is the contract F4b rests on,
// and it is easy to break silently by forgetting to forward the mesh index.
void testBakedShardsReachRenderPoses() {
    Scene scene = bondedWallScene();
    // Two shards side by side, each a unit box hull centred on its proxy.
    engine::VoronoiPackage package;
    package.source = "voronoi/test.voronoi.json";
    for (int shard = 0; shard < 2; ++shard) {
        engine::ConvexMesh mesh;
        const float cx = (shard == 0 ? -1.0f : 1.0f);
        const float corners[8][3] = {{-0.5f,-0.5f,-0.15f},{0.5f,-0.5f,-0.15f},{0.5f,0.5f,-0.15f},{-0.5f,0.5f,-0.15f},
                                     {-0.5f,-0.5f, 0.15f},{0.5f,-0.5f, 0.15f},{0.5f,0.5f, 0.15f},{-0.5f,0.5f, 0.15f}};
        for (const auto& corner : corners) {
            engine::ConvexVertex vertex;
            for (int axis = 0; axis < 3; ++axis) vertex.position[axis] = corner[axis];
            mesh.vertices.push_back(vertex);
        }
        const std::uint32_t faces[36] = {0,1,2, 0,2,3, 4,6,5, 4,7,6, 0,4,5, 0,5,1,
                                         1,5,6, 1,6,2, 2,6,7, 2,7,3, 3,7,4, 3,4,0};
        mesh.indices.assign(faces, faces + 36);
        mesh.min[0] = -0.5f; mesh.min[1] = -0.5f; mesh.min[2] = -0.15f;
        mesh.max[0] = 0.5f;  mesh.max[1] = 0.5f;  mesh.max[2] = 0.15f;
        mesh.baked = true;
        package.meshes.push_back(std::move(mesh));
        engine::VoronoiShardProxy proxy;
        proxy.center[0] = cx; proxy.center[1] = 0.0f; proxy.center[2] = 0.0f;
        proxy.half[0] = 0.5f; proxy.half[1] = 0.5f; proxy.half[2] = 0.15f;
        proxy.volume = 0.3f;
        proxy.mesh = shard;
        package.shards.push_back(proxy);
    }
    package.links.push_back({0, 1, 0.3f});
    package.volume = 0.6f;
    package.boxVolume = 0.6f;
    scene.voronoiPackages.push_back(std::move(package));
    for (Entity& entity : scene.entities) {
        if (entity.bonded) entity.voronoi = "voronoi/test.voronoi.json";
    }

    Game game;
    game.initialize(scene);
    // The scene has one bonded wall, so its two shards are the whole table.
    assert(game.convexMeshes().size() == 2);
    assert(game.bakedShardCount() == 2);
    assert(game.convexMeshes()[0].baked);
    assert(game.convexMeshes()[0].indices.size() == 36);

    // Nothing is drawn as a hull until the structure shatters, because the intact wall is
    // still the single authored body.
    for (const engine::Pose& pose : game.renderPoses()) assert(pose.mesh == -1);

    game.fireProjectileAt({0.0f, 1.5f, -4.0f}, 36.0f);
    advance(game, 180);
    assert(game.bondedHoles() >= 1);

    unsigned hullPoses = 0;
    for (const engine::Pose& pose : game.renderPoses()) {
        if (pose.mesh < 0) continue;
        ++hullPoses;
        assert(pose.mesh < static_cast<int>(game.convexMeshes().size()));
        // A hull pose must not be scaled: the mesh already carries the shard's size.
        assert(pose.scale.x == 1.0f && pose.scale.y == 1.0f && pose.scale.z == 1.0f);
    }
    assert(hullPoses > 0);
}

// Authored joints must reach the rigid world. This is the piece that turns the C++ joint
// API into something a scene can express, so a silent no-op here would look like "the
// door is just a box" rather than an error.
void testSceneJointsAreCreated() {
    Scene scene;
    scene.gravity = 20.0f;
    scene.entities.push_back(box("ground", "ground", {0, -0.5f, 0}, {20, 1, 20}));
    scene.entities.push_back(box("player", "player", {0, 0, 9}, {0.6f, 1.8f, 0.6f}));
    Entity post = box("post", "prop", {-1.0f, 1.7f, -4}, {0.3f, 3.4f, 0.3f});
    scene.entities.push_back(post);
    Entity door = box("door", "prop", {-2.4f, 1.6f, -4}, {2.6f, 3.0f, 0.2f});
    door.hasPhysics = true;
    door.dynamicBody = true;
    door.physicsMass = 26.0f;
    scene.entities.push_back(door);

    engine::SceneJoint hinge;
    hinge.type = "hinge";
    hinge.bodyA = "door";
    hinge.bodyB = "post";
    hinge.anchorA = {1.3f, 0.0f, 0.0f};
    hinge.anchorB = {-0.1f, -0.1f, 0.0f};
    hinge.axis = {0.0f, 1.0f, 0.0f};
    hinge.lowerLimit = -1.3f;
    hinge.upperLimit = 1.3f;
    scene.joints.push_back(hinge);

    Game game;
    game.initialize(scene);
    assert(game.physicsStats().joints == 1);
    assert(game.physicsStats().brokenJoints == 0);

    // A joint the scene names but no entity backs must be skipped, not crash or wedge.
    Scene broken = scene;
    broken.joints[0].bodyA = "does-not-exist";
    Game skipped;
    skipped.initialize(broken);
    assert(skipped.physicsStats().joints == 0);

    // Joint state survives a save/load: without this a restore drops every hinge.
    const auto state = game.snapshot();
    Game resumed;
    resumed.initialize(scene);
    resumed.restore(state);
    assert(resumed.physicsStats().joints == 1);
    for (int tick = 0; tick < 60; ++tick) {
        game.update({}, 1.0f / 120.0f);
        resumed.update({}, 1.0f / 120.0f);
        assert(game.snapshot() == resumed.snapshot());
    }
}

} // namespace

int main() {
    testProjectileFractureAndCollapse();
    testExplosionAndReset();
    testDeterministicDestruction();
    testCharacterOnDynamicBody();
    testCharacterStepUp();
    testFracturedColliderRemoved();
    testDynamicSettling();
    testBondedHoleKeepsWallStanding();
    testBondedSnapshotRoundTrip();
    testBakedShardsReachRenderPoses();
    testSceneJointsAreCreated();
    std::cout << "Native destruction: projectile impact, fracture debris, structural "
                 "collapse, blast damage, reset, deterministic replay, dynamic-body "
                 "sleep, character-on-body support, step-up, fractured-collider "
                 "removal, bonded holes, bonded snapshot restore, baked shard hulls and "
                 "authored joints passed.\n";
    return 0;
}

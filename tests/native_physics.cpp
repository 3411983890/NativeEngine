#include "../engine/native/physics.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace engine;
using namespace engine::physics;

namespace {

physics::BodyDesc box(Vec3 position, Vec3 half, float mass = 1.0f) {
    physics::BodyDesc desc;
    desc.position = position;
    desc.halfExtents = half;
    desc.mass = mass;
    desc.dynamic = true;
    desc.friction = 0.7f;
    desc.restitution = 0.03f;
    return desc;
}

unsigned ground(physics::World& world) {
    physics::BodyDesc desc;
    desc.position = {0.0f, -0.5f, 0.0f};
    desc.halfExtents = {12.0f, 0.5f, 12.0f};
    desc.dynamic = false;
    desc.friction = 0.85f;
    desc.restitution = 0.02f;
    return world.addBody(desc);
}

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

void testQuaternions() {
    const Quat q = fromEuler(0.7f, -0.3f, 1.1f);
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    toEuler(q, yaw, pitch, roll);
    assert(near(yaw, 0.7f, 1.0e-4f));
    assert(near(pitch, -0.3f, 1.0e-4f));
    assert(near(roll, 1.1f, 1.0e-4f));
    const Vec3 rotated = rotate(fromAxisAngle({0, 1, 0}, 1.5707963f), {1, 0, 0});
    assert(near(rotated.x, 0.0f, 1.0e-4f));
    assert(near(rotated.z, -1.0f, 1.0e-4f));
}

void testRestOnGround() {
    physics::World world;
    ground(world);
    const unsigned index = world.addBody(box({0, 2, 0}, {0.5f, 0.5f, 0.5f}, 2.0f));
    bool sawContact = false;
    for (int i = 0; i < 360; ++i) {
        world.step(1.0f / 120.0f);
        if (world.lastContactCount() > 0) sawContact = true;
    }
    const Body* body = world.body(index);
    assert(body);
    assert(near(body->position.y, 0.5f, 0.035f));
    assert(near(body->velocity.y, 0.0f, 0.08f));
    assert(body->sleeping);
    assert(sawContact);
    const auto stats = world.stats();
    assert(stats.sleepingBodies >= 1);
}

void testStack() {
    physics::World world;
    ground(world);
    const unsigned a = world.addBody(box({0, 0.5f, 0}, {0.5f, 0.5f, 0.5f}, 2.0f));
    const unsigned b = world.addBody(box({0, 1.5f, 0}, {0.5f, 0.5f, 0.5f}, 2.0f));
    const unsigned c = world.addBody(box({0, 2.5f, 0}, {0.5f, 0.5f, 0.5f}, 2.0f));
    for (int i = 0; i < 720; ++i) world.step(1.0f / 120.0f);
    assert(near(world.body(a)->position.y, 0.5f, 0.05f));
    assert(near(world.body(b)->position.y, 1.5f, 0.06f));
    assert(near(world.body(c)->position.y, 2.5f, 0.07f));
    assert(std::abs(world.body(c)->position.x) < 0.05f);
    assert(std::abs(world.body(c)->position.z) < 0.05f);
}

void testFrictionAndRestitution() {
    physics::World world;
    ground(world);
    physics::BodyDesc sliding = box({-5, 0.5f, 0}, {0.5f, 0.5f, 0.5f}, 1.0f);
    sliding.velocity = {6, 0, 0};
    const unsigned index = world.addBody(sliding);
    for (int i = 0; i < 360; ++i) world.step(1.0f / 120.0f);
    assert(world.body(index)->velocity.x < 0.1f);
    assert(world.body(index)->position.x < -2.0f);

    physics::World bouncing;
    ground(bouncing);
    physics::BodyDesc ball = box({0, 1.2f, 0}, {0.4f, 0.4f, 0.4f}, 1.0f);
    ball.restitution = 0.75f;
    const unsigned bouncer = bouncing.addBody(ball);
    float peak = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 240; ++i) {
        bouncing.step(1.0f / 120.0f);
        const float y = bouncing.body(bouncer)->position.y;
        if (i > 20 && bouncing.body(bouncer)->velocity.y > 0.5f) bounced = true;
        if (i > 20) peak = std::max(peak, y);
    }
    assert(bounced);
    assert(peak > 0.8f);
}

void testDeterminism() {
    physics::World a, b;
    for (physics::World* world : {&a, &b}) {
        ground(*world);
        for (int i = 0; i < 5; ++i) {
            world->addBody(box({0.02f * i, 0.5f + 1.01f * i, -0.01f * i},
                               {0.45f, 0.45f, 0.45f}, 1.4f));
        }
        world->applyRadialImpulse({0, 1.5f, 0}, 4.0f, 3.0f);
    }
    for (int i = 0; i < 480; ++i) {
        a.step(1.0f / 120.0f);
        b.step(1.0f / 120.0f);
    }
    for (std::size_t i = 0; i < a.bodies().size(); ++i) {
        const Body& left = a.bodies()[i];
        const Body& right = b.bodies()[i];
        if (!left.active) continue;
        assert(left.position.x == right.position.x);
        assert(left.position.y == right.position.y);
        assert(left.position.z == right.position.z);
        assert(left.orientation.w == right.orientation.w);
    }
}

void testTunnellingGuard() {
    physics::World world;
    world.setGravity({0, 0, 0});
    physics::BodyDesc wall;
    wall.position = {0, 1, 0};
    wall.halfExtents = {0.10f, 2.0f, 2.0f};
    wall.dynamic = false;
    world.addBody(wall);
    physics::BodyDesc bullet = box({-4, 1, 0}, {0.15f, 0.15f, 0.15f}, 1.0f);
    bullet.velocity = {40, 0, 0};
    bullet.restitution = 0.0f;
    const unsigned index = world.addBody(bullet);
    for (int i = 0; i < 240; ++i) world.step(1.0f / 120.0f);
    assert(world.body(index)->position.x < -0.15f);
    assert(std::isfinite(world.body(index)->position.x));
}

void testExplosionAndLifetime() {
    physics::World world;
    world.setGravity({0, 0, 0});
    physics::BodyDesc a = box({-1, 0, 0}, {0.2f, 0.2f, 0.2f}, 1.0f);
    a.lifetime = 0.05f;
    a.debris = true;
    a.visible = true;
    const unsigned index = world.addBody(a);
    world.addBody(box({1, 0, 0}, {0.2f, 0.2f, 0.2f}, 1.0f));
    world.applyRadialImpulse({0, 0, 0}, 5.0f, 6.0f);
    assert(world.body(0)->velocity.x < -1.0f);
    assert(world.body(1)->velocity.x > 1.0f);
    for (int i = 0; i < 12; ++i) world.step(1.0f / 120.0f);
    assert(!world.body(index)->active);
    const auto stats = world.stats();
    assert(stats.debris == 0);  // inactive debris is not counted
    assert(stats.projectiles == 0);
}

// F4c: the same shard geometry, simulated twice. With the box proxy the body has a wide
// flat base and never tips; with the hull it rests on a single vertex and has to fall onto
// a face. This is the behaviour the whole convex narrowphase exists for, so the test
// asserts the two outcomes actually differ rather than just "no crash".
void testHullCollidesAsItsShape() {
    // Tetrahedron whose only lowest point is one vertex, inside a 1 x 1 x 0.8 AABB.
    static const Vec3 points[4]{{-0.5f, 0.5f, 0.0f},
                                {0.5f, 0.5f, 0.0f},
                                {0.0f, -0.5f, 0.4f},
                                {0.0f, 0.2f, -0.4f}};
    static const std::uint32_t indices[12]{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};

    auto settle = [](bool useHull, float& tilt, float& height) {
        World world;
        world.setGravity({0.0f, -20.0f, 0.0f});
        BodyDesc ground;
        ground.position = {0.0f, -0.5f, 0.0f};
        ground.halfExtents = {6.0f, 0.5f, 6.0f};
        ground.mass = 0.0f;
        ground.dynamic = false;
        world.addBody(ground);

        BodyDesc shard = box({0.0f, 0.62f, 0.0f}, {0.5f, 0.5f, 0.4f}, 3.0f);
        if (useHull) {
            shard.hullPoints = points;
            shard.hullPointCount = 4;
            shard.hullIndices = indices;
            shard.hullIndexCount = 12;
        }
        const unsigned id = world.addBody(shard);
        for (int tick = 0; tick < 360; ++tick) world.step(1.0f / 120.0f);
        const Body* body = world.body(id);
        const Vec3 up = rotate(body->orientation, Vec3{0.0f, 1.0f, 0.0f});
        tilt = std::acos(std::clamp(up.y, -1.0f, 1.0f)) * 57.29578f;
        height = body->position.y;
    };

    float boxTilt = 0.0f, boxHeight = 0.0f, hullTilt = 0.0f, hullHeight = 0.0f;
    settle(false, boxTilt, boxHeight);
    settle(true, hullTilt, hullHeight);
    // The box proxy stands on its flat base.
    assert(boxTilt < 8.0f);
    // The hull cannot: a tetrahedron balanced on one vertex falls onto a face.
    assert(hullTilt > 15.0f);
    // And it ends up lower, because a face rests flatter than a corner.
    assert(hullHeight < boxHeight);

    // Same input twice must produce the same result, since the contact stream feeds replay.
    float repeatTilt = 0.0f, repeatHeight = 0.0f;
    settle(true, repeatTilt, repeatHeight);
    assert(repeatTilt == hullTilt && repeatHeight == hullHeight);
}

} // namespace

int main() {
    testQuaternions();
    testRestOnGround();
    testStack();
    testFrictionAndRestitution();
    testDeterminism();
    testTunnellingGuard();
    testExplosionAndLifetime();
    testHullCollidesAsItsShape();
    std::cout << "Native physics: quaternion round-trip, resting contact, stack stability, "
                 "friction, restitution, deterministic replay, tunnelling guard, radial "
                 "impulse, debris lifetime and hull-shaped collision passed.\n";
    return 0;
}

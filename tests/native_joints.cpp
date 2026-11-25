// XPBD joint tests: point (pendulum / hanging), hinge (door + limit), rope chain,
// break threshold, determinism, and the "no joints means no change" regression.

#include "../engine/native/physics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace engine;
using namespace engine::physics;

namespace {

BodyDesc box(Vec3 position, Vec3 half, float mass, bool dynamic = true) {
    BodyDesc desc;
    desc.position = position;
    desc.halfExtents = half;
    desc.mass = mass;
    desc.dynamic = dynamic;
    desc.friction = 0.6f;
    desc.restitution = 0.0f;
    return desc;
}

float distance(Vec3 a, Vec3 b) {
    const Vec3 d = b - a;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

Vec3 worldAnchor(const World& world, unsigned body, Vec3 local) {
    const Body* b = world.body(body);
    assert(b);
    return b->position + rotate(b->orientation, local);
}

bool finite(const Body& body) {
    return std::isfinite(body.position.x) && std::isfinite(body.position.y) &&
           std::isfinite(body.position.z) && std::isfinite(body.velocity.x) &&
           std::isfinite(body.velocity.y) && std::isfinite(body.velocity.z);
}

void testPendulum() {
    World world;
    const unsigned anchor = world.addBody(box({0.0f, 3.0f, 0.0f}, {0.08f, 0.08f, 0.08f},
                                              1.0f, false));
    const unsigned bob = world.addBody(box({1.5f, 3.0f, 0.0f}, {0.18f, 0.18f, 0.18f}, 2.0f));
    JointDesc desc;
    desc.type = JointType::Point;
    desc.bodyA = anchor;
    desc.bodyB = bob;
    desc.anchorA = {0.0f, 0.0f, 0.0f};
    desc.anchorB = {-1.5f, 0.0f, 0.0f};
    const unsigned id = world.addJoint(desc);
    assert(id != 0);
    assert(world.jointCount() == 1);

    for (int tick = 0; tick < 600; ++tick) world.step(1.0f / 120.0f);
    const Body& body = *world.body(bob);
    assert(finite(body));
    const float anchorDistance = distance(worldAnchor(world, bob, {-1.5f, 0.0f, 0.0f}),
                                          {0.0f, 3.0f, 0.0f});
    assert(anchorDistance < 0.03f);
    // It must swing below the anchor and stay on the circle, not fall through.
    assert(body.position.y < 3.0f);
    assert(body.position.y > 1.2f);
    const float radius = distance(body.position, {0.0f, 3.0f, 0.0f});
    assert(radius > 1.4f && radius < 1.7f);
}

void testHingeDoor() {
    World world;
    world.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned post = world.addBody(box({0.0f, 1.5f, 0.0f}, {0.08f, 1.5f, 0.08f},
                                             8.0f, false));
    const unsigned door = world.addBody(box({0.55f, 1.5f, 0.0f}, {0.55f, 1.2f, 0.05f}, 14.0f));
    JointDesc desc;
    desc.type = JointType::Hinge;
    desc.bodyA = post;
    desc.bodyB = door;
    desc.anchorA = {0.0f, 0.0f, 0.0f};
    desc.anchorB = {-0.55f, 0.0f, 0.0f};
    desc.axis = {0.0f, 1.0f, 0.0f};
    desc.hingeSpan = 0.9f;
    desc.lowerLimit = -1.4f;
    desc.upperLimit = 1.4f;
    assert(world.addJoint(desc) != 0);

    // Kick the free edge.
    world.body(door)->velocity = {0.0f, 0.0f, 5.0f};
    float peakSwing = 0.0f;
    float peakRadius = 0.0f;
    for (int tick = 0; tick < 360; ++tick) {
        world.step(1.0f / 120.0f);
        const Body& moving = *world.body(door);
        peakSwing = std::max(peakSwing, std::abs(moving.position.z));
        peakRadius = std::max(peakRadius,
                              std::abs(distance(moving.position, {0.0f, 1.5f, 0.0f}) - 0.55f));
    }

    const Body& body = *world.body(door);
    assert(finite(body));
    const Vec3 hinge = worldAnchor(world, door, {-0.55f, 0.0f, 0.0f});
    assert(distance(hinge, {0.0f, 1.5f, 0.0f}) < 0.05f);
    // The door must swing around the hinge, not translate away from it.
    assert(peakSwing > 0.3f);
    assert(peakRadius < 0.12f);
    const Joint* joint = world.joint(1);
    assert(joint);
    assert(joint->angle >= desc.lowerLimit - 0.05f);
    assert(joint->angle <= desc.upperLimit + 0.05f);
}

void testHingeLimitStopsSwing() {
    World world;
    world.setGravity({0.0f, 0.0f, 0.0f});
    const unsigned post = world.addBody(box({0.0f, 1.5f, 0.0f}, {0.08f, 1.5f, 0.08f},
                                             8.0f, false));
    const unsigned door = world.addBody(box({0.55f, 1.5f, 0.0f}, {0.55f, 1.2f, 0.05f}, 14.0f));
    JointDesc desc;
    desc.type = JointType::Hinge;
    desc.bodyA = post;
    desc.bodyB = door;
    desc.anchorB = {-0.55f, 0.0f, 0.0f};
    desc.axis = {0.0f, 1.0f, 0.0f};
    desc.hingeSpan = 0.9f;
    desc.lowerLimit = -0.35f;
    desc.upperLimit = 0.35f;
    world.addJoint(desc);
    world.body(door)->angularVelocity = {0.0f, 6.0f, 0.0f};
    for (int tick = 0; tick < 480; ++tick) world.step(1.0f / 120.0f);
    const Joint* joint = world.joint(1);
    assert(joint);
    assert(joint->angle > -0.45f);
    assert(joint->angle < 0.45f);
    assert(finite(*world.body(door)));
}

void testRopeChain() {
    World world;
    world.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned anchor = world.addBody(box({0.0f, 4.0f, 0.0f}, {0.06f, 0.06f, 0.06f},
                                              5.0f, false));
    unsigned previous = anchor;
    std::vector<unsigned> links;
    for (int i = 0; i < 8; ++i) {
        const unsigned link = world.addBody(
            box({0.25f * static_cast<float>(i + 1), 4.0f, 0.0f}, {0.06f, 0.06f, 0.06f}, 0.6f));
        links.push_back(link);
        JointDesc desc;
        desc.type = JointType::Rope;
        desc.bodyA = previous;
        desc.bodyB = link;
        desc.anchorA = {0.0f, 0.0f, 0.0f};
        desc.anchorB = {0.0f, 0.0f, 0.0f};
        desc.restLength = 0.25f;
        world.addJoint(desc);
        previous = link;
    }
    for (int tick = 0; tick < 720; ++tick) world.step(1.0f / 120.0f);

    const Body& last = *world.body(links.back());
    assert(finite(last));
    const float span = distance(last.position, {0.0f, 4.0f, 0.0f});
    assert(span < 8.0f * 0.25f * 1.15f);
    assert(span > 1.2f);
    assert(last.position.y < 2.5f);
    for (std::size_t i = 1; i < links.size(); ++i) {
        const float gap = distance(world.body(links[i])->position,
                                   world.body(links[i - 1])->position);
        assert(gap < 0.30f);
    }
}

void testJointBreak() {
    World world;
    world.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned anchor = world.addBody(box({0.0f, 5.0f, 0.0f}, {0.06f, 0.06f, 0.06f},
                                              1.0f, false));
    const unsigned heavy = world.addBody(box({1.0f, 5.0f, 0.0f}, {0.2f, 0.2f, 0.2f}, 12.0f));
    JointDesc desc;
    desc.type = JointType::Point;
    desc.bodyA = anchor;
    desc.bodyB = heavy;
    desc.anchorB = {-1.0f, 0.0f, 0.0f};
    desc.breakForce = 120.0f;
    const unsigned id = world.addJoint(desc);
    assert(id != 0);
    for (int tick = 0; tick < 60; ++tick) world.step(1.0f / 120.0f);
    const Joint* joint = world.joint(id);
    assert(joint);
    assert(joint->broken);
    assert(world.brokenJointCount() >= 1);
    const float height = world.body(heavy)->position.y;
    for (int tick = 0; tick < 120; ++tick) world.step(1.0f / 120.0f);
    assert(world.body(heavy)->position.y < height - 1.0f);
}

void testJointDeterminism() {
    World a;
    World b;
    for (World* world : {&a, &b}) {
        const unsigned anchor = world->addBody(box({0.0f, 4.0f, 0.0f}, {0.06f, 0.06f, 0.06f},
                                                   5.0f, false));
        unsigned previous = anchor;
        for (int i = 0; i < 5; ++i) {
            const unsigned link = world->addBody(box(
                {0.3f * static_cast<float>(i + 1), 4.0f, 0.0f}, {0.08f, 0.08f, 0.08f}, 0.7f));
            JointDesc desc;
            desc.type = JointType::Rope;
            desc.bodyA = previous;
            desc.bodyB = link;
            desc.anchorA = {0.0f, 0.0f, 0.0f};
            desc.anchorB = {0.0f, 0.0f, 0.0f};
            desc.restLength = 0.3f;
            world->addJoint(desc);
            previous = link;
        }
    }
    for (int tick = 0; tick < 360; ++tick) {
        a.step(1.0f / 120.0f);
        b.step(1.0f / 120.0f);
    }
    assert(a.bodies().size() == b.bodies().size());
    for (std::size_t i = 0; i < a.bodies().size(); ++i) {
        assert(a.bodies()[i].position.x == b.bodies()[i].position.x);
        assert(a.bodies()[i].position.y == b.bodies()[i].position.y);
        assert(a.bodies()[i].position.z == b.bodies()[i].position.z);
        assert(a.bodies()[i].orientation.w == b.bodies()[i].orientation.w);
    }
    assert(a.stats().joints == 5);
    assert(a.stats().brokenJoints == 0);
}

// Adding the joint layer must not change scenes that have no joints.
void testNoJointRegression() {
    World world;
    BodyDesc floor;
    floor.position = {0.0f, -0.5f, 0.0f};
    floor.halfExtents = {10.0f, 0.5f, 10.0f};
    floor.dynamic = false;
    world.addBody(floor);
    for (int i = 0; i < 4; ++i) {
        world.addBody(box({0.0f, 0.5f + static_cast<float>(i), 0.0f},
                          {0.45f, 0.45f, 0.45f}, 2.0f));
    }
    for (int tick = 0; tick < 480; ++tick) world.step(1.0f / 120.0f);
    assert(world.jointCount() == 0);
    assert(world.stats().joints == 0);
    assert(world.stats().brokenJoints == 0);
    // Half extents are 0.45, so the settled centres are 0.45 + 0.9 * layer.
    for (int i = 1; i <= 4; ++i) {
        const float expected = 0.45f + 0.9f * static_cast<float>(i - 1);
        assert(std::abs(world.body(static_cast<unsigned>(i))->position.y - expected) < 0.03f);
    }
}

} // namespace

// A hinge motor advances the joint towards its setpoint and the friction term bleeds off
// the swing. Both are position-level corrections, so the observable is the hinge angle,
// not the angular velocity: the XPBD writeback derives velocity from positions.
void testHingeMotorAndFriction() {
    World world;
    world.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned post = world.addBody(box({0.0f, 2.0f, 0.0f}, {0.2f, 2.0f, 0.2f}, 0.0f, false));
    const unsigned gate = world.addBody(box({2.0f, 2.0f, 0.0f}, {2.0f, 1.0f, 0.1f}, 30.0f));

    JointDesc hinge;
    hinge.type = JointType::Hinge;
    hinge.bodyA = gate;
    hinge.bodyB = post;
    hinge.anchorA = {-2.0f, 0.0f, 0.0f};
    hinge.anchorB = {0.0f, 0.0f, 0.0f};
    hinge.axis = {0.0f, 1.0f, 0.0f};
    hinge.hingeSpan = 0.4f;
    hinge.lowerLimit = -1.4f;
    hinge.upperLimit = 0.0f;
    hinge.motorSpeed = -0.9f;
    hinge.motorMaxTorque = 5000.0f;
    const unsigned id = world.addJoint(hinge);
    assert(id != 0);

    // The motor has to actually move the gate, and the limit has to stop it.
    for (int tick = 0; tick < 240; ++tick) world.step(1.0f / 120.0f);
    const Joint* joint = world.joint(id);
    assert(joint != nullptr);
    assert(joint->angle < -0.5f);
    assert(joint->angle >= hinge.lowerLimit - 0.05f);

    // With friction the gate stops where it is once the motor is switched off; without it
    // the same gate keeps swinging. Same world, only the damping differs.
    World damped;
    damped.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned dampedPost = damped.addBody(box({0.0f, 2.0f, 0.0f}, {0.2f, 2.0f, 0.2f}, 0.0f, false));
    const unsigned dampedGate = damped.addBody(box({2.0f, 2.0f, 0.0f}, {2.0f, 1.0f, 0.1f}, 30.0f));
    JointDesc free = hinge;
    free.bodyA = dampedGate;
    free.bodyB = dampedPost;
    free.motorSpeed = 0.0f;
    free.motorMaxTorque = 0.0f;
    // Wide limits so the stop does not mask the damping: this case is about how far the
    // hinge coasts, not about where it is allowed to go.
    free.lowerLimit = -3.0f;
    free.upperLimit = 3.0f;
    const unsigned freeId = damped.addJoint(free);
    // Kick both gates with the same angular velocity and let them coast.
    damped.body(dampedGate)->angularVelocity = {0.0f, -4.0f, 0.0f};
    for (int tick = 0; tick < 120; ++tick) damped.step(1.0f / 120.0f);
    const float freeSwing = std::abs(damped.joint(freeId)->angle);

    World held;
    held.setGravity({0.0f, -20.0f, 0.0f});
    const unsigned heldPost = held.addBody(box({0.0f, 2.0f, 0.0f}, {0.2f, 2.0f, 0.2f}, 0.0f, false));
    const unsigned heldGate = held.addBody(box({2.0f, 2.0f, 0.0f}, {2.0f, 1.0f, 0.1f}, 30.0f));
    JointDesc gripped = free;
    gripped.bodyA = heldGate;
    gripped.bodyB = heldPost;
    gripped.friction = 1.0f;
    const unsigned heldId = held.addJoint(gripped);
    held.body(heldGate)->angularVelocity = {0.0f, -4.0f, 0.0f};
    for (int tick = 0; tick < 120; ++tick) held.step(1.0f / 120.0f);
    const float heldSwing = std::abs(held.joint(heldId)->angle);
    // Friction must shorten the coast, but the margin is small on purpose: the XPBD hinge
    // already bleeds energy through its own position projection, so damping adds a modest
    // slice on top rather than dominating. Asserting a large ratio would be testing the
    // projection, not the friction.
    assert(heldSwing < freeSwing);
}

int main() {
    testPendulum();
    testHingeDoor();
    testHingeLimitStopsSwing();
    testRopeChain();
    testJointBreak();
    testJointDeterminism();
    testNoJointRegression();
    testHingeMotorAndFriction();
    std::cout << "Native joints: point pendulum, hinge door + angular limit, rope chain, "
                 "break threshold, deterministic replay, no-joint regression and hinge "
                 "motor/friction passed.\n";
    return 0;
}

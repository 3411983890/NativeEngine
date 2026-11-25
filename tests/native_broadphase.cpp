// Broadphase contract tests: the dynamic AABB tree must (a) never miss a pair the
// exact narrowphase would accept, (b) produce the same contact stream as the O(n^2)
// reference path, and (c) survive proxy churn without leaking nodes.

#include "../engine/native/broadphase.hpp"
#include "../engine/native/physics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace engine;
using namespace engine::physics;

namespace {

struct Rng {
    unsigned state = 1u;
    unsigned next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
    float range(float low, float high) { return low + (high - low) * unit(); }
};

Aabb boxAt(Vec3 center, Vec3 half) {
    Aabb box;
    box.min = {center.x - half.x, center.y - half.y, center.z - half.z};
    box.max = {center.x + half.x, center.y + half.y, center.z + half.z};
    return box;
}

unsigned long long pairKey(unsigned a, unsigned b) {
    if (a > b) std::swap(a, b);
    return (static_cast<unsigned long long>(a) << 32) | static_cast<unsigned long long>(b);
}

bool containsPair(const std::vector<unsigned long long>& keys, unsigned a, unsigned b) {
    return std::binary_search(keys.begin(), keys.end(), pairKey(a, b));
}

physics::BodyDesc dynamicBox(Vec3 position, Vec3 half, float mass = 2.0f) {
    physics::BodyDesc desc;
    desc.position = position;
    desc.halfExtents = half;
    desc.mass = mass;
    desc.dynamic = true;
    desc.friction = 0.7f;
    desc.restitution = 0.04f;
    return desc;
}

unsigned ground(physics::World& world) {
    physics::BodyDesc desc;
    desc.position = {0.0f, -0.5f, 0.0f};
    desc.halfExtents = {16.0f, 0.5f, 16.0f};
    desc.dynamic = false;
    desc.friction = 0.9f;
    desc.restitution = 0.02f;
    return world.addBody(desc);
}

void testTreeIsSuperset() {
    AabbTree tree;
    Rng rng{0xC0FFEEu};
    std::vector<Aabb> boxes;
    std::vector<int> proxies;
    const int count = 320;
    boxes.reserve(count);
    proxies.reserve(count);
    for (int i = 0; i < count; ++i) {
        const Aabb box = boxAt({rng.range(-20.0f, 20.0f), rng.range(-2.0f, 12.0f),
                                rng.range(-20.0f, 20.0f)},
                               {rng.range(0.15f, 1.2f), rng.range(0.15f, 1.2f),
                                rng.range(0.15f, 1.2f)});
        boxes.push_back(box);
        proxies.push_back(tree.createProxy(box, static_cast<unsigned>(i), 0.02f));
    }
    assert(tree.proxyCount() == static_cast<unsigned>(count));
    // One leaf per proxy plus internal nodes: a full binary tree has 2n - 1 nodes.
    assert(tree.nodeCount() == 2u * static_cast<unsigned>(count) - 1u);

    std::vector<AabbPair> pairs;
    for (int iteration = 0; iteration < 24; ++iteration) {
        tree.queryPairs(pairs);
        std::vector<unsigned long long> keys;
        keys.reserve(pairs.size());
        for (const AabbPair& pair : pairs) {
            assert(pair.a < pair.b);
            keys.push_back(pairKey(pair.a, pair.b));
        }
        std::sort(keys.begin(), keys.end());
        assert(std::adjacent_find(keys.begin(), keys.end()) == keys.end());

        std::size_t exact = 0;
        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                if (!boxes[i].expanded(0.002f).overlaps(boxes[j])) continue;
                ++exact;
                assert(containsPair(keys, static_cast<unsigned>(i),
                                    static_cast<unsigned>(j)));
            }
        }
        assert(exact > 0);
        const std::size_t allPairs =
            static_cast<std::size_t>(count) * static_cast<std::size_t>(count - 1) / 2;
        assert(pairs.size() <= allPairs);
        assert(pairs.size() < allPairs / 3);

        // Move every proxy and re-check the invariant. Half of them move far
        // enough to force a re-insert, half stay inside the fat box.
        for (int i = 0; i < count; ++i) {
            Vec3 center = boxes[i].center();
            const float step = (i % 2 == 0) ? 0.9f : 0.004f;
            center = {center.x + rng.range(-step, step), center.y,
                      center.z + rng.range(-step, step)};
            const Vec3 half = boxes[i].extent() * 0.5f;
            boxes[i] = boxAt(center, half);
            tree.moveProxy(proxies[i], boxes[i], 0.02f);
        }
    }
    for (int proxy : proxies) tree.destroyProxy(proxy);
    assert(tree.proxyCount() == 0);
    assert(tree.empty());
}

void testProxyLifecycle() {
    AabbTree tree;
    std::vector<int> proxies;
    for (int i = 0; i < 12; ++i) {
        const Aabb box = boxAt({static_cast<float>(i) * 0.4f, 0.0f, 0.0f},
                               {0.5f, 0.5f, 0.5f});
        proxies.push_back(tree.createProxy(box, static_cast<unsigned>(i), 0.01f));
    }
    for (int i = 0; i < 12; i += 2) {
        tree.destroyProxy(proxies[i]);
        proxies[i] = -1;
    }
    assert(tree.proxyCount() == 6);
    std::vector<AabbPair> pairs;
    tree.queryPairs(pairs);
    for (const AabbPair& pair : pairs) {
        assert(pair.a % 2 == 1);
        assert(pair.b % 2 == 1);
    }
    // Destroyed handles are ignored rather than corrupting the tree.
    tree.destroyProxy(proxies[0]);
    assert(tree.proxyCount() == 6);
    for (int i = 1; i < 12; i += 2) tree.destroyProxy(proxies[i]);
    assert(tree.proxyCount() == 0);
    assert(tree.empty());
    tree.queryPairs(pairs);
    assert(pairs.empty());

    // clear() must reset bookkeeping, otherwise a reused world reports stale nodes.
    AabbTree reused;
    reused.createProxy(boxAt({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}), 0, 0.01f);
    reused.createProxy(boxAt({0.5f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}), 1, 0.01f);
    assert(reused.nodeCount() == 3);
    reused.clear();
    assert(reused.nodeCount() == 0);
    assert(reused.proxyCount() == 0);
    assert(reused.height() == 0);
    const int again =
        reused.createProxy(boxAt({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}), 0, 0.01f);
    assert(again >= 0);
    assert(reused.nodeCount() == 1);
}

// The whole point of the tree is that it is only a filter: with the same body
// order the solver must produce bit-identical state to the O(n^2) path.
void testWorldEquivalence() {
    physics::World brute;
    physics::World tree;
    physics::Settings bruteSettings;
    bruteSettings.broadphase = 1;
    physics::Settings treeSettings;
    treeSettings.broadphase = 2;
    brute.setSettings(bruteSettings);
    tree.setSettings(treeSettings);

    for (physics::World* world : {&brute, &tree}) {
        ground(*world);
        for (int layer = 0; layer < 5; ++layer) {
            for (int i = 0; i < 6; ++i) {
                world->addBody(dynamicBox({-3.0f + static_cast<float>(i) * 1.02f,
                                           0.5f + static_cast<float>(layer) * 1.01f,
                                           -1.0f + static_cast<float>(layer % 2) * 0.02f},
                                          {0.5f, 0.5f, 0.5f}, 2.0f));
            }
        }
        for (int i = 0; i < 12; ++i) {
            world->addBody(dynamicBox({-6.0f + static_cast<float>(i) * 0.7f, 3.0f,
                                       4.0f + static_cast<float>(i % 3) * 0.9f},
                                      {0.3f, 0.3f, 0.3f}, 1.1f));
        }
        world->applyRadialImpulse({0.0f, 1.5f, 0.0f}, 8.0f, 4.5f);
    }

    for (int tick = 0; tick < 900; ++tick) {
        brute.step(1.0f / 120.0f);
        tree.step(1.0f / 120.0f);
        assert(brute.lastContactCount() == tree.lastContactCount());
        if (tick % 30 != 0) continue;
        assert(brute.bodies().size() == tree.bodies().size());
        for (std::size_t i = 0; i < brute.bodies().size(); ++i) {
            const Body& left = brute.bodies()[i];
            const Body& right = tree.bodies()[i];
            if (!left.active) {
                assert(!right.active);
                continue;
            }
            assert(left.position.x == right.position.x);
            assert(left.position.y == right.position.y);
            assert(left.position.z == right.position.z);
            assert(left.orientation.w == right.orientation.w);
            assert(left.velocity.y == right.velocity.y);
            assert(left.sleeping == right.sleeping);
        }
    }
    const physics::Stats stats = tree.stats();
    assert(stats.broadphaseMode == 1);
    assert(stats.broadphasePairs > 0);
    assert(stats.broadphasePairs < static_cast<unsigned>(tree.bodyCount()) *
                                      static_cast<unsigned>(tree.bodyCount() - 1) / 4);
}

void testAutoModeAndChurn() {
    physics::World world;
    ground(world);
    for (int i = 0; i < 120; ++i) {
        world.addBody(dynamicBox({-8.0f + static_cast<float>(i % 16) * 1.05f,
                                  0.4f + static_cast<float>(i / 16) * 0.9f,
                                  -2.0f + static_cast<float>((i * 7) % 9) * 1.1f},
                                 {0.28f, 0.28f, 0.28f}, 0.9f));
    }
    physics::Settings settings;
    settings.broadphase = 0;
    settings.broadphaseThreshold = 64;
    world.setSettings(settings);
    for (int tick = 0; tick < 240; ++tick) world.step(1.0f / 120.0f);
    physics::Stats stats = world.stats();
    assert(stats.broadphaseMode == 1);
    const unsigned active = stats.activeBodies;
    assert(active > 64);
    // Leaves only exist for active bodies; every internal node has two children.
    assert(stats.broadphaseNodes == 2u * active - 1u);

    // Deactivating bodies must release their proxies, and switching back to brute
    // force must drop the tree entirely.
    for (unsigned i = 1; i < 40; ++i) world.deactivate(i);
    for (int tick = 0; tick < 4; ++tick) world.step(1.0f / 120.0f);
    stats = world.stats();
    assert(stats.broadphaseNodes == 2u * stats.activeBodies - 1u);
    settings.broadphase = 1;
    world.setSettings(settings);
    world.step(1.0f / 120.0f);
    stats = world.stats();
    assert(stats.broadphaseMode == 0);
    assert(stats.broadphaseNodes == 0);
}

void testSparseScale() {
    physics::World world;
    world.setGravity({0.0f, -9.8f, 0.0f});
    ground(world);
    // A 20 x 20 grid of small crates spread over 40 m: 400 dynamic bodies with
    // almost no neighbourhood, the case where O(n^2) wastes the most work.
    for (int z = 0; z < 20; ++z) {
        for (int x = 0; x < 20; ++x) {
            world.addBody(dynamicBox({-19.0f + static_cast<float>(x) * 2.0f, 0.3f,
                                      -19.0f + static_cast<float>(z) * 2.0f},
                                     {0.25f, 0.25f, 0.25f}, 0.8f));
        }
    }
    physics::Settings settings;
    settings.broadphase = 0;
    world.setSettings(settings);
    world.step(1.0f / 120.0f);
    const physics::Stats stats = world.stats();
    assert(stats.broadphaseMode == 1);
    assert(stats.broadphasePairs < 4000);
    std::cout << "  sparse 401-body broadphase: " << stats.broadphasePairs
              << " candidate pairs, " << stats.broadphaseNodes << " nodes, "
              << stats.broadphaseRebuilds << " rebuilds\n";
}

} // namespace

int main() {
    testTreeIsSuperset();
    testProxyLifecycle();
    testWorldEquivalence();
    testAutoModeAndChurn();
    testSparseScale();
    std::cout << "Native broadphase: superset guarantee, proxy lifecycle, "
                 "bit-identical brute-force equivalence, auto mode switching and "
                 "sparse 400-body filtering passed.\n";
    return 0;
}

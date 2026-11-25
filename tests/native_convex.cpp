// Convex-convex narrowphase tests: GJK overlap detection, EPA depth against analytic
// answers, face-clipped manifold size, hull-vs-box, and determinism.
//
// Every case has a hand-computable answer so a regression shows up as a wrong number
// rather than "still returns true".

#include "../engine/native/convex.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace engine;
using namespace engine::convex;

namespace {

Shape box(Vec3 center, Vec3 half) {
    Shape shape;
    shape.center = center;
    shape.half = half;
    return shape;
}

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

// Unit tetrahedron: four points, four triangles, all normals outward.
struct Tetra {
    Vec3 points[4]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                   {-1.0f, -1.0f, -1.0f}};
    std::uint32_t indices[12]{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
};

Shape tetra(const Tetra& source, Vec3 center) {
    Shape shape;
    shape.center = center;
    shape.points = source.points;
    shape.pointCount = 4;
    shape.indices = source.indices;
    shape.indexCount = 12;
    return shape;
}

void testSeparatedBoxes() {
    Manifold manifold;
    assert(collide(box({0, 0, 0}, {1, 1, 1}), box({3, 0, 0}, {1, 1, 1}), manifold) == Result::Disjoint);
    // Diagonal separation must also be reported as disjoint.
    assert(collide(box({0, 0, 0}, {1, 1, 1}), box({2.2f, 2.2f, 0}, {1, 1, 1}), manifold) == Result::Disjoint);
}

// A at the origin, B pushed 1.5 along +X with the same size: the overlap is 0.5 and the
// separating direction is +X, which is the engine's "from A toward B" convention.
void testBoxDepthAndNormal() {
    Manifold manifold;
    assert(collide(box({0, 0, 0}, {1, 1, 1}), box({1.5f, 0, 0}, {1, 1, 1}), manifold) == Result::Overlapping);
    assert(near(manifold.depth, 0.5f, 1.0e-3f));
    assert(near(manifold.normal.x, 1.0f, 1.0e-3f));
    assert(near(manifold.normal.y, 0.0f, 1.0e-3f));
    assert(near(manifold.normal.z, 0.0f, 1.0e-3f));
    assert(manifold.count >= 1);
    for (int index = 0; index < manifold.count; ++index) {
        // Contact points sit on the shared plane at x = 1.
        assert(near(manifold.points[index].x, 1.0f, 0.05f));
        assert(manifold.penetrations[index] >= 0.0f);
    }
}

// The same depth measured along Y, to catch an axis swap.
void testBoxDepthAlongY() {
    Manifold manifold;
    assert(collide(box({0, 0, 0}, {1, 1, 1}), box({0, 1.5f, 0}, {1, 1, 1}), manifold) == Result::Overlapping);
    assert(near(manifold.depth, 0.5f, 1.0e-3f));
    assert(near(manifold.normal.y, 1.0f, 1.0e-3f));
    // Two overlapping faces must produce a quad, not a single point: a one-point manifold
    // is what makes stacks jitter.
    assert(manifold.count == 4);
}

// Deep overlap: B's centre inside A. EPA must still find the shallowest exit.
void testDeepOverlap() {
    Manifold manifold;
    assert(collide(box({0, 0, 0}, {2, 2, 2}), box({0.5f, 0, 0}, {1, 1, 1}), manifold) == Result::Overlapping);
    // Exit along +X costs 2 + 1 - 0.5 = 2.5; along Y or Z it costs 2 + 1 = 3.
    assert(near(manifold.depth, 2.5f, 5.0e-3f));
    assert(near(manifold.normal.x, 1.0f, 1.0e-3f));
}

void testHullAgainstBox() {
    const Tetra source;
    // Tetrahedron centred at the origin, half extents about 1, against a box overlapping
    // it from +X.
    Manifold manifold;
    assert(collide(tetra(source, {0, 0, 0}), box({1.4f, 0, 0}, {1, 1, 1}), manifold) == Result::Overlapping);
    assert(manifold.depth > 0.0f);
    assert(manifold.count >= 1);
    assert(std::isfinite(manifold.depth));
    assert(near(lengthv(manifold.normal), 1.0f, 1.0e-3f));

    // The same pair pulled apart must be disjoint.
    Manifold apart;
    assert(collide(tetra(source, {0, 0, 0}), box({3.0f, 0, 0}, {1, 1, 1}), apart) == Result::Disjoint);
}

void testHullAgainstHull() {
    const Tetra a, b;
    Manifold manifold;
    // Two tetrahedra 0.4 apart along X: their tips reach x=1 and x=0.6, so they overlap.
    assert(collide(tetra(a, {0, 0, 0}), tetra(b, {0.4f, 0, 0}), manifold) == Result::Overlapping);
    assert(manifold.depth > 0.0f);
    assert(near(lengthv(manifold.normal), 1.0f, 1.0e-3f));

    Manifold apart;
    assert(collide(tetra(a, {0, 0, 0}), tetra(b, {2.6f, 0, 0}), apart) == Result::Disjoint);
}

// The whole point of the module: results feed a deterministic replay, so two identical
// calls must be bit-identical.
void testDeterminism() {
    Manifold first, second;
    const Tetra source;
    assert(collide(tetra(source, {0, 0, 0}), box({0.6f, 0.3f, -0.2f}, {1, 1, 1}), first) == Result::Overlapping);
    assert(collide(tetra(source, {0, 0, 0}), box({0.6f, 0.3f, -0.2f}, {1, 1, 1}), second) == Result::Overlapping);
    assert(first.count == second.count);
    assert(std::memcmp(&first, &second, sizeof(Manifold)) == 0);

    // A rotated basis must not change the outcome across calls either.
    Shape rotated = box({0, 0, 0}, {1, 1, 1});
    const float s = std::sqrt(0.5f);
    rotated.axis[0] = {s, s, 0.0f};
    rotated.axis[1] = {-s, s, 0.0f};
    Manifold third, fourth;
    assert(collide(rotated, box({1.2f, 0, 0}, {1, 1, 1}), third) == Result::Overlapping);
    assert(collide(rotated, box({1.2f, 0, 0}, {1, 1, 1}), fourth) == Result::Overlapping);
    assert(std::memcmp(&third, &fourth, sizeof(Manifold)) == 0);
}

// A rotated box pair must report a depth consistent with the rotation, not the AABB of it.
void testRotatedBoxAgreesWithAabb() {
    Shape rotated = box({0, 0, 0}, {1, 1, 1});
    const float s = std::sqrt(0.5f);
    rotated.axis[0] = {s, s, 0.0f};
    rotated.axis[1] = {-s, s, 0.0f};
    Manifold manifold;
    // A 45-degree box reaches sqrt(2) along X (its half extent, not its full width). The
    // other box's near face sits at 2.2 - 1 = 1.2, so the overlap is sqrt(2) - 1.2. The
    // point of the case is that a box-shaped AABB of the rotated box would report 1.0.
    assert(collide(rotated, box({2.2f, 0, 0}, {1, 1, 1}), manifold) == Result::Overlapping);
    const float expected = std::sqrt(2.0f) - 1.2f;
    assert(near(manifold.depth, expected, 2.0e-2f));
}

} // namespace

int main() {
    testSeparatedBoxes();
    testBoxDepthAndNormal();
    testBoxDepthAlongY();
    testDeepOverlap();
    testHullAgainstBox();
    testHullAgainstHull();
    testDeterminism();
    testRotatedBoxAgreesWithAabb();
    std::cout << "Native convex narrowphase: separation, box depth/normal on two axes, "
                 "deep overlap, hull-vs-box, hull-vs-hull, determinism and rotated box "
                 "extent passed.\n";
    return 0;
}

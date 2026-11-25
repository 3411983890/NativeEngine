#include "../../engine/native/physics.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace engine::physics;

int main() {
    // 3x3 field: a 4 cm step on the +X half, flat elsewhere.
    std::vector<float> heights = {
        0, 0, 0,
        0, .04f, .04f,
        0, .04f, .04f,
    };
    Heightfield field;
    field.originX = -1.5f; field.originZ = -1.5f;
    field.sizeX = 3.0f; field.sizeZ = 3.0f;
    field.width = 3; field.height = 3;
    field.heights = heights.data();
    field.maxErrorMetres = .001f;
    if (!field.valid()) { std::printf("heightfield invalid\n"); return 1; }
    std::printf("samples: low=%.6f high=%.6f\n", field.sample(-1.4f, -1.4f), field.sample(1.4f, 1.4f));
    if (std::abs(field.sample(-1.4f, -1.4f)) > 1e-3f || std::abs(field.sample(1.4f, 1.4f) - .04f) > 1e-3f) {
        std::printf("heightfield sampling failed\n"); return 1;
    }
    if (field.normal(1, 1).y < .99f) { std::printf("flat normal is wrong\n"); return 1; }

    World world;
    world.setGravity({0, -9.81f, 0});
    BodyDesc terrain;
    terrain.position = {0, 0, 0};
    terrain.halfExtents = {1.5f, .2f, 1.5f};
    terrain.dynamic = false;
    terrain.heightfield = &field;
    terrain.entity = 0;
    world.addBody(terrain);
    BodyDesc box;
    box.position = {1.0f, 2.0f, 1.0f};
    box.halfExtents = {.2f, .2f, .2f};
    box.mass = 1.0f;
    box.dynamic = true;
    box.entity = 1;
    const unsigned boxIndex = world.addBody(box);
    for (int tick = 0; tick < 240; ++tick) world.step(1.0f / 120.0f);
    const Body* body = world.body(boxIndex);
    if (!body) { std::printf("body missing\n"); return 1; }
    const float expected = .04f + .2f;
    if (std::abs(body->position.y - expected) > .01f) {
        std::printf("box did not rest on the raised step: y=%.4f expected~%.4f\n", body->position.y, expected);
        return 1;
    }
    // The lower half must still resolve to the lower terrain, not the box top.
    BodyDesc low;
    low.position = {-1.4f, 1.0f, -1.4f};
    low.halfExtents = {.2f, .2f, .2f};
    low.mass = 1.0f;
    low.dynamic = true;
    const unsigned lowIndex = world.addBody(low);
    for (int tick = 0; tick < 240; ++tick) world.step(1.0f / 120.0f);
    body = world.body(lowIndex);
    if (std::abs(body->position.y - .2f) > .01f) {
        std::printf("box did not rest on the flat side: y=%.4f expected~0.2\n", body->position.y);
        return 1;
    }
    std::printf("Heightfield collision: sampling, step retention and resting contact passed.\n");
    return 0;
}

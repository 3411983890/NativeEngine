// Native checks for the matrix helpers used by the DXR shadow pass.
// The shadow ray generator reconstructs world positions with the inverse view-projection,
// so a wrong inverse silently produces shadows in the wrong place. These checks keep the
// math honest without needing a GPU.
#include "../renderer/matrix_math.h"
#include "../engine/native/math.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& name, const std::string& detail = {}) {
    ++checks;
    if (condition) {
        std::printf("  ok   %s\n", name.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
}

bool near(float a, float b, float tolerance = 1.0e-4f) {
    return std::abs(a - b) <= tolerance;
}

void testIdentity() {
    const nativegpu::Mat4 identity = nativegpu::identityMatrix();
    check(near(nativegpu::identityError(identity), 0.0f), "identity has no error");
    nativegpu::Mat4 inverse{};
    check(nativegpu::invertMatrix(identity, inverse), "identity is invertible");
    check(near(nativegpu::identityError(inverse), 0.0f), "inverse of identity is identity");
}

void testSingular() {
    nativegpu::Mat4 singular{};
    singular.m[0] = 0.0f;
    singular.m[5] = 0.0f;
    singular.m[10] = 0.0f;
    singular.m[15] = 0.0f;
    nativegpu::Mat4 inverse{};
    check(!nativegpu::invertMatrix(singular, inverse), "zero matrix is reported singular");
    check(near(nativegpu::identityError(inverse), 0.0f),
          "singular input leaves the output as identity");
}

void testViewProjectionRoundTrip() {
    // A realistic left-handed camera: the same helpers the host uses to build the render
    // view-projection passed into Renderer::render.
    const engine::Vec3 eye{-3.5f, 4.2f, 9.0f};
    const engine::Vec3 target{0.25f, 1.1f, 0.0f};
    const auto view = engine::lookAtLH(eye, target);
    const auto projection = engine::perspectiveLH(1.05f, 1280.0f / 720.0f, 0.1f, 120.0f);
    const auto viewProjection = engine::multiply(projection, view);

    nativegpu::Mat4 matrix{};
    std::memcpy(matrix.m, viewProjection.m, sizeof(matrix.m));

    nativegpu::Mat4 inverse{};
    check(nativegpu::invertMatrix(matrix, inverse), "view-projection is invertible");
    const nativegpu::Mat4 roundTrip = nativegpu::multiplyMatrix(matrix, inverse);
    const float error = nativegpu::identityError(roundTrip);
    check(error < 1.0e-3f, "view-projection * inverse is identity",
          "error " + std::to_string(error));
}

void testPointRoundTrip() {
    // Convert a world point to clip space and back; the reconstructed point must match.
    const engine::Vec3 eye{-2.0f, 3.0f, 7.5f};
    const engine::Vec3 target{1.0f, 1.0f, -1.0f};
    const auto view = engine::lookAtLH(eye, target);
    const auto projection = engine::perspectiveLH(1.05f, 16.0f / 9.0f, 0.1f, 120.0f);
    const auto viewProjection = engine::multiply(projection, view);
    nativegpu::Mat4 matrix{};
    std::memcpy(matrix.m, viewProjection.m, sizeof(matrix.m));
    nativegpu::Mat4 inverse{};
    check(nativegpu::invertMatrix(matrix, inverse), "inverse available for point round trip");

    const float world[3] = {1.25f, 0.75f, -2.5f};
    float clip[3] = {};
    nativegpu::transformPoint(matrix, world, clip);
    // transformPoint applies the perspective divide, so the shadow shader's inverse path
    // can feed it straight back in.
    float reconstructed[3] = {};
    nativegpu::transformPoint(inverse, clip, reconstructed);
    const bool match = near(reconstructed[0], world[0], 1.0e-3f) &&
                       near(reconstructed[1], world[1], 1.0e-3f) &&
                       near(reconstructed[2], world[2], 1.0e-3f);
    check(match, "world -> clip -> world round trip",
          "got " + std::to_string(reconstructed[0]) + "," + std::to_string(reconstructed[1]) +
              "," + std::to_string(reconstructed[2]));
}

void testTranslationScale() {
    nativegpu::Mat4 model = nativegpu::identityMatrix();
    model.m[0] = 2.0f;
    model.m[5] = 3.0f;
    model.m[10] = 4.0f;
    model.m[12] = 5.0f;
    model.m[13] = -6.0f;
    model.m[14] = 0.5f;
    nativegpu::Mat4 inverse{};
    check(nativegpu::invertMatrix(model, inverse), "affine transform is invertible");
    const float point[3] = {1.0f, 1.0f, 1.0f};
    float transformed[3] = {};
    nativegpu::transformPoint(model, point, transformed);
    float restored[3] = {};
    nativegpu::transformPoint(inverse, transformed, restored);
    check(near(restored[0], 1.0f) && near(restored[1], 1.0f) && near(restored[2], 1.0f),
          "scale and translation round trip");
}

} // namespace

int main() {
    std::printf("Renderer math checks\n");
    testIdentity();
    testSingular();
    testViewProjectionRoundTrip();
    testPointRoundTrip();
    testTranslationScale();
    std::printf("Renderer math: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

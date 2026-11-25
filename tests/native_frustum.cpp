// Frustum culling contract tests.
//
// The renderer culls draw items against six planes extracted from the view-projection
// matrix. Two failure modes matter and both are silent: culling something that is
// visible (a hole in the frame) and keeping something that is not (the cost we were
// trying to remove). Every case below pins one of them with an analytic answer.

#include "../renderer/frustum.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace nativegpu;

namespace {

Mat4 identity() {
    Mat4 result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}

Mat4 translation(float x, float y, float z) {
    Mat4 result = identity();
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

Mat4 yawRotation(float radians) {
    Mat4 result = identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    return result;
}

// Column-major multiply, matching matrix_math's layout: out[c*4+r] = sum_k a[k*4+r]*b[c*4+k].
Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

// Left-handed perspective mapping z to [0,1], the convention the shaders use.
Mat4 perspective(float fovY, float aspect, float nearPlane, float farPlane) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 result{};
    result.m[0] = f / aspect;
    result.m[5] = f;
    result.m[10] = farPlane / (farPlane - nearPlane);
    result.m[11] = 1.0f;
    result.m[14] = -nearPlane * farPlane / (farPlane - nearPlane);
    return result;
}

bool visible(const Frustum& frustum, float cx, float cy, float cz, float half) {
    const float minimum[3] = {cx - half, cy - half, cz - half};
    const float maximum[3] = {cx + half, cy + half, cz + half};
    return !aabbOutsideFrustum(frustum, minimum, maximum);
}

void testClipSpaceIdentity() {
    // With clip = position, the visible region is exactly [-1,1]^3.
    const Frustum frustum = frustumFromViewProjection(identity());
    assert(visible(frustum, 0.0f, 0.0f, 0.5f, 0.25f));
    assert(!visible(frustum, 2.5f, 0.0f, 0.5f, 0.25f));
    assert(!visible(frustum, 0.0f, 0.0f, -0.5f, 0.25f));  // behind the near plane
    assert(visible(frustum, 0.95f, 0.0f, 0.5f, 0.25f));  // straddles the right plane
}

void testPerspectiveCases() {
    // Camera at the origin looking down +Z; world translated so z=0 is 5 m ahead.
    const Mat4 viewProjection = multiply(perspective(1.0472f, 1.0f, 0.1f, 100.0f), translation(0.0f, 0.0f, -5.0f));
    const Frustum frustum = frustumFromViewProjection(viewProjection);

    assert(visible(frustum, 0.0f, 0.0f, 10.0f, 1.0f));       // straight ahead
    assert(!visible(frustum, 0.0f, 0.0f, -10.0f, 1.0f));     // behind the eye
    assert(!visible(frustum, 0.0f, 0.0f, 200.0f, 1.0f));     // beyond the far plane
    assert(!visible(frustum, 1000.0f, 0.0f, 10.0f, 1.0f));   // far to the side
    // Straddling the near plane must be kept: this is the ground slab case.
    assert(visible(frustum, 0.0f, 0.0f, 5.0f, 0.5f));
    // A large box whose centre is outside but whose body crosses the frustum.
    assert(visible(frustum, 0.0f, 0.0f, 10.0f, 40.0f));
}

void testTransformAabb() {
    float minimum[3], maximum[3];
    const float localMinimum[3] = {-1.0f, -1.0f, -1.0f};
    const float localMaximum[3] = {1.0f, 1.0f, 1.0f};

    transformAabb(translation(10.0f, 2.0f, -3.0f), localMinimum, localMaximum, minimum, maximum);
    assert(std::fabs(minimum[0] - 9.0f) < 1e-5f && std::fabs(maximum[0] - 11.0f) < 1e-5f);
    assert(std::fabs(minimum[1] - 1.0f) < 1e-5f && std::fabs(maximum[1] - 3.0f) < 1e-5f);
    assert(std::fabs(minimum[2] + 4.0f) < 1e-5f && std::fabs(maximum[2] + 2.0f) < 1e-5f);

    // A 45-degree box: the AABB half-extent is sqrt(2), not 1. Scaling the extents
    // instead of transforming the corners would under-report by 41% and cull the box
    // while part of it is still on screen.
    transformAabb(yawRotation(0.785398f), localMinimum, localMaximum, minimum, maximum);
    assert(std::fabs(maximum[0] - 1.41421f) < 1e-4f);
    assert(std::fabs(maximum[2] - 1.41421f) < 1e-4f);
}

void testRotatedBoxIsNotCulledEarly() {
    // Box rotated 45 degrees, positioned so only its rotated corner is inside the view.
    const Mat4 viewProjection = multiply(perspective(1.0472f, 1.0f, 0.1f, 100.0f), translation(0.0f, 0.0f, -5.0f));
    const Frustum frustum = frustumFromViewProjection(viewProjection);
    const float localMinimum[3] = {-1.0f, -1.0f, -1.0f};
    const float localMaximum[3] = {1.0f, 1.0f, 1.0f};
    Mat4 model = multiply(translation(0.0f, 0.0f, 10.0f), yawRotation(0.785398f));
    float minimum[3], maximum[3];
    transformAabb(model, localMinimum, localMaximum, minimum, maximum);
    assert(!aabbOutsideFrustum(frustum, minimum, maximum));
}

} // namespace

int main() {
    testClipSpaceIdentity();
    testPerspectiveCases();
    testTransformAabb();
    testRotatedBoxIsNotCulledEarly();
    std::cout << "Native frustum: clip-space identity, perspective near/far/side cases, "
                 "conservative AABB transform and rotated-box retention passed.\n";
    return 0;
}

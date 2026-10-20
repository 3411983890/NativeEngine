#pragma once

#include "game.hpp"

#include <algorithm>
#include <cmath>

namespace engine {

// Column-major matrices, column vectors, left-handed view/projection, and
// Direct3D's 0..1 normalized depth interval. Positive object yaw turns -Z
// toward +X, matching Game's movement and the orbit camera.
struct Matrix {
    float m[16]{};
};

inline Matrix identity() {
    Matrix result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}

inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result.m[column * 4 + row] +=
                    a.m[k * 4 + row] * b.m[column * 4 + k];
            }
        }
    }
    return result;
}

inline Matrix perspectiveLH(float verticalFovRadians, float aspect,
                            float nearPlane, float farPlane) {
    verticalFovRadians = std::clamp(verticalFovRadians, 0.01f, 3.13f);
    aspect = std::max(aspect, 0.001f);
    nearPlane = std::max(nearPlane, 0.001f);
    farPlane = std::max(farPlane, nearPlane + 0.001f);
    const float yScale = 1.0f / std::tan(verticalFovRadians * 0.5f);
    Matrix result{};
    result.m[0] = yScale / aspect;
    result.m[5] = yScale;
    result.m[10] = farPlane / (farPlane - nearPlane);
    result.m[11] = 1.0f;
    result.m[14] = -nearPlane * farPlane / (farPlane - nearPlane);
    return result;
}

inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline Vec3 normalized(Vec3 value, Vec3 fallback = {0.0f, 0.0f, 1.0f}) {
    const float lengthSquared = dot(value, value);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f) {
        return fallback;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    return {value.x * inverseLength, value.y * inverseLength,
            value.z * inverseLength};
}

inline Matrix lookAtLH(Vec3 eye, Vec3 target, Vec3 up = {0.0f, 1.0f, 0.0f}) {
    const Vec3 forward = normalized(
        {target.x - eye.x, target.y - eye.y, target.z - eye.z});
    Vec3 right = cross(up, forward);
    if (dot(right, right) < 1.0e-8f) {
        // A vertical camera still gets an orthonormal basis.
        right = cross({0.0f, 0.0f, 1.0f}, forward);
    }
    right = normalized(right, {1.0f, 0.0f, 0.0f});
    const Vec3 cameraUp = cross(forward, right);
    Matrix result = identity();
    result.m[0] = right.x;
    result.m[4] = right.y;
    result.m[8] = right.z;
    result.m[1] = cameraUp.x;
    result.m[5] = cameraUp.y;
    result.m[9] = cameraUp.z;
    result.m[2] = forward.x;
    result.m[6] = forward.y;
    result.m[10] = forward.z;
    result.m[12] = -dot(right, eye);
    result.m[13] = -dot(cameraUp, eye);
    result.m[14] = -dot(forward, eye);
    return result;
}

// Translation * Yaw * Pitch * Roll * Scale. Limb pitch/roll make the
// procedural mannequin articulated without a skeletal animation asset.
inline Matrix compose(Vec3 position, Vec3 scale, float yaw = 0.0f,
                      float pitch = 0.0f, float roll = 0.0f) {
    Matrix yawMatrix = identity();
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    yawMatrix.m[0] = cy;
    yawMatrix.m[2] = sy;
    yawMatrix.m[8] = -sy;
    yawMatrix.m[10] = cy;
    Matrix pitchMatrix = identity();
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    pitchMatrix.m[5] = cp;
    pitchMatrix.m[6] = sp;
    pitchMatrix.m[9] = -sp;
    pitchMatrix.m[10] = cp;
    Matrix rollMatrix = identity();
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    rollMatrix.m[0] = cr;
    rollMatrix.m[1] = sr;
    rollMatrix.m[4] = -sr;
    rollMatrix.m[5] = cr;
    Matrix result = multiply(multiply(yawMatrix, pitchMatrix), rollMatrix);
    for (int row = 0; row < 3; ++row) {
        result.m[row] *= scale.x;
        result.m[4 + row] *= scale.y;
        result.m[8 + row] *= scale.z;
    }
    result.m[12] = position.x;
    result.m[13] = position.y;
    result.m[14] = position.z;
    return result;
}

} // namespace engine

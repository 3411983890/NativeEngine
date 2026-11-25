#pragma once

#include "../vec3.hpp"
#include <array>
#include <cmath>

namespace engine::cloth {

// 4x4 affine matrix in the same layout as the character rig and the JS cook:
// column-vector convention, m[column * 4 + row], translation in m[12..14].
//
// The solver only ever needs point transforms and products of bone matrices with their
// inverse bind matrices, so there is no projection, no quaternion and no general inverse
// here. Keeping the convention identical to the rig is what lets a baked pose be compared
// element by element across the JS/native boundary.
struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row) {
            float sum = 0;
            for (unsigned k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            result.m[column * 4 + row] = sum;
        }
    return result;
}

inline Vec3 transformPoint(const Mat4& matrix, Vec3 point) {
    return {
        matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
        matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
        matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14],
    };
}

inline Vec3 transformDirection(const Mat4& matrix, Vec3 direction) {
    return {
        matrix.m[0] * direction.x + matrix.m[4] * direction.y + matrix.m[8] * direction.z,
        matrix.m[1] * direction.x + matrix.m[5] * direction.y + matrix.m[9] * direction.z,
        matrix.m[2] * direction.x + matrix.m[6] * direction.y + matrix.m[10] * direction.z,
    };
}

inline bool isFinite(const Mat4& matrix) {
    for (float value : matrix.m) if (!std::isfinite(value)) return false;
    return true;
}

} // namespace engine::cloth

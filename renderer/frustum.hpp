#pragma once

// View-frustum culling, shared by the renderer and the native tests.
//
// The renderer used to submit one DrawIndexedInstanced per scene item with no
// visibility test at all, so a 608-entity town cost 608 draws whether the camera
// looked at it or not. This header is the smallest correct thing that fixes that:
// six world-space planes extracted once per frame, and a conservative AABB test.
//
// Two details are easy to get wrong and are pinned here:
//   * Plane extraction depends on the depth convention. D3D clips z to [0,1], so the
//     near plane is row2 alone. The OpenGL variant (row3 + row2) accepts geometry
//     behind the eye and is the usual source of "it culls things it should not".
//   * The test uses the positive vertex (the AABB corner furthest along the plane
//     normal). A centre-and-radius test would reject boxes that are large enough to
//     straddle a plane, which is exactly the case for ground slabs and walls.

#include "matrix_math.h"

#include <algorithm>
#include <cmath>

namespace nativegpu {

// n·p + d >= 0 means "inside". Planes are normalised so d is a metric distance.
struct FrustumPlane {
    float n[3];
    float d;
};

struct Frustum {
    FrustumPlane plane[6];
};

// Gribb-Hartmann extraction for the D3D depth convention.
inline Frustum frustumFromViewProjection(const Mat4& viewProjection) {
    const float* m = viewProjection.m;
    // Column-major storage: element (row r, column c) lives at m[c * 4 + r].
    const float row[4][4] = {
        {m[0], m[4], m[8], m[12]},
        {m[1], m[5], m[9], m[13]},
        {m[2], m[6], m[10], m[14]},
        {m[3], m[7], m[11], m[15]},
    };
    Frustum frustum{};
    // `addW` selects the Gribb-Hartmann form. The side planes and the far plane are
    // combinations of the w row with a clip row; the near plane is the clip row alone
    // because D3D already clips z to [0,1].
    const auto set = [&](int slot, const float source[4], float sign, bool addW) {
        float n[3] = {
            (addW ? row[3][0] : 0.0f) + sign * source[0],
            (addW ? row[3][1] : 0.0f) + sign * source[1],
            (addW ? row[3][2] : 0.0f) + sign * source[2],
        };
        float d = (addW ? row[3][3] : 0.0f) + sign * source[3];
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 1e-20f) {
            const float inverse = 1.0f / length;
            n[0] *= inverse;
            n[1] *= inverse;
            n[2] *= inverse;
            d *= inverse;
        }
        frustum.plane[slot] = {{n[0], n[1], n[2]}, d};
    };
    set(0, row[0], 1.0f, true);   // left
    set(1, row[0], -1.0f, true);  // right
    set(2, row[1], 1.0f, true);   // bottom
    set(3, row[1], -1.0f, true);  // top
    set(4, row[2], 1.0f, false);  // near, z >= 0
    set(5, row[2], -1.0f, true);  // far, z <= 1
    return frustum;
}

// True only when the box is provably outside; everything straddling a plane is kept.
inline bool aabbOutsideFrustum(const Frustum& frustum, const float minimum[3], const float maximum[3]) {
    for (int i = 0; i < 6; ++i) {
        const FrustumPlane& plane = frustum.plane[i];
        const float x = plane.n[0] >= 0.0f ? maximum[0] : minimum[0];
        const float y = plane.n[1] >= 0.0f ? maximum[1] : minimum[1];
        const float z = plane.n[2] >= 0.0f ? maximum[2] : minimum[2];
        if (plane.n[0] * x + plane.n[1] * y + plane.n[2] * z + plane.d < 0.0f) return true;
    }
    return false;
}

// World AABB of a local box under an affine transform. Rotation is allowed, so this
// transforms all eight corners instead of scaling the extents: for a 45-degree box the
// scaled-extent shortcut is wrong by 41% and culls visible geometry.
inline void transformAabb(const Mat4& model, const float minimum[3], const float maximum[3],
                          float outMinimum[3], float outMaximum[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        outMinimum[axis] = 3.402823466e+38f;
        outMaximum[axis] = -3.402823466e+38f;
    }
    for (int corner = 0; corner < 8; ++corner) {
        const float point[3] = {
            (corner & 1) ? maximum[0] : minimum[0],
            (corner & 2) ? maximum[1] : minimum[1],
            (corner & 4) ? maximum[2] : minimum[2],
        };
        for (int axis = 0; axis < 3; ++axis) {
            const float world = model.m[0 * 4 + axis] * point[0] +
                                model.m[1 * 4 + axis] * point[1] +
                                model.m[2 * 4 + axis] * point[2] +
                                model.m[3 * 4 + axis];
            outMinimum[axis] = std::min(outMinimum[axis], world);
            outMaximum[axis] = std::max(outMaximum[axis], world);
        }
    }
}

} // namespace nativegpu

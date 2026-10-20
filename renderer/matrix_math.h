#pragma once

// Small, testable matrix helpers shared by the renderer and the native tests.
// Matrices are column-major with column vectors: clip = viewProjection * model * position.
// This matches the HLSL `column_major float4x4` declarations used by every shader.

namespace nativegpu {

struct Mat4 {
    float m[16];
};

Mat4 identityMatrix();

// Returns false for a singular or non-finite matrix and leaves `out` as identity.
bool invertMatrix(const Mat4& in, Mat4& out);

Mat4 multiplyMatrix(const Mat4& a, const Mat4& b);

// Column-major transform of a point (w = 1) without perspective divide.
void transformPoint(const Mat4& matrix, const float in[3], float out[3]);

// Largest absolute difference between the matrix and identity; used to assert that a
// view-projection round-trips through its inverse.
float identityError(const Mat4& matrix);

} // namespace nativegpu

#include "matrix_math.h"

#include <cmath>
#include <cstring>

namespace nativegpu {

Mat4 identityMatrix() {
    Mat4 result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}

bool invertMatrix(const Mat4& in, Mat4& out) {
    // Cofactor expansion for a column-major 4x4. Same arithmetic as the classic
    // gluInvertMatrix, which assumes column-major storage like Mat4 does.
    const float* m = in.m;
    float inv[16];

    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    const float determinant = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12f) {
        out = identityMatrix();
        return false;
    }
    const float scale = 1.0f / determinant;
    for (int index = 0; index < 16; ++index) {
        const float value = inv[index] * scale;
        if (!std::isfinite(value)) {
            out = identityMatrix();
            return false;
        }
        out.m[index] = value;
    }
    return true;
}

Mat4 multiplyMatrix(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            result.m[column * 4 + row] = sum;
        }
    }
    return result;
}

void transformPoint(const Mat4& matrix, const float in[3], float out[3]) {
    const float x = in[0], y = in[1], z = in[2];
    const float w = matrix.m[3]*x + matrix.m[7]*y + matrix.m[11]*z + matrix.m[15];
    const float safe = (std::abs(w) > 1.0e-9f) ? w : 1.0f;
    out[0] = (matrix.m[0]*x + matrix.m[4]*y + matrix.m[8]*z + matrix.m[12]) / safe;
    out[1] = (matrix.m[1]*x + matrix.m[5]*y + matrix.m[9]*z + matrix.m[13]) / safe;
    out[2] = (matrix.m[2]*x + matrix.m[6]*y + matrix.m[10]*z + matrix.m[14]) / safe;
}

float identityError(const Mat4& matrix) {
    const Mat4 identity = identityMatrix();
    float worst = 0.0f;
    for (int index = 0; index < 16; ++index) {
        const float difference = std::abs(matrix.m[index] - identity.m[index]);
        if (difference > worst) worst = difference;
    }
    return worst;
}

} // namespace nativegpu

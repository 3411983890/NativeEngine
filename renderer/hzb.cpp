#include "hzb.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nativegpu {
namespace {
float sanitize(float value) {
    // A NaN or negative depth disables culling for that tile: treating it as "far"
    // keeps the tile from rejecting anything.
    return std::isfinite(value) ? std::max(0.0f, std::min(1.0f, value)) : 1.0f;
}
}

void DepthPyramid::reset() {
    width_ = height_ = mips_ = 0;
    levels_.clear();
}

void DepthPyramid::build(const float* depth, std::uint32_t width, std::uint32_t height) {
    reset();
    if (!depth || width == 0 || height == 0) return;
    width_ = width; height_ = height;
    levels_.emplace_back(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < levels_[0].size(); ++index) levels_[0][index] = sanitize(depth[index]);
    while (levels_.back().size() > 1) {
        const auto& previous = levels_.back();
        const auto previousWidth = width_, previousHeight = height_;
        const auto nextWidth = std::max(1u, previousWidth / 2), nextHeight = std::max(1u, previousHeight / 2);
        std::vector<float> next(static_cast<std::size_t>(nextWidth) * nextHeight, 0.0f);
        for (std::uint32_t y = 0; y < nextHeight; ++y) {
            for (std::uint32_t x = 0; x < nextWidth; ++x) {
                float value = 0.0f;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const auto sx = std::min(previousWidth - 1, x * 2 + dx);
                        const auto sy = std::min(previousHeight - 1, y * 2 + dy);
                        value = std::max(value, previous[static_cast<std::size_t>(sy) * previousWidth + sx]);
                    }
                }
                next[static_cast<std::size_t>(y) * nextWidth + x] = value;
            }
        }
        levels_.push_back(std::move(next));
        width_ = nextWidth; height_ = nextHeight;
    }
    mips_ = static_cast<std::uint32_t>(levels_.size());
    width_ = width; height_ = height;
}

bool DepthPyramid::occluded(const ScreenRect& rect, float nearestDepth, std::uint32_t* chosenMip) const {
    if (!valid() || rect.empty()) return false;
    // Clamp the rectangle instead of rejecting it: an object partly off-screen still
    // has an on-screen part that must be tested.
    const int maxX = static_cast<int>(width()) - 1, maxY = static_cast<int>(height()) - 1;
    const int x0 = std::max(0, rect.minX), y0 = std::max(0, rect.minY);
    const int x1 = std::min(maxX, rect.maxX), y1 = std::min(maxY, rect.maxY);
    if (x1 < x0 || y1 < y0) return false;

    // Choose the coarsest mip whose rectangle is still at least one texel: a coarser
    // mip covers more screen area, so it can only make the test more conservative.
    std::uint32_t mip = 0;
    std::uint32_t mipWidth = width(), mipHeight = height();
    while (mip + 1 < mips() && ((x1 - x0) >> (mip + 1)) >= 1 && ((y1 - y0) >> (mip + 1)) >= 1) {
        ++mip;
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }
    if (chosenMip) *chosenMip = mip;
    const auto& level = levels_[mip];
    const auto scaleX = static_cast<float>(mipWidth) / static_cast<float>(width());
    const auto scaleY = static_cast<float>(mipHeight) / static_cast<float>(height());
    const int tx0 = std::max(0, static_cast<int>(std::floor(x0 * scaleX)));
    const int ty0 = std::max(0, static_cast<int>(std::floor(y0 * scaleY)));
    const int tx1 = std::min(static_cast<int>(mipWidth) - 1, static_cast<int>(std::ceil(x1 * scaleX)));
    const int ty1 = std::min(static_cast<int>(mipHeight) - 1, static_cast<int>(std::ceil(y1 * scaleY)));
    float farthest = 0.0f;
    for (int y = ty0; y <= ty1; ++y) for (int x = tx0; x <= tx1; ++x) {
        farthest = std::max(farthest, level[static_cast<std::size_t>(y) * mipWidth + x]);
    }
    // Strictly greater: a coplanar box is not behind the occluder.
    return nearestDepth > farthest;
}

ScreenRect DepthPyramid::projectBounds(const float m[16], const float minimum[3], const float maximum[3],
                                       std::uint32_t width, std::uint32_t height, float* nearestDepth) {
    ScreenRect rect{std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    float nearest = 1.0f;
    bool anyInFront = false;
    for (int corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1) ? maximum[0] : minimum[0];
        const float y = (corner & 2) ? maximum[1] : minimum[1];
        const float z = (corner & 4) ? maximum[2] : minimum[2];
        const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
        const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        const float cz = m[2] * x + m[6] * y + m[10] * z + m[14];
        const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (!(cw > 1e-6f)) continue; // behind the eye; the frustum test owns that case
        anyInFront = true;
        const float ndcX = cx / cw, ndcY = cy / cw, ndcZ = cz / cw;
        rect.minX = std::min(rect.minX, static_cast<int>(std::floor((ndcX * 0.5f + 0.5f) * width)));
        rect.maxX = std::max(rect.maxX, static_cast<int>(std::ceil((ndcX * 0.5f + 0.5f) * width)));
        // NDC +Y is up, screen Y is down.
        rect.minY = std::min(rect.minY, static_cast<int>(std::floor((0.5f - ndcY * 0.5f) * height)));
        rect.maxY = std::max(rect.maxY, static_cast<int>(std::ceil((0.5f - ndcY * 0.5f) * height)));
        nearest = std::min(nearest, ndcZ);
    }
    if (nearestDepth) *nearestDepth = nearest;
    if (!anyInFront) return ScreenRect{1, 1, 0, 0};
    return rect;
}

Frustum Frustum::fromViewProjection(const float m[16]) {
    // Gribb–Hartmann extraction for a column-major, column-vector matrix. Row i of the
    // matrix is (m[i], m[4+i], m[8+i], m[12+i]).
    Frustum frustum;
    const auto row = [&](int index, float out[4]) {
        out[0] = m[index]; out[1] = m[4 + index]; out[2] = m[8 + index]; out[3] = m[12 + index];
    };
    float r0[4], r1[4], r2[4], r3[4];
    row(0, r0); row(1, r1); row(2, r2); row(3, r3);
    for (int axis = 0; axis < 4; ++axis) {
        frustum.planes[0][axis] = r3[axis] + r0[axis]; // left
        frustum.planes[1][axis] = r3[axis] - r0[axis]; // right
        frustum.planes[2][axis] = r3[axis] + r1[axis]; // bottom
        frustum.planes[3][axis] = r3[axis] - r1[axis]; // top
        frustum.planes[4][axis] = r2[axis];            // near (0..1 depth)
        frustum.planes[5][axis] = r3[axis] - r2[axis]; // far
    }
    for (auto& plane : frustum.planes) {
        const auto length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length > 1e-12f) for (int axis = 0; axis < 4; ++axis) plane[axis] /= length;
    }
    return frustum;
}

bool Frustum::intersectsAabb(const float minimum[3], const float maximum[3]) const {
    for (const auto& plane : planes) {
        // The positive vertex is the corner farthest along the plane normal. If even it
        // is outside, the whole box is outside.
        const float x = plane[0] >= 0 ? maximum[0] : minimum[0];
        const float y = plane[1] >= 0 ? maximum[1] : minimum[1];
        const float z = plane[2] >= 0 ? maximum[2] : minimum[2];
        if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0) return false;
    }
    return true;
}

bool Frustum::containsPoint(const float point[3]) const {
    for (const auto& plane : planes) {
        if (plane[0] * point[0] + plane[1] * point[1] + plane[2] * point[2] + plane[3] < 0) return false;
    }
    return true;
}

} // namespace nativegpu

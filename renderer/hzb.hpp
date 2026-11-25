#pragma once

#include <cstdint>
#include <vector>

// Hierarchical depth (HZB) occlusion test.
//
// Conventions are part of the contract and are stated here because getting them
// backwards turns a conservative test into a hole in the world:
//   * depth is 0 at the near plane and 1 at the far plane (Direct3D, non-reverse),
//   * the pyramid stores the *maximum* depth of each tile, i.e. the farthest occluder,
//   * a box is rejected only when its nearest depth is farther than that maximum.
//
// The last rule is why the test cannot over-cull: a tile maximum is an upper bound on
// every occluder inside the tile, so if the box is behind that bound it is behind every
// occluder in the tile. A conservative miss (not culling something hidden) only costs
// time; the opposite is a visible hole.

namespace nativegpu {

struct ScreenRect {
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool empty() const { return maxX < minX || maxY < minY; }
};

class DepthPyramid {
public:
    // `depth` is width*height in row-major order, 0 near / 1 far. Non-finite values are
    // treated as far so a broken depth buffer disables culling instead of punching holes.
    void build(const float* depth, std::uint32_t width, std::uint32_t height);
    void reset();

    bool valid() const { return mips_ > 0; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t mips() const { return mips_; }
    const std::vector<float>& level(std::uint32_t mip) const { return levels_[mip]; }

    // Conservative occlusion query for a screen-space rectangle. `nearestDepth` is the
    // smallest depth of the box's eight corners.
    bool occluded(const ScreenRect& rect, float nearestDepth, std::uint32_t* chosenMip = nullptr) const;

    // Screen rectangle of a world-space AABB under a column-major view-projection.
    static ScreenRect projectBounds(const float viewProjection[16], const float minimum[3], const float maximum[3],
                                    std::uint32_t width, std::uint32_t height, float* nearestDepth);

private:
    std::uint32_t width_ = 0, height_ = 0, mips_ = 0;
    std::vector<std::vector<float>> levels_;
};

// Six-plane frustum in world space. Plane normals point inwards.
struct Frustum {
    float planes[6][4]{};
    static Frustum fromViewProjection(const float viewProjection[16]);
    bool intersectsAabb(const float minimum[3], const float maximum[3]) const;
    bool containsPoint(const float point[3]) const;
};

} // namespace nativegpu

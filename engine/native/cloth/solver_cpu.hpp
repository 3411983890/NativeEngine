#pragma once

#include "asset.hpp"
#include "skin.hpp"

namespace engine::cloth {

struct Diagnostics {
    std::uint64_t tick = 0;
    std::uint32_t substeps = 0;
    float edgeErrorP95 = 0, edgeErrorMax = 0;
    float penetrationP99 = 0, penetrationMax = 0;
    float residualMax = 0, maxSpeed = 0;
    std::uint32_t nonFinite = 0;
    float totalMassKg = 0;
    // Garment-only measurements. They are zero for environment cloth, which keeps the
    // existing report fields meaningful instead of overloading them.
    float stretchRatioMax = 0;
    // Stretch implied by the skin goals alone. A welded seam, or a garment whose weights put
    // two neighbours on opposite limbs, cannot be solved tighter than its own target, so this
    // is the floor a stretch report has to be read against rather than a solver failure.
    float targetStretchMax = 0;
    // Same edge, both endpoints fully skin-driven: kinematic, not the solver's geometry.
    float seamStretchMax = 0;
    float skinErrorMax = 0;
    std::uint32_t limitedVertices = 0, penetrationSamples = 0;
};

// CPU XPBD membrane reference and fallback. This is the authority the GPU solver must
// match numerically; it is not evidence that a GPU cloth path exists.
//
// For a garment asset the same solver additionally follows a character skeleton: every
// pose change recomputes the per-vertex skin target and moves the bone-local collision
// proxies, then each substep blends the dynamic vertices toward that target within their
// displacement limit. Environment cloth never enters that path.
class Solver {
public:
    explicit Solver(std::shared_ptr<const Asset> asset, Vec3 origin = {});

    void reset();
    // dtSeconds is the fixed simulation tick. Substepping is derived from it and the
    // asset budget, never from the render frame rate.
    void step(float dtSeconds);
    void setWind(Vec3 wind);
    // One spatial sample per simulation vertex, replaced atomically. Call once
    // before stepping; setWind/reset/teleport discard the spatial samples.
    void setWindSamples(std::vector<Vec3> wind);
    // Teleport: moves the sheet and drops carried velocity so a streamed level cannot
    // inject energy through a large displacement.
    void setOrigin(Vec3 origin);
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    // The reference has no self-collision. Asking for it is an explicit failure rather
    // than silently returning an interpenetrating cloth.
    void setSelfCollision(bool enabled);

    // Garment skinning. setSkeleton takes bone world matrices in the asset's rest space;
    // setPose selects one of the baked poses from the cooked document. Both are rejected
    // on environment cloth instead of being silently ignored.
    void setSkeleton(const std::vector<Mat4>& boneWorld);
    void setPose(std::size_t index);
    std::size_t poseCount() const { return asset_ ? asset_->poses.size() : 0; }
    const std::string& poseName(std::size_t index) const { return asset_->poses.at(index).name; }
    bool skinned() const { return skinned_; }
    const Vec3& skinTarget(std::size_t index) const { return target_.at(index); }
    // Re-seats every vertex on its skin target and drops velocity. This is the garment
    // equivalent of a teleport: a large pose change must not be integrated.
    void reseed();

    Diagnostics diagnostics() const;
    std::uint64_t digest() const;
    std::size_t vertexCount() const { return position_.size(); }
    const Vec3& position(std::size_t index) const { return position_.at(index); }
    const Vec3& velocity(std::size_t index) const { return velocity_.at(index); }
    const Asset& asset() const { return *asset_; }
    std::uint32_t lodFor(float distanceMetres) const;
    float totalMassKg() const;

private:
    void integrate(float h);
    void solveConstraints(float h, float stretchCompliance, float shearCompliance, float bendCompliance);
    void solveFamily(const std::vector<DistanceConstraint>& constraints, float compliance, float h,
                     std::size_t lambdaOffset);
    void solvePins();
    void solveMembrane(std::uint32_t id,float h);
    void solveHinge(std::uint32_t id,float h);
    void solveCollisions();
    void solveEllipsoid(const Ellipsoid& ellipsoid, Vec3& point, float thickness);
    void solveSkin();
    void solveStrain();
    void updateSkinTarget();
    void updateBoneProxies();
    Vec3 windAcceleration(std::size_t vertex) const;

    std::shared_ptr<const Asset> asset_;
    std::vector<Vec3> rest_, position_, previous_, velocity_;
    std::vector<float> inverseMass_, lambda_;
    std::vector<Vec3> windSamples_;
    Vec3 origin_{}, wind_{};
    bool paused_ = false;
    std::uint64_t tick_ = 0;
    std::uint32_t substeps_ = 0;
    // Garment state.
    bool skinned_ = false;
    std::size_t poseIndex_ = 0;
    std::vector<Mat4> boneWorld_, skinMatrices_;
    std::vector<Vec3> target_;
    std::vector<Sphere> activeSpheres_;
    std::vector<Capsule> activeCapsules_;
    std::vector<Ellipsoid> activeEllipsoids_;
    std::vector<std::uint8_t> limited_;
};

} // namespace engine::cloth

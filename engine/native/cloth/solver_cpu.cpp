#include "solver_cpu.hpp"
#include "triangle_math.hpp"
#include "../determinism.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::cloth {
namespace {
constexpr float Gravity = -9.81f;
constexpr float AirDensity = 1.225f;
constexpr float MaxWindAcceleration = 4000.0f;

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float percentile(std::vector<float> values, float fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(values.size() - 1, index)];
}
} // namespace

Solver::Solver(std::shared_ptr<const Asset> asset, Vec3 origin) : asset_(std::move(asset)), origin_(origin) {
    if (!asset_ || asset_->rest.empty() || asset_->masses.size() != asset_->rest.size())
        throw Error("NG-E-CLOTH-ASSET", "Invalid native cloth asset");
    if (!finite(origin)) throw Error("NG-E-CLOTH-ASSET", "Cloth origin is not finite");
    rest_ = asset_->rest;
    inverseMass_.resize(rest_.size());
    for (std::size_t i = 0; i < rest_.size(); ++i) inverseMass_[i] = 1.0f / asset_->masses[i];
    for (const auto& pin : asset_->pins) inverseMass_[pin.vertex] = 0;
    if(asset_->triangleModel)validateTriangleTopology(*asset_);
    lambda_.assign(asset_->triangleModel?asset_->triangles.size()*3+asset_->hinges.size():asset_->stretch.size() + asset_->shear.size() + asset_->bend.size(), 0);
    skinned_ = asset_->garment;
    if (skinned_) {
        if (asset_->bones.empty() || asset_->skinJoints.size() != rest_.size()
            || asset_->skinWeights.size() != rest_.size() || asset_->poses.empty()
            || asset_->blend.size() != rest_.size() || asset_->maxDisplacement.size() != rest_.size()
            || asset_->thickness.size() != rest_.size() || asset_->anchor.size() != rest_.size()
            || asset_->edges.empty())
            throw Error("NG-E-CLOTH-ASSET", "Garment asset is missing part of its skinning contract");
        boneWorld_.assign(asset_->bones.size(), Mat4{});
        skinMatrices_.assign(asset_->bones.size(), Mat4{});
        target_ = rest_;
        for (auto& point : target_) point += origin_;
        limited_.assign(rest_.size(), 0);
        // A garment always starts from a declared pose; the identity skeleton is not a
        // neutral default here, because the bind pose is what the inverse binds cancel.
        setPose(0);
    }
    reset();
}

void Solver::reset() {
    windSamples_.clear();
    if (skinned_ && target_.size() == rest_.size()) position_ = target_;
    else {
        position_ = rest_;
        for (auto& point : position_) point += origin_;
    }
    previous_ = position_;
    velocity_.assign(position_.size(), {});
    std::fill(limited_.begin(), limited_.end(), 0);
    tick_ = 0;
    substeps_ = 0;
}

void Solver::reseed() {
    if (!skinned_) { reset(); return; }
    position_ = target_;
    previous_ = position_;
    velocity_.assign(position_.size(), {});
    std::fill(limited_.begin(), limited_.end(), 0);
    windSamples_.clear();
}

void Solver::setSkeleton(const std::vector<Mat4>& boneWorld) {
    if (!skinned_ || skinMatrices_.empty())
        throw Error("NG-E-CLOTH-UNSUPPORTED", "Only a garment asset has a skeleton bound to its vertices");
    if (boneWorld.size() != asset_->bones.size())
        throw Error("NG-E-CLOTH-SKIN", "Skeleton bone count does not match the garment asset");
    for (const auto& matrix : boneWorld)
        if (!isFinite(matrix)) throw Error("NG-E-CLOTH-SKIN", "Skeleton bone matrix is not finite");
    boneWorld_ = boneWorld;
    for (std::size_t index = 0; index < boneWorld_.size(); ++index) {
        Mat4 bind{};
        bind.m = asset_->bones[index].inverseBind;
        skinMatrices_[index] = boneWorld_[index] * bind;
    }
    updateBoneProxies();
    updateSkinTarget();
}

void Solver::setPose(std::size_t index) {
    if (!skinned_ || skinMatrices_.empty())
        throw Error("NG-E-CLOTH-UNSUPPORTED", "Only a garment asset carries a baked pose set");
    if (index >= asset_->poses.size()) throw Error("NG-E-CLOTH-SKIN", "Pose index outside the baked pose set");
    std::vector<Mat4> world;
    world.reserve(asset_->poses[index].bones.size());
    for (const auto& matrix : asset_->poses[index].bones) {
        Mat4 value{};
        value.m = matrix;
        world.push_back(value);
    }
    poseIndex_ = index;
    setSkeleton(world);
}

void Solver::updateSkinTarget() {
    for (std::size_t vertex = 0; vertex < rest_.size(); ++vertex) {
        Vec3 sum{};
        float total = 0;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const float weight = asset_->skinWeights[vertex][slot];
            if (weight <= 0) continue;
            sum += transformPoint(skinMatrices_[asset_->skinJoints[vertex][slot]], rest_[vertex]) * weight;
            total += weight;
        }
        target_[vertex] = (total > 1e-4f ? sum / total : rest_[vertex]) + origin_;
    }
}

void Solver::updateBoneProxies() {
    activeSpheres_.clear();
    activeCapsules_.clear();
    activeEllipsoids_.clear();
    activeSpheres_.reserve(asset_->boneSpheres.size());
    activeCapsules_.reserve(asset_->boneCapsules.size());
    activeEllipsoids_.reserve(asset_->boneEllipsoids.size());
    for (const auto& sphere : asset_->boneSpheres)
        activeSpheres_.push_back({transformPoint(boneWorld_[sphere.bone], sphere.centre), sphere.radius});
    for (const auto& capsule : asset_->boneCapsules)
        activeCapsules_.push_back({transformPoint(boneWorld_[capsule.bone], capsule.a),
                                   transformPoint(boneWorld_[capsule.bone], capsule.b), capsule.radius});
    for (const auto& ellipsoid : asset_->boneEllipsoids) {
        const Mat4& matrix = boneWorld_[ellipsoid.bone];
        // Radii follow the bone's own axes, so a rotated bone rotates its collider with it.
        const Vec3 axisX = transformDirection(matrix, {1, 0, 0});
        const Vec3 axisY = transformDirection(matrix, {0, 1, 0});
        const Vec3 axisZ = transformDirection(matrix, {0, 0, 1});
        const float scaleX = std::sqrt(dot(axisX, axisX));
        const float scaleY = std::sqrt(dot(axisY, axisY));
        const float scaleZ = std::sqrt(dot(axisZ, axisZ));
        Ellipsoid active;
        active.centre = transformPoint(matrix, ellipsoid.centre);
        active.radii = {ellipsoid.radii.x * std::max(scaleX, 1e-3f),
                        ellipsoid.radii.y * std::max(scaleY, 1e-3f),
                        ellipsoid.radii.z * std::max(scaleZ, 1e-3f)};
        activeEllipsoids_.push_back(active);
    }
}

namespace {
// Signed penetration of a point inside a bone-axis-aligned ellipsoid, measured along the
// direction the solver pushes it. Exact for a sphere, and — unlike a nearest-surface
// distance — it is exactly the distance the collider will move the vertex, which is what a
// penetration budget should report.
float ellipsoidPenetration(const Ellipsoid& ellipsoid, Vec3 point) {
    const Vec3 offset{point.x - ellipsoid.centre.x, point.y - ellipsoid.centre.y, point.z - ellipsoid.centre.z};
    const Vec3 scaled{offset.x / ellipsoid.radii.x, offset.y / ellipsoid.radii.y, offset.z / ellipsoid.radii.z};
    const float radius = std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z);
    if (radius >= 1.0f || radius < 1e-9f) return 0;
    return (1.0f - radius) / radius * std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
}
} // namespace

void Solver::solveEllipsoid(const Ellipsoid& ellipsoid, Vec3& point, float thickness) {
    const Vec3 offset{point.x - ellipsoid.centre.x, point.y - ellipsoid.centre.y, point.z - ellipsoid.centre.z};
    Vec3 scaled{offset.x / ellipsoid.radii.x, offset.y / ellipsoid.radii.y, offset.z / ellipsoid.radii.z};
    const float radius = std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z);
    // The clearance is a world distance; converting it through the mean radius is what makes
    // a 4 mm clearance mean 4 mm on a thin thigh and on a wide chest alike.
    const float mean = (ellipsoid.radii.x + ellipsoid.radii.y + ellipsoid.radii.z) / 3.0f;
    const float required = 1.0f + thickness / std::max(mean, 1e-3f);
    if (radius >= required) return;
    if (radius < 1e-6f) { point.y = ellipsoid.centre.y + ellipsoid.radii.y * required; return; }
    const float scale = required / radius;
    point = {ellipsoid.centre.x + scaled.x * scale * ellipsoid.radii.x,
             ellipsoid.centre.y + scaled.y * scale * ellipsoid.radii.y,
             ellipsoid.centre.z + scaled.z * scale * ellipsoid.radii.z};
}

void Solver::setWind(Vec3 wind) {
    if (!finite(wind)) throw Error("NG-E-CLOTH-WIND", "Cloth wind is not finite");
    wind_ = wind;
    windSamples_.clear();
}

void Solver::setWindSamples(std::vector<Vec3> wind) {
    if(wind.size()!=position_.size()||!std::all_of(wind.begin(),wind.end(),finite))
        throw Error("NG-E-CLOTH-WIND", "Spatial wind requires one finite sample per vertex");
    windSamples_=std::move(wind);
}

void Solver::setOrigin(Vec3 origin) {
    if (!finite(origin)) throw Error("NG-E-CLOTH-ORIGIN", "Cloth origin is not finite");
    const Vec3 delta{origin.x - origin_.x, origin.y - origin_.y, origin.z - origin_.z};
    for (std::size_t i = 0; i < position_.size(); ++i) {
        position_[i] += delta;
        previous_[i] += delta;
        velocity_[i] = {};
    }
    // The skin target is expressed in the cloth's space, so it moves with the origin. A
    // garment whose target stayed behind would be dragged back by its own limit.
    for (auto& point : target_) point += delta;
    for (auto& sphere : activeSpheres_) sphere.centre += delta;
    for (auto& capsule : activeCapsules_) { capsule.a += delta; capsule.b += delta; }
    origin_ = origin;
    windSamples_.clear();
}

void Solver::setSelfCollision(bool enabled) {
    if (enabled) throw Error("NG-E-CLOTH-UNSUPPORTED",
        "Self-collision is not implemented in the CPU reference; disable it as an explicit LOD instead");
}

Vec3 Solver::windAcceleration(std::size_t vertex) const {
    if(asset_->triangleModel){
        Vec3 force{};for(auto id:asset_->adjacency[vertex]){const auto& t=asset_->triangles[id];const auto n=detail::cross3(position_[t[1]]-position_[t[0]],position_[t[2]]-position_[t[0]]);const float twiceArea=detail::length3(n);if(twiceArea<1e-12f)continue;const auto normal=n/twiceArea;
            Vec3 relative{};for(auto v:t)relative+=(windSamples_.empty()?wind_:windSamples_[v])-velocity_[v];relative=relative/3;const float normalSpeed=detail::dot3(relative,normal);force+=normal*(.5f*AirDensity*asset_->material.dragCoefficient*(twiceArea*.5f)*normalSpeed*std::fabs(normalSpeed)/3);
        }
        Vec3 acceleration=force/asset_->masses[vertex];const float magnitude=detail::length3(acceleration);if(magnitude>MaxWindAcceleration)acceleration*=MaxWindAcceleration/magnitude;return acceleration;
    }
    const auto wind=windSamples_.empty()?wind_:windSamples_[vertex];
    if (wind.x == 0 && wind.y == 0 && wind.z == 0) return {};
    const Vec3 relative{wind.x - velocity_[vertex].x, wind.y - velocity_[vertex].y, wind.z - velocity_[vertex].z};
    const float speed = length(relative);
    if (speed < 1e-5f) return {};
    const float area = asset_->areas[vertex];
    const float force = 0.5f * AirDensity * asset_->material.dragCoefficient * area * speed * speed;
    const float magnitude = std::min(MaxWindAcceleration, force / asset_->masses[vertex]);
    return relative * (magnitude / speed);
}

void Solver::integrate(float h) {
    // Garments use the same fixed-tick integration but a different aerodynamic model: a
    // fabric patch under a garment is driven by drag on the body's airflow, and the JS
    // contract (engine/garment/physics.mjs) reports against that. A zero-blend vertex is
    // welded to its skin target and must not integrate at all, or it would accumulate
    // velocity it can never use.
    if (skinned_) {
        for (std::size_t i = 0; i < position_.size(); ++i) {
            if (inverseMass_[i] == 0 || asset_->blend[i] <= 0) {
                previous_[i] = position_[i];
                velocity_[i] = {};
                continue;
            }
            const Vec3 wind = windSamples_.empty() ? wind_ : windSamples_[i];
            Vec3 acceleration{0, Gravity, 0};
            acceleration += wind * asset_->material.dragCoefficient;
            const float magnitude = length(acceleration);
            if (magnitude > MaxWindAcceleration) acceleration = acceleration * (MaxWindAcceleration / magnitude);
            velocity_[i] += acceleration * h;
            velocity_[i] *= 1.0f / (1.0f + asset_->material.damping * h);
            previous_[i] = position_[i];
            position_[i] += velocity_[i] * h;
        }
        return;
    }
    // All aerodynamic faces read the same pre-integration state. In-place
    // neighbour reads here would make the triangular model vertex-order dependent.
    std::vector<Vec3> aerodynamic;
    if(asset_->triangleModel){aerodynamic.resize(position_.size());for(std::size_t i=0;i<position_.size();++i)if(inverseMass_[i]!=0)aerodynamic[i]=windAcceleration(i);}
    for (std::size_t i = 0; i < position_.size(); ++i) {
        if (inverseMass_[i] == 0) { previous_[i] = position_[i]; continue; }
        Vec3 acceleration{0, Gravity, 0};
        acceleration += asset_->triangleModel?aerodynamic[i]:windAcceleration(i);
        velocity_[i] += acceleration * h;
        velocity_[i] *= 1.0f / (1.0f + asset_->material.damping * h);
        previous_[i] = position_[i];
        position_[i] += velocity_[i] * h;
    }
}

void Solver::solveFamily(const std::vector<DistanceConstraint>& constraints, float compliance, float h,
                         std::size_t lambdaOffset) {
    const float alpha = compliance / (h * h);
    for (std::size_t index = 0; index < constraints.size(); ++index) {
        const auto& constraint = constraints[index];
        const std::uint32_t a = constraint.a, b = constraint.b;
        const float wa = inverseMass_[a], wb = inverseMass_[b];
        if (wa + wb == 0) continue;
        Vec3 delta{position_[b].x - position_[a].x, position_[b].y - position_[a].y, position_[b].z - position_[a].z};
        const float current = length(delta);
        if (current < 1e-9f) continue;
        const float constraintValue = current - constraint.rest;
        auto& lambda = lambda_[lambdaOffset + index];
        const float deltaLambda = (-constraintValue - alpha * lambda) / (wa + wb + alpha);
        lambda += deltaLambda;
        const Vec3 normal = delta / current;
        position_[a] -= normal * (wa * deltaLambda);
        position_[b] += normal * (wb * deltaLambda);
    }
}

void Solver::solveConstraints(float h, float stretchCompliance, float shearCompliance, float bendCompliance) {
    if(asset_->triangleModel){for(const auto& color:asset_->membraneColors)for(auto id:color)solveMembrane(id,h);for(const auto& color:asset_->hingeColors)for(auto id:color)solveHinge(id,h);return;}
    solveFamily(asset_->stretch, stretchCompliance, h, 0);
    solveFamily(asset_->shear, shearCompliance, h, asset_->stretch.size());
    solveFamily(asset_->bend, bendCompliance, h, asset_->stretch.size() + asset_->shear.size());
}

void Solver::solveMembrane(std::uint32_t id,float h){
    const auto& ids=asset_->triangles[id];const auto& m=asset_->membranes[id];
    for(unsigned kind=0;kind<3;++kind){std::array<Vec3,3> p{position_[ids[0]],position_[ids[1]],position_[ids[2]]},gradient;float c;if(!detail::membrane(p,m,kind,c,gradient))continue;
        const float alpha=(kind<2?asset_->material.stretchCompliance:asset_->material.shearCompliance)/(m.area*h*h);float denominator=0;for(unsigned i=0;i<3;++i)denominator+=inverseMass_[ids[i]]*detail::dot3(gradient[i],gradient[i]);if(denominator<1e-12f)continue;
        auto& lambda=lambda_[id*3+kind];const float delta=(-c-alpha*lambda)/(denominator+alpha);lambda+=delta;for(unsigned i=0;i<3;++i)position_[ids[i]]+=gradient[i]*(inverseMass_[ids[i]]*delta);
    }
}
void Solver::solveHinge(std::uint32_t id,float h){
    const auto& hinge=asset_->hinges[id];std::array<Vec3,4> p,gradient;for(unsigned i=0;i<4;++i)p[i]=position_[hinge.vertices[i]];float angle;if(!detail::dihedral(p,angle,gradient))return;
    float denominator=0;for(unsigned i=0;i<4;++i)denominator+=inverseMass_[hinge.vertices[i]]*detail::dot3(gradient[i],gradient[i]);if(denominator<1e-12f)return;
    const float alpha=asset_->material.bendAngleCompliance/(h*h);auto& lambda=lambda_[asset_->triangles.size()*3+id];const float delta=(-detail::angleDifference(angle,hinge.angle)-alpha*lambda)/(denominator+alpha);lambda+=delta;for(unsigned i=0;i<4;++i)position_[hinge.vertices[i]]+=gradient[i]*(inverseMass_[hinge.vertices[i]]*delta);
}

void Solver::solvePins() {
    for (const auto& pin : asset_->pins) position_[pin.vertex] = pin.anchor + origin_;
}

void Solver::solveCollisions() {
    const auto& collision = asset_->collision;
    // A garment collides against the *posed* skeleton proxies; an environment cloth
    // collides against the capsules authored in its own file.
    const auto& spheres = skinned_ ? activeSpheres_ : collision.spheres;
    const auto& capsules = skinned_ ? activeCapsules_ : collision.capsules;
    for (std::size_t i = 0; i < position_.size(); ++i) {
        if (inverseMass_[i] == 0) continue;
        if (skinned_ && asset_->blend[i] <= 0) continue;
        float thickness = asset_->material.thicknessMetres * 0.5f + collision.thicknessMetres;
        if (skinned_) thickness = asset_->thickness[i] + collision.thicknessMetres;
        if (collision.plane.present) {
            const float distance = dot(position_[i], collision.plane.normal) - collision.plane.offsetMetres;
            if (distance < thickness) {
                position_[i] += collision.plane.normal * (thickness - distance);
            }
        }
        for (const auto& sphere : spheres) {
            Vec3 delta{position_[i].x - sphere.centre.x, position_[i].y - sphere.centre.y, position_[i].z - sphere.centre.z};
            const float distance = length(delta);
            const float radius = sphere.radius + thickness;
            if (distance < radius) {
                const Vec3 normal = distance > 1e-6f ? delta / distance : Vec3{0, 1, 0};
                position_[i] = sphere.centre + normal * radius;
            }
        }
        for (const auto& capsule : capsules) {
            Vec3 axis{capsule.b.x - capsule.a.x, capsule.b.y - capsule.a.y, capsule.b.z - capsule.a.z};
            const float axisLength2 = dot(axis, axis);
            if (axisLength2 < 1e-12f) continue;
            Vec3 offset{position_[i].x - capsule.a.x, position_[i].y - capsule.a.y, position_[i].z - capsule.a.z};
            const float t = std::clamp(dot(offset, axis) / axisLength2, 0.0f, 1.0f);
            const Vec3 closest = capsule.a + axis * t;
            Vec3 delta{position_[i].x - closest.x, position_[i].y - closest.y, position_[i].z - closest.z};
            const float distance = length(delta);
            const float radius = capsule.radius + thickness;
            if (distance < radius) {
                const Vec3 normal = distance > 1e-6f ? delta / distance : Vec3{0, 1, 0};
                position_[i] = closest + normal * radius;
            }
        }
        for (const auto& ellipsoid : activeEllipsoids_) solveEllipsoid(ellipsoid, position_[i], thickness);
    }
}

// Skin projection: the dynamic blend decides how much of the body's motion the vertex
// follows, the anchor keeps a seam welded, and the displacement limit keeps the fabric
// near its skinned goal. All three are separate channels on purpose — collapsing them
// into one number is what makes "the sleeve is stiff" impossible to diagnose.
void Solver::solveSkin() {
    for (std::size_t i = 0; i < position_.size(); ++i) {
        if (inverseMass_[i] == 0) continue;
        if (asset_->blend[i] <= 0) {
            // Fully skin-driven: welded to the target, no residual velocity. This is the
            // collar/waist seam behaviour, not a soft pull.
            position_[i] = target_[i];
            previous_[i] = target_[i];
            velocity_[i] = {};
            continue;
        }
        const float anchor = asset_->anchor[i];
        if (anchor > 0) position_[i] += (target_[i] - position_[i]) * std::min(1.0f, anchor);
        const Vec3 offset = position_[i] - target_[i];
        const float distance = length(offset);
        const float limit = asset_->maxDisplacement[i];
        if (distance > limit && distance > 1e-9f) {
            // Soft limit: half of the excess per projection. A hard clamp re-imposes itself
            // after the strain solve and turns a region boundary into a torn seam, because
            // two neighbours are then held rigidly by different limits.
            const float allowed = limit + (distance - limit) * 0.5f;
            position_[i] = target_[i] + offset * (allowed / distance);
            limited_[i] = 1;
        }
    }
}

// Strain limiting over the baked rest-edge list. Pure XPBD membrane iterations let a long
// edge run past its rest length before the correction propagates, and the reported stretch
// then describes the iteration count rather than the fabric.
void Solver::solveStrain() {
    for (const auto& edge : asset_->edges) {
        const std::uint32_t a = edge.a, b = edge.b;
        const float wa = asset_->blend[a] > 0 ? inverseMass_[a] : 0;
        const float wb = asset_->blend[b] > 0 ? inverseMass_[b] : 0;
        const float total = wa + wb;
        if (total <= 0) continue;
        const Vec3 delta = position_[b] - position_[a];
        const float current = length(delta);
        const float maximum = edge.rest * asset_->strainLimit;
        if (current <= maximum || current < 1e-9f) continue;
        const float correction = ((current - maximum) / current) / total;
        position_[a] += delta * (correction * wa);
        position_[b] -= delta * (correction * wb);
    }
}

void Solver::step(float dtSeconds) {
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0 || dtSeconds > 0.25f)
        throw Error("NG-E-CLOTH-TIME", "Cloth step requires a finite tick in (0, 0.25] seconds");
    if (paused_) return;
    const auto required = static_cast<std::uint32_t>(std::ceil(dtSeconds * 480.0f));
    substeps_ = std::min(asset_->maxSubsteps, std::max(1u, required));
    const float h = dtSeconds / static_cast<float>(substeps_);
    for (std::uint32_t substep = 0; substep < substeps_; ++substep) {
        integrate(h);
        std::fill(lambda_.begin(), lambda_.end(), 0.0f);
        for (std::uint32_t iteration = 0; iteration < asset_->iterations; ++iteration) {
            solveConstraints(h, asset_->material.stretchCompliance, asset_->material.shearCompliance,
                             asset_->material.bendCompliance);
            // Projection order: membrane, skin (anchor + displacement limit), strain, then
            // collision. Strain has to come *after* the skin projection, because a welded
            // seam next to a limited vertex is exactly where an edge gets stretched: clamping
            // first and welding afterwards reports a stretch the limiter never saw. Collision
            // stays last so a corrected vertex is never pushed back into the body — which is
            // why the displacement budget is explicitly second to the penetration budget.
            if (skinned_) solveSkin();
            if (skinned_) solveStrain();
            solvePins();
            solveCollisions();
        }
        solvePins();
        for (std::size_t i = 0; i < position_.size(); ++i) {
            if (inverseMass_[i] == 0 || (skinned_ && asset_->blend[i] <= 0)) { velocity_[i] = {}; continue; }
            velocity_[i] = (position_[i] - previous_[i]) / h;
            if (skinned_) {
                // A teleport or a snapped pose can leave one component far outside any real
                // fabric speed; clamping keeps that from becoming a launch on the next tick.
                const float speed = length(velocity_[i]);
                if (speed > asset_->maxSpeed && speed > 1e-9f) velocity_[i] = velocity_[i] * (asset_->maxSpeed / speed);
            }
        }
    }
    ++tick_;
}

Diagnostics Solver::diagnostics() const {
    Diagnostics result;
    result.tick = tick_;
    result.substeps = substeps_;
    result.totalMassKg = totalMassKg();
    std::vector<float> errors, penetrations;
    if (skinned_) {
        // Garment: stretch is measured against the baked rest edges, penetration against
        // the *posed* proxies the solver actually collided with, and the skin error is the
        // distance the fabric was allowed to drift away from the character.
        errors.reserve(asset_->edges.size());
        for (const auto& edge : asset_->edges) {
            const float current = length(position_[edge.b] - position_[edge.a]);
            const float goal = length(target_[edge.b] - target_[edge.a]);
            result.targetStretchMax = std::max(result.targetStretchMax, goal / edge.rest);
            // Edges whose endpoints are both fully skin-driven are kinematic: the skeleton
            // moves them, the solver does not. Reporting them as solver stretch would blame
            // the physics for the rest shape of the garment.
            if (asset_->blend[edge.a] <= 0 && asset_->blend[edge.b] <= 0)
                result.seamStretchMax = std::max(result.seamStretchMax, current / edge.rest);
            else
                result.stretchRatioMax = std::max(result.stretchRatioMax, current / edge.rest);
            const float error = std::fabs(current - edge.rest) / edge.rest;
            errors.push_back(error);
            result.edgeErrorMax = std::max(result.edgeErrorMax, error);
        }
        result.edgeErrorP95 = percentile(errors, 0.95f);
        result.residualMax = result.edgeErrorMax;
        for (std::size_t i = 0; i < position_.size(); ++i) {
            result.skinErrorMax = std::max(result.skinErrorMax, length(position_[i] - target_[i]));
            if (limited_[i]) ++result.limitedVertices;
            for (const auto& sphere : activeSpheres_) {
                if (const float depth = sphere.radius - length(position_[i] - sphere.centre); depth > 0)
                    penetrations.push_back(depth);
            }
            for (const auto& capsule : activeCapsules_) {
                const Vec3 axis = capsule.b - capsule.a;
                const float axisLength2 = dot(axis, axis);
                if (axisLength2 < 1e-12f) continue;
                const float t = std::clamp(dot(position_[i] - capsule.a, axis) / axisLength2, 0.0f, 1.0f);
                if (const float depth = capsule.radius - length(position_[i] - (capsule.a + axis * t)); depth > 0)
                    penetrations.push_back(depth);
            }
            for (const auto& ellipsoid : activeEllipsoids_) {
                if (const float depth = ellipsoidPenetration(ellipsoid, position_[i]); depth > 0)
                    penetrations.push_back(depth);
            }
        }
        result.penetrationSamples = static_cast<std::uint32_t>(penetrations.size());
        result.penetrationP99 = percentile(penetrations, 0.99f);
        result.penetrationMax = penetrations.empty() ? 0 : *std::max_element(penetrations.begin(), penetrations.end());
        for (std::size_t i = 0; i < position_.size(); ++i) {
            if (!finite(position_[i]) || !finite(velocity_[i])) ++result.nonFinite;
            result.maxSpeed = std::max(result.maxSpeed, length(velocity_[i]));
        }
        return result;
    }
    errors.reserve(asset_->stretch.size());
    for (const auto& constraint : asset_->stretch) {
        Vec3 delta{position_[constraint.b].x - position_[constraint.a].x,
                   position_[constraint.b].y - position_[constraint.a].y,
                   position_[constraint.b].z - position_[constraint.a].z};
        const float error = std::fabs(length(delta) - constraint.rest) / constraint.rest;
        errors.push_back(error);
        result.edgeErrorMax = std::max(result.edgeErrorMax, error);
        result.residualMax = std::max(result.residualMax, error);
    }
    result.edgeErrorP95 = percentile(errors, 0.95f);
    // Penetration is measured on the committed state after the solve, not on the
    // intermediate positions inside an iteration. That is what a renderer or a
    // gameplay query can observe.
    const auto& collision = asset_->collision;
    for (std::size_t i = 0; i < position_.size(); ++i) {
        if (collision.plane.present) {
            const float distance = dot(position_[i], collision.plane.normal) - collision.plane.offsetMetres;
            if (distance < 0) penetrations.push_back(-distance);
        }
        for (const auto& sphere : collision.spheres) {
            Vec3 delta{position_[i].x - sphere.centre.x, position_[i].y - sphere.centre.y, position_[i].z - sphere.centre.z};
            if (const float distance = length(delta); distance < sphere.radius)
                penetrations.push_back(sphere.radius - distance);
        }
        for (const auto& capsule : collision.capsules) {
            Vec3 axis{capsule.b.x - capsule.a.x, capsule.b.y - capsule.a.y, capsule.b.z - capsule.a.z};
            const float axisLength2 = dot(axis, axis);
            if (axisLength2 < 1e-12f) continue;
            Vec3 offset{position_[i].x - capsule.a.x, position_[i].y - capsule.a.y, position_[i].z - capsule.a.z};
            const float t = std::clamp(dot(offset, axis) / axisLength2, 0.0f, 1.0f);
            const Vec3 closest = capsule.a + axis * t;
            Vec3 delta{position_[i].x - closest.x, position_[i].y - closest.y, position_[i].z - closest.z};
            if (const float distance = length(delta); distance < capsule.radius)
                penetrations.push_back(capsule.radius - distance);
        }
    }
    result.penetrationP99 = percentile(penetrations, 0.99f);
    result.penetrationMax = penetrations.empty() ? 0 : *std::max_element(penetrations.begin(), penetrations.end());
    for (std::size_t i = 0; i < position_.size(); ++i) {
        if (!finite(position_[i]) || !finite(velocity_[i])) ++result.nonFinite;
        result.maxSpeed = std::max(result.maxSpeed, length(velocity_[i]));
    }
    return result;
}

float Solver::totalMassKg() const {
    float total = 0;
    for (auto mass : asset_->masses) total += mass;
    return total;
}

std::uint32_t Solver::lodFor(float distanceMetres) const {
    if (asset_->lod.empty()) return 0;
    for (std::uint32_t index = 0; index < asset_->lod.size(); ++index)
        if (distanceMetres <= asset_->lod[index].maxDistanceMetres) return index;
    return static_cast<std::uint32_t>(asset_->lod.size() - 1);
}

std::uint64_t Solver::digest() const {
    determinism::Hasher hash;
    hash.text(skinned_ ? "nativegpu.cloth-cpu-reference/3-garment" : "nativegpu.cloth-cpu-reference/2");
    hash.text(asset_->hash);
    hash.u64(tick_);
    if (skinned_) {
        hash.u64(poseIndex_);
        for (std::size_t i = 0; i < target_.size(); ++i) {
            hash.quantized(target_[i].x); hash.quantized(target_[i].y); hash.quantized(target_[i].z);
        }
    }
    hash.quantized(origin_.x); hash.quantized(origin_.y); hash.quantized(origin_.z);
    hash.quantized(wind_.x); hash.quantized(wind_.y); hash.quantized(wind_.z);
    hash.u64(windSamples_.size());
    for(const auto& wind:windSamples_) {hash.quantized(wind.x);hash.quantized(wind.y);hash.quantized(wind.z);}
    hash.u8(paused_);
    for (std::size_t i = 0; i < position_.size(); ++i) {
        hash.quantized(position_[i].x); hash.quantized(position_[i].y); hash.quantized(position_[i].z);
        hash.quantized(velocity_[i].x); hash.quantized(velocity_[i].y); hash.quantized(velocity_[i].z);
    }
    return hash.value();
}

} // namespace engine::cloth

#include "game.hpp"
#include "math.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {
constexpr double kFixedStep = 1.0 / 120.0;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kContactEpsilon = 0.0001f;
constexpr unsigned kPhysicsDebrisCap = 220;

struct ShardPiece {
    Vec3 center;
    Vec3 half;
};

std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float random01(std::uint32_t& state) {
    return static_cast<float>(nextRandom(state) & 0x00FFFFFFu) /
           static_cast<float>(0x01000000u);
}

// Deterministic binary fracture pattern. This is intentionally shard-based rather
// than a true Voronoi cell decomposition because the current raster path instances
// oriented boxes only. It preserves the two properties that matter for gameplay:
// mass/volume conservation and hierarchical splitting, so a 3x2x0.3 wall produces
// 12 irregular slabs instead of 12 identical cubes.
std::vector<ShardPiece> splitBox(Vec3 half, int count, std::uint32_t seed) {
    std::vector<ShardPiece> pieces;
    pieces.push_back({{0, 0, 0}, half});
    if (seed == 0) seed = 0x9E3779B9u;
    while (static_cast<int>(pieces.size()) < count) {
        std::size_t best = 0;
        float bestVolume = -1.0f;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const ShardPiece& piece = pieces[i];
            const float volume = piece.half.x * piece.half.y * piece.half.z;
            if (volume > bestVolume) {
                bestVolume = volume;
                best = i;
            }
        }
        ShardPiece& piece = pieces[best];
        int axis = 0;
        if (piece.half.y > piece.half.x) axis = 1;
        if ((axis == 0 ? piece.half.x : piece.half.y) < piece.half.z) axis = 2;
        // If the largest axis is already very thin, use the next one so shards do
        // not degenerate into paper.
        const float dims[3] = {piece.half.x, piece.half.y, piece.half.z};
        if (dims[axis] < 0.055f) {
            axis = dims[0] > dims[1] ? (dims[0] > dims[2] ? 0 : 2)
                                     : (dims[1] > dims[2] ? 1 : 2);
        }
        float fraction = 0.36f + random01(seed) * 0.28f;
        float halfAxis = axis == 0 ? piece.half.x
                                   : (axis == 1 ? piece.half.y : piece.half.z);
        ShardPiece low = piece;
        ShardPiece high = piece;
        const float lowHalf = halfAxis * fraction;
        const float highHalf = halfAxis * (1.0f - fraction);
        const float lowOffset = -halfAxis + lowHalf;
        const float highOffset = halfAxis - highHalf;
        if (axis == 0) {
            low.half.x = lowHalf; low.center.x += lowOffset;
            high.half.x = highHalf; high.center.x += highOffset;
        } else if (axis == 1) {
            low.half.y = lowHalf; low.center.y += lowOffset;
            high.half.y = highHalf; high.center.y += highOffset;
        } else {
            low.half.z = lowHalf; low.center.z += lowOffset;
            high.half.z = highHalf; high.center.z += highOffset;
        }
        pieces[best] = low;
        pieces.push_back(high);
    }
    return pieces;
}

Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

bool overlap(float amin, float amax, float bmin, float bmax) {
    return amax > bmin + kContactEpsilon && amin < bmax - kContactEpsilon;
}

float distanceSquaredXZ(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

Vec3 halfExtents(const Entity& entity) {
    return {std::abs(entity.scale.x) * 0.5f,
            std::abs(entity.scale.y) * 0.5f,
            std::abs(entity.scale.z) * 0.5f};
}

unsigned textureIndex(const std::string& pattern) {
    if (pattern == "brick") return 1;
    if (pattern == "metal") return 2;
    return 0;
}
} // namespace

float Game::playerHeight() const {
    return playerSceneIndex_>=0?std::max(.001f,finiteOr(scene_.entities[static_cast<std::size_t>(playerSceneIndex_)].scale.y,1.8f)):1.8f;
}
float Game::playerRadius() const {
    if(playerSceneIndex_<0)return .3f;const auto scale=scene_.entities[static_cast<std::size_t>(playerSceneIndex_)].scale;
    return std::max(.0005f,std::max(std::abs(finiteOr(scale.x,.6f)),std::abs(finiteOr(scale.z,.6f)))*.5f);
}
float Game::playerParameter(const char* key,float fallback) const {
    if(playerSceneIndex_<0)return fallback;const auto& profile=scene_.entities[static_cast<std::size_t>(playerSceneIndex_)].character.controller;
    return profile.contains(key)&&profile.at(key).is_number()?finiteOr(profile.at(key).get<float>(),fallback):fallback;
}
float Game::playerStepHeight() const {return std::clamp(playerParameter("stepHeight",.36f*playerHeight()/1.8f),0.f,playerHeight()*.99f);}
float Game::playerModelScale() const {
    if(playerSceneIndex_>=0){const auto& ref=scene_.entities[static_cast<std::size_t>(playerSceneIndex_)].character;
        if(std::isfinite(ref.scale.y)&&ref.scale.y>0.0001f)return ref.scale.y;}
    return playerHeight()/1.8f;
}
float Game::movementEdge() const {return std::max(worldEdge(),playerRadius()+.001f);}
Vec3 Game::orbitCameraTarget() const {return {player_.x,visualY_+playerParameter("targetHeight",1.1f*playerHeight()/1.8f),player_.z};}
Vec3 Game::orbitCameraEye() const {
    const float distance=playerParameter("orbitDistance",7.f*std::max(playerHeight()/1.8f,playerRadius()/.3f));
    return {player_.x-std::sin(yaw_)*distance*std::cos(pitch_),visualY_+playerParameter("orbitHeight",1.4f*playerHeight()/1.8f)+std::sin(pitch_)*distance,player_.z+std::cos(yaw_)*distance*std::cos(pitch_)};
}
Vec3 Game::characterBodyOffset(int entity,physics::Quat orientation) const {
    if(entity<0||static_cast<std::size_t>(entity)>=scene_.entities.size())return {};
    const auto& source=scene_.entities[static_cast<std::size_t>(entity)];return source.characterAsset>=0&&source.character.anchor=="feet"?physics::rotate(orientation,{0,source.scale.y*.5f,0}):Vec3{};
}

void Game::initialize(Scene scene) {
    contentId_ = gameplay::sceneIdentity(scene);
    initialScene_=scene;
    scene_ = std::move(scene);
    playerSceneIndex_=-1;for(std::size_t i=0;i<scene_.entities.size();++i)if(scene_.entities[i].kind=="player"){playerSceneIndex_=static_cast<int>(i);break;}
    vehicleSpawns_.clear();
    for (const Entity& entity : scene_.entities) {
        if (entity.kind == "vehicle") vehicleSpawns_.push_back(entity.position);
    }
    scene_.walkSpeed = std::clamp(finiteOr(scene_.walkSpeed, 3.0f), 0.1f, 30.0f);
    scene_.runSpeed = std::clamp(finiteOr(scene_.runSpeed, 6.0f),
                                scene_.walkSpeed, 60.0f);
    scene_.jumpSpeed = std::clamp(finiteOr(scene_.jumpSpeed, 7.0f), 0.1f, 30.0f);
    scene_.gravity = std::clamp(finiteOr(scene_.gravity, 20.0f), 1.0f, 100.0f);
    spawn_ = location("player", {0.0f, 0.0f, 5.0f});
    spawn_.x = std::clamp(finiteOr(spawn_.x, 0.0f),
                          -movementEdge() + playerRadius(),
                          movementEdge() - playerRadius());
    spawn_.y = std::max(finiteOr(spawn_.y, 0.0f), 0.0f);
    spawn_.z = std::clamp(finiteOr(spawn_.z, 5.0f),
                          -movementEdge() + playerRadius(),
                          movementEdge() - playerRadius());
    reset();
}

void Game::reset() {
    scene_=initialScene_;
    player_ = spawn_;
    // Vehicles are part of the scene state: reset puts them back at their authored
    // positions so a replay starts from the same traffic pattern.
    std::size_t vehicle = 0;
    for (Entity& entity : scene_.entities) {
        if (entity.kind == "vehicle" && vehicle < vehicleSpawns_.size()) {
            entity.position = vehicleSpawns_[vehicle++];
        }
    }
    verticalVelocity_ = 0.0f;
    yaw_ = 0.0f;
    pitch_ = firstPerson() ? 0.0f : 0.35f;
    for(const auto& e:scene_.entities)if(e.kind=="player"&&e.components.contains("camera")){yaw_=e.components["camera"].value("initialYaw",0.f);pitch_=e.components["camera"].value("initialPitch",pitch_);}
    bodyYaw_ = yaw_;
    visualY_ = player_.y;
    animationPhase_ = 0.0f;
    animationSpeed_ = 0.0f;
    accumulator_ = 0.0;
    grounded_ = player_.y <= 0.0f;
    hasKey_ = false;
    doorOpen_ = false;
    complete_ = false;
    jumpWasDown_ = false;
    interactWasDown_ = false;
    fireWasDown_ = false;
    blastWasDown_ = false;
    jumpPending_ = false;
    interactPending_ = false;
    firePending_ = false;
    blastPending_ = false;
    feedbackTime_ = 0.0f;
    feedback_.clear();
    fractureCount_ = 0;
    fragmentsSpawned_ = 0;
    structuralCollapses_ = 0;
    projectileCount_ = 0;
    explosionCount_ = 0;
    impactCount_ = 0;
    peakImpactImpulse_ = 0.0f;
    totalImpactEnergy_ = 0.0f;
    // Rebuild the rigid world from the immutable scene so F5 restores every intact
    // destructible and removes debris. Hot reload uses a fresh Game, not reset().
    initializePhysics();
    if (water_) water_->reset();
    footprintState_ = {};
    bodyWaterVolume_.assign(physics_.bodyCount(), 0.0f);
    waterDisturbances_ = 0;
    waterMaxImmersion_ = 0.0f;
    wading_ = {};
    // A reset puts the simulation back at tick 0 so a recorded run can be replayed from
    // its own beginning instead of a global counter.
    ticks_ = 0;
    animation_.reset();
    animationEvents_.clear();
    initializeGameplay();
}

std::vector<animation::Event> Game::takeAnimationEvents() {
    std::vector<animation::Event> events;
    events.swap(animationEvents_);
    return events;
}

void Game::setCharacter(std::shared_ptr<const animation::Asset> asset, bool features) {
    if(!asset)throw animation::Error("NG-E-ANIM-SKELETON","Missing character asset");
    for(const auto& required:{asset->chestBone,asset->rightHandBone}) {
        if(required.empty())continue;
        bool found=false;
        for(const auto& bone:asset->skeleton.bones)if(bone.name==required)found=true;
        if(!found)throw animation::Error("NG-E-ANIM-SKELETON",std::string("Missing gameplay attachment: ")+required);
    }
    animation::ControllerConfig config;
    config.clips=asset->controllerClips;
    config.motionMatching=config.proceduralRig=config.muscles=config.softTissue=features;
    config.walkSpeed=scene_.walkSpeed;
    config.runSpeed=std::max(scene_.runSpeed,scene_.walkSpeed+0.01f);
    if(playerSceneIndex_>=0){const auto& rig=scene_.entities[static_cast<std::size_t>(playerSceneIndex_)].character.rig;
        if(!rig.empty()){
            config.rigOverride=true;config.rig=asset->rig.settings;
            auto read=[&](const char* key,float& value){if(rig.contains(key)&&rig.at(key).is_number()){const float v=rig.at(key).get<float>();if(std::isfinite(v))value=v;}};
            read("height",config.rig.height);read("turnThreshold",config.rig.turnThreshold);
            read("turnStepHeight",config.rig.turnStepHeight);read("turnStepDuration",config.rig.turnStepDuration);
            read("turnCooldown",config.rig.turnCooldown);read("maxTwist",config.rig.maxTwist);
            read("maxSlopeCosine",config.rig.maxSlopeCosine);read("swingPrediction",config.rig.swingPrediction);
            read("maxFootCorrection",config.rig.maxFootCorrection);read("pelvisLower",config.rig.pelvisLower);
            read("pelvisUpper",config.rig.pelvisUpper);read("maxStepHeight",config.rig.maxStepHeight);
            read("maxPelvisShift",config.rig.maxPelvisShift);read("soleRoll",config.rig.soleRoll);
            if(rig.contains("footLock")&&rig.at("footLock").is_boolean())config.rig.footLock=rig.at("footLock").get<bool>();
            if(rig.contains("turnInPlace")&&rig.at("turnInPlace").is_boolean())config.rig.turnInPlace=rig.at("turnInPlace").get<bool>();
            if(rig.contains("enabled")&&rig.at("enabled").is_boolean()&&!rig.at("enabled").get<bool>())config.proceduralRig=false;
        }
    }
    animation_.initialize(std::move(asset),config);
    animation_.setGroundQuery([this](animation::V3 origin,float distance){
        animation::GroundHit best;float nearest=distance;
        for(const auto& body:physics_.bodies()){
            if(!body.active||body.projectile)continue;
            if(body.entity>=0&&std::size_t(body.entity)<scene_.entities.size()){
                const auto& entity=scene_.entities[std::size_t(body.entity)];
                if(entity.kind!="ground"&&!solid(entity))continue;
            }
            if (body.heightfield != nullptr) {
                const auto& field = *body.heightfield;
                // Height samples are already in world metres. The body's box is
                // only a broadphase proxy and can extend metres above the riverbed.
                if (!field.valid() || origin.x < field.originX || origin.z < field.originZ ||
                    origin.x > field.originX + field.sizeX || origin.z > field.originZ + field.sizeZ) continue;
                const float height = field.sample(origin.x, origin.z);
                const float separation = origin.y - height;
                if (separation < 0 || separation > nearest) continue;
                nearest = separation;
                const auto normal = field.normal(origin.x, origin.z);
                best = {};
                best.hit = true; best.surface = body.id;
                best.point = {origin.x, height, origin.z};
                best.normal = {normal.x, normal.y, normal.z};
                continue;
            }
            animation::Transform transform;transform.translation={body.position.x,body.position.y,body.position.z};transform.rotation={body.orientation.x,body.orientation.y,body.orientation.z,body.orientation.w};auto matrix=animation::matrix(transform);
            auto a=animation::inversePoint(matrix,origin),b=animation::inversePoint(matrix,{origin.x,origin.y-distance,origin.z});
            float start[3]{a.x,a.y,a.z},delta[3]{b.x-a.x,b.y-a.y,b.z-a.z},half[3]{body.half.x,body.half.y,body.half.z};float enter=0,exit=1;int axis=-1;float sign=1;
            bool valid=true;for(int k=0;k<3;++k){if(std::abs(delta[k])<1e-7f){if(std::abs(start[k])>half[k])valid=false;continue;}float lo=(-half[k]-start[k])/delta[k],hi=(half[k]-start[k])/delta[k],normal=-1;if(lo>hi){std::swap(lo,hi);normal=1;}if(lo>enter){enter=lo;axis=k;sign=normal;}exit=std::min(exit,hi);if(enter>exit)valid=false;}
            if(!valid||axis<0||enter<0||enter>1||enter*distance>nearest)continue;
            nearest=enter*distance;animation::V3 localNormal{};if(axis==0)localNormal.x=sign;else if(axis==1)localNormal.y=sign;else localNormal.z=sign;auto n=animation::point(matrix,localNormal);
            best.hit=true;best.surface=body.id;best.point={origin.x,origin.y-nearest,origin.z};best.normal={n.x-body.position.x,n.y-body.position.y,n.z-body.position.z};best.surfaceToWorld=matrix;
            auto arm=Vec3{best.point.x-body.position.x,best.point.y-body.position.y,best.point.z-body.position.z};auto w=body.angularVelocity;
            best.velocity={body.velocity.x+w.y*arm.z-w.z*arm.y,body.velocity.y+w.z*arm.x-w.x*arm.z,body.velocity.z+w.x*arm.y-w.y*arm.x};
        }return best;
    });
    // Asset changes start a new simulation; there is no partial hot replacement.
    reset();
}

Vec3 Game::location(const std::string& kind, Vec3 fallback) const {
    for (const Entity& entity : scene_.entities) {
        if (entity.kind == kind) {
            return entity.position;
        }
    }
    return fallback;
}

bool Game::solid(const Entity& entity) const {
    if (!entity.collider || entity.kind == "ground" || entity.kind == "player" ||
        entity.kind == "key" || entity.kind == "goal" || entity.kind == "light") {
        return false;
    }
    const auto handle = gameplay_.world.find(entity.id);
    if (gameplay_.world.valid(handle)) {
        const auto* collider = gameplay_.world.get(handle, "collider");
        if (collider) return collider->at("enabled").get<bool>();
    }
    return false;
}

void Game::update(Input input, float dt) {
    input.forward = std::clamp(finiteOr(input.forward, 0.0f), -1.0f, 1.0f);
    input.right = std::clamp(finiteOr(input.right, 0.0f), -1.0f, 1.0f);
    const bool firstPersonCamera = firstPerson();
    // The LH view faces -Z at yaw zero, so its screen-right axis is -X.
    // Positive first-person look input must turn toward that screen-right axis.
    yaw_ = std::remainder(yaw_ + (firstPersonCamera ? -1.0f : 1.0f) * finiteOr(input.yawDelta, 0.0f), 2.0f * kPi);
    pitch_ = std::clamp(pitch_ + finiteOr(input.pitchDelta, 0.0f), firstPersonCamera ? -1.45f : -0.15f, firstPersonCamera ? 1.45f : 1.15f);
    jumpPending_ = jumpPending_ || (input.jump && !jumpWasDown_);
    interactPending_ = interactPending_ || (input.interact && !interactWasDown_);
    firePending_ = firePending_ || (input.fire && !fireWasDown_);
    blastPending_ = blastPending_ || (input.blast && !blastWasDown_);
    jumpWasDown_ = input.jump;
    interactWasDown_ = input.interact;
    fireWasDown_ = input.fire;
    blastWasDown_ = input.blast;

    // A debugger pause or window drag cannot produce a giant physics step.
    // All movement, contacts, and interactions run at a fixed 120 Hz.
    accumulator_ += static_cast<double>(
        std::clamp(finiteOr(dt, 0.0f), 0.0f, 0.10f));
    while (accumulator_ + 1.0e-9 >= kFixedStep) {
        step(input, static_cast<float>(kFixedStep));
        if(tickObserver_)tickObserver_(ticks_,player_);
        accumulator_ -= kFixedStep;
    }
    accumulator_ = std::max(accumulator_, 0.0);
}

void Game::updateVehicles(float dt) {
    for (Entity& entity : scene_.entities) {
        if (entity.kind != "vehicle" || entity.vehicleSpeed <= 0.0f) continue;
        const float direction = entity.vehicleDirection >= 0 ? 1.0f : -1.0f;
        entity.position.z += direction * entity.vehicleSpeed * dt;
        // The town street runs roughly -37..41; wrap with a small off-screen margin.
        if (direction > 0.0f && entity.position.z > 40.0f) entity.position.z = -36.0f;
        if (direction < 0.0f && entity.position.z < -36.0f) entity.position.z = 40.0f;
        const auto h=gameplay_.world.find(entity.id);
        if(gameplay_.world.valid(h)) {auto transform=*gameplay_.world.get(h,"transform");transform["position"]={entity.position.x,entity.position.y,entity.position.z};gameplay_.world.set(h,"transform",transform);}
    }
}

void Game::step(const Input& input, float dt) {
    ++ticks_;
    gameplay_.tick({{"forward",input.forward},{"right",input.right},{"run",input.run},{"jump",input.jump},{"interact",interactPending_}});
    syncGameplay();
    updateVehicles(dt);
    updateWadingState();
    const auto requestActor=gameplay_.world.find(playerEntityId());
    if(gameplay_.world.valid(requestActor))if(const auto* request=gameplay_.world.get(requestActor,"blast_request")){
        const auto center=request->at("center");const float radius=request->at("radius"),speed=request->at("speed");
        gameplay_.world.remove(requestActor,"blast_request");
        detonate({center[0],center[1],center[2]},radius,speed);
    }
    feedbackTime_ = std::max(0.0f, feedbackTime_ - dt);
    if (firePending_) {
        fireProjectile();
        firePending_ = false;
    }
    if (blastPending_) {
        const float s = std::sin(yaw_);
        const float c = std::cos(yaw_);
        detonate({player_.x + s * 3.8f, player_.y + 0.55f*playerHeight()/1.8f, player_.z - c * 3.8f},
                 6.0f, 9.0f);
        blastPending_ = false;
    }
    if (jumpPending_) {
        if (grounded_) {
            verticalVelocity_ = scene_.jumpSpeed * wading_.jumpScale;
            grounded_ = false;
            stepGrace_ = 0.0f;
        }
        jumpPending_ = false;
    }
    float forward = input.forward;
    float right = firstPerson() ? -input.right : input.right;
    const float length = std::sqrt(forward * forward + right * right);
    if (length > 1.0f) {
        forward /= length;
        right /= length;
    }
    const float speed = (input.run ? scene_.runSpeed : scene_.walkSpeed) * wading_.speedScale;
    const float s = std::sin(yaw_);
    const float c = std::cos(yaw_);
    const Vec3 oldPosition = player_;
    const bool followingTerrain = grounded_ && verticalVelocity_ <= 0.0f &&
        std::abs(player_.y - terrainHeightAt(player_.x, player_.z)) <= .02f * playerHeight() / 1.8f;
    const float moveX = (s * forward + c * right) * speed * dt;
    const float moveZ = (-c * forward + s * right) * speed * dt;
    tryStepUp(moveX, moveZ);
    moveHorizontal(moveX, moveZ);
    // A step-up lifts the body onto a tread it is not standing over yet. Letting
    // gravity run during that short forward slide made the character read as
    // airborne for a dozen ticks per riser, which reset the foot solve every tick
    // (the "climbing stairs catches the legs" report). The grace only covers the
    // slide onto the tread the body was just lifted onto.
    if (stepGrace_ > 0.0f) stepGrace_ = std::max(0.0f, stepGrace_ - dt);
    const bool stepping = stepGrace_ > 0.0f;
    if (stepping) {
        verticalVelocity_ = 0.0f;
    } else {
        verticalVelocity_ -= scene_.gravity * dt;
        if (wading_.verticalDampingPerSecond > 0.0f)
            verticalVelocity_ *= std::max(0.0f, 1.0f - wading_.verticalDampingPerSecond * dt);
    }
    // Retain support down a continuous walkable slope. Gravity alone takes
    // several ticks to catch a descending surface and would reset CDM each time.
    // Limit the snap to the horizontal travel (45 degrees) plus contact tolerance;
    // a jump or a ledge drop must remain airborne.
    const float groundSnap = followingTerrain
        ? std::sqrt(distanceSquaredXZ(player_, oldPosition)) + .005f * playerHeight() / 1.8f : 0.0f;
    moveVertical(verticalVelocity_ * dt, groundSnap);
    if (stepping) grounded_ = true;
    if (water_) {
        // Real soles when the rig exposes them. The pivot belongs to this actor, so a
        // stale or uninitialised pose is rejected by proximity instead of stamping a
        // contact at the world origin. A lifted foot is raised in Y, which lets the
        // state machine emit a weak `Leaving` contact instead of dragging under water.
        bool usedFeet = false;
        // A controller without a solved foot has not produced usable sole data yet
        // (features disabled, first tick, or a rig without foot locks). The root
        // footprint is the documented degraded path in that case.
        bool rigFeet = false;
        if (animation_.asset()) {
            const auto& poseFeet = animation_.featureState().feet;
            rigFeet = poseFeet[0].pivotValid || poseFeet[0].locked || poseFeet[1].pivotValid || poseFeet[1].locked;
        }
        if (rigFeet) {
            for (unsigned side = 0; side < 2; ++side) {
                const auto& foot = animation_.featureState().feet[side];
                const auto base = foot.pivotValid || foot.locked ? foot.pivot : foot.animated;
                const float dxFoot = base.x - player_.x, dzFoot = base.z - player_.z;
                if (std::fabs(dxFoot) > 3.0f || std::fabs(dzFoot) > 3.0f) continue;
                const float lift = foot.locked ? 0.0f : 0.35f;
                const Vec3 sole{base.x, base.y + lift, base.z};
                const auto contact = water::updateFootContact(
                    *water_, sole, dt, side + 1, footWaterState_[side], footWaterSettings_,
                    1u,
                    side == 0 ? water::ContactSource::LeftFoot : water::ContactSource::RightFoot);
                if (contact.interacted) ++waterDisturbances_;
                usedFeet = true;
            }
        }
        if (!usedFeet) {
            const auto footprint = water::updateFootprint(
                *water_, player_, playerHeight(), player_.x - oldPosition.x,
                player_.z - oldPosition.z, dt, footprintState_, footprintSettings_,
                std::max(0.0f, -verticalVelocity_));
            if (footprint.interacted) ++waterDisturbances_;
        }
        // Record the post-move immersion for reporting and for the next HUD frame.
        // Movement itself used the state at the start of the tick.
        updateWadingState();
        applyWaterCoupling(dt);
    }

    const float actualSpeed =
        std::sqrt(distanceSquaredXZ(player_, oldPosition)) / dt;
    // A step-up moves the collision body a whole riser in one tick. The model (and
    // the rig's foot solve) catch up over ~0.1 s instead, so the feet stay on the
    // tread they were standing on and the leg is not stretched up the riser.
    if (firstPerson() || std::abs(player_.y - visualY_) > 1.0f) {
        visualY_ = player_.y;
    } else {
        visualY_ += (player_.y - visualY_) * std::min(1.0f, dt * 14.0f);
        if (std::abs(player_.y - visualY_) < 1.0e-4f) visualY_ = player_.y;
    }
    // The camera steers; the body follows. While both feet are planted the body
    // does not rotate at all: it only turns once the rig has lifted a foot for a
    // turn step, which is what removes the "model spins on its heels" slide. A
    // small leftover (below one step) is settled by a slow pivot once the player
    // stops steering.
    if (firstPerson()) {
        bodyYaw_ = yaw_;
    } else {
        const float delta = std::remainder(yaw_ - bodyYaw_, 2.0f * kPi);
        const bool moving = actualSpeed > 0.6f ||
                            animation_.state() == animation::State::Walk ||
                            animation_.state() == animation::State::Run;
        const bool airborne = animation_.feetAirborne();
        const bool steering = std::abs(finiteOr(input.yawDelta, 0.0f)) > 1.0e-5f;
        // Only a foot that is off the ground may carry a rotation. A leftover
        // smaller than one step is settled by a slow pivot once the player stops
        // steering, so the facing never stays permanently off the camera.
        const float rate = airborne ? (moving ? 10.0f : 4.0f)
                                    : (!steering && std::abs(delta) < 0.30f ? 1.2f : 0.0f);
        const float limit = rate * dt;
        bodyYaw_ = std::abs(delta) <= limit
                       ? yaw_
                       : std::remainder(bodyYaw_ + std::copysign(limit, delta),
                                        2.0f * kPi);
    }
    animationSpeed_ += (actualSpeed - animationSpeed_) * std::min(1.0f, dt * 18.0f);
    animationPhase_ = std::remainder(
        animationPhase_ + actualSpeed * dt * 3.4f, 2.0f * kPi);
    if (interactPending_) {
        if(animation_.asset()) {
            // A slot produces events for consumers, never gameplay writes.
            animation::BoneMask upper;
            const auto& bones=animation_.asset()->skeleton.bones;
            for(unsigned b=1;b<bones.size();++b) {
                int parent=static_cast<int>(b);
                while(parent>0 && bones[parent].name!=animation_.asset()->chestBone) parent=bones[parent].parent;
                if(parent>0)upper.set(b);
            }
            // Interaction animation is optional content, but a requested clip must exist.
            bool hasInteraction=false;
            for(const auto& clip:animation_.asset()->clips) if(clip.name=="interact")hasInteraction=true;
            if(hasInteraction)animation_.play("interact",upper);
        }
        interact();
        interactPending_ = false;
    }
    // Rigid bodies run at the same fixed 120 Hz as gameplay. Character movement is a
    // swept AABB controller over the current static/dynamic body AABBs: it can stand
    // on crates and debris, pushes light props, and a post-step pass removes any
    // penetration introduced by bodies that moved into the player this tick.
    physics_.step(dt);
    if (water_) water_->step(dt);
    processImpacts();
    // Physics bodies may have moved into the character during this tick (a pushed
    // crate, a collapsing shard, a falling box). Resolve that penetration before the
    // next input tick so the player never renders inside a dynamic object.
    resolveCharacterPenetration();
    publishGameplayPhysics();
    const auto actor = gameplay_.world.find(playerEntityId());
    if (gameplay_.world.valid(actor)) {
        auto transform = *gameplay_.world.get(actor, "transform");
        transform["position"] = {player_.x,player_.y,player_.z};
        gameplay_.world.set(actor,"transform",transform);publishedTransforms_[playerEntityId()]=transform;
        gameplay_.collectTriggers(playerEntityId());
    }
    syncGameplay();
    if(animation_.asset()) {
        animation::Intent intent{ticks_,actualSpeed,verticalVelocity_,bodyYaw_,grounded_};
        intent.position={player_.x,visualY_,player_.z};
        // Where the body is being steered, not where it currently faces. The rig
        // lifts a foot for the difference; the body only turns afterwards.
        intent.hasTurnTarget=true;
        intent.turnTarget=yaw_;
        intent.modelScale=playerModelScale();
        intent.actualVelocity={(player_.x-oldPosition.x)/dt,(player_.y-oldPosition.y)/dt,(player_.z-oldPosition.z)/dt};
        intent.desiredVelocity={moveX/dt,0,moveZ/dt};
        if(animation_.featureState().hasPreviousPosition)intent.angularVelocity=std::remainder(bodyYaw_-animation_.featureState().previousHeading,2*kPi)/dt;
        animation_.tick(intent);
        const auto& events=animation_.events();
        animationEvents_.insert(animationEvents_.end(),events.begin(),events.end());
    }
}

void Game::collectCharacterBoxes(std::vector<CharacterBox>& boxes) const {
    boxes.clear();
    boxes.reserve(physics_.bodyCount() + 8);
    for (const physics::Body& body : physics_.bodies()) {
        if (!body.active || body.projectile) continue;
        // Terrain is resolved by direct height sampling, not as a box.
        if (body.heightfield != nullptr) continue;
        // Use the body's current world AABB. Static bodies are authored AABBs;
        // dynamic bodies rotate and translate, so the authored scene AABB would be
        // stale and invisible to the character.
        const physics::Mat3 rotation = physics::toMatrix(body.orientation);
        const Vec3 extent{
            std::abs(rotation.m[0]) * body.half.x +
                std::abs(rotation.m[3]) * body.half.y +
                std::abs(rotation.m[6]) * body.half.z,
            std::abs(rotation.m[1]) * body.half.x +
                std::abs(rotation.m[4]) * body.half.y +
                std::abs(rotation.m[7]) * body.half.z,
            std::abs(rotation.m[2]) * body.half.x +
                std::abs(rotation.m[5]) * body.half.y +
                std::abs(rotation.m[8]) * body.half.z};
        CharacterBox box;
        box.center = body.position;
        box.half = extent;
        box.bodyIndex = static_cast<int>(&body - physics_.bodies().data());
        box.dynamic = body.dynamic;
        boxes.push_back(box);
    }
    // Doors and vehicles are still gameplay-owned scene boxes, not physics bodies.
    // Entities whose physics body exists (active or fractured) are already represented
    // above; this also removes the old invisible collider left after destruction.
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        if (entityBody_[index] >= 0) continue;
        const Entity& entity = scene_.entities[index];
        if (!solid(entity)) continue;
        const Vec3 half = halfExtents(entity);
        CharacterBox box;
        box.center = entity.position+characterBodyOffset(static_cast<int>(index));
        box.half = half;
        boxes.push_back(box);
    }
}

void Game::pushDynamicBody(int bodyIndex, Vec3 direction, float speed) {
    if (bodyIndex < 0) return;
    physics::Body* body = physics_.body(static_cast<unsigned>(bodyIndex));
    if (!body || !body->active || !body->dynamic) return;
    const float length = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    if (length < 1.0e-6f) return;
    const float massScale = std::min(1.0f, 5.0f / std::max(0.4f, body->mass));
    body->velocity.x += direction.x / length * speed * massScale;
    body->velocity.z += direction.z / length * speed * massScale;
    physics_.wake(static_cast<unsigned>(bodyIndex));
}

void Game::tryStepUp(float dx, float dz) {
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!grounded_ || length < 1.0e-6f) return;
    const float bodyScale=playerHeight()/1.8f;
    const Vec3 direction{dx / length, 0.0f, dz / length};
    const Vec3 probe{player_.x + direction.x * (playerRadius() + 0.05f*bodyScale), player_.y,
                     player_.z + direction.z * (playerRadius() + 0.05f*bodyScale)};
    std::vector<CharacterBox> boxes;
    collectCharacterBoxes(boxes);
    float stepTop = player_.y;
    for (const CharacterBox& box : boxes) {
        const float top = box.center.y + box.half.y;
        if (top <= player_.y + 0.015f*bodyScale || top > player_.y + playerStepHeight()) continue;
        if (std::abs(probe.x - box.center.x) > box.half.x + playerRadius() * 0.45f ||
            std::abs(probe.z - box.center.z) > box.half.z + playerRadius() * 0.45f) {
            continue;
        }
        stepTop = std::max(stepTop, top);
    }
    // Continuous terrain follows the height at the new position in moveVertical.
    // Looking ahead here turns every uphill slope into a stair, lifting the actor
    // above the true support surface and continually renewing step grace.
    if (stepTop <= player_.y + 0.015f*bodyScale) return;
    // Refuse the step if the new head position would intersect a ceiling or a tall
    // object. This keeps the auto-step from lifting the player into a wall.
    for (const CharacterBox& box : boxes) {
        const float bottom = box.center.y - box.half.y;
        if (bottom < stepTop + playerHeight() - 0.02f*bodyScale && bottom > stepTop + 0.02f*bodyScale &&
            std::abs(player_.x - box.center.x) < box.half.x + playerRadius() &&
            std::abs(player_.z - box.center.z) < box.half.z + playerRadius()) {
            return;
        }
    }
    player_.y = stepTop + 0.001f*bodyScale;
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    stepGrace_ = std::max(stepGrace_, .25f);
}

void Game::resolveCharacterPenetration() {
    std::vector<CharacterBox> boxes;
    collectCharacterBoxes(boxes);
    for (const CharacterBox& box : boxes) {
        const float left = box.center.x - box.half.x;
        const float right = box.center.x + box.half.x;
        const float front = box.center.z - box.half.z;
        const float back = box.center.z + box.half.z;
        const float bottom = box.center.y - box.half.y;
        const float top = box.center.y + box.half.y;
        const float overlapX = std::min(player_.x + playerRadius(), right) -
                               std::max(player_.x - playerRadius(), left);
        const float overlapZ = std::min(player_.z + playerRadius(), back) -
                               std::max(player_.z - playerRadius(), front);
        const float overlapY = std::min(player_.y + playerHeight(), top) -
                               std::max(player_.y, bottom);
        if (overlapX <= 0.0f || overlapZ <= 0.0f || overlapY <= 0.0f) continue;
        // Vertical resolution takes priority when the player is close to the top:
        // this is what makes a crate or a debris slab a standable surface.
        if (playerStepHeight()>0&&top - player_.y <= playerStepHeight() + 0.04f*playerHeight()/1.8f && verticalVelocity_ <= 0.5f) {
            player_.y = top;
            verticalVelocity_ = 0.0f;
            grounded_ = true;
            continue;
        }
        const float pushRight = right - (player_.x - playerRadius());
        const float pushLeft = (player_.x + playerRadius()) - left;
        const float pushBack = back - (player_.z - playerRadius());
        const float pushFront = (player_.z + playerRadius()) - front;
        const float smallest = std::min(std::min(pushRight, pushLeft),
                                       std::min(pushBack, pushFront));
        const float correction = std::min(smallest + 0.001f, 0.12f);
        if (smallest == pushRight) {
            player_.x += correction;
            pushDynamicBody(box.bodyIndex, {1, 0, 0}, 0.4f);
        } else if (smallest == pushLeft) {
            player_.x -= correction;
            pushDynamicBody(box.bodyIndex, {-1, 0, 0}, 0.4f);
        } else if (smallest == pushBack) {
            player_.z += correction;
            pushDynamicBody(box.bodyIndex, {0, 0, 1}, 0.4f);
        } else {
            player_.z -= correction;
            pushDynamicBody(box.bodyIndex, {0, 0, -1}, 0.4f);
        }
    }
    const float terrain = terrainHeightAt(player_.x, player_.z);
    if (player_.y < terrain) {
        player_.y = terrain;
        verticalVelocity_ = std::max(0.0f, verticalVelocity_);
        grounded_ = true;
    }
}

float Game::terrainHeightAt(float x, float z) const {
    float height = 0.0f;
    bool found = false;
    for (const physics::Heightfield& field : heightfields_) {
        if (!field.valid()) continue;
        height = found ? std::max(height, field.sample(x, z)) : field.sample(x, z);
        found = true;
    }
    return found ? height : 0.0f;
}

void Game::moveHorizontal(float dx, float dz) {
    // Sweep each axis from its previous position rather than testing only the
    // final overlap; even a thin collider cannot be crossed between steps.
    std::vector<CharacterBox> boxes;
    collectCharacterBoxes(boxes);
    float targetX = std::clamp(player_.x + dx, -movementEdge() + playerRadius(),
                              movementEdge() - playerRadius());
    for (const CharacterBox& box : boxes) {
        if (!overlap(player_.y, player_.y + playerHeight(),
                     box.center.y - box.half.y, box.center.y + box.half.y) ||
            !overlap(player_.z - playerRadius(), player_.z + playerRadius(),
                     box.center.z - box.half.z, box.center.z + box.half.z)) {
            continue;
        }
        const float left = box.center.x - box.half.x - playerRadius();
        const float right = box.center.x + box.half.x + playerRadius();
        if (dx > 0.0f && player_.x <= left + kContactEpsilon && targetX > left) {
            targetX = left;
            pushDynamicBody(box.bodyIndex, {1, 0, 0}, 1.4f);
        } else if (dx < 0.0f && player_.x >= right - kContactEpsilon &&
                   targetX < right) {
            targetX = right;
            pushDynamicBody(box.bodyIndex, {-1, 0, 0}, 1.4f);
        }
    }
    player_.x = targetX;
    float targetZ = std::clamp(player_.z + dz, -movementEdge() + playerRadius(),
                              movementEdge() - playerRadius());
    for (const CharacterBox& box : boxes) {
        if (!overlap(player_.y, player_.y + playerHeight(),
                     box.center.y - box.half.y, box.center.y + box.half.y) ||
            !overlap(player_.x - playerRadius(), player_.x + playerRadius(),
                     box.center.x - box.half.x, box.center.x + box.half.x)) {
            continue;
        }
        const float front = box.center.z - box.half.z - playerRadius();
        const float back = box.center.z + box.half.z + playerRadius();
        if (dz > 0.0f && player_.z <= front + kContactEpsilon && targetZ > front) {
            targetZ = front;
            pushDynamicBody(box.bodyIndex, {0, 0, 1}, 1.4f);
        } else if (dz < 0.0f && player_.z >= back - kContactEpsilon &&
                   targetZ < back) {
            targetZ = back;
            pushDynamicBody(box.bodyIndex, {0, 0, -1}, 1.4f);
        }
    }
    player_.z = targetZ;
}

void Game::moveVertical(float dy, float groundSnap) {
    float targetY = player_.y + dy;
    grounded_ = false;
    std::vector<CharacterBox> boxes;
    collectCharacterBoxes(boxes);
    for (const CharacterBox& box : boxes) {
        if (!overlap(player_.x - playerRadius(), player_.x + playerRadius(),
                     box.center.x - box.half.x, box.center.x + box.half.x) ||
            !overlap(player_.z - playerRadius(), player_.z + playerRadius(),
                     box.center.z - box.half.z, box.center.z + box.half.z)) {
            continue;
        }
        const float top = box.center.y + box.half.y;
        const float bottom = box.center.y - box.half.y;
        if (dy < 0.0f && player_.y >= top - kContactEpsilon && targetY < top) {
            targetY = top;
            verticalVelocity_ = 0.0f;
            grounded_ = true;
        } else if (dy > 0.0f && player_.y + playerHeight() <= bottom + kContactEpsilon &&
                   targetY + playerHeight() > bottom) {
            targetY = bottom - playerHeight();
            verticalVelocity_ = 0.0f;
        }
    }
    const float floorY = terrainHeightAt(player_.x, player_.z);
    if (targetY <= floorY || (!grounded_ && dy <= 0.0f && groundSnap > 0.0f &&
                            player_.y >= floorY && player_.y - floorY <= groundSnap)) {
        targetY = floorY;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    }
    player_.y = targetY;
}

// ------------------------------------------------------------------- physics

void Game::initializePhysics() {
    physics_.clear();
    const std::size_t count = scene_.entities.size();
    entityBody_.assign(count, -1);
    fractured_.assign(count, false);
    damage_.assign(count, 0.0f);
    bonded_.assign(count, fracture::Structure{});
    bondedActive_.assign(count, false);
    bondedHoles_ = 0;
    bondedBrokenBonds_ = 0;
    convexMeshes_ = scene_.importedMeshes;
    hullPoints_.clear();
    hullIndices_.clear();
    hullPoints_.resize(convexMeshes_.size());
    hullIndices_.resize(convexMeshes_.size());
    heightfields_.assign(count, physics::Heightfield{});
    destructibleCount_ = 0;
    physics_.setGravity({0.0f, -scene_.gravity, 0.0f});
    physics_.reserve(count + 48, 1024);
    for (std::size_t index = 0; index < count; ++index) {
        const Entity& entity = scene_.entities[index];
        if (!entity.collider) continue;
        if (entity.kind != "ground" && entity.kind != "wall" &&
            entity.kind != "prop") {
            continue;
        }
        physics::BodyDesc desc;
        desc.position = entity.position+characterBodyOffset(static_cast<int>(index));
        desc.halfExtents = {std::abs(entity.scale.x) * 0.5f,
                            std::abs(entity.scale.y) * 0.5f,
                            std::abs(entity.scale.z) * 0.5f};
        desc.dynamic = entity.hasPhysics && entity.dynamicBody &&
                       entity.kind != "ground" && entity.physicsMass > 0.0f;
        desc.mass = std::max(0.05f, entity.physicsMass);
        desc.friction = std::clamp(entity.friction, 0.0f, 1.5f);
        desc.restitution = std::clamp(entity.restitution, 0.0f, 0.95f);
        desc.entity = static_cast<int>(index);
        desc.visible = false;
        if (entity.hasHeightfield && entity.heightfieldEnabled && !entity.heightfieldHeights.empty()) {
            physics::Heightfield& field = heightfields_[index];
            field.originX = entity.heightfieldOrigin[0]; field.originZ = entity.heightfieldOrigin[1];
            field.sizeX = entity.heightfieldSize[0]; field.sizeZ = entity.heightfieldSize[1];
            field.width = entity.heightfieldWidth; field.height = entity.heightfieldHeight;
            field.heights = entity.heightfieldHeights.data();
            field.maxErrorMetres = entity.heightfieldMaxError;
            if (field.valid()) desc.heightfield = &field;
        }
        if (entity.destructible) ++destructibleCount_;
        entityBody_[index] = static_cast<int>(physics_.addBody(desc));
        if (entity.destructible && entity.bonded) {
            const Vec3 half{std::abs(entity.scale.x) * 0.5f,
                            std::abs(entity.scale.y) * 0.5f,
                            std::abs(entity.scale.z) * 0.5f};
            const float volume = std::max(1.0e-4f, 8.0f * half.x * half.y * half.z);
            const float density = std::max(0.05f, entity.physicsMass) / volume;
            // A baked Voronoi package wins when the scene names one: the shard hulls
            // become the visuals and their AABB the physics proxy. Otherwise the uniform
            // grid is used, which is what every pre-bake scene expects.
            const VoronoiPackage* package = findVoronoiPackage(entity.voronoi);
            if (package && adoptVoronoi(index, *package, density)) continue;
            int nx = 1, ny = 1, nz = 1;
            fracture::chooseDivisions(half, entity.bondedCells, nx, ny, nz);
            // A structure that cannot be built leaves the cell list empty, and every
            // bonded path checks for that, so the entity falls back to 0.11.0
            // whole-entity fracture instead of silently doing nothing.
            bonded_[index].buildBox(half, nx, ny, nz, density);
        }
    }
    createJoints();
}

// Resolves authored joints against the rigid bodies that were just built. A joint that
// names a missing entity or a body-less entity is skipped rather than failing the scene:
// the rest of the level is still playable, and the count is reported so an authoring typo
// shows up as "fewer joints than I wrote" instead of a silent no-op.
void Game::createJoints() {
    jointIds_.clear();
    for (const SceneJoint& authored : scene_.joints) {
        int bodyA = -1, bodyB = -1;
        for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
            const Entity& entity = scene_.entities[index];
            if (!authored.bodyA.empty() && entity.id == authored.bodyA) {
                bodyA = entityBody_[index];
            }
            if (!authored.bodyB.empty() && entity.id == authored.bodyB) {
                bodyB = entityBody_[index];
            }
        }
        if (bodyA < 0) continue;
        physics::JointDesc desc;
        if (authored.type == "hinge") desc.type = physics::JointType::Hinge;
        else if (authored.type == "rope") desc.type = physics::JointType::Rope;
        else desc.type = physics::JointType::Point;
        desc.bodyA = static_cast<unsigned>(bodyA);
        // An empty bodyB anchors to the world; anchorA is then a world-space point.
        desc.bodyB = bodyB >= 0 ? static_cast<unsigned>(bodyB) : 0xFFFFFFFFu;
        desc.anchorA = authored.anchorA;
        desc.anchorB = authored.anchorB;
        desc.axis = authored.axis;
        desc.hingeSpan = authored.hingeSpan;
        desc.restLength = authored.restLength;
        desc.compliance = authored.compliance;
        desc.lowerLimit = authored.lowerLimit;
        desc.upperLimit = authored.upperLimit;
        desc.breakForce = authored.breakForce;
        desc.collideConnected = authored.collideConnected;
        desc.motorSpeed = authored.motorSpeed;
        desc.motorMaxTorque = authored.motorMaxTorque;
        desc.friction = authored.friction;
        const unsigned id = physics_.addJoint(desc);
        if (id != 0) jointIds_.push_back(id);
    }
}

const VoronoiPackage* Game::findVoronoiPackage(const std::string& path) const {
    if (path.empty()) return nullptr;
    for (const VoronoiPackage& package : scene_.voronoiPackages) {
        if (package.source == path) return &package;
    }
    return nullptr;
}

// Turns a baked package into the cell/bond graph plus the convex hull table. The mesh
// index is global (one table for the whole scene) so the renderer uploads one pool.
bool Game::adoptVoronoi(std::size_t entity, const VoronoiPackage& package, float density) {
    if (package.empty()) return false;
    std::vector<fracture::Cell> cells;
    cells.reserve(package.shards.size());
    const int meshBase = static_cast<int>(convexMeshes_.size());
    for (std::size_t shard = 0; shard < package.shards.size(); ++shard) {
        const VoronoiShardProxy& proxy = package.shards[shard];
        if (proxy.mesh < 0 || static_cast<std::size_t>(proxy.mesh) >= package.meshes.size()) {
            return false;
        }
        if (package.meshes[static_cast<std::size_t>(proxy.mesh)].empty()) return false;
        fracture::Cell cell;
        cell.center = {proxy.center[0], proxy.center[1], proxy.center[2]};
        cell.half = {proxy.half[0], proxy.half[1], proxy.half[2]};
        cell.volume = proxy.volume;
        cell.mass = std::max(0.01f, proxy.volume * density);
        cell.mesh = meshBase + proxy.mesh;
        if (proxy.meshInterior >= 0 &&
            static_cast<std::size_t>(proxy.meshInterior) < package.meshes.size()) {
            cell.meshInterior = meshBase + proxy.meshInterior;
        }
        cells.push_back(cell);
    }
    std::vector<fracture::Bond> bonds;
    bonds.reserve(package.links.size());
    for (const VoronoiLink& link : package.links) {
        fracture::Bond bond;
        bond.a = link.a;
        bond.b = link.b;
        bond.area = std::max(link.area, 1.0e-6f);
        bonds.push_back(bond);
    }
    if (!bonded_[entity].adopt(std::move(cells), std::move(bonds))) return false;
    for (const ConvexMesh& mesh : package.meshes) {
        convexMeshes_.push_back(mesh);
        // The collision pool mirrors the render table entry by entry so `cell.mesh` indexes
        // both without a second mapping.
        std::vector<Vec3> points;
        points.reserve(mesh.vertices.size());
        for (const ConvexVertex& vertex : mesh.vertices) {
            points.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
        }
        hullPoints_.push_back(std::move(points));
        hullIndices_.push_back(mesh.indices);
    }
    return true;
}

unsigned Game::bakedShardCount() const {
    unsigned count = 0;
    for (const fracture::Structure& structure : bonded_) {
        for (const fracture::Cell& cell : structure.cells()) {
            if (cell.mesh >= 0) ++count;
        }
    }
    return count;
}

int Game::bodyIndexForEntity(int entity) const {
    if (entity < 0 || static_cast<std::size_t>(entity) >= entityBody_.size()) return -1;
    return entityBody_[entity];
}

void Game::fireProjectile() {
    const float s = std::sin(yaw_);
    const float c = std::cos(yaw_);
    const Vec3 target{player_.x + s * 14.0f, player_.y + 1.15f*playerHeight()/1.8f,
                      player_.z - c * 14.0f};
    fireProjectileAt(target, 36.0f);
}

void Game::fireProjectileAt(Vec3 target, float speed) {
    speed = std::clamp(speed, 8.0f, 80.0f);
    Vec3 origin{player_.x, player_.y + 1.35f*playerHeight()/1.8f, player_.z};
    Vec3 toTarget = target - origin;
    const float horizontal = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
    if (horizontal < 0.25f) return;
    const Vec3 horizontalDir{toTarget.x / horizontal, 0.0f, toTarget.z / horizontal};
    origin += horizontalDir * 0.65f;
    const float travelTime = horizontal / speed;
    const float drop = 0.5f * scene_.gravity * travelTime * travelTime;
    const Vec3 velocity{toTarget.x / travelTime,
                        (toTarget.y + drop) / travelTime,
                        toTarget.z / travelTime};
    physics::BodyDesc desc;
    desc.position = origin;
    desc.halfExtents = {0.12f, 0.12f, 0.12f};
    desc.mass = 5.5f;
    desc.friction = 0.45f;
    desc.restitution = 0.16f;
    desc.linearDamping = 0.015f;
    desc.angularDamping = 0.05f;
    desc.dynamic = true;
    desc.projectile = true;
    desc.visible = true;
    desc.velocity = velocity;
    desc.lifetime = 10.0f;
    desc.color[0] = 1.0f; desc.color[1] = 0.48f; desc.color[2] = 0.12f;
    desc.material = materialIndex("metal");
    desc.texture = 2;
    desc.roughness = 0.30f;
    desc.metalness = 0.55f;
    physics_.addBody(desc);
    ++projectileCount_;
}

void Game::detonate(Vec3 center, float radius, float deltaSpeed) {
    radius = std::clamp(radius, 0.5f, 20.0f);
    deltaSpeed = std::clamp(deltaSpeed, 0.5f, 30.0f);
    ++explosionCount_;
    physics_.applyRadialImpulse(center, radius, deltaSpeed);
    bool fracturedSomething = false;
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        const Entity& entity = scene_.entities[index];
        if (!entity.destructible || fractured_[index]) continue;
        const int entityBody = entityBody_[index];
        const physics::Body* body = entityBody >= 0
                                        ? physics_.body(static_cast<unsigned>(entityBody))
                                        : nullptr;
        const Vec3 position = body && body->active ? body->position : entity.position;
        const Vec3 half{entity.scale.x * 0.5f, entity.scale.y * 0.5f,
                        entity.scale.z * 0.5f};
        const Vec3 offset{std::max(std::abs(center.x - position.x) - half.x, 0.0f),
                          std::max(std::abs(center.y - position.y) - half.y, 0.0f),
                          std::max(std::abs(center.z - position.z) - half.z, 0.0f)};
        const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y +
                                         offset.z * offset.z);
        if (distance > radius) continue;
        const float falloff = 1.0f - distance / radius;
        if (entity.bonded && bonded_[index].cellCount() > 0) {
            applyBondedImpact(static_cast<int>(index), center, 42.0f * falloff);
            fracturedSomething = true;
            continue;
        }
        damage_[index] += 1.15f * falloff + 0.25f;
        if (damage_[index] >= 1.0f) {
            fractureEntity(static_cast<int>(index), false, center,
                           entity.strength * falloff * 3.0f);
            fracturedSomething = true;
        }
    }
    if (fracturedSomething) collapseUnsupported();
    feedback_ = "Blast: radial impulse and structural damage applied.";
    feedbackTime_ = 2.0f;
}

void Game::processImpacts() {
    std::vector<physics::Impact> impacts = physics_.takeImpacts();
    bool damagedStructure = false;
    for (const physics::Impact& impact : impacts) {
        ++impactCount_;
        peakImpactImpulse_ = std::max(peakImpactImpulse_, impact.normalImpulse);
        totalImpactEnergy_ += 0.5f * impact.normalImpulse * impact.approachSpeed;
        for (int entity : {impact.entityA, impact.entityB}) {
            if (entity < 0 || static_cast<std::size_t>(entity) >= scene_.entities.size()) continue;
            const Entity& target = scene_.entities[entity];
            if (!target.destructible || fractured_[entity]) continue;
            if (target.bonded && bonded_[entity].cellCount() > 0) {
                const float energy =
                    0.5f * impact.normalImpulse * impact.approachSpeed * 0.02f;
                applyBondedImpact(entity, impact.point, energy);
                continue;
            }
            const float threshold = std::max(3.0f, target.strength * 0.20f);
            if (impact.normalImpulse <= threshold) continue;
            damage_[entity] +=
                (impact.normalImpulse - threshold) / std::max(target.strength * 1.8f, 1.0f);
            if (damage_[entity] >= 1.0f) {
                fractureEntity(entity, false, impact.point, impact.normalImpulse);
                damagedStructure = true;
            } else {
                damagedStructure = true;
            }
        }
    }
    if (damagedStructure) collapseUnsupported();
}

void Game::damageEntity(int entity, float impulse, Vec3 point) {
    if (entity < 0 || static_cast<std::size_t>(entity) >= scene_.entities.size()) return;
    const Entity& target = scene_.entities[entity];
    if (!target.destructible || fractured_[entity]) return;
    if (target.bonded && bonded_[entity].cellCount() > 0) {
        applyBondedImpact(entity, point, impulse * 0.4f);
        return;
    }
    const float threshold = std::max(3.0f, target.strength * 0.20f);
    if (impulse <= threshold) return;
    damage_[entity] += (impulse - threshold) / std::max(target.strength * 1.8f, 1.0f);
    if (damage_[entity] >= 1.0f) {
        fractureEntity(entity, false, point, impulse);
        collapseUnsupported();
    }
}

void Game::fractureEntity(int entity, bool structuralCollapse, Vec3 impulsePoint,
                          float impulseMagnitude) {
    if (entity < 0 || static_cast<std::size_t>(entity) >= scene_.entities.size()) return;
    if (fractured_[entity]) return;
    Entity& source = scene_.entities[entity];
    if (!source.destructible) return;
    fractured_[entity] = true;
    damage_[entity] = 1.0f;

    const int bodyIndex = bodyIndexForEntity(entity);
    Vec3 sourcePosition = source.position;
    physics::Quat sourceOrientation;
    Vec3 sourceVelocity;
    Vec3 sourceAngular;
    Vec3 sourceHalf{source.scale.x * 0.5f, source.scale.y * 0.5f,
                    source.scale.z * 0.5f};
    float sourceMass = std::max(1.0f, source.physicsMass);
    physics::Body* body = bodyIndex >= 0 ? physics_.body(static_cast<unsigned>(bodyIndex))
                                         : nullptr;
    if (body && body->active) {
        sourcePosition = body->position;
        sourceOrientation = body->orientation;
        sourceVelocity = body->velocity;
        sourceAngular = body->angularVelocity;
        sourceHalf = body->half;
        sourceMass = std::max(1.0f, body->mass);
    }
    if (bodyIndex >= 0) physics_.deactivate(static_cast<unsigned>(bodyIndex));

    std::uint32_t seed = source.fractureSeed ^
                         (static_cast<std::uint32_t>(entity) * 2654435761u) ^
                         (static_cast<std::uint32_t>(ticks_) * 2246822519u) ^
                         0x9E3779B9u;
    if (seed == 0) seed = 1;
    const int requested = std::clamp(source.fragments, 4, 28);
    const unsigned currentDebris = physics_.stats().debris;
    const int available = currentDebris >= kPhysicsDebrisCap
                              ? 0
                              : static_cast<int>(kPhysicsDebrisCap - currentDebris);
    const int shardCount = std::min(requested, available);
    const std::vector<ShardPiece> shards =
        shardCount > 0 ? splitBox(sourceHalf, shardCount, seed)
                       : std::vector<ShardPiece>{};
    const float shardMass = std::max(0.22f, sourceMass /
                                                  static_cast<float>(std::max(shardCount, 1)));
    for (const ShardPiece& shard : shards) {
        physics::BodyDesc desc;
        desc.position = sourcePosition + rotate(sourceOrientation, shard.center);
        desc.halfExtents = {std::max(0.025f, shard.half.x),
                            std::max(0.025f, shard.half.y),
                            std::max(0.025f, shard.half.z)};
        desc.orientation = sourceOrientation;
        desc.mass = shardMass;
        desc.friction = source.friction;
        desc.restitution = source.restitution * 0.8f;
        desc.linearDamping = 0.14f;
        desc.angularDamping = 0.28f;
        desc.dynamic = true;
        desc.debris = true;
        desc.visible = true;
        desc.entity = -1;
        desc.lifetime = std::max(4.0f, source.debrisLifetime);
        desc.material = source.materialIndex;
        desc.texture = textureIndex(source.material.pattern);
        desc.roughness = source.hasMaterial ? source.material.roughness : 0.78f;
        desc.metalness = source.hasMaterial ? source.material.metalness : 0.0f;
        const float shade = 0.76f + random01(seed) * 0.18f;
        for (int channel = 0; channel < 3; ++channel) {
            desc.color[channel] = std::clamp(source.color[channel] * shade, 0.0f, 1.0f);
        }
        Vec3 radial = desc.position - impulsePoint;
        float radialLength = std::sqrt(radial.x * radial.x + radial.y * radial.y +
                                       radial.z * radial.z);
        if (radialLength < 0.02f) {
            radial = {random01(seed) - 0.5f, 0.65f, random01(seed) - 0.5f};
            radialLength = std::sqrt(radial.x * radial.x + radial.y * radial.y +
                                     radial.z * radial.z);
        }
        const Vec3 direction = radialLength > 0.0f ? radial / radialLength
                                                   : Vec3{0.0f, 1.0f, 0.0f};
        const float kick = structuralCollapse
                               ? 2.2f + random01(seed) * 2.2f
                               : std::clamp(impulseMagnitude / (sourceMass * 1.8f),
                                            0.8f, 12.0f) *
                                     (0.55f + random01(seed) * 0.8f);
        const Vec3 r = desc.position - sourcePosition;
        desc.velocity = sourceVelocity + cross3(sourceAngular, r) +
                        direction * kick + Vec3{0.0f, structuralCollapse ? -0.35f : 0.9f, 0.0f};
        desc.angularVelocity = sourceAngular +
            Vec3{random01(seed) - 0.5f, random01(seed) - 0.5f, random01(seed) - 0.5f} *
                (2.2f + kick * 0.65f);
        physics_.addBody(desc);
        ++fragmentsSpawned_;
    }
    ++fractureCount_;
    if (structuralCollapse) ++structuralCollapses_;
    feedback_ = structuralCollapse ? "Structural support lost: debris collapsed."
                                   : "Object fractured into rigid debris.";
    feedbackTime_ = 2.2f;
}

bool Game::supportedByScene(int entity, const std::vector<bool>& supported) const {
    if (entity < 0 || static_cast<std::size_t>(entity) >= scene_.entities.size()) return false;
    const Entity& node = scene_.entities[entity];
    const int nodeBody = bodyIndexForEntity(entity);
    const physics::Body* nodeShape = nodeBody >= 0
                                         ? physics_.body(static_cast<unsigned>(nodeBody))
                                         : nullptr;
    const Vec3 nodeCenter = nodeShape && nodeShape->active ? nodeShape->position : node.position;
    const Vec3 nodeHalf = nodeBody >= 0 && nodeShape ? nodeShape->half
                                                     : Vec3{node.scale.x * 0.5f,
                                                            node.scale.y * 0.5f,
                                                            node.scale.z * 0.5f};
    const float bottom = nodeCenter.y - nodeHalf.y;
    for (std::size_t j = 0; j < scene_.entities.size(); ++j) {
        if (static_cast<int>(j) == entity || fractured_[j]) continue;
        const Entity& support = scene_.entities[j];
        if (!support.collider) continue;
        if (support.kind != "ground" && support.kind != "wall" &&
            support.kind != "prop") {
            continue;
        }
        if (support.destructible && support.structural && !supported[j]) continue;
        const int supportBody = bodyIndexForEntity(static_cast<int>(j));
        const physics::Body* shape = supportBody >= 0
                                         ? physics_.body(static_cast<unsigned>(supportBody))
                                         : nullptr;
        const Vec3 supportCenter = shape && shape->active ? shape->position : support.position;
        const Vec3 supportHalf = supportBody >= 0 && shape
                                     ? shape->half
                                     : Vec3{support.scale.x * 0.5f,
                                            support.scale.y * 0.5f,
                                            support.scale.z * 0.5f};
        const float top = supportCenter.y + supportHalf.y;
        if (std::abs(bottom - top) > 0.16f) continue;
        const float overlapX = std::min(nodeCenter.x + nodeHalf.x, supportCenter.x + supportHalf.x) -
                               std::max(nodeCenter.x - nodeHalf.x, supportCenter.x - supportHalf.x);
        const float overlapZ = std::min(nodeCenter.z + nodeHalf.z, supportCenter.z + supportHalf.z) -
                               std::max(nodeCenter.z - nodeHalf.z, supportCenter.z - supportHalf.z);
        if (overlapX > 0.035f && overlapZ > 0.035f) return true;
    }
    return false;
}

void Game::collapseUnsupported() {
    std::vector<int> nodes;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (scene_.entities[i].destructible && scene_.entities[i].structural &&
            !fractured_[i]) {
            nodes.push_back(static_cast<int>(i));
        }
    }
    if (nodes.empty()) return;
    std::vector<bool> supported(scene_.entities.size(), false);
    for (int node : nodes) {
        const Entity& entity = scene_.entities[node];
        const int bodyIndex = bodyIndexForEntity(node);
        const physics::Body* body = bodyIndex >= 0
                                        ? physics_.body(static_cast<unsigned>(bodyIndex))
                                        : nullptr;
        const float centerY = body && body->active ? body->position.y : entity.position.y;
        const float halfY = body && body->active ? body->half.y : entity.scale.y * 0.5f;
        if (centerY - halfY <= 0.14f) supported[node] = true;
    }
    // Propagate support upward until no new node becomes grounded. Nodes resting on
    // non-destructible geometry are accepted immediately.
    for (int pass = 0; pass <= static_cast<int>(nodes.size()); ++pass) {
        bool changed = false;
        for (int node : nodes) {
            if (supported[node]) continue;
            if (supportedByScene(node, supported)) {
                supported[node] = true;
                changed = true;
            }
        }
        if (!changed) break;
    }
    std::vector<int> collapsing;
    for (int node : nodes) {
        if (!supported[node]) collapsing.push_back(node);
    }
    if (collapsing.empty()) return;
    for (int node : collapsing) {
        if (fractured_[node]) continue;
        const Entity& entity = scene_.entities[node];
        const int bodyIndex = bodyIndexForEntity(node);
        const physics::Body* body = bodyIndex >= 0
                                        ? physics_.body(static_cast<unsigned>(bodyIndex))
                                        : nullptr;
        const Vec3 point = body && body->active ? body->position : entity.position;
        fractureEntity(node, true, point, 0.0f);
    }
}

// ------------------------------------------------------- bonded destruction

void Game::applyBondedImpact(int entity, Vec3 point, float energy) {
    if (entity < 0 || static_cast<std::size_t>(entity) >= scene_.entities.size()) return;
    if (energy <= 0.0f) return;
    Entity& target = scene_.entities[entity];
    if (!target.bonded) return;
    fracture::Structure& structure = bonded_[entity];
    if (structure.cellCount() == 0) return;

    const int bodyIndex = bodyIndexForEntity(entity);
    physics::Body* body =
        bodyIndex >= 0 ? physics_.body(static_cast<unsigned>(bodyIndex)) : nullptr;
    Vec3 origin = target.position;
    physics::Quat orientation{};
    if (body && body->active) {
        origin = body->position;
        orientation = body->orientation;
    }
    const physics::Quat inverse{orientation.w, -orientation.x, -orientation.y,
                                -orientation.z};
    const Vec3 local = rotate(inverse, point - origin);
    // Toughness scales with authored strength: a strong wall needs more energy per
    // unit of bond area before it lets go.
    const float toughness = std::max(6.0f, target.strength * 0.6f);
    const int broken = structure.applyEnergy(local, energy, 0.45f, toughness);
    if (broken <= 0) return;
    bondedBrokenBonds_ += static_cast<unsigned>(broken);
    activateBonded(entity, point, energy);
    feedback_ = "Section broke: local hole, the rest of the structure stands.";
    feedbackTime_ = 2.0f;
}

void Game::activateBonded(int entity, Vec3 point, float energy) {
    Entity& target = scene_.entities[entity];
    fracture::Structure& structure = bonded_[entity];
    if (structure.cellCount() == 0) return;

    const int bodyIndex = bodyIndexForEntity(entity);
    physics::Body* source =
        bodyIndex >= 0 ? physics_.body(static_cast<unsigned>(bodyIndex)) : nullptr;
    Vec3 origin = target.position;
    physics::Quat orientation{};
    Vec3 inherited;
    if (source && source->active) {
        origin = source->position;
        orientation = source->orientation;
        inherited = source->velocity;
    }

    if (!bondedActive_[entity]) {
        bondedActive_[entity] = true;
        if (bodyIndex >= 0) physics_.deactivate(static_cast<unsigned>(bodyIndex));
        for (fracture::Cell& cell : structure.cells()) {
            physics::BodyDesc desc;
            desc.position = origin + rotate(orientation, cell.center);
            desc.orientation = orientation;
            desc.halfExtents = {std::max(0.02f, cell.half.x),
                                std::max(0.02f, cell.half.y),
                                std::max(0.02f, cell.half.z)};
            // A baked shard collides as its hull. The box stays as the fallback shape for
            // the broadphase and for any cell that has no hull.
            if (cell.mesh >= 0 && static_cast<std::size_t>(cell.mesh) < hullPoints_.size() &&
                !hullPoints_[static_cast<std::size_t>(cell.mesh)].empty()) {
                const std::size_t hull = static_cast<std::size_t>(cell.mesh);
                desc.hullPoints = hullPoints_[hull].data();
                desc.hullPointCount = static_cast<int>(hullPoints_[hull].size());
                if (hull < hullIndices_.size() && !hullIndices_[hull].empty()) {
                    desc.hullIndices = hullIndices_[hull].data();
                    desc.hullIndexCount = static_cast<int>(hullIndices_[hull].size());
                }
            }
            desc.mass = std::max(0.05f, cell.mass);
            desc.dynamic = false;
            desc.friction = target.friction;
            desc.restitution = target.restitution * 0.4f;
            desc.entity = -1;
            desc.visible = false;
            desc.material = target.materialIndex;
            desc.texture = textureIndex(target.material.pattern);
            desc.roughness = target.hasMaterial ? target.material.roughness : 0.78f;
            desc.metalness = target.hasMaterial ? target.material.metalness : 0.0f;
            for (int channel = 0; channel < 3; ++channel) {
                desc.color[channel] = target.color[channel];
            }
            cell.body = static_cast<int>(physics_.addBody(desc));
        }
    }

    // Support pass. The largest island is the standing structure; any other island
    // only stays put if one of its cells still rests on the ground.
    const std::vector<fracture::Island> islands = structure.islands();
    const int largest = structure.largestIsland();
    for (std::size_t islandIndex = 0; islandIndex < islands.size(); ++islandIndex) {
        const fracture::Island& island = islands[islandIndex];
        bool allDetached = true;
        for (int cellIndex : island.cells) {
            if (!structure.cells()[static_cast<std::size_t>(cellIndex)].detached) {
                allDetached = false;
                break;
            }
        }
        if (allDetached) continue;
        bool supported = static_cast<int>(islandIndex) == largest;
        if (!supported) {
            for (int cellIndex : island.cells) {
                const fracture::Cell& cell = structure.cells()[static_cast<std::size_t>(cellIndex)];
                if (cell.body < 0) continue;
                const physics::Body* cellBody =
                    physics_.body(static_cast<unsigned>(cell.body));
                if (cellBody && cellBody->position.y - cellBody->half.y <= 0.16f) {
                    supported = true;
                    break;
                }
            }
        }
        if (supported) continue;

        for (int cellIndex : island.cells) {
            fracture::Cell& cell = structure.cells()[static_cast<std::size_t>(cellIndex)];
            if (cell.body < 0) continue;
            const physics::Body* previous = physics_.body(static_cast<unsigned>(cell.body));
            if (!previous) continue;
            physics::BodyDesc desc;
            desc.position = previous->position;
            desc.orientation = previous->orientation;
            desc.halfExtents = previous->half;
            // Detaching a baked shard retains its hull when replacing the pinned
            // body with a dynamic one. Saves reconstruct the same immutable hull.
            desc.hullPoints=previous->hullPoints;desc.hullPointCount=previous->hullPointCount;
            desc.hullIndices=previous->hullIndices;desc.hullIndexCount=previous->hullIndexCount;
            desc.mass = std::max(0.05f, cell.mass);
            desc.dynamic = true;
            desc.debris = true;
            desc.friction = target.friction;
            desc.restitution = target.restitution * 0.4f;
            desc.entity = -1;
            desc.visible = false;
            desc.lifetime = std::max(4.0f, target.debrisLifetime);
            desc.material = target.materialIndex;
            desc.texture = textureIndex(target.material.pattern);
            desc.roughness = target.hasMaterial ? target.material.roughness : 0.78f;
            desc.metalness = target.hasMaterial ? target.material.metalness : 0.0f;
            for (int channel = 0; channel < 3; ++channel) {
                desc.color[channel] = target.color[channel];
            }
            Vec3 radial = previous->position - point;
            const float distance = std::sqrt(radial.x * radial.x + radial.y * radial.y +
                                             radial.z * radial.z);
            const Vec3 direction =
                distance > 0.02f ? radial / distance : Vec3{0.0f, 1.0f, 0.0f};
            desc.velocity = inherited + direction * (1.0f + energy * 0.01f) +
                            Vec3{0.0f, 0.4f, 0.0f};
            desc.angularVelocity = {0.7f, 1.1f, 0.5f};
            physics_.deactivate(static_cast<unsigned>(cell.body));
            cell.body = static_cast<int>(physics_.addBody(desc));
            cell.detached = true;
            ++fragmentsSpawned_;
        }
        ++bondedHoles_;
    }
}

Pose Game::poseForEntityBody(int entity, const physics::Body& body) const {
    Pose pose;
    pose.position = body.position-characterBodyOffset(entity,body.orientation);
    pose.scale = {body.half.x * 2.0f, body.half.y * 2.0f, body.half.z * 2.0f};
    physics::toEuler(body.orientation, pose.yaw, pose.pitch, pose.roll);
    const Entity& source = scene_.entities[entity];
    pose.id = source.id;
    pose.mesh = source.importedMesh;
    const float tint = 1.0f - 0.30f * std::min(1.0f, damage_[entity]);
    for (int channel = 0; channel < 3; ++channel) {
        pose.color[channel] = std::clamp(source.color[channel] * tint, 0.0f, 1.0f);
    }
    pose.roughness = source.hasMaterial ? source.material.roughness : 0.72f;
    pose.metalness = source.hasMaterial ? source.material.metalness : 0.0f;
    if (source.hasMaterial) pose.texture = textureIndex(source.material.pattern);
    pose.material = source.materialIndex;
    pose.rayVisibilityMask = 2;
    return pose;
}

void Game::appendRuntimeBodyPoses(std::vector<Pose>& poses) const {
    for (const physics::Body& body : physics_.bodies()) {
        if (!body.active || !body.visible || body.entity >= 0) continue;
        Pose pose;
        pose.position = body.position;
        pose.scale = {body.half.x * 2.0f, body.half.y * 2.0f, body.half.z * 2.0f};
        physics::toEuler(body.orientation, pose.yaw, pose.pitch, pose.roll);
        const float fade = body.lifetime > 0.0f && body.lifetime < 2.0f
                               ? std::max(0.15f, body.lifetime * 0.5f)
                               : 1.0f;
        for (int channel = 0; channel < 3; ++channel) {
            pose.color[channel] = std::clamp(body.color[channel] * fade, 0.0f, 1.0f);
        }
        pose.roughness = body.roughness;
        pose.metalness = body.metalness;
        pose.texture = body.texture;
        pose.material = body.material;
        pose.rayVisibilityMask = 2;
        poses.push_back(pose);
    }
}

std::string Game::status() const {
    if (complete_) return "Complete! Press R to restart the playtest.";
    if (feedbackTime_ > 0.0f) return feedback_;
    if(std::none_of(scene_.entities.begin(),scene_.entities.end(),[](const Entity& entity){return entity.kind=="key"||entity.kind=="door"||entity.kind=="goal";}))
        return "WASD: walk | Shift: run | Space: jump | RMB drag: orbit | F5: reset";
    if (!hasKey_) {
        const Vec3 key = location("key", {4.0f, 0.8f, 1.0f});
        if (distanceSquaredXZ(player_, key) <= 1.65f * 1.65f) {
            return "Press E to collect the gold key.";
        }
        return "Find the gold key in the courtyard.";
    }
    if (!doorOpen_) {
        const Vec3 door = location("door", {0.0f, 1.5f, -8.0f});
        if (distanceSquaredXZ(player_, door) <= 2.2f * 2.2f) {
            return "Press E to unlock the gate.";
        }
        return "Bring the key to the gate.";
    }
    return "Walk through the gate to the green exit.";
}

int Game::materialIndex(const std::string& name) const {
    for (std::size_t index = 0; index < scene_.materials.size(); ++index) {
        if (scene_.materials[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

void Game::addVehiclePoses(const Entity& entity, std::vector<Pose>& poses) const {
    const float direction = entity.vehicleDirection >= 0 ? 1.0f : -1.0f;
    const int bodyMaterial = entity.materialIndex;
    const int glassMaterial = materialIndex("glass") >= 0 ? materialIndex("glass") : bodyMaterial;
    const int lampMaterial = materialIndex("window") >= 0 ? materialIndex("window") : bodyMaterial;
    const auto add = [&](Vec3 offset, Vec3 scale, const float* color, int material,
                         float roughness, float metalness) {
        Pose pose;
        pose.id = entity.id;
        pose.position = {entity.position.x + offset.x,
                         entity.position.y + offset.y,
                         entity.position.z + offset.z};
        pose.scale = scale;
        std::copy(color, color + 3, pose.color);
        pose.roughness = roughness;
        pose.metalness = metalness;
        pose.material = material;
        pose.rayVisibilityMask = 2;
        poses.push_back(pose);
    };
    const float bodyColor[3] = {entity.color[0], entity.color[1], entity.color[2]};
    const float dark[3] = {0.06f, 0.07f, 0.08f};
    const float warm[3] = {1.0f, 0.82f, 0.55f};
    const float red[3] = {1.0f, 0.08f, 0.04f};
    // Lower body and cabin.
    add({0.0f, 0.0f, 0.0f}, {entity.scale.x, entity.scale.y * 0.58f, entity.scale.z},
        bodyColor, bodyMaterial, 0.35f, 0.75f);
    add({0.0f, entity.scale.y * 0.46f, -0.25f * direction},
        {entity.scale.x * 0.82f, entity.scale.y * 0.42f, entity.scale.z * 0.48f},
        bodyColor, glassMaterial, 0.18f, 0.0f);
    // Four wheels.
    for (const float side : {-1.0f, 1.0f}) {
        for (const float axle : {-1.0f, 1.0f}) {
            add({side * entity.scale.x * 0.48f, -entity.scale.y * 0.42f,
                 axle * entity.scale.z * 0.32f},
                {0.28f, 0.52f, 0.52f}, dark, bodyMaterial, 0.85f, 0.0f);
        }
    }
    // Headlights and taillights are emissive plates; the actual spot lights come from
    // dynamicLights() so the traffic lights the road, not just its own body.
    for (const float side : {-0.55f, 0.55f}) {
        add({side, 0.02f, direction * entity.scale.z * 0.5f},
            {0.30f, 0.16f, 0.10f}, warm, lampMaterial, 0.2f, 0.0f);
        poses.back().rayVisibilityMask=4;
        add({side, 0.02f, -direction * entity.scale.z * 0.5f},
            {0.28f, 0.14f, 0.10f}, red, lampMaterial, 0.2f, 0.0f);
        poses.back().rayVisibilityMask=4;
    }
}

std::vector<Light> Game::dynamicLights() const {
    std::vector<Light> lights;
    for(auto h:gameplay_.world.query("illumination")) {
        const auto& value=*gameplay_.world.get(h,"illumination");
        const auto* transform=gameplay_.world.get(h,"transform");
        if(!transform||!value.at("enabled").get<bool>())continue;
        Light light;const auto& p=transform->at("position");light.position={p[0],p[1],p[2]};
        for(int i=0;i<3;++i)light.color[i]=value.at("color")[i];
        light.intensity=value.at("intensity");light.range=value.at("range");lights.push_back(light);
    }
    for (const Entity& entity : scene_.entities) {
        if (entity.kind != "vehicle") continue;
        const float direction = entity.vehicleDirection >= 0 ? 1.0f : -1.0f;
        for (const float side : {-0.55f, 0.55f}) {
            Light headlight;
            headlight.position = {entity.position.x + side,
                                  entity.position.y + 0.02f,
                                  entity.position.z + direction * entity.scale.z * 0.5f};
            headlight.direction = {0.0f, -0.08f, direction};
            headlight.color[0] = 1.0f;
            headlight.color[1] = 0.66f;
            headlight.color[2] = 0.36f;
            headlight.intensity = 10.0f;
            headlight.range = 20.0f;
            headlight.type = 1;
            headlight.cosOuter = 0.52f;
            headlight.cosInner = 0.90f;
            lights.push_back(headlight);
        }
        Light taillight;
        taillight.position = {entity.position.x,
                              entity.position.y + 0.05f,
                              entity.position.z - direction * entity.scale.z * 0.5f};
        taillight.color[0] = 1.0f;
        taillight.color[1] = 0.05f;
        taillight.color[2] = 0.02f;
        taillight.intensity = 1.2f;
        taillight.range = 5.0f;
        taillight.type = 0;
        lights.push_back(taillight);
    }
    return lights;
}

std::vector<CharacterPose> Game::renderCharacters() const {
    std::vector<CharacterPose> output;output.reserve(scene_.entities.size());
    for(std::size_t index=0;index<scene_.entities.size();++index) {
        const auto& entity=scene_.entities[index];if(entity.characterAsset<0||static_cast<std::size_t>(entity.characterAsset)>=scene_.characterAssets.size())continue;
        if(entity.kind=="player"&&firstPerson())continue;
        const auto handle=gameplay_.world.find(entity.id);if(!gameplay_.world.valid(handle))continue;
        const auto* renderable=gameplay_.world.get(handle,"renderable");if(renderable&&!renderable->value("enabled",true))continue;
        const auto& ref=entity.character;CharacterPose character;character.entityId=entity.id;character.asset=scene_.characterAssets[static_cast<std::size_t>(entity.characterAsset)].get();character.scale=ref.scale;character.materials=ref.materials;
        character.position=entity.kind=="player"?Vec3{player_.x,visualY_,player_.z}:entity.position;
        character.yaw=entity.kind=="player"?bodyYaw_:0;
        if(entity.kind!="player")if(const auto* transform=gameplay_.world.get(handle,"transform"))character.yaw=transform->value("yaw",0.f);
        const auto bodyIndex=bodyIndexForEntity(static_cast<int>(index));if(entity.kind!="player"&&bodyIndex>=0)if(const auto* body=physics_.body(static_cast<unsigned>(bodyIndex));body&&body->active&&body->dynamic){const auto pose=poseForEntityBody(static_cast<int>(index),*body);character.position=pose.position;character.yaw=pose.yaw;character.pitch=pose.pitch;character.roll=pose.roll;}
        if(renderable){const auto& offset=renderable->at("offset");character.position.x+=offset[0].get<float>();character.position.y+=offset[1].get<float>();character.position.z+=offset[2].get<float>();}
        character.yaw+=ref.yaw;const auto model=compose(character.position,ref.scale,character.yaw,character.pitch,character.roll);
        character.position={model.m[12]+model.m[0]*ref.offset.x+model.m[4]*ref.offset.y+model.m[8]*ref.offset.z,model.m[13]+model.m[1]*ref.offset.x+model.m[5]*ref.offset.y+model.m[9]*ref.offset.z,model.m[14]+model.m[2]*ref.offset.x+model.m[6]*ref.offset.y+model.m[10]*ref.offset.z};
        std::copy(entity.color,entity.color+3,character.color);
        if(entity.kind=="player"&&ref.clip.empty()&&animation_.asset()) {
            character.pose=animation_.pose();character.clip=animation_.asset()->clips[animation_.selection().clip].name;
            character.morphWeights=animation_.featureState().morphWeights;character.deformation=&animation_.deformation();
        }else {
            character.clip=ref.clip.empty()?character.asset->controllerClips[0]:ref.clip;
            const auto& clip=character.asset->clips[character.asset->clipIndex(character.clip)];character.pose=clip.sample(double(ticks_)/animation::TickRate*ref.speed+ref.timeOffset);
            if(ref.rootMotion=="extract"||(ref.rootMotion=="asset"&&!character.asset->preserveRootTranslation))character.pose.translation[0]=character.asset->skeleton.restPose().translation[0];
        }
        if(!character.deformation)for(unsigned i=0;i<animation::MaxMorphTargets;++i)character.morphWeights[i]=character.pose.curves[animation::MorphCurveBase+i];
        character.palette=character.asset->skeleton.palette(character.pose);output.push_back(std::move(character));
    }
    return output;
}

std::vector<Pose> Game::renderPoses() const {
    std::vector<Pose> poses;
    poses.reserve(scene_.entities.size() + physics_.bodyCount() + 24);
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        const Entity& entity = scene_.entities[index];
        if (entity.kind == "player"||entity.characterAsset>=0) continue;
        const auto gameplayHandle=gameplay_.world.find(entity.id);
        if(!gameplay_.world.valid(gameplayHandle))continue;
        const auto* renderable=gameplay_.world.valid(gameplayHandle)?gameplay_.world.get(gameplayHandle,"renderable"):nullptr;
        if(renderable&&!renderable->at("enabled").get<bool>())continue;
        if (entity.kind == "vehicle") {
            addVehiclePoses(entity, poses);
            continue;
        }
        if (fractured_[index]) continue;
        if (bondedActive_[index]) {
            const fracture::Structure& structure = bonded_[index];
            const float tint = 1.0f - 0.30f * std::min(1.0f, damage_[index]);
            for (const fracture::Cell& cell : structure.cells()) {
                if (cell.body < 0) continue;
                const physics::Body* cellBody =
                    physics_.body(static_cast<unsigned>(cell.body));
                if (!cellBody || !cellBody->active) continue;
                Pose cellPose;
                cellPose.id = entity.id;
                cellPose.position = cellBody->position;
                cellPose.scale = {cellBody->half.x * 2.0f, cellBody->half.y * 2.0f,
                                  cellBody->half.z * 2.0f};
                // A baked shard draws its real hull; the box is only the physics proxy.
                // The mesh is body-local, so `scale` must stay 1 for the renderer.
                if (cell.mesh >= 0 &&
                    static_cast<std::size_t>(cell.mesh) < convexMeshes_.size()) {
                    cellPose.mesh = cell.mesh;
                    cellPose.scale = {1.0f, 1.0f, 1.0f};
                }
                physics::toEuler(cellBody->orientation, cellPose.yaw, cellPose.pitch,
                                 cellPose.roll);
                for (int channel = 0; channel < 3; ++channel) {
                    cellPose.color[channel] =
                        std::clamp(entity.color[channel] * tint, 0.0f, 1.0f);
                }
                cellPose.rayVisibilityMask = 2;
                cellPose.material = entity.materialIndex;
                if (entity.hasMaterial) {
                    cellPose.roughness = entity.material.roughness;
                    cellPose.metalness = entity.material.metalness;
                    cellPose.texture = textureIndex(entity.material.pattern);
                }
                poses.push_back(cellPose);
                // Cut faces are drawn as a second pose over the same body. They are the
                // raw interior of the prop, so they take a darker, rougher tint than the
                // authored surface instead of the brick pattern.
                if (cell.meshInterior >= 0 &&
                    static_cast<std::size_t>(cell.meshInterior) < convexMeshes_.size()) {
                    Pose interiorPose = cellPose;
                    interiorPose.mesh = cell.meshInterior;
                    interiorPose.scale = {1.0f, 1.0f, 1.0f};
                    for (int channel = 0; channel < 3; ++channel) {
                        interiorPose.color[channel] =
                            std::clamp(cellPose.color[channel] * 0.55f, 0.0f, 1.0f);
                    }
                    interiorPose.roughness = std::min(1.0f, cellPose.roughness + 0.2f);
                    interiorPose.metalness = cellPose.metalness * 0.35f;
                    poses.push_back(interiorPose);
                }
            }
            continue;
        }
        const int bodyIndex = bodyIndexForEntity(static_cast<int>(index));
        const physics::Body* body = bodyIndex >= 0
                                        ? physics_.body(static_cast<unsigned>(bodyIndex))
                                        : nullptr;
        if (body && body->active && body->dynamic) {
            auto pose=poseForEntityBody(static_cast<int>(index), *body);
            pose.id = entity.id;
            if(renderable){const auto& offset=renderable->at("offset");pose.position.x+=offset[0].get<float>();pose.position.y+=offset[1].get<float>();pose.position.z+=offset[2].get<float>();}
            poses.push_back(pose);
            continue;
        }
        Pose pose;
        pose.id = entity.id;
        pose.position = entity.position;
        pose.rayVisibilityMask = entity.kind=="light" ? 4u : entity.kind=="door" ? 2u : 1u;
        pose.scale = entity.scale;
        pose.mesh = entity.importedMesh;
        if(const auto* transform=gameplay_.world.get(gameplayHandle,"transform"))pose.yaw=transform->at("yaw");
        const float tint = entity.destructible
                               ? 1.0f - 0.30f * std::min(1.0f, damage_[index])
                               : 1.0f;
        for (int channel = 0; channel < 3; ++channel) {
            pose.color[channel] = std::clamp(entity.color[channel] * tint, 0.0f, 1.0f);
        }
        if (entity.hasMaterial) {
            pose.roughness = entity.material.roughness;
            pose.metalness = entity.material.metalness;
            pose.texture = textureIndex(entity.material.pattern);
            pose.material = entity.materialIndex;
        }
        if(renderable) {
            const auto& offset=renderable->at("offset");
            pose.position.x+=offset[0].get<float>();pose.position.y+=offset[1].get<float>();pose.position.z+=offset[2].get<float>();
        }
        poses.push_back(pose);
    }
    appendRuntimeBodyPoses(poses);

    std::set<std::string> authoredIds;for(const auto& e:scene_.entities)authoredIds.insert(e.id);
    for(auto h:gameplay_.world.query("renderable")) {
        const auto& record=gameplay_.world.entity(h);
        if(authoredIds.count(record.id))continue;
        const auto* transform=gameplay_.world.get(h,"transform");const auto& render=*gameplay_.world.get(h,"renderable");
        if(!transform||!render.at("enabled").get<bool>())continue;
        Pose pose;const auto& position=transform->at("position");const auto& scale=transform->at("scale");const auto& offset=render.at("offset");
        pose.id=record.id;
        pose.position={position[0].get<float>()+offset[0].get<float>(),position[1].get<float>()+offset[1].get<float>(),position[2].get<float>()+offset[2].get<float>()};
        pose.scale={scale[0],scale[1],scale[2]};pose.yaw=transform->at("yaw");
        if(runtimeBodies_.count(record.id))if(const auto* body=physics_.body(static_cast<unsigned>(runtimeBodies_.at(record.id)));body&&body->active)physics::toEuler(body->orientation,pose.yaw,pose.pitch,pose.roll);
        poses.push_back(pose);
    }

    if(firstPerson()) return poses;
    if(animation_.asset()) {
        if(hasKey_&&!doorOpen_) {
            // Attach the carried key to the evaluated hand instead of a second motion path.
            const auto& skeleton=animation_.asset()->skeleton;
            auto pose=animation_.pose();Vec3 origin=player_,modelScale{1,1,1};float modelYaw=yaw_;
            for(const auto& character:renderCharacters())if(character.entityId==playerEntityId()){pose=character.pose;origin=character.position;modelScale=character.scale;modelYaw=character.yaw;break;}
            auto globals=skeleton.globals(pose);
            unsigned hand=0;
            for(unsigned b=0;b<skeleton.bones.size();++b)if(skeleton.bones[b].name==animation_.asset()->rightHandBone)hand=b;
            auto local=animation::point(globals[hand],{0.10f,0,-0.03f});
            Pose key;key.rayVisibilityMask=2;const float cy=std::cos(modelYaw),sy=std::sin(modelYaw);
            key.position={origin.x+cy*local.x*modelScale.x-sy*local.z*modelScale.z,origin.y+local.y*modelScale.y,origin.z+sy*local.x*modelScale.x+cy*local.z*modelScale.z};
            key.scale={0.06f,0.25f,0.06f};key.color[0]=1;key.color[1]=0.76f;key.color[2]=0.10f;key.yaw=yaw_;poses.push_back(key);
        }
        return poses;
    }
    const float skin[3] = {0.96f, 0.72f, 0.48f};
    const float shirt[3] = {0.05f, 0.67f, 0.81f};
    const float trousers[3] = {0.13f, 0.24f, 0.36f};
    const float shoes[3] = {0.06f, 0.09f, 0.14f};
    const float eyeColor[3] = {0.08f, 0.10f, 0.14f};
    const float activity = std::clamp(animationSpeed_ / scene_.runSpeed, 0.0f, 1.0f);
    const float swing = std::sin(animationPhase_);
    const float bob = grounded_ ? std::abs(swing) * activity * 0.035f : 0.03f;
    const float cy = std::cos(yaw_);
    const float sy = std::sin(yaw_);
    auto world = [&](Vec3 local) {
        return Vec3{player_.x + cy * local.x - sy * local.z,
                    player_.y + local.y + bob,
                    player_.z + sy * local.x + cy * local.z};
    };
    auto cube = [&](Vec3 local, Vec3 scale, const float* color, float pitch = 0.0f) {
        Pose pose;
        pose.position = world(local);
        pose.rayVisibilityMask = 2;
        pose.scale = scale;
        std::copy(color, color + 3, pose.color);
        pose.roughness = 0.55f;
        pose.metalness = 0.12f;
        pose.texture = 2;
        pose.yaw = yaw_;
        pose.pitch = pitch;
        poses.push_back(pose);
    };
    auto limb = [&](Vec3 joint, float length, float width, float angle,
                    const float* color) {
        const Vec3 end{joint.x, joint.y - std::cos(angle) * length,
                       joint.z - std::sin(angle) * length};
        cube({(joint.x + end.x) * 0.5f, (joint.y + end.y) * 0.5f,
              (joint.z + end.z) * 0.5f},
             {width, length, width}, color, angle);
        return end;
    };
    cube({0.0f, 1.13f, 0.0f}, {0.62f, 0.60f, 0.32f}, shirt);
    cube({0.0f, 0.77f, 0.0f}, {0.48f, 0.20f, 0.30f}, trousers);
    cube({0.0f, 1.59f, -0.01f}, {0.38f, 0.39f, 0.35f}, skin);
    cube({-0.085f, 1.63f, -0.191f}, {0.055f, 0.055f, 0.025f}, eyeColor);
    cube({0.085f, 1.63f, -0.191f}, {0.055f, 0.055f, 0.025f}, eyeColor);
    for (int side : {-1, 1}) {
        const float sideFloat = static_cast<float>(side);
        const float phase = swing * sideFloat;
        const float thighAngle = grounded_ ? phase * activity * 0.70f : -0.40f;
        const float calfAngle = grounded_
            ? thighAngle - std::max(0.0f, phase) * activity * 0.70f
            : 0.70f;
        Vec3 knee = limb({sideFloat * 0.15f, 0.74f, 0.0f},
                         0.34f, 0.19f, thighAngle, trousers);
        Vec3 ankle = limb(knee, 0.32f, 0.17f, calfAngle, trousers);
        cube({ankle.x, ankle.y - 0.025f, ankle.z - 0.06f},
             {0.21f, 0.13f, 0.32f}, shoes);
        const float upperArmAngle = grounded_ ? -phase * activity * 0.75f : -0.65f;
        const float lowerArmAngle = upperArmAngle + (grounded_ ? -0.20f : -0.60f);
        Vec3 elbow = limb({sideFloat * 0.40f, 1.41f, 0.0f},
                          0.30f, 0.18f, upperArmAngle, shirt);
        Vec3 wrist = limb(elbow, 0.26f, 0.15f, lowerArmAngle, skin);
        cube(wrist, {0.17f, 0.17f, 0.17f}, skin);
        if (side == 1 && hasKey_ && !doorOpen_) {
            const float gold[3] = {1.0f, 0.76f, 0.10f};
            cube({wrist.x + 0.10f, wrist.y, wrist.z - 0.03f},
                 {0.06f, 0.25f, 0.06f}, gold);
        }
    }
    return poses;
}

physics::Stats Game::physicsStats() const {
    return physics_.stats();
}

unsigned Game::intactDestructibleCount() const {
    unsigned result = 0;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (scene_.entities[i].destructible && !fractured_[i]) ++result;
    }
    return result;
}

void Game::setWater(std::shared_ptr<water::Body> body,
                    const water::CouplingSettings& coupling,
                    const water::WadingSettings& wading,
                    const water::FootprintSettings& footprint) {
    water_ = std::move(body);
    waterCoupling_ = coupling;
    wadingSettings_ = wading;
    footprintSettings_ = footprint;
    footprintState_ = {};
    footWaterState_ = {};
    bodyWaterVolume_.assign(physics_.bodyCount(), 0.0f);
    waterDisturbances_ = 0;
    waterMaxImmersion_ = 0.0f;
    wading_ = {};
    if (water_) water_->reset();
}

void Game::clearWater() {
    water_.reset();
    footprintState_ = {};
    footWaterState_ = {};
    bodyWaterVolume_.clear();
    waterDisturbances_ = 0;
    waterMaxImmersion_ = 0.0f;
    wading_ = {};
}

water::Query Game::queryWater(float x, float z) const {
    return water_ ? water_->query(x, z) : water::Query{};
}

water::Diagnostics Game::waterDiagnostics() const {
    return water_ ? water_->diagnostics() : water::Diagnostics{};
}

const std::vector<water::InteractionEvent>& Game::waterEvents() const {
    static const std::vector<water::InteractionEvent> empty;
    return water_ ? water_->events() : empty;
}

void Game::updateWadingState() {
    wading_ = {};
    if (!water_) return;
    wading_ = water::wadingState(*water_, player_, playerHeight(), wadingSettings_);
    waterMaxImmersion_ = std::max(waterMaxImmersion_, wading_.immersionMetres);
}

void Game::applyWaterCoupling(float dtSeconds) {
    if (!water_) return;
    const auto& bodies = physics_.bodies();
    if (bodyWaterVolume_.size() != bodies.size()) bodyWaterVolume_.assign(bodies.size(), 0.0f);
    const Vec3 gravity = physics_.gravity();
    for (std::size_t index = 0; index < bodies.size(); ++index) {
        const physics::Body& body = bodies[index];
        if (!body.active || !body.dynamic) {
            bodyWaterVolume_[index] = 0.0f;
            continue;
        }
        water::CouplingInput input;
        input.centre = body.position;
        input.halfExtents = body.half;
        input.axisX = physics::rotate(body.orientation, {1, 0, 0});
        input.axisY = physics::rotate(body.orientation, {0, 1, 0});
        input.axisZ = physics::rotate(body.orientation, {0, 0, 1});
        input.velocity = body.velocity;
        input.angularVelocity = body.angularVelocity;
        input.massKg = body.mass;
        input.previousSubmergedVolume = bodyWaterVolume_[index];
        const water::CouplingResult coupled =
            water::coupleBox(*water_, input, gravity, waterCoupling_);
        bodyWaterVolume_[index] = coupled.submergedVolume;
        if (!coupled.valid) continue;
        if (body.sleeping) {
            // A floating body may settle and sleep. Wake it only when the load or the
            // displaced volume changed enough that the old equilibrium is gone.
            if (!coupled.wake) continue;
            physics_.wake(static_cast<unsigned>(index));
        }
        physics_.addForceAtPoint(static_cast<unsigned>(index), coupled.force, coupled.centre);
        physics_.addTorque(static_cast<unsigned>(index), coupled.torque);
        // Newton's third law for the horizontal exchange. The drag above is applied to
        // the body as a force for this tick, so the same tick's integrated impulse is
        // returned to the water. Vertical buoyancy is a pressure effect and is not
        // rewritten into horizontal momentum.
        const float reactionRadius = std::max(0.1f,
            std::max(input.halfExtents.x, input.halfExtents.z));
        water_->applyImpulse(coupled.centre.x, coupled.centre.z, reactionRadius,
                             -coupled.force.x * dtSeconds, -coupled.force.z * dtSeconds);
    }
}

void Game::digest(determinism::Hasher& hasher) const {
    // /4 adds joint state (broken, angle, motor setpoint).
    hasher.text("nativegpu.game/4");
    hasher.text(gameplay_.digest());
    hasher.text(gameplay::fingerprint(publishedTransforms_));
    for(const auto& [id,index]:runtimeBodies_){hasher.text(id);hasher.u32(static_cast<unsigned>(index));}
    for(const auto& [id,v]:publishedVelocity_){hasher.text(id);hasher.quantized(v.x);hasher.quantized(v.y);hasher.quantized(v.z);}
    hasher.u64(ticks_);
    hasher.quantized(player_.x);
    hasher.quantized(player_.y);
    hasher.quantized(player_.z);
    hasher.quantized(verticalVelocity_);
    hasher.quantized(yaw_);
    hasher.quantized(bodyYaw_);
    hasher.quantized(visualY_);
    hasher.quantized(stepGrace_);
    hasher.quantized(pitch_);
    hasher.quantized(animationPhase_);
    hasher.quantized(animationSpeed_);
    animation_.digest(hasher);
    hasher.u64(animationEvents_.size());
    for(const auto& event:animationEvents_) { hasher.u64(event.tick);hasher.text(event.name);hasher.u32(event.clip); }
    hasher.quantized(feedbackTime_);
    hasher.u8(static_cast<std::uint8_t>(grounded_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(hasKey_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(doorOpen_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(complete_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(jumpPending_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(interactPending_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(firePending_ ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(blastPending_ ? 1 : 0));
    hasher.text("nativgpu.physics/2");
    hasher.u64(fractureCount_);
    hasher.u64(fragmentsSpawned_);
    hasher.u64(structuralCollapses_);
    hasher.u64(projectileCount_);
    hasher.u64(explosionCount_);
    hasher.u64(impactCount_);
    hasher.u64(bondedHoles_);
    hasher.u64(bondedBrokenBonds_);
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        if (!scene_.entities[index].bonded) continue;
        hasher.u32(static_cast<unsigned>(bonded_[index].brokenBonds()));
    }
    // Joint state is part of what the next tick reads: a broken hinge or a motor setpoint
    // that diverged would change the future without changing any body pose yet. Ids are
    // deliberately excluded: they are allocated per world and a restored world continues
    // the counter, so hashing them would make two equivalent worlds disagree.
    hasher.u32(static_cast<unsigned>(physics_.joints().size()));
    for (const physics::Joint& joint : physics_.joints()) {
        hasher.u8(static_cast<std::uint8_t>(joint.broken ? 1 : 0));
        hasher.quantized(joint.angle);
    }
    const physics::Stats stats = physics_.stats();
    hasher.u32(stats.activeBodies);
    hasher.u32(stats.contacts);
    for (std::size_t index = 0; index < scene_.entities.size(); ++index) {
        if (!scene_.entities[index].destructible) continue;
        hasher.u8(static_cast<std::uint8_t>(fractured_[index] ? 1 : 0));
        hasher.quantized(damage_[index]);
    }
    for (const physics::Body& body : physics_.bodies()) {
        if (!body.active || !body.dynamic) continue;
        hasher.u32(body.id);
        hasher.u8(static_cast<std::uint8_t>(body.sleeping ? 1 : 0));
        hasher.quantized(body.position.x);
        hasher.quantized(body.position.y);
        hasher.quantized(body.position.z);
        hasher.quantized(body.orientation.w);
        hasher.quantized(body.orientation.x);
        hasher.quantized(body.orientation.y);
        hasher.quantized(body.orientation.z);
        hasher.quantized(body.velocity.x);
        hasher.quantized(body.velocity.y);
        hasher.quantized(body.velocity.z);
        hasher.quantized(body.angularVelocity.x);
        hasher.quantized(body.angularVelocity.y);
        hasher.quantized(body.angularVelocity.z);
    }
    // Water authority and the coupling bookkeeping that decides whether a sleeping
    // body reacts next tick. Events are render-only and deliberately excluded.
    hasher.text("nativegpu.water-coupling/1");
    hasher.u8(static_cast<std::uint8_t>(water_ ? 1 : 0));
    if (water_) hasher.u64(water_->digest());
    hasher.u64(waterDisturbances_);
    hasher.u8(static_cast<std::uint8_t>(footprintState_.observed ? 1 : 0));
    hasher.u8(static_cast<std::uint8_t>(footprintState_.contact ? 1 : 0));
    hasher.quantized(footprintState_.travel);
    hasher.quantized(footprintState_.previous.x);
    hasher.quantized(footprintState_.previous.z);
    hasher.u32(static_cast<std::uint32_t>(bodyWaterVolume_.size()));
    for (float volume : bodyWaterVolume_) hasher.quantized(volume);
    // The rendered pose list is derived from the state above, but hashing it catches a
    // divergence in the pose construction that the raw fields would miss.
    const std::vector<Pose> poses = renderPoses();
    hasher.u32(static_cast<std::uint32_t>(poses.size()));
    for (const Pose& pose : poses) {
        hasher.quantized(pose.position.x);
        hasher.quantized(pose.position.y);
        hasher.quantized(pose.position.z);
        hasher.quantized(pose.scale.x);
        hasher.quantized(pose.scale.y);
        hasher.quantized(pose.scale.z);
        hasher.quantized(pose.yaw);
        hasher.quantized(pose.pitch);
        hasher.quantized(pose.roll);
        hasher.u32(pose.texture);
        hasher.i32(pose.material);
    }
}

} // namespace engine

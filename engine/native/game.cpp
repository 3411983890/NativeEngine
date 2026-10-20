#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine {
namespace {
constexpr float kPlayerRadius = 0.30f;
constexpr float kPlayerHeight = 1.80f;
constexpr float kWorldEdge = 10.0f;
constexpr double kFixedStep = 1.0 / 120.0;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kContactEpsilon = 0.0001f;

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

void Game::initialize(Scene scene) {
    scene_ = std::move(scene);
    scene_.walkSpeed = std::clamp(finiteOr(scene_.walkSpeed, 3.0f), 0.1f, 30.0f);
    scene_.runSpeed = std::clamp(finiteOr(scene_.runSpeed, 6.0f),
                                scene_.walkSpeed, 60.0f);
    scene_.jumpSpeed = std::clamp(finiteOr(scene_.jumpSpeed, 7.0f), 0.1f, 30.0f);
    scene_.gravity = std::clamp(finiteOr(scene_.gravity, 20.0f), 1.0f, 100.0f);
    spawn_ = location("player", {0.0f, 0.0f, 5.0f});
    spawn_.x = std::clamp(finiteOr(spawn_.x, 0.0f),
                          -kWorldEdge + kPlayerRadius,
                          kWorldEdge - kPlayerRadius);
    spawn_.y = std::max(finiteOr(spawn_.y, 0.0f), 0.0f);
    spawn_.z = std::clamp(finiteOr(spawn_.z, 5.0f),
                          -kWorldEdge + kPlayerRadius,
                          kWorldEdge - kPlayerRadius);
    reset();
}

void Game::reset() {
    player_ = spawn_;
    verticalVelocity_ = 0.0f;
    yaw_ = 0.0f;
    pitch_ = 0.35f;
    animationPhase_ = 0.0f;
    animationSpeed_ = 0.0f;
    accumulator_ = 0.0;
    grounded_ = player_.y <= 0.0f;
    hasKey_ = false;
    doorOpen_ = false;
    complete_ = false;
    jumpWasDown_ = false;
    interactWasDown_ = false;
    jumpPending_ = false;
    interactPending_ = false;
    feedbackTime_ = 0.0f;
    feedback_.clear();
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
    return entity.kind != "door" || !doorOpen_;
}

void Game::update(Input input, float dt) {
    input.forward = std::clamp(finiteOr(input.forward, 0.0f), -1.0f, 1.0f);
    input.right = std::clamp(finiteOr(input.right, 0.0f), -1.0f, 1.0f);
    yaw_ = std::remainder(yaw_ + finiteOr(input.yawDelta, 0.0f), 2.0f * kPi);
    pitch_ = std::clamp(pitch_ + finiteOr(input.pitchDelta, 0.0f), -0.15f, 1.15f);
    jumpPending_ = jumpPending_ || (input.jump && !jumpWasDown_);
    interactPending_ = interactPending_ || (input.interact && !interactWasDown_);
    jumpWasDown_ = input.jump;
    interactWasDown_ = input.interact;

    // A debugger pause or window drag cannot produce a giant physics step.
    // All movement, contacts, and interactions run at a fixed 120 Hz.
    accumulator_ += static_cast<double>(
        std::clamp(finiteOr(dt, 0.0f), 0.0f, 0.10f));
    while (accumulator_ + 1.0e-9 >= kFixedStep) {
        step(input, static_cast<float>(kFixedStep));
        accumulator_ -= kFixedStep;
    }
    accumulator_ = std::max(accumulator_, 0.0);
}

void Game::step(const Input& input, float dt) {
    feedbackTime_ = std::max(0.0f, feedbackTime_ - dt);
    if (jumpPending_) {
        if (grounded_) {
            verticalVelocity_ = scene_.jumpSpeed;
            grounded_ = false;
        }
        jumpPending_ = false;
    }
    float forward = input.forward;
    float right = input.right;
    const float length = std::sqrt(forward * forward + right * right);
    if (length > 1.0f) {
        forward /= length;
        right /= length;
    }
    const float speed = input.run ? scene_.runSpeed : scene_.walkSpeed;
    const float s = std::sin(yaw_);
    const float c = std::cos(yaw_);
    const Vec3 oldPosition = player_;
    moveHorizontal((s * forward + c * right) * speed * dt,
                   (-c * forward + s * right) * speed * dt);
    verticalVelocity_ -= scene_.gravity * dt;
    moveVertical(verticalVelocity_ * dt);

    const float actualSpeed =
        std::sqrt(distanceSquaredXZ(player_, oldPosition)) / dt;
    animationSpeed_ += (actualSpeed - animationSpeed_) * std::min(1.0f, dt * 18.0f);
    animationPhase_ = std::remainder(
        animationPhase_ + actualSpeed * dt * 3.4f, 2.0f * kPi);
    if (interactPending_) {
        interact();
        interactPending_ = false;
    }
    const Vec3 goal = location("goal", {0.0f, 0.0f, -9.0f});
    if (doorOpen_ && distanceSquaredXZ(player_, goal) <= 0.9f * 0.9f &&
        std::abs(player_.y - goal.y) < 1.0f) {
        complete_ = true;
    }
}

void Game::moveHorizontal(float dx, float dz) {
    // Sweep each axis from its previous position rather than testing only the
    // final overlap; even a thin collider cannot be crossed between steps.
    float targetX = std::clamp(player_.x + dx, -kWorldEdge + kPlayerRadius,
                              kWorldEdge - kPlayerRadius);
    for (const Entity& entity : scene_.entities) {
        if (!solid(entity)) continue;
        const Vec3 h = halfExtents(entity);
        if (!overlap(player_.y, player_.y + kPlayerHeight,
                     entity.position.y - h.y, entity.position.y + h.y) ||
            !overlap(player_.z - kPlayerRadius, player_.z + kPlayerRadius,
                     entity.position.z - h.z, entity.position.z + h.z)) {
            continue;
        }
        const float left = entity.position.x - h.x - kPlayerRadius;
        const float right = entity.position.x + h.x + kPlayerRadius;
        if (dx > 0.0f && player_.x <= left + kContactEpsilon && targetX > left) {
            targetX = left;
        } else if (dx < 0.0f && player_.x >= right - kContactEpsilon &&
                   targetX < right) {
            targetX = right;
        }
    }
    player_.x = targetX;
    float targetZ = std::clamp(player_.z + dz, -kWorldEdge + kPlayerRadius,
                              kWorldEdge - kPlayerRadius);
    for (const Entity& entity : scene_.entities) {
        if (!solid(entity)) continue;
        const Vec3 h = halfExtents(entity);
        if (!overlap(player_.y, player_.y + kPlayerHeight,
                     entity.position.y - h.y, entity.position.y + h.y) ||
            !overlap(player_.x - kPlayerRadius, player_.x + kPlayerRadius,
                     entity.position.x - h.x, entity.position.x + h.x)) {
            continue;
        }
        const float front = entity.position.z - h.z - kPlayerRadius;
        const float back = entity.position.z + h.z + kPlayerRadius;
        if (dz > 0.0f && player_.z <= front + kContactEpsilon && targetZ > front) {
            targetZ = front;
        } else if (dz < 0.0f && player_.z >= back - kContactEpsilon &&
                   targetZ < back) {
            targetZ = back;
        }
    }
    player_.z = targetZ;
}

void Game::moveVertical(float dy) {
    float targetY = player_.y + dy;
    grounded_ = false;
    for (const Entity& entity : scene_.entities) {
        if (!solid(entity)) continue;
        const Vec3 h = halfExtents(entity);
        if (!overlap(player_.x - kPlayerRadius, player_.x + kPlayerRadius,
                     entity.position.x - h.x, entity.position.x + h.x) ||
            !overlap(player_.z - kPlayerRadius, player_.z + kPlayerRadius,
                     entity.position.z - h.z, entity.position.z + h.z)) {
            continue;
        }
        const float top = entity.position.y + h.y;
        const float bottom = entity.position.y - h.y;
        if (dy < 0.0f && player_.y >= top - kContactEpsilon && targetY < top) {
            targetY = top;
            verticalVelocity_ = 0.0f;
            grounded_ = true;
        } else if (dy > 0.0f && player_.y + kPlayerHeight <= bottom + kContactEpsilon &&
                   targetY + kPlayerHeight > bottom) {
            targetY = bottom - kPlayerHeight;
            verticalVelocity_ = 0.0f;
        }
    }
    if (targetY <= 0.0f) {
        targetY = 0.0f;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    }
    player_.y = targetY;
}

void Game::interact() {
    if (!hasKey_) {
        const Vec3 key = location("key", {4.0f, 0.8f, 1.0f});
        const float bodyCenterY = player_.y + 0.8f;
        if (distanceSquaredXZ(player_, key) <= 1.65f * 1.65f &&
            std::abs(bodyCenterY - key.y) < 1.8f) {
            hasKey_ = true;
            feedback_ = "Key collected. Find the gate.";
            feedbackTime_ = 2.5f;
            return;
        }
    }
    const Vec3 door = location("door", {0.0f, 1.5f, -8.0f});
    if (!doorOpen_ && distanceSquaredXZ(player_, door) <= 2.2f * 2.2f &&
        std::abs(player_.y + 0.9f - door.y) < 2.5f) {
        if (hasKey_) {
            doorOpen_ = true;
            feedback_ = "Gate open. Reach the green exit.";
        } else {
            feedback_ = "The gate is locked. Find the gold key.";
        }
        feedbackTime_ = 2.5f;
    }
}

std::string Game::status() const {
    if (complete_) return "Complete! Press R to restart the playtest.";
    if (feedbackTime_ > 0.0f) return feedback_;
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

std::vector<Pose> Game::renderPoses() const {
    std::vector<Pose> poses;
    poses.reserve(scene_.entities.size() + 24);
    for (const Entity& entity : scene_.entities) {
        if (entity.kind == "player" || (entity.kind == "key" && hasKey_)) continue;
        Pose pose;
        pose.position = entity.position;
        pose.scale = entity.scale;
        std::copy(entity.color, entity.color + 3, pose.color);
        if (entity.hasMaterial) {
            pose.roughness = entity.material.roughness;
            pose.metalness = entity.material.metalness;
            pose.texture = textureIndex(entity.material.pattern);
        }
        if (entity.kind == "door" && doorOpen_) {
            // Rotate around the left hinge, keeping the open gate visible.
            pose.position.x -= entity.scale.x * 0.5f;
            pose.position.z += entity.scale.x * 0.5f;
            pose.yaw = kPi * 0.5f;
        }
        poses.push_back(pose);
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

} // namespace engine

#pragma once

#include <string>
#include <vector>

#include "determinism.hpp"

namespace engine {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Scene objects use center positions and full extents. A player object's
// position is the spawn location of its feet.
struct Material {
    float albedo[3]{0.65f, 0.65f, 0.65f};
    float roughness = 0.72f;
    float metalness = 0.0f;
    std::string pattern = "checker";
};

struct Light {
    Vec3 position;
    float color[3]{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
};

struct Entity {
    std::string id;
    std::string name;
    std::string kind;
    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float color[3]{0.65f, 0.65f, 0.65f};
    Material material;
    bool hasMaterial = false;
    bool hasLight = false;
    float lightIntensity = 1.0f;
    float lightRange = 10.0f;
    float lightColor[3]{1.0f, 1.0f, 1.0f};
    bool collider = false;
};

struct Scene {
    std::vector<Entity> entities;
    std::vector<Light> lights;
    float walkSpeed = 3.0f;
    float runSpeed = 6.0f;
    float jumpSpeed = 7.0f;
    float gravity = 20.0f;
};

struct Input {
    float forward = 0.0f;
    float right = 0.0f;
    bool run = false;
    bool jump = false;
    bool interact = false;
    // Mouse/orbit deltas are radians per update, not angular velocities.
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
};

struct Pose {
    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float color[3]{0.65f, 0.65f, 0.65f};
    float roughness = 0.72f;
    float metalness = 0.0f;
    unsigned texture = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

class Game {
public:
    void initialize(Scene scene);
    void update(Input input, float dt);
    void reset();
    std::vector<Pose> renderPoses() const;

    // Deterministic digest over every value the simulation reads next tick. Rendering
    // state, camera smoothing and wall-clock time are deliberately excluded so the
    // digest cannot change because a frame was drawn differently.
    void digest(determinism::Hasher& hasher) const;
    unsigned long long ticks() const { return ticks_; }

    Vec3 playerPosition() const { return player_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    bool hasKey() const { return hasKey_; }
    bool doorOpen() const { return doorOpen_; }
    bool complete() const { return complete_; }
    bool grounded() const { return grounded_; }
    std::string status() const;

private:
    bool solid(const Entity& entity) const;
    void step(const Input& input, float dt);
    void moveHorizontal(float dx, float dz);
    void moveVertical(float dy);
    void interact();
    Vec3 location(const std::string& kind, Vec3 fallback) const;

    Scene scene_;
    Vec3 spawn_{0.0f, 0.0f, 5.0f};
    Vec3 player_{0.0f, 0.0f, 5.0f};
    float verticalVelocity_ = 0.0f;
    float yaw_ = 0.0f;
    float pitch_ = 0.35f;
    float animationPhase_ = 0.0f;
    float animationSpeed_ = 0.0f;
    double accumulator_ = 0.0;
    bool grounded_ = true;
    bool hasKey_ = false;
    bool doorOpen_ = false;
    bool complete_ = false;
    bool jumpWasDown_ = false;
    bool interactWasDown_ = false;
    bool jumpPending_ = false;
    bool interactPending_ = false;
    float feedbackTime_ = 0.0f;
    std::string feedback_;
    unsigned long long ticks_ = 0;
};

} // namespace engine

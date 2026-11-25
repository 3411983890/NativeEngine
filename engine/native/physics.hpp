#pragma once

namespace engine::gameplay { struct NativeSnapshot; }

// Deterministic, CPU-side rigid-body physics for the engine's box primitives.
//
// The solver is a small fixed-step sequential-impulse solver:
//   * dynamic AABB tree broadphase (deterministic, with an O(n^2) fallback) and
//     SAT narrowphase,
//   * clipped face manifolds plus edge contacts,
//   * Coulomb friction with a two-axis tangent basis,
//   * restitution with a velocity threshold,
//   * Baumgarte bias and split positional correction,
//   * adaptive substeps bounded by a maximum travel distance (tunnelling guard),
//   * island-free contact/impact events consumed by the destruction layer.
//
// Everything is single threaded and iterated in stable index order. A fixed seed,
// fixed iteration count and fixed substep count make runs byte-reproducible on the
// same binary. The world deliberately has no dependency on D3D12 or the host.

#include "broadphase.hpp"
#include "convex.hpp"
#include "vec3.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine {
namespace physics {

struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat3 {
    // Column-major: column c is {m[c*3+0], m[c*3+1], m[c*3+2]}.
    float m[9]{};
};

Quat normalize(Quat q);
Quat multiply(Quat a, Quat b);
Quat fromAxisAngle(Vec3 axis, float radians);
Vec3 rotate(Quat q, Vec3 v);
Mat3 toMatrix(Quat q);
Mat3 transpose(Mat3 a);
Vec3 transform(Mat3 a, Vec3 v);
Mat3 multiply(Mat3 a, Mat3 b);
Quat fromEuler(float yaw, float pitch, float roll);
// Inverse of the engine's T * Yaw * Pitch * Roll convention.
void toEuler(Quat q, float& yaw, float& pitch, float& roll);

// Single-valued static terrain sampled in world space. Heights are world metres.
// The array is owned by the caller and must outlive every body referencing it.
struct Heightfield {
    float originX = 0.0f, originZ = 0.0f;
    float sizeX = 1.0f, sizeZ = 1.0f;
    unsigned width = 0, height = 0;
    const float* heights = nullptr;
    float maxErrorMetres = 0.0f;
    bool valid() const { return heights != nullptr && width > 1 && height > 1 && sizeX > 0.0f && sizeZ > 0.0f; }
    float sample(float x, float z) const {
        if (!valid()) return 0.0f;
        const float u = std::min(1.0f, std::max(0.0f, (x - originX) / sizeX));
        const float v = std::min(1.0f, std::max(0.0f, (z - originZ) / sizeZ));
        const float fx = u * static_cast<float>(width - 1);
        const float fz = v * static_cast<float>(height - 1);
        const unsigned x0 = static_cast<unsigned>(fx), z0 = static_cast<unsigned>(fz);
        const unsigned x1 = std::min(width - 1, x0 + 1), z1 = std::min(height - 1, z0 + 1);
        const float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
        const float a = heights[z0 * width + x0] * (1.0f - tx) + heights[z0 * width + x1] * tx;
        const float b = heights[z1 * width + x0] * (1.0f - tx) + heights[z1 * width + x1] * tx;
        return a * (1.0f - tz) + b * tz;
    }
    Vec3 normal(float x, float z) const {
        if (!valid()) return {0.0f, 1.0f, 0.0f};
        const float stepX = sizeX / static_cast<float>(width - 1);
        const float stepZ = sizeZ / static_cast<float>(height - 1);
        const float gx = (sample(x + stepX, z) - sample(x - stepX, z)) / (2.0f * stepX);
        const float gz = (sample(x, z + stepZ) - sample(x, z - stepZ)) / (2.0f * stepZ);
        const float length = std::sqrt(gx * gx + 1.0f + gz * gz);
        return {-gx / length, 1.0f / length, -gz / length};
    }
};

struct BodyDesc {
    Vec3 position;
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    // Optional convex hull (see Body). Null keeps the box shape.
    const Vec3* hullPoints = nullptr;
    int hullPointCount = 0;
    const std::uint32_t* hullIndices = nullptr;
    int hullIndexCount = 0;
    // Optional static terrain. When set the box shape is replaced by the field.
    const Heightfield* heightfield = nullptr;
    Quat orientation;
    Vec3 velocity;
    Vec3 angularVelocity;
    float mass = 1.0f;
    float friction = 0.62f;
    float restitution = 0.06f;
    float linearDamping = 0.08f;
    float angularDamping = 0.18f;
    bool dynamic = true;
    bool debris = false;
    bool projectile = false;
    bool visible = true;
    // -1 means runtime-created (projectile/debris), otherwise an entity index.
    int entity = -1;
    // Optional render metadata used for runtime-created boxes.
    float color[3]{0.65f, 0.65f, 0.65f};
    int material = -1;
    int texture = 0;
    float roughness = 0.75f;
    float metalness = 0.0f;
    float lifetime = 0.0f;  // seconds; 0 = permanent
};

struct Body {
    unsigned id = 0;
    int entity = -1;
    bool dynamic = false;
    bool debris = false;
    bool projectile = false;
    bool visible = true;
    bool active = true;
    bool sleeping = false;
    Vec3 half{0.5f, 0.5f, 0.5f};
    // Optional convex hull, local to the body and expressed in the body basis. When set,
    // the narrowphase uses GJK/EPA instead of the box SAT path, so the shard collides as
    // its real shape rather than as the box around it. The arrays are owned by the caller
    // (the game's convex mesh table) and must outlive the body.
    const Vec3* hullPoints = nullptr;
    int hullPointCount = 0;
    const std::uint32_t* hullIndices = nullptr;
    int hullIndexCount = 0;
    const Heightfield* heightfield = nullptr;
    Vec3 position;
    Quat orientation;
    Vec3 velocity;
    Vec3 angularVelocity;
    // External forces/torques accumulated for the next step. Cleared after every
    // step, so they are transient inputs rather than simulation state.
    Vec3 externalForce;
    Vec3 externalTorque;
    float mass = 1.0f;
    float invMass = 1.0f;
    Mat3 invInertiaLocal;
    Mat3 invInertiaWorld;
    float friction = 0.62f;
    float restitution = 0.06f;
    float linearDamping = 0.08f;
    float angularDamping = 0.18f;
    float sleepTimer = 0.0f;
    float lifetime = 0.0f;
    float color[3]{0.65f, 0.65f, 0.65f};
    int material = -1;
    int texture = 0;
    float roughness = 0.75f;
    float metalness = 0.0f;
    unsigned long long ageTicks = 0;
};

struct ContactPoint {
    Vec3 point;
    float penetration = 0.0f;
};

struct Impact {
    unsigned indexA = 0;
    unsigned indexB = 0;
    int entityA = -1;
    int entityB = -1;
    Vec3 point;
    Vec3 normal;          // from A toward B
    float normalImpulse = 0.0f;
    float approachSpeed = 0.0f;
};

// XPBD joint layer. Joints are solved as position projections after the
// sequential-impulse contact solve, so the contact solver keeps producing the
// real normal impulses the destruction layer consumes.
//
// Hinge is implemented as two point constraints offset along the hinge axis plus
// an optional angular limit. That avoids quaternion joint math and keeps a door
// from drifting off its frame.
enum class JointType : int {
    Point = 0,
    Hinge = 1,
    Rope = 2,
};

struct JointDesc {
    JointType type = JointType::Point;
    unsigned bodyA = 0;
    // kNoBody anchors to the world: anchorB is then a world-space point and anchorA stays
    // local to A. (The solver reads the world point from anchorB, not anchorA.)
    unsigned bodyB = 0xFFFFFFFFu;
    Vec3 anchorA;                 // local to A (world when bodyB == kNoBody)
    Vec3 anchorB;                 // local to B
    Vec3 axis{0.0f, 1.0f, 0.0f};  // hinge axis, local to A
    float hingeSpan = 0.25f;      // half distance between the two hinge points
    float restLength = 0.0f;      // rope segment length; point joints use 0
    float compliance = 0.0f;      // 0 = rigid, larger = softer (m/N)
    float lowerLimit = -3.14159265f;
    float upperLimit = 3.14159265f;
    float breakForce = 0.0f;      // 0 = unbreakable; |lambda|/h above this breaks (N)
    bool collideConnected = false;
    // Hinge drive. A motor advances an internal target angle at motorSpeed and pulls the
    // hinge towards it; motorMaxTorque bounds the correction so a heavy door stalls
    // instead of teleporting. friction (0..1) removes that fraction of the relative
    // swing per second, which is what stops a door from swinging forever.
    float motorSpeed = 0.0f;        // rad/s, 0 = no motor
    float motorMaxTorque = 0.0f;    // N*m, 0 = no motor
    float friction = 0.0f;          // 0..1 damping per second about the hinge axis
};

struct Joint {
    unsigned id = 0;
    JointType type = JointType::Point;
    unsigned bodyA = 0;
    unsigned bodyB = 0xFFFFFFFFu;
    Vec3 localAnchorA;
    Vec3 localAnchorB;
    Vec3 localAxis{0.0f, 1.0f, 0.0f};
    Vec3 localReference{1.0f, 0.0f, 0.0f};  // perpendicular to localAxis
    float hingeSpan = 0.25f;
    float restLength = 0.0f;
    float compliance = 0.0f;
    float lowerLimit = -3.14159265f;
    float upperLimit = 3.14159265f;
    float breakForce = 0.0f;
    float lambda = 0.0f;   // accumulated XPBD multiplier of the primary constraint
    float angle = 0.0f;    // hinge angle, radians
    bool broken = false;
    bool collideConnected = false;
    float motorSpeed = 0.0f;
    float motorMaxTorque = 0.0f;
    float friction = 0.0f;
    // Signed limit violation measured by the position solve this step: negative below the
    // lower limit, positive above the upper one, 0 when the hinge is free. The velocity
    // drive reads it to know which direction is blocked.
    float limitError = 0.0f;
};

struct Settings {
    int velocityIterations = 12;
    int positionIterations = 2;
    // XPBD joint iterations. Chains (rope) need more Gauss-Seidel sweeps than a
    // stack needs position passes, so this is separate from positionIterations.
    int jointIterations = 8;
    int maxSubsteps = 8;
    float maxTravelPerSubstep = 0.12f;
    float sleepLinear = 0.055f;
    float sleepAngular = 0.09f;
    float sleepDelay = 0.65f;
    float restitutionThreshold = 1.2f;
    float baumgarte = 0.18f;
    float positionSlop = 0.004f;
    float maxBiasSpeed = 2.0f;
    // Broadphase selection: 0 = auto (tree above broadphaseThreshold active
    // bodies), 1 = brute force O(n^2), 2 = dynamic AABB tree.
    int broadphase = 0;
    int broadphaseThreshold = 64;
    // Fat AABB margin. Clamped to >= 0.004 so the tree always reports a superset
    // of the pairs the exact narrowphase AABB test would accept.
    float broadphaseMargin = 0.02f;
};

struct Stats {
    unsigned bodies = 0;
    unsigned activeBodies = 0;
    unsigned dynamicBodies = 0;
    unsigned sleepingBodies = 0;
    unsigned debris = 0;
    unsigned projectiles = 0;
    unsigned joints = 0;
    unsigned brokenJoints = 0;
    unsigned contacts = 0;
    unsigned substeps = 0;
    unsigned long long totalSteps = 0;
    double lastStepMs = 0.0;
    double averageStepMs = 0.0;
    double maxStepMs = 0.0;
    // Broadphase telemetry: 0 = brute force, 1 = dynamic AABB tree.
    unsigned broadphaseMode = 0;
    unsigned broadphasePairs = 0;
    unsigned broadphaseNodes = 0;
    unsigned broadphaseRebuilds = 0;
};

class World {
public:
    void clear();
    // Reserves capacity once so adding debris never invalidates the Game's cached
    // entity->body indices. Bodies themselves are marked inactive rather than erased.
    void reserve(std::size_t count, std::size_t bodies);
    unsigned addBody(const BodyDesc& desc);
    void deactivate(unsigned index);
    void setGravity(Vec3 gravity) { gravity_ = gravity; }
    Vec3 gravity() const { return gravity_; }
    void setSettings(const Settings& settings) { settings_ = settings; }
    const Settings& settings() const { return settings_; }

    void step(float dt);
    void applyRadialImpulse(Vec3 center, float radius, float deltaSpeed,
                            bool includeSleeping = true);
    // External force/torque inputs. They accumulate until the next step integrates
    // them and then clears them. They do not wake a sleeping body by themselves:
    // the caller decides whether a sleeping body should react this tick.
    void addForce(unsigned index, Vec3 force);
    // Applies `force` at a world point, adding the lever-arm torque about the centre.
    void addForceAtPoint(unsigned index, Vec3 force, Vec3 point);
    void addTorque(unsigned index, Vec3 torque);
    void wakeAll();
    void wake(unsigned index);

    // XPBD joints. Returns a stable joint id (ids are never reused).
    unsigned addJoint(const JointDesc& desc);
    void removeJoint(unsigned id);
    unsigned jointCount() const { return static_cast<unsigned>(joints_.size()); }
    Joint* joint(unsigned id);
    const Joint* joint(unsigned id) const;
    std::vector<Joint>& joints() { return joints_; }
    const std::vector<Joint>& joints() const { return joints_; }
    // Cumulative number of broken joints (also reported by stats()).
    unsigned brokenJointCount() const;

    Body* body(unsigned index);
    const Body* body(unsigned index) const;
    std::vector<Body>& bodies() { return bodies_; }
    const std::vector<Body>& bodies() const { return bodies_; }
    unsigned bodyCount() const { return static_cast<unsigned>(bodies_.size()); }
    std::size_t capacity() const { return bodies_.capacity(); }

    std::vector<Impact> takeImpacts();
    const std::vector<Impact>& lastImpacts() const { return frameImpacts_; }
    Stats stats() const;
    // Number of contacts generated in the most recent substep.
    unsigned lastContactCount() const { return lastContactCount_; }

private:
    friend struct engine::gameplay::NativeSnapshot;
    struct Contact {
        unsigned indexA = 0;
        unsigned indexB = 0;
        int entityA = -1;
        int entityB = -1;
        Vec3 point;
        Vec3 normal;
        float penetration = 0.0f;
        float restitution = 0.0f;
        float normalImpulse = 0.0f;
        Vec3 tangentImpulse;
        float approachSpeed = 0.0f;
        float normalMass = 0.0f;
        float tangentMass[2]{0.0f, 0.0f};
        Vec3 tangent[2];
    };

    void integrateVelocities(float dt);
    void integratePositions(float dt);
    void generateContacts();
    // Narrowphase + manifold generation for one candidate pair. Kept separate so
    // the brute-force and BVH paths share exactly the same per-pair code.
    void collidePair(unsigned indexA, unsigned indexB);
    void emitContacts(unsigned indexA, unsigned indexB, const std::vector<ContactPoint>& points,
                      Vec3 normal, float fallbackDepth);
    void updateBroadphase();
    bool useBvh() const;
    void solveContacts(float dt);
    void correctPositions();
    void updateSleep(float dt);
    void collectImpacts();
    void wakeIfNeeded(Body& a, Body& b);
    void applyImpulse(Body& body, Vec3 impulse, Vec3 r);

    // Joint solving (XPBD position projection).
    void captureJointBodies();
    void solveJoints(float dt);
    float projectPoint(unsigned indexA, unsigned indexB, Vec3 anchorA, Vec3 anchorB,
                       float restLength, bool inequality, float compliance, float h,
                       float& lambda);
    void solveHingeLimit(Joint& joint);
    void solveHingeDrive(Joint& joint, float dt);
    void rebuildNoCollidePairs();
    bool skipConnectedPair(unsigned indexA, unsigned indexB) const;
    void applyPositionalImpulse(Body& body, Vec3 impulse, Vec3 r);
    void applyAngularCorrection(Body& body, Vec3 angularImpulse);

    struct JointPrev {
        unsigned body = 0;
        Vec3 position;
        Quat orientation;
    };

    Settings settings_;
    Vec3 gravity_{0.0f, -20.0f, 0.0f};
    std::vector<Body> bodies_;
    std::vector<Contact> contacts_;
    std::vector<Impact> frameImpacts_;
    std::vector<Joint> joints_;
    std::vector<JointPrev> jointPrev_;
    // Pairs connected by a joint with collideConnected == false. Narrowphase skips
    // them so a hinged door does not fight the frame it is bolted to.
    std::vector<std::pair<unsigned, unsigned>> noCollidePairs_;
    AabbTree broadphase_;
    std::vector<int> bodyProxy_;       // body index -> proxy handle, -1 = not in tree
    std::vector<AabbPair> candidates_;  // sorted candidate pairs for this substep
    unsigned broadphasePairs_ = 0;
    unsigned broadphaseMode_ = 0;
    unsigned nextJointId_ = 1;
    unsigned nextId_ = 1;
    unsigned lastContactCount_ = 0;
    unsigned substepsLast_ = 0;
    unsigned long long totalSteps_ = 0;
    double lastStepMs_ = 0.0;
    double totalStepMs_ = 0.0;
    double maxStepMs_ = 0.0;
    unsigned long long timedSteps_ = 0;
};

// Public helpers reused by the destruction layer and tests.
bool intersects(const Body& a, const Body& b);
// Tight world-space AABB, computed the same way the broadphase does.
Aabb worldBounds(const Body& body);
Vec3 worldAxis(const Body& body, int axis);
float volume(const Body& body);
float kineticEnergy(const Body& body);

} // namespace physics
} // namespace engine
